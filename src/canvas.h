#pragma once
// The <canvas> element's contexts (HTML 4.12.5): the 2D context (canvas2d.cpp, drawn with Direct2D on the GPU) and
// WebGL 1 (webgl.cpp). Both draw into a picture of their own; when a script returns, what changed is read back and
// handed to the page as the canvas's picture.
#include <cstdint>
#include <vector>

#include "js/js.h"

namespace tb {

struct Node;

// What the canvas contexts need from the page around them
struct CanvasHost {
    virtual ~CanvasHost() = default;
    virtual Node* nodeOf(js::Value v) = 0;    // the element a script's wrapper stands for, or null
    virtual js::Value wrapNode(Node* n) = 0;  // the script's wrapper of an element
    // An <img> or a <canvas> as non-premultiplied RGBA rows from the top: 1, or 0 when it has no picture (yet), 2
    // when the picture came from another origin (the pixels are given all the same: a 2D canvas may draw them, but
    // is then no longer origin-clean)
    virtual int imagePixels(Node* source, int& w, int& h, std::vector<uint8_t>& rgba) = 0;
    // The picture of an <img> as the page keeps it: premultiplied BGRA rows from the top, valid until the script
    // returns, and a number that is the same for the same picture. 0, 1 or 2 as imagePixels.
    virtual int imageBGRA(Node* img, int& w, int& h, const uint8_t*& bgra, int& id) = 0;
    [[noreturn]] virtual void securityError(const char* message) = 0;  // throws a SecurityError DOMException
    // A canvas has new contents: premultiplied BGRA rows from the top
    virtual void canvasPresented(Node* canvas, int w, int h, std::vector<uint8_t>&& bgra) = 0;
};

// The 2D context: defines __canvas2d(canvas, attributes), __canvasReset(canvas), __canvasDataURL(canvas, type,
// quality) for the prelude, and CanvasRenderingContext2D, CanvasGradient, CanvasPattern, Path2D and TextMetrics
void canvas2d_install(js::Vm& vm, js::JSObject* global, CanvasHost& host);
void canvas2d_present(js::Vm& vm);  // hands what changed since the last time to the host
void canvas2d_release(js::Vm& vm);
// The pixels a 2D canvas shows now, straight RGBA rows from the top; false when it has no 2D context. `clean` is
// false once something from another origin was drawn on it.
bool canvas2d_pixels(js::Vm& vm, Node* canvas, int& w, int& h, std::vector<uint8_t>& rgba, bool& clean);

}  // namespace tb
