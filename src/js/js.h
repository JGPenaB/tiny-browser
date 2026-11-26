// Tiny Browser's own JavaScript engine: values, heap objects and the virtual machine interface.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
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
    inline bool isObject() const;
    inline JSString* asString() const;
    inline JSObject* asObject() const;
    bool operator==(Value o) const { return b == o.b; }
    bool operator!=(Value o) const { return b != o.b; }
};

// ---------------------------------------------------------------------------- heap cells
enum class CK : uint8_t { String, Symbol, Object, Context, Code, Accessor };
enum class Cls : uint8_t {
    Object, Array, Arguments, Function, Native, Bound, Error, Boolean, Number, String, SymbolObj, Date, RegExp,
    Map, Set, WeakMap, WeakSet, Promise, Generator, ArrayIter, MapIter, SetIter, StringIter, ForIn, Global, Host, ArrayBuffer, TypedArray, DataView
};

struct Cell {
    virtual ~Cell() = default;
    virtual void trace(Gc&) {}
    CK kind = CK::Object;
    uint8_t mark = 0;
    Cls cls = Cls::Object;
    uint8_t oflags = 0;
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
    Symbol() { kind = CK::Symbol; }
    void trace(Gc&) override;
};

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

enum : uint8_t { PF_W = 1, PF_E = 2, PF_C = 4, PF_ACC = 8, PF_ALL = 7 };
enum : uint8_t { OF_NOEXT = 1, OF_FROZEN = 2, OF_CTOR = 4 };

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
    std::vector<Value> elems;
    JSArray() { cls = Cls::Array; }
    void trace(Gc&) override;
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

struct FuncProto : Cell {
    std::vector<Ins> code;
    std::vector<Value> consts;
    std::vector<FuncProto*> inner;
    std::vector<Handler> handlers;
    std::vector<uint32_t> lines;
    JSString* name = nullptr;
    uint16_t nparams = 0, length = 0;
    uint32_t nregs = 2;
    bool isArrow = false, isStrict = false, needsArgs = false, isGenerator = false, isAsync = false, isClassCtor = false,
         isDerived = false, isMethod = false;
    std::shared_ptr<const std::u16string> source;
    uint32_t srcStart = 0, srcEnd = 0;
    std::string file;
    FuncProto() { kind = CK::Code; }
    void trace(Gc&) override;
};

struct JSFunction : JSObject {
    FuncProto* code = nullptr;
    Context* ctx = nullptr;
    JSObject* home = nullptr;  // [[HomeObject]] for super lookups
    Value fields;              // class field initialisers: array of key, function pairs
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
struct JSArrayBuffer : JSObject {
    std::vector<uint8_t> bytes;
    JSArrayBuffer() { cls = Cls::ArrayBuffer; }
};
enum class TA : uint8_t { I8, U8, U8C, I16, U16, I32, U32, F32, F64 };
struct JSTypedArray : JSObject {  // also backs DataView (cls DataView, lengths in bytes)
    JSArrayBuffer* buffer = nullptr;
    uint32_t offset = 0, length = 0;  // byte offset into the buffer, length in elements
    TA type = TA::U8;
    JSTypedArray() { cls = Cls::TypedArray; }
    void trace(Gc&) override;
};
Value typedGet(const JSTypedArray* t, uint32_t i);  // i must be below t->length
void typedSet(Vm& vm, JSTypedArray* t, uint32_t i, Value v);
JSTypedArray* newTypedArray(Vm& vm, TA type, uint32_t length);

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

class Vm {
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
    JSObject* global = nullptr;
    Value pending;  // the value being thrown

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
    int32_t toInt32(Value v);
    uint32_t toUint32(Value v) { return (uint32_t)toInt32(v); }
    double toInteger(Value v);
    JSString* toString(Value v);
    const std::u16string& toStr16(Value v) { return toString(v)->s; }
    JSObject* toObject(Value v);
    Value toPrimitive(Value v, int hint);  // 0 default, 1 number, 2 string
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
    bool isArray(Value v) { return v.isObject() && v.asObject()->cls == Cls::Array; }
    Value add(Value a, Value b);
    Value concat(Value a, Value b);  // both must be strings
    int compare(Value a, Value b);  // -1 less, 0 equal, 1 greater, 2 unordered (NaN)

    // ---- objects
    Value get(Value target, PropKey k);
    Value getObj(JSObject* o, PropKey k, Value receiver);
    Value get(Value target, JSString* name) { return get(target, PropKey::of(name)); }
    Value get(Value target, const char* name) { return get(target, PropKey::of(atom(name))); }
    void set(Value target, PropKey k, Value v);
    bool setObj(JSObject* o, PropKey k, Value v, Value receiver);
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
    Value compileFunction(const std::u16string& params, const std::u16string& body, const std::string& file);

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
    void enqueueJob(const Job& j) { jobQueue.push_back(j); }
    std::deque<Job> jobQueue;
    std::vector<Value> rejections;              // rejected promises nobody has handled yet
    std::vector<std::vector<Value>*> rootVecs;  // extra GC roots registered by natives
    std::vector<std::function<void(Gc&)>> rootTracers;  // embedder callbacks that mark what they keep alive
    void* hostData = nullptr;                           // the embedder's context (the page a script belongs to)

    // ---- well-known objects
    Atoms a;
    JSObject *objectProto = nullptr, *functionProto = nullptr, *arrayProto = nullptr, *stringProto = nullptr, *numberProto = nullptr,
             *booleanProto = nullptr, *symbolProto = nullptr, *errorProto = nullptr, *iteratorProto = nullptr, *arrayIterProto = nullptr,
             *mapProto = nullptr, *setProto = nullptr, *weakMapProto = nullptr, *weakSetProto = nullptr, *promiseProto = nullptr,
             *generatorProto = nullptr, *dateProto = nullptr, *regexpProto = nullptr, *mapIterProto = nullptr, *setIterProto = nullptr,
             *stringIterProto = nullptr, *globalLex = nullptr;
    JSObject* errorProtos[7] = {};
    JSObject* typedProtos[9] = {};
    JSObject *arrayBufferProto = nullptr, *dataViewProto = nullptr;
    JSObject *asyncIteratorProto = nullptr, *asyncGeneratorProto = nullptr, *asyncFromSyncProto = nullptr;
    JSObject* genReturnSignal = nullptr;  // thrown inside a generator to make it return through its finally blocks
    Value importHook;  // called for import(): (specifier, options, file of the calling code)
    Symbol *symIterator = nullptr, *symAsyncIterator = nullptr, *symHasInstance = nullptr, *symToPrimitive = nullptr,
           *symToStringTag = nullptr, *symSpecies = nullptr;
    Value regexpCtor, promiseCtor;
    std::unordered_map<std::u16string, Symbol*> symbolRegistry;

    // ---- interpreter internals
    Value runFrames(size_t base);
    void pushFrame(JSFunction* f, Value* base, int argc, Value newTarget, Value* ret, bool entry, bool construct);
    Value callFunction(JSFunction* f, Value thisv, const Value* args, int argc, Value newTarget, bool construct);
    Value resumeGenerator(JSGenerator* g, Value sent, int mode, bool& done);  // mode 0 next, 1 throw, 2 return
    Value startGenerator(JSFunction* f, Value thisv, const Value* args, int argc);
    Value startAsyncGenerator(JSFunction* f, Value thisv, const Value* args, int argc);
    Value getAsyncIterator(Value src, Value& next);
    Value delegateCall(Value* regs);
    void delegateResult(Value* regs, Value& value, bool& done);
    Value startAsync(JSFunction* f, Value thisv, const Value* args, int argc);
    void asyncStep(JSGenerator* g, Value sent, int mode);
    void initFields(JSFunction* f, Value obj);
    JSFunction* makeClosure(FuncProto* p, Context* ctx, Frame* parent);
    std::u16string captureStack();
    void markRoots(Gc& g);
    void runJob(const Job& j);

    std::vector<Value> stack;
    std::vector<Frame> frames;
    Value* stackTop = nullptr;
    Value* stackEnd = nullptr;
    int depth = 0;             // C++ re-entrancy depth (native -> JS -> native ...)
    bool yielded = false;      // runFrames returned because of yield/await
    bool resumeThrow = false;  // resume a generator by throwing `pending` at the yield

    // ---- time limit: a script still running at deadlineTick (a GetTickCount64 value, 0 = no limit) is stopped.
    // Once terminated, try/catch in the script no longer sees the error, so it cannot keep itself alive.
    uint64_t deadlineTick = 0;
    bool terminated = false;
    uint16_t pollTicks = 0;
    void pollInterrupt();
    bool traceThrows = false;  // debugging aid: report every exception to the console when it is thrown
    JSObject* freshError = nullptr;
    std::unordered_map<std::u16string_view, JSString*> atomTable;
    void setupGlobals();
    void setupCollections();
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
    std::vector<int> requestSlots;         // scope slot that receives each requested module's namespace
    std::vector<Export> exports;
    std::vector<int> stars;                               // export * from
    std::vector<std::pair<int, std::u16string>> imports;  // named imports: request index, name
    bool hasTopLevelAwait = false;
};
FuncProto* compileScript(Vm& vm, std::shared_ptr<const std::u16string> src, const std::string& file, bool functionBody,
                         const std::u16string* params, ModuleInfo* module = nullptr);
void moduleInit(Vm& vm, Value ns, Context* ctx);

std::u16string utf8to16(std::string_view s);
std::string utf16to8(std::u16string_view s);

}  // namespace tb::js
