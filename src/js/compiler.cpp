// Bytecode compiler: walks the resolved AST and emits register-machine code.
#include "bytecode.h"
#include "parse.h"

namespace tb::js {

namespace {

struct Loop {
    std::vector<JSString*> labels;
    std::vector<size_t> breaks, conts;
    int breakDepth = 0, contDepth = 0, iterReg = -1;
    bool isLoop = false, isSwitch = false;
    size_t finMark = 0;
};
struct Fin {
    Node* block;
    int ctxDepth;
    size_t loopMark;
};
struct Fn {
    FunctionInfo* info = nullptr;
    FuncProto* p = nullptr;
    int nextReg = 2, maxReg = 2, ctxDepth = 0;
    size_t chainBase = 0;
    std::vector<Loop> loops;
    std::vector<Fin> fins;
    std::unordered_map<uint64_t, uint32_t> kmap;
    std::vector<std::vector<size_t>> chains;  // optional-chain bail-out jumps
    std::vector<JSString*> pendingLabels;
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
        Scope* sc = info->scope;
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
        for (Node* s : info->body) {
            if (s->type == N::ExprStmt) {
                curLine = s->line;
                int mark = f->nextReg;
                expr(s->a, result);
                f->nextReg = mark;
            } else
                stmt(s);
        }
        emit(OP_Return, result);
        return end(fn);
    }

private:
    Vm& vm;
    Ast& ast;
    std::string file_;
    Fn* f = nullptr;
    std::vector<Scope*> chain;  // scopes that own a runtime context, innermost last
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
            emit(OP_CreateArgs, t);
            storeVar(info->argsVar, info->argsVar->name, t, true);
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
        for (int i = 0; i < np; ++i) {
            Node* p = info->params[i];
            Node* target = p->type == N::AssignPat ? p->a : p;
            if (target->type == N::Ident && !target->var->captured) target->var->reg = 2 + i;
        }
        enterScope(sc, true);
        if (info == ast.moduleFn) emit(OP_ModuleInit, 2);  // r2 is the namespace argument
        prologuePseudo(info);
        for (int i = 0; i < np; ++i) {
            Node* p = info->params[i];
            int reg = 2 + i;
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
                if (target->var->captured) storeVar(target->var, target->str, reg, true);
            } else
                bindTarget(target, reg, true);
        }
        hoist(sc);
        if (info == ast.moduleFn) {
            // The module's scope now exists with its functions in place (ECMA-262 InitializeEnvironment). The
            // body waits here until the loader evaluates the module.
            int t = newReg();
            emit(OP_LoadUndef, t);
            emit(OP_Yield, t, t);
            f->nextReg--;
        }
        for (Node* s : info->body) stmt(s);
        emit(OP_RetUndef);
        FuncProto* p = end(fn);
        f = parent;
        curLine = savedLine;
        return p;
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
            else if (v->kind == VK_FUNC && init) emitX(OP_DefGlobalFunc, src, kstr(v->name));
            else emitX(OP_SetGlobal, src, kstr(v->name));
            return;
        }
        if (v->kind == VK_IMPORT) {  // imports are immutable bindings
            emitX(OP_ThrowConst, 0, kstr(v->name));
            return;
        }
        if (!init && v->kind == VK_CONST) {
            emitX(OP_ThrowConst, 0, kstr(v->name));
            return;
        }
        if (!init && (v->kind == VK_FUNCNAME || v->kind == VK_PRIVATE)) return;
        if (v->captured) emit(OP_SetCtx, src, depthOf(v), v->slot);
        else if (v->reg != src) emit(OP_Move, v->reg, src);
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
            case N::Num: case N::Str: case N::Null: case N::True: case N::False: case N::Binary: case N::Unary: case N::Call:
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
            return r;
        }
        return 1;
    }

    int expr0(Node* n, int dst) {
        auto need = [&] {
            if (dst < 0) dst = newReg();
            return dst;
        };
        switch (n->type) {
            case N::Num: loadNum(need(), n->num); return dst;
            case N::Str: emitX(OP_LoadK, need(), kstr(n->str)); return dst;
            case N::Null: emit(OP_LoadNull, need()); return dst;
            case N::True: emit(OP_LoadTrue, need()); return dst;
            case N::False: emit(OP_LoadFalse, need()); return dst;
            case N::Hole: emit(OP_LoadUndef, need()); return dst;
            case N::Ident: {
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
                need();
                int mark = f->nextReg;
                int base = newReg();
                newReg();
                calleeInto(n->a, base);
                int strs = newReg();
                emit(OP_NewArray, strs);
                int raw = newReg();
                emit(OP_NewArray, raw);
                Node* tl = n->b;
                int t = newReg();
                for (size_t i = 0; i < tl->strs.size(); i += 2) {
                    emitX(OP_LoadK, t, kstr(tl->strs[i]));
                    emit(OP_ArrayPush, strs, t);
                    emitX(OP_LoadK, t, kstr(tl->strs[i + 1]));
                    emit(OP_ArrayPush, raw, t);
                }
                setProp(strs, vm.a.raw, raw);
                f->nextReg = base + 3;  // strs is the first argument
                for (Node* e : tl->list) {
                    int r = newReg();
                    expr(e, r);
                    f->nextReg = r + 1;
                }
                emit(OP_Call, dst, base, 1 + (int)tl->list.size());
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
                        if (n->a->type == N::Ident && (!n->a->var || n->a->var->isGlobal)) {
                            emitX(OP_GetGlobalTypeof, dst, kstr(n->a->str));
                        } else
                            emit(OP_Typeof, dst, expr(n->a));
                        break;
                    case P_Void:
                        expr(n->a);
                        emit(OP_LoadUndef, dst);
                        break;
                    case P_Delete: {
                        Node* t = n->a->type == N::OptChain ? n->a->a : n->a;
                        if (t->type == N::Member) {
                            int o = expr(t->a);
                            uint32_t k = kstr(t->str);
                            if (k > 65535) fail("Too many constants");
                            emit(OP_DelProp, dst, o, (int)k);
                        } else if (t->type == N::Index) {
                            int o = expr(t->a);
                            emit(OP_DelElem, dst, o, expr(t->b));
                        } else {
                            expr(n->a);
                            emit(OP_LoadTrue, dst);
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
                    int k = superKey(n);
                    emit(OP_GetSuper, dst, k, thisReg(n->a));
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
                    int k = superKey(n);
                    emit(OP_GetSuper, dst, k, thisReg(n->a));
                } else {
                    int o = operand(n->a, n->b);
                    if (n->flags & NF_OPTIONAL) bail(o);
                    if (n->b->type == N::Str && !(n->flags & NF_PRIVATE)) getProp(dst, o, n->b->str);
                    else emit(OP_GetElem, dst, o, expr(n->b));
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
                if (n->flags & NF_DELEGATE) {
                    // yield* inner: forward every value of the inner iterator
                    int it = newReg();
                    newReg();
                    emit(OP_GetIter, it, expr(n->a));
                    int v = newReg(), done = newReg(), sent = newReg();
                    size_t top = here();
                    emit(OP_IterNextRet, v, it, done);
                    size_t out = jump(OP_JmpT, done);
                    emit(OP_Yield, sent, v);
                    patchTo(jump(OP_Jmp), top);
                    patch(out);
                    emit(OP_Move, dst, v);
                } else {
                    int v;
                    if (n->a) v = expr(n->a);
                    else {
                        v = newReg();
                        emit(OP_LoadUndef, v);
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
            else emit(OP_GetElem, base, base + 1, expr(c->b));
            f->nextReg = m2;
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

    int call(Node* n, int dst) {
        int mark = f->nextReg;
        int base = newReg();
        newReg();
        if (n->a->type == N::Super) {
            if (!f->info->isClassCtor || !f->info->isDerived) fail("'super' call is only valid in a derived class constructor");
            if (args(n, base)) emit(OP_SuperCallSpread, dst, base);
            else emit(OP_SuperCall, dst, base, (int)n->list.size());
            if (f->info->thisVar) storeVar(f->info->thisVar, f->info->thisVar->name, 1, true);
            f->nextReg = mark;
            return dst;
        }
        calleeInto(n->a, base);
        if (n->flags & NF_OPTIONAL) bail(base);
        if (args(n, base)) emit(OP_CallSpread, dst, base);
        else emit(OP_Call, dst, base, (int)n->list.size());
        f->nextReg = mark;
        return dst;
    }

    int update(Node* n, int dst, bool discard) {
        Op op = n->op == P_Inc ? OP_Inc : OP_Dec;
        bool prefix = (n->flags & NF_PREFIX) || discard;
        Node* t = n->a;
        if (t->type == N::Ident && t->var && !t->var->captured && !t->var->isGlobal && t->var->kind != VK_CONST) {
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
        if (t->type == N::Ident) {
            int cur = expr(t);
            emit(OP_ToNumeric, oldv, cur);
            emit(op, newv, oldv);
            storeVar(t->var, t->str, newv, false);
        } else if (t->type == N::Member) {
            int o = expr(t->a);
            getProp(oldv, o, t->str);
            emit(OP_ToNumeric, oldv, oldv);
            emit(op, newv, oldv);
            setProp(o, t->str, newv);
        } else {
            int o = expr(t->a);
            int k = newReg();
            emit(OP_ToPropKey, k, expr(t->b));
            emit(OP_GetElem, oldv, o, k);
            emit(OP_ToNumeric, oldv, oldv);
            emit(op, newv, oldv);
            emit(OP_SetElem, o, k, newv);
        }
        emit(OP_Move, dst, prefix ? newv : oldv);
        f->nextReg = mark;
        return dst;
    }

    int assign(Node* n, int dst) {
        Node* t = n->a;
        if (n->op == P_Assign) {
            if (t->type == N::Ident) {
                Variable* v = t->var;
                if (v && !v->captured && !v->isGlobal && v->kind != VK_CONST && v->kind != VK_FUNCNAME && v->kind != VK_PRIVATE &&
                    directOk(n->b) && !mentions(n->b, v)) {
                    expr(n->b, v->reg);
                    if (dst < 0 || dst == v->reg) return v->reg;
                    emit(OP_Move, dst, v->reg);
                    return dst;
                }
                if (dst < 0) dst = newReg();
                int mark = f->nextReg;
                int r = expr(n->b);
                storeVar(v, t->str, r, false);
                if (r != dst) emit(OP_Move, dst, r);
                f->nextReg = mark;
                return dst;
            }
            if (dst < 0) dst = newReg();
            int mark = f->nextReg;
            if (t->type == N::Member) {
                if (t->a->type == N::Super) {
                    int k = superKey(t);
                    expr(n->b, dst);
                    emit(OP_SetSuper, k, dst, thisReg(t->a));
                } else {
                    int o = operand(t->a, n->b);
                    expr(n->b, dst);
                    setProp(o, t->str, dst);
                }
            } else if (t->type == N::Index) {
                int o = operand(t->a, n->b);
                int k = expr(t->b);
                if (t->b->type != N::Num && t->b->type != N::Str && !(t->flags & NF_PRIVATE)) {
                    int kk = newReg();
                    emit(OP_Move, kk, k);
                    k = kk;
                }
                expr(n->b, dst);
                emit(OP_SetElem, o, k, dst);
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
        int o = -1, k = -1;
        if (t->type == N::Ident) {
            int r = expr(t);
            if (r != cur) emit(OP_Move, cur, r);
        } else if (t->type == N::Member) {
            o = expr(t->a);
            getProp(cur, o, t->str);
        } else {
            o = expr(t->a);
            k = newReg();
            emit(OP_ToPropKey, k, expr(t->b));
            emit(OP_GetElem, cur, o, k);
        }
        size_t skip = 0;
        if (logical) {
            skip = jump(jop, cur);
            expr(n->b, cur);
        } else {
            int r = expr(n->b);
            emit(binop(n->op), cur, cur, r);
        }
        if (t->type == N::Ident) storeVar(t->var, t->str, cur, false);
        else if (t->type == N::Member) setProp(o, t->str, cur);
        else emit(OP_SetElem, o, k, cur);
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
                    emit(OP_DefElem, obj, kr, expr(p->b));
                }
            } else {
                int kr = keyReg(p, p->a);
                int fr = newReg();
                closure(p->b->fn, fr);
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
        int parent = -1;
        if (n->a) {
            parent = newReg();
            expr(n->a, parent);
        }
        ScopeMark sm = enterScope(n->scope);
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
        ctor->srcStart = n->pos;
        f->p->inner.push_back(ctor);
        emitX(OP_Closure, rc, (uint32_t)f->p->inner.size() - 1);
        emit(OP_MakeClass, rc, parent < 0 ? 0xFFFF : parent, rp);
        for (Node* m : n->list) {
            if (m->op == 255) continue;
            int m2 = f->nextReg;
            curLine = m->line;
            int target = (m->flags & NF_STATIC) ? rc : rp;
            int kr = keyReg(m, m->a);
            if (m->op == PK_FIELD) {
                int fr = newReg();
                if (m->b) closure(m->b->fn, fr);
                else emit(OP_LoadUndef, fr);
                emit((m->flags & NF_STATIC) ? OP_DefStaticField : OP_AddField, rc, kr, fr);
            } else {
                int fr = newReg();
                closure(m->b->fn, fr);
                emit(m->op == PK_GET ? OP_DefGetter : m->op == PK_SET ? OP_DefSetter : OP_DefMethod, target, kr, fr);
            }
            f->nextReg = m2;
        }
        if (n->var && n->var->scope == n->scope) storeVar(n->var, n->var->name, rc, true);
        emit(OP_Move, dst, rc);
        exitScope(sm);
        f->nextReg = mark;
        return dst;
    }

    // ---------------------------------------------------------------- destructuring
    void bindTarget(Node* t, int src, bool init) {
        int mark = f->nextReg;
        switch (t->type) {
            case N::Ident: storeVar(t->var, t->str, src, init); break;
            case N::Member: {
                int o = expr(t->a);
                setProp(o, t->str, src);
                break;
            }
            case N::Index: {
                int o = expr(t->a);
                emit(OP_SetElem, o, expr(t->b), src);
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
                for (Node* e : t->list) {
                    if (e->type == N::Rest) {
                        int arr = newReg();
                        emit(OP_NewArray, arr);
                        size_t top = here();
                        emit(OP_IterNext, v, it, done);
                        size_t out = jump(OP_JmpT, done);
                        emit(OP_ArrayPush, arr, v);
                        patchTo(jump(OP_Jmp), top);
                        patch(out);
                        bindTarget(e->a, arr, init);
                    } else {
                        emit(OP_IterNext, v, it, done);
                        if (e->type != N::Hole) bindTarget(e, v, init);
                    }
                }
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
                        emit(OP_ObjRest, v, src, excluded);
                        bindTarget(p->a, v, init);
                    } else {
                        if (!(p->flags & NF_COMPUTED)) {
                            getProp(v, src, p->a->str);
                            if (hasRest) {
                                int kr = newReg();
                                emitX(OP_LoadK, kr, kstr(p->a->str));
                                emit(OP_ArrayPush, excluded, kr);
                            }
                        } else {
                            int kr = newReg();
                            emit(OP_ToPropKey, kr, expr(p->a));
                            emit(OP_GetElem, v, src, kr);
                            if (hasRest) emit(OP_ArrayPush, excluded, kr);
                        }
                        bindTarget(p->b, v, init);
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

    // Emits the code needed before a jump that leaves try/finally blocks, loops and scopes.
    void unwind(size_t finMark, int targetDepth, size_t loopFrom) {
        for (size_t i = f->loops.size(); i-- > loopFrom;)
            if (f->loops[i].iterReg >= 0) emit(OP_IterClose, f->loops[i].iterReg);
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
            stmt(fin.block);
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
            if (d->b) {
                if (t->type == N::Ident) {
                    Variable* v = t->var;
                    if (v && !v->captured && !v->isGlobal && directOk(d->b) && !mentions(d->b, v)) expr(d->b, v->reg);
                    else storeVar(v, t->str, expr(d->b), true);
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
        ScopeMark sm = enterScope(n->scope);
        int mark = f->nextReg;
        int src = expr(n->b);
        int state = newReg();
        newReg();
        emit(isOf ? OP_GetIter : OP_ForInPrep, state, src);
        int v = newReg(), done = newReg();
        Loop& L = pushLoop(true);
        if (isOf) L.iterReg = state;
        size_t top = here();
        emit(isOf ? OP_IterNext : OP_ForInNext, v, state, done);
        size_t out = jump(OP_JmpT, done);
        if (n->scope->hasCtx) emit(OP_CloneCtx);
        if (n->a->type == N::VarDecl) bindTarget(n->a->list[0]->a, v, true);
        else bindTarget(n->a, v, false);
        stmt(n->d);
        patchConts();
        patchTo(jump(OP_Jmp), top);
        // a `break` must not run IterClose twice, so breaks land after the normal exit
        patch(out);
        popLoop();
        f->nextReg = mark;
        exitScope(sm);
    }

    void tryStmt(Node* n) {
        int depth = f->ctxDepth;
        bool hasFin = n->d != nullptr;
        if (hasFin) f->fins.push_back({n->d, depth, f->loops.size()});
        size_t start = here();
        stmt(n->a);
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
            f->p->handlers.push_back({(uint32_t)start, (uint32_t)endTry, (uint32_t)handler, (uint16_t)depth});
        }
        if (hasFin) {
            size_t endAll = here();
            f->fins.pop_back();
            stmt(n->d);
            size_t after = jump(OP_Jmp);
            size_t handler = here();
            int e = newReg();
            emit(OP_Catch, e);
            stmt(n->d);
            emit(OP_Throw, e);
            f->nextReg--;
            patch(after);
            f->p->handlers.push_back({(uint32_t)start, (uint32_t)endAll, (uint32_t)handler, (uint16_t)depth});
        }
    }

    void stmt(Node* n) {
        curLine = n->line;
        int mark = f->nextReg;
        switch (n->type) {
            case N::ExprStmt: effect(n->a); break;
            case N::VarDecl: varDecl(n); break;
            case N::Function:
            case N::Empty:
            case N::Debugger: break;
            case N::Class: {
                int r = emitClass(n, newReg());
                storeVar(n->var, n->str, r, true);
                break;
            }
            case N::Block: {
                ScopeMark sm = enterScope(n->scope);
                hoist(n->scope);
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
                if (n->a) stmt(n->a);
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
                exitScope(sm);
                break;
            }
            case N::ForIn:
            case N::ForOf: forInOf(n); break;
            case N::Return: {
                int r = -1;
                if (n->a) {
                    r = newReg();
                    expr(n->a, r);
                }
                if (!f->fins.empty() || hasOpenIter()) unwind(0, -1, 0);
                if (r >= 0) emit(OP_Return, r);
                else emit(OP_RetUndef);
                break;
            }
            case N::Break:
            case N::Continue: jumpOut(n); break;
            case N::Throw: emit(OP_Throw, expr(n->a)); break;
            case N::Try: tryStmt(n); break;
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
