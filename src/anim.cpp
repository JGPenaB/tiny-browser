#include "anim.h"

#include <algorithm>
#include <cmath>

#include "common.h"

namespace tb {

// CSS Easing 1, 2.2-2.3: the output progress for an input progress t in [0, 1]
double Timing::apply(double t) const {
    if (k == Linear) return t;
    if (k == Steps) {
        int jumps = stepPos == 2 ? steps - 1 : stepPos == 3 ? steps + 1 : steps;
        if (jumps <= 0) return t;
        double step = std::floor(t * steps);
        if (stepPos == 0 || stepPos == 3) step += 1;
        if (t >= 0 && step < 0) step = 0;
        if (t <= 1 && step > jumps) step = jumps;
        return step / jumps;
    }
    // a cubic Bézier from (0,0) to (1,1): find the curve parameter whose x is t, then its y
    auto bez = [](double a, double b, double s) { return 3 * a * s * (1 - s) * (1 - s) + 3 * b * s * s * (1 - s) + s * s * s; };
    if (t <= 0 || t >= 1) return t;
    double lo = 0, hi = 1, s = t;
    for (int i = 0; i < 40; ++i) {
        s = (lo + hi) / 2;
        if (bez(x1, x2, s) < t) lo = s;
        else hi = s;
    }
    return bez(y1, y2, s);
}

bool parse_timing(std::string_view v, Timing& out) {
    std::string s(v);
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    Timing t;
    if (s == "linear") t.k = Timing::Linear;
    else if (s == "ease") t = Timing{};
    else if (s == "ease-in") t.x1 = 0.42f, t.y1 = 0, t.x2 = 1, t.y2 = 1;
    else if (s == "ease-out") t.x1 = 0, t.y1 = 0, t.x2 = 0.58f, t.y2 = 1;
    else if (s == "ease-in-out") t.x1 = 0.42f, t.y1 = 0, t.x2 = 0.58f, t.y2 = 1;
    else if (s == "step-start") t.k = Timing::Steps, t.steps = 1, t.stepPos = 0;
    else if (s == "step-end") t.k = Timing::Steps, t.steps = 1, t.stepPos = 1;
    else if (s.rfind("cubic-bezier(", 0) == 0 && s.back() == ')') {
        float v4[4];
        std::string inner = s.substr(13, s.size() - 14);
        for (auto& ch : inner)
            if (ch == ',') ch = ' ';
        if (sscanf_s(inner.c_str(), "%f %f %f %f", &v4[0], &v4[1], &v4[2], &v4[3]) != 4) return false;
        if (v4[0] < 0 || v4[0] > 1 || v4[2] < 0 || v4[2] > 1) return false;
        t.x1 = v4[0], t.y1 = v4[1], t.x2 = v4[2], t.y2 = v4[3];
    } else if (s.rfind("steps(", 0) == 0 && s.back() == ')') {
        std::string inner = s.substr(6, s.size() - 7);
        size_t comma = inner.find(',');
        t.k = Timing::Steps;
        t.steps = atoi(inner.c_str());
        if (t.steps < 1) return false;
        if (comma != std::string::npos) {
            std::string pos = inner.substr(comma + 1);
            pos.erase(0, pos.find_first_not_of(' '));
            t.stepPos = pos == "jump-start" || pos == "start" ? 0 : pos == "jump-none" ? 2 : pos == "jump-both" ? 3 : 1;
            if (t.stepPos == 2 && t.steps < 2) return false;
        }
    } else if (s.rfind("linear(", 0) == 0) {
        t.k = Timing::Linear;  // a piecewise linear function: taken as plain linear
    } else
        return false;
    out = t;
    return true;
}

void animation_phase(const AnimationDecl& d, double start, double now, int& phase, int& iteration, double& elapsed) {
    double local = now - start - d.delay;
    double active = d.iterations < 0 ? INFINITY : d.duration * d.iterations;
    iteration = 0;
    if (local < 0) {
        phase = 0;
        elapsed = 0;
        return;
    }
    if (local >= active) {
        phase = 2;
        iteration = d.iterations < 0 ? 0 : std::max(0, (int)std::ceil(d.iterations) - 1);
        elapsed = active / 1000;
        return;
    }
    phase = 1;
    iteration = d.duration > 0 ? (int)std::floor(local / d.duration) : 0;
    elapsed = local / 1000;
}

namespace {

// The progress through the current iteration of an animation, with its direction (CSS Animations 1, 4.2), or
// false when it has no effect now (before or after its active time without a fill mode that holds it)
bool animation_progress(const AnimationDecl& d, double start, double now, double& progress) {
    double local = now - start - d.delay;
    double active = d.iterations < 0 ? INFINITY : d.duration * d.iterations;
    double iterProgress;
    double iteration;
    if (local < 0) {
        if (!(d.fill & 2)) return false;
        iteration = 0;
        iterProgress = 0;
    } else if (local >= active) {
        if (!(d.fill & 1)) return false;
        double total = d.iterations < 0 ? 0 : d.iterations;
        iteration = std::ceil(total) - 1;
        iterProgress = total - std::floor(total);
        if (iterProgress == 0) iterProgress = 1;
        if (iteration < 0) iteration = 0;
        if (d.duration <= 0) iteration = std::max(0.0, total - 1), iterProgress = total > 0 ? 1 : 0;
    } else {
        if (d.duration <= 0) return false;
        iteration = std::floor(local / d.duration);
        iterProgress = (local - iteration * d.duration) / d.duration;
    }
    bool reverse = d.direction == 1 || (d.direction == 2 && std::fmod(iteration, 2) == 1) || (d.direction == 3 && std::fmod(iteration, 2) == 0);
    progress = reverse ? 1 - iterProgress : iterProgress;
    return true;
}

// Interpolation of transform lists (CSS Transforms 1, 9): function by function when the two lists have the same
// functions, else through their matrices
std::shared_ptr<const TransformList> mix(const std::shared_ptr<const TransformList>& a, const std::shared_ptr<const TransformList>& b, double t) {
    static const TransformList empty;
    const TransformList& A = a ? *a : empty;
    const TransformList& B = b ? *b : empty;
    auto lerp = [&](float x, float y) { return (float)(x + (y - x) * t); };
    auto lerpLen = [&](const Len& x, const Len& y) {
        Len r;
        r.u = x.u == Len::Pct || y.u == Len::Pct ? Len::Pct : Len::Px;
        float xp = x.u == Len::Pct ? x.v : 0, yp = y.u == Len::Pct ? y.v : 0;
        float xo = x.u == Len::Pct ? x.off : x.v, yo = y.u == Len::Pct ? y.off : y.v;
        if (r.u == Len::Pct) r.v = lerp(xp, yp), r.off = lerp(xo, yo);
        else r.v = lerp(xo, yo);
        return r;
    };
    auto out = std::make_shared<TransformList>();
    bool same = A.size() == B.size() || A.empty() || B.empty();
    for (size_t i = 0; same && i < std::max(A.size(), B.size()); ++i)
        if (i < A.size() && i < B.size() && A[i].k != B[i].k) same = false;
    if (same) {
        for (size_t i = 0; i < std::max(A.size(), B.size()); ++i) {
            TransformFn ident;  // the identity of the other's kind fills in for a missing function
            const TransformFn* fa = i < A.size() ? &A[i] : nullptr;
            const TransformFn* fb = i < B.size() ? &B[i] : nullptr;
            TransformFn x = fa ? *fa : ident, y = fb ? *fb : ident;
            if (!fa) x.k = y.k, x.tx = Len{Len::Px, 0}, x.ty = Len{Len::Px, 0}, x.angle = 0, x.ax = x.k == TransformFn::Scale ? 1.f : 0.f, x.ay = x.ax;
            if (!fb) y.k = x.k, y.tx = Len{Len::Px, 0}, y.ty = Len{Len::Px, 0}, y.angle = 0, y.ax = y.k == TransformFn::Scale ? 1.f : 0.f, y.ay = y.ax;
            TransformFn r = x;
            switch (x.k) {
                case TransformFn::Translate:
                    r.tx = lerpLen(x.tx, y.tx);
                    r.ty = lerpLen(x.ty, y.ty);
                    break;
                case TransformFn::Rotate: r.angle = lerp(x.angle, y.angle); r.set_rotate(r.angle); break;
                case TransformFn::Scale: r.ax = lerp(x.ax, y.ax), r.ay = lerp(x.ay, y.ay); r.set_scale(r.ax, r.ay); break;
                case TransformFn::Skew: r.ax = lerp(x.ax, y.ax), r.ay = lerp(x.ay, y.ay); r.set_skew(r.ax, r.ay); break;
                default:
                    for (int k = 0; k < 6; ++k) r.v[k] = lerp(x.v[k], y.v[k]);
            }
            out->push_back(r);
        }
        return out;
    }
    // different functions: the matrices, entry by entry (an approximation of decomposing them)
    TransformFn r;
    float ma[6], mb[6];
    transform_matrix(A, 0, 0, ma);
    transform_matrix(B, 0, 0, mb);
    for (int k = 0; k < 6; ++k) r.v[k] = lerp(ma[k], mb[k]);
    out->push_back(r);
    return out;
}

// A property's value from the keyframes at `progress`, with the element's own value standing in for missing
// keyframes at 0% and 100% (CSS Animations 1, 3.1)
template <class V, class Get, class Has, class Mix>
V sample(const AnimationDecl& d, double progress, const V& base, Get get, Has has, Mix mixer) {
    const auto& frames = d.frames->frames;
    std::vector<const Keyframe*> list;
    for (auto& f : frames)
        if (has(f)) list.push_back(&f);
    struct P {
        float offset;
        V v;
        const Timing* timing;
    };
    std::vector<P> pts;
    if (list.empty() || list.front()->offset > 0) pts.push_back({0, base, &d.timing});
    for (auto* f : list) pts.push_back({f->offset, get(*f), f->hasTiming ? &f->timing : &d.timing});
    if (pts.back().offset < 1) pts.push_back({1, base, &d.timing});
    if (progress <= pts.front().offset) return pts.front().v;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        if (progress <= pts[i + 1].offset || i + 2 == pts.size()) {
            float span = pts[i + 1].offset - pts[i].offset;
            double local = span > 0 ? (progress - pts[i].offset) / span : 1;
            return mixer(pts[i].v, pts[i + 1].v, pts[i].timing->apply(std::clamp(local, 0.0, 1.0)));
        }
    return pts.back().v;
}

}  // namespace

bool animate(const AnimPaint& a, double now, float& opacity, std::shared_ptr<const TransformList>& transform) {
    opacity = a.baseOpacity;
    transform = a.baseTransform;
    bool running = false;
    // transitions first: animations override them (CSS Transitions 1, 4)
    for (auto& t : a.state.transitions) {
        double local = now - t.start - t.delay;
        double p = local <= 0 ? 0 : t.duration <= 0 ? 1 : std::min(1.0, local / t.duration);
        if (p < 1) running = true;
        double e = t.timing.apply(p);
        if (t.transform) transform = mix(t.from, t.to, e);
        else opacity = (float)(t.fromOpacity + (t.toOpacity - t.fromOpacity) * e);
    }
    if (a.anims)
        for (auto& d : *a.anims) {
            if (!d.frames || d.paused) continue;
            double start = now;
            for (auto& [name, at] : a.state.starts)
                if (name == d.name) start = at;
            double progress;
            if (d.iterations < 0 || now - start - d.delay < d.duration * d.iterations) running = true;
            if (!animation_progress(d, start, now, progress)) continue;
            if (d.frames->opacity && a.opacity)
                opacity = sample(d, progress, opacity, [](const Keyframe& f) { return f.opacity; }, [](const Keyframe& f) { return f.hasOpacity; },
                                 [](float x, float y, double t) { return (float)(x + (y - x) * t); });
            if (d.frames->transform && a.transform)
                transform = sample(d, progress, transform, [](const Keyframe& f) { return f.transform; },
                                   [](const Keyframe& f) { return f.hasTransform; },
                                   [](const std::shared_ptr<const TransformList>& x, const std::shared_ptr<const TransformList>& y, double t) { return mix(x, y, t); });
        }
    opacity = std::clamp(opacity, 0.f, 1.f);
    return running;
}

}  // namespace tb
