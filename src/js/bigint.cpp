// BigInt (ECMA-262 6.1.6.2 and 21.2): whole numbers of any size, kept as a sign and a magnitude made of 32-bit
// limbs with the least significant limb first. A BigInt is never changed once made.
#include <algorithm>
#include <bit>

#include "builtins.h"
#include "bytecode.h"

namespace tb::js {

namespace {

using Mag = std::vector<uint32_t>;
const size_t kMaxLimbs = 1u << 20;  // about 33 million bits
const uint64_t kMaxBits = (uint64_t)kMaxLimbs * 32;

void trim(Mag& a) {
    while (!a.empty() && a.back() == 0) a.pop_back();
}
uint64_t bitLength(const Mag& a) { return a.empty() ? 0 : (uint64_t)(a.size() - 1) * 32 + (32 - std::countl_zero(a.back())); }
int cmpMag(const Mag& a, const Mag& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
Mag addMag(const Mag& a, const Mag& b) {
    const Mag& x = a.size() >= b.size() ? a : b;
    const Mag& y = a.size() >= b.size() ? b : a;
    Mag r(x.size() + 1);
    uint64_t carry = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        uint64_t s = (uint64_t)x[i] + (i < y.size() ? y[i] : 0) + carry;
        r[i] = (uint32_t)s;
        carry = s >> 32;
    }
    r[x.size()] = (uint32_t)carry;
    trim(r);
    return r;
}
Mag subMag(const Mag& a, const Mag& b) {  // a must not be smaller than b
    Mag r(a.size());
    int64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        int64_t s = (int64_t)a[i] - (i < b.size() ? b[i] : 0) - borrow;
        borrow = s < 0;
        r[i] = (uint32_t)s;
    }
    trim(r);
    return r;
}
Mag mulMag(const Mag& a, const Mag& b) {
    if (a.empty() || b.empty()) return {};
    Mag r(a.size() + b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t carry = 0, x = a[i];
        if (!x) continue;
        for (size_t j = 0; j < b.size(); ++j) {
            uint64_t t = x * b[j] + r[i + j] + carry;
            r[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        r[i + b.size()] = (uint32_t)carry;
    }
    trim(r);
    return r;
}
uint32_t divSmall(Mag& a, uint32_t d) {  // a /= d; returns the remainder
    uint64_t rem = 0;
    for (size_t i = a.size(); i-- > 0;) {
        uint64_t cur = (rem << 32) | a[i];
        a[i] = (uint32_t)(cur / d);
        rem = cur % d;
    }
    trim(a);
    return (uint32_t)rem;
}
void mulSmallAdd(Mag& a, uint32_t m, uint32_t add) {  // a = a * m + add
    uint64_t carry = add;
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t t = (uint64_t)a[i] * m + carry;
        a[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) a.push_back((uint32_t)carry);
}
Mag shlMag(const Mag& a, uint64_t n) {
    if (a.empty()) return {};
    size_t limbs = (size_t)(n / 32);
    int bits = (int)(n % 32);
    Mag r(a.size() + limbs + 1);
    for (size_t i = 0; i < a.size(); ++i) {
        r[i + limbs] |= a[i] << bits;
        if (bits) r[i + limbs + 1] |= a[i] >> (32 - bits);
    }
    trim(r);
    return r;
}
Mag shrMag(const Mag& a, uint64_t n) {  // drops the low bits
    size_t limbs = (size_t)(n / 32);
    int bits = (int)(n % 32);
    if (limbs >= a.size()) return {};
    Mag r(a.size() - limbs);
    for (size_t i = 0; i < r.size(); ++i) {
        r[i] = a[i + limbs] >> bits;
        if (bits && i + limbs + 1 < a.size()) r[i] |= a[i + limbs + 1] << (32 - bits);
    }
    trim(r);
    return r;
}
bool lowBitsSet(const Mag& a, uint64_t n) {  // is any of the lowest n bits set?
    size_t limbs = (size_t)(n / 32);
    for (size_t i = 0; i < limbs && i < a.size(); ++i)
        if (a[i]) return true;
    return (n % 32) && limbs < a.size() && (a[limbs] & ((1u << (n % 32)) - 1));
}
// Long division (Knuth's algorithm D). b must not be zero.
void divMod(const Mag& a, const Mag& b, Mag& q, Mag& r) {
    if (cmpMag(a, b) < 0) {
        q.clear();
        r = a;
        return;
    }
    if (b.size() == 1) {
        q = a;
        uint32_t rem = divSmall(q, b[0]);
        r.clear();
        if (rem) r.push_back(rem);
        return;
    }
    int s = std::countl_zero(b.back());
    Mag u = shlMag(a, s), v = shlMag(b, s);
    size_t n = v.size(), m = a.size() - n;
    u.resize(a.size() + 1);
    q.assign(m + 1, 0);
    const uint64_t B = 1ull << 32;
    for (size_t j = m + 1; j-- > 0;) {
        uint64_t num = ((uint64_t)u[j + n] << 32) | u[j + n - 1];
        uint64_t qhat = num / v[n - 1], rhat = num % v[n - 1];
        while (qhat >= B || qhat * v[n - 2] > ((rhat << 32) | u[j + n - 2])) {
            --qhat;
            rhat += v[n - 1];
            if (rhat >= B) break;
        }
        int64_t borrow = 0;
        uint64_t carry = 0;
        for (size_t i = 0; i < n; ++i) {
            uint64_t p = qhat * v[i] + carry;
            carry = p >> 32;
            int64_t t = (int64_t)u[i + j] - borrow - (int64_t)(p & 0xFFFFFFFFull);
            u[i + j] = (uint32_t)t;
            borrow = t < 0 ? 1 : 0;
        }
        int64_t t = (int64_t)u[j + n] - borrow - (int64_t)carry;
        u[j + n] = (uint32_t)t;
        if (t < 0) {  // the estimate was one too large: add the divisor back
            --qhat;
            uint64_t c = 0;
            for (size_t i = 0; i < n; ++i) {
                uint64_t sum = (uint64_t)u[i + j] + v[i] + c;
                u[i + j] = (uint32_t)sum;
                c = sum >> 32;
            }
            u[j + n] += (uint32_t)c;
        }
        q[j] = (uint32_t)qhat;
    }
    trim(q);
    u.resize(n);
    r = shrMag(u, s);
}

[[noreturn]] void tooBig(Vm& vm) { vm.throwError(Err::RangeError, "Maximum BigInt size exceeded"); }

JSBigInt* make(Vm& vm, bool neg, Mag&& d) {
    trim(d);
    if (d.size() > kMaxLimbs) tooBig(vm);
    JSBigInt* b = vm.alloc<JSBigInt>();
    b->neg = neg && !d.empty();
    b->d = std::move(d);
    vm.gc.noteExternal(b->d.size() * 4);
    return b;
}

Mag magFromU64(uint64_t v) {
    Mag d;
    if (v) d.push_back((uint32_t)v);
    if (v >> 32) d.push_back((uint32_t)(v >> 32));
    return d;
}
Mag magFromDouble(double d) {  // d is a whole number, not negative
    if (d < 18446744073709551616.0) return magFromU64((uint64_t)d);
    int e;
    double m = std::frexp(d, &e);
    return shlMag(magFromU64((uint64_t)std::ldexp(m, 53)), (uint64_t)(e - 53));
}

JSBigInt* addSigned(Vm& vm, const JSBigInt* a, const JSBigInt* b, bool bneg) {
    if (a->neg == bneg) return make(vm, a->neg, addMag(a->d, b->d));
    int c = cmpMag(a->d, b->d);
    if (c == 0) return make(vm, false, {});
    return c > 0 ? make(vm, a->neg, subMag(a->d, b->d)) : make(vm, bneg, subMag(b->d, a->d));
}

// Two's complement form with the given number of limbs, and back. The sign is the top bit of the last limb.
Mag twos(const JSBigInt* a, size_t len) {
    Mag r(len, 0);
    std::copy(a->d.begin(), a->d.begin() + std::min(len, a->d.size()), r.begin());
    if (a->neg) {
        uint64_t carry = 1;
        for (uint32_t& w : r) {
            uint64_t t = (uint64_t)(uint32_t)~w + carry;
            w = (uint32_t)t;
            carry = t >> 32;
        }
    }
    return r;
}
JSBigInt* fromTwos(Vm& vm, Mag&& r) {
    bool neg = !r.empty() && (r.back() >> 31);
    if (neg) {
        uint64_t carry = 1;
        for (uint32_t& w : r) {
            uint64_t t = (uint64_t)(uint32_t)~w + carry;
            w = (uint32_t)t;
            carry = t >> 32;
        }
    }
    return make(vm, neg, std::move(r));
}

JSBigInt* bitwise(Vm& vm, int op, const JSBigInt* a, const JSBigInt* b) {
    size_t len = std::max(a->d.size(), b->d.size()) + 1;
    Mag x = twos(a, len), y = twos(b, len);
    for (size_t i = 0; i < len; ++i) x[i] = op == OP_BitAnd ? (x[i] & y[i]) : op == OP_BitOr ? (x[i] | y[i]) : (x[i] ^ y[i]);
    return fromTwos(vm, std::move(x));
}

JSBigInt* shift(Vm& vm, JSBigInt* x, const JSBigInt* y, bool left) {
    if (x->d.empty() || y->d.empty()) return x;
    if (left != y->neg) {
        if (y->d.size() > 1 || (uint64_t)y->d[0] + bitLength(x->d) > kMaxBits) tooBig(vm);
        return make(vm, x->neg, shlMag(x->d, y->d[0]));
    }
    // a right shift rounds toward minus infinity
    if (y->d.size() > 1 || y->d[0] >= bitLength(x->d)) return make(vm, x->neg, x->neg ? Mag{1} : Mag{});
    Mag r = shrMag(x->d, y->d[0]);
    if (x->neg && lowBitsSet(x->d, y->d[0])) r = addMag(r, Mag{1});
    return make(vm, x->neg, std::move(r));
}

JSBigInt* power(Vm& vm, JSBigInt* base, const JSBigInt* e) {
    if (e->neg) vm.throwError(Err::RangeError, "Exponent must be non-negative");
    if (e->d.empty()) return make(vm, false, Mag{1});
    if (base->d.empty()) return base;
    bool neg = base->neg && (e->d[0] & 1);
    if (base->d.size() == 1 && base->d[0] == 1) return make(vm, neg, Mag{1});
    if (e->d.size() > 1 || (bitLength(base->d) - 1) * e->d[0] > kMaxBits) tooBig(vm);
    Mag result{1}, b = base->d;
    for (uint32_t n = e->d[0]; n;) {
        if (n & 1) result = mulMag(result, b);
        n >>= 1;
        if (n) b = mulMag(b, b);
    }
    return make(vm, neg, std::move(result));
}

int digitOf(char16_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}
// Reads digits of the given radix; false when a character is not one (or there are none).
bool parseMag(const char16_t* p, size_t n, int radix, Mag& out) {
    out.clear();
    if (n == 0) return false;
    uint32_t mul = 1, chunk = 0;
    for (size_t i = 0; i < n; ++i) {
        int h = digitOf(p[i]);
        if (h >= radix) return false;
        if ((uint64_t)mul * radix > 0xFFFFFFFFull) {
            mulSmallAdd(out, mul, chunk);
            mul = 1;
            chunk = 0;
        }
        chunk = chunk * radix + h;
        mul *= radix;
    }
    mulSmallAdd(out, mul, chunk);
    trim(out);
    return true;
}
int prefixRadix(const std::u16string& s, size_t i, size_t n) {
    if (n - i > 2 && s[i] == '0') {
        char16_t c = s[i + 1] | 0x20;
        if (c == 'x') return 16;
        if (c == 'o') return 8;
        if (c == 'b') return 2;
    }
    return 10;
}

// ToIndex (7.1.22)
double toIndex(Vm& vm, Value v) {
    if (v.isUndef()) return 0;
    double d = vm.toInteger(v);
    if (d < 0 || d > 9007199254740991.0) vm.throwError(Err::RangeError, "Invalid index");
    return d;
}

JSBigInt* asUintN(Vm& vm, double bits, JSBigInt* x) {
    if (bits == 0 || x->d.empty()) return make(vm, false, {});
    if (!x->neg && (double)bitLength(x->d) <= bits) return x;
    if (bits > (double)kMaxBits) tooBig(vm);
    size_t nb = (size_t)bits, limbs = (nb + 31) / 32;
    Mag t = twos(x, std::max(limbs, x->d.size() + 1));
    t.resize(limbs);
    if (nb % 32) t.back() &= (1u << (nb % 32)) - 1;
    return make(vm, false, std::move(t));
}
JSBigInt* asIntN(Vm& vm, double bits, JSBigInt* x) {
    if (bits == 0 || x->d.empty()) return make(vm, false, {});
    uint64_t bl = bitLength(x->d);
    if ((double)bl < bits) return x;  // the magnitude is below 2^(bits-1)
    if (x->neg && (double)bl == bits && !lowBitsSet(x->d, bl - 1)) return x;  // exactly -2^(bits-1)
    size_t nb = (size_t)bits, limbs = (nb + 31) / 32;  // bits is at most bl here
    Mag t = twos(x, std::max(limbs, x->d.size() + 1));
    t.resize(limbs);
    if (nb % 32) {  // spread the sign bit over the rest of the top limb
        uint32_t top = t.back(), mask = (1u << (nb % 32)) - 1;
        t.back() = ((top >> ((nb - 1) % 32)) & 1) ? (top | ~mask) : (top & mask);
    }
    return fromTwos(vm, std::move(t));
}

// ---------------------------------------------------------------------------- the BigInt built-in
JSBigInt* thisBigInt(CallInfo& ci) {
    if (ci.thisv.isBigInt()) return ci.thisv.asBigInt();
    if (ci.thisv.isObject() && ci.thisv.asObject()->cls == Cls::BigInt) return ((JSPrimitive*)ci.thisv.asObject())->prim.asBigInt();
    ci.vm.throwError(Err::TypeError, "Receiver is not a BigInt");
}
JSFN(bigintCtor) {
    Vm& vm = ci.vm;
    if (ci.isConstruct()) vm.throwError(Err::TypeError, "BigInt is not a constructor");
    Value prim = vm.toPrimitive(ci.arg(0), 1);
    if (prim.isNumber()) {
        double d = prim.number();
        if (!std::isfinite(d) || d != std::trunc(d))
            vm.throwError(Err::RangeError, "The number %s cannot be converted to a BigInt because it is not an integer", utf16to8(vm.numberToString(d)->s).c_str());
        return V(bigFromDouble(vm, d));
    }
    return V(vm.toBigInt(prim));
}
JSFN(bigintAsIntN) {
    double bits = toIndex(ci.vm, ci.arg(0));
    return V(asIntN(ci.vm, bits, ci.vm.toBigInt(ci.arg(1))));
}
JSFN(bigintAsUintN) {
    double bits = toIndex(ci.vm, ci.arg(0));
    return V(asUintN(ci.vm, bits, ci.vm.toBigInt(ci.arg(1))));
}
JSFN(bigintToString) {
    Vm& vm = ci.vm;
    JSBigInt* b = thisBigInt(ci);
    double radix = 10;
    if (!ci.arg(0).isUndef()) {
        radix = vm.toInteger(ci.arg(0));
        if (radix < 2 || radix > 36) vm.throwError(Err::RangeError, "toString() radix must be between 2 and 36");
    }
    return vm.strv(bigToString(b, (int)radix));
}
JSFN(bigintToLocaleString) { return callIntl(ci.vm, "numberToLocaleString", {V(thisBigInt(ci)), ci.arg(0), ci.arg(1)}); }  // ECMA-402 20.3.1
JSFN(bigintValueOf) { return V(thisBigInt(ci)); }

}  // namespace

// ---------------------------------------------------------------------------- conversions
JSBigInt* bigFromI64(Vm& vm, int64_t v) { return make(vm, v < 0, magFromU64(v < 0 ? 0 - (uint64_t)v : (uint64_t)v)); }
JSBigInt* bigFromU64(Vm& vm, uint64_t v) { return make(vm, false, magFromU64(v)); }
JSBigInt* bigFromDouble(Vm& vm, double d) { return make(vm, d < 0, magFromDouble(std::fabs(d))); }

uint64_t bigToU64(const JSBigInt* b) {
    uint64_t v = (b->d.size() > 0 ? b->d[0] : 0) | ((uint64_t)(b->d.size() > 1 ? b->d[1] : 0) << 32);
    return b->neg ? 0 - v : v;
}

double bigToDouble(const JSBigInt* b) {
    const Mag& d = b->d;
    if (d.empty()) return 0;
    uint64_t bl = bitLength(d);
    double r;
    if (bl <= 64) {
        r = (double)(d[0] | (d.size() > 1 ? (uint64_t)d[1] << 32 : 0));
    } else if (bl > 1024) {
        r = INFINITY;
    } else {
        // The top 64 bits decide the result; one extra low bit records whether anything below them was set, so
        // that the conversion rounds a tie the right way.
        Mag top = shrMag(d, bl - 64);
        uint64_t v = top[0] | ((uint64_t)top[1] << 32);
        if (lowBitsSet(d, bl - 64)) v |= 1;
        r = std::ldexp((double)v, (int)(bl - 64));
    }
    return b->neg ? -r : r;
}

std::u16string bigToString(const JSBigInt* b, int radix) {
    if (b->d.empty()) return u"0";
    static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::u16string out;
    if ((radix & (radix - 1)) == 0) {
        int shift = std::countr_zero((unsigned)radix);
        uint64_t bl = bitLength(b->d);
        for (uint64_t pos = 0; pos < bl; pos += shift) {
            uint32_t v = 0;
            for (int k = 0; k < shift; ++k) {
                uint64_t bit = pos + k;
                if (bit < bl && ((b->d[(size_t)(bit / 32)] >> (bit % 32)) & 1)) v |= 1u << k;
            }
            out += (char16_t)kDigits[v];
        }
    } else {
        uint32_t chunk = (uint32_t)radix;
        int k = 1;
        while ((uint64_t)chunk * radix <= 0xFFFFFFFFull) {
            chunk *= radix;
            ++k;
        }
        Mag t = b->d;
        while (!t.empty()) {
            uint32_t rem = divSmall(t, chunk);
            for (int i = 0; i < k; ++i) {
                if (t.empty() && rem == 0) break;
                out += (char16_t)kDigits[rem % radix];
                rem /= radix;
            }
        }
    }
    if (b->neg) out += u'-';
    std::reverse(out.begin(), out.end());
    return out;
}

// A literal from source text: decimal digits, or 0x / 0o / 0b followed by digits. Separators are already gone.
JSBigInt* bigFromLiteral(Vm& vm, const std::u16string& text) {
    int radix = prefixRadix(text, 0, text.size());
    Mag d;
    size_t skip = radix == 10 ? 0 : 2;
    parseMag(text.data() + skip, text.size() - skip, radix, d);
    return make(vm, false, std::move(d));
}

// StringToBigInt (7.1.14): null when the text is not a whole number.
JSBigInt* stringToBigInt(Vm& vm, const std::u16string& s) {
    size_t i = 0, n = s.size();
    while (i < n && isJsSpace(s[i])) ++i;
    while (n > i && isJsSpace(s[n - 1])) --n;
    if (i == n) return make(vm, false, {});
    Mag d;
    int radix = prefixRadix(s, i, n);
    if (radix != 10) {
        if (!parseMag(s.data() + i + 2, n - i - 2, radix, d)) return nullptr;
        return make(vm, false, std::move(d));
    }
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') {
        neg = s[i] == '-';
        ++i;
    }
    if (!parseMag(s.data() + i, n - i, 10, d)) return nullptr;
    return make(vm, neg, std::move(d));
}

bool bigEquals(const JSBigInt* a, const JSBigInt* b) { return a->neg == b->neg && a->d == b->d; }
int bigCompare(const JSBigInt* a, const JSBigInt* b) {
    if (a->neg != b->neg) return a->neg ? -1 : 1;
    int c = cmpMag(a->d, b->d);
    return a->neg ? -c : c;
}
// d must not be NaN
int bigCompareDouble(const JSBigInt* b, double d) {
    if (d == INFINITY) return -1;
    if (d == -INFINITY) return 1;
    if (b->d.empty()) return d > 0 ? -1 : d < 0 ? 1 : 0;
    if (d == 0 || b->neg != (d < 0)) return b->neg ? -1 : 1;
    double ad = std::fabs(d), whole = std::trunc(ad);
    int c = 1;
    if (ad >= 1) {
        c = cmpMag(b->d, magFromDouble(whole));
        if (c == 0 && ad != whole) c = -1;
    }
    return b->neg ? -c : c;
}
size_t bigHash(const JSBigInt* b) {
    size_t h = b->neg ? 0x9E3779B97F4A7C15ull : 0;
    for (uint32_t w : b->d) h = h * 1099511628211ull + w;
    return h;
}

// ---------------------------------------------------------------------------- Vm: numeric operators
// ToNumeric (7.1.3)
Value Vm::toNumeric(Value v) {
    if (v.isNumber() || v.isBigInt()) return v;
    Value p = toPrimitive(v, 1);
    if (p.isBigInt()) return p;
    return Value::num(toNumber(p));
}

// ToBigInt (7.1.13)
JSBigInt* Vm::toBigInt(Value v) {
    Value p = toPrimitive(v, 1);
    if (p.isBigInt()) return p.asBigInt();
    if (p.isBool()) return make(*this, false, p.isTrue() ? Mag{1} : Mag{});
    if (p.isString()) {
        JSBigInt* b = stringToBigInt(*this, p.asString()->s);
        if (!b) throwError(Err::SyntaxError, "Cannot convert %s to a BigInt", utf16to8(p.asString()->s).c_str());
        return b;
    }
    throwError(Err::TypeError, "Cannot convert %s to a BigInt", utf16to8(inspect(p, 0)).c_str());
}

static double numberPow(double base, double e) {  // Number::exponentiate (6.1.6.1.3)
    if (e != e) return NAN;
    if (std::isinf(e) && std::fabs(base) == 1) return NAN;
    return std::pow(base, e);
}

// The binary operators other than +, for operands that are not both numbers (13.15.3 ApplyStringOrNumericBinaryOperator).
Value Vm::arith(int op, Value x, Value y) {
    Value a = toNumeric(x);
    Value b = toNumeric(y);
    if (a.isNumber() && b.isNumber()) {
        double p = a.number(), q = b.number();
        switch (op) {
            case OP_Add: return Value::num(p + q);
            case OP_Sub: return Value::num(p - q);
            case OP_Mul: return Value::num(p * q);
            case OP_Div: return Value::num(p / q);
            case OP_Mod: return Value::num(std::fmod(p, q));
            case OP_Exp: return Value::num(numberPow(p, q));
            case OP_BitAnd: return Value::i32(toInt32(a) & toInt32(b));
            case OP_BitOr: return Value::i32(toInt32(a) | toInt32(b));
            case OP_BitXor: return Value::i32(toInt32(a) ^ toInt32(b));
            case OP_Shl: return Value::i32((int32_t)((uint32_t)toInt32(a) << (toInt32(b) & 31)));
            case OP_Shr: return Value::i32(toInt32(a) >> (toInt32(b) & 31));
            default: return Value::num((double)((uint32_t)toInt32(a) >> (toInt32(b) & 31)));
        }
    }
    if (!a.isBigInt() || !b.isBigInt()) throwError(Err::TypeError, "Cannot mix BigInt and other types, use explicit conversions");
    JSBigInt* p = a.asBigInt();
    JSBigInt* q = b.asBigInt();
    switch (op) {
        case OP_Add: return V(addSigned(*this, p, q, q->neg));
        case OP_Sub: return V(addSigned(*this, p, q, !q->neg));
        case OP_Mul: return V(make(*this, p->neg != q->neg, mulMag(p->d, q->d)));
        case OP_Div:
        case OP_Mod: {
            if (q->d.empty()) throwError(Err::RangeError, "Division by zero");
            Mag quot, rem;
            divMod(p->d, q->d, quot, rem);
            return op == OP_Div ? V(make(*this, p->neg != q->neg, std::move(quot))) : V(make(*this, p->neg, std::move(rem)));
        }
        case OP_Exp: return V(power(*this, p, q));
        case OP_BitAnd:
        case OP_BitOr:
        case OP_BitXor: return V(bitwise(*this, op, p, q));
        case OP_Shl: return V(shift(*this, p, q, true));
        case OP_Shr: return V(shift(*this, p, q, false));
        default: throwError(Err::TypeError, "BigInts have no unsigned right shift, use >> instead");
    }
}

// Unary -, ~, ++ and -- for an operand that is not a number.
Value Vm::unaryNumeric(int op, Value x) {
    Value a = toNumeric(x);
    if (a.isNumber()) {
        switch (op) {
            case OP_Neg: return Value::num(-a.number());
            case OP_BitNot: return Value::i32(~toInt32(a));
            case OP_Inc: return Value::num(a.number() + 1);
            default: return Value::num(a.number() - 1);
        }
    }
    JSBigInt* p = a.asBigInt();
    static const Mag one{1};
    JSBigInt unit;
    unit.d = one;
    switch (op) {
        case OP_Neg: return V(make(*this, !p->neg, Mag(p->d)));
        case OP_BitNot: return V(p->neg ? make(*this, false, subMag(p->d, one)) : make(*this, true, addMag(p->d, one)));  // -x - 1
        case OP_Inc: return V(addSigned(*this, p, &unit, false));
        default: return V(addSigned(*this, p, &unit, true));
    }
}

void setupBigInt(Vm& vm) {
    vm.bigintProto = vm.newObject(vm.objectProto);
    JSNative* c = makeCtor(vm, "BigInt", bigintCtor, 1, vm.bigintProto);
    vm.defFn(c, "asIntN", bigintAsIntN, 2);
    vm.defFn(c, "asUintN", bigintAsUintN, 2);
    vm.defFn(vm.bigintProto, "toString", bigintToString, 0);
    vm.defFn(vm.bigintProto, "toLocaleString", bigintToLocaleString, 0);
    vm.defFn(vm.bigintProto, "valueOf", bigintValueOf, 0);
    vm.defineOwn(vm.bigintProto, PropKey::of(vm.symToStringTag), vm.strv("BigInt"), PF_C);
}

}  // namespace tb::js
