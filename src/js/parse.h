// AST, scopes and the parser interface for the JavaScript engine.
#pragma once
#include "js.h"

namespace tb::js {

// Punctuators and operator keywords.
enum P : uint8_t {
    P_LBrace, P_RBrace, P_LParen, P_RParen, P_LBrack, P_RBrack, P_Dot, P_Ellipsis, P_Semi, P_Comma,
    P_Lt, P_Gt, P_Le, P_Ge, P_EqEq, P_Ne, P_SEq, P_SNe,
    P_Plus, P_Minus, P_Star, P_Slash, P_Percent, P_StarStar, P_Inc, P_Dec,
    P_Shl, P_Shr, P_UShr, P_Amp, P_Pipe, P_Caret, P_Bang, P_Tilde, P_AndAnd, P_OrOr, P_QQ,
    P_Question, P_QDot, P_Colon,
    P_Assign, P_PlusEq, P_MinusEq, P_StarEq, P_SlashEq, P_PercentEq, P_StarStarEq, P_ShlEq, P_ShrEq, P_UShrEq,
    P_AmpEq, P_PipeEq, P_CaretEq, P_AndAndEq, P_OrOrEq, P_QQEq, P_Arrow,
    P_Typeof, P_Void, P_Delete, P_In, P_InstanceOf, P_None
};

enum class N : uint8_t {
    Num, Str, Template, TaggedTemplate, Regex, Ident, This, Super, NewTarget, Null, True, False,
    Array, Object, Property, Function, Class,
    Unary, Update, Binary, Logical, Assign, Cond, Call, New, Member, Index, OptChain, Spread, Seq, Yield, Await, Hole,
    ArrayPat, ObjectPat, AssignPat, Rest,
    VarDecl, Declarator, ExprStmt, Block, Empty, If, For, ForIn, ForOf, While, DoWhile, Return, Break, Continue, Throw,
    Try, Switch, Case, Labeled, Debugger, Program, ImportCall
};

enum : uint16_t {
    NF_COMPUTED = 1, NF_STATIC = 2, NF_SHORTHAND = 4, NF_OPTIONAL = 8, NF_PREFIX = 16, NF_DELEGATE = 32, NF_PRIVATE = 64,
    NF_PAREN = 128, NF_EXPRBODY = 256
};
enum : uint8_t { PK_INIT, PK_GET, PK_SET, PK_METHOD, PK_SPREAD, PK_FIELD };
enum : uint8_t { VK_VAR, VK_LET, VK_CONST, VK_PARAM, VK_FUNC, VK_CLASS, VK_CATCH, VK_FUNCNAME, VK_PSEUDO, VK_PRIVATE, VK_IMPORT };

struct Scope;
struct FunctionInfo;

struct Variable {
    JSString* name = nullptr;
    uint8_t kind = VK_VAR;
    bool captured = false, needsTdz = false, isGlobal = false;
    int reg = -1, slot = -1;
    Scope* scope = nullptr;
    uint32_t declEnd = 0;
    Variable* importNs = nullptr;     // VK_IMPORT: the variable holding the source module's namespace
    JSString* importName = nullptr;  // VK_IMPORT: the export it stands for (null: the namespace itself)
};

struct Node {
    N type;
    uint8_t op = 0;
    uint16_t flags = 0;
    uint32_t line = 0, pos = 0;
    Node *a = nullptr, *b = nullptr, *c = nullptr, *d = nullptr;
    std::vector<Node*> list;
    JSString* str = nullptr;
    JSString* str2 = nullptr;
    double num = 0;
    Variable* var = nullptr;
    Scope* scope = nullptr;
    FunctionInfo* fn = nullptr;
    std::vector<JSString*> strs;  // template quasis: cooked, raw, cooked, raw...
};

struct Scope {
    enum K : uint8_t { Global, Function, Block, Catch, Class, For } kind = Block;
    Scope* parent = nullptr;
    FunctionInfo* fn = nullptr;
    std::vector<Variable*> vars;
    std::unordered_map<JSString*, Variable*> map;
    std::vector<std::pair<Node*, bool>> refs;  // unresolved identifier references (node, crossed a function boundary)
    std::vector<Node*> funcDecls;              // function declarations hoisted to the top of this scope
    // filled in by the compiler
    bool hasCtx = false;
    int ctxSlots = 0;
    Variable* find(JSString* name) {
        if (vars.size() <= 8) {
            for (Variable* v : vars)
                if (v->name == name) return v;
            return nullptr;
        }
        if (map.size() != vars.size()) {
            map.clear();
            for (Variable* v : vars) map[v->name] = v;
        }
        auto it = map.find(name);
        return it == map.end() ? nullptr : it->second;
    }
};

struct FunctionInfo {
    Scope* scope = nullptr;
    std::vector<Node*> params;
    std::vector<Node*> body;
    JSString* name = nullptr;
    bool isArrow = false, isAsync = false, isGenerator = false, isMethod = false, isGetter = false, isSetter = false,
         isClassCtor = false, isDerived = false, strict = false, isDecl = false, exprBody = false, usesSuper = false,
         isFieldInit = false;
    Variable *thisVar = nullptr, *argsVar = nullptr, *selfVar = nullptr, *newTargetVar = nullptr;
    uint32_t srcStart = 0, srcEnd = 0, line = 0;
    int length = 0;
    FunctionInfo* parent = nullptr;
};

struct Ast {
    std::deque<Node> nodes;
    std::deque<Variable> vars;
    std::deque<Scope> scopes;
    std::deque<FunctionInfo> fns;
    FunctionInfo* top = nullptr;
    std::shared_ptr<const std::u16string> src;
    // ES module being compiled (moduleFn is null for scripts). ECMA-262 16.2.1.7: the fields below are the
    // module record's [[RequestedModules]], import entries and local/indirect/star export entries.
    struct Export {
        JSString* name;        // exported name
        JSString* local;       // module-scope variable, or null for a re-export
        Variable* var;         // resolved local
        int request;           // index into requests for a re-export, else -1
        JSString* importName;  // name in the source module (null: its whole namespace)
        uint32_t line;
    };
    struct Import {
        int request;
        JSString* name;
    };
    FunctionInfo* moduleFn = nullptr;
    bool hasTLA = false;                 // the module body awaits at its top level
    std::vector<JSString*> requests;     // module specifiers in source order, without repeats
    std::vector<Variable*> requestVars;  // the variable that holds each requested module's namespace
    std::vector<Export> exports;
    std::vector<Import> imports;  // named imports, checked when the module is linked
    std::vector<int> starRequests;
};

struct SyntaxErr {
    std::string msg;
    uint32_t line;
};

// Parses a whole script, or with `asModule` an ES module.
void parseScript(Vm& vm, Ast& ast, bool asModule, const std::u16string* params);

}  // namespace tb::js
