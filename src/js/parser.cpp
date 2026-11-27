// Lexer and recursive-descent parser. Produces an AST with identifiers already resolved to variables.
#include <cstdlib>

#include "parse.h"
#include "regex.h"

namespace tb::js {

namespace {

struct Tok {
    enum T : uint8_t { End, Ident, Num, Str, Templ, Regex, Punct, Private } type = End;
    uint8_t p = P_None;
    bool nl = false, tail = false, esc = false, oct = false, big = false;
    bool bad = false;  // a template piece with an escape that has no meaning: fine only in a tagged template
    uint32_t pos = 0, end = 0, line = 1;
    double num = 0;
    JSString* str = nullptr;
    JSString* raw = nullptr;
};

bool isWs(char16_t c) {
    return c == ' ' || c == '\t' || c == 0x0B || c == 0x0C || c == 0xA0 || c == 0xFEFF || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}
bool isLt(char16_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }
bool isDigit(char16_t c) { return c >= '0' && c <= '9'; }
// For one UTF-16 unit. Half of a surrogate pair is let through here; name() checks the whole character.
bool idStart(char16_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_';
    return (c >= 0xD800 && c <= 0xDFFF) || unicodeIdStart(c);
}
bool idPart(char16_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_' || isDigit(c);
    return c == 0x200C || c == 0x200D || (c >= 0xD800 && c <= 0xDFFF) || unicodeIdContinue(c);
}
bool idStartCp(uint32_t c) { return c < 0x80 ? idStart((char16_t)c) : unicodeIdStart(c); }
bool idPartCp(uint32_t c) { return c < 0x80 ? idPart((char16_t)c) : c == 0x200C || c == 0x200D || unicodeIdContinue(c); }
int hexVal(char16_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
void appendCp(std::u16string& o, uint32_t cp) {
    if (cp < 0x10000) o += (char16_t)cp;
    else {
        cp -= 0x10000;
        o += (char16_t)(0xD800 + (cp >> 10));
        o += (char16_t)(0xDC00 + (cp & 0x3FF));
    }
}

class Lexer {
public:
    Vm& vm;
    const std::u16string& s;
    size_t pos = 0;
    uint32_t line = 1;
    Lexer(Vm& v, const std::u16string& src) : vm(v), s(src) {}
    [[noreturn]] void fail(const std::string& m) { throw SyntaxErr{m, line}; }

    // IdentifierName: letters, digits, $ and _, with \uXXXX escapes that must themselves spell a valid character.
    void name(std::u16string& id, bool& escaped) {
        size_t n = s.size(), first = id.size();
        while (pos < n && (idPart(s[pos]) || s[pos] == '\\')) {
            size_t at = id.size();
            if (s[pos] == '\\') {
                ++pos;
                if (pos >= n || s[pos] != 'u') fail("Invalid Unicode escape sequence");
                escape(id);
                escaped = true;
            } else {
                id += s[pos++];
                if (id.back() >= 0xD800 && id.back() <= 0xDBFF && pos < n && s[pos] >= 0xDC00 && s[pos] <= 0xDFFF) id += s[pos++];
            }
            uint32_t ch = id[at];
            if (ch >= 0xD800 && ch <= 0xDBFF && at + 1 < id.size()) ch = 0x10000 + ((ch - 0xD800) << 10) + (id[at + 1] - 0xDC00);
            if (at == first ? !idStartCp(ch) : !idPartCp(ch)) fail("Invalid or unexpected token");
        }
    }

    bool skipSpace() {
        bool nl = false;
        size_t n = s.size();
        if (pos == 0 && s.compare(0, 2, u"#!") == 0)  // hashbang comment: only at the very start
            while (pos < n && !isLt(s[pos])) ++pos;
        while (pos < n) {
            char16_t c = s[pos];
            if (isWs(c)) ++pos;
            else if (isLt(c)) {
                if (c == '\n') ++line;
                else if (c == '\r' && !(pos + 1 < n && s[pos + 1] == '\n')) ++line;
                nl = true;
                ++pos;
            } else if (c == '/' && pos + 1 < n && s[pos + 1] == '/') {
                while (pos < n && !isLt(s[pos])) ++pos;
            } else if (c == '/' && pos + 1 < n && s[pos + 1] == '*') {
                pos += 2;
                while (pos < n && !(s[pos] == '*' && pos + 1 < n && s[pos + 1] == '/')) {
                    if (isLt(s[pos])) {  // a comment that spans lines counts as a line terminator (12.4)
                        if (s[pos] == '\n' || (s[pos] == '\r' && !(pos + 1 < n && s[pos + 1] == '\n'))) ++line;
                        nl = true;
                    }
                    ++pos;
                }
                if (pos >= n) fail("Unterminated comment");
                pos += 2;
            } else if (c == '<' && !noHtmlComments && s.compare(pos, 4, u"<!--") == 0) {
                while (pos < n && !isLt(s[pos])) ++pos;
            } else if (c == '-' && !noHtmlComments && (nl || atStart) && s.compare(pos, 3, u"-->") == 0) {
                while (pos < n && !isLt(s[pos])) ++pos;
            } else
                break;
        }
        return nl;
    }

    void escape(std::u16string& out) {
        size_t n = s.size();
        if (pos >= n) fail("Bad escape");
        char16_t c = s[pos++];
        switch (c) {
            case 'n': out += u'\n'; break;
            case 't': out += u'\t'; break;
            case 'r': out += u'\r'; break;
            case 'b': out += u'\b'; break;
            case 'f': out += u'\f'; break;
            case 'v': out += u'\v'; break;
            case 'x': {
                int a = pos + 1 < n ? hexVal(s[pos]) : -1, b = pos + 1 < n ? hexVal(s[pos + 1]) : -1;
                if ((a < 0 || b < 0) && templEsc) {
                    badEsc = true;
                    return;
                }
                if (a < 0 || b < 0) fail("Bad \\x escape");
                out += (char16_t)(a * 16 + b);
                pos += 2;
                break;
            }
            case 'u': {
                uint32_t cp = 0;
                bool ok = true;
                if (pos < n && s[pos] == '{') {
                    ++pos;
                    bool any = false;
                    while (ok && pos < n && s[pos] != '}') {
                        int h = hexVal(s[pos]);
                        if (h < 0) ok = false;
                        else {
                            ++pos;
                            any = true;
                            cp = cp * 16 + h;
                            if (cp > 0x10FFFF) ok = false;
                        }
                    }
                    if (pos >= n || !any) ok = false;
                    if (ok) ++pos;
                } else {
                    for (int i = 0; ok && i < 4; ++i) {
                        int h = pos < n ? hexVal(s[pos]) : -1;
                        if (h < 0) ok = false;
                        else {
                            ++pos;
                            cp = cp * 16 + h;
                        }
                    }
                }
                if (!ok && templEsc) {
                    badEsc = true;
                    return;
                }
                if (!ok) fail("Bad unicode escape");
                appendCp(out, cp);
                break;
            }
            case '\r':
                if (pos < n && s[pos] == '\n') ++pos;
                ++line;
                break;
            case '\n': ++line; break;
            case 0x2028:
            case 0x2029: break;
            default:
                if (templEsc && ((c >= '1' && c <= '9') || (c == '0' && pos < n && isDigit(s[pos])))) {  // no octal escapes in templates
                    badEsc = true;
                    return;
                }
                if (c == '8' || c == '9') sawOctal = true;
                if (c >= '0' && c <= '7') {
                    int v = c - '0';
                    if (!(c == '0' && !(pos < n && isDigit(s[pos])))) {
                        sawOctal = true;
                        for (int i = 0; i < 2 && pos < n && s[pos] >= '0' && s[pos] <= '7' && v * 8 + (s[pos] - '0') < 256; ++i)
                            v = v * 8 + (s[pos++] - '0');
                    }
                    out += (char16_t)v;
                } else
                    out += c;
        }
    }

    Tok templ(size_t from) {  // scans template characters; `from` is just after ` or }
        Tok t;
        t.type = Tok::Templ;
        t.pos = (uint32_t)from;
        t.line = line;
        pos = from;
        size_t n = s.size();
        std::u16string cooked, raw;
        for (;;) {
            if (pos >= n) fail("Unterminated template literal");
            char16_t c = s[pos];
            if (c == '`') {
                ++pos;
                t.tail = true;
                break;
            }
            if (c == '$' && pos + 1 < n && s[pos + 1] == '{') {
                pos += 2;
                break;
            }
            if (c == '\\') {
                size_t st = pos++;
                templEsc = true;
                badEsc = false;
                escape(cooked);
                templEsc = false;
                if (badEsc) t.bad = true;
                for (size_t i = st; i < pos; ++i) {  // the raw text of CR and CR LF is LF (12.9.6 TRV)
                    if (s[i] == '\r') {
                        raw += u'\n';
                        if (i + 1 < pos && s[i + 1] == '\n') ++i;
                    } else
                        raw += s[i];
                }
                continue;
            }
            if (c == '\r') {
                ++pos;
                if (pos < n && s[pos] == '\n') ++pos;
                ++line;
                cooked += u'\n';
                raw += u'\n';
                continue;
            }
            if (c == '\n') ++line;
            cooked += c;
            raw += c;
            ++pos;
        }
        t.str = vm.atom(cooked);
        t.raw = vm.atom(raw);
        t.end = (uint32_t)pos;
        return t;
    }

    Tok regex(size_t start) {
        Tok t;
        t.type = Tok::Regex;
        t.pos = (uint32_t)start;
        t.line = line;
        pos = start + 1;
        size_t n = s.size();
        bool cls = false;
        size_t st = pos;
        for (;;) {
            if (pos >= n || isLt(s[pos])) fail("Unterminated regular expression");
            char16_t c = s[pos];
            if (c == '\\') {
                if (pos + 1 >= n || isLt(s[pos + 1])) fail("Unterminated regular expression");
                pos += 2;
                continue;
            }
            if (c == '[') cls = true;
            else if (c == ']') cls = false;
            else if (c == '/' && !cls) break;
            ++pos;
        }
        t.str = vm.atom(std::u16string_view(s).substr(st, pos - st));
        ++pos;
        size_t fs = pos;
        while (pos < n && idPart(s[pos])) ++pos;
        t.raw = vm.atom(std::u16string_view(s).substr(fs, pos - fs));
        t.end = (uint32_t)pos;
        return t;
    }

    bool sawOctal = false;  // the token being read used a legacy octal number or escape
    bool templEsc = false, badEsc = false;  // reading a template's escape: a bad one is noted, not an error yet
    bool atStart = true;                     // nothing but white space and comments so far
    bool noHtmlComments = false;             // module code: <!-- and --> are operators there
    bool sawBig = false;    // the number being read is a BigInt literal; its digits are in bigText
    std::u16string bigText;
    Tok next() {
        Tok t;
        sawOctal = false;
        t.nl = skipSpace();
        atStart = false;
        t.pos = (uint32_t)pos;
        t.line = line;
        size_t n = s.size();
        if (pos >= n) {
            t.end = (uint32_t)pos;
            return t;
        }
        char16_t c = s[pos];
        if (idStart(c) || c == '\\') {
            std::u16string id;
            name(id, t.esc);
            t.type = Tok::Ident;
            t.str = vm.atom(id);
        } else if (isDigit(c) || (c == '.' && pos + 1 < n && isDigit(s[pos + 1]))) {
            t.type = Tok::Num;
            sawBig = false;
            t.num = number();
            t.oct = sawOctal;
            if (sawBig) {
                t.big = true;
                t.str = vm.atom(bigText);
            }
        } else if (c == '"' || c == '\'') {
            ++pos;
            std::u16string v;
            for (;;) {
                if (pos >= n) fail("Unterminated string");
                char16_t ch = s[pos++];
                if (ch == c) break;
                if (ch == '\n' || ch == '\r') fail("Unterminated string");
                if (ch == '\\') escape(v);
                else v += ch;
            }
            t.type = Tok::Str;
            t.str = vm.atom(v);
            t.oct = sawOctal;
        } else if (c == '`') {
            uint32_t p0 = t.pos;
            bool nl = t.nl;
            t = templ(pos + 1);
            t.pos = p0;
            t.nl = nl;
            return t;
        } else if (c == '#' && pos + 1 < n && (idStart(s[pos + 1]) || s[pos + 1] == '\\')) {
            ++pos;
            std::u16string id = u"#";
            bool esc = false;
            name(id, esc);
            t.type = Tok::Private;
            t.str = vm.atom(id);
        } else {
            t.type = Tok::Punct;
            t.p = punct();
        }
        t.end = (uint32_t)pos;
        return t;
    }

private:
    double number() {
        size_t n = s.size();
        char16_t c = s[pos];
        auto radix = [&](int r) {
            bigText.assign(s, pos, 2);
            pos += 2;
            double v = 0;
            bool any = false;
            bool lastDigit = false;
            while (pos < n) {
                if (s[pos] == '_') {
                    int nextDigit = pos + 1 < n ? hexVal(s[pos + 1]) : -1;
                    if (!lastDigit || nextDigit < 0 || nextDigit >= r) fail("Numeric separators are not allowed here");
                    lastDigit = false;
                    ++pos;
                    continue;
                }
                int h = hexVal(s[pos]);
                if (h < 0 || h >= r) break;
                v = v * r + h;
                bigText += s[pos];
                any = lastDigit = true;
                ++pos;
            }
            if (!any) fail("Invalid number");
            if (pos < n && s[pos] == 'n') {
                sawBig = true;
                ++pos;
            }
            if (pos < n && (idStart(s[pos]) || isDigit(s[pos]))) fail("Invalid number");
            return v;
        };
        if (c == '0' && pos + 1 < n) {
            char16_t x = s[pos + 1] | 0x20;
            if (x == 'x') return radix(16);
            if (x == 'o') return radix(8);
            if (x == 'b') return radix(2);
            if (s[pos + 1] == '_') fail("Numeric separators are not allowed here");
            if (isDigit(s[pos + 1])) {  // legacy octal, unless a digit is 8 or 9
                size_t e = pos;
                bool oct = true;
                while (e < n && isDigit(s[e])) {
                    if (s[e] > '7') oct = false;
                    ++e;
                }
                sawOctal = true;
                if (e < n && (s[e] == '_' || s[e] == 'n')) fail("Invalid number");
                if (oct) {
                    double v = 0;
                    while (pos < e) v = v * 8 + (s[pos++] - '0');
                    if (pos < n && (idStart(s[pos]) || s[pos] == '.')) fail("Invalid number");
                    return v;
                }
            }
        }
        std::string buf;
        auto digits = [&] {
            bool lastDigit = false;
            while (pos < n && (isDigit(s[pos]) || s[pos] == '_')) {
                if (s[pos] == '_') {
                    if (!lastDigit || !(pos + 1 < n && isDigit(s[pos + 1]))) fail("Numeric separators are not allowed here");
                    lastDigit = false;
                } else {
                    buf += (char)s[pos];
                    lastDigit = true;
                }
                ++pos;
            }
        };
        digits();
        if (pos < n && s[pos] == '.') {
            buf += '.';
            ++pos;
            digits();
        }
        if (pos < n && (s[pos] == 'e' || s[pos] == 'E')) {
            size_t save = pos;
            std::string ex = "e";
            ++pos;
            if (pos < n && (s[pos] == '+' || s[pos] == '-')) ex += (char)s[pos++];
            if (pos < n && isDigit(s[pos])) {
                buf += ex;
                digits();
            } else
                pos = save;
        }
        if (pos < n && s[pos] == 'n') {
            for (char ch : buf)
                if (ch < '0' || ch > '9') fail("Invalid BigInt literal");
            sawBig = true;
            bigText.assign(buf.begin(), buf.end());
            ++pos;
        }
        if (pos < n && idStart(s[pos])) fail("Invalid number");
        return strtod(buf.c_str(), nullptr);
    }

    uint8_t punct() {
        size_t n = s.size();
        char16_t c = s[pos++];
        auto eat = [&](char16_t x) {
            if (pos < n && s[pos] == x) {
                ++pos;
                return true;
            }
            return false;
        };
        switch (c) {
            case '{': return P_LBrace;
            case '}': return P_RBrace;
            case '(': return P_LParen;
            case ')': return P_RParen;
            case '[': return P_LBrack;
            case ']': return P_RBrack;
            case ';': return P_Semi;
            case ',': return P_Comma;
            case '~': return P_Tilde;
            case ':': return P_Colon;
            case '.':
                if (pos + 1 < n && s[pos] == '.' && s[pos + 1] == '.') {
                    pos += 2;
                    return P_Ellipsis;
                }
                return P_Dot;
            case '<':
                if (eat('<')) return eat('=') ? P_ShlEq : P_Shl;
                return eat('=') ? P_Le : P_Lt;
            case '>':
                if (eat('>')) {
                    if (eat('>')) return eat('=') ? P_UShrEq : P_UShr;
                    return eat('=') ? P_ShrEq : P_Shr;
                }
                return eat('=') ? P_Ge : P_Gt;
            case '=':
                if (eat('=')) return eat('=') ? P_SEq : P_EqEq;
                return eat('>') ? P_Arrow : P_Assign;
            case '!':
                if (eat('=')) return eat('=') ? P_SNe : P_Ne;
                return P_Bang;
            case '+':
                if (eat('+')) return P_Inc;
                return eat('=') ? P_PlusEq : P_Plus;
            case '-':
                if (eat('-')) return P_Dec;
                return eat('=') ? P_MinusEq : P_Minus;
            case '*':
                if (eat('*')) return eat('=') ? P_StarStarEq : P_StarStar;
                return eat('=') ? P_StarEq : P_Star;
            case '/': return eat('=') ? P_SlashEq : P_Slash;
            case '%': return eat('=') ? P_PercentEq : P_Percent;
            case '&':
                if (eat('&')) return eat('=') ? P_AndAndEq : P_AndAnd;
                return eat('=') ? P_AmpEq : P_Amp;
            case '|':
                if (eat('|')) return eat('=') ? P_OrOrEq : P_OrOr;
                return eat('=') ? P_PipeEq : P_Pipe;
            case '^': return eat('=') ? P_CaretEq : P_Caret;
            case '?':
                if (eat('?')) return eat('=') ? P_QQEq : P_QQ;
                if (pos < n && s[pos] == '.' && !(pos + 1 < n && isDigit(s[pos + 1]))) {
                    ++pos;
                    return P_QDot;
                }
                return P_Question;
        }
        fail("Unexpected character");
    }
};

// ============================================================================ parser

class Parser {
public:
    Parser(Vm& v, Ast& a) : vm(v), ast(a), s(*a.src), lx(v, *a.src) {
        const char* names[] = {"var", "let", "const", "function", "class", "if", "else", "for", "while", "do", "return", "break",
                               "continue", "throw", "try", "catch", "finally", "switch", "case", "default", "new", "delete", "typeof",
                               "void", "in", "instanceof", "this", "super", "null", "true", "false", "async", "await", "yield", "of",
                               "get", "set", "static", "extends", "debugger", "import", "export", "with", "enum", "constructor",
                               "arguments", "target", "use strict", "__proto__", "prototype"};
        JSString** slots[] = {&k.var_, &k.let_, &k.const_, &k.function_, &k.class_, &k.if_, &k.else_, &k.for_, &k.while_, &k.do_, &k.return_,
                              &k.break_, &k.continue_, &k.throw_, &k.try_, &k.catch_, &k.finally_, &k.switch_, &k.case_, &k.default_, &k.new_,
                              &k.delete_, &k.typeof_, &k.void_, &k.in_, &k.instanceof_, &k.this_, &k.super_, &k.null_, &k.true_, &k.false_,
                              &k.async_, &k.await_, &k.yield_, &k.of_, &k.get_, &k.set_, &k.static_, &k.extends_, &k.debugger_, &k.import_,
                              &k.export_, &k.with_, &k.enum_, &k.constructor_, &k.arguments_, &k.target_, &k.useStrict, &k.proto_, &k.prototype_};
        for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) *slots[i] = vm.atom(names[i]);
        JSString* res[] = {k.var_, k.const_, k.function_, k.class_, k.if_, k.else_, k.for_, k.while_, k.do_, k.return_, k.break_,
                           k.continue_, k.throw_, k.try_, k.catch_, k.finally_, k.switch_, k.case_, k.default_, k.new_, k.delete_,
                           k.typeof_, k.void_, k.in_, k.instanceof_, k.this_, k.super_, k.null_, k.true_, k.false_, k.debugger_,
                           k.import_, k.export_, k.with_, k.enum_, k.extends_};
        for (JSString* r : res) reserved[r] = true;
    }

    void finish() {
        if (!protoDups.empty()) throw SyntaxErr{"Duplicate __proto__ fields are not allowed in object literals", protoDups[0]->line};
    }
    void program() {
        FunctionInfo& f = ast.fns.emplace_back();
        ast.top = &f;
        fn = &f;
        Scope* g = open(Scope::Global);
        g->fn = &f;
        f.scope = g;
        advance();
        directives(f);
        while (t.type != Tok::End) f.body.push_back(statement());
        f.srcEnd = (uint32_t)s.size();
        close(g);
    }

    // CreateDynamicFunction (20.2.1.1.1): the text is exactly one function, and nothing may follow it.
    void dynamicFunction(int kind) {
        FunctionInfo& f = ast.fns.emplace_back();
        ast.top = &f;
        fn = &f;
        Scope* g = open(Scope::Global);
        g->fn = &f;
        f.scope = g;
        advance();
        uint32_t start = t.pos;
        if (kind & 2) advance();  // async
        advance();                // function
        if (kind & 1) advance();  // *
        JSString* name = t.str;   // anonymous: the function's name, but not a binding its code can see
        advance();
        Node* st = mk(N::ExprStmt);
        st->a = functionRest(name, start, (kind & 2) != 0, (kind & 1) != 0, false);
        if (t.type != Tok::End) unexpected();
        f.body.push_back(st);
        f.srcEnd = (uint32_t)s.size();
        close(g);
    }
    // The parameters on their own, so that text such as a comment opened in them cannot swallow the `) {` that follows.
    void paramsOnly(int kind) {
        FunctionInfo& f = ast.fns.emplace_back();
        ast.top = &f;
        fn = &f;
        Scope* g = open(Scope::Global);
        g->fn = &f;
        f.scope = g;
        advance();
        FunctionInfo* pf = beginFn(nullptr, 0);
        pf->isAsync = (kind & 2) != 0;
        pf->isGenerator = (kind & 1) != 0;
        params(*pf);
        expectP(P_RParen);
        if (t.type != Tok::End) unexpected();
        endFn(pf);
        close(g);
    }

    // Direct eval (ECMA-262 19.2.1.1): the code is compiled as an arrow function placed at the call site, which
    // gives it the caller's variables, `this` and `arguments`. The caller's scopes are rebuilt from the
    // description recorded when the call was compiled.
    void evalProgram(const EvalSite& site) {
        FunctionInfo& top = ast.fns.emplace_back();
        ast.top = &top;
        fn = &top;
        Scope* g = open(Scope::Global);
        g->fn = &top;
        top.scope = g;
        evalVarScope = g;
        for (const EvalScope& es : site.scopes) {
            Scope* sc = g;
            if (es.isFunction) {
                FunctionInfo& f = ast.fns.emplace_back();
                f.parent = fn;
                f.isArrow = es.isArrow;
                f.isMethod = es.isMethod;
                f.isFieldInit = es.isFieldInit;
                f.isClassCtor = es.isClassCtor;
                f.isDerived = es.isDerived;
                f.strict = site.strict;
                fn = &f;
                sc = open(Scope::Function);
                f.scope = sc;
                evalVarScope = sc;  // arrow functions keep their own `var`s too
            } else if (!es.isGlobal) {
                sc = open(es.isWith ? Scope::With : es.isClass ? Scope::Class : Scope::Block);
                if (es.isVarScope) {
                    sc->varScope = true;
                    sc->fn = fn;
                    evalVarScope = sc;
                }
            }
            sc->hasCtx = true;
            ast.evalScopes.push_back(sc);
            for (size_t i = 0; i < es.vars.size(); ++i) {
                Variable* v = newVar(sc, es.vars[i].name, es.vars[i].kind);
                v->isGlobal = false;
                v->captured = true;
                v->slot = es.vars[i].slot;
                v->needsTdz = es.vars[i].tdz;
                v->argIndex = es.vars[i].arg;
                v->declEnd = 0;
                if ((int)i == es.mappedVar) sc->fn->mappedVar = v;
                if ((int)i == es.thisVar) sc->fn->thisVar = v;
                if ((int)i == es.argsVar) sc->fn->argsVar = v;
                if ((int)i == es.newTargetVar) sc->fn->newTargetVar = v;
                if ((int)i == es.ctorVar) sc->fn->ctorVar = v;
                if ((int)i == es.evalVars) sc->fn->evalVars = v;
                if ((int)i == es.withVar) sc->withVar = v;
            }
        }
        declaredNow.clear();
        evalInParams = site.inParams;
        advance();
        FunctionInfo* f = beginFn(nullptr, 0);
        f->isArrow = true;
        f->isEvalBody = true;
        f->strict = site.strict;
        evalScope = f->scope;
        ast.evalFn = f;
        directives(*f);
        while (t.type != Tok::End) f->body.push_back(statement());
        prevEnd = (uint32_t)s.size();
        endFn(f);
        while (scope) close(scope);  // names the eval code did not declare resolve against the caller's scopes
    }

private:
    Scope* evalScope = nullptr;     // the eval code's own top-level scope
    Scope* evalVarScope = nullptr;  // where the caller keeps its `var`s: the nearest function, or the global scope
    // A call written as eval(...) may be a direct eval, so everything it could name has to stay in a context.
    FunctionInfo* paramsOf = nullptr;  // the function whose parameter list is being read
    bool evalInParams = false;         // this eval code was called from a parameter's default value
    void markEval(Node* call) {
        call->flags |= NF_EVAL;
        if (fn == paramsOf) call->flags |= NF_INPARAM;
        for (Scope* sc = scope; sc; sc = sc->parent) sc->hasEval = true;
        FunctionInfo* owner = fn;
        while (owner->isArrow && owner->parent) owner = owner->parent;
        auto force = [&](Variable* FunctionInfo::*slot, JSString* name) {
            if (!(owner->*slot)) {
                Variable& v = ast.vars.emplace_back();
                v.name = name;
                v.kind = VK_PSEUDO;
                v.scope = owner->scope;
                v.declEnd = 0;
                owner->scope->vars.push_back(&v);
                if (slot == &FunctionInfo::argsVar) owner->scope->map[name] = &v;
                owner->*slot = &v;
            }
            (owner->*slot)->captured = true;
        };
        if (!fn->strict && fn->scope && fn->scope->kind == Scope::Function && !fn->evalVars) {
            Variable* v = newVar(fn->scope, vm.atom("%evalvars"), VK_VAR);  // starts out undefined
            v->declEnd = 0;
            v->captured = true;
            declaredNow.pop_back();
            fn->evalVars = v;
        }
        force(&FunctionInfo::thisVar, vm.atom("this"));
        if (owner->scope->kind == Scope::Function && !owner->isArrow) {
            if (!owner->scope->find(k.arguments_)) force(&FunctionInfo::argsVar, k.arguments_);
            force(&FunctionInfo::newTargetVar, vm.atom("new.target"));
            force(&FunctionInfo::ctorVar, vm.atom("%ctor"));
        }
    }
public:

    // An ES module is compiled as `async function (namespace, meta) { ...module code... }`. Its top level is that
    // function's scope: exported variables are read out of the scope by the namespace object, and an imported
    // name is a variable that reads through the namespace of the module it comes from (a live binding).
    void module() {
        FunctionInfo& outer = ast.fns.emplace_back();
        ast.top = &outer;
        fn = &outer;
        Scope* g = open(Scope::Global);
        g->fn = &outer;
        outer.scope = g;
        outer.strict = true;
        lx.noHtmlComments = true;
        advance();
        Node* fnode = mk(N::Function);
        FunctionInfo* f = beginFn(nullptr, 0);
        f->isAsync = true;
        f->strict = true;
        ast.moduleFn = f;
        for (const char* name : {"%ns", "%meta"}) {
            Node* id = mk(N::Ident);
            id->str = vm.atom(name);
            id->var = declare(id->str, VK_PARAM);
            id->var->declEnd = 0;
            f->params.push_back(id);
        }
        declaredNow.clear();
        while (t.type != Tok::End) f->body.push_back(statement());
        for (size_t i = 0; i < ast.exports.size(); ++i) {
            auto& e = ast.exports[i];
            for (size_t j = 0; j < i; ++j)
                if (ast.exports[j].name == e.name) throw SyntaxErr{"Duplicate export of '" + utf16to8(e.name->s) + "'", e.line};
            if (!e.local) continue;
            Variable* v = f->scope->find(e.local);
            if (!v) throw SyntaxErr{"Export '" + utf16to8(e.local->s) + "' is not defined in module", e.line};
            if (v->kind == VK_IMPORT) {  // export { imported }: an indirect export of where it came from
                e.local = nullptr;
                e.request = requestOf(v->importNs);
                e.importName = v->importName;
            } else {
                v->captured = true;  // other modules read it out of the scope
                if (v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS) v->needsTdz = true;
                e.var = v;
            }
        }
        prevEnd = (uint32_t)s.size();
        endFn(f);
        fnode->fn = f;
        Node* st = mk(N::ExprStmt);
        st->a = fnode;
        outer.body.push_back(st);
        outer.srcEnd = (uint32_t)s.size();
        close(g);
    }

private:
    int subDepth = 0;  // how many bodies of if, loops and labels are open
    bool atModuleTop() const { return ast.moduleFn && fn == ast.moduleFn && scope == ast.moduleFn->scope && subDepth == 0; }
    int requestOf(Variable* nsVar) {
        for (size_t i = 0; i < ast.requestVars.size(); ++i)
            if (ast.requestVars[i] == nsVar) return (int)i;
        return -1;
    }
    // Registers a module specifier and returns its index; the first use also creates its namespace variable.
    int request(JSString* spec, JSString* type = nullptr) {  // a ModuleRequest: the specifier and its attributes (16.2.1.3)
        for (size_t i = 0; i < ast.requests.size(); ++i)
            if (ast.requests[i]->s == spec->s && ast.requestTypes[i] == type) return (int)i;
        int idx = (int)ast.requests.size();
        ast.requests.push_back(spec);
        ast.requestTypes.push_back(type);
        Variable* v = newVar(ast.moduleFn->scope, vm.atom("%m" + std::to_string(idx)), VK_LET);
        v->declEnd = 0;
        v->captured = true;
        declaredNow.pop_back();
        ast.requestVars.push_back(v);
        return idx;
    }
    bool isWord(const char* w) const { return t.type == Tok::Ident && !t.esc && t.str->s == std::u16string(w, w + strlen(w)); }
    JSString* moduleName() {  // a name in an import/export list: any word, or a string
        if (t.type != Tok::Ident && t.type != Tok::Str) unexpected();
        if (t.type == Tok::Str) {
            const std::u16string& w = t.str->s;
            for (size_t i = 0; i < w.size(); ++i) {
                bool high = w[i] >= 0xD800 && w[i] <= 0xDBFF, low = w[i] >= 0xDC00 && w[i] <= 0xDFFF;
                if (high && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) ++i;
                else if (high || low) fail("Module export names must be well-formed Unicode");
            }
        }
        JSString* n = vm.atomOf(t.str);
        advance();
        return n;
    }
    JSString* fromClause() {
        if (!isWord("from")) unexpected();
        advance();
        if (t.type != Tok::Str) unexpected();
        JSString* spec = t.str;
        advance();
        withClause();
        return spec;
    }
    // with { key: "value", ... } (16.2.2 WithClause). `type` is the one attribute this host knows; its value is
    // left in lastType for the request that follows.
    JSString* lastType = nullptr;
    void withClause() {
        lastType = nullptr;
        if (!isWord("with")) return;
        uint32_t line = t.line;
        advance();
        expectP(P_LBrace);
        std::vector<JSString*> seen;
        while (!isP(P_RBrace)) {
            if (t.type != Tok::Ident && t.type != Tok::Str) unexpected();
            JSString* key = vm.atomOf(t.str);
            advance();
            expectP(P_Colon);
            if (t.type != Tok::Str) unexpected();
            JSString* value = vm.atomOf(t.str);
            advance();
            for (JSString* k2 : seen)
                if (k2 == key) throw SyntaxErr{"Duplicate import attribute '" + utf16to8(key->s) + "'", line};
            seen.push_back(key);
            if (key->s != u"type") throw SyntaxErr{"Import attribute '" + utf16to8(key->s) + "' is not supported", line};
            lastType = value;
            if (!eatP(P_Comma)) break;
        }
        expectP(P_RBrace);
    }
    Node* importDecl() {
        advance();  // import
        if (t.type == Tok::Str) {  // import "module": only for its side effects
            JSString* spec = t.str;
            advance();
            withClause();
            request(spec, lastType);
            semi();
            return mk(N::Empty);
        }
        std::vector<Variable*> made;
        auto bind = [&](JSString* local, JSString* imported) {
            if (isRes(local)) fail("Unexpected reserved word '" + utf16to8(local->s) + "'");
            if (isEvalOrArguments(local)) fail("Unexpected eval or arguments in strict mode");
            Variable* v = declare(local, VK_CONST);
            v->kind = VK_IMPORT;
            v->declEnd = 0;
            v->captured = true;  // never in a register: every use reads through the source module
            v->importName = imported;
            declaredNow.pop_back();
            made.push_back(v);
        };
        bool more = true;
        if (t.type == Tok::Ident && !isRes(t.str)) {  // import name from: the default export
            JSString* local = t.str;
            advance();
            bind(local, k.default_);
            more = eatP(P_Comma);
        }
        if (more && isP(P_Star)) {  // import * as name
            advance();
            if (!isWord("as")) unexpected();
            advance();
            if (t.type != Tok::Ident) unexpected();
            JSString* local = t.str;
            advance();
            bind(local, nullptr);
        } else if (more && isP(P_LBrace)) {  // import { a, b as c }
            advance();
            while (!isP(P_RBrace)) {
                bool wasString = t.type == Tok::Str;
                JSString* imported = moduleName();
                JSString* local = imported;
                if (isWord("as")) {
                    advance();
                    if (t.type != Tok::Ident) unexpected();
                    local = t.str;
                    advance();
                } else if (wasString)
                    unexpected();
                bind(local, imported);
                if (!eatP(P_Comma)) break;
            }
            expectP(P_RBrace);
        } else if (more)
            unexpected();
        JSString* from = fromClause();
        int idx = request(from, lastType);
        for (Variable* v : made) {
            v->importNs = ast.requestVars[idx];
            if (v->importName) ast.imports.push_back({idx, v->importName});
        }
        semi();
        return mk(N::Empty);
    }
    void addExport(JSString* name, JSString* local, int request, JSString* importName, uint32_t line) {
        ast.exports.push_back({name, local, nullptr, request, importName, line});
    }
    // export default <expression>: the value lives in a hidden variable that stays uninitialised until this runs
    Node* hiddenDefault(Node* value, uint32_t line) {
        JSString* name = vm.atom("%default");
        if (ast.moduleFn->scope->find(name)) throw SyntaxErr{"Duplicate export of 'default'", line};
        Variable* v = newVar(ast.moduleFn->scope, name, VK_LET);
        v->declEnd = 0;
        declaredNow.pop_back();
        nameFn(value, k.default_);
        Node* target = mk(N::Ident);
        target->str = name;
        target->var = v;
        Node* st = mk(N::VarDecl);  // let %default = value: the statement initialises the binding
        st->op = VK_LET;
        Node* decl = mk(N::Declarator);
        decl->a = target;
        decl->b = value;
        st->list.push_back(decl);
        addExport(k.default_, name, -1, nullptr, line);
        return st;
    }
    Node* exportDecl() {
        uint32_t line = t.line;
        advance();  // export
        if (isK(k.default_)) {
            advance();
            bool isAsyncFn = isK(k.async_) && peek().type == Tok::Ident && peek().str == k.function_ && !peek().nl;
            if (isK(k.function_) || isAsyncFn) {  // a declaration (hoisted like any other), named or not
                Node* d = functionDecl(isAsyncFn, true);
                addExport(k.default_, d->var->name, -1, nullptr, line);
                return d;
            }
            if (isK(k.class_)) {
                Tok nx = peek();
                if (nx.type == Tok::Ident && nx.str != k.extends_ && !isRes(nx.str)) {
                    Node* d = classNode(true);
                    addExport(k.default_, d->str, -1, nullptr, line);
                    return d;
                }
                return hiddenDefault(classNode(false), line);
            }
            Node* st = hiddenDefault(assign(true), line);
            semi();
            return st;
        }
        if (isP(P_Star)) {  // export * from, export * as name from
            advance();
            JSString* as = nullptr;
            if (isWord("as")) {
                advance();
                as = moduleName();
            }
            JSString* from = fromClause();
            int idx = request(from, lastType);
            if (as) addExport(as, nullptr, idx, nullptr, line);
            else ast.starRequests.push_back(idx);
            semi();
            return mk(N::Empty);
        }
        if (isP(P_LBrace)) {  // export { a, b as c } [from]
            advance();
            struct Item {
                JSString *local, *exported;
                bool isString;
            };
            std::vector<Item> list;
            while (!isP(P_RBrace)) {
                bool isString = t.type == Tok::Str;
                JSString* local = moduleName();
                JSString* exported = local;
                if (isWord("as")) {
                    advance();
                    exported = moduleName();
                }
                list.push_back({local, exported, isString});
                if (!eatP(P_Comma)) break;
            }
            expectP(P_RBrace);
            if (isWord("from")) {
                JSString* from = fromClause();
                int idx = request(from, lastType);
                for (auto& it : list) addExport(it.exported, nullptr, idx, it.local, line);
            } else
                for (auto& it : list) {
                    if (it.isString || isRes(it.local)) throw SyntaxErr{"Unexpected export name '" + utf16to8(it.local->s) + "'", line};
                    addExport(it.exported, it.local, -1, nullptr, line);
                }
            semi();
            return mk(N::Empty);
        }
        // export var/let/const/function/class: an ordinary declaration whose names are exported
        bool decl = isK(k.var_) || isK(k.let_) || isK(k.const_) || isK(k.function_) || isK(k.class_) ||
                    (isK(k.async_) && peek().type == Tok::Ident && peek().str == k.function_ && !peek().nl);
        if (!decl) unexpected();
        Scope* ms = ast.moduleFn->scope;
        size_t before = ms->vars.size();
        Node* d = statement();
        for (size_t i = before; i < ms->vars.size(); ++i) {
            Variable* v = ms->vars[i];
            if (v->kind == VK_VAR || v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_FUNC || v->kind == VK_CLASS)
                addExport(v->name, v->name, -1, nullptr, line);
        }
        return d;
    }

    struct KW {
        JSString *var_, *let_, *const_, *function_, *class_, *if_, *else_, *for_, *while_, *do_, *return_, *break_, *continue_,
            *throw_, *try_, *catch_, *finally_, *switch_, *case_, *default_, *new_, *delete_, *typeof_, *void_, *in_, *instanceof_,
            *this_, *super_, *null_, *true_, *false_, *async_, *await_, *yield_, *of_, *get_, *set_, *static_, *extends_, *debugger_,
            *import_, *export_, *with_, *enum_, *constructor_, *arguments_, *target_, *useStrict, *proto_, *prototype_;
    } k;
    Vm& vm;
    Ast& ast;
    const std::u16string& s;
    Lexer lx;
    Tok t;
    uint32_t prevEnd = 0;
    Scope* scope = nullptr;
    FunctionInfo* fn = nullptr;
    std::unordered_map<JSString*, bool> reserved;
    std::vector<Variable*> declaredNow;
    JSString* strictWords[9] = {vm.atom("implements"), vm.atom("interface"), vm.atom("let"),       vm.atom("package"), vm.atom("private"),
                                vm.atom("protected"),  vm.atom("public"),    vm.atom("static"),    vm.atom("yield")};
    bool noLetDecl = false;           // a `let` here starts an expression, not a declaration
    bool nextBlockIsCatchBody = false;
    struct Labels {
        std::vector<std::pair<JSString*, bool>> names;  // label, does it name a loop
        int loops = 0, breakables = 0;
    };
    Labels lab;  // of the function being parsed
    std::vector<Labels> labStack;
    bool isStrictRes(JSString* a) const {
        for (JSString* w : strictWords)
            if (w == a) return true;
        return false;
    }
    bool inStaticBlock() const {
        const FunctionInfo* f = fn;
        while (f->isArrow && f->parent) f = f->parent;
        return f->isStaticBlock;
    }
    // An identifier used as a name: a reference, a binding or a label (ECMA-262 13.1.1).
    void checkName(JSString* name, bool escaped) {
        if (escaped && isRes(name)) fail("Keyword must not contain escaped characters");
        bool arrowParam = fn == paramsOf && fn->isArrow && fn->parent;
        if (name == k.yield_ && (fn->strict || fn->isGenerator || (arrowParam && fn->parent->isGenerator))) fail("Unexpected identifier 'yield'");
        if (name == k.await_ && (ast.moduleFn || fn->isAsync || fn->isStaticBlock || (arrowParam && (fn->parent->isAsync || fn->parent->isStaticBlock))))
            fail("Unexpected reserved word 'await'");
        if (fn->strict && isStrictRes(name)) fail("Unexpected strict mode reserved word '" + utf16to8(name->s) + "'");
    }
    bool isEvalOrArguments(JSString* a) const { return a == evalAtom || a == k.arguments_; }
    // Early errors that depend on the whole function: its parameters against its strictness (15.1.1, 15.2.1).
    void checkFunction(FunctionInfo* f) {
        if (f->hasUseStrict && !f->simpleParams) fail("Illegal 'use strict' directive in function with non-simple parameter list");
        if (f->dupParams && (f->strict || !f->simpleParams || f->isArrow || f->isMethod)) fail("Duplicate parameter name not allowed in this context");
        if (f->strict && f->strictNames) fail("Unexpected eval or arguments in strict mode");
        if (f->strict && f->name && !f->isMethod && !f->isClassCtor && (isEvalOrArguments(f->name) || isStrictRes(f->name)))
            fail("Unexpected eval or arguments in strict mode");
    }
    // The body of if, a loop or a label: one statement, where declarations are not allowed (14.6, 14.7, 14.13).
    Node* subStatement(bool ifBody = false, bool labelled = false) {
        struct Depth {
            int& d;
            explicit Depth(int& x) : d(x) { ++d; }
            ~Depth() { --d; }
        } depth(subDepth);
        bool noLet = false;
        if (t.type == Tok::Ident && !t.esc) {
            JSString* w = t.str;
            const char* lexical = "Lexical declaration cannot appear in a single-statement context";
            if (w == k.class_ || w == k.const_ || usingAhead(false)) fail(lexical);
            if (w == k.let_) {
                Tok nx = peek();
                if (nx.type == Tok::Punct && nx.p == P_LBrack) fail(lexical);
                if (!nx.nl && ((nx.type == Tok::Ident && nx.str != k.in_ && nx.str != k.instanceof_) || (nx.type == Tok::Punct && nx.p == P_LBrace))) fail(lexical);
                noLet = true;
            }
            if (w == k.function_) {
                if (fn->strict || !(ifBody || labelled)) fail("In this context a function can only be declared at the top level or inside a block");
                Tok nx = peek();
                if (nx.type == Tok::Punct && nx.p == P_Star) fail("Generators can only be declared at the top level or inside a block");
                if (ifBody) {  // Annex B.3.4: as if the declaration stood in a block of its own
                    Node* b = mk(N::Block);
                    Scope* sc = open(Scope::Block);
                    b->list.push_back(statement());
                    b->scope = sc;
                    close(sc);
                    return b;
                }
            }
            if (w == k.async_) {
                Tok nx = peek();
                if (nx.type == Tok::Ident && nx.str == k.function_ && !nx.nl) fail("Async functions can only be declared at the top level or inside a block");
            }
        }
        noLetDecl = noLet;
        Node* r = statement();
        noLetDecl = false;
        if (!labelled && r->type == N::Labeled) {  // IsLabelledFunction: a labelled function cannot be the body of if or a loop
            Node* inner = r;
            while (inner && inner->type == N::Labeled) inner = inner->a;
            if (inner && inner->type == N::Function) fail("In this context a function can only be declared at the top level or inside a block");
        }
        return r;
    }
    Node* loopBody() {
        ++lab.loops;
        ++lab.breakables;
        Node* b = subStatement();
        --lab.loops;
        --lab.breakables;
        return b;
    }
    JSString* evalAtom = vm.atom("eval");
    JSString* usingAtom = vm.atom("using");

    [[noreturn]] void fail(const std::string& m) { throw SyntaxErr{m, t.line}; }
    [[noreturn]] void unexpected() {
        if (t.type == Tok::End) fail("Unexpected end of input");
        std::string what = utf16to8(std::u16string_view(s).substr(t.pos, std::min<size_t>(t.end - t.pos, 20)));
        fail("Unexpected token '" + what + "'");
    }
    void advance() {
        prevEnd = t.end;
        t = lx.next();
    }
    Tok peek(int n = 1) {
        size_t p = lx.pos;
        uint32_t l = lx.line;
        Tok r;
        for (int i = 0; i < n; ++i) r = lx.next();
        lx.pos = p;
        lx.line = l;
        return r;
    }
    bool isP(uint8_t p) const { return t.type == Tok::Punct && t.p == p; }
    bool eatP(uint8_t p) {
        if (!isP(p)) return false;
        advance();
        return true;
    }
    void expectP(uint8_t p) {
        if (!eatP(p)) unexpected();
    }
    bool isK(JSString* kw) const { return t.type == Tok::Ident && t.str == kw && !t.esc; }
    bool eatK(JSString* kw) {
        if (!isK(kw)) return false;
        advance();
        return true;
    }
    void semi() {
        if (eatP(P_Semi)) return;
        if (isP(P_RBrace) || t.type == Tok::End || t.nl) return;
        unexpected();
    }
    bool isRes(JSString* a) const { return reserved.count(a) != 0; }
    Node* mk(N type) {
        Node& n = ast.nodes.emplace_back();
        n.type = type;
        n.line = t.line;
        n.pos = t.pos;
        return &n;
    }

    // ---------------------------------------------------------------- scopes
    Scope* open(Scope::K kind) {
        Scope& sc = ast.scopes.emplace_back();
        sc.kind = kind;
        sc.parent = scope;
        sc.fn = fn;
        scope = &sc;
        return &sc;
    }
    Scope* fnScope(Scope* sc) {
        while (sc->kind != Scope::Function && sc->kind != Scope::Global && !sc->varScope) sc = sc->parent;
        return sc;
    }
    // ContainsExpression of a parameter (8.5.2): a default value or a computed property name
    static bool patternHasExpr(Node* n) {
        if (!n) return false;
        switch (n->type) {
            case N::AssignPat: return true;
            case N::Rest: return patternHasExpr(n->a);
            case N::ArrayPat:
                for (Node* e : n->list)
                    if (patternHasExpr(e)) return true;
                return false;
            case N::ObjectPat:
                for (Node* e : n->list) {
                    if (e->type == N::Rest) continue;
                    if ((e->flags & NF_COMPUTED) || patternHasExpr(e->b)) return true;
                }
                return false;
            default: return false;
        }
    }
    // FunctionDeclarationInstantiation (10.2.11) step 28: when the parameters hold expressions the body's variables get an
    // environment of their own, so that closures made in the parameter list cannot see them.
    Scope* openBody(FunctionInfo* f) {
        bool any = false;
        for (Node* p : f->params) any = any || patternHasExpr(p);
        if (!any) return nullptr;
        Scope* b = open(Scope::Block);
        b->varScope = true;
        b->fn = f;
        f->bodyScope = b;
        return b;
    }
    Variable* newVar(Scope* sc, JSString* name, uint8_t kind) {
        Variable& v = ast.vars.emplace_back();
        v.name = name;
        v.kind = kind;
        v.scope = sc;
        v.isGlobal = sc->kind == Scope::Global;
        v.declEnd = 0xFFFFFFFF;
        sc->vars.push_back(&v);
        declaredNow.push_back(&v);
        return &v;
    }
    // The name of a function expression is bound in a scope of its own around the function (15.2.5), so a parameter
    // or a declaration of the same name inside the function simply hides it.
    void hideSelf(Scope* sc, JSString* name) {
        if (sc->kind != Scope::Function || !sc->fn->selfVar || sc->fn->selfVar->name != name) return;
        Variable* self = sc->fn->selfVar;
        sc->vars.erase(std::remove(sc->vars.begin(), sc->vars.end(), self), sc->vars.end());
        sc->map.clear();
        sc->fn->selfVar = nullptr;
    }
    bool declSpecial = false;  // the function declaration being declared is a generator or async
    Variable* varShadow = nullptr;  // set by declare: the catch parameter a `var` of the same name writes to
    Variable* declare(JSString* name, uint8_t kind) {
        varShadow = nullptr;
        hideSelf(kind == VK_VAR ? fnScope(scope) : scope, name);
        if (scope->varScope && (kind == VK_LET || kind == VK_CONST || kind == VK_CLASS)) {  // 15.2.1: not a parameter's name
            Variable* pv = scope->parent->find(name);
            if (pv && pv->kind == VK_PARAM) fail("Identifier '" + utf16to8(name->s) + "' has already been declared");
        }
        if (kind == VK_VAR || (kind == VK_FUNC && (scope->kind == Scope::Function || scope->kind == Scope::Global || scope->varScope))) {
            Scope* fs = fnScope(scope);
            if (evalScope && fs == evalScope && !fs->fn->strict) {
                // sloppy direct eval: var and function declarations belong to the code that called eval
                {
                    bool inner = false;
                    for (Scope* sc : ast.evalScopes) {
                        if (inner) {
                            Variable* v = sc->find(name);
                            if (v && (v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS || (v->kind == VK_FUNC && sc->kind == Scope::Block)))
                                fail("Identifier '" + utf16to8(name->s) + "' has already been declared");
                        }
                        if (sc == evalVarScope) inner = true;
                    }
                }
                if (Variable* v = evalVarScope->find(name); v && v->kind != VK_FUNCNAME) {
                    // EvalDeclarationInstantiation (19.2.1.3) step 3: no var over a lexical binding on the way out; for
                    // eval in a default value that includes the parameters and the arguments object
                    bool param = evalInParams && (v->kind == VK_PARAM || (v == evalVarScope->fn->argsVar && !evalVarScope->fn->isArrow));
                    if (v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS || param)
                        fail("Identifier '" + utf16to8(name->s) + "' has already been declared");
                    return v;
                }
                if (evalVarScope->kind == Scope::Global) {
                    Variable* v = newVar(evalVarScope, name, kind);
                    v->declEnd = 0;
                    ast.evalGlobals.push_back(v);
                    return v;
                }
                if (evalVarScope->fn->evalVars) {  // a new variable of the calling function: it lives in an object there
                    bool known = false;
                    for (JSString* n : ast.evalDynVars) known = known || n == name;
                    if (!known) ast.evalDynVars.push_back(name);
                    ast.evalDynTarget = evalVarScope->fn->evalVars;
                    return nullptr;
                }
            }
            if (kind == VK_VAR)
                for (Scope* sc = scope; sc != fs; sc = sc->parent) {
                    Variable* v = sc->find(name);
                    if (v && (v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS || v->kind == VK_FUNC))
                        fail("Identifier '" + utf16to8(name->s) + "' has already been declared");
                    if (v && v->kind == VK_CATCH && !varShadow) varShadow = v;
                    sc->varNames.push_back(name);
                }
            if (Variable* v = fs->find(name)) {
                bool moduleTop = ast.moduleFn && fs == ast.moduleFn->scope;
                if (v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS || v->kind == VK_IMPORT ||
                    (moduleTop && (kind == VK_FUNC || v->kind == VK_FUNC) && (v->kind == VK_VAR || v->kind == VK_FUNC)))
                    fail("Identifier '" + utf16to8(name->s) + "' has already been declared");
                if (kind == VK_FUNC && v->kind == VK_VAR) v->kind = VK_FUNC;
                return v;
            }
            Variable* v = newVar(fs, name, kind);
            v->declEnd = 0;
            if (kind == VK_VAR && name == k.arguments_ && fs->kind == Scope::Function && !fs->fn->isArrow && !fs->fn->argsVar) fs->fn->argsVar = v;
            if (fs->varScope && !(evalScope && fs == evalScope)) {
                // a var named like a parameter is a binding of its own that starts with the parameter's value
                Scope* ps = fs->parent;
                Variable* pv = ps->find(name);
                if (!pv && name == k.arguments_ && !ps->fn->isArrow) {
                    pv = newVar(ps, name, VK_PSEUDO);
                    pv->declEnd = 0;
                    declaredNow.pop_back();
                    ps->fn->argsVar = pv;
                }
                if (pv && (pv->kind == VK_PARAM || pv == ps->fn->argsVar)) v->initFrom = pv;
            }
            return v;
        }
        if (Variable* v = scope->find(name)) {
            if (v->annexB && (kind == VK_LET || kind == VK_CONST || kind == VK_CLASS)) {
                // the name turns out to be lexical here, so the block-level function does not become a var after all
                v->annexB = false;
                v->kind = kind;
                v->declEnd = 0xFFFFFFFF;
                declaredNow.push_back(v);
                return v;
            }
            if (v->kind == VK_PARAM && kind == VK_PARAM) {  // duplicate parameter names: sloppy functions with plain parameters only
                fn->dupParams = true;
                return v;
            }
            // a block may declare the same plain function twice in sloppy code (Annex B.3.2.4)
            if (v->kind == VK_FUNC && kind == VK_FUNC && !fn->strict && scope->kind != Scope::Function && scope->kind != Scope::Global &&
                !scope->varScope && !v->special && !declSpecial)
                return v;
            if (!(v->kind == VK_PSEUDO)) fail("Identifier '" + utf16to8(name->s) + "' has already been declared");
        }
        if (kind == VK_LET || kind == VK_CONST || kind == VK_CLASS || kind == VK_FUNC) {
            for (JSString* n : scope->varNames)
                if (n == name) fail("Identifier '" + utf16to8(name->s) + "' has already been declared");
            if (scope->catchBody && scope->parent && scope->parent->find(name)) fail("Identifier '" + utf16to8(name->s) + "' has already been declared");
        }
        Variable* v = newVar(scope, name, kind);
        // a parameter may not be read before it is bound: its default value can name only earlier parameters
        if (kind == VK_FUNC || kind == VK_CATCH || kind == VK_FUNCNAME) v->declEnd = 0;
        if (kind == VK_FUNC) v->special = declSpecial;
        return v;
    }
    void reference(Node* ident) { scope->refs.push_back({ident, false}); }

    void noteDynamic(Scope* sc, Node* node, bool crossed) {
        if (node->flags & NF_PRIVATE) return;
        Variable* dv = sc->kind == Scope::With ? sc->withVar : sc->kind == Scope::Function && sc->fn->evalVars ? sc->fn->evalVars : nullptr;
        if (!dv) return;
        ast.dyn[node].push_back(dv);
        node->flags |= NF_DYN;
        if (crossed || sc->kind == Scope::Function) dv->captured = true;
    }
    void close(Scope* sc) {
        if (sc->hasEval)
            for (Variable* v : sc->vars) {
                if (v->isGlobal) continue;
                v->captured = true;
                if (v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS) v->needsTdz = true;
            }
        for (auto& [node, crossed] : sc->refs) {
            Variable* v = sc->find(node->str);
            if (!v && sc->kind == Scope::Function && node->str == k.arguments_ && !sc->fn->isArrow && !(node->flags & NF_PRIVATE)) {
                v = newVar(sc, k.arguments_, VK_PSEUDO);
                v->declEnd = 0;
                sc->fn->argsVar = v;
            }
            if (v) {
                node->var = v;
                if (crossed) v->captured = true;
                if (v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS)
                    if (crossed || node->pos < v->declEnd) v->needsTdz = true;
                if (v->kind == VK_PARAM && v->declEnd != 0xFFFFFFFF && node->pos < v->declEnd) v->needsTdz = true;
            } else if (sc->parent) {
                noteDynamic(sc, node, crossed);
                sc->parent->refs.push_back({node, crossed || sc->kind == Scope::Function});
            } else if (node->flags & NF_PRIVATE) {
                throw SyntaxErr{"Private field '" + utf16to8(node->str->s) + "' must be declared in an enclosing class", node->line};
            } else
                noteDynamic(sc, node, crossed);
        }
        sc->refs.clear();
        scope = sc->parent;
    }

    Node* identRef(JSString* name) {
        Node* n = mk(N::Ident);
        n->str = name;
        reference(n);
        return n;
    }

    // `this`, `new.target`: inside arrows they belong to the nearest non-arrow function.
    Variable* pseudo(Variable* FunctionInfo::*slot, const char* name) {
        FunctionInfo* f = fn;
        bool crossed = false;
        while (f->isArrow && f->parent) {
            f = f->parent;
            crossed = true;
        }
        if (!crossed) return nullptr;
        if (!(f->*slot)) {
            Variable& v = ast.vars.emplace_back();
            v.name = vm.atom(name);
            v.kind = VK_PSEUDO;
            v.scope = f->scope;
            v.declEnd = 0;
            f->scope->vars.push_back(&v);
            f->*slot = &v;
        }
        (f->*slot)->captured = true;
        return f->*slot;
    }

    void finishDecl(size_t mark) {
        for (size_t i = mark; i < declaredNow.size(); ++i)
            if (declaredNow[i]->declEnd == 0xFFFFFFFF) declaredNow[i]->declEnd = prevEnd;
        declaredNow.resize(mark);
    }

    // ---------------------------------------------------------------- statements
    void directives(FunctionInfo& f) {
        bool octal = false;
        while (t.type == Tok::Str) {
            Tok nx = peek();
            if (!(nx.type == Tok::End || nx.nl || (nx.type == Tok::Punct && (nx.p == P_Semi || nx.p == P_RBrace)))) break;
            if (t.oct) octal = true;
            if (t.str == k.useStrict && !t.esc && t.end - t.pos == 12) f.strict = f.hasUseStrict = true;
            if (f.strict && octal) fail("Octal escape sequences are not allowed in strict mode");  // also in a directive before "use strict"
            f.body.push_back(statement());
        }
    }

    Node* statement() {
        bool letIsName = noLetDecl;
        noLetDecl = false;
        if (isP(P_LBrace)) return block(true);
        if (isP(P_Semi)) {
            Node* e = mk(N::Empty);
            advance();
            return e;
        }
        if (t.type == Tok::Ident && t.esc && !isRes(t.str)) {
            Tok nx = peek();
            if (nx.type == Tok::Punct && nx.p == P_Colon) return labelled(t.str);
        }
        if (t.type == Tok::Ident && !t.esc) {
            JSString* w = t.str;
            if (int u = usingAhead(false)) {
                Node* d = usingDecl(u == 2, true);
                requireInitializers(d);
                semi();
                return d;
            }
            if (w == k.var_ || w == k.const_) {
                Node* d = varDecl(w == k.var_ ? VK_VAR : VK_CONST, true);
                requireInitializers(d);
                semi();
                return d;
            }
            if (w == k.let_ && !letIsName) {
                Tok nx = peek();
                if ((nx.type == Tok::Ident && nx.str != k.in_ && nx.str != k.instanceof_) ||
                    (nx.type == Tok::Punct && (nx.p == P_LBrack || nx.p == P_LBrace))) {
                    Node* d = varDecl(VK_LET, true);
                    requireInitializers(d);
                    semi();
                    return d;
                }
            }
            if (w == k.function_) return functionDecl(false);
            if (w == k.async_) {
                Tok nx = peek();
                if (nx.type == Tok::Ident && nx.str == k.function_ && !nx.nl) return functionDecl(true);
            }
            if (w == k.class_) return classNode(true);
            if (w == k.if_) return ifStmt();
            if (w == k.for_) return forStmt();
            if (w == k.while_) {
                Node* n = mk(N::While);
                advance();
                expectP(P_LParen);
                n->a = expression();
                expectP(P_RParen);
                n->d = loopBody();
                return n;
            }
            if (w == k.do_) {
                Node* n = mk(N::DoWhile);
                advance();
                n->d = loopBody();
                if (!eatK(k.while_)) unexpected();
                expectP(P_LParen);
                n->a = expression();
                expectP(P_RParen);
                eatP(P_Semi);
                return n;
            }
            if (w == k.return_) {
                if (fn == ast.top || fn->isEvalBody || fn == ast.moduleFn || fn->isStaticBlock) fail("Illegal return statement");
                Node* n = mk(N::Return);
                advance();
                if (!isP(P_Semi) && !isP(P_RBrace) && t.type != Tok::End && !t.nl) n->a = expression();
                semi();
                return n;
            }
            if (w == k.break_ || w == k.continue_) {
                Node* n = mk(w == k.break_ ? N::Break : N::Continue);
                advance();
                if (t.type == Tok::Ident && !t.nl && !isRes(t.str)) {
                    n->str = t.str;
                    bool found = false, loop = false;
                    for (auto& l : lab.names)
                        if (l.first == t.str) found = true, loop = l.second;
                    if (!found) fail("Undefined label '" + utf16to8(t.str->s) + "'");
                    if (w == k.continue_ && !loop) fail("Illegal continue statement: '" + utf16to8(t.str->s) + "' does not denote an iteration statement");
                    advance();
                } else if (w == k.continue_ ? lab.loops == 0 : lab.breakables == 0)
                    fail(w == k.continue_ ? "Illegal continue statement: no surrounding iteration statement" : "Illegal break statement");
                semi();
                return n;
            }
            if (w == k.throw_) {
                Node* n = mk(N::Throw);
                advance();
                if (t.nl) fail("Illegal newline after throw");
                n->a = expression();
                semi();
                return n;
            }
            if (w == k.try_) return tryStmt();
            if (w == k.switch_) return switchStmt();
            if (w == k.debugger_) {
                Node* n = mk(N::Debugger);
                advance();
                semi();
                return n;
            }
            if (w == k.import_) {
                Tok nx = peek();
                if (!(nx.type == Tok::Punct && (nx.p == P_LParen || nx.p == P_Dot))) {
                    if (!atModuleTop()) fail("Cannot use import statement outside a module");
                    return importDecl();
                }
            }
            if (w == k.export_) {
                if (!atModuleTop()) fail("Unexpected token 'export'");
                return exportDecl();
            }
            if (w == k.with_) {  // with (object) statement: names are looked up on the object first (14.11)
                if (fn->strict) fail("Strict mode code may not include a with statement");
                Node* n = mk(N::With);
                advance();
                expectP(P_LParen);
                n->a = expression();
                expectP(P_RParen);
                Scope* sc = open(Scope::With);
                Variable* wv = newVar(sc, vm.atom("%with"), VK_LET);
                wv->declEnd = 0;
                declaredNow.pop_back();
                sc->withVar = wv;
                n->scope = sc;
                n->var = wv;
                n->d = subStatement();
                close(sc);
                return n;
            }
            if (!isRes(w)) {
                Tok nx = peek();
                if (nx.type == Tok::Punct && nx.p == P_Colon) return labelled(w);
            }
        }
        Node* e = mk(N::ExprStmt);
        e->a = expression();
        semi();
        return e;
    }
    Node* labelled(JSString* w) {  // at the label's name; the colon follows
        checkName(w, t.esc);
        for (auto& l : lab.names)
            if (l.first == w) fail("Label '" + utf16to8(w->s) + "' has already been declared");
        Node* n = mk(N::Labeled);
        n->str = w;
        advance();
        advance();
        // the label names a loop when the statement it leads to, past any further labels, is one
        Tok look = t;
        for (int off = 0; off < 16 && look.type == Tok::Ident && !look.esc && !isRes(look.str); off += 2) {
            Tok colon = peek(off + 1);
            if (!(colon.type == Tok::Punct && colon.p == P_Colon)) break;
            look = peek(off + 2);
        }
        bool loop = look.type == Tok::Ident && !look.esc && (look.str == k.for_ || look.str == k.while_ || look.str == k.do_);
        lab.names.push_back({w, loop});
        n->a = subStatement(false, true);
        lab.names.pop_back();
        return n;
    }

    Node* block(bool newScope) {
        Node* b = mk(N::Block);
        expectP(P_LBrace);
        Scope* sc = newScope ? open(Scope::Block) : nullptr;
        if (sc) sc->catchBody = nextBlockIsCatchBody;
        nextBlockIsCatchBody = false;
        while (!isP(P_RBrace)) {
            if (t.type == Tok::End) unexpected();
            b->list.push_back(statement());
        }
        advance();
        if (sc) {
            b->scope = sc;
            close(sc);
        }
        return b;
    }

    // Is this the start of a `using` (1) or `await using` (2) declaration? (14.3.1; `using` is a name anywhere else)
    int usingAhead(bool forHead) {
        if (t.type != Tok::Ident || t.esc) return 0;
        if (t.str == usingAtom) {
            Tok nx = peek();
            if (nx.type != Tok::Ident || nx.nl || isRes(nx.str)) return 0;
            if (forHead && nx.str == k.of_ && peek(2).p != P_Assign) return 0;
            return 1;
        }
        if (t.str == k.await_ && (fn->isAsync || fn->isStaticBlock)) {
            Tok nx = peek();
            if (nx.type != Tok::Ident || nx.str != usingAtom || nx.nl || nx.esc) return 0;
            Tok n2 = peek(2);
            if (n2.type != Tok::Ident || n2.nl || isRes(n2.str)) return 0;
            return fn->isAsync ? 2 : 0;
        }
        return 0;
    }
    Node* usingDecl(bool isAwait, bool in) {
        if (scope->kind == Scope::Global || (fn->isEvalBody && scope == fn->scope)) fail("A using declaration is not allowed at the top level of a script");
        if (scope->isSwitch) fail("A using declaration is not allowed directly in a switch case");
        Node* d = mk(N::VarDecl);
        d->op = VK_CONST;
        d->flags |= NF_USING | (isAwait ? NF_AWAIT : 0);
        if (isAwait) {
            advance();  // await
            if (fn == ast.moduleFn) ast.hasTLA = true;
        }
        advance();  // using
        do {
            size_t mark = declaredNow.size();
            Node* decl = mk(N::Declarator);
            if (isP(P_LBrack) || isP(P_LBrace)) fail("A using declaration takes names, not patterns");
            decl->a = bindingIdent(VK_CONST);
            if (eatP(P_Assign)) {
                decl->b = assign(in);
                nameFn(decl->b, decl->a->str);
            }
            finishDecl(mark);
            d->list.push_back(decl);
        } while (eatP(P_Comma));
        if (scope->usingKind < (isAwait ? 2 : 1)) scope->usingKind = isAwait ? 2 : 1;
        return d;
    }
    void requireInitializers(Node* d) {
        for (Node* decl : d->list) {
            if (decl->b) continue;
            if (d->op == VK_CONST) fail("Missing initializer in const declaration");
            if (decl->a->type != N::Ident) fail("Missing initializer in destructuring declaration");
        }
    }
    Node* varDecl(uint8_t kind, bool in) {
        Node* d = mk(N::VarDecl);
        d->op = kind;
        advance();
        do {
            size_t mark = declaredNow.size();
            Node* decl = mk(N::Declarator);
            decl->a = bindingTarget(kind);
            if (eatP(P_Assign)) {
                decl->b = assign(in);
                nameFn(decl->b, decl->a->type == N::Ident ? decl->a->str : nullptr);
            }
            finishDecl(mark);
            d->list.push_back(decl);
        } while (eatP(P_Comma));
        return d;
    }

    // Annex B.3.3: in sloppy code a function declared in a block is also a `var` of the enclosing function (or
    // the global object), unless a let, const, class or parameter of that name stands in the way.
    bool annexBDynamic = false;  // set by annexBVar when the var it wants lives in the caller's eval-made variables
    Variable* annexBVar(JSString* name) {
        if (fn->strict) return nullptr;
        Scope* fs = fnScope(scope);
        if (scope == fs) return nullptr;
        if (name == k.arguments_ && fs->kind != Scope::Global && fs->fn && !fs->fn->isArrow && !fs->fn->isEvalBody) return nullptr;  // the arguments object keeps its name
        for (Scope* sc = scope->parent; sc && sc != fs; sc = sc->parent) {
            Variable* v = sc->find(name);
            if (v && (v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS || v->kind == VK_FUNC)) return nullptr;
        }
        if (evalScope && fs == evalScope) {  // eval code (B.3.2.3): the var belongs to the code that called eval
            bool inner = false;  // a binding of that name in a block between the call and its function blocks the copy
            for (Scope* sc : ast.evalScopes) {
                if (inner && sc->find(name)) return nullptr;
                if (sc == evalVarScope) inner = true;
            }
            if (Variable* v = evalVarScope->find(name)) return v->kind == VK_LET || v->kind == VK_CONST || v->kind == VK_CLASS ? nullptr : v;
            if (evalVarScope->kind == Scope::Global) {
                Variable* v = newVar(evalVarScope, name, VK_VAR);
                v->declEnd = 0;
                ast.evalGlobals.push_back(v);
                return v;
            }
            if (evalVarScope->fn->evalVars) {  // a new variable of the calling function, kept in its object of eval-made vars
                bool known = false;
                for (JSString* n : ast.evalDynVars) known = known || n == name;
                if (!known) ast.evalDynVars.push_back(name);
                ast.evalDynTarget = evalVarScope->fn->evalVars;
                annexBDynamic = true;
            }
            return nullptr;
        }
        hideSelf(fs, name);
        if (fs->varScope) {  // B.3.2.1: not when a parameter has that name
            Variable* pv = fs->parent->find(name);
            if (pv && pv->kind == VK_PARAM) return nullptr;
        }
        if (Variable* v = fs->find(name)) return v->kind == VK_VAR || (v->kind == VK_FUNC && v->scope == fs) ? v : nullptr;
        Variable* v = newVar(fs, name, VK_VAR);
        v->declEnd = 0;
        v->annexB = true;
        return v;
    }

    Node* bindingIdent(uint8_t kind) {
        if (t.type != Tok::Ident || isRes(t.str)) unexpected();
        checkBinding(t.str, t.esc, kind);
        Node* n = mk(N::Ident);
        n->str = t.str;
        n->var = declare(t.str, kind);
        if (kind == VK_VAR && varShadow) n->var = varShadow;
        bound(n, kind);
        advance();
        return n;
    }

    // A `var` declared inside `with` assigns through the object, and one made by eval is found at run time.
    void bound(Node* n, uint8_t kind) {
        if (!n->var) {
            reference(n);
            return;
        }
        if (kind != VK_VAR) return;
        for (Scope* sc = scope; sc && sc != n->var->scope; sc = sc->parent)
            if (sc->kind == Scope::With && sc->withVar) {
                ast.dyn[n].push_back(sc->withVar);
                n->flags |= NF_DYN;
            }
    }
    void checkBinding(JSString* name, bool escaped, uint8_t kind) {
        checkName(name, escaped);
        if (isEvalOrArguments(name)) {
            if (fn->strict) fail("Unexpected eval or arguments in strict mode");
            if (kind == VK_PARAM) fn->strictNames = true;
        } else if (kind == VK_PARAM && isStrictRes(name))
            fn->strictNames = true;
        if ((kind == VK_LET || kind == VK_CONST) && name == k.let_) fail("let is disallowed as a lexically bound name");
    }
    Node* bindingTarget(uint8_t kind) {
        if (isP(P_LBrack)) {
            Node* p = mk(N::ArrayPat);
            advance();
            while (!isP(P_RBrack)) {
                if (isP(P_Comma)) {
                    p->list.push_back(mk(N::Hole));
                    advance();
                    continue;
                }
                if (isP(P_Ellipsis)) {
                    Node* r = mk(N::Rest);
                    advance();
                    r->a = bindingTarget(kind);
                    p->list.push_back(r);
                    if (!isP(P_RBrack)) fail("Rest element must be last element");
                } else {
                    p->list.push_back(bindingElement(kind));
                }
                if (!isP(P_RBrack)) expectP(P_Comma);
            }
            advance();
            return p;
        }
        if (isP(P_LBrace)) {
            Node* p = mk(N::ObjectPat);
            advance();
            while (!isP(P_RBrace)) {
                if (isP(P_Ellipsis)) {
                    Node* r = mk(N::Rest);
                    advance();
                    r->a = bindingIdent(kind);
                    p->list.push_back(r);
                    if (!isP(P_RBrace)) fail("Rest element must be last element");
                } else {
                    Node* pr = mk(N::Property);
                    bool isIdent = t.type == Tok::Ident;
                    bool nameEscaped = t.esc;
                    JSString* name = t.str;
                    pr->a = propertyName(pr);
                    if (eatP(P_Colon)) {
                        pr->b = bindingElement(kind);
                    } else {
                        if (!isIdent || isRes(name)) unexpected();
                        checkBinding(name, nameEscaped, kind);
                        Node* id = mk(N::Ident);
                        id->str = name;
                        id->var = declare(name, kind);
                        if (kind == VK_VAR && varShadow) id->var = varShadow;
                        bound(id, kind);
                        pr->flags |= NF_SHORTHAND;
                        if (eatP(P_Assign)) {
                            Node* ap = mk(N::AssignPat);
                            ap->a = id;
                            ap->b = assign();
                            nameFn(ap->b, name);
                            pr->b = ap;
                        } else
                            pr->b = id;
                    }
                    p->list.push_back(pr);
                }
                if (!isP(P_RBrace)) expectP(P_Comma);
            }
            advance();
            return p;
        }
        return bindingIdent(kind);
    }

    Node* bindingElement(uint8_t kind) {
        Node* target = bindingTarget(kind);
        if (eatP(P_Assign)) {
            Node* ap = mk(N::AssignPat);
            ap->a = target;
            ap->b = assign();
            nameFn(ap->b, target->type == N::Ident ? target->str : nullptr);
            return ap;
        }
        return target;
    }

    Node* ifStmt() {
        Node* n = mk(N::If);
        advance();
        expectP(P_LParen);
        n->a = expression();
        expectP(P_RParen);
        n->b = subStatement(true);
        if (eatK(k.else_)) n->c = subStatement(true);
        return n;
    }

    Node* forStmt() {
        Node* n = mk(N::For);
        advance();
        bool isAwait = false;
        if (isK(k.await_) && fn->isAsync) {  // for await (x of asyncIterable)
            isAwait = true;
            if (fn == ast.moduleFn) ast.hasTLA = true;
            advance();
        }
        expectP(P_LParen);
        Scope* sc = open(Scope::For);
        n->scope = sc;
        Node* init = nullptr;
        size_t mark = declaredNow.size();
        bool isDecl = false;
        bool asyncOf = isK(k.async_) && !t.esc && peek().type == Tok::Ident && peek().str == k.of_;
        if (isP(P_Semi)) {
        } else if (int u = usingAhead(true)) {
            init = usingDecl(u == 2, false);
            isDecl = true;
        } else if (isK(k.var_) || isK(k.const_) || (isK(k.let_) && peek().type != Tok::Punct && !(peek().type == Tok::Ident && peek().str == k.in_)) ||
                   (isK(k.let_) && (peek().p == P_LBrack || peek().p == P_LBrace))) {
            init = varDecl(isK(k.var_) ? VK_VAR : isK(k.let_) ? VK_LET : VK_CONST, false);
            isDecl = true;
        } else {
            init = expression(false);
        }
        if (isAwait && !(init && isK(k.of_))) fail("for await needs the form: for await (x of y)");
        if (init && (isK(k.of_) || isK(k.in_))) {
            n->type = isK(k.of_) ? N::ForOf : N::ForIn;
            if (isAwait) n->flags |= NF_AWAIT;
            if (asyncOf && !isAwait && n->type == N::ForOf && init->type == N::Ident) fail("The left-hand side of a for-of loop may not be 'async'");
            advance();
            if (isDecl) {
                finishDecl(mark);
                if ((init->flags & NF_USING) && n->type == N::ForIn) fail("A using declaration cannot be the variable of a for-in loop");
                if (init->list.size() != 1) fail("Invalid left-hand side in for-loop");
                if (init->list[0]->b) {  // only `for (var x = 1 in obj)` in sloppy code may have an initializer (B.3.5)
                    bool legacy = n->type == N::ForIn && init->op == VK_VAR && !fn->strict && init->list[0]->a->type == N::Ident;
                    if (!legacy) fail("The variable of a for-in or for-of loop may not have an initializer");
                }
                // the loop variable is initialised by the loop itself
                for (Variable* v : sc->vars) v->declEnd = 0;
            } else {
                if (init->type == N::Assign) fail("Invalid left-hand side in for-loop");
                init = toPattern(init);
            }
            n->a = init;
            n->b = n->type == N::ForOf ? assign() : expression();
            if (isDecl && init->op != VK_VAR)
                for (Variable* v : sc->vars)
                    if (v->kind == VK_LET || v->kind == VK_CONST) v->declEnd = prevEnd;
            expectP(P_RParen);
            n->d = loopBody();
            close(sc);
            return n;
        }
        finishDecl(mark);
        if (init && isDecl) requireInitializers(init);
        if (init && !isDecl) {
            Node* es = mk(N::ExprStmt);
            es->a = init;
            init = es;
        }
        n->a = init;
        expectP(P_Semi);
        if (!isP(P_Semi)) n->b = expression();
        expectP(P_Semi);
        if (!isP(P_RParen)) n->c = expression();
        expectP(P_RParen);
        n->d = loopBody();
        close(sc);
        return n;
    }

    Node* tryStmt() {
        Node* n = mk(N::Try);
        advance();
        n->a = block(true);
        if (eatK(k.catch_)) {
            Scope* sc = open(Scope::Catch);
            n->scope = sc;
            if (eatP(P_LParen)) {
                size_t mark = declaredNow.size();
                n->b = bindingTarget(VK_CATCH);
                if (n->b->type != N::Ident)
                    for (Variable* v : sc->vars)
                        if (v->kind == VK_CATCH) v->kind = VK_LET;
                finishDecl(mark);
                expectP(P_RParen);
            }
            nextBlockIsCatchBody = true;
            n->c = block(true);
            close(sc);
        }
        if (eatK(k.finally_)) n->d = block(true);
        if (!n->c && !n->d) fail("Missing catch or finally after try");
        return n;
    }

    Node* switchStmt() {
        Node* n = mk(N::Switch);
        advance();
        expectP(P_LParen);
        n->a = expression();
        expectP(P_RParen);
        expectP(P_LBrace);
        Scope* sc = open(Scope::Block);
        sc->isSwitch = true;
        n->scope = sc;
        while (!isP(P_RBrace)) {
            Node* c = mk(N::Case);
            if (eatK(k.case_)) c->a = expression();
            else if (!eatK(k.default_)) unexpected();
            else {
                for (Node* other : n->list)
                    if (other->flags & 1) fail("More than one default clause in switch statement");
                c->flags = 1;
            }
            expectP(P_Colon);
            ++lab.breakables;
            while (!isP(P_RBrace) && !isK(k.case_) && !isK(k.default_)) {
                if (t.type == Tok::End) unexpected();
                c->list.push_back(statement());
            }
            --lab.breakables;
            n->list.push_back(c);
        }
        advance();
        close(sc);
        return n;
    }

    // ---------------------------------------------------------------- functions
    // anonymousDefault: `export default function () {}` is a declaration too, bound to a hidden name
    Node* functionDecl(bool isAsync, bool anonymousDefault = false) {
        uint32_t start = t.pos;
        uint32_t line = t.line;
        if (isAsync) advance();  // async
        advance();               // function
        bool gen = eatP(P_Star);
        JSString* name;
        Variable* v;
        if (anonymousDefault && isP(P_LParen)) {
            name = k.default_;
            v = declare(vm.atom("%default"), VK_FUNC);
        } else {
            if (t.type != Tok::Ident || isRes(t.str)) unexpected();
            name = t.str;
            checkBinding(name, t.esc, VK_FUNC);
            declSpecial = isAsync || gen;
            v = declare(name, VK_FUNC);
            declSpecial = false;
            advance();
        }
        annexBDynamic = false;
        Variable* outer = !isAsync && !gen && !anonymousDefault ? annexBVar(name) : nullptr;
        bool copyToEvalVars = annexBDynamic;
        Node* n = functionRest(name, start, isAsync, gen, false);
        if (copyToEvalVars) n->flags |= NF_DYN;
        n->fn->isDecl = true;
        n->fn->line = line;
        n->var = v;
        n->var2 = outer != v ? outer : nullptr;
        scope->funcDecls.push_back(n);
        return n;
    }

    FunctionInfo* beginFn(JSString* name, uint32_t start) {
        FunctionInfo& f = ast.fns.emplace_back();
        f.name = name;
        f.srcStart = start;
        f.line = t.line;
        f.parent = fn;
        f.strict = fn->strict;
        fn = &f;
        labStack.push_back(std::move(lab));
        lab = Labels{};
        Scope* fs = open(Scope::Function);
        fs->fn = &f;
        f.scope = fs;
        return &f;
    }
    void endFn(FunctionInfo* f) {
        lab = std::move(labStack.back());
        labStack.pop_back();
        f->srcEnd = prevEnd;
        close(f->scope);
        fn = f->parent;
    }

    void params(FunctionInfo& f) {
        FunctionInfo* outerParams = paramsOf;
        paramsOf = &f;
        paramList(f);
        paramsOf = outerParams;
    }
    void paramList(FunctionInfo& f) {
        bool simple = true, counting = true;
        while (!isP(P_RParen)) {
            size_t mark = declaredNow.size();
            if (isP(P_Ellipsis)) {
                Node* r = mk(N::Rest);
                advance();
                r->a = bindingTarget(VK_PARAM);
                f.params.push_back(r);
                simple = false;
                f.simpleParams = false;
                if (!isP(P_RParen)) fail("Rest parameter must be last formal parameter");
            } else {
                Node* p = bindingElement(VK_PARAM);
                if (p->type != N::Ident) simple = false, f.simpleParams = false;
                if (p->type == N::AssignPat) counting = false;
                if (counting) f.length = (int)f.params.size() + 1;
                f.params.push_back(p);
            }
            finishDecl(mark);
            if (!isP(P_RParen)) expectP(P_Comma);
        }
    }

    void body(FunctionInfo& f) {
        expectP(P_LBrace);
        directives(f);
        while (!isP(P_RBrace)) {
            if (t.type == Tok::End) unexpected();
            f.body.push_back(statement());
        }
        advance();
    }

    // Parses "(params) { body }" for a function whose keyword/name have been consumed.
    Node* functionRest(JSString* name, uint32_t start, bool isAsync, bool gen, bool isExpr, uint8_t methodKind = 0) {
        Node* n = mk(N::Function);
        FunctionInfo* f = beginFn(name, start);
        f->isAsync = isAsync;
        f->isGenerator = gen;
        if (methodKind) {
            f->isMethod = true;
            f->isGetter = methodKind == 2;
            f->isSetter = methodKind == 3;
        }
        if (isExpr && name) {
            f->selfVar = newVar(f->scope, name, VK_FUNCNAME);
            f->selfVar->declEnd = 0;
        }
        expectP(P_LParen);
        params(*f);
        expectP(P_RParen);
        if (methodKind == 2 && !f->params.empty()) fail("Getter must not have any formal parameters");
        if (methodKind == 3 && (f->params.size() != 1 || f->params[0]->type == N::Rest)) fail("Setter must have exactly one formal parameter");
        Scope* bodyScope = openBody(f);
        body(*f);
        if (bodyScope) close(bodyScope);
        checkFunction(f);
        endFn(f);
        n->fn = f;
        n->str = name;
        return n;
    }

    Node* arrow(bool isAsync, uint32_t start) {
        Node* n = mk(N::Function);
        FunctionInfo* f = beginFn(nullptr, start);
        f->isArrow = true;
        f->isAsync = isAsync;
        if (isP(P_LParen)) {
            advance();
            params(*f);
            expectP(P_RParen);
        } else {
            FunctionInfo* outerParams = paramsOf;
            paramsOf = f;
            f->params.push_back(bindingIdent(VK_PARAM));
            paramsOf = outerParams;
            f->params.back()->var->declEnd = 0;
            f->length = 1;
        }
        if (!isP(P_Arrow) || t.nl) unexpected();
        advance();
        if (isP(P_LBrace)) {
            Scope* bodyScope = openBody(f);
            body(*f);
            if (bodyScope) close(bodyScope);
        } else {
            Node* r = mk(N::Return);
            r->a = assign();
            f->body.push_back(r);
            f->exprBody = true;
        }
        checkFunction(f);
        endFn(f);
        n->fn = f;
        return n;
    }

    // Decides whether the parenthesis at `p` starts an arrow function's parameter list.
    bool arrowAhead(size_t p) {
        size_t n = s.size(), i = p + 1;
        while (i < n && (isWs(s[i]) || isLt(s[i]))) ++i;
        if (i >= n) return false;
        char16_t f = s[i];
        if (!(idStart(f) || f == ')' || f == '[' || f == '{' || f == '.' || f == '/')) return false;
        if (s.compare(i, 8, u"function") == 0 && !(i + 8 < n && idPart(s[i + 8]))) return false;
        int depth = 0;
        char16_t prev = '(';
        for (i = p; i < n; ++i) {
            char16_t c = s[i];
            if (c == '(' || c == '[' || c == '{') {
                ++depth;
                prev = c;
            } else if (c == ')' || c == ']' || c == '}') {
                if (--depth == 0) {
                    size_t j = i + 1;
                    for (;;) {
                        while (j < n && isWs(s[j])) ++j;
                        if (j + 1 < n && s[j] == '/' && s[j + 1] == '*') {
                            size_t e = s.find(u"*/", j + 2);
                            if (e == std::u16string::npos) return false;
                            j = e + 2;
                        } else
                            break;
                    }
                    return j + 1 < n && s[j] == '=' && s[j + 1] == '>';
                }
                prev = c;
            } else if (c == '"' || c == '\'') {
                for (++i; i < n && s[i] != c; ++i)
                    if (s[i] == '\\') ++i;
                prev = 'a';
            } else if (c == '`') {
                i = skipTemplate(i);
                prev = 'a';
            } else if (c == '/') {
                if (i + 1 < n && s[i + 1] == '/') {
                    while (i < n && !isLt(s[i])) ++i;
                } else if (i + 1 < n && s[i + 1] == '*') {
                    size_t e = s.find(u"*/", i + 2);
                    if (e == std::u16string::npos) return false;
                    i = e + 1;
                } else if (prev == '(' || prev == ',' || prev == '=' || prev == ':' || prev == '[' || prev == '!' || prev == '&' ||
                           prev == '|' || prev == '?' || prev == '{' || prev == '}' || prev == ';' || prev == '+' || prev == '-' ||
                           prev == '*' || prev == '%' || prev == '<' || prev == '>' || prev == '~' || prev == '^') {
                    bool cls = false;
                    for (++i; i < n && !isLt(s[i]); ++i) {
                        if (s[i] == '\\') ++i;
                        else if (s[i] == '[') cls = true;
                        else if (s[i] == ']') cls = false;
                        else if (s[i] == '/' && !cls) break;
                    }
                    prev = 'a';
                } else
                    prev = c;
            } else if (!isWs(c) && !isLt(c)) {
                prev = c;
            }
        }
        return false;
    }
    size_t skipTemplate(size_t i) {  // s[i] == '`'; returns the index of the closing backtick
        size_t n = s.size();
        for (++i; i < n; ++i) {
            if (s[i] == '\\') ++i;
            else if (s[i] == '`') return i;
            else if (s[i] == '$' && i + 1 < n && s[i + 1] == '{') {
                int depth = 1;
                for (i += 2; i < n && depth > 0; ++i) {
                    char16_t c = s[i];
                    if (c == '{') ++depth;
                    else if (c == '}') --depth;
                    else if (c == '`') i = skipTemplate(i);
                    else if (c == '"' || c == '\'') {
                        for (++i; i < n && s[i] != c; ++i)
                            if (s[i] == '\\') ++i;
                    }
                }
                --i;
            }
        }
        return n;
    }

    // The name a method defined with a plain key gets: the key, with "get " or "set " in front for an accessor.
    void nameMethod(Node* fnNode, JSString* key, uint8_t kind) {
        if (!key) return;
        fnNode->fn->name = kind == PK_GET ? vm.atom(u"get " + key->s) : kind == PK_SET ? vm.atom(u"set " + key->s) : key;
    }
    static bool anonymousFn(Node* value) {
        return value && ((value->type == N::Function && !value->fn->name) || (value->type == N::Class && !value->str && !value->str2));
    }
    void nameFn(Node* value, JSString* name) {
        if (!value || !name) return;
        if (value->type == N::Function && !value->fn->name) value->fn->name = name;
        else if (value->type == N::Class && !value->str) value->str2 = name;
    }

    // ---------------------------------------------------------------- classes
    Node* classNode(bool isDecl) {
        Node* n = mk(N::Class);
        advance();  // class
        Variable* outer = nullptr;
        if (t.type == Tok::Ident && !isK(k.extends_) && !isRes(t.str)) {
            n->str = t.str;
            {  // a class name is bound in strict code
                bool saved = fn->strict;
                fn->strict = true;
                checkBinding(t.str, t.esc, VK_CLASS);
                fn->strict = saved;
            }
            if (isDecl) {
                size_t mark = declaredNow.size();
                outer = declare(t.str, VK_CLASS);
                advance();
                finishDecl(mark);
            } else
                advance();
        } else if (isDecl)
            unexpected();
        bool savedStrict = fn->strict;
        fn->strict = true;
        // ClassDefinitionEvaluation (15.7.14): the class's own scope, with its name not yet initialised, is in place
        // before the heritage expression is evaluated
        Scope* cs = open(Scope::Class);
        n->scope = cs;
        n->var2 = outer;
        if (n->str) {  // the name is also bound inside the class body, and set before static fields and blocks run
            n->var = newVar(cs, n->str, VK_CONST);
            declaredNow.pop_back();
        }
        if (eatK(k.extends_)) {
            n->a = lhs();
            // the heritage is outside the class's private names (15.7.1): what it mentions belongs to the classes around
            for (size_t i = 0; i < cs->refs.size();) {
                if (cs->refs[i].first->flags & NF_PRIVATE) {
                    cs->parent->refs.push_back(cs->refs[i]);
                    cs->refs.erase(cs->refs.begin() + (ptrdiff_t)i);
                } else
                    ++i;
            }
        }
        expectP(P_LBrace);
        classPrivates.emplace_back();
        while (!isP(P_RBrace)) {
            if (eatP(P_Semi)) continue;
            if (t.type == Tok::End) unexpected();
            n->list.push_back(classMember(n));
        }
        classPrivates.pop_back();
        advance();
        n->num = (double)prevEnd;  // where the class's text ends
        if (n->var) n->var->declEnd = prevEnd;
        close(cs);
        fn->strict = savedStrict;
        return n;
    }

    // Private names of the class being read, with what declared them: 1 getter, 2 setter, 4 anything else, +8 static.
    std::vector<std::vector<std::pair<JSString*, int>>> classPrivates;
    void declarePrivate(JSString* name, int what) {
        if (name->s == u"#constructor") fail("Classes may not have a private field named '#constructor'");
        for (auto& p : classPrivates.back()) {
            if (p.first != name) continue;
            bool pair = ((p.second & 7) | (what & 7)) == 3 && (p.second & 7) != (what & 7) && (p.second & 8) == (what & 8);
            if (!pair || (p.second & 7) == 3) fail("Identifier '" + utf16to8(name->s) + "' has already been declared");
            p.second |= what;
            return;
        }
        classPrivates.back().push_back({name, what});
    }
    Node* classMember(Node* cls) {
        Node* m = mk(N::Property);
        uint32_t start = t.pos;
        bool isStatic = false, isAsync = false, gen = false;
        uint8_t kind = PK_METHOD;
        auto nameFollows = [&] {
            Tok nx = peek();
            return !(nx.type == Tok::Punct && (nx.p == P_LParen || nx.p == P_Assign || nx.p == P_Semi || nx.p == P_RBrace)) && nx.type != Tok::End;
        };
        if (isK(k.static_) && nameFollows()) {
            isStatic = true;
            advance();
            start = t.pos;  // the text of a method does not include `static`
            if (isP(P_LBrace)) {  // static { ... }: runs once while the class is defined, with `this` set to the class
                m->op = PK_STATIC_BLOCK;
                m->flags |= NF_STATIC;
                FunctionInfo* f = beginFn(nullptr, t.pos);
                f->isMethod = true;
                f->isFieldInit = true;
                f->isStaticBlock = true;
                f->strict = true;
                advance();
                while (!isP(P_RBrace)) {
                    if (t.type == Tok::End) unexpected();
                    f->body.push_back(statement());
                }
                advance();
                endFn(f);
                Node* fnNode = mk(N::Function);
                fnNode->fn = f;
                m->b = fnNode;
                return m;
            }
        }
        if (isK(k.async_) && nameFollows() && !peek().nl) {
            isAsync = true;
            advance();
        }
        if (eatP(P_Star)) gen = true;
        if ((isK(k.get_) || isK(k.set_)) && nameFollows() && !(peek().type == Tok::Punct && peek().p == P_Star)) {
            kind = isK(k.get_) ? PK_GET : PK_SET;
            advance();
        }
        if (isStatic) m->flags |= NF_STATIC;
        JSString* keyName = nullptr;
        if (t.type == Tok::Private) {
            Node* id = mk(N::Ident);
            id->str = t.str;
            Variable* v = scope->find(t.str);
            if (!v) {
                v = newVar(scope, t.str, VK_PRIVATE);
                v->declEnd = 0;
            }
            id->var = v;
            m->a = id;
            m->flags |= NF_PRIVATE | NF_COMPUTED;
            advance();
        } else {
            if (t.type == Tok::Ident || t.type == Tok::Str) keyName = t.str;
            m->a = propertyName(m);
        }
        bool plainKey = !(m->flags & NF_COMPUTED);
        if (m->flags & NF_PRIVATE) declarePrivate(m->a->str, (isP(P_LParen) ? (kind == PK_GET ? 1 : kind == PK_SET ? 2 : 4) : 4) | (isStatic ? 8 : 0));
        if (isP(P_LParen)) {
            // 15.7.1 early errors: `constructor` is an ordinary method, declared once; no static method is called prototype
            if (plainKey && !isStatic && keyName == k.constructor_) {
                if (kind != PK_METHOD || isAsync || gen) fail("Class constructor may not be an accessor, a generator or async");
                if (cls->c) fail("A class may only have one constructor");
            }
            if (plainKey && isStatic && keyName == k.prototype_) fail("Classes may not have a static property named 'prototype'");
            bool isCtor = !isStatic && kind == PK_METHOD && keyName == k.constructor_ && !(m->flags & NF_COMPUTED);
            m->op = kind;
            m->b = functionRest((m->flags & NF_COMPUTED) ? nullptr : keyName, start, isAsync, gen, false, kind == PK_GET ? 2 : kind == PK_SET ? 3 : 1);
            m->b->fn->strict = true;
            if (!(m->flags & NF_COMPUTED) || (m->flags & NF_PRIVATE)) nameMethod(m->b, m->a->str, kind);
            if (isCtor) {
                m->b->fn->isClassCtor = true;
                m->b->fn->isDerived = cls->a != nullptr;
                m->b->fn->isMethod = false;
                m->b->fn->name = cls->str;
                cls->c = m->b;
                m->op = 255;  // not a regular member
            }
            return m;
        }
        // field: the initialiser is compiled as a small method so `this` refers to the instance (or the class).
        if (isAsync || gen || kind != PK_METHOD) unexpected();
        if (plainKey && (keyName == k.constructor_ || (isStatic && keyName == k.prototype_))) fail("Classes may not have a field named 'constructor', or a static one named 'prototype'");
        m->op = PK_FIELD;
        FunctionInfo* f = beginFn(nullptr, t.pos);
        f->isMethod = true;
        f->isFieldInit = true;
        f->strict = true;
        if (eatP(P_Assign)) {
            Node* r = mk(N::Return);
            r->a = assign();
            f->body.push_back(r);
            if (m->flags & NF_PRIVATE) nameFn(r->a, m->a->str);
            else if (m->flags & NF_COMPUTED) f->namesValue = anonymousFn(r->a);
            else nameFn(r->a, m->a->str);
        }
        endFn(f);
        Node* fnNode = mk(N::Function);
        fnNode->fn = f;
        m->b = f->body.empty() ? nullptr : fnNode;
        semi();
        return m;
    }

    // Property name in an object literal, class body or pattern. Sets NF_COMPUTED on `owner` when needed.
    Node* propertyName(Node* owner) {
        if (t.type == Tok::Ident || t.type == Tok::Str) {
            Node* n = mk(N::Str);
            n->str = t.str;
            advance();
            return n;
        }
        if (t.type == Tok::Num) {
            Node* n = mk(N::Str);
            n->str = t.big ? vm.atom(bigToString(bigFromLiteral(vm, t.str->s))) : vm.atomOf(vm.numberToString(t.num));
            advance();
            return n;
        }
        if (isP(P_LBrack)) {
            advance();
            Node* e = assign();
            expectP(P_RBrack);
            owner->flags |= NF_COMPUTED;
            return e;
        }
        unexpected();
    }

    // ---------------------------------------------------------------- expressions
    Node* expression(bool in = true) {
        Node* e = assign(in);
        if (!isP(P_Comma)) return e;
        Node* seq = mk(N::Seq);
        seq->list.push_back(e);
        while (eatP(P_Comma)) seq->list.push_back(assign(in));
        return seq;
    }

    Node* assign(bool in = true) {
        uint32_t start = t.pos;
        if (t.type == Tok::Ident && !t.esc) {
            if (t.str == k.async_) {
                Tok nx = peek();
                if (!nx.nl) {
                    if (nx.type == Tok::Ident && !isRes(nx.str)) {
                        Tok n2 = peek(2);
                        if (n2.type == Tok::Punct && n2.p == P_Arrow) {
                            advance();
                            return arrow(true, start);
                        }
                    } else if (nx.type == Tok::Punct && nx.p == P_LParen && arrowAhead(nx.pos)) {
                        advance();
                        return arrow(true, start);
                    }
                }
            } else if (t.str == k.yield_ && fn->isGenerator) {
                if (fn == paramsOf) fail("Yield expression not allowed in formal parameter");
                Node* y = mk(N::Yield);
                advance();
                if (!t.nl && !isP(P_RParen) && !isP(P_RBrack) && !isP(P_RBrace) && !isP(P_Comma) && !isP(P_Semi) && !isP(P_Colon) &&
                    t.type != Tok::End) {
                    if (eatP(P_Star)) y->flags |= NF_DELEGATE;
                    y->a = assign(in);
                }
                return y;
            } else if (!isRes(t.str)) {
                Tok nx = peek();
                if (nx.type == Tok::Punct && nx.p == P_Arrow && !nx.nl) return arrow(false, start);
            }
        } else if (isP(P_LParen) && arrowAhead(t.pos)) {
            return arrow(false, start);
        }
        Node* left = conditional(in);
        if (t.type == Tok::Punct && t.p >= P_Assign && t.p <= P_QQEq) {
            Node* n = mk(N::Assign);
            n->op = t.p;
            if (internalRef(left)) fail("Invalid assignment target");
            if (n->op == P_Assign) left = toPattern(left);
            else if (left->type != N::Ident && left->type != N::Member && left->type != N::Index &&
                     !(left->type == N::Call && !fn->strict && n->op != P_AndAndEq && n->op != P_OrOrEq && n->op != P_QQEq))
                fail("Invalid assignment target");
            else if (left->type == N::Ident && fn->strict && isEvalOrArguments(left->str)) fail("Unexpected eval or arguments in strict mode");
            n->a = left;
            advance();
            n->b = assign(in);
            if (left->type == N::Ident && !(left->flags & NF_PAREN)) nameFn(n->b, left->str);
            return n;
        }
        return left;
    }

    Node* conditional(bool in) {
        Node* c = binary(0, in);
        if (!isP(P_Question)) return c;
        Node* n = mk(N::Cond);
        advance();
        n->a = c;
        n->b = assign(true);
        expectP(P_Colon);
        n->c = assign(in);
        return n;
    }

    int precOf(bool in, uint8_t& op) {
        if (t.type == Tok::Punct) {
            op = t.p;
            switch (t.p) {
                case P_QQ: return 1;
                case P_OrOr: return 2;
                case P_AndAnd: return 3;
                case P_Pipe: return 4;
                case P_Caret: return 5;
                case P_Amp: return 6;
                case P_EqEq: case P_Ne: case P_SEq: case P_SNe: return 7;
                case P_Lt: case P_Gt: case P_Le: case P_Ge: return 8;
                case P_Shl: case P_Shr: case P_UShr: return 9;
                case P_Plus: case P_Minus: return 10;
                case P_Star: case P_Slash: case P_Percent: return 11;
                case P_StarStar: return 12;
            }
            return -1;
        }
        if (t.type == Tok::Ident && !t.esc) {
            if (t.str == k.instanceof_) {
                op = P_InstanceOf;
                return 8;
            }
            if (t.str == k.in_ && in) {
                op = P_In;
                return 8;
            }
        }
        return -1;
    }

    bool unaryOperand = false;  // set by unary(): what it returned began with a unary operator
    Node* binary(int minPrec, bool in) {
        Node* left = unary();
        bool unaryBase = unaryOperand;
        if (left->type == N::Ident && (left->flags & NF_PRIVATE)) {  // #name is only ever the left operand of `in` (13.10.1)
            uint8_t next = P_None;
            if (precOf(in, next) < minPrec || next != P_In) fail("Unexpected private name");
        }
        auto bareLogical = [](Node* x, bool coalesce) { return x->type == N::Logical && !(x->flags & NF_PAREN) && (x->op == P_QQ) == coalesce; };
        for (;;) {
            uint8_t op = P_None;
            int prec = precOf(in, op);
            if (prec < 0 || prec < minPrec) break;
            if (op == P_StarStar && unaryBase) fail("A unary operator cannot be used immediately before '**': add parentheses");
            unaryBase = false;
            Node* n = mk((op == P_AndAnd || op == P_OrOr || op == P_QQ) ? N::Logical : N::Binary);
            n->op = op;
            advance();
            n->a = left;
            n->b = op == P_StarStar ? binary(prec, in) : binary(prec + 1, in);
            if (op == P_QQ && (bareLogical(left, false) || bareLogical(n->b, false))) fail("'??' cannot be mixed with '||' or '&&' without parentheses");
            if ((op == P_OrOr || op == P_AndAnd) && (bareLogical(left, true) || bareLogical(n->b, true))) fail("'??' cannot be mixed with '||' or '&&' without parentheses");
            left = n;
        }
        return left;
    }

    Node* unary() {
        if (t.type == Tok::Punct) {
            if (t.p == P_Bang || t.p == P_Tilde || t.p == P_Plus || t.p == P_Minus) {
                Node* n = mk(N::Unary);
                n->op = t.p;
                advance();
                n->a = unary();
                unaryOperand = true;
                if (n->a->type == N::Num && (n->op == P_Minus || n->op == P_Plus)) {  // fold numeric literals
                    n->a->num = n->op == P_Minus ? -n->a->num : n->a->num;
                    return n->a;
                }
                return n;
            }
            if (t.p == P_Inc || t.p == P_Dec) {
                Node* n = mk(N::Update);
                n->op = t.p;
                n->flags = NF_PREFIX;
                advance();
                n->a = unary();
                if (n->a->type != N::Ident && n->a->type != N::Member && n->a->type != N::Index && !(n->a->type == N::Call && !fn->strict))
                    fail("Invalid update target");
                if (internalRef(n->a)) fail("Invalid update target");
                if (n->a->type == N::Ident && fn->strict && isEvalOrArguments(n->a->str)) fail("Unexpected eval or arguments in strict mode");
                unaryOperand = false;
                return n;
            }
        } else if (t.type == Tok::Ident && !t.esc) {
            if (t.str == k.typeof_ || t.str == k.void_ || t.str == k.delete_) {
                Node* n = mk(N::Unary);
                n->op = t.str == k.typeof_ ? P_Typeof : t.str == k.void_ ? P_Void : P_Delete;
                advance();
                n->a = unary();
                if (n->op == P_Delete) {  // 13.5.1.1: not a private member, and in strict code not a plain name
                    Node* x = n->a->type == N::OptChain ? n->a->a : n->a;
                    if (x->type == N::Index && (x->flags & NF_PRIVATE)) fail("Private fields can not be deleted");
                    if (x->type == N::Ident && fn->strict) fail("Delete of an unqualified identifier in strict mode");
                }
                unaryOperand = true;
                return n;
            }
            if (t.str == k.await_ && fn->isAsync) {
                if (fn == paramsOf) fail("Await expression not allowed in formal parameter");
                if (fn == ast.moduleFn) ast.hasTLA = true;
                Node* n = mk(N::Await);
                advance();
                n->a = unary();
                unaryOperand = true;
                return n;
            }
        }
        Node* e = lhs();
        unaryOperand = false;
        if (!t.nl && (isP(P_Inc) || isP(P_Dec))) {
            if (e->type != N::Ident && e->type != N::Member && e->type != N::Index && !(e->type == N::Call && !fn->strict)) fail("Invalid update target");
            if (internalRef(e)) fail("Invalid update target");
            if (e->type == N::Ident && fn->strict && isEvalOrArguments(e->str)) fail("Unexpected eval or arguments in strict mode");
            Node* n = mk(N::Update);
            n->op = t.p;
            n->a = e;
            advance();
            return n;
        }
        return e;
    }

    void arguments(Node* call) {
        expectP(P_LParen);
        while (!isP(P_RParen)) {
            if (isP(P_Ellipsis)) {
                Node* sp = mk(N::Spread);
                advance();
                sp->a = assign();
                call->list.push_back(sp);
            } else
                call->list.push_back(assign());
            if (!isP(P_RParen)) expectP(P_Comma);
        }
        advance();
    }

    Node* memberName(Node* obj, bool optional) {
        if (t.type == Tok::Private) {
            Node* n = mk(N::Index);
            n->a = obj;
            Node* id = mk(N::Ident);
            id->str = t.str;
            id->flags = NF_PRIVATE;
            reference(id);
            n->b = id;
            n->flags = NF_PRIVATE | (optional ? NF_OPTIONAL : 0);
            advance();
            return n;
        }
        if (t.type != Tok::Ident) unexpected();
        Node* n = mk(N::Member);
        n->a = obj;
        n->str = t.str;
        if (optional) n->flags = NF_OPTIONAL;
        advance();
        return n;
    }

    Node* lhs() {
        Node* e;
        if (isK(k.new_)) {
            e = newExpr();
        } else if (isK(k.super_)) {
            e = mk(N::Super);
            e->var = pseudo(&FunctionInfo::thisVar, "this");
            fn->usesSuper = true;
            advance();
            if (!isP(P_Dot) && !isP(P_LBrack) && !isP(P_LParen)) unexpected();
            {  // super.x needs a method (or a field initializer or constructor) around it: 15.7.1, 13.3.7 early errors
                const FunctionInfo* owner = fn;
                while (owner->isArrow && owner->parent) owner = owner->parent;
                if (!isP(P_LParen) && !owner->isMethod && !owner->isClassCtor) fail("'super' keyword unexpected here");
                if (isP(P_Dot) && peek().type == Tok::Private) fail("Unexpected private field");
                if (isP(P_LParen) && owner != fn) {  // super() from an arrow: it needs the constructor and its new.target
                    pseudo(&FunctionInfo::newTargetVar, "new.target");
                    pseudo(&FunctionInfo::ctorVar, "%ctor");
                }
            }
        } else if (isK(k.import_)) {
            advance();
            if (eatP(P_Dot)) {  // import.meta
                if (!isWord("meta")) unexpected();
                advance();
                if (!ast.moduleFn) fail("Cannot use 'import.meta' outside a module");
                e = identRef(vm.atom("%meta"));
            } else {
                expectP(P_LParen);
                e = mk(N::ImportCall);
                e->a = assign(true);
                if (eatP(P_Comma) && !isP(P_RParen)) {
                    e->b = assign(true);  // options
                    eatP(P_Comma);
                }
                expectP(P_RParen);
            }
        } else {
            e = primary();
        }
        bool chain = false;
        for (;;) {
            if (isP(P_Dot)) {
                advance();
                e = memberName(e, false);
            } else if (isP(P_QDot)) {
                advance();
                chain = true;
                if (isP(P_LParen)) {
                    Node* c = mk(N::Call);
                    c->a = e;
                    c->flags = NF_OPTIONAL;
                    arguments(c);
                    e = c;
                } else if (isP(P_LBrack)) {
                    Node* n = mk(N::Index);
                    advance();
                    n->a = e;
                    n->b = expression();
                    n->flags = NF_OPTIONAL;
                    expectP(P_RBrack);
                    e = n;
                } else
                    e = memberName(e, true);
            } else if (isP(P_LBrack)) {
                Node* n = mk(N::Index);
                advance();
                n->a = e;
                n->b = expression();
                expectP(P_RBrack);
                e = n;
            } else if (isP(P_LParen)) {
                Node* c = mk(N::Call);
                c->a = e;
                arguments(c);
                if (e->type == N::Ident && e->str == evalAtom) markEval(c);
                e = c;
            } else if (t.type == Tok::Templ) {
                if (chain) fail("A tagged template cannot be part of an optional chain");
                Node* tt = mk(N::TaggedTemplate);
                tt->a = e;
                tt->b = templateLit(true);
                e = tt;
            } else
                break;
        }
        if (chain) {
            Node* oc = mk(N::OptChain);
            oc->a = e;
            return oc;
        }
        return e;
    }

    Node* newExpr() {
        Node* n = mk(N::New);
        advance();  // new
        if (isP(P_Dot)) {
            advance();
            if (!isK(k.target_)) unexpected();
            advance();
            n->type = N::NewTarget;
            FunctionInfo* owner = fn;
            while (owner->isArrow && owner->parent) owner = owner->parent;
            if (!owner->scope || owner->scope->kind != Scope::Function || owner == ast.moduleFn) fail("new.target expression is not allowed here");
            n->var = pseudo(&FunctionInfo::newTargetVar, "new.target");
            return n;
        }
        Node* callee;
        if (isK(k.import_)) {  // only import.meta can follow new
            advance();
            expectP(P_Dot);
            if (!isWord("meta")) unexpected();
            advance();
            if (!ast.moduleFn) fail("Cannot use 'import.meta' outside a module");
            callee = identRef(vm.atom("%meta"));
        } else
            callee = isK(k.new_) ? newExpr() : primary();
        for (;;) {
            if (isP(P_Dot)) {
                advance();
                callee = memberName(callee, false);
            } else if (isP(P_LBrack)) {
                Node* ix = mk(N::Index);
                advance();
                ix->a = callee;
                ix->b = expression();
                expectP(P_RBrack);
                callee = ix;
            } else if (t.type == Tok::Templ) {  // MemberExpression TemplateLiteral: the tag call's result is what gets constructed
                Node* tt = mk(N::TaggedTemplate);
                tt->a = callee;
                tt->b = templateLit(true);
                callee = tt;
            } else
                break;
        }
        n->a = callee;
        if (isP(P_LParen)) arguments(n);
        return n;
    }

    Node* templateLit(bool tagged = false) {
        Node* n = mk(N::Template);
        for (;;) {
            if (t.bad && !tagged) fail("Invalid escape sequence in template literal");
            n->strs.push_back(t.bad ? nullptr : t.str);
            n->strs.push_back(t.raw);
            if (t.tail) {
                advance();
                break;
            }
            advance();
            n->list.push_back(expression());
            if (!isP(P_RBrace)) unexpected();
            bool nl = t.nl;
            t = lx.templ(t.pos + 1);
            t.nl = nl;
        }
        return n;
    }

    Node* primary() {
        switch (t.type) {
            case Tok::Num: {
                if (t.oct && fn->strict) fail("Octal literals are not allowed in strict mode");
                Node* n = mk(t.big ? N::BigInt : N::Num);
                n->num = t.num;
                n->str = t.big ? t.str : nullptr;
                advance();
                return n;
            }
            case Tok::Str: {
                if (t.oct && fn->strict) fail("Octal escape sequences are not allowed in strict mode");
                Node* n = mk(N::Str);
                n->str = t.str;
                advance();
                return n;
            }
            case Tok::Templ: return templateLit();
            case Tok::Private: {  // `#name in object`: does the object have this private member
                Tok nx = peek();
                if (!(nx.type == Tok::Ident && nx.str == k.in_ && !nx.esc)) fail("Unexpected private name");
                Node* id = mk(N::Ident);
                id->str = t.str;
                id->flags = NF_PRIVATE;
                reference(id);
                advance();
                return id;
            }
            case Tok::Punct:
                if (t.p == P_LParen) {
                    advance();
                    Node* e = expression();
                    expectP(P_RParen);
                    e->flags |= NF_PAREN;
                    return e;
                }
                if (t.p == P_LBrack) return arrayLit();
                if (t.p == P_LBrace) return objectLit();
                if (t.p == P_Slash || t.p == P_SlashEq) {
                    bool nl = t.nl;
                    t = lx.regex(t.pos);
                    t.nl = nl;
                    Node* n = mk(N::Regex);
                    n->str = t.str;
                    n->str2 = t.raw;
                    std::string error;  // a malformed literal is an early error (ECMA-262 13.2.7.2)
                    if (!regexValidate(t.str->s, t.raw->s, error)) fail("Invalid regular expression: /" + utf16to8(t.str->s) + "/: " + error);
                    advance();
                    return n;
                }
                unexpected();
            case Tok::Ident: {
                JSString* w = t.str;
                if (!t.esc) {
                    if (w == k.this_) {
                        Node* n = mk(N::This);
                        n->var = pseudo(&FunctionInfo::thisVar, "this");
                        advance();
                        return n;
                    }
                    if (w == k.null_ || w == k.true_ || w == k.false_) {
                        Node* n = mk(w == k.null_ ? N::Null : w == k.true_ ? N::True : N::False);
                        advance();
                        return n;
                    }
                    if (w == k.function_) {
                        uint32_t start = t.pos;
                        advance();
                        bool gen = eatP(P_Star);
                        JSString* name = nullptr;
                        if (t.type == Tok::Ident && !isRes(t.str)) {
                            name = t.str;
                            if (name == k.yield_ && (gen || fn->strict)) fail("Unexpected identifier 'yield'");
                            if (name == k.await_ && ast.moduleFn) fail("Unexpected reserved word 'await'");
                            advance();
                        }
                        return functionRest(name, start, false, gen, true);
                    }
                    if (w == k.async_) {
                        Tok nx = peek();
                        if (nx.type == Tok::Ident && nx.str == k.function_ && !nx.nl) {
                            uint32_t start = t.pos;
                            advance();
                            advance();
                            bool gen = eatP(P_Star);
                            JSString* name = nullptr;
                            if (t.type == Tok::Ident && !isRes(t.str)) {
                                name = t.str;
                                if (name == k.await_ || (name == k.yield_ && (gen || fn->strict))) fail("Unexpected reserved word");
                                advance();
                            }
                            return functionRest(name, start, true, gen, true);
                        }
                    }
                    if (w == k.class_) return classNode(false);
                    if (isRes(w)) unexpected();
                }
                checkName(w, t.esc);
                if (w == k.arguments_) {
                    const FunctionInfo* f = fn;
                    while (f->isArrow && f->parent) f = f->parent;
                    if (f->isFieldInit) fail("'arguments' is not allowed in class field initializer or static initialization block");
                }
                Node* n = identRef(w);
                advance();
                return n;
            }
            default: unexpected();
        }
    }

    Node* arrayLit() {
        Node* a = mk(N::Array);
        advance();
        while (!isP(P_RBrack)) {
            if (isP(P_Comma)) {
                a->list.push_back(mk(N::Hole));
                advance();
                continue;
            }
            if (isP(P_Ellipsis)) {
                Node* sp = mk(N::Spread);
                advance();
                sp->a = assign();
                a->list.push_back(sp);
                if (isP(P_Comma)) a->flags |= NF_TRAILING;
            } else {
                a->flags &= ~NF_TRAILING;
                a->list.push_back(assign());
            }
            if (!isP(P_RBrack)) expectP(P_Comma);
        }
        advance();
        return a;
    }

    std::vector<Node*> protoDups;  // object literals that spell __proto__: twice; fine only as destructuring patterns
    Node* objectLit() {
        Node* o = mk(N::Object);
        advance();
        while (!isP(P_RBrace)) {
            Node* p = mk(N::Property);
            uint32_t start = t.pos;
            if (isP(P_Ellipsis)) {
                advance();
                p->op = PK_SPREAD;
                p->a = assign();
            } else {
                bool isAsync = false, gen = false;
                uint8_t kind = PK_INIT;
                auto nameFollows = [&] {
                    Tok nx = peek();
                    return !(nx.type == Tok::Punct && (nx.p == P_LParen || nx.p == P_Colon || nx.p == P_Comma || nx.p == P_RBrace || nx.p == P_Assign));
                };
                if (isK(k.async_) && nameFollows() && !peek().nl) {
                    isAsync = true;
                    advance();
                }
                if (eatP(P_Star)) gen = true;
                if ((isK(k.get_) || isK(k.set_)) && nameFollows()) {
                    kind = isK(k.get_) ? PK_GET : PK_SET;
                    advance();
                }
                bool isIdent = t.type == Tok::Ident && !t.esc, escaped = t.esc;
                JSString* name = (t.type == Tok::Ident || t.type == Tok::Str) ? t.str : nullptr;
                p->a = propertyName(p);
                if (!isP(P_LParen) && (isAsync || gen || kind != PK_INIT)) unexpected();  // async, * and get/set need a method
                if (isP(P_LParen)) {
                    p->op = kind == PK_INIT ? PK_METHOD : kind;
                    p->b = functionRest((p->flags & NF_COMPUTED) ? nullptr : name, start, isAsync, gen, false, kind == PK_GET ? 2 : kind == PK_SET ? 3 : 1);
                    if (!(p->flags & NF_COMPUTED)) nameMethod(p->b, p->a->str, kind);
                } else if (eatP(P_Colon)) {
                    p->op = PK_INIT;
                    p->b = assign();
                    bool proto = !(p->flags & NF_COMPUTED) && name == k.proto_;  // __proto__: value sets the prototype
                    if (proto) {
                        if (o->flags & NF_PREFIX) protoDups.push_back(o);  // an error unless this turns out to be a pattern
                        o->flags |= NF_PREFIX;
                    }
                    if (!(p->flags & NF_COMPUTED) && !proto) nameFn(p->b, name);
                } else {
                    if (!isIdent || isRes(name) || (p->flags & NF_COMPUTED)) unexpected();
                    checkName(name, escaped);
                    p->op = PK_INIT;
                    p->flags |= NF_SHORTHAND;
                    Node* id = identRef(name);
                    if (isP(P_Assign)) {  // only valid when the literal turns out to be a destructuring pattern
                        advance();
                        Node* ap = mk(N::AssignPat);
                        ap->a = id;
                        ap->b = assign();
                        nameFn(ap->b, name);
                        p->b = ap;
                    } else
                        p->b = id;
                }
            }
            o->list.push_back(p);
            if (!isP(P_RBrace)) {
                expectP(P_Comma);
                if (isP(P_RBrace)) o->flags |= NF_TRAILING;
            }
        }
        advance();
        return o;
    }

    // import.meta is kept as a reference to a hidden variable; it is a value, never a target
    static bool internalRef(Node* e) { return e->type == N::Ident && !(e->flags & NF_PRIVATE) && !e->str->s.empty() && e->str->s[0] == '%'; }
    // Reinterprets an expression as an assignment target (destructuring patterns included).
    Node* toPattern(Node* n, bool nested = false) {
        if (internalRef(n)) fail("Invalid assignment target");
        if ((n->flags & NF_PAREN) && (n->type == N::Array || n->type == N::Object || (nested && n->type == N::Assign))) fail("Invalid destructuring assignment target");
        switch (n->type) {
            case N::Ident:
                if (fn->strict && isEvalOrArguments(n->str)) fail("Unexpected eval or arguments in strict mode");
                return n;
            case N::AssignPat:
                if (n->a->type == N::Ident && fn->strict && isEvalOrArguments(n->a->str)) fail("Unexpected eval or arguments in strict mode");
                return n;
            case N::Member:
            case N::Index:
            case N::ArrayPat:
            case N::ObjectPat:
            case N::Hole: return n;
            case N::Call:
                if (!nested && !fn->strict) return n;
                break;
            case N::Array:
                n->type = N::ArrayPat;
                for (Node*& e : n->list) {
                    if (e->type == N::Spread) {
                        if (&e != &n->list.back() || (n->flags & NF_TRAILING)) fail("Rest element must be last element");
                        e->type = N::Rest;
                        e->a = toPattern(e->a, true);
                        if (e->a->type == N::AssignPat) fail("Rest element may not have an initializer");
                    } else
                        e = toPattern(e, true);
                }
                return n;
            case N::Object:
                protoDups.erase(std::remove(protoDups.begin(), protoDups.end(), n), protoDups.end());
                n->type = N::ObjectPat;
                for (Node*& p : n->list) {
                    if (p->op == PK_SPREAD) {
                        if (&p != &n->list.back() || (n->flags & NF_TRAILING)) fail("Rest element must be last element");
                        if (p->a->type != N::Ident && p->a->type != N::Member && p->a->type != N::Index) fail("Invalid rest element in an object pattern");
                        p->type = N::Rest;
                        p->a = toPattern(p->a, true);
                    } else if (p->op == PK_INIT)
                        p->b = toPattern(p->b, true);
                    else
                        fail("Invalid destructuring target");
                }
                return n;
            case N::Assign:
                if (n->op != P_Assign || !nested) break;
                n->type = N::AssignPat;
                n->a = toPattern(n->a, true);
                return n;
            default: break;
        }
        fail("Invalid assignment target");
    }
};

}  // namespace

void parseEval(Vm& vm, Ast& ast, const EvalSite& site) {
    Parser p(vm, ast);
    p.evalProgram(site);
    p.finish();
}

void parseDynamicFunction(Vm& vm, Ast& ast, int kind) {
    Parser p(vm, ast);
    p.dynamicFunction(kind);
    p.finish();
}

void parseParamsOnly(Vm& vm, Ast& ast, int kind) {
    Parser p(vm, ast);
    p.paramsOnly(kind);
    p.finish();
}

void parseScript(Vm& vm, Ast& ast, bool asModule, const std::u16string*) {
    Parser p(vm, ast);
    if (asModule) p.module();
    else p.program();
    p.finish();
}

}  // namespace tb::js
