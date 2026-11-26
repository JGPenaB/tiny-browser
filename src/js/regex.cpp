// Regular expression engine. A pattern is parsed into a tree, compiled into instructions for a backtracking
// matcher with an explicit stack (so long inputs cannot overflow the native stack), and run on UTF-16 text.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>

#include "regex.h"
#include "unicode_data.h"

namespace tb::js {

namespace {

const uint32_t INF = 0xFFFFFFFFu;

enum : uint8_t {
    I_CHAR, I_CHAR_I, I_ANY, I_CLASS, I_SPLIT, I_JMP, I_SAVE, I_RESET, I_LOOP_START, I_LOOP_CHECK,
    I_BOL, I_EOL, I_WORDB, I_NWORDB, I_BACKREF, I_REP, I_LOOK, I_LOOK_END, I_MATCH
};

struct Inst {
    uint8_t op = 0, flag = 0;
    uint16_t a = 0;
    uint32_t x = 0, y = 0, z = 0;
};

// ---------------------------------------------------------------------------- characters
bool isHigh(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool isLow(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
bool isLineEnd(uint32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }
bool isWord(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

uint32_t upperOf(uint32_t c) {
    if (c < 128) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c > 0xFFFF) return c;
    return (uint32_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)(wchar_t)c) & 0xFFFF;
}
uint32_t lowerOf(uint32_t c) {
    if (c < 128) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c > 0xFFFF) return c;
    return (uint32_t)(uintptr_t)CharLowerW((LPWSTR)(uintptr_t)(wchar_t)c) & 0xFFFF;
}

// Unicode properties come from generated tables (unicode_data.h). A property is the number of its table.
int findProp(const std::string& name) {
    size_t lo = 0, hi = sizeof(kUniNames) / sizeof(kUniNames[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(kUniNames[mid].name, name.c_str());
        if (c == 0) return kUniNames[mid].table;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}
bool hasProp(int prop, uint32_t cp) {
    const UniTable& t = kUniTables[prop];
    size_t lo = 0, hi = t.count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < t.ranges[mid * 2]) hi = mid;
        else if (cp > t.ranges[mid * 2 + 1]) lo = mid + 1;
        else return true;
    }
    return false;
}

struct CharClass {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::vector<std::pair<int, bool>> props;  // Unicode property, negated?
    bool negate = false;
    bool ascii[128] = {};  // final answer for ASCII, filled in once the class is complete
};

bool rawIn(const CharClass& cc, uint32_t cp) {
    // ranges are sorted and merged
    size_t lo = 0, hi = cc.ranges.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < cc.ranges[mid].first) hi = mid;
        else if (cp > cc.ranges[mid].second) lo = mid + 1;
        else return true;
    }
    for (auto& p : cc.props)
        if (hasProp(p.first, cp) != p.second) return true;
    return false;
}
bool classMatchSlow(const CharClass& cc, uint32_t cp, bool ignoreCase) {
    bool in = rawIn(cc, cp);
    if (!in && ignoreCase) {
        uint32_t u = upperOf(cp), l = lowerOf(cp);
        in = (u != cp && rawIn(cc, u)) || (l != cp && rawIn(cc, l));
    }
    return in != cc.negate;
}
void finishClass(CharClass& cc, bool ignoreCase) {
    std::sort(cc.ranges.begin(), cc.ranges.end());
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (auto& r : cc.ranges) {
        if (!merged.empty() && r.first <= merged.back().second + 1 && merged.back().second != INF) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    }
    cc.ranges.swap(merged);
    for (uint32_t c = 0; c < 128; ++c) cc.ascii[c] = classMatchSlow(cc, c, ignoreCase);
}

void addEscapeClass(CharClass& cc, char16_t kind) {
    std::vector<std::pair<uint32_t, uint32_t>> r;
    switch (kind | 0x20) {
        case 'd': r = {{'0', '9'}}; break;
        case 'w': r = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}}; break;
        default:
            r = {{9, 13}, {32, 32}, {0xA0, 0xA0}, {0x1680, 0x1680}, {0x2000, 0x200A}, {0x2028, 0x2029}, {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000}, {0xFEFF, 0xFEFF}};
    }
    if (kind >= 'a') {  // lower case letter: the set itself
        cc.ranges.insert(cc.ranges.end(), r.begin(), r.end());
        return;
    }
    uint32_t next = 0;  // upper case letter: everything else
    for (auto& p : r) {
        if (p.first > next) cc.ranges.push_back({next, p.first - 1});
        next = p.second + 1;
    }
    cc.ranges.push_back({next, 0x10FFFF});
}

// ---------------------------------------------------------------------------- parser
struct RxNode {
    enum T : uint8_t { Empty, Char, Any, Class, Seq, Alt, Group, Repeat, Bol, Eol, WordB, NWordB, BackRef, Look } t = Empty;
    uint32_t c = 0;  // Char: code point, Class: index into the class table
    int cap = -1;    // Group: capture number (-1: not capturing), BackRef: group referred to
    uint32_t min = 0, max = 0;
    bool greedy = true, behind = false, negative = false;
    std::vector<RxNode*> kids;
    std::u16string name;  // named back reference, resolved after parsing
};

struct ParseFail {};

struct Parser {
    const std::u16string& src;
    RegexFlags fl;
    size_t i = 0;
    std::deque<RxNode> arena;
    std::vector<CharClass> classes;
    std::vector<std::pair<std::u16string, int>> names;
    std::vector<RxNode*> namedRefs;
    int ncaps = 1, totalCaps = 1, depth = 0;
    bool hasNamed = false;
    std::string err;

    [[noreturn]] void fail(const char* m) {
        err = m;
        throw ParseFail{};
    }
    RxNode* mk(RxNode::T t) {
        arena.emplace_back();
        arena.back().t = t;
        return &arena.back();
    }
    RxNode* chr(uint32_t c) {
        RxNode* n = mk(RxNode::Char);
        n->c = c;
        return n;
    }
    RxNode* classNode(CharClass&& cc) {
        classes.push_back(std::move(cc));
        RxNode* n = mk(RxNode::Class);
        n->c = (uint32_t)classes.size() - 1;
        return n;
    }
    bool more() const { return i < src.size(); }
    static bool isDigit(char16_t c) { return c >= '0' && c <= '9'; }
    static int hexVal(char16_t c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1; }
    uint32_t nextCp() {
        char16_t c = src[i++];
        if (fl.unicode && isHigh(c) && i < src.size() && isLow(src[i])) return 0x10000 + ((c - 0xD800) << 10) + (src[i++] - 0xDC00);
        return c;
    }

    void prescan() {  // back references need the number of groups before the groups are parsed
        bool inClass = false;
        for (size_t k = 0; k < src.size(); ++k) {
            char16_t c = src[k];
            if (c == '\\') {
                ++k;
                continue;
            }
            if (inClass) {
                if (c == ']') inClass = false;
                continue;
            }
            if (c == '[') inClass = true;
            if (c != '(') continue;
            if (k + 1 < src.size() && src[k + 1] == '?') {
                if (k + 3 < src.size() && src[k + 2] == '<' && src[k + 3] != '=' && src[k + 3] != '!') {
                    ++totalCaps;
                    hasNamed = true;
                }
            } else
                ++totalCaps;
        }
    }

    RxNode* parse() {
        prescan();
        RxNode* root = disjunction();
        if (more()) fail(src[i] == ')' ? "Unmatched ')'" : "Unexpected character");
        for (RxNode* r : namedRefs) {
            for (auto& n : names)
                if (n.first == r->name) r->cap = n.second;
            if (r->cap < 0) fail("Invalid named capture referenced");
        }
        return root;
    }
    RxNode* disjunction() {
        RxNode* first = alternative();
        if (!more() || src[i] != '|') return first;
        RxNode* alt = mk(RxNode::Alt);
        alt->kids.push_back(first);
        while (more() && src[i] == '|') {
            ++i;
            alt->kids.push_back(alternative());
        }
        return alt;
    }
    RxNode* alternative() {
        RxNode* seq = mk(RxNode::Seq);
        while (more() && src[i] != '|' && src[i] != ')') {
            RxNode* t = term();
            uint32_t mn = 0, mx = 0;
            bool greedy = true;
            if (quantifier(mn, mx, greedy)) {
                if (t->t == RxNode::Bol || t->t == RxNode::Eol || t->t == RxNode::WordB || t->t == RxNode::NWordB || (t->t == RxNode::Look && t->behind))
                    fail("Nothing to repeat");
                RxNode* r = mk(RxNode::Repeat);
                r->min = mn;
                r->max = mx;
                r->greedy = greedy;
                r->kids.push_back(t);
                t = r;
            }
            seq->kids.push_back(t);
        }
        return seq->kids.size() == 1 ? seq->kids[0] : seq;
    }
    bool braces(uint32_t& mn, uint32_t& mx) {  // at '{'; leaves i untouched unless it is a real quantifier
        size_t k = i + 1;
        if (k >= src.size() || !isDigit(src[k])) return false;
        uint64_t a = 0, b = 0;
        while (k < src.size() && isDigit(src[k])) a = std::min<uint64_t>(a * 10 + (src[k++] - '0'), INF - 1);
        b = a;
        if (k < src.size() && src[k] == ',') {
            ++k;
            if (k < src.size() && isDigit(src[k])) {
                b = 0;
                while (k < src.size() && isDigit(src[k])) b = std::min<uint64_t>(b * 10 + (src[k++] - '0'), INF - 1);
            } else
                b = INF;
        }
        if (k >= src.size() || src[k] != '}') return false;
        if (b < a) fail("numbers out of order in {} quantifier");
        mn = (uint32_t)a;
        mx = (uint32_t)b;
        i = k + 1;
        return true;
    }
    bool quantifier(uint32_t& mn, uint32_t& mx, bool& greedy) {
        if (!more()) return false;
        char16_t c = src[i];
        if (c == '*') mn = 0, mx = INF, ++i;
        else if (c == '+') mn = 1, mx = INF, ++i;
        else if (c == '?') mn = 0, mx = 1, ++i;
        else if (c != '{' || !braces(mn, mx)) return false;
        greedy = true;
        if (more() && src[i] == '?') {
            greedy = false;
            ++i;
        }
        return true;
    }
    RxNode* term() {
        char16_t c = src[i];
        switch (c) {
            case '^': ++i; return mk(RxNode::Bol);
            case '$': ++i; return mk(RxNode::Eol);
            case '.': ++i; return mk(RxNode::Any);
            case '(': return group();
            case '[': return charClass();
            case '\\': return atomEscape();
            case '*':
            case '+':
            case '?': fail("Nothing to repeat");
            case '{': {
                uint32_t a, b;
                size_t save = i;
                if (braces(a, b)) fail("Nothing to repeat");
                i = save;
                if (fl.unicode) fail("Lone quantifier brackets");
                ++i;
                return chr('{');
            }
            case '}':
            case ']':
                if (fl.unicode) fail("Lone quantifier brackets");
                ++i;
                return chr(c);
            default: return chr(nextCp());
        }
    }
    RxNode* group() {
        ++i;
        RxNode* g = mk(RxNode::Group);
        if (more() && src[i] == '?') {
            ++i;
            if (!more()) fail("Invalid group");
            char16_t k = src[i++];
            if (k == '=' || k == '!') {
                g->t = RxNode::Look;
                g->negative = k == '!';
            } else if (k == '<') {
                if (more() && (src[i] == '=' || src[i] == '!')) {
                    g->t = RxNode::Look;
                    g->behind = true;
                    g->negative = src[i++] == '!';
                } else {
                    std::u16string name;
                    while (more() && src[i] != '>') name += src[i++];
                    if (!more() || name.empty()) fail("Invalid capture group name");
                    ++i;
                    g->cap = ncaps++;
                    names.push_back({name, g->cap});
                }
            } else if (k != ':')
                fail("Invalid group");
        } else
            g->cap = ncaps++;
        if (++depth > 200) fail("Regular expression too deeply nested");
        g->kids.push_back(disjunction());
        --depth;
        if (!more() || src[i] != ')') fail("Unterminated group");
        ++i;
        return g;
    }
    uint32_t octal() {
        uint32_t v = 0;
        for (int n = 0; n < 3 && more() && src[i] >= '0' && src[i] <= '7' && v * 8 + (src[i] - '0') <= 0377; ++n) v = v * 8 + (src[i++] - '0');
        return v;
    }
    bool hexN(int n, uint32_t& v) {
        if (i + n > src.size()) return false;
        v = 0;
        for (int k = 0; k < n; ++k) {
            int d = hexVal(src[i + k]);
            if (d < 0) return false;
            v = v * 16 + d;
        }
        i += n;
        return true;
    }
    uint32_t charEscape() {  // just after the backslash
        char16_t c = src[i++];
        uint32_t v = 0;
        switch (c) {
            case 't': return 9;
            case 'n': return 10;
            case 'v': return 11;
            case 'f': return 12;
            case 'r': return 13;
            case '0':
                if (!(more() && isDigit(src[i]))) return 0;
                if (fl.unicode) fail("Invalid decimal escape");
                --i;
                return octal();
            case 'c':
                if (more() && ((src[i] | 0x20) >= 'a' && (src[i] | 0x20) <= 'z')) return src[i++] & 31;
                if (fl.unicode) fail("Invalid unicode escape");
                --i;
                return '\\';
            case 'x':
                if (hexN(2, v)) return v;
                if (fl.unicode) fail("Invalid escape");
                return 'x';
            case 'u': {
                if (fl.unicode && more() && src[i] == '{') {
                    size_t k = i + 1;
                    int n = 0;
                    while (k < src.size() && hexVal(src[k]) >= 0) {
                        v = v * 16 + hexVal(src[k++]);
                        if (v > 0x10FFFF) fail("Invalid Unicode escape");
                        ++n;
                    }
                    if (!n || k >= src.size() || src[k] != '}') fail("Invalid Unicode escape");
                    i = k + 1;
                    return v;
                }
                if (hexN(4, v)) {
                    if (fl.unicode && v >= 0xD800 && v <= 0xDBFF && i + 1 < src.size() && src[i] == '\\' && src[i + 1] == 'u') {
                        size_t save = i;
                        uint32_t lo = 0;
                        i += 2;
                        if (hexN(4, lo) && lo >= 0xDC00 && lo <= 0xDFFF) return 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                        i = save;
                    }
                    return v;
                }
                if (fl.unicode) fail("Invalid Unicode escape");
                return 'u';
            }
            default:
                if (c >= '1' && c <= '7' && !fl.unicode) {
                    --i;
                    return octal();
                }
                if (fl.unicode && !(c < 128 && strchr("^$\\.*+?()[]{}|/-", (char)c))) fail("Invalid escape");
                --i;
                return nextCp();
        }
    }
    void property(CharClass& cc, bool negated) {  // \p{...}
        if (!more() || src[i] != '{') fail("Invalid property name");
        ++i;
        std::string name;
        while (more() && src[i] != '}') name += (char)src[i++];
        if (!more()) fail("Invalid property name");
        ++i;
        int prop = findProp(name);
        if (prop < 0) fail("Invalid property name");
        cc.props.push_back({prop, negated});
    }
    RxNode* atomEscape() {
        ++i;
        if (!more()) fail("\\ at end of pattern");
        char16_t c = src[i];
        if (c == 'b' || c == 'B') {
            ++i;
            return mk(c == 'b' ? RxNode::WordB : RxNode::NWordB);
        }
        if (c >= '1' && c <= '9') {
            size_t save = i;
            uint64_t n = 0;
            while (more() && isDigit(src[i]) && n < 1000000) n = n * 10 + (src[i++] - '0');
            if (n < (uint64_t)totalCaps) {
                RxNode* r = mk(RxNode::BackRef);
                r->cap = (int)n;
                return r;
            }
            if (fl.unicode) fail("Invalid escape");
            i = save;
            if (c >= '8') {
                ++i;
                return chr(c);
            }
            return chr(octal());
        }
        if (c == 'k' && (hasNamed || fl.unicode)) {
            ++i;
            if (!more() || src[i] != '<') fail("Invalid named reference");
            ++i;
            RxNode* r = mk(RxNode::BackRef);
            while (more() && src[i] != '>') r->name += src[i++];
            if (!more()) fail("Invalid named reference");
            ++i;
            namedRefs.push_back(r);
            return r;
        }
        if (c == 'd' || c == 'D' || c == 'w' || c == 'W' || c == 's' || c == 'S') {
            ++i;
            CharClass cc;
            addEscapeClass(cc, c);
            return classNode(std::move(cc));
        }
        if ((c == 'p' || c == 'P') && fl.unicode) {
            ++i;
            CharClass cc;
            property(cc, c == 'P');
            return classNode(std::move(cc));
        }
        return chr(charEscape());
    }
    uint32_t classAtom(CharClass& cc, bool& isSet) {
        if (src[i] != '\\') return nextCp();
        ++i;
        if (!more()) fail("\\ at end of pattern");
        char16_t c = src[i];
        if (c == 'd' || c == 'D' || c == 'w' || c == 'W' || c == 's' || c == 'S') {
            ++i;
            addEscapeClass(cc, c);
            isSet = true;
            return 0;
        }
        if ((c == 'p' || c == 'P') && fl.unicode) {
            ++i;
            property(cc, c == 'P');
            isSet = true;
            return 0;
        }
        if (c == 'b') {
            ++i;
            return 8;
        }
        if (c == '-') {
            ++i;
            return '-';
        }
        return charEscape();
    }
    RxNode* charClass() {
        ++i;
        CharClass cc;
        if (more() && src[i] == '^') {
            cc.negate = true;
            ++i;
        }
        for (;;) {
            if (!more()) fail("Unterminated character class");
            if (src[i] == ']') {
                ++i;
                break;
            }
            bool isSet = false;
            uint32_t lo = classAtom(cc, isSet);
            if (isSet) continue;
            if (more() && src[i] == '-' && i + 1 < src.size() && src[i + 1] != ']') {
                ++i;
                bool isSet2 = false;
                uint32_t hi = classAtom(cc, isSet2);
                if (isSet2) {  // [a-\d]: the dash is literal
                    if (fl.unicode) fail("Invalid character class");
                    cc.ranges.push_back({lo, lo});
                    cc.ranges.push_back({'-', '-'});
                    continue;
                }
                if (hi < lo) fail("Range out of order in character class");
                cc.ranges.push_back({lo, hi});
                continue;
            }
            cc.ranges.push_back({lo, lo});
        }
        return classNode(std::move(cc));
    }
};

}  // namespace

struct RegexProgram {
    std::vector<Inst> code;
    std::vector<CharClass> classes;
    std::vector<std::pair<std::u16string, int>> names;
    RegexFlags flags;
    int ncaps = 1, nregs = 0;
    int firstChar = -1;        // every match starts with this code unit (-1: unknown)
    bool anchoredStart = false;  // pattern starts with ^ and is not multiline
};

namespace {

// ---------------------------------------------------------------------------- compiler
struct Compiler {
    Parser& P;
    RegexProgram& out;
    static constexpr uint64_t WIDE = 1ull << 40;

    size_t emit(uint8_t op, uint32_t x = 0, uint32_t y = 0, uint32_t z = 0, uint8_t flag = 0, uint16_t a = 0) {
        if (out.code.size() > 300000) P.fail("Regular expression too large");
        Inst in;
        in.op = op;
        in.flag = flag;
        in.a = a;
        in.x = x;
        in.y = y;
        in.z = z;
        out.code.push_back(in);
        return out.code.size() - 1;
    }
    uint32_t here() const { return (uint32_t)out.code.size(); }

    void width(RxNode* n, uint64_t& mn, uint64_t& mx) {
        mn = mx = 0;
        switch (n->t) {
            case RxNode::Char: mn = mx = n->c > 0xFFFF ? 2 : 1; break;
            case RxNode::Any:
            case RxNode::Class:
                mn = 1;
                mx = P.fl.unicode ? 2 : 1;
                break;
            case RxNode::BackRef: mx = WIDE; break;
            case RxNode::Seq:
                for (RxNode* k : n->kids) {
                    uint64_t a, b;
                    width(k, a, b);
                    mn += a;
                    mx = std::min(mx + b, WIDE);
                }
                break;
            case RxNode::Alt: {
                mn = WIDE;
                for (RxNode* k : n->kids) {
                    uint64_t a, b;
                    width(k, a, b);
                    mn = std::min(mn, a);
                    mx = std::max(mx, b);
                }
                break;
            }
            case RxNode::Group: width(n->kids[0], mn, mx); break;
            case RxNode::Repeat: {
                uint64_t a, b;
                width(n->kids[0], a, b);
                mn = std::min(a * n->min, WIDE);
                mx = b == 0 ? 0 : (n->max == INF ? WIDE : std::min(b * n->max, WIDE));
                break;
            }
            default: break;
        }
    }
    void capRange(RxNode* n, int& lo, int& hi) {
        if (n->t == RxNode::Group && n->cap >= 0) {
            lo = std::min(lo, n->cap);
            hi = std::max(hi, n->cap);
        }
        for (RxNode* k : n->kids) capRange(k, lo, hi);
    }
    bool single(RxNode* n) const { return n->t == RxNode::Char || n->t == RxNode::Any || n->t == RxNode::Class; }
    void one(RxNode* n, uint8_t& op, uint32_t& x) {
        if (n->t == RxNode::Char) {
            op = P.fl.ignoreCase ? I_CHAR_I : I_CHAR;
            x = P.fl.ignoreCase ? upperOf(n->c) : n->c;
        } else if (n->t == RxNode::Any) {
            op = I_ANY;
            x = 0;
        } else {
            op = I_CLASS;
            x = n->c;
        }
    }

    void gen(RxNode* n) {
        switch (n->t) {
            case RxNode::Empty: break;
            case RxNode::Char:
            case RxNode::Any:
            case RxNode::Class: {
                uint8_t op;
                uint32_t x;
                one(n, op, x);
                emit(op, x);
                break;
            }
            case RxNode::Seq:
                for (RxNode* k : n->kids) gen(k);
                break;
            case RxNode::Alt: {
                std::vector<size_t> jumps;
                for (size_t k = 0; k + 1 < n->kids.size(); ++k) {
                    size_t sp = emit(I_SPLIT);
                    gen(n->kids[k]);
                    jumps.push_back(emit(I_JMP));
                    out.code[sp].x = (uint32_t)sp + 1;
                    out.code[sp].y = here();
                }
                gen(n->kids.back());
                for (size_t j : jumps) out.code[j].x = here();
                break;
            }
            case RxNode::Group:
                if (n->cap >= 0) emit(I_SAVE, n->cap * 2);
                gen(n->kids[0]);
                if (n->cap >= 0) emit(I_SAVE, n->cap * 2 + 1);
                break;
            case RxNode::Bol: emit(I_BOL); break;
            case RxNode::Eol: emit(I_EOL); break;
            case RxNode::WordB: emit(I_WORDB); break;
            case RxNode::NWordB: emit(I_NWORDB); break;
            case RxNode::BackRef: emit(I_BACKREF, (uint32_t)n->cap); break;
            case RxNode::Look: {
                size_t at = emit(I_LOOK, 0, 0, 0, (uint8_t)((n->behind ? 1 : 0) | (n->negative ? 2 : 0)));
                gen(n->kids[0]);
                emit(I_LOOK_END);
                out.code[at].x = here();
                if (n->behind) {
                    uint64_t mn, mx;
                    width(n->kids[0], mn, mx);
                    out.code[at].y = (uint32_t)std::min<uint64_t>(mn, INF);
                    out.code[at].z = (uint32_t)std::min<uint64_t>(mx, INF);
                }
                break;
            }
            case RxNode::Repeat: repeat(n); break;
        }
    }
    void repeat(RxNode* n) {
        RxNode* c = n->kids[0];
        uint32_t mn = n->min, mx = n->max;
        if (mx == 0) return;
        if (single(c)) {
            uint8_t op;
            uint32_t x;
            one(c, op, x);
            emit(I_REP, x, mn, mx, n->greedy ? 1 : 0, op);
            return;
        }
        int lo = 1 << 30, hi = -1;
        capRange(c, lo, hi);
        uint64_t wmin, wmax;
        width(c, wmin, wmax);
        auto body = [&] {
            if (hi >= 0) emit(I_RESET, (uint32_t)lo * 2, (uint32_t)(hi + 1) * 2);
            gen(c);
        };
        for (uint32_t k = 0; k < mn; ++k) body();
        if (mx == INF) {
            size_t sp = emit(I_SPLIT);
            int reg = -1;
            if (wmin == 0) {  // a pass that matches nothing must not loop forever
                reg = out.nregs++;
                emit(I_LOOP_START, (uint32_t)reg);
            }
            body();
            if (reg >= 0) emit(I_LOOP_CHECK, (uint32_t)reg);
            emit(I_JMP, (uint32_t)sp);
            out.code[sp].x = n->greedy ? (uint32_t)sp + 1 : here();
            out.code[sp].y = n->greedy ? here() : (uint32_t)sp + 1;
            return;
        }
        std::vector<size_t> splits;
        for (uint32_t k = mn; k < mx; ++k) {
            splits.push_back(emit(I_SPLIT));
            body();
        }
        for (size_t sp : splits) {
            out.code[sp].x = n->greedy ? (uint32_t)sp + 1 : here();
            out.code[sp].y = n->greedy ? here() : (uint32_t)sp + 1;
        }
    }
};

// ---------------------------------------------------------------------------- matcher
enum : uint8_t { B_BRANCH, B_CAP, B_REG, B_GREEDY, B_LAZY };
struct Bt {
    uint8_t kind;
    uint32_t pc;
    int a, b;
};

struct Matcher {
    const RegexProgram& P;
    const char16_t* s;
    int len;
    std::vector<int>& caps;
    std::vector<int>& regs;
    std::vector<Bt>& stack;
    bool uni, icase;
    uint64_t fails = 0;
    bool aborted = false;

    uint32_t at(int pos, int& w) const {
        char16_t c = s[pos];
        if (uni && isHigh(c) && pos + 1 < len && isLow(s[pos + 1])) {
            w = 2;
            return 0x10000 + ((c - 0xD800) << 10) + (s[pos + 1] - 0xDC00);
        }
        w = 1;
        return c;
    }
    bool classHit(uint32_t idx, uint32_t cp) const {
        const CharClass& cc = P.classes[idx];
        return cp < 128 ? cc.ascii[cp] : classMatchSlow(cc, cp, icase);
    }
    // Tests one character with a single-character matcher (the operand of I_REP, or a plain instruction).
    bool one(uint8_t op, uint32_t x, int pos, int& w) const {
        if (pos >= len) return false;
        uint32_t cp = at(pos, w);
        switch (op) {
            case I_CHAR: return cp == x;
            case I_CHAR_I: return cp == x || upperOf(cp) == x;
            case I_ANY: return P.flags.dotAll || !isLineEnd(cp);
            default: return classHit(x, cp);
        }
    }
    void unwind(size_t base) {
        while (stack.size() > base) {
            Bt b = stack.back();
            stack.pop_back();
            if (b.kind == B_CAP) caps[b.a] = b.b;
            else if (b.kind == B_REG) regs[b.a] = b.b;
        }
    }

    // Runs from pc at pos. Returns true at I_MATCH / I_LOOK_END; on failure everything pushed above `base` is undone.
    bool run(uint32_t pc, int pos, size_t base, int lookEnd, int& endPos) {
        const Inst* code = P.code.data();
        for (;;) {
            const Inst& in = code[pc];
            int w = 1;
            switch (in.op) {
                case I_CHAR:
                case I_CHAR_I:
                case I_ANY:
                case I_CLASS:
                    if (!one(in.op, in.x, pos, w)) goto fail;
                    pos += w;
                    ++pc;
                    continue;
                case I_SPLIT:
                    stack.push_back({B_BRANCH, in.y, pos, 0});
                    pc = in.x;
                    continue;
                case I_JMP: pc = in.x; continue;
                case I_SAVE:
                    stack.push_back({B_CAP, 0, (int)in.x, caps[in.x]});
                    caps[in.x] = pos;
                    ++pc;
                    continue;
                case I_RESET:
                    for (uint32_t k = in.x; k < in.y; ++k)
                        if (caps[k] != -1) {
                            stack.push_back({B_CAP, 0, (int)k, caps[k]});
                            caps[k] = -1;
                        }
                    ++pc;
                    continue;
                case I_LOOP_START:
                    stack.push_back({B_REG, 0, (int)in.x, regs[in.x]});
                    regs[in.x] = pos;
                    ++pc;
                    continue;
                case I_LOOP_CHECK:
                    if (regs[in.x] == pos) goto fail;
                    ++pc;
                    continue;
                case I_BOL:
                    if (!(pos == 0 || (P.flags.multiline && isLineEnd(s[pos - 1])))) goto fail;
                    ++pc;
                    continue;
                case I_EOL:
                    if (!(pos == len || (P.flags.multiline && isLineEnd(s[pos])))) goto fail;
                    ++pc;
                    continue;
                case I_WORDB:
                case I_NWORDB: {
                    bool before = pos > 0 && isWord(s[pos - 1]), after = pos < len && isWord(s[pos]);
                    if ((before != after) != (in.op == I_WORDB)) goto fail;
                    ++pc;
                    continue;
                }
                case I_BACKREF: {
                    int a = caps[in.x * 2], b = caps[in.x * 2 + 1];
                    if (a >= 0 && b >= a) {
                        int n = b - a;
                        if (pos + n > len) goto fail;
                        for (int k = 0; k < n; ++k) {
                            char16_t p = s[a + k], q = s[pos + k];
                            if (p != q && !(icase && upperOf(p) == upperOf(q))) goto fail;
                        }
                        pos += n;
                    }
                    ++pc;
                    continue;
                }
                case I_REP: {
                    uint8_t op = (uint8_t)in.a;
                    int p = pos;
                    uint32_t count = 0;
                    if (in.flag) {  // greedy: take as many as possible, give them back one at a time
                        int minPos = in.y == 0 ? pos : -1;
                        while (count < in.z && one(op, in.x, p, w)) {
                            p += w;
                            if (++count == in.y) minPos = p;
                        }
                        if (count < in.y) goto fail;
                        if (p > minPos) stack.push_back({B_GREEDY, pc + 1, minPos, p});
                    } else {  // lazy: take the minimum, add one more each time we come back
                        for (; count < in.y; ++count) {
                            if (!one(op, in.x, p, w)) goto fail;
                            p += w;
                        }
                        if (count < in.z) stack.push_back({B_LAZY, pc, p, (int)count});
                    }
                    pos = p;
                    ++pc;
                    continue;
                }
                case I_LOOK: {
                    bool behind = (in.flag & 1) != 0, negative = (in.flag & 2) != 0;
                    size_t mark = stack.size();
                    bool ok = false;
                    int ignored = 0;
                    if (!behind) {
                        ok = run(pc + 1, pos, mark, -1, ignored);
                    } else {  // try every start from which the body could end exactly here
                        int hi = pos - (int)std::min<uint32_t>(in.y, 0x7FFFFFFF);
                        int lo = in.z >= (uint32_t)pos ? 0 : pos - (int)in.z;
                        for (int p = hi; p >= lo && !ok && !aborted; --p) ok = run(pc + 1, p, mark, pos, ignored);
                    }
                    if (aborted) goto fail;
                    if (negative) {
                        if (ok) {
                            unwind(mark);
                            goto fail;
                        }
                    } else {
                        if (!ok) goto fail;
                        // a lookaround is atomic: keep what it captured, forget its choice points
                        size_t keep = mark;
                        for (size_t r = mark; r < stack.size(); ++r)
                            if (stack[r].kind == B_CAP || stack[r].kind == B_REG) stack[keep++] = stack[r];
                        stack.resize(keep);
                    }
                    pc = in.x;
                    continue;
                }
                case I_LOOK_END:
                    if (lookEnd >= 0 && pos != lookEnd) goto fail;
                    return true;
                case I_MATCH:
                    endPos = pos;
                    return true;
                default: goto fail;
            }
        fail:
            if (++fails > 60000000) aborted = true;  // runaway backtracking: give up instead of hanging the page
            for (;;) {
                if (stack.size() <= base || aborted) {
                    unwind(base);
                    return false;
                }
                Bt b = stack.back();
                stack.pop_back();
                if (b.kind == B_CAP) {
                    caps[b.a] = b.b;
                } else if (b.kind == B_REG) {
                    regs[b.a] = b.b;
                } else if (b.kind == B_BRANCH) {
                    pc = b.pc;
                    pos = b.a;
                    break;
                } else if (b.kind == B_GREEDY) {
                    int np = b.b - 1;
                    if (uni && np > b.a && isLow(s[np]) && isHigh(s[np - 1])) --np;
                    if (np > b.a) stack.push_back({B_GREEDY, b.pc, b.a, np});
                    pc = b.pc;
                    pos = np;
                    break;
                } else {  // B_LAZY
                    const Inst& rep = code[b.pc];
                    int w2 = 1;
                    if (!one((uint8_t)rep.a, rep.x, b.a, w2)) continue;
                    uint32_t count = (uint32_t)b.b + 1;
                    if (count < rep.z) stack.push_back({B_LAZY, b.pc, b.a + w2, (int)count});
                    pc = b.pc + 1;
                    pos = b.a + w2;
                    break;
                }
            }
        }
    }
};

}  // namespace

std::shared_ptr<RegexProgram> regexCompile(const std::u16string& pattern, const RegexFlags& flags, std::string& error) {
    Parser parser{pattern, flags};
    auto prog = std::make_shared<RegexProgram>();
    try {
        RxNode* root = parser.parse();
        prog->flags = flags;
        Compiler c{parser, *prog};
        c.emit(I_SAVE, 0);
        c.gen(root);
        c.emit(I_SAVE, 1);
        c.emit(I_MATCH);
    } catch (ParseFail&) {
        error = parser.err;
        return nullptr;
    }
    prog->ncaps = parser.ncaps;
    prog->names = std::move(parser.names);
    prog->classes = std::move(parser.classes);
    for (auto& cc : prog->classes) finishClass(cc, flags.ignoreCase);
    const Inst& first = prog->code[1];
    if (first.op == I_CHAR && first.x <= 0xFFFF) prog->firstChar = (int)first.x;
    if (first.op == I_REP && first.a == I_CHAR && first.y >= 1 && first.x <= 0xFFFF) prog->firstChar = (int)first.x;
    if (first.op == I_BOL && !flags.multiline) prog->anchoredStart = true;
    return prog;
}

int regexGroupCount(const RegexProgram& p) { return p.ncaps; }
const std::vector<std::pair<std::u16string, int>>& regexGroupNames(const RegexProgram& p) { return p.names; }

bool regexExec(const RegexProgram& p, const std::u16string& s, size_t from, bool sticky, std::vector<int>& caps) {
    static thread_local std::vector<Bt> stack;
    static thread_local std::vector<int> regs;
    int len = (int)s.size();
    if (from > (size_t)len) return false;
    caps.assign((size_t)p.ncaps * 2, -1);
    regs.assign((size_t)p.nregs, -1);
    Matcher m{p, s.data(), len, caps, regs, stack, p.flags.unicode, p.flags.ignoreCase};
    for (int start = (int)from; start <= len; ++start) {
        if (!sticky && p.firstChar >= 0) {
            size_t f = s.find((char16_t)p.firstChar, (size_t)start);
            if (f == std::u16string::npos) return false;
            start = (int)f;
        }
        stack.clear();
        int end = 0;
        if (m.run(0, start, 0, -1, end)) {
            stack.clear();
            return true;
        }
        if (m.aborted || sticky || (p.anchoredStart && start == 0) || (p.anchoredStart && from > 0)) break;
        std::fill(caps.begin(), caps.end(), -1);
        if (p.flags.unicode && start + 1 < len && isHigh(s[start]) && isLow(s[start + 1])) ++start;
    }
    stack.clear();
    return false;
}

bool regexValidate(const std::u16string& pattern, const std::u16string& flags, std::string& error) {
    RegexFlags f;
    bool unicodeSets = false;
    for (char16_t c : flags) {
        bool* slot = c == 'g' ? &f.global : c == 'i' ? &f.ignoreCase : c == 'm' ? &f.multiline : c == 's' ? &f.dotAll : c == 'u' ? &f.unicode
                     : c == 'y' ? &f.sticky : c == 'd' ? &f.hasIndices : c == 'v' ? &unicodeSets : nullptr;
        if (!slot || *slot) {
            error = "Invalid regular expression flags";
            return false;
        }
        *slot = true;
    }
    if (unicodeSets && f.unicode) {
        error = "Invalid regular expression flags";
        return false;
    }
    if (unicodeSets) f.unicode = true;
    return regexCompile(pattern, f, error) != nullptr;
}

bool unicodeIdStart(uint32_t cp) { return hasProp(kUniIdStart, cp); }
bool unicodeIdContinue(uint32_t cp) { return hasProp(kUniIdContinue, cp); }

}  // namespace tb::js
