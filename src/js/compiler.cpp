// Bytecode compiler: walks the resolved AST and emits register-machine code.
#include <algorithm>
#include "bytecode.h"
#include "parse.h"

namespace tb::js {

namespace {

struct Loop {
    std::vector<JSString*> labels;
    std::vector<size_t> breaks, conts;
    int breakDepth = 0, contDepth = 0, iterReg = -1;
    bool isLoop = false, isSwitch = false, asyncIter = false;
    size_t finMark = 0;
};
struct Fin {
    Node* block;  // the finally block, or null for a scope whose `using` resources are disposed on the way out
    int ctxDepth;
    size_t loopMark;
    int disposeReg = -1;
    bool disposeAsync = false;
};
struct Fn {
    FunctionInfo* info = nullptr;
    FuncProto* p = nullptr;
    int nextReg = 2, maxReg = 2, ctxDepth = 0;
    uint32_t templateSites = 0;  // tagged templates seen so far in this function
    int disposeReg = -1;         // the resource list of the innermost scope with `using` declarations
    int tryDepth = 0;            // try blocks with a catch clause that are open: a call inside one is not a tail call
    struct Unwound {
        size_t start, end, index;
    };
    std::vector<Unwound> unwound;  // finally blocks compiled in line at a return, break or continue: where, and for which entry of fins
    size_t chainBase = 0;
    std::vector<Loop> loops;
    std::vector<Fin> fins;
    std::unordered_map<uint64_t, uint32_t> kmap;
    std::vector<std::vector<size_t>> chains;  // optional-chain bail-out jumps
    std::vector<JSString*> pendingLabels;
    int cptn = -1;  // eval code and scripts: the register that holds the completion value so far
};
struct ScopeMark {
    int regMark;
    bool ctx;
};

class Compiler {
public:
    Compiler(Vm& v, Ast& a, const std::string& file) : vm(v), ast(a), file_(file) {}

    FuncProto* program() {
        FunctionInfo* info = ast.top;
        Fn fn;
        begin(fn, info);
        fn.p->isTop = true;
        Scope* sc = info->scope;
        for (Variable* v : sc->vars) {
            if (!v->isGlobal || v->annexB) continue;
            bool lex = v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS;
            if (lex || v->kind == VK_VAR || v->kind == VK_FUNC) emitX(OP_CheckGlobalDecl, lex ? 0 : v->kind == VK_VAR ? 1 : 2, kstr(v->name));
        }
        for (Variable* v : sc->vars) {
            if (!v->isGlobal) continue;
            if (v->kind == VK_VAR) emitX(OP_DeclGlobalVar, 0, kstr(v->name));
            else if (v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS) emitX(OP_DeclGlobalLet, v->kind == VK_CONST, kstr(v->name));
        }
        enterScope(sc);
        prologuePseudo(info);
        hoist(sc);
        int result = newReg();
        emit(OP_LoadUndef, result);
        f->cptn = result;
        for (Node* s : info->body) stmt(s);
        emit(OP_Return, result);
        return end(fn);
    }

    // Direct eval: the code runs as a function whose closure is the caller's context at the call site.
    FuncProto* evalFunction() {
        chain = ast.evalScopes;
        return compileFunction(ast.evalFn);
    }

private:
    Vm& vm;
    Ast& ast;
    std::string file_;
    Fn* f = nullptr;
    std::vector<Scope*> chain;  // scopes that own a runtime context, innermost last
    int evalObjReg = -1;        // eval code: the register holding the caller's object of eval-made variables
    bool bindingParams = false; // the arguments are being put into the parameters: nothing to copy back yet
    bool tailCall = false;      // the call about to be compiled is in tail position (15.10.2)
    uint32_t curLine = 1;

    [[noreturn]] void fail(const std::string& m) { throw SyntaxErr{m, curLine}; }

    // ---------------------------------------------------------------- emit helpers
    size_t emit(Op op, int a = 0, int b = 0, int c = 0) {
        Ins i;
        i.op = op;
        i.a = (uint16_t)a;
        i.b = (uint16_t)b;
        i.c = (uint16_t)c;
        f->p->code.push_back(i);
        f->p->lines.push_back(curLine);
        return f->p->code.size() - 1;
    }
    size_t emitX(Op op, int a, uint32_t bx) {
        Ins i;
        i.op = op;
        i.a = (uint16_t)a;
        i.bx = bx;
        f->p->code.push_back(i);
        f->p->lines.push_back(curLine);
        return f->p->code.size() - 1;
    }
    size_t jump(Op op, int a = 0) { return emitX(op, a, 0); }
    size_t here() { return f->p->code.size(); }
    void patch(size_t at) { f->p->code[at].bx = (uint32_t)here(); }
    void patchTo(size_t at, size_t target) { f->p->code[at].bx = (uint32_t)target; }
    int newReg() {
        int r = f->nextReg++;
        if (f->nextReg > f->maxReg) f->maxReg = f->nextReg;
        if (r > 65000) fail("Function is too large");
        return r;
    }
    uint32_t konst(Value v) {
        auto it = f->kmap.find(v.b);
        if (it != f->kmap.end()) return it->second;
        f->p->consts.push_back(v);
        uint32_t i = (uint32_t)f->p->consts.size() - 1;
        f->kmap[v.b] = i;
        return i;
    }
    uint32_t kstr(JSString* s) { return konst(Value::cell(s)); }
    void loadNum(int dst, double d) {
        Value v = Value::num(d);
        if (v.isInt()) emitX(OP_LoadInt, dst, (uint32_t)v.asInt());
        else emitX(OP_LoadK, dst, konst(v));
    }
    // Names that look like array indices take the keyed path, where they are turned into index keys.
    static bool indexLike(JSString* name) { return !name->s.empty() && name->s[0] >= '0' && name->s[0] <= '9'; }
    void getProp(int dst, int obj, JSString* name) {
        uint32_t k = kstr(name);
        if (k < 65536 && !indexLike(name)) {
            emit(OP_GetProp, dst, obj, (int)k);
        } else {
            int t = newReg();
            emitX(OP_LoadK, t, k);
            emit(OP_GetElem, dst, obj, t);
            f->nextReg--;
        }
    }
    void setProp(int obj, JSString* name, int src) {
        uint32_t k = kstr(name);
        if (k < 65536 && !indexLike(name)) {
            emit(OP_SetProp, obj, (int)k, src);
        } else {
            int t = newReg();
            emitX(OP_LoadK, t, k);
            emit(OP_SetElem, obj, t, src);
            f->nextReg--;
        }
    }

    // ---------------------------------------------------------------- functions
    void begin(Fn& fn, FunctionInfo* info) {
        fn.info = info;
        fn.p = vm.alloc<FuncProto>();
        fn.p->name = info->name;
        fn.p->isArrow = info->isArrow;
        fn.p->isStrict = info->strict;
        fn.p->isGenerator = info->isGenerator;
        fn.p->isAsync = info->isAsync;
        fn.p->isClassCtor = info->isClassCtor;
        fn.p->isDerived = info->isDerived;
        fn.p->isMethod = info->isMethod;
        fn.p->namesValue = info->namesValue;
        fn.p->simpleParams = info->simpleParams;
        fn.p->isTop = info->isEvalBody || info == ast.moduleFn;
        fn.p->source = ast.src;
        fn.p->srcStart = info->srcStart;
        fn.p->srcEnd = info->srcEnd;
        fn.p->file = file_;
        fn.p->length = (uint16_t)info->length;
        fn.chainBase = chain.size();
        f = &fn;
    }
    FuncProto* end(Fn& fn) {
        fn.p->nregs = (uint32_t)fn.maxReg + 1;
        chain.resize(fn.chainBase);
        return fn.p;
    }

    void prologuePseudo(FunctionInfo* info) {
        if (info->argsVar) {
            int t = newReg();
            if (info->mappedVar && !info->isEvalBody) {
                for (Node* p : info->params) f->p->argSlots.push_back(p->var->argIndex == (int)f->p->argSlots.size() ? p->var->slot : -1);
            }
            emit(OP_CreateArgs, t);
            storeVar(info->argsVar, info->argsVar->name, t, true);
            if (info->mappedVar && !info->isEvalBody) storeVar(info->mappedVar, info->mappedVar->name, t, true);
            f->nextReg--;
            f->p->needsArgs = true;
        }
        if (info->thisVar) {
            if (info->isClassCtor && info->isDerived) {
                info->thisVar->needsTdz = true;
                int t = newReg();
                emit(OP_LoadEmpty, t);
                storeVar(info->thisVar, info->thisVar->name, t, true);
                f->nextReg--;
            } else
                storeVar(info->thisVar, info->thisVar->name, 1, true);
        }
        if (info->newTargetVar) {
            int t = newReg();
            emit(OP_LoadNewTarget, t);
            storeVar(info->newTargetVar, info->newTargetVar->name, t, true);
            f->nextReg--;
        }
        if (info->selfVar) storeVar(info->selfVar, info->selfVar->name, 0, true);
        if (info->ctorVar) storeVar(info->ctorVar, info->ctorVar->name, 0, true);
    }

    FuncProto* compileFunction(FunctionInfo* info) {
        Fn fn;
        Fn* parent = f;
        uint32_t savedLine = curLine;
        begin(fn, info);
        curLine = info->line;
        Scope* sc = info->scope;
        int np = (int)info->params.size();
        fn.p->nparams = (uint16_t)np;
        fn.nextReg = fn.maxReg = 2 + np;
        for (Variable* v : sc->vars) v->reg = -1;
        if (info->argsVar && !info->strict && info->simpleParams && !info->isArrow && np > 0 && !info->isEvalBody && info != ast.moduleFn) {
            // CreateMappedArgumentsObject (10.4.4.7): arguments[i] and the i-th parameter share a value, so the
            // parameters are kept in the context, where the arguments object can reach them after the call too
            for (int i = 0; i < np; ++i) {  // with a repeated name the last parameter is the one that is tied
                info->params[i]->var->captured = true;
                info->params[i]->var->argIndex = i;
            }
            if (!info->mappedVar) {
                Variable& hv = ast.vars.emplace_back();
                hv.name = vm.atom("%mapped");
                hv.kind = VK_PSEUDO;
                hv.scope = sc;
                hv.captured = true;
                hv.declEnd = 0;
                sc->vars.push_back(&hv);
                info->mappedVar = &hv;
            }
        }
        for (int i = 0; i < np; ++i) {
            Node* p = info->params[i];
            Node* target = p->type == N::AssignPat ? p->a : p;
            if (target->type == N::Ident && !target->var->captured) target->var->reg = 2 + i;
        }
        enterScope(sc, true);
        if (info == ast.moduleFn) emit(OP_ModuleInit, 2);  // r2 is the namespace argument
        prologuePseudo(info);
        // Parameters read by a default value before they are bound (ECMA-262 10.2.11): the arguments are set
        // aside and those parameters start out uninitialised.
        bool paramTdz = false;
        for (Variable* v : sc->vars)
            if (v->kind == VK_PARAM && v->needsTdz) paramTdz = true;
        std::vector<int> argReg(np);
        for (int i = 0; i < np; ++i) argReg[i] = 2 + i;
        if (paramTdz) {
            for (int i = 0; i < np; ++i) {
                argReg[i] = newReg();
                emit(OP_Move, argReg[i], 2 + i);
            }
            for (Variable* v : sc->vars) {
                if (v->kind != VK_PARAM || !v->needsTdz) continue;
                if (v->captured) {
                    int t = newReg();
                    emit(OP_LoadEmpty, t);
                    emit(OP_SetCtx, t, 0, v->slot);
                    f->nextReg--;
                } else
                    emit(OP_LoadEmpty, v->reg);
            }
        }
        bindingParams = true;
        for (int i = 0; i < np; ++i) {
            Node* p = info->params[i];
            int reg = argReg[i];
            if (p->type == N::Rest) {
                emit(OP_CreateRest, reg, i);
                f->p->needsArgs = true;
                bindTarget(p->a, reg, true);
                continue;
            }
            Node* target = p;
            if (p->type == N::AssignPat) {
                size_t j = jump(OP_JmpNotUndef, reg);
                expr(p->b, reg);
                patch(j);
                target = p->a;
            }
            if (target->type == N::Ident) {
                if (target->var->captured || paramTdz) storeVar(target->var, target->str, reg, true);
            } else
                bindTarget(target, reg, true);
        }
        bindingParams = false;
        if (info->isGenerator) {
            // The arguments are bound when the generator is called; the body waits for the first next().
            int t = newReg();
            emit(OP_LoadUndef, t);
            emit(OP_Yield, t, t);
            f->nextReg--;
        }
        if (info->isEvalBody) {
            for (Variable* v : ast.evalGlobals)  // EvalDeclarationInstantiation (19.2.1.3): nothing is declared if one name cannot be
                if (!v->annexB && (v->kind == VK_VAR || v->kind == VK_FUNC)) emitX(OP_CheckGlobalDecl, v->kind == VK_VAR ? 1 : 2, kstr(v->name));
            for (Variable* v : ast.evalGlobals)
                if (v->kind == VK_VAR) emitX(OP_DeclGlobalVar, 1, kstr(v->name));
            if (ast.evalDynTarget) {
                Variable* ev = ast.evalDynTarget;
                evalObjReg = newReg();
                int name = newReg();
                emit(OP_GetCtx, evalObjReg, depthOf(ev), ev->slot);
                size_t have = jump(OP_JmpNotUndef, evalObjReg);
                emit(OP_NewScopeObj, evalObjReg);
                emit(OP_SetCtx, evalObjReg, depthOf(ev), ev->slot);
                patch(have);
                for (JSString* nm : ast.evalDynVars) {
                    emitX(OP_LoadK, name, kstr(nm));
                    emit(OP_ScopeDeclare, evalObjReg, name);
                }
                f->nextReg--;
            }
        }
        if (Scope* body = info->bodyScope) {  // the body's own variables (10.2.11 steps 28 to 36)
            enterScope(body);
            for (Variable* v : body->vars) {
                Variable* from = v->initFrom;
                if (!from) continue;
                int t = newReg();
                if (from->captured) emit(OP_GetCtx, t, depthOf(from), from->slot);
                else emit(OP_Move, t, from->reg);
                storeVar(v, v->name, t, true);
                f->nextReg--;
            }
            hoist(body);
        }
        hoist(sc);
        if (info->isEvalBody) {  // the value of eval(...) is the value of its last expression statement
            int result = newReg();
            emit(OP_LoadUndef, result);
            f->cptn = result;
            for (Node* s : info->body) stmt(s);
            emit(OP_Return, result);
            FuncProto* p = end(fn);
            f = parent;
            curLine = savedLine;
            return p;
        }
        if (info == ast.moduleFn) {
            // The module's scope now exists with its functions in place (ECMA-262 InitializeEnvironment). The
            // body waits here until the loader evaluates the module.
            int t = newReg();
            emit(OP_LoadUndef, t);
            emit(OP_Yield, t, t);
            f->nextReg--;
        }
        {
            Scope* bodyScope = info->bodyScope ? info->bodyScope : sc;
            if (bodyScope->usingKind) withDisposal(bodyScope->usingKind, [&] { for (Node* s : info->body) stmt(s); });
            else
                for (Node* s : info->body) stmt(s);
        }
        syncThis();
        emit(OP_RetUndef);
        FuncProto* p = end(fn);
        f = parent;
        curLine = savedLine;
        return p;
    }

    // A derived constructor whose `this` is shared with arrow functions: super() may have run inside one of them, so
    // the frame's own `this` is refreshed before the constructor returns.
    void syncThis() {
        FunctionInfo* info = f->info;
        if (info->isClassCtor && info->isDerived && info->thisVar && info->thisVar->captured) emit(OP_GetCtx, 1, depthOf(info->thisVar), info->thisVar->slot);
    }
    void closure(FunctionInfo* info, int dst) {
        FuncProto* p = compileFunction(info);
        f->p->inner.push_back(p);
        emitX(OP_Closure, dst, (uint32_t)f->p->inner.size() - 1);
    }

    // ---------------------------------------------------------------- scopes and variables
    ScopeMark enterScope(Scope* sc, bool isFunctionScope = false) {
        ScopeMark m{f->nextReg, false};
        if (isFunctionScope) m.regMark = 0;
        int slots = 0;
        for (Variable* v : sc->vars)
            if (!v->isGlobal && v->captured && v->kind != VK_IMPORT) v->slot = slots++;
        sc->hasCtx = slots > 0;
        if (slots) {
            emitX(OP_PushCtx, 0, (uint32_t)slots);
            chain.push_back(sc);
            f->ctxDepth++;
            m.ctx = true;
        }
        for (Variable* v : sc->vars) {
            if (v->isGlobal || v->captured) continue;
            if (isFunctionScope && v->kind == VK_PARAM && v->reg >= 0) continue;
            v->reg = newReg();
        }
        for (Variable* v : sc->vars) {
            if (v->isGlobal) continue;
            bool lex = v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS;
            if (lex && v->needsTdz) {
                if (v->captured) {
                    int t = newReg();
                    emit(OP_LoadEmpty, t);
                    emit(OP_SetCtx, t, 0, v->slot);
                    f->nextReg--;
                } else
                    emit(OP_LoadEmpty, v->reg);
            } else if (!v->captured && !(isFunctionScope && v->kind == VK_PARAM && v->reg < 2 + (int)f->info->params.size())) {
                emit(OP_LoadUndef, v->reg);
            }
        }
        return m;
    }
    void exitScope(const ScopeMark& m) {
        if (m.ctx) {
            emitX(OP_PopCtx, 0, 1);
            chain.pop_back();
            f->ctxDepth--;
        }
        f->nextReg = m.regMark;
    }
    void hoist(Scope* sc) {
        for (Node* fd : sc->funcDecls) {
            int t = newReg();
            closure(fd->fn, t);
            if (!fd->var && evalObjReg >= 0 && f->info->isEvalBody) {  // a function eval adds to its caller
                int name = newReg();
                emitX(OP_LoadK, name, kstr(fd->str));
                emit(OP_SetElem, evalObjReg, name, t);
                f->nextReg--;
            } else
                storeVar(fd->var, fd->str, t, true);
            f->nextReg--;
        }
    }
    int depthOf(Variable* v) {
        int d = 0;
        for (size_t i = chain.size(); i-- > 0; ++d)
            if (chain[i] == v->scope) return d;
        fail("internal: variable '" + utf16to8(v->name->s) + "' is out of scope");
    }
    void storeVar(Variable* v, JSString* name, int src, bool init) {
        if (!v) {
            emitX(OP_SetGlobal, src, kstr(name));
            return;
        }
        if (v->isGlobal) {
            bool lex = v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS;
            if (lex && init) emitX(OP_InitGlobalLet, src, kstr(v->name));
            else if (v->kind == VK_FUNC && init) emitX(ast.evalFn ? OP_DefGlobalFuncD : OP_DefGlobalFunc, src, kstr(v->name));
            else emitX(OP_SetGlobal, src, kstr(v->name));
            return;
        }
        if (v->kind == VK_IMPORT) {  // imports are immutable bindings
            emitX(OP_ThrowConst, 0, kstr(v->name));
            return;
        }
        bool tdzWrite = !init && v->needsTdz && (v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS);
        if (tdzWrite && v->kind == VK_CONST) {
            int t = newReg();
            if (v->captured) emit(OP_GetCtx, t, depthOf(v), v->slot);
            emitX(OP_CheckTDZ, v->captured ? t : v->reg, kstr(v->name));
            f->nextReg--;
        }
        if (!init && v->kind == VK_CONST) {
            emitX(OP_ThrowConst, 0, kstr(v->name));
            return;
        }
        if (!init && v->kind == VK_FUNCNAME) {  // the name of a function expression is an immutable binding: strict code may not write it
            if (f->info->strict) emitX(OP_ThrowConst, 0, kstr(v->name));
            return;
        }
        if (!init && v->kind == VK_PRIVATE) return;
        if (tdzWrite && v->captured) {
            emit(OP_SetCtxTdz, src, depthOf(v), v->slot);
            emitX(OP_Nop, 0, kstr(v->name));
        } else if (v->captured) {
            emit(OP_SetCtx, src, depthOf(v), v->slot);
        } else {
            if (tdzWrite) emitX(OP_CheckTDZ, v->reg, kstr(v->name));
            if (v->reg != src) emit(OP_Move, v->reg, src);
        }
        if (v->argIndex >= 0 && !bindingParams && v->scope->fn && v->scope->fn->mappedVar) {  // keep arguments[i] in step
            Variable* mv = v->scope->fn->mappedVar;
            emit(OP_SyncArg, src, depthOf(mv), mv->slot);
            emitX(OP_Nop, 0, (uint32_t)v->argIndex);
        }
    }

    static bool assignsVar(Node* n, Variable* v) {
        if (!n) return false;
        if (n->type == N::Function) return false;
        if (n->type == N::Assign || n->type == N::Update) {
            if (mentions(n->a, v)) return true;
        }
        if (assignsVar(n->a, v) || assignsVar(n->b, v) || assignsVar(n->c, v) || assignsVar(n->d, v)) return true;
        for (Node* k : n->list)
            if (assignsVar(k, v)) return true;
        return false;
    }
    static bool mentions(Node* n, Variable* v) {
        if (!n) return false;
        if (n->type == N::Ident) return n->var == v;
        if (n->type == N::Function) return false;
        if (mentions(n->a, v) || mentions(n->b, v)) return true;
        for (Node* k : n->list)
            if (mentions(k, v)) return true;
        return false;
    }
    // Evaluates `left`; if it is a plain local that `right` may overwrite, snapshots it first.
    int operand(Node* left, Node* right) {
        int l = expr(left);
        if (left->type == N::Ident && left->var && !left->var->captured && !left->var->isGlobal && l == left->var->reg &&
            right && assignsVar(right, left->var)) {
            int t = newReg();
            emit(OP_Move, t, l);
            return t;
        }
        return l;
    }

    // ---------------------------------------------------------------- expressions
    int expr(Node* n, int dst = -1) {
        uint32_t saved = curLine;
        curLine = n->line;
        int r = expr0(n, dst);
        curLine = saved;
        return r;
    }

    static Op binop(uint8_t p) {
        switch (p) {
            case P_Plus: case P_PlusEq: return OP_Add;
            case P_Minus: case P_MinusEq: return OP_Sub;
            case P_Star: case P_StarEq: return OP_Mul;
            case P_Slash: case P_SlashEq: return OP_Div;
            case P_Percent: case P_PercentEq: return OP_Mod;
            case P_StarStar: case P_StarStarEq: return OP_Exp;
            case P_Amp: case P_AmpEq: return OP_BitAnd;
            case P_Pipe: case P_PipeEq: return OP_BitOr;
            case P_Caret: case P_CaretEq: return OP_BitXor;
            case P_Shl: case P_ShlEq: return OP_Shl;
            case P_Shr: case P_ShrEq: return OP_Shr;
            case P_UShr: case P_UShrEq: return OP_UShr;
            case P_EqEq: return OP_Eq;
            case P_Ne: return OP_Ne;
            case P_SEq: return OP_SEq;
            case P_SNe: return OP_SNe;
            case P_Lt: return OP_Lt;
            case P_Le: return OP_Le;
            case P_Gt: return OP_Gt;
            case P_Ge: return OP_Ge;
            case P_In: return OP_In;
            case P_InstanceOf: return OP_InstanceOf;
        }
        return OP_Nop;
    }

    void bail(int reg) {  // optional chaining: jump out when reg is null/undefined
        if (f->chains.empty()) fail("internal: optional chain");
        f->chains.back().push_back(jump(OP_JmpNullish, reg));
    }

    // Loads the key of an Index/Property node into a register.
    int keyReg(Node* owner, Node* key) {
        if (owner->flags & NF_PRIVATE) return expr(key);
        if (owner->type == N::Property && !(owner->flags & NF_COMPUTED)) {
            int r = newReg();
            emitX(OP_LoadK, r, kstr(key->str));
            return r;
        }
        int k = expr(key);
        if (owner->type == N::Property) {
            int r = newReg();
            emit(OP_ToPropKey, r, k);
            return r;
        }
        return k;
    }

    static bool directOk(Node* v) {
        switch (v->type) {
            case N::Num: case N::BigInt: case N::Str: case N::Null: case N::True: case N::False: case N::Binary: case N::Unary: case N::Call:
            case N::New: case N::Member: case N::Index: case N::Function: case N::Regex: case N::This:
                return true;
            default: return false;
        }
    }

    int superKey(Node* n) {  // n is Member(Super, name) or Index(Super, expr)
        if (n->type == N::Member) {
            int r = newReg();
            emitX(OP_LoadK, r, kstr(n->str));
            return r;
        }
        return expr(n->b);
    }
    int thisReg(Node* superNode) {
        if (superNode->var) {
            int r = newReg();
            emit(OP_GetCtx, r, depthOf(superNode->var), superNode->var->slot);
            if (superNode->var->needsTdz) emitX(OP_CheckTDZ, r, kstr(superNode->var->name));
            return r;
        }
        syncThis();
        if (f->info->isClassCtor && f->info->isDerived) emit(OP_ThisCheck);
        return 1;
    }

    int expr0(Node* n, int dst) {
        auto need = [&] {
            if (dst < 0) dst = newReg();
            return dst;
        };
        switch (n->type) {
            case N::Num: loadNum(need(), n->num); return dst;
            case N::BigInt: emitX(OP_LoadK, need(), konst(Value::cell(bigFromLiteral(vm, n->str->s)))); return dst;
            case N::Str: emitX(OP_LoadK, need(), kstr(n->str)); return dst;
            case N::Null: emit(OP_LoadNull, need()); return dst;
            case N::True: emit(OP_LoadTrue, need()); return dst;
            case N::False: emit(OP_LoadFalse, need()); return dst;
            case N::Hole: emit(OP_LoadUndef, need()); return dst;
            case N::Ident: {
                if (const std::vector<Variable*>* dl = dynOf(n)) return dynLoad(n, *dl, dst);
                Variable* v = n->var;
                if (!v || v->isGlobal) {
                    need();
                    if (!v && n->str == vm.a.undefined) emit(OP_LoadUndef, dst);
                    else emitX(OP_GetGlobal, dst, kstr(n->str));
                    return dst;
                }
                if (v->kind == VK_IMPORT) {  // read the export out of the module it comes from, every time
                    need();
                    Variable* from = v->importNs;
                    emit(OP_GetCtx, dst, depthOf(from), from->slot);
                    if (v->importName) getProp(dst, dst, v->importName);
                    return dst;
                }
                if (v->captured) {
                    need();
                    emit(OP_GetCtx, dst, depthOf(v), v->slot);
                    if (v->needsTdz) emitX(OP_CheckTDZ, dst, kstr(v->name));
                    return dst;
                }
                if (v->reg < 0) fail("internal: unallocated variable '" + utf16to8(v->name->s) + "'");
                if (v->needsTdz && n->pos < v->declEnd) emitX(OP_CheckTDZ, v->reg, kstr(v->name));
                if (dst < 0 || dst == v->reg) return v->reg;
                emit(OP_Move, dst, v->reg);
                return dst;
            }
            case N::This: {
                if (n->var) {
                    need();
                    emit(OP_GetCtx, dst, depthOf(n->var), n->var->slot);
                    if (n->var->needsTdz) emitX(OP_CheckTDZ, dst, kstr(n->var->name));
                    return dst;
                }
                syncThis();
                if (f->info->isClassCtor && f->info->isDerived) emit(OP_ThisCheck);
                if (dst < 0) return 1;
                emit(OP_Move, dst, 1);
                return dst;
            }
            case N::NewTarget:
                need();
                if (n->var) emit(OP_GetCtx, dst, depthOf(n->var), n->var->slot);
                else emit(OP_LoadNewTarget, dst);
                return dst;
            case N::Super: fail("'super' keyword unexpected here");
            case N::Regex: {
                need();
                uint32_t a = kstr(n->str), b = kstr(n->str2);
                if (a > 65535 || b > 65535) fail("Too many constants");
                emit(OP_NewRegExp, dst, (int)a, (int)b);
                return dst;
            }
            case N::Template: {
                need();
                int mark = f->nextReg;
                int acc = newReg();
                emitX(OP_LoadK, acc, kstr(n->strs[0]));
                for (size_t i = 0; i < n->list.size(); ++i) {
                    int m2 = f->nextReg;
                    int r = expr(n->list[i]);
                    emit(OP_StrCat, acc, acc, r);
                    f->nextReg = m2;
                    JSString* s = n->strs[(i + 1) * 2];
                    if (!s->s.empty()) {
                        int t = newReg();
                        emitX(OP_LoadK, t, kstr(s));
                        emit(OP_Add, acc, acc, t);
                        f->nextReg = m2;
                    }
                }
                emit(OP_Move, dst, acc);
                f->nextReg = mark;
                return dst;
            }
            case N::TaggedTemplate: {
                bool tail = tailCall;
                tailCall = false;
                need();
                int mark = f->nextReg;
                int base = newReg();
                newReg();
                calleeInto(n->a, base);
                int strs = newReg();
                int raw = newReg();
                Node* tl = n->b;
                int t = newReg();
                uint32_t site = f->templateSites++;
                if (site > 65535) fail("Too many tagged templates in one function");
                emitX(OP_GetTemplate, strs, site);  // one strings object per site, however often it runs
                size_t made = jump(OP_JmpNotUndef, strs);
                emit(OP_NewArray, strs);
                emit(OP_NewArray, raw);
                for (size_t i = 0; i < tl->strs.size(); i += 2) {
                    if (tl->strs[i]) emitX(OP_LoadK, t, kstr(tl->strs[i]));
                    else emit(OP_LoadUndef, t);  // an escape that means nothing: no cooked text
                    emit(OP_ArrayPush, strs, t);
                    emitX(OP_LoadK, t, kstr(tl->strs[i + 1]));
                    emit(OP_ArrayPush, raw, t);
                }
                emit(OP_SetTemplate, strs, raw, (int)site);
                patch(made);
                f->nextReg = base + 3;  // strs is the first argument
                for (Node* e : tl->list) {
                    int r = newReg();
                    expr(e, r);
                    f->nextReg = r + 1;
                }
                emit(tail ? OP_TailCall : OP_Call, dst, base, 1 + (int)tl->list.size());
                f->nextReg = mark;
                return dst;
            }
            case N::Array: {
                need();
                int mark = f->nextReg;
                int arr = (dst >= mark - 1 || true) ? newReg() : dst;
                emit(OP_NewArray, arr);
                for (Node* e : n->list) {
                    int m2 = f->nextReg;
                    if (e->type == N::Hole) emit(OP_ArrayHole, arr);
                    else if (e->type == N::Spread) emit(OP_ArraySpread, arr, expr(e->a));
                    else emit(OP_ArrayPush, arr, expr(e));
                    f->nextReg = m2;
                }
                emit(OP_Move, dst, arr);
                f->nextReg = mark;
                return dst;
            }
            case N::Object: return objectLit(n, need());
            case N::Function: closure(n->fn, need()); return dst;
            case N::Class: return emitClass(n, need());
            case N::Unary: {
                need();
                int mark = f->nextReg;
                switch (n->op) {
                    case P_Typeof:
                        if (n->a->type == N::Ident && dynOf(n->a)) {
                            dynLoad(n->a, *dynOf(n->a), dst, 1);
                        } else if (n->a->type == N::Ident && (!n->a->var || n->a->var->isGlobal)) {
                            emitX(OP_GetGlobalTypeof, dst, kstr(n->a->str));
                        } else
                            emit(OP_Typeof, dst, expr(n->a));
                        break;
                    case P_Void:
                        expr(n->a);
                        emit(OP_LoadUndef, dst);
                        break;
                    case P_Delete: {
                        // `delete a?.b.c`: when the chain stops at a null or undefined link the result is true
                        Node* t = n->a->type == N::OptChain ? n->a->a : n->a;
                        bool chain = t != n->a && (t->type == N::Member || t->type == N::Index);
                        if (chain) f->chains.emplace_back();
                        if ((t->type == N::Member || t->type == N::Index) && t->a->type == N::Super) {
                            thisReg(t->a);
                            if (t->type == N::Index) expr(t->b);
                            emit(OP_ThrowBadTarget, 1);
                        } else if (t->type == N::Member) {
                            int o = expr(t->a);
                            if (t->flags & NF_OPTIONAL) bail(o);
                            uint32_t k = kstr(t->str);
                            if (k > 65535) fail("Too many constants");
                            emit(OP_DelProp, dst, o, (int)k);
                        } else if (t->type == N::Index) {
                            int o = expr(t->a);
                            if (t->flags & NF_OPTIONAL) bail(o);
                            emit(OP_DelElem, dst, o, expr(t->b));
                        } else if (t->type == N::Ident && dynOf(t)) {
                            dynLoad(t, *dynOf(t), dst, 2);
                        } else if (t->type == N::Ident && !(t->flags & NF_PRIVATE)) {
                            // 13.5.1.2: a declared binding stays; a name left to the global object is a property of it
                            Variable* v = t->var;
                            if (v && (!v->isGlobal || v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS)) emit(OP_LoadFalse, dst);
                            else emitX(OP_DelGlobal, dst, kstr(t->str));
                        } else {
                            expr(n->a);
                            emit(OP_LoadTrue, dst);
                        }
                        if (chain) {
                            size_t e = jump(OP_Jmp);
                            for (size_t j : f->chains.back()) patch(j);
                            f->chains.pop_back();
                            emit(OP_LoadTrue, dst);
                            patch(e);
                        }
                        break;
                    }
                    case P_Bang: emit(OP_Not, dst, expr(n->a)); break;
                    case P_Minus: emit(OP_Neg, dst, expr(n->a)); break;
                    case P_Plus: emit(OP_Plus, dst, expr(n->a)); break;
                    case P_Tilde: emit(OP_BitNot, dst, expr(n->a)); break;
                }
                f->nextReg = mark;
                return dst;
            }
            case N::Update: return update(n, dst, false);
            case N::Binary: {
                need();
                if (n->op == P_In && n->a->type == N::Ident && (n->a->flags & NF_PRIVATE)) {
                    int mark = f->nextReg;
                    int k = expr(n->a);
                    emit(OP_HasPrivate, dst, k, expr(n->b));
                    f->nextReg = mark;
                    return dst;
                }
                int mark = f->nextReg;
                int l = operand(n->a, n->b);
                int r = expr(n->b);
                emit(binop(n->op), dst, l, r);
                f->nextReg = mark;
                return dst;
            }
            case N::Logical: {
                need();
                int mark = f->nextReg;
                int t = newReg();
                expr(n->a, t);
                size_t j = jump(n->op == P_AndAnd ? OP_JmpF : n->op == P_OrOr ? OP_JmpT : OP_JmpNotNullish, t);
                expr(n->b, t);
                patch(j);
                emit(OP_Move, dst, t);
                f->nextReg = mark;
                return dst;
            }
            case N::Cond: {
                need();
                int mark = f->nextReg;
                int t = newReg();
                size_t j = jump(OP_JmpF, expr(n->a));
                expr(n->b, t);
                size_t e = jump(OP_Jmp);
                patch(j);
                expr(n->c, t);
                patch(e);
                emit(OP_Move, dst, t);
                f->nextReg = mark;
                return dst;
            }
            case N::Seq: {
                need();
                for (size_t i = 0; i + 1 < n->list.size(); ++i) effect(n->list[i]);
                int mark = f->nextReg;
                int r = expr(n->list.back());
                if (r != dst) emit(OP_Move, dst, r);
                f->nextReg = mark;
                return dst;
            }
            case N::Assign: return assign(n, dst);
            case N::Member: {
                need();
                int mark = f->nextReg;
                if (n->a->type == N::Super) {
                    int th = thisReg(n->a);
                    int k = superKey(n);
                    emit(OP_GetSuper, dst, k, th);
                } else {
                    int o = expr(n->a);
                    if (n->flags & NF_OPTIONAL) bail(o);
                    getProp(dst, o, n->str);
                }
                f->nextReg = mark;
                return dst;
            }
            case N::Index: {
                need();
                int mark = f->nextReg;
                if (n->a->type == N::Super) {
                    int th = thisReg(n->a);
                    int k = superKey(n);
                    emit(OP_GetSuper, dst, k, th);
                } else {
                    int o = operand(n->a, n->b);
                    if (n->flags & NF_OPTIONAL) bail(o);
                    if (n->b->type == N::Str && !(n->flags & NF_PRIVATE)) getProp(dst, o, n->b->str);
                    else emit((n->flags & NF_PRIVATE) ? OP_GetPrivate : OP_GetElem, dst, o, expr(n->b));
                }
                f->nextReg = mark;
                return dst;
            }
            case N::OptChain: {
                need();
                int mark = f->nextReg;
                int t = newReg();
                f->chains.emplace_back();
                expr(n->a, t);
                size_t e = jump(OP_Jmp);
                for (size_t j : f->chains.back()) patch(j);
                f->chains.pop_back();
                emit(OP_LoadUndef, t);
                patch(e);
                emit(OP_Move, dst, t);
                f->nextReg = mark;
                return dst;
            }
            case N::Call: return call(n, need());
            case N::New: {
                need();
                int mark = f->nextReg;
                int base = newReg();
                newReg();
                expr(n->a, base);
                emit(OP_LoadUndef, base + 1);
                if (args(n, base)) emit(OP_NewSpread, dst, base);
                else emit(OP_New, dst, base, (int)n->list.size());
                f->nextReg = mark;
                return dst;
            }
            case N::ImportCall: {  // import(specifier, options)
                need();
                int mark = f->nextReg;
                int a0 = newReg();
                int a1 = newReg();
                int a2 = newReg();
                expr(n->a, a0);
                if (n->b) expr(n->b, a1);
                else emit(OP_LoadUndef, a1);
                emitX(OP_LoadK, a2, konst(Value::cell(vm.str(utf8to16(file_)))));
                emit(OP_ImportCall, dst, a0);
                f->nextReg = mark;
                return dst;
            }
            case N::Spread: fail("Unexpected spread");
            case N::Yield: {
                if (!f->info->isGenerator) fail("yield outside a generator");
                need();
                int mark = f->nextReg;
                bool isAsync = f->info->isAsync;
                if (n->flags & NF_DELEGATE) {
                    // yield* inner (ECMA-262 15.5.5): next(), throw() and return() on this generator are passed
                    // to the inner iterator until it is done. Registers: it, next, received, mode, result.
                    int it = newReg();
                    newReg();
                    int recv = newReg(), mode = newReg(), r = newReg(), v = newReg(), done = newReg();
                    emit(isAsync ? OP_GetAsyncIter : OP_GetIter, it, expr(n->a));
                    emit(OP_LoadUndef, recv);
                    emitX(OP_LoadInt, mode, 0);
                    size_t top = here();
                    emit(OP_DelegateCall, r, it);
                    if (isAsync) emit(OP_Await, r, r);
                    emit(OP_DelegateResult, v, it, done);
                    size_t out = jump(OP_JmpT, done);
                    if (isAsync) {
                        getProp(v, r, vm.a.value);
                        emit(OP_YieldDelegate, recv, v);
                    } else
                        emit(OP_YieldDelegate, recv, r);
                    patchTo(jump(OP_Jmp), top);
                    patch(out);
                    size_t valuePath = jump(OP_JmpF, mode);
                    // this generator was told to return and the inner iterator has finished: return its value
                    if (!f->fins.empty() || hasOpenIter()) unwind(0, -1, 0);
                    emit(OP_Return, v);
                    patch(valuePath);
                    emit(OP_Move, dst, v);
                } else {
                    int v;
                    if (n->a) v = expr(n->a);
                    else {
                        v = newReg();
                        emit(OP_LoadUndef, v);
                    }
                    if (isAsync) {  // an async generator awaits what it yields
                        int t = newReg();
                        emit(OP_Await, t, v);
                        v = t;
                    }
                    emit(OP_Yield, dst, v);
                }
                f->nextReg = mark;
                return dst;
            }
            case N::Await: {
                need();
                int mark = f->nextReg;
                emit(OP_Await, dst, expr(n->a));
                f->nextReg = mark;
                return dst;
            }
            case N::ArrayPat:
            case N::ObjectPat:
            case N::AssignPat: fail("Invalid destructuring pattern");
            default: fail("internal: unexpected node in expression");
        }
    }

    // Evaluates an expression only for its side effects.
    void effect(Node* n) {
        int mark = f->nextReg;
        if (n->type == N::Update) update(n, -1, true);
        else expr(n);
        f->nextReg = mark;
    }

    // Puts the callee in r[base] and its receiver in r[base+1].
    void calleeInto(Node* c, int base) {
        if (c->type == N::Member || c->type == N::Index) {
            if (c->a->type == N::Super) {
                int m2 = f->nextReg;
                int k = superKey(c);
                int th = thisReg(c->a);
                emit(OP_GetSuper, base, k, th);
                emit(OP_Move, base + 1, th);
                f->nextReg = m2;
                return;
            }
            expr(c->a, base + 1);
            if (c->flags & NF_OPTIONAL) bail(base + 1);
            int m2 = f->nextReg;
            if (c->type == N::Member) getProp(base, base + 1, c->str);
            else if (c->b->type == N::Str && !(c->flags & NF_PRIVATE)) getProp(base, base + 1, c->b->str);
            else emit((c->flags & NF_PRIVATE) ? OP_GetPrivate : OP_GetElem, base, base + 1, expr(c->b));
            f->nextReg = m2;
        } else if (c->type == N::OptChain && (c->a->type == N::Member || c->a->type == N::Index)) {
            f->chains.emplace_back();
            calleeInto(c->a, base);
            size_t done = jump(OP_Jmp);
            for (size_t j : f->chains.back()) patch(j);
            f->chains.pop_back();
            emit(OP_LoadUndef, base);
            emit(OP_LoadUndef, base + 1);
            patch(done);
        } else if (c->type == N::Ident && dynOf(c)) {  // a function found in a `with` object is called on that object
            emit(OP_LoadUndef, base + 1);
            dynLoad(c, *dynOf(c), base, 0, base + 1);
        } else {
            expr(c, base);
            emit(OP_LoadUndef, base + 1);
        }
    }

    // Evaluates call arguments into r[base+2...]. Returns true if spread was used (then r[base+2] is an array).
    bool args(Node* n, int base) {
        bool spread = false;
        for (Node* a : n->list)
            if (a->type == N::Spread) spread = true;
        f->nextReg = base + 2;
        if (!spread) {
            for (Node* a : n->list) {
                int r = newReg();
                expr(a, r);
                f->nextReg = r + 1;
            }
            return false;
        }
        int arr = newReg();
        emit(OP_NewArray, arr);
        for (Node* a : n->list) {
            int m2 = f->nextReg;
            if (a->type == N::Spread) emit(OP_ArraySpread, arr, expr(a->a));
            else emit(OP_ArrayPush, arr, expr(a));
            f->nextReg = m2;
        }
        return true;
    }

    // An expression in tail position (15.10.2 HasCallInTailPosition): the call it ends in, if any, may reuse the frame.
    void tailExpr(Node* n, int dst) {
        switch (n->type) {
            case N::Cond: {
                int mark = f->nextReg;
                size_t j = jump(OP_JmpF, expr(n->a));
                f->nextReg = mark;
                tailExpr(n->b, dst);
                size_t e = jump(OP_Jmp);
                patch(j);
                tailExpr(n->c, dst);
                patch(e);
                return;
            }
            case N::Logical: {
                expr(n->a, dst);
                size_t j = jump(n->op == P_AndAnd ? OP_JmpF : n->op == P_OrOr ? OP_JmpT : OP_JmpNotNullish, dst);
                tailExpr(n->b, dst);
                patch(j);
                return;
            }
            case N::Seq:
                for (size_t i = 0; i + 1 < n->list.size(); ++i) effect(n->list[i]);
                tailExpr(n->list.back(), dst);
                return;
            case N::Call:
            case N::TaggedTemplate:
                tailCall = !(n->flags & NF_OPTIONAL) && n->a->type != N::Super;
                expr(n, dst);
                tailCall = false;
                return;
            default: expr(n, dst);
        }
    }
    int call(Node* n, int dst) {
        bool tail = tailCall;
        tailCall = false;
        int mark = f->nextReg;
        int base = newReg();
        newReg();
        if (n->a->type == N::Super) {
            FunctionInfo* owner = f->info;
            while (owner->isArrow && owner->parent) owner = owner->parent;
            if (!owner->isClassCtor || !owner->isDerived) fail("'super' keyword unexpected here");
            if (owner != f->info) {  // from an arrow function or eval code inside the constructor (13.3.7.1 SuperCall)
                if (!owner->ctorVar || !owner->newTargetVar || !owner->thisVar) fail("'super' keyword unexpected here");
                emit(OP_GetCtx, base, depthOf(owner->ctorVar), owner->ctorVar->slot);
                emit(OP_GetCtx, base + 1, depthOf(owner->newTargetVar), owner->newTargetVar->slot);
                bool spread = args(n, base);
                emit(OP_SuperCallIn, dst, base, spread ? 0xFFFF : (int)n->list.size());
                int cur = newReg();
                emit(OP_GetCtx, cur, depthOf(owner->thisVar), owner->thisVar->slot);
                emit(OP_BindThis, dst, cur);
                storeVar(owner->thisVar, owner->thisVar->name, dst, true);
                emit(OP_InitFields, base, dst);
                f->nextReg = mark;
                return dst;
            }
            if (f->info->thisVar) {  // closures or eval share `this`: it is in its variable before the field initialisers run
                emit(OP_Move, base, 0);
                emit(OP_LoadNewTarget, base + 1);
                bool spread = args(n, base);
                emit(OP_SuperCallIn, dst, base, spread ? 0xFFFF : (int)n->list.size());
                emit(OP_BindThis, dst, 1);
                emit(OP_Move, 1, dst);
                storeVar(f->info->thisVar, f->info->thisVar->name, dst, true);
                emit(OP_InitFields, base, dst);
                f->nextReg = mark;
                return dst;
            }
            if (args(n, base)) emit(OP_SuperCallSpread, dst, base);
            else emit(OP_SuperCall, dst, base, (int)n->list.size());
            f->nextReg = mark;
            return dst;
        }
        calleeInto(n->a, base);
        if (n->flags & NF_OPTIONAL) bail(base);
        bool spread = args(n, base);
        if (spread && (n->flags & NF_EVAL)) {
            emit(OP_DirectEval, dst, base, 0xFFFF);
            emitX(OP_Nop, 0, evalSite((n->flags & NF_INPARAM) != 0));
        } else if (spread)
            emit(OP_CallSpread, dst, base);
        else if (n->flags & NF_EVAL) {
            // eval(...) by name: if the callee turns out to be the real eval, it runs with the scopes described here
            emit(OP_DirectEval, dst, base, (int)n->list.size());
            emitX(OP_Nop, 0, evalSite((n->flags & NF_INPARAM) != 0) | (tail ? 0x80000000u : 0));
        } else
            emit(tail ? OP_TailCall : OP_Call, dst, base, (int)n->list.size());
        f->nextReg = mark;
        return dst;
    }

    // ---- names that may live in a `with` object or among the variables a direct eval created
    const std::vector<Variable*>* dynOf(Node* n) {
        if (!(n->flags & NF_DYN)) return nullptr;
        auto it = ast.dyn.find(n);
        return it == ast.dyn.end() ? nullptr : &it->second;
    }
    void loadScopeObj(Variable* d, int dst) {
        if (d->captured) emit(OP_GetCtx, dst, depthOf(d), d->slot);
        else emit(OP_Move, dst, d->reg);
    }
    // mode 0: read into dst (thisReg >= 0: also the `this` a call gets), 1: typeof, 2: delete
    int dynLoad(Node* n, const std::vector<Variable*>& dl, int dst, int mode = 0, int thisReg = -1) {
        if (dst < 0) dst = newReg();
        int mark = f->nextReg;
        int o = newReg(), flag = newReg(), name = newReg();
        emitX(OP_LoadK, name, kstr(n->str));
        std::vector<size_t> done;
        for (Variable* d : dl) {
            bool with = d->scope->kind == Scope::With;
            loadScopeObj(d, o);
            emit(with ? OP_WithHas : OP_ScopeHas, flag, o, name);
            size_t next = jump(OP_JmpF, flag);
            if (mode == 2) emit(OP_DelElem, dst, o, name);
            else emit(with ? OP_WithGet : OP_GetElem, dst, o, name);
            if (mode == 1) emit(OP_Typeof, dst, dst);
            if (thisReg >= 0 && d->scope->kind == Scope::With) emit(OP_Move, thisReg, o);
            done.push_back(jump(OP_Jmp));
            patch(next);
        }
        n->flags &= ~NF_DYN;  // not found there: the ordinary lookup
        if (mode == 1) {
            if (!n->var || n->var->isGlobal) emitX(OP_GetGlobalTypeof, dst, kstr(n->str));
            else emit(OP_Typeof, dst, expr(n));
        } else if (mode == 2) {
            emit(n->var ? OP_LoadFalse : OP_LoadTrue, dst);
        } else {
            int r = expr(n, dst);
            if (r != dst) emit(OP_Move, dst, r);
        }
        n->flags |= NF_DYN;
        for (size_t j : done) patch(j);
        f->nextReg = mark;
        return dst;
    }
    // A name that may live in a `with` object or among eval-made variables is looked up once, before the right-hand
    // side of an assignment runs (13.15.2: the reference is evaluated first). The register gets the object that holds
    // the name at that moment, or undefined when the name is an ordinary variable.
    int resolveDyn(Node* id) {
        const std::vector<Variable*>* dl = dynOf(id);
        int base = newReg();
        int o = newReg(), flag = newReg(), name = newReg();
        emit(OP_LoadUndef, base);
        emitX(OP_LoadK, name, kstr(id->str));
        std::vector<size_t> done;
        for (Variable* d : *dl) {
            loadScopeObj(d, o);
            emit(d->scope->kind == Scope::With ? OP_WithHas : OP_ScopeHas, flag, o, name);
            size_t next = jump(OP_JmpF, flag);
            emit(OP_Move, base, o);
            done.push_back(jump(OP_Jmp));
            patch(next);
        }
        for (size_t j : done) patch(j);
        f->nextReg = base + 1;
        return base;
    }
    void loadResolved(Node* id, int base, int dst) {
        int mark = f->nextReg;
        size_t dynamic = jump(OP_JmpNotUndef, base);
        id->flags &= ~NF_DYN;
        expr(id, dst);
        id->flags |= NF_DYN;
        size_t end = jump(OP_Jmp);
        patch(dynamic);
        int name = newReg();
        emitX(OP_LoadK, name, kstr(id->str));
        emit(OP_WithGet, dst, base, name);
        patch(end);
        f->nextReg = mark;
    }
    void storeResolved(Node* id, int base, int src) {
        int mark = f->nextReg;
        size_t dynamic = jump(OP_JmpNotUndef, base);
        storeVar(id->var, id->str, src, false);
        size_t end = jump(OP_Jmp);
        patch(dynamic);
        int name = newReg();
        emitX(OP_LoadK, name, kstr(id->str));
        {
            int still = newReg();
            emit(OP_ScopeHas, still, base, name);
            if (f->info->strict) {
                size_t ok = jump(OP_JmpT, still);
                emitX(OP_ThrowUnresolved, 0, kstr(id->str));
                patch(ok);
            }
        }
        emit(OP_SetElem, base, name, src);
        patch(end);
        f->nextReg = mark;
    }
    void storeIdent(Node* id, int src, bool init) {
        const std::vector<Variable*>* dl = dynOf(id);
        if (!dl) {
            storeVar(id->var, id->str, src, init);
            return;
        }
        int mark = f->nextReg;
        int o = newReg(), flag = newReg(), name = newReg();
        emitX(OP_LoadK, name, kstr(id->str));
        std::vector<size_t> done;
        for (Variable* d : *dl) {
            loadScopeObj(d, o);
            emit(d->scope->kind == Scope::With ? OP_WithHas : OP_ScopeHas, flag, o, name);
            size_t next = jump(OP_JmpF, flag);
            emit(OP_SetElem, o, name, src);
            done.push_back(jump(OP_Jmp));
            patch(next);
        }
        storeVar(id->var, id->str, src, init);
        for (size_t j : done) patch(j);
        f->nextReg = mark;
    }

    // Records what a direct eval at this point can see: every scope with a context, outermost first.
    uint32_t evalSite(bool inParams) {
        EvalSite site;
        site.strict = f->info->strict;
        site.inParams = inParams;
        for (Scope* sc : chain) {
            EvalScope es;
            es.isFunction = sc->kind == Scope::Function;
            es.isGlobal = sc->kind == Scope::Global;
            es.isClass = sc->kind == Scope::Class;
            es.isArrow = es.isFunction && sc->fn->isArrow;
            if (es.isFunction) {
                es.isMethod = sc->fn->isMethod;
                es.isFieldInit = sc->fn->isFieldInit;
                es.isClassCtor = sc->fn->isClassCtor;
                es.isDerived = sc->fn->isDerived;
            }
            es.isWith = sc->kind == Scope::With;
            es.isVarScope = sc->varScope;
            for (Variable* v : sc->vars) {
                if (v->isGlobal || !v->captured || v->kind == VK_IMPORT) continue;
                bool owner = es.isFunction || es.isGlobal;
                if (owner && v == sc->fn->thisVar) es.thisVar = (int)es.vars.size();
                if (owner && v == sc->fn->argsVar) es.argsVar = (int)es.vars.size();
                if (owner && v == sc->fn->newTargetVar) es.newTargetVar = (int)es.vars.size();
                if (owner && v == sc->fn->ctorVar) es.ctorVar = (int)es.vars.size();
                if (es.isFunction && v == sc->fn->evalVars) es.evalVars = (int)es.vars.size();
                if (es.isWith && v == sc->withVar) es.withVar = (int)es.vars.size();
                if (es.isFunction && v == sc->fn->mappedVar) es.mappedVar = (int)es.vars.size();
                es.vars.push_back({v->name, (uint16_t)v->slot, v->kind, v->needsTdz, (int16_t)v->argIndex});
            }
            site.scopes.push_back(std::move(es));
        }
        f->p->evalSites.push_back(std::move(site));
        return (uint32_t)f->p->evalSites.size() - 1;
    }

    int update(Node* n, int dst, bool discard) {
        Op op = n->op == P_Inc ? OP_Inc : OP_Dec;
        bool prefix = (n->flags & NF_PREFIX) || discard;
        Node* t = n->a;
        if (t->type == N::Call) {
            if (dst < 0) dst = newReg();
            int mark = f->nextReg;
            expr(t);
            emit(OP_ThrowBadTarget);
            f->nextReg = mark;
            return dst;
        }
        if (t->type == N::Ident && t->var && !t->var->captured && !t->var->isGlobal && t->var->kind != VK_CONST && !(t->flags & NF_DYN)) {
            int r = t->var->reg;
            if (prefix) {
                emit(op, r, r);
                if (discard || dst < 0 || dst == r) return r;
                emit(OP_Move, dst, r);
                return dst;
            }
            if (dst < 0) dst = newReg();
            emit(OP_ToNumeric, dst, r);
            emit(op, r, dst);
            return dst;
        }
        if (dst < 0) dst = newReg();
        int mark = f->nextReg;
        int oldv = newReg(), newv = newReg();
        if ((t->type == N::Member || t->type == N::Index) && t->a->type == N::Super) {
            int th = thisReg(t->a);
            int k = newReg();
            if (t->type == N::Member) emitX(OP_LoadK, k, kstr(t->str));
            else emit(OP_ToPropKey, k, expr(t->b));
            emit(OP_GetSuper, oldv, k, th);
            emit(OP_ToNumeric, oldv, oldv);
            emit(op, newv, oldv);
            emit(OP_SetSuper, k, newv, th);
        } else if (t->type == N::Ident && dynOf(t)) {
            int base = resolveDyn(t);
            loadResolved(t, base, oldv);
            emit(OP_ToNumeric, oldv, oldv);
            emit(op, newv, oldv);
            storeResolved(t, base, newv);
        } else if (t->type == N::Ident) {
            int cur = expr(t);
            emit(OP_ToNumeric, oldv, cur);
            emit(op, newv, oldv);
            storeIdent(t, newv, false);
        } else if (t->type == N::Member) {
            int o = expr(t->a);
            getProp(oldv, o, t->str);
            emit(OP_ToNumeric, oldv, oldv);
            emit(op, newv, oldv);
            setProp(o, t->str, newv);
        } else {
            int o = expr(t->a);
            int k = newReg();
            int kv = expr(t->b);
            if (!(t->flags & NF_PRIVATE)) emit(OP_RequireObj, o);
            emit(OP_ToPropKey, k, kv);
            emit((t->flags & NF_PRIVATE) ? OP_GetPrivate : OP_GetElem, oldv, o, k);
            emit(OP_ToNumeric, oldv, oldv);
            emit(op, newv, oldv);
            emit((t->flags & NF_PRIVATE) ? OP_SetPrivate : OP_SetElem, o, k, newv);
        }
        emit(OP_Move, dst, prefix ? newv : oldv);
        f->nextReg = mark;
        return dst;
    }

    int assign(Node* n, int dst) {
        Node* t = n->a;
        if (t->type == N::Call) {
            if (dst < 0) dst = newReg();
            int mark = f->nextReg;
            expr(t);
            emit(OP_ThrowBadTarget);
            f->nextReg = mark;
            return dst;
        }
        if (n->op == P_Assign) {
            if (t->type == N::Ident) {
                Variable* v = t->var;
                if (v && !v->captured && !v->isGlobal && v->kind != VK_CONST && v->kind != VK_FUNCNAME && v->kind != VK_PRIVATE &&
                    !(v->needsTdz && (v->kind == VK_LET || v->kind == VK_CLASS)) && !(t->flags & NF_DYN) && directOk(n->b) && !mentions(n->b, v)) {
                    expr(n->b, v->reg);
                    if (dst < 0 || dst == v->reg) return v->reg;
                    emit(OP_Move, dst, v->reg);
                    return dst;
                }
                if (dst < 0) dst = newReg();
                int mark = f->nextReg;
                if (dynOf(t)) {  // the reference is resolved before the right-hand side runs
                    int base = resolveDyn(t);
                    int r = expr(n->b);
                    storeResolved(t, base, r);
                    if (r != dst) emit(OP_Move, dst, r);
                    f->nextReg = mark;
                    return dst;
                }
                int known = -1;
                if (!v && f->info->strict) {  // strict code: the name is resolved before the value is computed (13.15.2, 6.2.5.6)
                    known = newReg();
                    emitX(OP_HasGlobal, known, kstr(t->str));
                }
                int r = expr(n->b);
                if (known >= 0) {
                    size_t ok = jump(OP_JmpT, known);
                    emitX(OP_ThrowUnresolved, 0, kstr(t->str));
                    patch(ok);
                }
                storeIdent(t, r, false);
                if (r != dst) emit(OP_Move, dst, r);
                f->nextReg = mark;
                return dst;
            }
            if (dst < 0) dst = newReg();
            int mark = f->nextReg;
            if (t->type == N::Member) {
                if (t->a->type == N::Super) {
                    int th = thisReg(t->a);
                    int k = superKey(t);
                    expr(n->b, dst);
                    emit(OP_SetSuper, k, dst, th);
                } else {
                    int o = operand(t->a, n->b);
                    expr(n->b, dst);
                    setProp(o, t->str, dst);
                }
            } else if (t->type == N::Index && t->a->type == N::Super) {
                int th = thisReg(t->a);
                int k = newReg();
                expr(t->b, k);
                expr(n->b, dst);
                emit(OP_SetSuper, k, dst, th);
            } else if (t->type == N::Index) {
                int o = operand(t->a, n->b);
                int k = expr(t->b);
                if (t->b->type != N::Num && t->b->type != N::Str && !(t->flags & NF_PRIVATE)) {
                    int kk = newReg();
                    emit(OP_Move, kk, k);
                    k = kk;
                }
                expr(n->b, dst);
                emit((t->flags & NF_PRIVATE) ? OP_SetPrivate : OP_SetElem, o, k, dst);
            } else if (t->type == N::Ident && dynOf(t)) {
                int base = resolveDyn(t);
                expr(n->b, dst);
                storeResolved(t, base, dst);
            } else {
                expr(n->b, dst);
                bindTarget(t, dst, false);
            }
            f->nextReg = mark;
            return dst;
        }
        // compound assignment
        if (dst < 0) dst = newReg();
        int mark = f->nextReg;
        bool logical = n->op == P_AndAndEq || n->op == P_OrOrEq || n->op == P_QQEq;
        Op jop = n->op == P_AndAndEq ? OP_JmpF : n->op == P_OrOrEq ? OP_JmpT : OP_JmpNotNullish;
        int cur = newReg();
        int o = -1, k = -1, dynBase = -1, superThis = -1;
        if ((t->type == N::Member || t->type == N::Index) && t->a->type == N::Super) {
            superThis = thisReg(t->a);
            k = newReg();
            if (t->type == N::Member) emitX(OP_LoadK, k, kstr(t->str));
            else emit(OP_ToPropKey, k, expr(t->b));
            emit(OP_GetSuper, cur, k, superThis);
        } else if (t->type == N::Ident && dynOf(t)) {
            dynBase = resolveDyn(t);
            loadResolved(t, dynBase, cur);
        } else if (t->type == N::Ident) {
            int r = expr(t);
            if (r != cur) emit(OP_Move, cur, r);
        } else if (t->type == N::Member) {
            o = expr(t->a);
            getProp(cur, o, t->str);
        } else {
            o = expr(t->a);
            k = newReg();
            int kv = expr(t->b);
            if (!(t->flags & NF_PRIVATE)) emit(OP_RequireObj, o);
            emit(OP_ToPropKey, k, kv);
            emit((t->flags & NF_PRIVATE) ? OP_GetPrivate : OP_GetElem, cur, o, k);
        }
        size_t skip = 0;
        if (logical) {
            skip = jump(jop, cur);
            expr(n->b, cur);
        } else {
            int r = expr(n->b);
            emit(binop(n->op), cur, cur, r);
        }
        if (superThis >= 0) emit(OP_SetSuper, k, cur, superThis);
        else if (dynBase >= 0) storeResolved(t, dynBase, cur);
        else if (t->type == N::Ident) storeIdent(t, cur, false);
        else if (t->type == N::Member) setProp(o, t->str, cur);
        else emit((t->flags & NF_PRIVATE) ? OP_SetPrivate : OP_SetElem, o, k, cur);
        if (logical) patch(skip);
        emit(OP_Move, dst, cur);
        f->nextReg = mark;
        return dst;
    }

    int objectLit(Node* n, int dst) {
        int mark = f->nextReg;
        int obj = newReg();
        emit(OP_NewObject, obj);
        for (Node* p : n->list) {
            int m2 = f->nextReg;
            curLine = p->line;
            if (p->op == PK_SPREAD) {
                emit(OP_ObjSpread, obj, expr(p->a));
            } else if (p->op == PK_INIT) {
                if (!(p->flags & NF_COMPUTED)) {
                    if (p->a->str == vm.a.proto && !(p->flags & NF_SHORTHAND)) {
                        emit(OP_SetProto, obj, expr(p->b));
                    } else {
                        int v = expr(p->b);
                        uint32_t k = kstr(p->a->str);
                        if (k < 65536 && !indexLike(p->a->str)) {
                            emit(OP_DefField, obj, (int)k, v);
                        } else {
                            int kr = newReg();
                            emitX(OP_LoadK, kr, k);
                            emit(OP_DefElem, obj, kr, v);
                        }
                    }
                } else {
                    int kr = keyReg(p, p->a);
                    bool anonymous = (p->b->type == N::Function && !p->b->fn->name) || (p->b->type == N::Class && !p->b->str && !p->b->str2);
                    int v = expr(p->b);
                    if (anonymous) emit(OP_SetFnName, v, kr, 0);
                    emit(OP_DefElem, obj, kr, v);
                }
            } else {
                int kr = keyReg(p, p->a);
                int fr = newReg();
                closure(p->b->fn, fr);
                if (p->flags & NF_COMPUTED) emit(OP_SetFnName, fr, kr, p->op == PK_GET ? 1 : p->op == PK_SET ? 2 : 0);
                emit(p->op == PK_GET ? OP_DefGetterE : p->op == PK_SET ? OP_DefSetterE : OP_DefMethodE, obj, kr, fr);
            }
            f->nextReg = m2;
        }
        emit(OP_Move, dst, obj);
        f->nextReg = mark;
        return dst;
    }

    FuncProto* defaultCtor(bool derived, JSString* name) {
        FuncProto* p = vm.alloc<FuncProto>();
        p->name = name;
        p->isClassCtor = true;
        p->isDerived = derived;
        p->isStrict = true;
        p->file = file_;
        auto add = [&](Op op, int a, int b, int c) {
            Ins i;
            i.op = op;
            i.a = (uint16_t)a;
            i.b = (uint16_t)b;
            i.c = (uint16_t)c;
            p->code.push_back(i);
            p->lines.push_back(curLine);
        };
        if (derived) {
            p->needsArgs = true;
            p->nregs = 7;
            add(OP_CreateRest, 4, 0, 0);
            add(OP_SuperCallSpread, 5, 2, 0);
        } else
            p->nregs = 3;
        add(OP_RetUndef, 0, 0, 0);
        return p;
    }

    int emitClass(Node* n, int dst) {
        int mark = f->nextReg;
        ScopeMark sm = enterScope(n->scope);
        int parent = -1;
        if (n->a) {
            parent = newReg();
            expr(n->a, parent);
        }
        for (Variable* v : n->scope->vars)
            if (v->kind == VK_PRIVATE) {
                int t = newReg();
                emitX(OP_NewPrivate, t, kstr(v->name));
                storeVar(v, v->name, t, true);
                f->nextReg--;
            }
        JSString* name = n->str ? n->str : n->str2;
        int rc = newReg(), rp = newReg();
        FuncProto* ctor;
        if (n->c) {
            if (!n->c->fn->name) n->c->fn->name = name;
            ctor = compileFunction(n->c->fn);
        } else
            ctor = defaultCtor(n->a != nullptr, name);
        ctor->name = name;
        ctor->source = ast.src;
        ctor->srcStart = n->pos;
        ctor->srcEnd = (uint32_t)n->num;
        f->p->inner.push_back(ctor);
        emitX(OP_Closure, rc, (uint32_t)f->p->inner.size() - 1);
        emit(OP_MakeClass, rc, parent < 0 ? 0xFFFF : parent, rp);
        // ECMA-262 ClassDefinitionEvaluation: methods are defined and every computed key is evaluated in source
        // order; static fields and static blocks run afterwards, once the class's own name is bound.
        struct Late {
            Node* m;
            int key;
        };
        std::vector<Late> late;
        for (Node* m : n->list) {
            if (m->op == 255) continue;
            curLine = m->line;
            if (m->op == PK_STATIC_BLOCK) {
                late.push_back({m, -1});
                continue;
            }
            if (m->op == PK_FIELD && (m->flags & NF_STATIC)) {
                int keep = newReg();  // holds the key until the field is defined
                int kr = keyReg(m, m->a);
                if (kr != keep) emit(OP_Move, keep, kr);
                f->nextReg = keep + 1;
                late.push_back({m, keep});
                continue;
            }
            int m2 = f->nextReg;
            int target = (m->flags & NF_STATIC) ? rc : rp;
            int kr = keyReg(m, m->a);
            if (m->op == PK_FIELD) {
                int fr = newReg();
                if (m->b) closure(m->b->fn, fr);
                else emit(OP_LoadUndef, fr);
                emit(OP_AddField, rc, kr, fr);
            } else if (m->flags & NF_PRIVATE) {
                int fr = newReg();
                closure(m->b->fn, fr);
                emit(OP_PrivMethod, rc, kr, fr);
                emitX(OP_Nop, 0, (uint32_t)((m->op == PK_GET ? 1 : m->op == PK_SET ? 2 : 0) | ((m->flags & NF_STATIC) ? 4 : 0)));
            } else {
                int fr = newReg();
                closure(m->b->fn, fr);
                if (m->flags & NF_COMPUTED) emit(OP_SetFnName, fr, kr, m->op == PK_GET ? 1 : m->op == PK_SET ? 2 : 0);
                emit(m->op == PK_GET ? OP_DefGetter : m->op == PK_SET ? OP_DefSetter : OP_DefMethod, target, kr, fr);
            }
            f->nextReg = m2;
        }
        if (n->var) storeVar(n->var, n->var->name, rc, true);
        for (auto& l : late) {
            int m2 = f->nextReg;
            curLine = l.m->line;
            int fr = newReg();
            if (l.m->op == PK_STATIC_BLOCK) {
                closure(l.m->b->fn, fr);
                emit(OP_ClassInit, rc, fr);
            } else {
                if (l.m->b) closure(l.m->b->fn, fr);
                else emit(OP_LoadUndef, fr);
                emit(OP_DefStaticField, rc, l.key, fr);
            }
            f->nextReg = m2;
        }
        emit(OP_Move, dst, rc);
        exitScope(sm);
        f->nextReg = mark;
        return dst;
    }

    // ---------------------------------------------------------------- destructuring
    // In a destructuring assignment a target that is a property access is worked out (its object and key expressions
    // run) before the value for it is fetched (13.15.5.5, 13.15.5.6).
    struct Ref {
        Node* t = nullptr;
        int o = -1, k = -1;
    };
    Ref prepareRef(Node* e, bool init) {
        Ref r;
        if (init) return r;
        Node* t = e->type == N::AssignPat ? e->a : e;
        if ((t->type != N::Member && t->type != N::Index) || t->a->type == N::Super) return r;
        r.t = t;
        r.o = newReg();
        expr(t->a, r.o);
        if (t->type == N::Index) {
            r.k = newReg();
            expr(t->b, r.k);
        }
        return r;
    }
    void storeRef(const Ref& r, Node* e, int src) {  // e may carry a default value
        int mark = f->nextReg;
        int v = src;
        if (e->type == N::AssignPat) {
            v = newReg();
            emit(OP_Move, v, src);
            size_t j = jump(OP_JmpNotUndef, v);
            expr(e->b, v);
            patch(j);
        }
        if (r.t->type == N::Member) setProp(r.o, r.t->str, v);
        else emit((r.t->flags & NF_PRIVATE) ? OP_SetPrivate : OP_SetElem, r.o, r.k, v);
        f->nextReg = mark;
    }
    void bindTarget(Node* t, int src, bool init) {
        int mark = f->nextReg;
        switch (t->type) {
            case N::Ident: storeIdent(t, src, init); break;
            case N::Call:
                expr(t);
                emit(OP_ThrowBadTarget);
                break;
            case N::Member: {
                int o = expr(t->a);
                setProp(o, t->str, src);
                break;
            }
            case N::Index: {
                int o = expr(t->a);
                emit((t->flags & NF_PRIVATE) ? OP_SetPrivate : OP_SetElem, o, expr(t->b), src);
                break;
            }
            case N::AssignPat: {
                int tmp = newReg();
                emit(OP_Move, tmp, src);
                size_t j = jump(OP_JmpNotUndef, tmp);
                expr(t->b, tmp);
                patch(j);
                bindTarget(t->a, tmp, init);
                break;
            }
            case N::ArrayPat: {
                int it = newReg();
                newReg();
                emit(OP_GetIter, it, src);
                int v = newReg(), done = newReg();
                size_t start = here();
                for (Node* e : t->list) {
                    int m3 = f->nextReg;
                    if (e->type == N::Rest) {
                        Ref ref = prepareRef(e->a, init);
                        int arr = newReg();
                        emit(OP_NewArray, arr);
                        size_t top = here();
                        emit(OP_IterNext, v, it, done);
                        size_t out = jump(OP_JmpT, done);
                        emit(OP_ArrayPush, arr, v);
                        patchTo(jump(OP_Jmp), top);
                        patch(out);
                        if (ref.t) storeRef(ref, e->a, arr);
                        else bindTarget(e->a, arr, init);
                    } else if (e->type == N::Hole) {
                        emit(OP_IterNext, v, it, done);
                    } else {
                        Ref ref = prepareRef(e, init);
                        emit(OP_IterNext, v, it, done);
                        if (ref.t) storeRef(ref, e, v);
                        else bindTarget(e, v, init);
                    }
                    f->nextReg = m3;
                }
                // ECMA-262 8.6.2: an iterator that was not used up is closed, also when binding throws
                size_t end = here();
                emit(OP_IterClose, it);
                size_t over = jump(OP_Jmp);
                size_t handler = here();
                int e = newReg();
                emit(OP_Catch, e);
                emit(OP_IterCloseAbrupt, it, e);
                emit(OP_Throw, e);
                patch(over);
                f->p->handlers.push_back({(uint32_t)start, (uint32_t)end, (uint32_t)handler, (uint16_t)f->ctxDepth, 1});
                break;
            }
            case N::ObjectPat: {
                emit(OP_CheckObj, src);
                bool hasRest = false;
                for (Node* p : t->list)
                    if (p->type == N::Rest) hasRest = true;
                int excluded = -1;
                if (hasRest) {
                    excluded = newReg();
                    emit(OP_NewArray, excluded);
                }
                int v = newReg();
                for (Node* p : t->list) {
                    int m2 = f->nextReg;
                    if (p->type == N::Rest) {
                        Ref ref = prepareRef(p->a, init);
                        emit(OP_ObjRest, v, src, excluded);
                        if (ref.t) storeRef(ref, p->a, v);
                        else bindTarget(p->a, v, init);
                    } else {
                        Ref ref;
                        if (!(p->flags & NF_COMPUTED)) {
                            ref = prepareRef(p->b, init);
                            getProp(v, src, p->a->str);
                            if (hasRest) {
                                int kr = newReg();
                                emitX(OP_LoadK, kr, kstr(p->a->str));
                                emit(OP_ArrayPush, excluded, kr);
                            }
                        } else {
                            int kr = newReg();
                            emit(OP_ToPropKey, kr, expr(p->a));
                            ref = prepareRef(p->b, init);
                            emit(OP_GetElem, v, src, kr);
                            if (hasRest) emit(OP_ArrayPush, excluded, kr);
                        }
                        if (ref.t) storeRef(ref, p->b, v);
                        else bindTarget(p->b, v, init);
                    }
                    f->nextReg = m2;
                }
                break;
            }
            default: fail("Invalid assignment target");
        }
        f->nextReg = mark;
    }

    // ---------------------------------------------------------------- statements
    Loop& pushLoop(bool isLoop, bool isSwitch = false) {
        Loop L;
        L.labels = std::move(f->pendingLabels);
        f->pendingLabels.clear();
        L.isLoop = isLoop;
        L.isSwitch = isSwitch;
        L.breakDepth = L.contDepth = f->ctxDepth;
        L.finMark = f->fins.size();
        f->loops.push_back(std::move(L));
        return f->loops.back();
    }
    void patchConts() {
        for (size_t j : f->loops.back().conts) patch(j);
        f->loops.back().conts.clear();
    }
    void popLoop() {
        for (size_t j : f->loops.back().breaks) patch(j);
        f->loops.pop_back();
    }

    // Closes a for-of iterator that is being left early. `quiet` is the exception path: whatever return() does is
    // ignored and the exception carries on. An async iterator's return() is awaited (ECMA-262 7.4.13).
    void emitIterClose(int state, bool isAsync, bool quiet) {
        if (!isAsync) {
            emit(quiet ? OP_IterCloseQuiet : OP_IterClose, state);
            return;
        }
        int mark = f->nextReg;
        int r = newReg(), flag = newReg();
        emit(quiet ? OP_IterReturnCallQuiet : OP_IterReturnCall, r, state, flag);
        size_t skip = jump(OP_JmpF, flag);
        if (quiet) {
            size_t start = here();
            emit(OP_Await, r, r);
            size_t end = here();
            size_t over = jump(OP_Jmp);
            size_t handler = here();
            int e = newReg();
            emit(OP_Catch, e);  // a rejected return() is dropped: the exception already under way wins
            patch(over);
            f->p->handlers.push_back({(uint32_t)start, (uint32_t)end, (uint32_t)handler, (uint16_t)f->ctxDepth, 0});
        } else {
            emit(OP_Await, r, r);
            emit(OP_CheckIterResult, r);
        }
        patch(skip);
        f->nextReg = mark;
    }

    // DisposeResources (9.5.6) for the list in `list`. errIn: the register of the error the scope is being left with, or -1.
    void disposeCode(int list, int errIn, bool isAsync) {
        int mark = f->nextReg;
        int err = newReg(), tmp = newReg();
        if (errIn >= 0) emit(OP_Move, err, errIn);
        else emit(OP_LoadEmpty, err);
        if (!isAsync) {
            emit(OP_DisposeNext, tmp, list, err);
        } else {  // every `await using` resource is awaited before the next one is disposed
            size_t top = here();
            emit(OP_DisposeNext, tmp, list, err);
            size_t done = jump(OP_JmpEmpty, tmp);
            size_t tryStart = here();
            emit(OP_Await, tmp, tmp);
            size_t tryEnd = here();
            size_t skip = jump(OP_Jmp);
            size_t handler = here();
            int e2 = newReg();
            emit(OP_Catch, e2);
            emit(OP_DisposeMerge, err, e2);
            f->nextReg--;
            patch(skip);
            patchTo(jump(OP_Jmp), top);
            patch(done);
            f->p->handlers.push_back({(uint32_t)tryStart, (uint32_t)tryEnd, (uint32_t)handler, (uint16_t)f->ctxDepth, 0});
        }
        emit(OP_ThrowIfSet, err);
        f->nextReg = mark;
    }
    // Runs `body` as the contents of a scope with `using` declarations: however it is left, its resources are disposed.
    template <class Body>
    void withDisposal(int usingKind, Body body) {
        int list = newReg();
        emit(OP_NewArray, list);
        int depth = f->ctxDepth;
        Fin fin{nullptr, depth, f->loops.size()};
        fin.disposeReg = list;
        fin.disposeAsync = usingKind == 2;
        f->fins.push_back(fin);
        int saved = f->disposeReg;
        f->disposeReg = list;
        size_t start = here();
        body();
        size_t end = here();
        f->disposeReg = saved;
        f->fins.pop_back();
        disposeCode(list, -1, usingKind == 2);
        size_t after = jump(OP_Jmp);
        size_t handler = here();
        int e = newReg();
        emit(OP_Catch, e);
        disposeCode(list, e, usingKind == 2);
        emit(OP_Throw, e);
        f->nextReg--;
        patch(after);
        f->p->handlers.push_back({(uint32_t)start, (uint32_t)end, (uint32_t)handler, (uint16_t)depth, 1});
    }

    // Emits the code needed before a jump that leaves try/finally blocks, loops and scopes.
    void unwind(size_t finMark, int targetDepth, size_t loopFrom) {
        for (size_t i = f->loops.size(); i-- > loopFrom;)
            if (f->loops[i].iterReg >= 0) emitIterClose(f->loops[i].iterReg, f->loops[i].asyncIter, false);
        int depth = f->ctxDepth;
        for (size_t i = f->fins.size(); i-- > finMark;) {
            Fin fin = f->fins[i];
            if (depth > fin.ctxDepth) {
                emitX(OP_PopCtx, 0, (uint32_t)(depth - fin.ctxDepth));
                depth = fin.ctxDepth;
            }
            // compile the finally block in the environment of its try statement
            std::vector<Fin> savedFins(f->fins.begin() + i, f->fins.end());
            f->fins.resize(i);
            std::vector<Loop> savedLoops;
            for (size_t k = fin.loopMark; k < f->loops.size(); ++k) savedLoops.push_back(std::move(f->loops[k]));
            f->loops.resize(std::min(fin.loopMark, f->loops.size()));
            size_t chainLen = f->chainBase + fin.ctxDepth;
            std::vector<Scope*> savedChain(chain.begin() + chainLen, chain.end());
            chain.resize(chainLen);
            int savedDepth = f->ctxDepth;
            f->ctxDepth = fin.ctxDepth;
            size_t inlineStart = here();
            if (fin.block) quiet(fin.block);
            else disposeCode(fin.disposeReg, -1, fin.disposeAsync);
            f->unwound.push_back({inlineStart, here(), i});
            f->ctxDepth = savedDepth;
            chain.insert(chain.end(), savedChain.begin(), savedChain.end());
            for (auto& L : savedLoops) f->loops.push_back(std::move(L));
            for (auto& x : savedFins) f->fins.push_back(x);
        }
        if (targetDepth >= 0 && depth > targetDepth) emitX(OP_PopCtx, 0, (uint32_t)(depth - targetDepth));
    }

    void jumpOut(Node* n) {
        bool isBreak = n->type == N::Break;
        int idx = -1;
        for (int i = (int)f->loops.size() - 1; i >= 0; --i) {
            Loop& L = f->loops[i];
            if (n->str) {
                bool has = false;
                for (JSString* l : L.labels)
                    if (l == n->str) has = true;
                if (has) {
                    idx = i;
                    break;
                }
            } else if (isBreak ? (L.isLoop || L.isSwitch) : L.isLoop) {
                idx = i;
                break;
            }
        }
        if (idx < 0 || (!isBreak && !f->loops[idx].isLoop)) fail(isBreak ? "Illegal break statement" : "Illegal continue statement");
        size_t finMark = f->loops[idx].finMark;
        int depth = isBreak ? f->loops[idx].breakDepth : f->loops[idx].contDepth;
        unwind(finMark, depth, isBreak ? (size_t)idx : (size_t)idx + 1);
        size_t j = jump(OP_Jmp);
        (isBreak ? f->loops[idx].breaks : f->loops[idx].conts).push_back(j);
    }

    void varDecl(Node* n) {
        for (Node* d : n->list) {
            int mark = f->nextReg;
            curLine = d->line;
            Node* t = d->a;
            if ((n->flags & NF_USING) && d->b) {
                if (f->disposeReg < 0) fail("internal: using declaration outside a disposal scope");
                int r = newReg();
                expr(d->b, r);
                emit(OP_AddDisposable, f->disposeReg, r, (n->flags & NF_AWAIT) ? 1 : 0);
                storeIdent(t, r, true);
                f->nextReg = mark;
                continue;
            }
            if (d->b && t->type == N::Ident && dynOf(t) && n->op == VK_VAR) {
                int base = resolveDyn(t);
                storeResolved(t, base, expr(d->b));
                f->nextReg = mark;
                continue;
            }
            if (d->b) {
                if (t->type == N::Ident) {
                    Variable* v = t->var;
                    if (v && !v->captured && !v->isGlobal && !(t->flags & NF_DYN) && directOk(d->b) && !mentions(d->b, v)) expr(d->b, v->reg);
                    else storeIdent(t, expr(d->b), true);
                } else {
                    bindTarget(t, expr(d->b), true);
                }
            } else if (n->op != VK_VAR && t->type == N::Ident) {
                int r = newReg();
                emit(OP_LoadUndef, r);
                storeVar(t->var, t->str, r, true);
            }
            f->nextReg = mark;
        }
    }

    void forInOf(Node* n) {
        bool isOf = n->type == N::ForOf;
        bool isAwait = isOf && (n->flags & NF_AWAIT);
        ScopeMark sm = enterScope(n->scope);
        int mark = f->nextReg;
        if (n->a->type == N::VarDecl && n->a->list[0]->b) {  // B.3.5: the initializer runs once, before the loop
            storeIdent(n->a->list[0]->a, expr(n->a->list[0]->b), true);
            f->nextReg = mark;
        }
        int src = expr(n->b);
        int state = newReg();
        newReg();
        emit(isAwait ? OP_GetAsyncIter : isOf ? OP_GetIter : OP_ForInPrep, state, src);
        int v = newReg(), done = newReg();
        Loop& L = pushLoop(true);
        if (isOf) L.iterReg = state;
        L.asyncIter = isAwait;
        size_t top = here();
        if (isAwait) {  // each step is awaited: next() gives a promise of the {value, done} result
            emit(OP_IterCall, v, state);
            emit(OP_Await, v, v);
            emit(OP_IterResult, v, v, done);
        } else
            emit(isOf ? OP_IterNext : OP_ForInNext, v, state, done);
        size_t out = jump(OP_JmpT, done);
        size_t bodyStart = here();
        int depth = f->ctxDepth;
        if (n->scope->hasCtx) emit(OP_CloneCtx);
        if (n->a->type == N::VarDecl && (n->a->flags & NF_USING)) {
            withDisposal((n->a->flags & NF_AWAIT) ? 2 : 1, [&] {
                emit(OP_AddDisposable, f->disposeReg, v, (n->a->flags & NF_AWAIT) ? 1 : 0);
                bindTarget(n->a->list[0]->a, v, true);
                stmt(n->d);
            });
        } else {
            if (n->a->type == N::VarDecl) bindTarget(n->a->list[0]->a, v, true);
            else bindTarget(n->a, v, false);
            stmt(n->d);
        }
        size_t bodyEnd = here();
        patchConts();
        patchTo(jump(OP_Jmp), top);
        if (isOf) {  // an exception in the body closes the iterator and then carries on (ECMA-262 14.7.5.7)
            size_t handler = here();
            int e = newReg();
            emit(OP_Catch, e);
            emitIterClose(state, isAwait, true);
            emit(OP_Throw, e);
            f->p->handlers.push_back({(uint32_t)bodyStart, (uint32_t)bodyEnd, (uint32_t)handler, (uint16_t)depth, 1});
        }
        // a `break` must not run IterClose twice, so breaks land after the normal exit
        patch(out);
        popLoop();
        f->nextReg = mark;
        exitScope(sm);
    }

    // Registers a handler for [start, end), leaving out the finally blocks that were compiled in line for this try
    // statement or the ones around it: by the time one of those runs, the statement has been left (14.15.3).
    void protect(size_t start, size_t end, size_t target, int depth, int isFinally, size_t finDepth, bool ownsFin) {
        std::vector<std::pair<size_t, size_t>> holes;
        for (auto& u : f->unwound)
            if (u.start >= start && u.end <= end && (ownsFin ? u.index <= finDepth : u.index < finDepth)) holes.push_back({u.start, u.end});
        std::sort(holes.begin(), holes.end());
        size_t cur = start;
        for (auto& h : holes) {
            if (h.first > cur) f->p->handlers.push_back({(uint32_t)cur, (uint32_t)h.first, (uint32_t)target, (uint16_t)depth, (uint16_t)isFinally});
            cur = std::max(cur, h.second);
        }
        if (cur < end) f->p->handlers.push_back({(uint32_t)cur, (uint32_t)end, (uint32_t)target, (uint16_t)depth, (uint16_t)isFinally});
    }
    void tryStmt(Node* n) {
        int depth = f->ctxDepth;
        size_t finDepth = f->fins.size();
        bool hasFin = n->d != nullptr;
        if (hasFin) f->fins.push_back({n->d, depth, f->loops.size()});
        size_t start = here();
        if (n->c) f->tryDepth++;
        stmt(n->a);
        if (n->c) f->tryDepth--;
        size_t endTry = here();
        if (n->c) {
            size_t after = jump(OP_Jmp);
            size_t handler = here();
            ScopeMark sm = enterScope(n->scope);
            int e = newReg();
            emit(OP_Catch, e);
            if (n->b) bindTarget(n->b, e, true);
            stmt(n->c);
            exitScope(sm);
            patch(after);
            protect(start, endTry, handler, depth, 0, finDepth, hasFin);
        }
        if (hasFin) {
            size_t endAll = here();
            f->fins.pop_back();
            quiet(n->d);
            size_t after = jump(OP_Jmp);
            size_t handler = here();
            int e = newReg();
            emit(OP_Catch, e);
            quiet(n->d);
            emit(OP_Throw, e);
            f->nextReg--;
            patch(after);
            protect(start, endAll, handler, depth, 1, finDepth, true);
        }
    }

    // A finally block's value never becomes the value of its try statement.
    void quiet(Node* n) {
        int saved = f->cptn;
        f->cptn = -1;
        stmt(n);
        f->cptn = saved;
    }
    void stmt(Node* n) {
        curLine = n->line;
        int mark = f->nextReg;
        // these statements have the value undefined unless something inside them produces one (UpdateEmpty)
        if (f->cptn >= 0 && (n->type == N::If || n->type == N::While || n->type == N::DoWhile || n->type == N::For || n->type == N::ForIn ||
                             n->type == N::ForOf || n->type == N::Switch || n->type == N::Try || n->type == N::With))
            emit(OP_LoadUndef, f->cptn);
        switch (n->type) {
            case N::ExprStmt:
                if (f->cptn >= 0) expr(n->a, f->cptn);
                else effect(n->a);
                break;
            case N::VarDecl: varDecl(n); break;
            case N::Function:
                // Annex B.3.3: reaching a block-level function declaration copies it to the function-level var
                if (n->var && n->var2) {
                    int r = newReg();
                    if (n->var->captured) emit(OP_GetCtx, r, depthOf(n->var), n->var->slot);
                    else emit(OP_Move, r, n->var->reg);
                    storeVar(n->var2, n->var2->name, r, false);
                } else if (n->var && (n->flags & NF_DYN) && evalObjReg >= 0) {  // the same, for a var that eval gave its caller
                    int r = newReg(), name = newReg();
                    if (n->var->captured) emit(OP_GetCtx, r, depthOf(n->var), n->var->slot);
                    else emit(OP_Move, r, n->var->reg);
                    emitX(OP_LoadK, name, kstr(n->var->name));
                    emit(OP_SetElem, evalObjReg, name, r);
                }
                break;
            case N::Empty:
            case N::Debugger: break;
            case N::Class: {
                int r = emitClass(n, newReg());
                storeVar(n->var2, n->str, r, true);
                break;
            }
            case N::Block: {
                ScopeMark sm = enterScope(n->scope);
                hoist(n->scope);
                if (n->scope && n->scope->usingKind) withDisposal(n->scope->usingKind, [&] { for (Node* s : n->list) stmt(s); });
                else
                    for (Node* s : n->list) stmt(s);
                exitScope(sm);
                break;
            }
            case N::If: {
                size_t j = jump(OP_JmpF, expr(n->a));
                f->nextReg = mark;
                stmt(n->b);
                if (n->c) {
                    size_t e = jump(OP_Jmp);
                    patch(j);
                    stmt(n->c);
                    patch(e);
                } else
                    patch(j);
                break;
            }
            case N::While: {
                pushLoop(true);
                size_t top = here();
                size_t out = jump(OP_JmpF, expr(n->a));
                f->nextReg = mark;
                stmt(n->d);
                patchConts();
                patchTo(jump(OP_Jmp), top);
                patch(out);
                popLoop();
                break;
            }
            case N::DoWhile: {
                pushLoop(true);
                size_t top = here();
                stmt(n->d);
                patchConts();
                patchTo(jump(OP_JmpT, expr(n->a)), top);
                popLoop();
                break;
            }
            case N::For: {
                ScopeMark sm = enterScope(n->scope);
                auto loop = [&] {
                    if (n->a) quiet(n->a);
                    if (n->a && n->scope->hasCtx) emit(OP_CloneCtx);
                    pushLoop(true);
                    size_t top = here();
                    size_t out = SIZE_MAX;
                    if (n->b) {
                        int m2 = f->nextReg;
                        out = jump(OP_JmpF, expr(n->b));
                        f->nextReg = m2;
                    }
                    stmt(n->d);
                    patchConts();
                    if (n->scope->hasCtx) emit(OP_CloneCtx);
                    if (n->c) effect(n->c);
                    patchTo(jump(OP_Jmp), top);
                    if (out != SIZE_MAX) patch(out);
                    popLoop();
                };
                if (n->scope->usingKind) withDisposal(n->scope->usingKind, loop);  // the whole loop is the scope of its `using` declarations
                else loop();
                exitScope(sm);
                break;
            }
            case N::ForIn:
            case N::ForOf: forInOf(n); break;
            case N::Return: {
                int r = -1;
                if (n->a) {
                    r = newReg();
                    FunctionInfo* in = f->info;
                    bool tailOk = in->strict && !in->isGenerator && !in->isAsync && !in->isClassCtor && !in->isEvalBody && in != ast.top && in != ast.moduleFn &&
                                  f->fins.empty() && !hasOpenIter() && f->tryDepth == 0;
                    if (tailOk) tailExpr(n->a, r);
                    else expr(n->a, r);
                    if (f->info->isAsync && f->info->isGenerator) emit(OP_Await, r, r);  // async generators await what they return
                }
                if (!f->fins.empty() || hasOpenIter()) unwind(0, -1, 0);
                syncThis();
                if (r >= 0) emit(OP_Return, r);
                else emit(OP_RetUndef);
                break;
            }
            case N::Break:
            case N::Continue: jumpOut(n); break;
            case N::Throw: emit(OP_Throw, expr(n->a)); break;
            case N::Try: tryStmt(n); break;
            case N::With: {
                int o = newReg();
                emit(OP_ToObject, o, expr(n->a));
                ScopeMark sm = enterScope(n->scope);
                storeVar(n->var, n->var->name, o, true);
                stmt(n->d);
                exitScope(sm);
                break;
            }
            case N::Switch: {
                int d = newReg();
                expr(n->a, d);
                ScopeMark sm = enterScope(n->scope);
                hoist(n->scope);
                pushLoop(false, true);
                std::vector<size_t> jumps(n->list.size(), SIZE_MAX);
                size_t deflt = SIZE_MAX;
                int t = newReg();
                for (size_t i = 0; i < n->list.size(); ++i) {
                    Node* c = n->list[i];
                    if (!c->a) {
                        deflt = i;
                        continue;
                    }
                    int m2 = f->nextReg;
                    emit(OP_SEq, t, d, expr(c->a));
                    jumps[i] = jump(OP_JmpT, t);
                    f->nextReg = m2;
                }
                size_t toDefault = jump(OP_Jmp);
                for (size_t i = 0; i < n->list.size(); ++i) {
                    if (jumps[i] != SIZE_MAX) patch(jumps[i]);
                    if (i == deflt) patch(toDefault);
                    for (Node* s : n->list[i]->list) stmt(s);
                }
                if (deflt == SIZE_MAX) patch(toDefault);
                popLoop();
                exitScope(sm);
                break;
            }
            case N::Labeled: {
                f->pendingLabels.push_back(n->str);
                N bt = n->a->type;
                if (bt == N::While || bt == N::DoWhile || bt == N::For || bt == N::ForIn || bt == N::ForOf || bt == N::Switch ||
                    bt == N::Labeled) {
                    stmt(n->a);
                } else {
                    pushLoop(false);
                    stmt(n->a);
                    popLoop();
                }
                f->pendingLabels.clear();
                break;
            }
            default: fail("internal: unexpected statement");
        }
        f->nextReg = mark;
    }

    bool hasOpenIter() {
        for (auto& L : f->loops)
            if (L.iterReg >= 0) return true;
        return false;
    }
};

}  // namespace

FuncProto* compileEval(Vm& vm, std::shared_ptr<const std::u16string> src, const EvalSite& site) {
    Ast ast;
    ast.src = std::move(src);
    try {
        parseEval(vm, ast, site);
        Compiler c(vm, ast, "<eval>");
        return c.evalFunction();
    } catch (const SyntaxErr& e) {
        std::string m = e.msg + " (<eval>:" + std::to_string(e.line) + ")";
        vm.throwError(Err::SyntaxError, utf8to16(m));
    }
}

// CreateDynamicFunction (20.2.1.1.1) steps 17 to 24: the parameters must be valid alone, then the text as a whole.
FuncProto* compileDynamicFunction(Vm& vm, std::shared_ptr<const std::u16string> src, const std::u16string& params, int kind, const std::string& file) {
    try {
        {
            Ast pa;
            pa.src = std::make_shared<const std::u16string>(params + u"\n)");
            parseParamsOnly(vm, pa, kind);
        }
        Ast ast;
        ast.src = std::move(src);
        parseDynamicFunction(vm, ast, kind);
        Compiler c(vm, ast, file);
        return c.program();
    } catch (const SyntaxErr& e) {
        std::string m = e.msg + " (" + file + ":" + std::to_string(e.line) + ")";
        vm.throwError(Err::SyntaxError, utf8to16(m));
    }
}

FuncProto* compileScript(Vm& vm, std::shared_ptr<const std::u16string> src, const std::string& file, bool functionBody,
                         const std::u16string* params, ModuleInfo* module) {
    Ast ast;
    ast.src = std::move(src);
    (void)functionBody;
    try {
        parseScript(vm, ast, module != nullptr, params);
        Compiler c(vm, ast, file);
        FuncProto* p = c.program();
        if (module) {
            for (JSString* r : ast.requests) module->requests.push_back(r->s);
            for (JSString* t : ast.requestTypes) module->requestTypes.push_back(t ? t->s : std::u16string());
            for (Variable* v : ast.requestVars) module->requestSlots.push_back(v->slot);
            for (auto& e : ast.exports)
                module->exports.push_back({e.name->s, e.var ? e.var->slot : -1, e.request, e.importName ? e.importName->s : std::u16string(), e.importName != nullptr});
            module->stars = ast.starRequests;
            for (auto& im : ast.imports) module->imports.push_back({im.request, im.name->s});
            module->hasTopLevelAwait = ast.hasTLA;
        }
        return p;
    } catch (const SyntaxErr& e) {
        std::string m = e.msg + " (" + file + ":" + std::to_string(e.line) + ")";
        vm.throwError(Err::SyntaxError, utf8to16(m));
    }
}

}  // namespace tb::js
