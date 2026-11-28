#pragma once
// CSS Animations 1 and CSS Transitions 1, for the properties a compositor can run without laying the page out
// again: opacity and transform. They are worked out when the page is painted, from the time each one started.
#include <memory>
#include <string>
#include <vector>

#include "css.h"

namespace tb {

// A timing function (CSS Easing 1): linear, a cubic Bézier curve, or steps
struct Timing {
    enum K : uint8_t { Linear, Cubic, Steps } k = Cubic;
    float x1 = 0.25f, y1 = 0.1f, x2 = 0.25f, y2 = 1;  // ease
    int steps = 1;
    uint8_t stepPos = 1;  // 0 jump-start, 1 jump-end, 2 jump-none, 3 jump-both
    double apply(double t) const;
};
bool parse_timing(std::string_view v, Timing& out);

struct Keyframe {
    float offset = 0;
    bool hasOpacity = false, hasTransform = false, hasTiming = false;
    float opacity = 1;
    std::shared_ptr<const TransformList> transform;  // null: none
    Timing timing;                                   // animation-timing-function inside the keyframe
};
struct Keyframes {
    std::vector<Keyframe> frames;  // in offset order
    bool opacity = false, transform = false;
};
// One animation of animation-name and the other animation-* lists (CSS Animations 1, 3)
struct AnimationDecl {
    std::string name;
    float duration = 0, delay = 0;  // ms
    float iterations = 1;           // < 0: infinite
    uint8_t direction = 0;          // 0 normal, 1 reverse, 2 alternate, 3 alternate-reverse
    uint8_t fill = 0;               // 0 none, 1 forwards, 2 backwards, 3 both
    bool paused = false;
    Timing timing;
    std::shared_ptr<const Keyframes> frames;  // the @keyframes rule of that name; null when there is none
};
// One transition of the transition-* lists (CSS Transitions 1, 2)
struct TransitionDecl {
    std::string property;  // "all" or a property name
    float duration = 0, delay = 0;
    Timing timing;
};
// What runs on an element: when each animation started, and the transitions under way
struct NodeAnim {
    std::vector<std::pair<std::string, double>> starts;  // animation name, start time (now_ms)
    struct Transition {
        bool transform = false;  // else opacity
        float fromOpacity = 1, toOpacity = 1;
        std::shared_ptr<const TransformList> from, to;
        double start = 0;
        float duration = 0, delay = 0;
        Timing timing;
    };
    std::vector<Transition> transitions;
    // what the last styling gave, which a change starts a transition from
    bool styled = false;
    float lastOpacity = 1;
    std::shared_ptr<const TransformList> lastTransform;
    // for the events: each animation's last iteration seen and phase (0 before, 1 active, 2 after), -1 when not yet
    std::vector<std::pair<int, int>> seen;
};
// What a PushAnim display item paints with (see DItem)
struct AnimPaint {
    std::shared_ptr<const std::vector<AnimationDecl>> anims;
    NodeAnim state;
    float baseOpacity = 1;
    std::shared_ptr<const TransformList> baseTransform;
    Len origin[2];
    bool opacity = false, transform = false;  // which of the two it runs
};
// The values at `now`: opacity, and a transform (null when there is none). False when nothing is running any more
// (so that nothing needs painting again for them).
bool animate(const AnimPaint& a, double now, float& opacity, std::shared_ptr<const TransformList>& transform);
// Progress of one animation at `now` (CSS Animations 1, 4): the phase (0 before, 1 active, 2 after), the
// iteration, and the elapsed time in seconds as the events give it
void animation_phase(const AnimationDecl& d, double start, double now, int& phase, int& iteration, double& elapsed);

}  // namespace tb
