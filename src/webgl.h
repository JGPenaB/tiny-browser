#pragma once
// WebGL 1 (Khronos WebGL 1.0): canvas.getContext("webgl") on the GPU's own OpenGL driver. Each context draws into a
// framebuffer object of the canvas's size in an OpenGL context of its own (on a hidden window); GLSL ES shaders are
// given to the driver as desktop GLSL 1.20. What a script drew is read back when the script returns and shown as the
// canvas's picture.
#include <cstdint>
#include <vector>

#include "js/js.h"

namespace tb {

struct Node;

// What the WebGL contexts need from the page around them
struct CanvasHost {
    virtual ~CanvasHost() = default;
    virtual Node* nodeOf(js::Value v) = 0;    // the element a script's wrapper stands for, or null
    virtual js::Value wrapNode(Node* n) = 0;  // the script's wrapper of an element
    // An <img> or a <canvas> as non-premultiplied RGBA rows from the top: 1, or 0 when it has no picture (yet), 2
    // when the picture came from another origin and must not be read
    virtual int imagePixels(Node* source, int& w, int& h, std::vector<uint8_t>& rgba) = 0;
    [[noreturn]] virtual void securityError(const char* message) = 0;  // throws a SecurityError DOMException
    // A canvas has new contents: premultiplied BGRA rows from the top
    virtual void canvasPresented(Node* canvas, int w, int h, std::vector<uint8_t>&& bgra) = 0;
};

// Defines __webglContext(canvas, type, attributes) for the prelude's getContext(), and the WebGL interfaces
void webgl_install(js::Vm& vm, js::JSObject* global, CanvasHost& host);
// Reads back every context of this VM that drew since the last time and hands the pixels to the host
void webgl_present(js::Vm& vm);
// Lets go of the contexts of a VM that is going away
void webgl_release(js::Vm& vm);
// The pixels a WebGL canvas shows now, for drawing it onto another canvas or texture
bool webgl_canvas_pixels(js::Vm& vm, Node* canvas, int& w, int& h, std::vector<uint8_t>& rgba);

}  // namespace tb
