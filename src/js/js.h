// Tiny Browser's own JavaScript engine: values, heap objects and the virtual machine interface.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tb::js {

class Vm;
class Gc;
struct Cell;
struct JSString;
struct Symbol;
struct JSBigInt;
struct JSObject;
struct JSArray;
struct JSFunction;
struct FuncProto;
struct Context;

// ---------------------------------------------------------------------------- Value (NaN-boxed, 8 bytes)
// doubles are stored as-is; everything else lives in the quiet-NaN space:
//   0x7FFC......  singletons (undefined, null, false, true, empty)
//   0x7FFD0000 + 32 bits   int32
//   0xFFFC + 48 bits       pointer to a heap Cell
struct Value {
    uint64_t b;
    static constexpr uint64_t QNAN = 0x7FFC000000000000ull;
    static constexpr uint64_t TAG_INT = 0x7FFD000000000000ull;
    static constexpr uint64_t TAG_PTR = 0xFFFC000000000000ull;
    static constexpr uint64_t PAYLOAD = 0x0000FFFFFFFFFFFFull;
    constexpr Value() : b(QNAN | 1) {}
    constexpr explicit Value(uint64_t bits) : b(bits) {}
    static constexpr Value undef() { return Value(QNAN | 1); }
    static constexpr Value null() { return Value(QNAN | 2); }
    static constexpr Value boolean(bool v) { return Value(QNAN | (v ? 4 : 3)); }
    static constexpr Value empty() { return Value(QNAN | 5); }  // array hole / uninitialised binding
    static Value i32(int32_t i) { return Value(TAG_INT | (uint32_t)i); }
    static Value dbl(double d) {
        if (d != d) return Value(0x7FF8000000000000ull);
        Value v;
        memcpy(&v.b, &d, 8);
        return v;
    }
    static Value num(double d) {
        if (d >= -2147483648.0 && d <= 2147483647.0) {
            int32_t i = (int32_t)d;
            if ((double)i == d && !(i == 0 && std::signbit(d))) return i32(i);
        }
        return dbl(d);
    }
    static Value cell(const Cell* c) { return Value(TAG_PTR | (uint64_t)(uintptr_t)c); }

    bool isUndef() const { return b == (QNAN | 1); }
    bool isNull() const { return b == (QNAN | 2); }
    bool isNullish() const { return b == (QNAN | 1) || b == (QNAN | 2); }
    bool isBool() const { return b == (QNAN | 3) || b == (QNAN | 4); }
    bool isTrue() const { return b == (QNAN | 4); }
    bool isEmpty() const { return b == (QNAN | 5); }
    bool isInt() const { return (b >> 48) == 0x7FFD; }
    bool isDouble() const { return (b & QNAN) != QNAN; }
    bool isNumber() const { return isInt() || isDouble(); }
    int32_t asInt() const { return (int32_t)(uint32_t)b; }
    double asDouble() const {
        double d;
        memcpy(&d, &b, 8);
        return d;
    }
    double number() const { return isInt() ? (double)asInt() : asDouble(); }
    bool isCell() const { return (b & TAG_PTR) == TAG_PTR; }
    Cell* asCell() const { return (Cell*)(uintptr_t)(b & PAYLOAD); }
    inline bool isString() const;
    inline bool isSymbol() const;
    inline bool isBigInt() const;
    inline JSBigInt* asBigInt() const;
    inline bool isObject() const;
    inline JSString* asString() const;
    inline JSObject* asObject() const;
    bool operator==(Value o) const { return b == o.b; }
    bool operator!=(Value o) const { return b != o.b; }
};

// ---------------------------------------------------------------------------- heap cells
enum class CK : uint8_t { String, Symbol, Object, Context, Code, Accessor, BigInt };
enum class Cls : uint8_t {
    Object, Array, Arguments, Function, Native, Bound, Error, Boolean, Number, String, SymbolObj, Date, RegExp,
    Map, Set, WeakMap, WeakSet, Promise, Generator, ArrayIter, MapIter, SetIter, StringIter, ForIn, Global, Host, ArrayBuffer, TypedArray, DataView, Proxy, BigInt,
    RawJSON,     // made by JSON.rawJSON: JSON.stringify writes its text as it is
    ShadowRealm  // a JSPrimitive whose value is the number of the realm behind it
};

struct Cell {
    virtual ~Cell() = default;
    virtual void trace(Gc&) {}
    CK kind = CK::Object;
    uint8_t mark = 0;
    Cls cls = Cls::Object;
    uint8_t oflags = 0;
    uint16_t realm = 0;  // of a function or a RegExp: the realm it was made in, its [[Realm]]. Kept here because the space is free.
};

// A string is either flat (text in `s`) or a rope: the lazy concatenation of two other strings, which is what
// makes building long strings with += cheap. Ropes are flattened the first time their text is needed.
struct JSString : Cell {
    std::u16string s;
    bool isAtom = false;
    JSString* atom = nullptr;  // interned twin, once looked up
    JSString *left = nullptr, *right = nullptr;
    uint32_t ropeLen = 0;
    JSString() { kind = CK::String; }
    size_t length() const { return left ? ropeLen : s.size(); }
    void trace(Gc&) override;
};
void flattenRope(JSString* s);

struct Symbol : Cell {
    JSString* desc = nullptr;
    bool isPrivate = false;  // a class's #name: never listed among an object's keys, never seen by a proxy
    Symbol() { kind = CK::Symbol; }
    void trace(Gc&) override;
};

// A BigInt: sign and magnitude, least significant 32 bits first, no leading zero limbs. Zero has no limbs.
struct JSBigInt : Cell {
    bool neg = false;
    std::vector<uint32_t> d;
    JSBigInt() { kind = CK::BigInt; }
};
inline bool Value::isBigInt() const { return isCell() && asCell()->kind == CK::BigInt; }
inline JSBigInt* Value::asBigInt() const { return (JSBigInt*)asCell(); }

inline bool Value::isString() const { return isCell() && asCell()->kind == CK::String; }
inline bool Value::isSymbol() const { return isCell() && asCell()->kind == CK::Symbol; }
inline bool Value::isObject() const { return isCell() && asCell()->kind == CK::Object; }
inline JSString* Value::asString() const {
    JSString* p = (JSString*)asCell();
    if (p->left) flattenRope(p);
    return p;
}
inline JSObject* Value::asObject() const { return (JSObject*)asCell(); }

// Property key: an array index, an interned string, or a symbol.
struct PropKey {
    uint64_t v = 0;
    static PropKey index(uint32_t i) { return {((uint64_t)i << 1) | 1}; }
    static PropKey of(const Cell* c) { return {(uint64_t)(uintptr_t)c}; }
    bool isIndex() const { return (v & 1) != 0; }
    uint32_t asIndex() const { return (uint32_t)(v >> 1); }
    Cell* cell() const { return (Cell*)(uintptr_t)v; }
    bool isSymbol() const { return !isIndex() && cell()->kind == CK::Symbol; }
    bool isString() const { return !isIndex() && cell()->kind == CK::String; }
    JSString* str() const { return (JSString*)cell(); }
    bool operator==(PropKey o) const { return v == o.v; }
};

// PF_LAZY: a data property whose value is made the first time it is used. It holds a native function that makes it;
// Vm::materialize swaps the result in. Only the engine sets it, for large built-ins written in JavaScript.
enum : uint8_t { PF_W = 1, PF_E = 2, PF_C = 4, PF_ACC = 8, PF_ALL = 7, PF_LAZY = 16 };
// Arrays: OF_SLOWIDX some elements are kept as ordinary properties (own attributes, accessors, or far past the
// dense part), OF_LENRO length is not writable, OF_SEALED the dense elements are not configurable.
enum : uint8_t { OF_NOEXT = 1, OF_FROZEN = 2, OF_CTOR = 4, OF_NO_NAME = 8, OF_NO_LENGTH = 16, OF_SLOWIDX = 32, OF_LENRO = 64, OF_SEALED = 128 };  // the last two: a function whose name/length was deleted

struct Prop {
    PropKey key;
    Value val;
    uint8_t flags;
};

struct Accessor : Cell {
    Value get, set;
    Accessor() { kind = CK::Accessor; }
    void trace(Gc&) override;
};

struct JSObject : Cell {
    JSObject* proto = nullptr;
    std::vector<Prop> props;  // insertion order
    std::unique_ptr<std::unordered_map<uint64_t, uint32_t>> index;  // built once an object has many properties
    Prop* find(PropKey k);
    Prop& add(PropKey k, Value v, uint8_t flags);
    bool remove(PropKey k);
    void trace(Gc&) override;
};

struct JSArray : JSObject {
    std::vector<Value> elems;  // the dense elements; a hole is Value::empty()
    uint32_t slowLen = 0;      // with OF_SLOWIDX: the length when it reaches past the dense elements
    JSArray() { cls = Cls::Array; }
    void trace(Gc&) override;
};
// An arguments object. In a sloppy function with plain parameters its first elements are tied to the parameters
// (10.4.4): the parameters live in a context, and each tied element knows its slot there. The element store always
// holds the current value; writes on either side are copied to the other while the tie lasts.
struct JSArguments : JSArray {
    Context* mapCtx = nullptr;
    std::vector<int32_t> mapSlots;  // per element: the parameter's slot in mapCtx, or -1 once untied
    JSArguments() { cls = Cls::Arguments; }
    void trace(Gc&) override;
};
inline uint32_t arrayLen(const JSArray* a) {
    uint32_t n = (uint32_t)a->elems.size();
    return (a->oflags & OF_SLOWIDX) && a->slowLen > n ? a->slowLen : n;
}

struct JSProxy : JSObject {  // target and handler become null when the proxy is revoked
    Value target = Value::null(), handler = Value::null();
    bool callable = false, constructible = false;
    JSProxy() { cls = Cls::Proxy; }
    void trace(Gc&) override;
};

// A Property Descriptor (ECMA-262 6.2.6): fields that are absent stay unset.
struct PropDesc {
    Value value, get, set;
    bool hasValue = false, hasGet = false, hasSet = false, hasWritable = false, hasEnumerable = false, hasConfigurable = false;
    bool writable = false, enumerable = false, configurable = false;
};

struct JSPrimitive : JSObject {  // Boolean / Number / String / Symbol wrappers, Date (time value), RegExp (source)
    Value prim, extra;
    void trace(Gc&) override;
};

struct Context : Cell {  // heap-allocated scope for captured variables
    Context* parent = nullptr;
    std::vector<Value> slots;
    Context() { kind = CK::Context; }
    void trace(Gc&) override;
};

// ---------------------------------------------------------------------------- bytecode
struct Ins {
    uint16_t op, a;
    union {
        struct {
            uint16_t b, c;
        };
        uint32_t bx;
    };
};

struct Handler {
    uint32_t start, end, target;
    uint16_t ctxDepth;
    uint16_t isFinally = 0;  // cleanup code that reruns the exception: also taken when a generator is told to return
};

// What a direct eval call can see (ECMA-262 19.2.1.1 PerformEval): the scopes around the call that own a
// runtime context, outermost first, with the variables each one holds.
struct EvalScope {
    struct Var {
        JSString* name;
        uint16_t slot;
        uint8_t kind;  // VK_*
        bool tdz;
        int16_t arg = -1;  // a parameter tied to this element of a mapped arguments object
    };
    std::vector<Var> vars;
    bool isFunction = false, isArrow = false, isClass = false, isGlobal = false, isWith = false;
    bool isMethod = false, isFieldInit = false, isClassCtor = false, isDerived = false;  // of a function scope: what eval code may use
    bool isVarScope = false;  // a function body kept apart from its parameters: eval's `var`s belong here
    int mappedVar = -1;       // position in vars of the hidden variable holding a mapped arguments object
    int thisVar = -1, argsVar = -1, newTargetVar = -1;  // positions in vars, for `this`, `arguments` and new.target
    int ctorVar = -1;                                    // the running function, for super() from eval code
    int withVar = -1, evalVars = -1;                    // the object of a `with`; the object holding eval-made vars
};
struct EvalSite {
    std::vector<EvalScope> scopes;
    bool strict = false;
    bool inParams = false;  // the call sits in a parameter's default value: its vars must not clash with the parameters
};

struct FuncProto : Cell {
    std::vector<Ins> code;
    std::vector<Value> consts;
    std::vector<FuncProto*> inner;
    std::vector<Handler> handlers;
    std::vector<EvalSite> evalSites;  // one per direct eval call in this function
    std::vector<Value> templates;     // the strings object of each tagged template, made the first time it runs (13.2.8.4)
    std::vector<uint32_t> lines;
    JSString* name = nullptr;
    uint16_t nparams = 0, length = 0;
    uint32_t nregs = 2;
    bool isArrow = false, isStrict = false, needsArgs = false, isGenerator = false, isAsync = false, isClassCtor = false,
         isDerived = false, isMethod = false;
    bool simpleParams = true;  // plain identifiers only: no defaults, patterns or rest
    bool isTop = false;        // a script, a module or eval code rather than a function someone wrote
    std::vector<int32_t> argSlots;  // mapped arguments: the context slot of the parameter behind each index, or -1
    bool namesValue = false;  // a field initialiser whose value is an anonymous function: it is named after the field's computed key
    std::shared_ptr<const std::u16string> source;
    uint32_t srcStart = 0, srcEnd = 0;
    std::string file;
    // Made from a string by eval or Function(): `file` is then that of the code that asked for it, which is what an
    // import() inside resolves against (9.4.1 GetActiveScriptOrModule). Stack traces show such code as "<eval>".
    bool isDynamic = false;
    FuncProto() { kind = CK::Code; }
    void trace(Gc&) override;
};

struct JSFunction : JSObject {
    FuncProto* code = nullptr;
    Context* ctx = nullptr;
    JSObject* home = nullptr;  // [[HomeObject]] for super lookups
    Value fields;              // class field initialisers: array of key, function pairs
    Value privates;            // private methods and accessors given to each instance: key, kind, function triples
    JSFunction() { cls = Cls::Function; }
    void trace(Gc&) override;
};

struct CallInfo {
    Vm& vm;
    Value thisv;
    Value* args;
    int argc;
    Value newTarget;
    struct JSNative* callee;
    Value arg(int i) const { return i < argc ? args[i] : Value::undef(); }
    bool isConstruct() const { return !newTarget.isUndef(); }
};
using NativeFn = Value (*)(CallInfo&);

struct JSNative : JSObject {
    NativeFn fn = nullptr;
    JSString* name = nullptr;
    int length = 0;
    Value data, data2;
    bool htmlDda = false;  // [[IsHTMLDDA]] (B.3.6): document.all and its like, which pass for undefined in typeof, == and conditions
    JSNative() { cls = Cls::Native; }
    void trace(Gc&) override;
};

struct JSBound : JSObject {
    Value target, boundThis;
    std::vector<Value> args;
    JSBound() { cls = Cls::Bound; }
    void trace(Gc&) override;
};

// Objects supplied by the embedder (DOM nodes, style declarations, storage...). The hooks let such an object
// answer for properties it does not store: each returns true when it handled the key.
struct HostHooks {
    bool (*get)(Vm& vm, JSObject* o, PropKey k, Value& out) = nullptr;
    bool (*set)(Vm& vm, JSObject* o, PropKey k, Value v) = nullptr;
    bool (*del)(Vm& vm, JSObject* o, PropKey k) = nullptr;
    void (*keys)(Vm& vm, JSObject* o, std::vector<PropKey>& out) = nullptr;
    uint8_t attrs = PF_ALL;  // attributes reported for the properties the hooks supply
    // Optional: answers `in` without reading the value, and makes delete fail for non-configurable hook properties.
    bool (*has)(Vm& vm, JSObject* o, PropKey k) = nullptr;
};
struct JSHost : JSObject {
    const HostHooks* hooks = nullptr;
    uint8_t hostKind = 0;
    void* ptr = nullptr;  // owned by the embedder
    Value slotA, slotB;
    bool flagA = false, flagB = false, flagC = false;
    JSHost() { cls = Cls::Host; }
    void trace(Gc&) override;
};

// Binary data: an ArrayBuffer owns bytes; typed arrays and DataViews are windows onto a buffer.
// The memory of a SharedArrayBuffer: one block that every agent holding the buffer reads and writes (25.2). Address
// space for the largest size the buffer may grow to is reserved when it is made, so growing never moves the bytes.
struct SharedBlock {
    uint8_t* base = nullptr;
    std::atomic<size_t> length{0};
    size_t limit = 0;  // bytes of address space reserved
    ~SharedBlock();
};
std::shared_ptr<SharedBlock> newSharedBlock(size_t length, size_t limit);  // null when the memory is not to be had
bool growSharedBlock(SharedBlock& b, size_t length);                        // false when the memory is not to be had

struct JSArrayBuffer : JSObject {  // ArrayBuffer and SharedArrayBuffer
    std::vector<uint8_t> bytes;          // an ArrayBuffer's bytes: empty once detached
    std::shared_ptr<SharedBlock> block;  // a SharedArrayBuffer's bytes
    uint8_t* data() const { return block ? block->base : const_cast<uint8_t*>(bytes.data()); }
    size_t size() const { return block ? block->length.load(std::memory_order_relaxed) : bytes.size(); }
    double maxLen = -1;          // the most bytes a resizable buffer may grow to; -1 when the length is fixed
    bool detached = false, shared = false, immutable = false;
    JSArrayBuffer() { cls = Cls::ArrayBuffer; }
};
enum class TA : uint8_t { I8, U8, U8C, I16, U16, I32, U32, F32, F64, F16, I64, U64 };
inline constexpr uint8_t kTaSize[] = {1, 1, 1, 2, 2, 4, 4, 4, 8, 2, 8, 8};
struct JSTypedArray : JSObject {  // also backs DataView (cls DataView, lengths in bytes)
    JSArrayBuffer* buffer = nullptr;
    uint32_t offset = 0, length = 0;  // byte offset into the buffer; length in elements when it is fixed
    TA type = TA::U8;
    bool tracking = false;  // the length follows a resizable buffer instead
    JSTypedArray() { cls = Cls::TypedArray; }
    void trace(Gc&) override;
};
// IsTypedArrayOutOfBounds (10.4.5.14): the buffer is detached or no longer holds the whole view
inline bool typedOutOfBounds(const JSTypedArray* t) {
    const JSArrayBuffer* b = t->buffer;
    size_t have = b->size();
    return b->detached || t->offset > have || (!t->tracking && (size_t)t->offset + (size_t)t->length * kTaSize[(int)t->type] > have);
}
// TypedArrayLength, and 0 for a view that is out of bounds
inline uint32_t typedLength(const JSTypedArray* t) {
    size_t have = t->buffer->size(), size = kTaSize[(int)t->type];
    if (!t->tracking) return (size_t)t->offset + (size_t)t->length * size <= have ? t->length : 0;
    return t->offset <= have ? (uint32_t)((have - t->offset) / size) : 0;
}
Value typedGet(Vm& vm, const JSTypedArray* t, uint32_t i);  // i must be below typedLength(t)
void typedSet(Vm& vm, JSTypedArray* t, uint32_t i, Value v);
JSTypedArray* newTypedArray(Vm& vm, TA type, uint32_t length);
int typedKeyKind(Vm& vm, const JSTypedArray* t, PropKey k);  // 0 ordinary key, 1 an element, 2 numeric but no element

// bigint.cpp
JSBigInt* bigFromI64(Vm& vm, int64_t v);
JSBigInt* bigFromU64(Vm& vm, uint64_t v);
JSBigInt* bigFromDouble(Vm& vm, double d);  // d must be a whole number
JSBigInt* bigFromLiteral(Vm& vm, const std::u16string& text);
JSBigInt* stringToBigInt(Vm& vm, const std::u16string& s);  // null when the text is not a whole number
uint64_t bigToU64(const JSBigInt* b);  // the low 64 bits in two's complement
double bigToDouble(const JSBigInt* b);
std::u16string bigToString(const JSBigInt* b, int radix = 10);
bool bigEquals(const JSBigInt* a, const JSBigInt* b);
int bigCompare(const JSBigInt* a, const JSBigInt* b);
int bigCompareDouble(const JSBigInt* b, double d);  // d must not be NaN
size_t bigHash(const JSBigInt* b);

struct ValueHash {
    size_t operator()(Value v) const;
};
struct ValueEq {
    bool operator()(Value a, Value b) const;
};

struct JSMap : JSObject {  // Map, Set, WeakMap, WeakSet (insertion ordered)
    std::vector<std::pair<Value, Value>> entries;  // key is empty() for deleted entries
    std::unordered_map<Value, uint32_t, ValueHash, ValueEq> lookup;
    size_t live = 0;
    uint32_t epoch = 0;             // bumped whenever deleted entries are squeezed out
    std::vector<uint32_t> removed;  // positions dropped by the last compaction, so live iterators can adjust
    void trace(Gc&) override;
};

struct JSIter : JSObject {  // built-in iterators and for-in state
    Value target;
    uint32_t pos = 0;
    uint8_t mode = 0;  // 0 values, 1 keys, 2 entries
    uint32_t epoch = 0;
    std::vector<Value> keys;
    void trace(Gc&) override;
};

struct Reaction {
    Value onFulfilled, onRejected, resultPromise;
};
struct JSPromise : JSObject {
    uint8_t state = 0;  // 0 pending, 1 fulfilled, 2 rejected
    bool handled = false;
    Value value;
    std::vector<Reaction> reactions;
    JSPromise() { cls = Cls::Promise; }
    void trace(Gc&) override;
};

struct JSGenerator : JSObject {  // also backs async functions
    JSFunction* fn = nullptr;
    std::vector<Value> regs;  // saved register window while suspended
    const Ins* pc = nullptr;
    Context* ctx = nullptr;
    uint16_t ctxDepth = 0;
    uint16_t resumeReg = 0;
    uint8_t state = 0;  // 0 suspended-start, 1 suspended-yield, 2 running, 3 done
    bool isAsync = false;
    bool isAsyncGen = false;        // an async generator: requests queue up and are answered with promises
    bool suspendedAtAwait = false;  // the body stopped at an await, not a yield
    bool delegating = false;        // stopped inside yield*: the next resumption goes to the inner iterator
    bool atStart = false;           // parameters are bound, the body has not begun
    uint8_t agState = 0;            // [[AsyncGeneratorState]]: start, yield, executing, draining-queue, completed
    Value newTarget, promise;
    Value retVal;  // the value of a return() that is unwinding the body
    JSArray* args = nullptr;
    struct Request {
        uint8_t type;  // 0 next, 1 throw, 2 return
        Value value;
        JSPromise* promise;
    };
    std::vector<Request> queue;  // [[AsyncGeneratorQueue]]
    JSGenerator() { cls = Cls::Generator; }
    void trace(Gc&) override;
};

// ---------------------------------------------------------------------------- garbage collector (mark & sweep)
// Cells live in size-class blocks. Roots are the VM stack, frames and globals (precise) plus the native C++
// stack (conservative), so native code can hold raw pointers without registering them.
class Gc {
public:
    explicit Gc(Vm& vm) : vm_(vm) {}
    ~Gc();
    void* allocate(size_t size);
    void collect();
    bool shouldCollect() const { return sinceGc_ > threshold_; }
    void noteExternal(size_t n) { sinceGc_ += n; }
    void mark(Cell* c) {
        if (c && !c->mark) {
            c->mark = 1;
            work_.push_back(c);
        }
    }
    void mark(Value v) {
        if (v.isCell()) mark(v.asCell());
    }
    size_t liveBytes() const { return live_; }
    size_t collections() const { return collections_; }
    struct Block;

private:
    Cell* findCell(uintptr_t p);
    void scanStack();
    void sweep();
    void destroyAll();
    Vm& vm_;
    std::vector<Block*> blocks_;
    std::unordered_map<uintptr_t, Block*> blockMap_;
    std::vector<Block*> classBlocks_[16];
    std::vector<Cell*> work_;
    size_t sinceGc_ = 0, threshold_ = 8u << 20, live_ = 0, collections_ = 0;
};

struct JSThrow {};  // C++ exception used to unwind; the JS value is in Vm::pending

enum class Err { Error, TypeError, RangeError, SyntaxError, ReferenceError, EvalError, URIError };

struct Frame {
    JSFunction* fn;
    FuncProto* code;
    const Ins* pc;
    Value* regs;
    Context* ctx;
    Value* ret;  // where the caller wants the result
    Value newTarget;
    JSArray* args;
    JSGenerator* gen;
    uint16_t ctxDepth;
    bool entry;      // entered from C++: runFrames() returns when this frame finishes
    bool construct;
    uint16_t callerRealm;  // of a constructor's frame: the realm that was running when it was called
};

struct Job {
    uint8_t kind;  // 0 fulfil reaction, 1 reject reaction, 2 thenable, 3 plain call
    Value a, b, c;
};

struct Atoms {
    JSString *length, *prototype, *constructor, *name, *message, *stack, *toString, *valueOf, *value, *done, *next, *get, *set,
        *enumerable, *configurable, *writable, *undefined, *null_, *true_, *false_, *object, *function, *string, *number, *boolean,
        *symbol, *empty, *arguments, *callee, *return_, *then, *index, *input, *cause, *errors, *lastIndex, *proto, *default_,
        *raw, *toJSON, *join, *source, *flags, *global, *status, *reason;
};

// What one realm owns (ECMA-262 9.3): its global object and its intrinsics. Vm derives from this, so the engine
// reads them as plain members (vm.objectProto): they are always those of the realm that is running. The fields
// of the other realms wait in Vm::realms, and Vm::enterRealm exchanges them.
struct RealmFields {
    JSObject* global = nullptr;
    JSObject *objectProto = nullptr, *functionProto = nullptr, *arrayProto = nullptr, *stringProto = nullptr, *numberProto = nullptr,
             *booleanProto = nullptr, *symbolProto = nullptr, *bigintProto = nullptr, *errorProto = nullptr, *iteratorProto = nullptr, *arrayIterProto = nullptr,
             *mapProto = nullptr, *setProto = nullptr, *weakMapProto = nullptr, *weakSetProto = nullptr, *promiseProto = nullptr,
             *generatorProto = nullptr, *dateProto = nullptr, *regexpProto = nullptr, *mapIterProto = nullptr, *setIterProto = nullptr,
             *stringIterProto = nullptr, *globalLex = nullptr;
    JSObject* errorProtos[7] = {};
    JSObject* typedProtos[12] = {};
    Value typedCtors[12];
    JSObject *arrayBufferProto = nullptr, *dataViewProto = nullptr, *sharedBufferProto = nullptr;
    Value arrayBufferCtor, sharedBufferCtor;
    JSObject *asyncIteratorProto = nullptr, *asyncGeneratorProto = nullptr, *asyncFromSyncProto = nullptr;
    // %GeneratorFunction.prototype%, %AsyncGeneratorFunction.prototype%, %AsyncFunction.prototype% (27.3.3, 27.4.3, 27.7.3)
    JSObject *generatorFunctionProto = nullptr, *asyncGeneratorFunctionProto = nullptr, *asyncFunctionProto = nullptr;
    Value throwTypeError;  // %ThrowTypeError% (10.2.4.1): behind Function.prototype.caller and a strict arguments.callee
    Value arrayCtorValue, arraySpeciesGetter;  // the built-in Array and the getter of Array[Symbol.species]
    Value arrayValuesFn, arrayIterNextFn;  // the original Array.prototype.values and array iterator next
    Value evalFn;  // the built-in eval: a call that names it directly sees the caller's variables
    Value importHook;  // called for import(): (specifier, options, file of the calling code)
    Value moduleHost;  // the module loader's `host` object: the embedder's resolve and fetch functions live on it
    // once Temporal or Intl is built: the object its JavaScript filled in (prelude.cpp bundleNatives)
    Value temporalNatives, intlNatives;
    struct PropWatch {  // one property of RegExp.prototype as the engine made it (regexp.cpp)
        PropKey key;
        uint64_t value;
        uint8_t flags;
    };
    std::vector<PropWatch> regexpWatch;
    // The legacy static properties of RegExp ($1, lastMatch, ...): the subject and offsets of the last match, or
    // not valid after a match by a RegExp subclass.
    Value regexpLastInput;
    std::vector<int> regexpLastCaps;
    bool regexpLegacyValid = true;
    Value regexpCtor, promiseCtor;
    Value suppressedErrorCtor;  // %SuppressedError%, for the errors `using` declarations collect
    // The prototypes built-in constructors fall back to (10.1.14 GetPrototypeFromConstructor). Every realm lists
    // them in the same order, which is how the one that stands for another realm's is found.
    std::vector<JSObject*> intrinsicProtos;
};

class Vm : public RealmFields {
public:
    Vm();
    ~Vm();

    // ---- embedding API
    Value eval(std::u16string_view src, const std::string& file);  // throws JSThrow; the value is in `pending`
    bool run(std::u16string_view src, const std::string& file, std::u16string* result, std::u16string* error);
    void runMicrotasks();
    std::u16string describeError(Value err);
    std::u16string inspect(Value v, int depth = 2, bool quoteStrings = true);
    std::function<void(int level, const std::u16string& text)> onConsole;  // level: 0 log, 1 info, 2 warn, 3 error
    Value pending;  // the value being thrown

    // ---- realms: the engine starts with realm 0, and a function always runs in the realm it was made in
    std::vector<std::unique_ptr<RealmFields>> realms;  // realms[i]: the fields of realm i while it is not the running one
    uint16_t currentRealm = 0;
    uint16_t createRealm();          // a new global object with its own intrinsics (9.3.1); returns the realm's number
    void enterRealm(uint16_t id);    // makes realm `id` the running one
    RealmFields& fieldsOf(uint16_t id) { return id == currentRealm ? *this : *realms[id]; }
    uint16_t functionRealm(JSObject* fn);  // GetFunctionRealm (7.3.24)

    // ---- allocation
    Gc gc{*this};
    template <class T>
    T* alloc() {
        void* mem = gc.allocate(sizeof(T));
        return new (mem) T();
    }
    JSString* str(std::u16string s);
    JSString* str(std::string_view ascii);
    JSString* atom(std::u16string_view s);
    JSString* atom(std::string_view ascii);
    JSString* atomOf(JSString* s);
    Symbol* newSymbol(JSString* desc);
    JSObject* newObject(JSObject* proto, Cls cls = Cls::Object);
    JSObject* newObject() { return newObject(objectProto); }
    JSArray* newArray(size_t n = 0);
    JSNative* newNative(NativeFn fn, std::string_view name, int length);
    JSObject* newError(Err type, const std::u16string& msg);
    JSIter* newIter(Cls cls, Value target, uint8_t mode);
    Value strv(std::u16string s) { return Value::cell(str(std::move(s))); }
    Value strv(std::string_view s) { return Value::cell(str(s)); }

    // ---- errors
    [[noreturn]] void throwValue(Value v);
    [[noreturn]] void throwError(Err type, const char* fmt, ...);
    [[noreturn]] void throwError(Err type, const std::u16string& msg);

    // ---- conversions
    bool toBool(Value v);
    double toNumber(Value v);
    double toIndex(Value v);  // typedarray.cpp
    Value toNumeric(Value v);             // a Number or a BigInt (bigint.cpp)
    JSBigInt* toBigInt(Value v);
    Value arith(int op, Value x, Value y);  // binary numeric operators when the operands are not both numbers
    Value unaryNumeric(int op, Value x);    // -, ~, ++ and -- likewise
    int32_t toInt32(Value v);
    uint32_t toUint32(Value v) { return (uint32_t)toInt32(v); }
    double toInteger(Value v);
    JSString* toString(Value v);
    const std::u16string& toStr16(Value v) { return toString(v)->s; }
    JSObject* toObject(Value v);
    Value toPrimitive(Value v, int hint);  // 0 default, 1 number, 2 string
    Value ordinaryToPrimitive(Value v, int hint);  // 7.1.1.1: hint 1 number first, 2 string first
    PropKey toKey(Value v);
    Value keyToValue(PropKey k);
    JSString* typeOf(Value v);
    JSString* numberToString(double d, int radix = 10);
    double stringToNumber(const std::u16string& s);
    bool strictEquals(Value a, Value b);
    bool looseEquals(Value a, Value b);
    bool sameValueZero(Value a, Value b);
    bool sameValue(Value a, Value b);
    bool isCallable(Value v);
    bool isConstructor(Value v);
    bool isArray(Value v) {  // IsArray (7.2.2): a proxy for an array counts
        while (v.isObject() && v.asObject()->cls == Cls::Proxy) {
            if (!((JSProxy*)v.asObject())->handler.isObject()) throwError(Err::TypeError, "Cannot perform 'IsArray' on a proxy that has been revoked");
            v = ((JSProxy*)v.asObject())->target;
        }
        return v.isObject() && v.asObject()->cls == Cls::Array;
    }
    Value add(Value a, Value b);
    Value concat(Value a, Value b);  // both must be strings
    int compare(Value a, Value b);  // -1 less, 0 equal, 1 greater, 2 unordered (NaN)

    // ---- objects
    Value get(Value target, PropKey k);
    Value getObj(JSObject* o, PropKey k, Value receiver);
    Value get(Value target, JSString* name) { return get(target, PropKey::of(name)); }
    Value get(Value target, const char* name) { return get(target, PropKey::of(atom(name))); }
    bool set(Value target, PropKey k, Value v);  // false when the assignment did not happen ([[Set]] returned false)
    [[noreturn]] void failedSet(Value target, PropKey k);  // the TypeError strict code gets for that
    bool setObj(JSObject* o, PropKey k, Value v, Value receiver);
    void addPrivateMember(JSObject* o, PropKey k, int kind, Value fn);
    bool getOwnDesc(JSObject* o, PropKey k, PropDesc& d);
    bool compatibleDesc(bool extensible, const PropDesc& d, bool exists, const PropDesc& current);
    // the object's internal methods that are not plain property access (10.1.1 to 10.1.4), proxy-aware
    JSObject* getProto(JSObject* o);
    bool setProtoOf(JSObject* o, JSObject* proto);
    bool isExtensible(JSObject* o);
    bool preventExtensions(JSObject* o);
    bool proxyGetOwn(JSObject* o, PropKey k, PropDesc& out);
    bool proxyDefineOwn(JSObject* o, PropKey k, const PropDesc& d);
    bool proxyHas(JSObject* o, PropKey k);
    Value proxyGet(JSObject* o, PropKey k, Value receiver);
    bool proxySet(JSObject* o, PropKey k, Value v, Value receiver);
    bool proxyDelete(JSObject* o, PropKey k);
    void proxyOwnKeys(JSObject* o, std::vector<PropKey>& out);
    Value proxyCall(JSObject* o, Value thisv, const Value* args, int argc);
    Value proxyConstruct(JSObject* o, const Value* args, int argc, Value newTarget);
    bool defineOwnDesc(JSObject* o, PropKey k, const PropDesc& d);
    bool defineElement(JSObject* o, PropKey k, Value v, uint8_t flags);
    void toPropDesc(Value v, PropDesc& d);
    Value fromPropDesc(const PropDesc& d);
    bool setIntegrity(JSObject* o, bool frozen);
    bool testIntegrity(JSObject* o, bool frozen);
    bool protoIndexProps = false;  // some realm's Array.prototype or Object.prototype has gained an index-named property
    void pinArgumentsLength(JSObject* o);
    static constexpr uint8_t kModuleHostKind = 200;  // JSHost::hostKind of a module namespace object
    static bool isHtmlDda(Value v) { return v.isObject() && v.asObject()->cls == Cls::Native && ((JSNative*)v.asObject())->htmlDda; }
    Value* mappedArg(JSObject* o, PropKey k);  // the parameter an arguments element is still tied to, or null
    void unmapArg(JSObject* o, PropKey k);
    void set(Value target, const char* name, Value v) { set(target, PropKey::of(atom(name)), v); }
    bool getOwn(JSObject* o, PropKey k, Prop& out);  // virtual properties included (array elements, length...)
    bool defineOwn(JSObject* o, PropKey k, Value v, uint8_t flags);
    void defineAccessor(JSObject* o, PropKey k, Value getter, Value setter, uint8_t flags);
    bool has(JSObject* o, PropKey k);
    bool hasOwn(JSObject* o, PropKey k);
    bool del(JSObject* o, PropKey k);
    void ownKeys(JSObject* o, std::vector<PropKey>& out, bool strings, bool symbols, bool onlyEnumerable);
    JSObject* protoOf(Value v);
    void def(JSObject* o, const char* name, Value v, uint8_t flags = PF_W | PF_C);
    JSNative* defFn(JSObject* o, const char* name, NativeFn fn, int length, uint8_t flags = PF_W | PF_C);
    // A writable, configurable, non-enumerable data property whose value `make` returns the first time it is read.
    void defLazy(JSObject* o, const char* name, NativeFn make);
    // Makes the value of a lazy property now. Does nothing for any other property.
    void materialize(JSObject* o, PropKey k);
    void defGetter(JSObject* o, const char* name, NativeFn getter, NativeFn setter = nullptr);
    uint32_t lengthOf(Value v);
    bool instanceOf(Value v, Value ctor);
    JSObject* protoFromNewTarget(Value newTarget, JSObject* fallback);
    void copyDataProps(JSObject* dst, Value src, JSArray* excluded);
    Value fnProtoProp(JSFunction* f);

    // ---- calls
    Value call(Value fn, Value thisv, const Value* args, int argc);
    Value construct(Value fn, const Value* args, int argc, Value newTarget);
    Value call(Value fn, Value thisv) { return call(fn, thisv, nullptr, 0); }
    Value call(Value fn, Value thisv, Value a0) { return call(fn, thisv, &a0, 1); }
    Value invoke(Value target, const char* method, const Value* args = nullptr, int argc = 0);
    // kind: 0 function, 1 generator, 2 async function, 3 async generator
    Value compileFunction(const std::u16string& params, const std::u16string& body, const std::string& file, int kind = 0, Value newTarget = Value::undef(),
                          bool dynamic = false);
    std::string activeFile(const char* otherwise) const;  // the file of the script that is running, or `otherwise`

    // ---- iteration helpers for natives
    bool iterate(Value iterable, const std::function<bool(Value)>& each);  // false from the callback stops early
    Value makeIterResult(Value v, bool done);
    bool iterStep(JSIter* it, Value& out);

    // ---- promises / jobs
    JSPromise* newPromise();
    void resolvePromise(JSPromise* p, Value v);
    void rejectPromise(JSPromise* p, Value v);
    void fulfillPromise(JSPromise* p, Value v);
    JSPromise* promiseThen(JSPromise* p, Value onFul, Value onRej, bool wantResult = true);
    JSPromise* promiseResolve(Value v);
    struct PromiseCapability {
        Value promise, resolve, reject;
    };
    PromiseCapability newPromiseCapability(Value ctor);          // 27.2.1.5
    Value promiseResolveWith(Value ctor, Value x);                // PromiseResolve (27.2.4.7.1)
    void promiseThenWith(JSPromise* p, Value onFul, Value onRej, Value resolve, Value reject);  // PerformPromiseThen
    Value speciesConstructor(JSObject* o, Value fallback);       // 7.3.22
    bool ordinaryHasInstance(Value ctor, Value v);                // 7.3.20
    void setFunctionName(Value fn, Value key, int prefix);       // 10.2.9, for a key only known at run time; prefix: 1 get, 2 set
    void enqueueJob(const Job& j) { jobQueue.push_back(j); }
    std::deque<Job> jobQueue;
    std::vector<Value> rejections;              // rejected promises nobody has handled yet
    std::vector<std::vector<Value>*> rootVecs;  // extra GC roots registered by natives
    std::vector<std::function<void(Gc&)>> rootTracers;  // embedder callbacks that mark what they keep alive
    void* hostData = nullptr;                           // the embedder's context (the page a script belongs to)

    // ---- well-known objects
    Atoms a;
    bool canBlock = false;  // may Atomics.wait put this thread to sleep?
    struct AsyncWaiter {  // an Atomics.waitAsync call that has not been answered yet
        Value resolve;
        uint32_t id;
        const uint8_t* place;  // where in shared memory it waits
    };
    std::vector<AsyncWaiter> asyncWaiters;
    uint32_t nextWaiterId = 1;
    // Agents (9.7) are threads, each with a Vm of its own, that share only the memory of SharedArrayBuffers. When
    // another agent's Atomics.notify wakes one of this agent's waitAsync calls, the waiter's id is left here, and
    // this agent settles the promise in its own time (settleNotifiedWaiters, called from the event loop).
    std::mutex notifiedLock;
    std::vector<uint32_t> notifiedWaiters;
    JSString* callerAtom = nullptr;
    JSObject* genReturnSignal = nullptr;  // thrown inside a generator to make it return through its finally blocks
    std::vector<JSObject*> joinStack;     // arrays whose join() is running, to stop self-reference
    // The well-known symbols, shared by every realm (6.1.5.1)
    Symbol *symUnscopables = nullptr, *symIsConcatSpreadable = nullptr;
    Symbol* symErrorStack = nullptr;  // hidden key: the stack trace an Error object was made with (read by Error.prototype.stack)
    Symbol *symIterator = nullptr, *symAsyncIterator = nullptr, *symHasInstance = nullptr, *symToPrimitive = nullptr,
           *symToStringTag = nullptr, *symSpecies = nullptr, *symMatch = nullptr, *symMatchAll = nullptr, *symReplace = nullptr,
           *symSearch = nullptr, *symSplit = nullptr, *symDispose = nullptr, *symAsyncDispose = nullptr;
    std::unordered_map<std::u16string, Symbol*> symbolRegistry;

    // ---- interpreter internals
    Value runFrames(size_t base);
    void pushFrame(JSFunction* f, Value* base, int argc, Value newTarget, Value* ret, bool entry, bool construct);
    Value callFunction(JSFunction* f, Value thisv, const Value* args, int argc, Value newTarget, bool construct);
    Value resumeGenerator(JSGenerator* g, Value sent, int mode, bool& done);  // mode 0 next, 1 throw, 2 return
    Value startGenerator(JSFunction* f, Value thisv, const Value* args, int argc);
    bool plainArrayIteration();
    Value startAsyncGenerator(JSFunction* f, Value thisv, const Value* args, int argc);
    Value getAsyncIterator(Value src, Value& next);
    Value asyncFromSyncIterator(Value syncIterator);
    Value delegateCall(Value* regs);
    void delegateResult(Value* regs, Value& value, bool& done);
    Value startAsync(JSFunction* f, Value thisv, const Value* args, int argc);
    void asyncStep(JSGenerator* g, Value sent, int mode);
    void initFields(JSFunction* f, Value obj);
    JSFunction* makeClosure(FuncProto* p, Context* ctx, Frame* parent);
    std::u16string captureStack();
    void markRoots(Gc& g);
    void runJob(const Job& j);
    void initRealm();  // builds the running realm's global object and intrinsics

    std::vector<Value> stack;
    std::vector<Frame> frames;
    Value* stackTop = nullptr;
    Value* stackEnd = nullptr;
    int depth = 0;             // C++ re-entrancy depth (native -> JS -> native ...)
    void checkStack();         // RangeError when the thread's stack is nearly used up: every recursion in native code asks
    bool yielded = false;      // runFrames returned because of yield/await
    bool resumeThrow = false;  // resume a generator by throwing `pending` at the yield

    // ---- time limit: a script still running at deadlineTick (a GetTickCount64 value, 0 = no limit) is stopped.
    // Once terminated, try/catch in the script no longer sees the error, so it cannot keep itself alive.
    std::atomic<uint64_t> deadlineTick{0};  // another agent's thread may set it, to stop this one
    bool terminated = false;
    uint16_t pollTicks = 0;
    void pollInterrupt();
    bool traceThrows = false;  // debugging aid: report every exception to the console when it is thrown
    JSObject* freshError = nullptr;
    std::unordered_map<std::u16string_view, JSString*> atomTable;
    void setupGlobals();
    void setupCollections();
};

void settleNotifiedWaiters(Vm& vm);  // typedarray.cpp: resolves the waitAsync promises other agents have notified
void forgetWaiters(Vm& vm);          // typedarray.cpp: an engine that goes away waits for nothing any more

// RAII: the code in its scope runs in the given realm.
struct RealmScope {
    Vm& vm;
    uint16_t from;
    RealmScope(Vm& v, uint16_t realm) : vm(v), from(v.currentRealm) {
        if (realm != from) vm.enterRealm(realm);
    }
    ~RealmScope() {
        if (vm.currentRealm != from) vm.enterRealm(from);
    }
};

// RAII: keeps the values of a std::vector alive across calls that may collect garbage.
struct RootScope {
    Vm& vm;
    explicit RootScope(Vm& v, std::vector<Value>& vec) : vm(v) { vm.rootVecs.push_back(&vec); }
    ~RootScope() { vm.rootVecs.pop_back(); }
};

// Parses and compiles source text into bytecode. Throws JSThrow (SyntaxError) on failure.
// With `module` set the source is compiled as an ES module: the result evaluates to an async function taking
// (own namespace, import.meta), and `module` receives what the loader needs to link it (ECMA-262 16.2.1.7).
struct ModuleInfo {
    struct Export {
        std::u16string name;
        int slot;                  // slot in the module's scope, or -1 for a re-export
        int request;               // index of the module re-exported from, or -1
        std::u16string importName; // name in that module
        bool named;                // false: re-exports that module's whole namespace (export * as x)
    };
    std::vector<std::u16string> requests;  // module specifiers
    std::vector<std::u16string> requestTypes;  // the `type` import attribute of each request, or empty
    std::vector<int> requestSlots;         // scope slot that receives each requested module's namespace
    std::vector<Export> exports;
    std::vector<int> stars;                               // export * from
    std::vector<std::pair<int, std::u16string>> imports;  // named imports: request index, name
    bool hasTopLevelAwait = false;
};
FuncProto* compileScript(Vm& vm, std::shared_ptr<const std::u16string> src, const std::string& file, bool functionBody,
                         const std::u16string* params, ModuleInfo* module = nullptr);
void moduleInit(Vm& vm, Value ns, Context* ctx);
FuncProto* compileEval(Vm& vm, std::shared_ptr<const std::u16string> src, const EvalSite& site, const std::string& callerFile);
FuncProto* compileDynamicFunction(Vm& vm, std::shared_ptr<const std::u16string> src, const std::u16string& params, int kind, const std::string& file,
                                  bool dynamic);

std::u16string utf8to16(std::string_view s);
std::string utf16to8(std::u16string_view s);

}  // namespace tb::js
