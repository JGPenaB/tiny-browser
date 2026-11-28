#pragma once
// WebGL 1 (Khronos WebGL 1.0): canvas.getContext("webgl") on the GPU's own OpenGL driver. Each context draws into a
// framebuffer object of the canvas's size in an OpenGL context of its own (on a hidden window); GLSL ES shaders are
// given to the driver as desktop GLSL 1.20. What a script drew is read back when the script returns and shown as the
// canvas's picture.
#include "canvas.h"

namespace tb {

// Defines __webglContext(canvas, type, attributes) for the prelude's getContext(), and the WebGL interfaces
void webgl_install(js::Vm& vm, js::JSObject* global, CanvasHost& host);
// Reads back every context of this VM that drew since the last time and hands the pixels to the host
void webgl_present(js::Vm& vm);
// Lets go of the contexts of a VM that is going away
void webgl_release(js::Vm& vm);
// The pixels a WebGL canvas shows now, for drawing it onto another canvas or texture
bool webgl_canvas_pixels(js::Vm& vm, Node* canvas, int& w, int& h, std::vector<uint8_t>& rgba);

}  // namespace tb
