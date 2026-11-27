// Property descriptors and the rules for defining properties (ECMA-262 6.2.6, 10.1.6, 10.4.2), including what
// makes arrays special: `length`, and elements that carry their own attributes or sit far beyond the dense part.
#include <algorithm>
#include <cmath>

#include "builtins.h"

namespace tb::js {

namespace {

// Writes past the end by more than this many slots are kept as individual properties instead of growing the
// dense element store.
const uint32_t kSparseGap = 1u << 22;

bool isArrayLike(JSObject* o) { return o->cls == Cls::Array || o->cls == Cls::Arguments; }

uint8_t elementFlags(JSObject* o) {
    if (o->oflags & OF_FROZEN) return PF_E;
    if (o->oflags & OF_SEALED) return PF_W | PF_E;
    return PF_ALL;
}

// Puts a data property where it belongs. Array elements with the default attributes live in the dense store;
// anything else is an ordinary property, which switches the array to its slower, fully general form.
void storeData(Vm& vm, JSObject* o, PropKey k, Value v, uint8_t flags) {
    if (k.isIndex() && (o == vm.arrayProto || o == vm.objectProto)) vm.protoIndexProps = true;
    if (Value* slot = vm.mappedArg(o, k)) {
        *slot = v;
        if (!(flags & PF_W)) vm.unmapArg(o, k);
    }
    if (isArrayLike(o) && k.isIndex()) {
        JSArray* arr = (JSArray*)o;
        uint32_t i = k.asIndex();
        size_t size = arr->elems.size();
        if (i >= size) vm.pinArgumentsLength(o);
        bool dense = flags == PF_ALL && elementFlags(o) == PF_ALL && (i < size || i - size <= kSparseGap);
        if (dense) {
            if (o->oflags & OF_SLOWIDX) o->remove(k);
            if (i >= size) arr->elems.resize((size_t)i + 1, Value::empty());
            arr->elems[i] = v;
            return;
        }
        o->oflags |= OF_SLOWIDX;
        if (i < size) arr->elems[i] = Value::empty();
        if (arr->slowLen < i + 1 && o->cls == Cls::Array) arr->slowLen = i + 1;
    }
    if (k.isIndex() && (o == vm.arrayProto || o == vm.objectProto)) vm.protoIndexProps = true;
    if (Prop* p = o->find(k)) {
        p->val = v;
        p->flags = flags;
    } else
        o->add(k, v, flags);
}

void storeAccessor(Vm& vm, JSObject* o, PropKey k, Value getter, Value setter, uint8_t flags) {
    vm.unmapArg(o, k);
    if (isArrayLike(o) && k.isIndex()) {
        JSArray* arr = (JSArray*)o;
        uint32_t i = k.asIndex();
        o->oflags |= OF_SLOWIDX;
        if (i < arr->elems.size()) arr->elems[i] = Value::empty();
        if (arr->slowLen < i + 1 && o->cls == Cls::Array) arr->slowLen = i + 1;
    }
    if (k.isIndex() && (o == vm.arrayProto || o == vm.objectProto)) vm.protoIndexProps = true;
    Accessor* acc = vm.alloc<Accessor>();
    acc->get = getter;
    acc->set = setter;
    uint8_t f = (uint8_t)(PF_ACC | (flags & (PF_E | PF_C)));
    if (Prop* p = o->find(k)) {
        p->val = Value::cell(acc);
        p->flags = f;
    } else
        o->add(k, Value::cell(acc), f);
}

// ValidateAndApplyPropertyDescriptor (10.1.6.3) for an object whose storage is described by getOwn/store*.
bool ordinaryDefine(Vm& vm, JSObject* o, PropKey k, const PropDesc& d) {
    Prop cur{};
    bool exists = vm.getOwn(o, k, cur);
    bool dAcc = d.hasGet || d.hasSet, dData = d.hasValue || d.hasWritable;
    if (!exists) {
        if (o->oflags & OF_NOEXT) return false;
        uint8_t flags = (uint8_t)((d.hasEnumerable && d.enumerable ? PF_E : 0) | (d.hasConfigurable && d.configurable ? PF_C : 0));
        if (dAcc) storeAccessor(vm, o, k, d.hasGet ? d.get : Value::undef(), d.hasSet ? d.set : Value::undef(), flags);
        else storeData(vm, o, k, d.hasValue ? d.value : Value::undef(), (uint8_t)(flags | (d.hasWritable && d.writable ? PF_W : 0)));
        return true;
    }
    bool curAcc = (cur.flags & PF_ACC) != 0, curC = (cur.flags & PF_C) != 0, curE = (cur.flags & PF_E) != 0, curW = (cur.flags & PF_W) != 0;
    Accessor* acc = curAcc ? (Accessor*)cur.val.asCell() : nullptr;
    if (!dAcc && !dData && !d.hasEnumerable && !d.hasConfigurable) return true;
    if (!curC) {
        if (d.hasConfigurable && d.configurable) return false;
        if (d.hasEnumerable && d.enumerable != curE) return false;
        if ((dAcc || dData) && dAcc != curAcc) return false;
        if (curAcc) {
            if (d.hasGet && !vm.sameValue(d.get, acc->get)) return false;
            if (d.hasSet && !vm.sameValue(d.set, acc->set)) return false;
        } else if (!curW) {
            if (d.hasWritable && d.writable) return false;
            if (d.hasValue && !vm.sameValue(d.value, cur.val)) return false;
        }
    }
    uint8_t flags = (uint8_t)(((d.hasEnumerable ? d.enumerable : curE) ? PF_E : 0) | ((d.hasConfigurable ? d.configurable : curC) ? PF_C : 0));
    if (dAcc) {
        storeAccessor(vm, o, k, d.hasGet ? d.get : (curAcc ? acc->get : Value::undef()), d.hasSet ? d.set : (curAcc ? acc->set : Value::undef()), flags);
    } else if (dData || !curAcc) {
        Value v = d.hasValue ? d.value : (curAcc ? Value::undef() : cur.val);
        bool w = d.hasWritable ? d.writable : (curAcc ? false : curW);
        uint8_t f = (uint8_t)(flags | (w ? PF_W : 0));
        // values that are not stored (a string's characters, a function's name) only pass validation unchanged
        if (o->cls == Cls::String && !o->find(k) && !(cur.flags & PF_C)) return true;
        if (f == cur.flags && !d.hasValue) return true;
        storeData(vm, o, k, v, f);
    } else {
        storeAccessor(vm, o, k, acc->get, acc->set, flags);
    }
    return true;
}

// ArraySetLength (10.4.2.4)
bool arraySetLength(Vm& vm, JSArray* arr, const PropDesc& d) {
    uint32_t newLen = 0;
    if (d.hasValue) {  // converted twice, as the specification does, and before anything is checked
        newLen = vm.toUint32(d.value);
        double numberLen = vm.toNumber(d.value);
        if ((double)newLen != numberLen) vm.throwError(Err::RangeError, "Invalid array length");
    }
    bool lengthRO = (arr->oflags & (OF_LENRO | OF_FROZEN)) != 0;
    if (d.hasGet || d.hasSet) return false;
    if (d.hasConfigurable && d.configurable) return false;
    if (d.hasEnumerable && d.enumerable) return false;
    if (!d.hasValue) {
        if (d.hasWritable) {
            if (d.writable && lengthRO) return false;
            if (!d.writable) arr->oflags |= OF_LENRO;
        }
        return true;
    }
    uint32_t oldLen = arrayLen(arr);
    if (d.hasWritable && d.writable && lengthRO) return false;
    if (newLen == oldLen) {
        if (d.hasWritable && !d.writable) arr->oflags |= OF_LENRO;
        return true;
    }
    if (lengthRO) return false;
    bool ok = true;
    uint32_t finalLen = newLen;
    if (newLen > oldLen) {
        if (newLen - arr->elems.size() > kSparseGap || (arr->oflags & OF_SLOWIDX)) {
            arr->oflags |= OF_SLOWIDX;
            arr->slowLen = newLen;
        } else
            arr->elems.resize(newLen, Value::empty());
    } else {
        // delete from the top down; the first property that cannot go becomes the new end
        if (arr->oflags & OF_SLOWIDX) {
            std::vector<uint32_t> high;
            uint32_t floor = std::max<uint32_t>(newLen, (uint32_t)std::min<size_t>(arr->elems.size(), 0xFFFFFFFFu));
            for (auto& p : arr->props)
                if (p.key.isIndex() && p.key.asIndex() >= floor) high.push_back(p.key.asIndex());
            std::sort(high.begin(), high.end(), [](uint32_t x, uint32_t y) { return x > y; });
            for (uint32_t i : high) {
                Prop* p = arr->find(PropKey::index(i));
                if (p && !(p->flags & PF_C)) {
                    finalLen = i + 1;
                    ok = false;
                    break;
                }
                arr->remove(PropKey::index(i));
            }
        }
        if (ok && arr->elems.size() > newLen) {
            bool fixed = elementFlags(arr) != PF_ALL;
            bool slow = (arr->oflags & OF_SLOWIDX) != 0;
            for (size_t i = arr->elems.size(); i-- > newLen;) {
                if (!arr->elems[i].isEmpty()) {
                    if (fixed) {
                        finalLen = (uint32_t)i + 1;
                        ok = false;
                        break;
                    }
                } else if (slow) {
                    Prop* p = arr->find(PropKey::index((uint32_t)i));
                    if (p && !(p->flags & PF_C)) {
                        finalLen = (uint32_t)i + 1;
                        ok = false;
                        break;
                    }
                    if (p) arr->remove(PropKey::index((uint32_t)i));
                }
            }
        }
        if (arr->elems.size() > finalLen) arr->elems.resize(finalLen);
        if (arr->oflags & OF_SLOWIDX) arr->slowLen = finalLen;
    }
    if (d.hasWritable && !d.writable) arr->oflags |= OF_LENRO;
    return ok;
}

}  // namespace

// The validation half of ValidateAndApplyPropertyDescriptor: may `d` be applied where `current` is (or nothing)
bool Vm::compatibleDesc(bool extensible, const PropDesc& d, bool exists, const PropDesc& cur) {
    if (!exists) return extensible;
    bool dAcc = d.hasGet || d.hasSet, dData = d.hasValue || d.hasWritable;
    bool curAcc = cur.hasGet || cur.hasSet;
    if (!dAcc && !dData && !d.hasEnumerable && !d.hasConfigurable) return true;
    if (!cur.configurable) {
        if (d.hasConfigurable && d.configurable) return false;
        if (d.hasEnumerable && d.enumerable != cur.enumerable) return false;
        if ((dAcc || dData) && dAcc != curAcc) return false;
        if (curAcc) {
            if (d.hasGet && !sameValue(d.get, cur.get)) return false;
            if (d.hasSet && !sameValue(d.set, cur.set)) return false;
        } else if (!cur.writable) {
            if (d.hasWritable && d.writable) return false;
            if (d.hasValue && !sameValue(d.value, cur.value)) return false;
        }
    }
    return true;
}

bool Vm::getOwnDesc(JSObject* o, PropKey k, PropDesc& d) {
    if (o->cls == Cls::Proxy) return proxyGetOwn(o, k, d);
    Prop p{};
    if (!getOwn(o, k, p)) return false;
    d = PropDesc{};
    if (p.flags & PF_ACC) {
        Accessor* acc = (Accessor*)p.val.asCell();
        d.get = acc->get;
        d.set = acc->set;
        d.hasGet = d.hasSet = true;
    } else {
        d.value = p.val;
        d.hasValue = d.hasWritable = true;
        d.writable = (p.flags & PF_W) != 0;
    }
    d.hasEnumerable = d.hasConfigurable = true;
    d.enumerable = (p.flags & PF_E) != 0;
    d.configurable = (p.flags & PF_C) != 0;
    return true;
}

// [[DefineOwnProperty]]: false when the object refuses the change.
bool Vm::defineOwnDesc(JSObject* o, PropKey k, const PropDesc& d) {
    if (o->cls == Cls::Proxy) return proxyDefineOwn(o, k, d);
    if (o->cls == Cls::Host && !k.isSymbol() && ((JSHost*)o)->hostKind == kModuleHostKind) {  // 10.4.6.6: nothing about an export can change
        JSHost* h = (JSHost*)o;
        Value current;
        if (!h->hooks || !h->hooks->get || !h->hooks->get(*this, o, k, current)) return false;
        if ((d.hasConfigurable && d.configurable) || (d.hasEnumerable && !d.enumerable) || d.hasGet || d.hasSet || (d.hasWritable && !d.writable)) return false;
        return !d.hasValue || sameValue(d.value, current);
    }
    if (o->cls == Cls::Array) {
        JSArray* arr = (JSArray*)o;
        if (!k.isIndex() && !k.isSymbol() && k.cell() == a.length) return arraySetLength(*this, arr, d);
        if (k.isIndex() && k.asIndex() >= arrayLen(arr) && (o->oflags & (OF_LENRO | OF_FROZEN))) return false;
    }
    if (o->cls == Cls::TypedArray) {  // 10.4.5.3: elements are always writable, enumerable, configurable data
        JSTypedArray* t = (JSTypedArray*)o;
        if (int kind = typedKeyKind(*this, t, k)) {
            if (kind == 2) return false;
            if (t->buffer->immutable) {  // nothing about an element of an immutable buffer can change
                PropDesc cur;
                cur.hasValue = cur.hasWritable = cur.hasEnumerable = cur.hasConfigurable = cur.enumerable = true;
                cur.value = typedGet(*this, t, k.asIndex());
                return compatibleDesc(false, d, true, cur);
            }
            if ((d.hasConfigurable && !d.configurable) || (d.hasEnumerable && !d.enumerable) || d.hasGet || d.hasSet || (d.hasWritable && !d.writable)) return false;
            if (d.hasValue) typedSet(*this, t, k.asIndex(), d.value);
            return true;
        }
    }
    return ordinaryDefine(*this, o, k, d);
}

// ToPropertyDescriptor (6.2.6.5)
void Vm::toPropDesc(Value v, PropDesc& d) {
    if (!v.isObject()) throwError(Err::TypeError, "Property description must be an object: %s", utf16to8(inspect(v, 0)).c_str());
    JSObject* o = v.asObject();
    d = PropDesc{};
    auto field = [&](JSString* name, Value& out) {
        PropKey fk = PropKey::of(name);
        if (!has(o, fk)) return false;
        out = getObj(o, fk, v);
        return true;
    };
    Value tmp;
    if (field(a.enumerable, tmp)) d.hasEnumerable = true, d.enumerable = toBool(tmp);
    if (field(a.configurable, tmp)) d.hasConfigurable = true, d.configurable = toBool(tmp);
    if (field(a.value, tmp)) d.hasValue = true, d.value = tmp;
    if (field(a.writable, tmp)) d.hasWritable = true, d.writable = toBool(tmp);
    if (field(a.get, tmp)) {
        if (!tmp.isUndef() && !isCallable(tmp)) throwError(Err::TypeError, "Getter must be a function: %s", utf16to8(inspect(tmp, 0)).c_str());
        d.hasGet = true, d.get = tmp;
    }
    if (field(a.set, tmp)) {
        if (!tmp.isUndef() && !isCallable(tmp)) throwError(Err::TypeError, "Setter must be a function: %s", utf16to8(inspect(tmp, 0)).c_str());
        d.hasSet = true, d.set = tmp;
    }
    if ((d.hasGet || d.hasSet) && (d.hasValue || d.hasWritable))
        throwError(Err::TypeError, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
}

// FromPropertyDescriptor (6.2.6.4)
Value Vm::fromPropDesc(const PropDesc& d) {
    JSObject* o = newObject();
    Value out = Value::cell(o);
    if (d.hasValue) o->add(PropKey::of(a.value), d.value, PF_ALL);
    if (d.hasWritable) o->add(PropKey::of(a.writable), Value::boolean(d.writable), PF_ALL);
    if (d.hasGet) o->add(PropKey::of(a.get), d.get, PF_ALL);
    if (d.hasSet) o->add(PropKey::of(a.set), d.set, PF_ALL);
    if (d.hasEnumerable) o->add(PropKey::of(a.enumerable), Value::boolean(d.enumerable), PF_ALL);
    if (d.hasConfigurable) o->add(PropKey::of(a.configurable), Value::boolean(d.configurable), PF_ALL);
    return out;
}

// SetIntegrityLevel (7.3.15): sealed, or frozen as well
bool Vm::setIntegrity(JSObject* o, bool frozen) {
    if (o->cls == Cls::Proxy) {  // the general algorithm, through the traps
        if (!preventExtensions(o)) return false;
        std::vector<PropKey> keys;
        proxyOwnKeys(o, keys);
        for (PropKey k : keys) {
            PropDesc d;
            d.hasConfigurable = true;
            if (frozen) {
                PropDesc cur;
                if (!getOwnDesc(o, k, cur)) continue;
                if (cur.hasValue || cur.hasWritable) d.hasWritable = true;
            }
            if (!defineOwnDesc(o, k, d)) throwError(Err::TypeError, u"Cannot redefine property: " + toStr16(keyToValue(k)));
        }
        return true;
    }
    if (frozen && o->cls == Cls::Host && ((JSHost*)o)->hostKind == kModuleHostKind) {  // an export cannot be made read-only (10.4.6.6)
        JSHost* h = (JSHost*)o;
        std::vector<PropKey> exported;
        if (h->hooks && h->hooks->keys) h->hooks->keys(*this, o, exported);
        if (!exported.empty()) return false;
    }
    if (!preventExtensions(o)) return false;
    if (o->cls == Cls::TypedArray && typedLength((JSTypedArray*)o) > 0 && !((JSTypedArray*)o)->buffer->immutable)  // an element cannot be made non-configurable
        throwError(Err::TypeError, frozen ? "Cannot freeze array buffer views with elements" : "Cannot seal array buffer views with elements");
    if (frozen && o->cls == Cls::Arguments) ((JSArguments*)o)->mapCtx = nullptr;  // read-only elements are no longer tied to parameters
    if (isArrayLike(o)) o->oflags |= frozen ? (OF_FROZEN | OF_SEALED) : OF_SEALED;  // covers the dense elements
    else if (frozen) o->oflags |= OF_FROZEN;
    auto fix = [&] {  // private members are not properties: freezing leaves them alone
        for (auto& p : o->props) {
            if (p.key.isSymbol() && ((Symbol*)p.key.cell())->isPrivate) continue;
            p.flags &= ~PF_C;
            if (frozen && !(p.flags & PF_ACC)) p.flags &= ~PF_W;
        }
    };
    fix();
    if (o->cls == Cls::Function || o->cls == Cls::Native) {  // name and length exist without being stored
        for (JSString* n : {a.name, a.length, a.arguments, callerAtom}) {
            Prop p{};
            PropKey k = PropKey::of(n);
            if (!o->find(k) && getOwn(o, k, p)) o->add(k, n == callerAtom ? Value::null() : p.val, 0);
        }
        if (o->cls == Cls::Function) fnProtoProp((JSFunction*)o);
        fix();
    }
    return true;
}

// TestIntegrityLevel (7.3.16)
bool Vm::testIntegrity(JSObject* o, bool frozen) {
    if (isExtensible(o)) return false;
    std::vector<PropKey> keys;
    ownKeys(o, keys, true, true, false);
    for (PropKey k : keys) {
        Prop p{};
        if (!getOwn(o, k, p)) continue;
        if (p.flags & PF_C) return false;
        if (frozen && !(p.flags & PF_ACC) && (p.flags & PF_W)) return false;
    }
    return true;
}

// CreateDataProperty on an array-like's index, used once the fast paths have been ruled out.
bool Vm::defineElement(JSObject* o, PropKey k, Value v, uint8_t flags) {
    PropDesc d;
    d.hasValue = d.hasWritable = d.hasEnumerable = d.hasConfigurable = true;
    d.value = v;
    d.writable = (flags & PF_W) != 0;
    d.enumerable = (flags & PF_E) != 0;
    d.configurable = (flags & PF_C) != 0;
    return defineOwnDesc(o, k, d);
}

}  // namespace tb::js
