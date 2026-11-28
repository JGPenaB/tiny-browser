// WebGL 1 on OpenGL (see webgl.h). The WebGL calls map one to one onto OpenGL 2.0 with framebuffer objects; what
// WebGL defines on top (its objects, its errors, the default framebuffer, pixel unpacking, the strings and
// parameters it reports) is done here.
#include "webgl.h"

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>

#include "common.h"
#include "dom.h"
#include "js/builtins.h"

#pragma comment(lib, "opengl32.lib")

namespace tb {

using namespace js;

// ---------------------------------------------------------------- OpenGL past 1.1, loaded from the driver
using GLchar = char;
using GLsizeiptr = ptrdiff_t;
using GLintptr = ptrdiff_t;
#define GLFN(ret, name, ...) using PFN_##name = ret(APIENTRY*)(__VA_ARGS__); static PFN_##name name##_ = nullptr;
GLFN(void, glActiveTexture, GLenum)
GLFN(void, glAttachShader, GLuint, GLuint)
GLFN(void, glBindAttribLocation, GLuint, GLuint, const GLchar*)
GLFN(void, glBindBuffer, GLenum, GLuint)
GLFN(void, glBindFramebuffer, GLenum, GLuint)
GLFN(void, glBindRenderbuffer, GLenum, GLuint)
GLFN(void, glBlendColor, GLfloat, GLfloat, GLfloat, GLfloat)
GLFN(void, glBlendEquation, GLenum)
GLFN(void, glBlendEquationSeparate, GLenum, GLenum)
GLFN(void, glBlendFuncSeparate, GLenum, GLenum, GLenum, GLenum)
GLFN(void, glBufferData, GLenum, GLsizeiptr, const void*, GLenum)
GLFN(void, glBufferSubData, GLenum, GLintptr, GLsizeiptr, const void*)
GLFN(GLenum, glCheckFramebufferStatus, GLenum)
GLFN(void, glCompileShader, GLuint)
GLFN(GLuint, glCreateProgram, void)
GLFN(GLuint, glCreateShader, GLenum)
GLFN(void, glDeleteBuffers, GLsizei, const GLuint*)
GLFN(void, glDeleteFramebuffers, GLsizei, const GLuint*)
GLFN(void, glDeleteProgram, GLuint)
GLFN(void, glDeleteRenderbuffers, GLsizei, const GLuint*)
GLFN(void, glDeleteShader, GLuint)
GLFN(void, glDetachShader, GLuint, GLuint)
GLFN(void, glDisableVertexAttribArray, GLuint)
GLFN(void, glEnableVertexAttribArray, GLuint)
GLFN(void, glFramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)
GLFN(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)
GLFN(void, glGenBuffers, GLsizei, GLuint*)
GLFN(void, glGenFramebuffers, GLsizei, GLuint*)
GLFN(void, glGenRenderbuffers, GLsizei, GLuint*)
GLFN(void, glGenerateMipmap, GLenum)
GLFN(void, glGetActiveAttrib, GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*)
GLFN(void, glGetActiveUniform, GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*)
GLFN(GLint, glGetAttribLocation, GLuint, const GLchar*)
GLFN(void, glGetBufferParameteriv, GLenum, GLenum, GLint*)
GLFN(void, glGetFramebufferAttachmentParameteriv, GLenum, GLenum, GLenum, GLint*)
GLFN(void, glGetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)
GLFN(void, glGetProgramiv, GLuint, GLenum, GLint*)
GLFN(void, glGetRenderbufferParameteriv, GLenum, GLenum, GLint*)
GLFN(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)
GLFN(void, glGetShaderiv, GLuint, GLenum, GLint*)
GLFN(void, glGetUniformfv, GLuint, GLint, GLfloat*)
GLFN(void, glGetUniformiv, GLuint, GLint, GLint*)
GLFN(GLint, glGetUniformLocation, GLuint, const GLchar*)
GLFN(void, glGetVertexAttribfv, GLuint, GLenum, GLfloat*)
GLFN(void, glGetVertexAttribiv, GLuint, GLenum, GLint*)
GLFN(void, glGetVertexAttribPointerv, GLuint, GLenum, void**)
GLFN(void, glLinkProgram, GLuint)
GLFN(void, glRenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)
GLFN(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)
GLFN(void, glStencilFuncSeparate, GLenum, GLenum, GLint, GLuint)
GLFN(void, glStencilMaskSeparate, GLenum, GLuint)
GLFN(void, glStencilOpSeparate, GLenum, GLenum, GLenum, GLenum)
GLFN(void, glUniform1fv, GLint, GLsizei, const GLfloat*)
GLFN(void, glUniform2fv, GLint, GLsizei, const GLfloat*)
GLFN(void, glUniform3fv, GLint, GLsizei, const GLfloat*)
GLFN(void, glUniform4fv, GLint, GLsizei, const GLfloat*)
GLFN(void, glUniform1iv, GLint, GLsizei, const GLint*)
GLFN(void, glUniform2iv, GLint, GLsizei, const GLint*)
GLFN(void, glUniform3iv, GLint, GLsizei, const GLint*)
GLFN(void, glUniform4iv, GLint, GLsizei, const GLint*)
GLFN(void, glUniformMatrix2fv, GLint, GLsizei, GLboolean, const GLfloat*)
GLFN(void, glUniformMatrix3fv, GLint, GLsizei, GLboolean, const GLfloat*)
GLFN(void, glUniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat*)
GLFN(void, glUseProgram, GLuint)
GLFN(void, glValidateProgram, GLuint)
GLFN(void, glVertexAttrib4fv, GLuint, const GLfloat*)
GLFN(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)
GLFN(void, glGenVertexArrays, GLsizei, GLuint*)
GLFN(void, glBindVertexArray, GLuint)
GLFN(void, glDeleteVertexArrays, GLsizei, const GLuint*)
GLFN(void, glDrawArraysInstanced, GLenum, GLint, GLsizei, GLsizei)
GLFN(void, glDrawElementsInstanced, GLenum, GLsizei, GLenum, const void*, GLsizei)
GLFN(void, glVertexAttribDivisor, GLuint, GLuint)
GLFN(void, glGetShaderPrecisionFormat, GLenum, GLenum, GLint*, GLint*)
#undef GLFN

// the constants of OpenGL 2.0 and of WebGL that gl.h (1.1) leaves out
enum : GLenum {
    kArrayBuffer = 0x8892, kElementArrayBuffer = 0x8893, kArrayBufferBinding = 0x8894, kElementArrayBufferBinding = 0x8895,
    kStreamDraw = 0x88E0, kStaticDraw = 0x88E4, kDynamicDraw = 0x88E8, kBufferSize = 0x8764, kBufferUsage = 0x8765,
    kFragmentShader = 0x8B30, kVertexShader = 0x8B31, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82, kValidateStatus = 0x8B83,
    kInfoLogLength = 0x8B84, kAttachedShaders = 0x8B85, kActiveUniforms = 0x8B86, kActiveAttributes = 0x8B89,
    kShaderType = 0x8B4F, kDeleteStatus = 0x8B80, kCurrentProgram = 0x8B8D, kFramebuffer = 0x8D40, kRenderbuffer = 0x8D41,
    kFramebufferComplete = 0x8CD5, kColorAttachment0 = 0x8CE0, kDepthAttachment = 0x8D00, kStencilAttachment = 0x8D20,
    kDepthStencilAttachment = 0x821A, kDepth24Stencil8 = 0x88F0, kDepthStencil = 0x84F9, kFramebufferBinding = 0x8CA6,
    kRenderbufferBinding = 0x8CA7, kTexture0 = 0x84C0, kActiveTexture = 0x84E0, kTextureBinding2D = 0x8069,
    kTextureCubeMap = 0x8513, kTextureBindingCubeMap = 0x8514, kMaxTextureImageUnits = 0x8872, kMaxVertexAttribs = 0x8869,
    kMaxVertexUniformVectors = 0x8DFB, kMaxVaryingVectors = 0x8DFC, kMaxFragmentUniformVectors = 0x8DFD,
    kMaxCombinedTextureImageUnits = 0x8B4D, kMaxVertexTextureImageUnits = 0x8B4C, kMaxCubeMapTextureSize = 0x851C,
    kMaxRenderbufferSize = 0x84E8, kShadingLanguageVersion = 0x8B8C, kAliasedLineWidthRange = 0x846E,
    kAliasedPointSizeRange = 0x846D, kBlendColor = 0x8005, kBlendEquationRgb = 0x8009, kBlendEquationAlpha = 0x883D,
    kBlendDstRgb = 0x80C8, kBlendSrcRgb = 0x80C9, kBlendDstAlpha = 0x80CA, kBlendSrcAlpha = 0x80CB, kStencilBackFunc = 0x8800,
    kStencilBackFail = 0x8801, kStencilBackPassDepthFail = 0x8802, kStencilBackPassDepthPass = 0x8803,
    kStencilBackRef = 0x8CA3, kStencilBackValueMask = 0x8CA4, kStencilBackWritemask = 0x8CA5, kSampleBuffers = 0x80A8,
    kSamples = 0x80A9, kGenerateMipmapHint = 0x8192, kFragmentShaderDerivativeHint = 0x8B8B, kVertexArrayBinding = 0x85B5,
    kUnpackFlipY = 0x9240, kUnpackPremultiplyAlpha = 0x9241, kContextLost = 0x9242, kUnpackColorspaceConversion = 0x9243,
    kBrowserDefault = 0x9244, kUnmaskedVendor = 0x9245, kUnmaskedRenderer = 0x9246, kImplementationColorReadType = 0x8B9A,
    kImplementationColorReadFormat = 0x8B9B, kVertexAttribArrayBufferBinding = 0x889F, kCurrentVertexAttrib = 0x8626,
    kVertexAttribArrayDivisor = 0x88FE, kInvalidFramebufferOperation = 0x0506, kHalfFloatOes = 0x8D61, kFloat = 0x1406,
    kBgra = 0x80E1, kRgba8 = 0x8058, kRgba32f = 0x8814, kRgba16f = 0x881A, kRgb32f = 0x8815, kRgb16f = 0x881B,
    kMaxTextureMaxAnisotropy = 0x84FF, kDepthComponent16 = 0x81A5, kStencilIndex8 = 0x8D48, kRgba4 = 0x8056, kRgb5A1 = 0x8057,
    kRgb565 = 0x8D62, kTextureMinFilter = 0x2801, kTextureWrapS = 0x2802
};

template <class F>
static void load(F& fn, const char* name, const char* alt = nullptr) {
    fn = (F)wglGetProcAddress(name);
    if (!fn && alt) fn = (F)wglGetProcAddress(alt);
}
static bool loadGL() {
    static bool done = false, ok = false;
    if (done) return ok;
    done = true;
#define L(name) load(name##_, #name)
    L(glActiveTexture); L(glAttachShader); L(glBindAttribLocation); L(glBindBuffer); L(glBlendColor); L(glBlendEquation);
    L(glBlendEquationSeparate); L(glBlendFuncSeparate); L(glBufferData); L(glBufferSubData); L(glCompileShader);
    L(glCreateProgram); L(glCreateShader); L(glDeleteBuffers); L(glDeleteProgram); L(glDeleteShader); L(glDetachShader);
    L(glDisableVertexAttribArray); L(glEnableVertexAttribArray); L(glGenBuffers); L(glGetActiveAttrib); L(glGetActiveUniform);
    L(glGetAttribLocation); L(glGetBufferParameteriv); L(glGetProgramInfoLog); L(glGetProgramiv); L(glGetShaderInfoLog);
    L(glGetShaderiv); L(glGetUniformfv); L(glGetUniformiv); L(glGetUniformLocation); L(glGetVertexAttribfv);
    L(glGetVertexAttribiv); L(glGetVertexAttribPointerv); L(glLinkProgram); L(glShaderSource); L(glStencilFuncSeparate);
    L(glStencilMaskSeparate); L(glStencilOpSeparate); L(glUniform1fv); L(glUniform2fv); L(glUniform3fv); L(glUniform4fv);
    L(glUniform1iv); L(glUniform2iv); L(glUniform3iv); L(glUniform4iv); L(glUniformMatrix2fv); L(glUniformMatrix3fv);
    L(glUniformMatrix4fv); L(glUseProgram); L(glValidateProgram); L(glVertexAttrib4fv); L(glVertexAttribPointer);
    L(glGetShaderPrecisionFormat);
#undef L
    load(glBindFramebuffer_, "glBindFramebuffer", "glBindFramebufferEXT");
    load(glBindRenderbuffer_, "glBindRenderbuffer", "glBindRenderbufferEXT");
    load(glCheckFramebufferStatus_, "glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
    load(glDeleteFramebuffers_, "glDeleteFramebuffers", "glDeleteFramebuffersEXT");
    load(glDeleteRenderbuffers_, "glDeleteRenderbuffers", "glDeleteRenderbuffersEXT");
    load(glFramebufferRenderbuffer_, "glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT");
    load(glFramebufferTexture2D_, "glFramebufferTexture2D", "glFramebufferTexture2DEXT");
    load(glGenFramebuffers_, "glGenFramebuffers", "glGenFramebuffersEXT");
    load(glGenRenderbuffers_, "glGenRenderbuffers", "glGenRenderbuffersEXT");
    load(glGenerateMipmap_, "glGenerateMipmap", "glGenerateMipmapEXT");
    load(glGetFramebufferAttachmentParameteriv_, "glGetFramebufferAttachmentParameteriv", "glGetFramebufferAttachmentParameterivEXT");
    load(glGetRenderbufferParameteriv_, "glGetRenderbufferParameteriv", "glGetRenderbufferParameterivEXT");
    load(glRenderbufferStorage_, "glRenderbufferStorage", "glRenderbufferStorageEXT");
    load(glGenVertexArrays_, "glGenVertexArrays", "glGenVertexArraysAPPLE");
    load(glBindVertexArray_, "glBindVertexArray", "glBindVertexArrayAPPLE");
    load(glDeleteVertexArrays_, "glDeleteVertexArrays", "glDeleteVertexArraysAPPLE");
    load(glDrawArraysInstanced_, "glDrawArraysInstanced", "glDrawArraysInstancedARB");
    load(glDrawElementsInstanced_, "glDrawElementsInstanced", "glDrawElementsInstancedARB");
    load(glVertexAttribDivisor_, "glVertexAttribDivisor", "glVertexAttribDivisorARB");
    ok = glCreateShader_ && glGenFramebuffers_ && glBindFramebuffer_ && glFramebufferTexture2D_ && glRenderbufferStorage_ && glUniformMatrix4fv_;
    return ok;
}

// ---------------------------------------------------------------- contexts
// Kinds of the script objects that stand for OpenGL objects (JSHost::hostKind); slotA holds the name, slotB the
// context object, flagA marks a deleted one
enum : uint8_t { HK_GLContext = 200, HK_GLBuffer, HK_GLTexture, HK_GLProgram, HK_GLShader, HK_GLFramebuffer, HK_GLRenderbuffer,
                 HK_GLUniform, HK_GLVertexArray, HK_GLActiveInfo };

struct GLContext {
    HWND wnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    Node* canvas = nullptr;
    GLuint fbo = 0, color = 0, depth = 0;
    int w = 0, h = 0;
    bool alpha = true, depthOn = true, stencil = false, premultiplied = true, preserve = false, antialias = false;
    bool dirty = false, lost = false;
    GLenum syntheticError = 0;  // an error WebGL itself raised, reported before the driver's
    Value framebuffer = Value::null(), arrayBuffer = Value::null(), elementBuffer = Value::null(), program = Value::null(),
          renderbuffer = Value::null(), vertexArray = Value::null();
    Value defaultElements = Value::null();  // the element array buffer of the default vertex array
    struct BufferInfo {
        Value object;
        GLenum target = 0;  // the first target it was bound to; WebGL lets it be bound to no other (WebGL 1.0, 5.1)
        size_t size = 0;
        std::vector<uint8_t> indices;  // an element array buffer's contents, for checking what draws read (6.6)
    };
    std::unordered_map<GLuint, BufferInfo> buffers;                 // by name
    std::unordered_map<GLuint, std::vector<GLint>> programAttribs;  // the attribute locations a linked program reads
    GLint maxAttribs = 16;
    GLuint scratchFbo = 0;  // for clearing a new renderbuffer
    std::vector<Value> textures2D, texturesCube;  // by texture unit
    int unit = 0;
    bool flipY = false, premultiply = false;
    GLint unpackAlign = 4, packAlign = 4;
    JSObject* self = nullptr;
    std::vector<std::string> extensions;  // the ones a script turned on
    std::unordered_map<std::string, JSObject*> extObjects;
};
struct VmContexts {
    CanvasHost* host = nullptr;
    JSObject *ctxProto = nullptr, *bufferProto = nullptr, *textureProto = nullptr, *programProto = nullptr, *shaderProto = nullptr,
             *framebufferProto = nullptr, *renderbufferProto = nullptr, *uniformProto = nullptr, *activeInfoProto = nullptr,
             *shaderPrecisionProto = nullptr, *vaoProto = nullptr;
    std::vector<std::unique_ptr<GLContext>> contexts;
};
static std::unordered_map<Vm*, VmContexts>& registry() {
    static std::unordered_map<Vm*, VmContexts> r;
    return r;
}
static HGLRC g_current = nullptr;
static void makeCurrent(GLContext& c) {
    if (g_current == c.rc) return;
    wglMakeCurrent(c.dc, c.rc);
    g_current = c.rc;
}

// Clears the bound framebuffer to transparent black, depth 1 and stencil 0 whatever the script's clear values,
// masks and scissor test are, leaving those as they were
static void clearDrawingBuffer() {
    GLfloat color[4], depth;
    GLint stencil, stencilMask, stencilBackMask;
    GLboolean colorMask[4], depthMask, scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &depth);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &stencil);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);
    glGetIntegerv(kStencilBackWritemask, &stencilBackMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glStencilMask(0xFFFFFFFF);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glClearColor(color[0], color[1], color[2], color[3]);
    glClearDepth(depth);
    glClearStencil(stencil);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glDepthMask(depthMask);
    glStencilMaskSeparate_(GL_FRONT, (GLuint)stencilMask);
    glStencilMaskSeparate_(GL_BACK, (GLuint)stencilBackMask);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

// The drawing buffer (WebGL 2.2): a framebuffer object of the canvas's size, made again when the size changes
static void sizeDrawingBuffer(GLContext& c, int w, int h) {
    w = std::clamp(w, 1, 8192);
    h = std::clamp(h, 1, 8192);
    if (w == c.w && h == c.h && c.fbo) return;
    makeCurrent(c);
    c.w = w;
    c.h = h;
    if (!c.fbo) glGenFramebuffers_(1, &c.fbo);
    if (!c.color) glGenTextures(1, &c.color);
    GLint prevTex = 0, prevFbo = 0;
    glGetIntegerv(kTextureBinding2D, &prevTex);
    glGetIntegerv(kFramebufferBinding, &prevFbo);
    glBindTexture(GL_TEXTURE_2D, c.color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, c.alpha ? kRgba8 : 0x8051 /* RGB8 */, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glBindFramebuffer_(kFramebuffer, c.fbo);
    glFramebufferTexture2D_(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, c.color, 0);
    if (c.depthOn || c.stencil) {
        if (!c.depth) glGenRenderbuffers_(1, &c.depth);
        glBindRenderbuffer_(kRenderbuffer, c.depth);
        glRenderbufferStorage_(kRenderbuffer, kDepth24Stencil8, w, h);
        glFramebufferRenderbuffer_(kFramebuffer, kDepthAttachment, kRenderbuffer, c.depth);
        if (c.stencil) glFramebufferRenderbuffer_(kFramebuffer, kStencilAttachment, kRenderbuffer, c.depth);
        glBindRenderbuffer_(kRenderbuffer, 0);
    }
    clearDrawingBuffer();  // a new drawing buffer starts transparent black (WebGL 2.2)
    if (prevFbo) glBindFramebuffer_(kFramebuffer, (GLuint)prevFbo);
    c.dirty = true;
}
// Before a call that touches the drawing buffer: the canvas may have been given another size
static void syncSize(GLContext& c) {
    int w = canvas_dimension(c.canvas, false), h = canvas_dimension(c.canvas, true);
    if (w != c.w || h != c.h) sizeDrawingBuffer(c, w, h);
}

static HWND glWindow() {
    static HWND wnd = nullptr;
    if (wnd) return wnd;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TinyBrowserGL";
    RegisterClassW(&wc);
    wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    return wnd;
}
static std::unique_ptr<GLContext> newContext() {
    auto c = std::make_unique<GLContext>();
    c->wnd = glWindow();
    if (!c->wnd) return nullptr;
    c->dc = GetDC(c->wnd);
    PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = GetPixelFormat(c->dc);
    if (!pf) {
        pf = ChoosePixelFormat(c->dc, &pfd);
        if (!pf || !SetPixelFormat(c->dc, pf, &pfd)) return nullptr;
    }
    c->rc = wglCreateContext(c->dc);
    if (!c->rc) return nullptr;
    makeCurrent(*c);
    if (!loadGL()) {
        wglMakeCurrent(nullptr, nullptr);
        g_current = nullptr;
        wglDeleteContext(c->rc);
        return nullptr;
    }
    return c;
}

// ---------------------------------------------------------------- shaders: GLSL ES 1.00 as desktop GLSL 1.20
// The precision qualifiers become nothing (desktop floats and ints are all of high precision), and the macros a
// GLSL ES compiler defines are defined: GL_ES, GL_FRAGMENT_PRECISION_HIGH, and one for each extension offered.
static std::string translateShader(const std::string& src) {
    std::string out = "#version 120\n#define lowp\n#define mediump\n#define highp\n";
    for (const char* macro : {"GL_ES", "GL_FRAGMENT_PRECISION_HIGH", "GL_OES_standard_derivatives"})  // some drivers have them
        out += std::string("#ifndef ") + macro + "\n#define " + macro + " 1\n#endif\n";
    static const std::regex precision(R"(\bprecision\s+(lowp|mediump|highp)\s+\w+\s*;)");
    static const std::regex version(R"(^\s*#\s*version[^\n]*)");
    static const std::regex extension(R"(^\s*#\s*extension\s+GL_OES_standard_derivatives[^\n]*)");
    std::string body = std::regex_replace(src, precision, "");
    std::string result;
    size_t i = 0;
    while (i <= body.size()) {
        size_t nl = body.find('\n', i);
        if (nl == std::string::npos) nl = body.size();
        std::string line = body.substr(i, nl - i);
        if (std::regex_search(line, version) || std::regex_search(line, extension)) line.clear();
        result += line + "\n";
        i = nl + 1;
    }
    return out + "#line 1\n" + result;
}

// ---------------------------------------------------------------- bindings
static VmContexts& vmc(Vm& vm) { return registry()[&vm]; }
static GLContext& ctxOf(CallInfo& ci) {
    if (ci.thisv.isObject() && ci.thisv.asObject()->cls == Cls::Host) {
        JSHost* h = (JSHost*)ci.thisv.asObject();
        if (h->hostKind == HK_GLContext && h->ptr) {
            GLContext& c = *(GLContext*)h->ptr;
            makeCurrent(c);
            return c;
        }
    }
    ci.vm.throwError(Err::TypeError, "Illegal invocation: not a WebGLRenderingContext");
}
static JSHost* objOf(Value v, uint8_t kind) {
    if (!v.isObject() || v.asObject()->cls != Cls::Host) return nullptr;
    JSHost* h = (JSHost*)v.asObject();
    return h->hostKind == kind ? h : nullptr;
}
static GLuint nameOf(Value v, uint8_t kind) {
    JSHost* h = objOf(v, kind);
    return h && !h->flagA ? (GLuint)h->slotA.asInt() : 0;
}
static Value newGLObject(Vm& vm, GLContext& c, uint8_t kind, GLuint name, JSObject* proto) {
    JSHost* h = vm.alloc<JSHost>();
    h->proto = proto;
    h->hostKind = kind;
    h->slotA = Value::i32((int32_t)name);
    h->slotB = Value::cell(c.self);
    return Value::cell(h);
}
static void synth(GLContext& c, GLenum e) {
    if (!c.syntheticError) c.syntheticError = e;
}
static double num(CallInfo& ci, int i) { return ci.vm.toNumber(ci.arg(i)); }
static GLint inti(CallInfo& ci, int i) { return ci.vm.toInt32(ci.arg(i)); }
static GLenum en(CallInfo& ci, int i) { return (GLenum)ci.vm.toUint32(ci.arg(i)); }
static GLfloat fl(CallInfo& ci, int i) { return (GLfloat)ci.vm.toNumber(ci.arg(i)); }
// The bytes of an ArrayBufferView or ArrayBuffer
static bool bytesOf(Value v, uint8_t*& p, size_t& n, TA* type = nullptr) {
    if (!v.isObject()) return false;
    JSObject* o = v.asObject();
    if (o->cls == Cls::TypedArray || o->cls == Cls::DataView) {
        JSTypedArray* t = (JSTypedArray*)o;
        if (typedOutOfBounds(t)) return false;
        size_t elem = o->cls == Cls::DataView ? 1 : kTaSize[(int)t->type];
        size_t len = t->tracking ? (t->buffer->size() - t->offset) / elem : t->length;
        p = t->buffer->data() + t->offset;
        n = len * elem;
        if (type) *type = o->cls == Cls::DataView ? TA::U8 : t->type;
        return true;
    }
    if (o->cls == Cls::ArrayBuffer) {
        JSArrayBuffer* b = (JSArrayBuffer*)o;
        p = b->data();
        n = b->size();
        if (type) *type = TA::U8;
        return true;
    }
    return false;
}
// A Float32Array or a sequence of numbers as floats (or ints)
template <class T>
static bool numbersOf(CallInfo& ci, Value v, std::vector<T>& out) {
    uint8_t* p;
    size_t n;
    TA type;
    if (bytesOf(v, p, n, &type)) {
        if (std::is_same_v<T, float> ? type != TA::F32 : (type != TA::I32 && type != TA::U32)) return false;
        out.assign((T*)p, (T*)p + n / sizeof(T));
        return true;
    }
    if (!v.isObject() || v.asObject()->cls != Cls::Array) return false;
    JSArray* a = (JSArray*)v.asObject();
    out.clear();
    for (size_t i = 0; i < a->elems.size(); ++i) out.push_back((T)ci.vm.toNumber(a->elems[i]));
    return true;
}
static Value str(Vm& vm, const std::string& s) { return vm.strv(std::string_view(s)); }
static Value typed(Vm& vm, TA type, const void* data, uint32_t count) {
    JSTypedArray* t = newTypedArray(vm, type, count);
    memcpy(t->buffer->data(), data, count * kTaSize[(int)type]);
    return Value::cell(t);
}

#define GLCALL(name) static Value name(CallInfo& ci)
#define CTX GLContext& c = ctxOf(ci); (void)c

// -- state
GLCALL(glActiveTextureJS) { CTX; GLenum t = en(ci, 0); if (t < kTexture0 || t >= kTexture0 + 32) { synth(c, GL_INVALID_ENUM); return Value::undef(); } c.unit = (int)(t - kTexture0); glActiveTexture_(t); return Value::undef(); }
GLCALL(blendColor) { CTX; glBlendColor_(fl(ci, 0), fl(ci, 1), fl(ci, 2), fl(ci, 3)); return Value::undef(); }
GLCALL(blendEquation) { CTX; glBlendEquation_(en(ci, 0)); return Value::undef(); }
GLCALL(blendEquationSeparate) { CTX; glBlendEquationSeparate_(en(ci, 0), en(ci, 1)); return Value::undef(); }
GLCALL(blendFunc) { CTX; glBlendFunc(en(ci, 0), en(ci, 1)); return Value::undef(); }
GLCALL(blendFuncSeparate) { CTX; glBlendFuncSeparate_(en(ci, 0), en(ci, 1), en(ci, 2), en(ci, 3)); return Value::undef(); }
GLCALL(clearColor) { CTX; glClearColor(fl(ci, 0), fl(ci, 1), fl(ci, 2), fl(ci, 3)); return Value::undef(); }
GLCALL(clearDepth) { CTX; glClearDepth(num(ci, 0)); return Value::undef(); }
GLCALL(clearStencil) { CTX; glClearStencil(inti(ci, 0)); return Value::undef(); }
GLCALL(colorMask) { CTX; glColorMask(ci.vm.toBool(ci.arg(0)), ci.vm.toBool(ci.arg(1)), ci.vm.toBool(ci.arg(2)), ci.vm.toBool(ci.arg(3))); return Value::undef(); }
GLCALL(cullFace) { CTX; glCullFace(en(ci, 0)); return Value::undef(); }
GLCALL(depthFunc) { CTX; glDepthFunc(en(ci, 0)); return Value::undef(); }
GLCALL(depthMask) { CTX; glDepthMask(ci.vm.toBool(ci.arg(0))); return Value::undef(); }
GLCALL(depthRange) { CTX; glDepthRange(num(ci, 0), num(ci, 1)); return Value::undef(); }
GLCALL(disable) { CTX; glDisable(en(ci, 0)); return Value::undef(); }
GLCALL(enable) { CTX; glEnable(en(ci, 0)); return Value::undef(); }
GLCALL(frontFace) { CTX; glFrontFace(en(ci, 0)); return Value::undef(); }
GLCALL(hint) { CTX; glHint(en(ci, 0), en(ci, 1)); return Value::undef(); }
GLCALL(isEnabled) { CTX; return Value::boolean(glIsEnabled(en(ci, 0)) != 0); }
GLCALL(lineWidth) { CTX; glLineWidth(fl(ci, 0)); return Value::undef(); }
GLCALL(polygonOffset) { CTX; glPolygonOffset(fl(ci, 0), fl(ci, 1)); return Value::undef(); }
GLCALL(sampleCoverage) { CTX; return Value::undef(); }
GLCALL(scissor) { CTX; glScissor(inti(ci, 0), inti(ci, 1), inti(ci, 2), inti(ci, 3)); return Value::undef(); }
GLCALL(stencilFunc) { CTX; glStencilFunc(en(ci, 0), inti(ci, 1), en(ci, 2)); return Value::undef(); }
GLCALL(stencilFuncSeparate) { CTX; glStencilFuncSeparate_(en(ci, 0), en(ci, 1), inti(ci, 2), en(ci, 3)); return Value::undef(); }
GLCALL(stencilMask) { CTX; glStencilMask(en(ci, 0)); return Value::undef(); }
GLCALL(stencilMaskSeparate) { CTX; glStencilMaskSeparate_(en(ci, 0), en(ci, 1)); return Value::undef(); }
GLCALL(stencilOp) { CTX; glStencilOp(en(ci, 0), en(ci, 1), en(ci, 2)); return Value::undef(); }
GLCALL(stencilOpSeparate) { CTX; glStencilOpSeparate_(en(ci, 0), en(ci, 1), en(ci, 2), en(ci, 3)); return Value::undef(); }
GLCALL(viewport) { CTX; glViewport(inti(ci, 0), inti(ci, 1), inti(ci, 2), inti(ci, 3)); return Value::undef(); }
GLCALL(finish) { CTX; glFinish(); return Value::undef(); }
GLCALL(flush) { CTX; glFlush(); return Value::undef(); }
GLCALL(isContextLost) { CTX; return Value::boolean(c.lost); }
GLCALL(getError) {
    CTX;
    if (GLenum e = c.syntheticError) {
        c.syntheticError = 0;
        return Value::i32((int32_t)e);
    }
    return Value::i32((int32_t)glGetError());
}
GLCALL(pixelStorei) {
    CTX;
    GLenum p = en(ci, 0);
    GLint v = inti(ci, 1);
    if (p == kUnpackFlipY) c.flipY = v != 0;
    else if (p == kUnpackPremultiplyAlpha) c.premultiply = v != 0;
    else if (p == kUnpackColorspaceConversion) {
        if (v != 0 && (GLenum)v != kBrowserDefault) synth(c, GL_INVALID_VALUE);
    } else if (p == GL_UNPACK_ALIGNMENT || p == GL_PACK_ALIGNMENT) {
        if (v != 1 && v != 2 && v != 4 && v != 8) {
            synth(c, GL_INVALID_VALUE);
            return Value::undef();
        }
        (p == GL_UNPACK_ALIGNMENT ? c.unpackAlign : c.packAlign) = v;
        glPixelStorei(p, v);
    } else
        synth(c, GL_INVALID_ENUM);
    return Value::undef();
}

// -- buffers
GLCALL(createBuffer) {
    CTX;
    GLuint b = 0;
    glGenBuffers_(1, &b);
    Value o = newGLObject(ci.vm, c, HK_GLBuffer, b, vmc(ci.vm).bufferProto);
    c.buffers[b].object = o;
    return o;
}
GLCALL(deleteBuffer) {
    CTX;
    if (JSHost* h = objOf(ci.arg(0), HK_GLBuffer); h && !h->flagA) {
        GLuint b = (GLuint)h->slotA.asInt();
        glDeleteBuffers_(1, &b);
        h->flagA = true;
        c.buffers.erase(b);
        if (c.arrayBuffer == ci.arg(0)) c.arrayBuffer = Value::null();
        if (c.elementBuffer == ci.arg(0)) c.elementBuffer = Value::null();
        if (c.defaultElements == ci.arg(0)) c.defaultElements = Value::null();
    }
    return Value::undef();
}
// The object an argument must be (WebGL's IDL): another kind of value is a TypeError; null may stand for none
static JSHost* argObject(CallInfo& ci, int i, uint8_t kind, const char* name) {
    Value v = ci.arg(i);
    if (v.isNullish()) return nullptr;
    JSHost* h = objOf(v, kind);
    if (!h) ci.vm.throwError(Err::TypeError, "parameter %d is not of type '%s'", i + 1, name);
    return h;
}
static GLContext::BufferInfo* boundBuffer(GLContext& c, GLenum target) {
    Value b = target == kArrayBuffer ? c.arrayBuffer : target == kElementArrayBuffer ? c.elementBuffer : Value::null();
    auto it = c.buffers.find(nameOf(b, HK_GLBuffer));
    return b.isNull() || it == c.buffers.end() ? nullptr : &it->second;
}
// Errors the driver has noted so far are kept for getError(), so that the next call's own can be told apart
static void keepDriverErrors(GLContext& c) {
    if (GLenum e = glGetError()) synth(c, e);
}
GLCALL(bindBuffer) {
    CTX;
    GLenum target = en(ci, 0);
    JSHost* h = argObject(ci, 1, HK_GLBuffer, "WebGLBuffer");
    if (target != kArrayBuffer && target != kElementArrayBuffer) { synth(c, GL_INVALID_ENUM); return Value::undef(); }
    if (h && h->flagA) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
    Value b = h ? Value::cell(h) : Value::null();
    if (h) {
        auto& info = c.buffers[(GLuint)h->slotA.asInt()];
        if (info.target && info.target != target) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
        info.target = target;
    }
    glBindBuffer_(target, nameOf(b, HK_GLBuffer));
    if (target == kArrayBuffer) c.arrayBuffer = b;
    else {  // part of the bound vertex array's state
        c.elementBuffer = b;
        if (JSHost* vao = objOf(c.vertexArray, HK_GLVertexArray)) vao->slotB = b;
        else c.defaultElements = b;
    }
    return Value::undef();
}
GLCALL(bufferData) {
    CTX;
    GLenum target = en(ci, 0), usage = en(ci, 2);
    GLContext::BufferInfo* info = boundBuffer(c, target);
    if (target != kArrayBuffer && target != kElementArrayBuffer) { synth(c, GL_INVALID_ENUM); return Value::undef(); }
    if (usage != kStreamDraw && usage != kStaticDraw && usage != kDynamicDraw) { synth(c, GL_INVALID_ENUM); return Value::undef(); }
    if (!info) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
    uint8_t* p = nullptr;
    size_t n = 0;
    std::vector<uint8_t> zeros;
    Value d = ci.arg(1);
    if (d.isNumber()) {
        double size = num(ci, 1);
        if (size < 0) { synth(c, GL_INVALID_VALUE); return Value::undef(); }
        if (size > 1 << 30) { synth(c, GL_OUT_OF_MEMORY); return Value::undef(); }
        zeros.assign((size_t)size, 0);  // a new buffer holds zeros, not what the memory held before
        p = zeros.data();
        n = zeros.size();
    } else if (d.isNull()) {
        synth(c, GL_INVALID_VALUE);
        return Value::undef();
    } else if (!bytesOf(d, p, n))
        ci.vm.throwError(Err::TypeError, "parameter 2 is not of type 'ArrayBuffer' or 'ArrayBufferView'");
    keepDriverErrors(c);
    glBufferData_(target, (GLsizeiptr)n, p, usage);
    if (GLenum e = glGetError()) {
        synth(c, e);
        return Value::undef();
    }
    info->size = n;
    if (target == kElementArrayBuffer) info->indices.assign(p, p + n);
    return Value::undef();
}
GLCALL(bufferSubData) {
    CTX;
    GLenum target = en(ci, 0);
    double offset = num(ci, 1);
    GLContext::BufferInfo* info = boundBuffer(c, target);
    uint8_t* p;
    size_t n;
    if (!bytesOf(ci.arg(2), p, n)) ci.vm.throwError(Err::TypeError, "parameter 3 is not of type 'ArrayBuffer' or 'ArrayBufferView'");
    if (target != kArrayBuffer && target != kElementArrayBuffer) { synth(c, GL_INVALID_ENUM); return Value::undef(); }
    if (!info) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
    if (offset < 0 || offset + (double)n > (double)info->size) { synth(c, GL_INVALID_VALUE); return Value::undef(); }
    glBufferSubData_(target, (GLintptr)offset, (GLsizeiptr)n, p);
    if (target == kElementArrayBuffer) memcpy(info->indices.data() + (size_t)offset, p, n);
    return Value::undef();
}
GLCALL(getBufferParameter) { CTX; GLint v = 0; glGetBufferParameteriv_(en(ci, 0), en(ci, 1), &v); return Value::i32(v); }
GLCALL(isBuffer) { CTX; return Value::boolean(objOf(ci.arg(0), HK_GLBuffer) && !objOf(ci.arg(0), HK_GLBuffer)->flagA); }

// -- textures
GLCALL(createTexture) { CTX; GLuint t = 0; glGenTextures(1, &t); return newGLObject(ci.vm, c, HK_GLTexture, t, vmc(ci.vm).textureProto); }
GLCALL(deleteTexture) {
    CTX;
    if (JSHost* h = objOf(ci.arg(0), HK_GLTexture); h && !h->flagA) {
        GLuint t = (GLuint)h->slotA.asInt();
        glDeleteTextures(1, &t);
        h->flagA = true;
        for (auto& v : c.textures2D) if (v == ci.arg(0)) v = Value::null();
        for (auto& v : c.texturesCube) if (v == ci.arg(0)) v = Value::null();
    }
    return Value::undef();
}
GLCALL(bindTexture) {
    CTX;
    GLenum target = en(ci, 0);
    Value t = ci.arg(1);
    if (!t.isNull() && !objOf(t, HK_GLTexture)) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
    glBindTexture(target, nameOf(t, HK_GLTexture));
    auto& slots = target == kTextureCubeMap ? c.texturesCube : c.textures2D;
    if ((int)slots.size() <= c.unit) slots.resize(c.unit + 1, Value::null());
    slots[c.unit] = t.isNull() ? Value::null() : t;
    return Value::undef();
}
GLCALL(isTexture) { CTX; JSHost* h = objOf(ci.arg(0), HK_GLTexture); return Value::boolean(h && !h->flagA); }
GLCALL(texParameterf) { CTX; glTexParameterf(en(ci, 0), en(ci, 1), fl(ci, 2)); return Value::undef(); }
GLCALL(texParameteri) { CTX; glTexParameteri(en(ci, 0), en(ci, 1), inti(ci, 2)); return Value::undef(); }
GLCALL(getTexParameter) { CTX; GLint v = 0; glGetTexParameteriv(en(ci, 0), en(ci, 1), &v); return Value::i32(v); }
GLCALL(generateMipmap) { CTX; glGenerateMipmap_(en(ci, 0)); return Value::undef(); }
// The pixels of a TexImageSource (WebGL 1.0, 5.14.8): ImageData, an <img> or a <canvas>, as RGBA bytes, flipped and
// premultiplied as pixelStorei asked. False when the source has no picture yet; anything else is a TypeError.
static bool sourcePixels(CallInfo& ci, GLContext& c, Value src, int& w, int& h, std::vector<uint8_t>& rgba) {
    VmContexts& v = vmc(ci.vm);
    Node* n = v.host ? v.host->nodeOf(src) : nullptr;
    if (n && (n->name == "img" || n->name == "canvas" || n->name == "video")) {
        int got = v.host->imagePixels(n, w, h, rgba);
        // a picture from another origin would let readPixels() read it (WebGL 1.0, 4.2)
        if (got == 2) v.host->securityError("The image element contains cross-origin data, and may not be loaded.");
        if (got != 1) return false;
    } else if (src.isObject() && ci.vm.instanceOf(src, ci.vm.get(Value::cell(ci.vm.global), "ImageData"))) {
        uint8_t* p;
        size_t len;
        w = ci.vm.toInt32(ci.vm.get(src, "width"));
        h = ci.vm.toInt32(ci.vm.get(src, "height"));
        if (!bytesOf(ci.vm.get(src, "data"), p, len) || w <= 0 || h <= 0 || len < (size_t)w * h * 4) return false;
        rgba.assign(p, p + (size_t)w * h * 4);
    } else
        ci.vm.throwError(Err::TypeError, "The provided value is not of type '(HTMLCanvasElement or HTMLImageElement or HTMLVideoElement or ImageData)'");
    if (c.premultiply)
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            for (int k = 0; k < 3; ++k) rgba[i + k] = (uint8_t)((rgba[i + k] * rgba[i + 3] + 127) / 255);
    if (c.flipY)
        for (int y = 0; y < h / 2; ++y) std::swap_ranges(rgba.begin() + (size_t)y * w * 4, rgba.begin() + (size_t)(y + 1) * w * 4, rgba.begin() + (size_t)(h - 1 - y) * w * 4);
    return true;
}
// The bytes a pixel of this format and type takes, 0 for a combination WebGL 1 does not have (5.14.8)
static int pixelBytes(GLContext& c, GLenum format, GLenum type) {
    int comps = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : format == GL_LUMINANCE_ALPHA ? 2 : format == GL_LUMINANCE || format == GL_ALPHA ? 1 : 0;
    auto on = [&](const char* ext) { return std::find(c.extensions.begin(), c.extensions.end(), ext) != c.extensions.end(); };
    switch (type) {
        case GL_UNSIGNED_BYTE: return comps;
        case 0x8363: return format == GL_RGB ? 2 : 0;               // UNSIGNED_SHORT_5_6_5
        case 0x8033: case 0x8034: return format == GL_RGBA ? 2 : 0;  // UNSIGNED_SHORT_4_4_4_4, UNSIGNED_SHORT_5_5_5_1
        case kFloat: return on("OES_texture_float") ? comps * 4 : 0;
        case kHalfFloatOes: return on("OES_texture_half_float") ? comps * 2 : 0;
        default: return 0;
    }
}
// The pixels of an ArrayBufferView for texImage2D and texSubImage2D: of the array type the pixel type wants, and as
// many as width and height take with UNPACK_ALIGNMENT's rows; turned over for UNPACK_FLIP_Y_WEBGL. False after
// noting the error. Null pixels are zeros.
static bool viewPixels(CallInfo& ci, GLContext& c, Value view, GLsizei w, GLsizei h, GLenum format, GLenum type, std::vector<uint8_t>& out,
                       const uint8_t*& data) {
    if (w < 0 || h < 0) { synth(c, GL_INVALID_VALUE); return false; }
    int bpp = pixelBytes(c, format, type);
    if (!bpp) { synth(c, GL_INVALID_ENUM); return false; }
    size_t rowBytes = (size_t)w * bpp, row = (rowBytes + c.unpackAlign - 1) / c.unpackAlign * c.unpackAlign;
    size_t need = w && h ? row * (h - 1) + rowBytes : 0;
    if (view.isNullish()) {
        out.assign(row * h, 0);  // a texture with no pixels given holds zeros, not what the memory held before
        data = out.data();
        return true;
    }
    uint8_t* p;
    size_t n;
    TA ta;
    if (!bytesOf(view, p, n, &ta) || !view.isObject() || view.asObject()->cls != Cls::TypedArray)
        ci.vm.throwError(Err::TypeError, "parameter 9 is not of type 'ArrayBufferView'");
    TA want = type == GL_UNSIGNED_BYTE ? TA::U8 : type == kFloat ? TA::F32 : TA::U16;
    if (ta != want && !(want == TA::U8 && ta == TA::U8C)) { synth(c, GL_INVALID_OPERATION); return false; }
    if (n < need) { synth(c, GL_INVALID_OPERATION); return false; }
    data = p;
    if (c.flipY && h > 1) {
        out.assign(row * h, 0);
        for (GLsizei y = 0; y < h; ++y) memcpy(out.data() + (size_t)y * row, p + (size_t)(h - 1 - y) * row, rowBytes);
        data = out.data();
    }
    return true;
}
static GLint internalFor(GLint internal, GLenum type) {  // float textures need a float internal format on the desktop
    if (type == kFloat) return internal == GL_RGB ? kRgb32f : internal == GL_RGBA ? kRgba32f : internal;
    if (type == kHalfFloatOes) return internal == GL_RGB ? kRgb16f : internal == GL_RGBA ? kRgba16f : internal;
    return internal;
}
static GLenum typeFor(GLenum type) { return type == kHalfFloatOes ? 0x140B /* HALF_FLOAT */ : type; }
GLCALL(texImage2D) {
    CTX;
    GLenum target = en(ci, 0);
    GLint level = inti(ci, 1), internal = inti(ci, 2);
    if (ci.argc >= 9) {  // (target, level, internalformat, width, height, border, format, type, pixels)
        GLsizei w = inti(ci, 3), h = inti(ci, 4);
        GLenum format = en(ci, 6), type = en(ci, 7);
        std::vector<uint8_t> buf;
        const uint8_t* data = nullptr;
        if (!viewPixels(ci, c, ci.arg(8), w, h, format, type, buf, data)) return Value::undef();
        if ((GLenum)internal != format) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
        glTexImage2D(target, level, internalFor(internal, type), w, h, inti(ci, 5), format, typeFor(type), data);
        return Value::undef();
    }
    if (ci.argc < 6) ci.vm.throwError(Err::TypeError, "6 or 9 arguments required, but only %d present.", ci.argc);
    // (target, level, internalformat, format, type, source)
    GLenum format = en(ci, 3), type = en(ci, 4);
    if (!pixelBytes(c, format, type)) { synth(c, GL_INVALID_ENUM); return Value::undef(); }
    if ((GLenum)internal != format) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
    int w, h;
    std::vector<uint8_t> rgba;
    if (!sourcePixels(ci, c, ci.arg(5), w, h, rgba)) return Value::undef();  // nothing to show yet
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(target, level, internalFor(internal, type), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, c.unpackAlign);
    return Value::undef();
}
GLCALL(texSubImage2D) {
    CTX;
    GLenum target = en(ci, 0);
    GLint level = inti(ci, 1), x = inti(ci, 2), y = inti(ci, 3);
    if (ci.argc >= 9) {  // (target, level, xoffset, yoffset, width, height, format, type, pixels)
        GLsizei w = inti(ci, 4), h = inti(ci, 5);
        GLenum format = en(ci, 6), type = en(ci, 7);
        if (ci.arg(8).isNullish()) ci.vm.throwError(Err::TypeError, "parameter 9 is not of type 'ArrayBufferView'");
        std::vector<uint8_t> buf;
        const uint8_t* data = nullptr;
        if (!viewPixels(ci, c, ci.arg(8), w, h, format, type, buf, data)) return Value::undef();
        glTexSubImage2D(target, level, x, y, w, h, format, typeFor(type), data);
        return Value::undef();
    }
    if (ci.argc < 7) ci.vm.throwError(Err::TypeError, "7 or 9 arguments required, but only %d present.", ci.argc);
    GLenum format = en(ci, 4), type = en(ci, 5);
    if (!pixelBytes(c, format, type)) { synth(c, GL_INVALID_ENUM); return Value::undef(); }
    int w, h;
    std::vector<uint8_t> rgba;
    if (!sourcePixels(ci, c, ci.arg(6), w, h, rgba)) return Value::undef();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, c.unpackAlign);
    return Value::undef();
}
GLCALL(copyTexImage2D) { CTX; syncSize(c); glCopyTexImage2D(en(ci, 0), inti(ci, 1), en(ci, 2), inti(ci, 3), inti(ci, 4), inti(ci, 5), inti(ci, 6), inti(ci, 7)); return Value::undef(); }
GLCALL(copyTexSubImage2D) { CTX; syncSize(c); glCopyTexSubImage2D(en(ci, 0), inti(ci, 1), inti(ci, 2), inti(ci, 3), inti(ci, 4), inti(ci, 5), inti(ci, 6), inti(ci, 7)); return Value::undef(); }
GLCALL(compressedTexImage2D) { CTX; synth(c, GL_INVALID_ENUM); return Value::undef(); }  // no compressed formats are offered

// -- shaders and programs
GLCALL(createShader) {
    CTX;
    GLenum type = en(ci, 0);
    if (type != kVertexShader && type != kFragmentShader) { synth(c, GL_INVALID_ENUM); return Value::null(); }
    JSHost* h = (JSHost*)newGLObject(ci.vm, c, HK_GLShader, glCreateShader_(type), vmc(ci.vm).shaderProto).asObject();
    h->slotB = str(ci.vm, "");  // a shader's slotB is its source as the script gave it
    return Value::cell(h);
}
GLCALL(deleteShader) {
    CTX;
    if (JSHost* h = objOf(ci.arg(0), HK_GLShader); h && !h->flagA) {
        glDeleteShader_((GLuint)h->slotA.asInt());
        h->flagA = true;
    }
    return Value::undef();
}
GLCALL(isShader) { CTX; JSHost* h = objOf(ci.arg(0), HK_GLShader); return Value::boolean(h && !h->flagA); }
GLCALL(shaderSource) {
    CTX;
    JSHost* h = objOf(ci.arg(0), HK_GLShader);
    if (!h || h->flagA) { synth(c, GL_INVALID_VALUE); return Value::undef(); }
    std::string src = utf16to8(ci.vm.toStr16(ci.arg(1)));
    h->slotB = str(ci.vm, src);
    std::string glsl = translateShader(src);
    const char* text = glsl.c_str();
    glShaderSource_((GLuint)h->slotA.asInt(), 1, &text, nullptr);
    return Value::undef();
}
GLCALL(getShaderSource) {
    CTX;
    JSHost* h = objOf(ci.arg(0), HK_GLShader);
    return h ? h->slotB : Value::null();
}
GLCALL(compileShader) { CTX; glCompileShader_(nameOf(ci.arg(0), HK_GLShader)); return Value::undef(); }
GLCALL(getShaderParameter) {
    CTX;
    GLenum p = en(ci, 1);
    GLint v = 0;
    glGetShaderiv_(nameOf(ci.arg(0), HK_GLShader), p, &v);
    if (p == kShaderType) return Value::i32(v);
    return Value::boolean(v != 0);
}
GLCALL(getShaderInfoLog) {
    CTX;
    GLuint s = nameOf(ci.arg(0), HK_GLShader);
    GLint len = 0;
    glGetShaderiv_(s, kInfoLogLength, &len);
    std::string log(std::max(1, len), '\0');
    glGetShaderInfoLog_(s, (GLsizei)log.size(), nullptr, log.data());
    return str(ci.vm, log.c_str());
}
GLCALL(createProgram) { CTX; return newGLObject(ci.vm, c, HK_GLProgram, glCreateProgram_(), vmc(ci.vm).programProto); }
GLCALL(deleteProgram) {
    CTX;
    if (JSHost* h = objOf(ci.arg(0), HK_GLProgram); h && !h->flagA) {
        glDeleteProgram_((GLuint)h->slotA.asInt());
        h->flagA = true;
    }
    return Value::undef();
}
GLCALL(isProgram) { CTX; JSHost* h = objOf(ci.arg(0), HK_GLProgram); return Value::boolean(h && !h->flagA); }
GLCALL(attachShader) { CTX; glAttachShader_(nameOf(ci.arg(0), HK_GLProgram), nameOf(ci.arg(1), HK_GLShader)); return Value::undef(); }
GLCALL(detachShader) { CTX; glDetachShader_(nameOf(ci.arg(0), HK_GLProgram), nameOf(ci.arg(1), HK_GLShader)); return Value::undef(); }
GLCALL(bindAttribLocation) {
    CTX;
    std::string name = utf16to8(ci.vm.toStr16(ci.arg(2)));
    glBindAttribLocation_(nameOf(ci.arg(0), HK_GLProgram), en(ci, 1), name.c_str());
    return Value::undef();
}
GLCALL(linkProgram) {
    CTX;
    GLuint pr = nameOf(ci.arg(0), HK_GLProgram);
    if (!pr) return Value::undef();
    glLinkProgram_(pr);
    c.programAttribs.erase(pr);
    GLint linked = 0, count = 0;
    glGetProgramiv_(pr, kLinkStatus, &linked);
    if (!linked) return Value::undef();
    std::vector<GLint>& locs = c.programAttribs[pr];
    glGetProgramiv_(pr, kActiveAttributes, &count);
    for (GLint i = 0; i < count; ++i) {
        char name[256] = {};
        GLsizei len = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveAttrib_(pr, (GLuint)i, sizeof name, &len, &size, &type, name);
        GLint loc = glGetAttribLocation_(pr, name);
        int slots = type == 0x8B5A ? 2 : type == 0x8B5B ? 3 : type == 0x8B5C ? 4 : 1;  // a matrix takes a location per column
        for (int k = 0; loc >= 0 && k < slots; ++k) locs.push_back(loc + k);
    }
    return Value::undef();
}
GLCALL(validateProgram) { CTX; glValidateProgram_(nameOf(ci.arg(0), HK_GLProgram)); return Value::undef(); }
GLCALL(useProgram) {
    CTX;
    glUseProgram_(nameOf(ci.arg(0), HK_GLProgram));
    c.program = objOf(ci.arg(0), HK_GLProgram) ? ci.arg(0) : Value::null();
    return Value::undef();
}
GLCALL(getProgramParameter) {
    CTX;
    GLenum p = en(ci, 1);
    GLint v = 0;
    glGetProgramiv_(nameOf(ci.arg(0), HK_GLProgram), p, &v);
    if (p == kDeleteStatus || p == kLinkStatus || p == kValidateStatus) return Value::boolean(v != 0);
    return Value::i32(v);
}
GLCALL(getProgramInfoLog) {
    CTX;
    GLuint pr = nameOf(ci.arg(0), HK_GLProgram);
    GLint len = 0;
    glGetProgramiv_(pr, kInfoLogLength, &len);
    std::string log(std::max(1, len), '\0');
    glGetProgramInfoLog_(pr, (GLsizei)log.size(), nullptr, log.data());
    return str(ci.vm, log.c_str());
}
static Value activeInfo(CallInfo& ci, bool uniform) {
    GLuint pr = nameOf(ci.arg(0), HK_GLProgram);
    char name[256] = {};
    GLsizei len = 0;
    GLint size = 0;
    GLenum type = 0;
    (uniform ? glGetActiveUniform_ : glGetActiveAttrib_)(pr, en(ci, 1), sizeof name, &len, &size, &type, name);
    if (len <= 0) return Value::null();
    JSObject* o = ci.vm.newObject(vmc(ci.vm).activeInfoProto);
    ci.vm.def(o, "size", Value::i32(size), PF_E);
    ci.vm.def(o, "type", Value::i32((int32_t)type), PF_E);
    ci.vm.def(o, "name", str(ci.vm, name), PF_E);
    return Value::cell(o);
}
GLCALL(getActiveAttrib) { CTX; return activeInfo(ci, false); }
GLCALL(getActiveUniform) { CTX; return activeInfo(ci, true); }
GLCALL(getAttachedShaders) { CTX; return Value::cell(ci.vm.newArray()); }
GLCALL(getAttribLocation) {
    CTX;
    std::string name = utf16to8(ci.vm.toStr16(ci.arg(1)));
    return Value::i32(glGetAttribLocation_(nameOf(ci.arg(0), HK_GLProgram), name.c_str()));
}
GLCALL(getUniformLocation) {
    CTX;
    std::string name = utf16to8(ci.vm.toStr16(ci.arg(1)));
    GLuint pr = nameOf(ci.arg(0), HK_GLProgram);
    GLint loc = glGetUniformLocation_(pr, name.c_str());
    if (loc < 0) return Value::null();
    // its type, from the active uniform it names: "u", "u[0]" or an element "u[2]" of an array
    GLenum type = 0;
    GLint count = 0;
    glGetProgramiv_(pr, kActiveUniforms, &count);
    std::string base = name.substr(0, name.find('['));
    for (GLint i = 0; i < count && !type; ++i) {
        char un[256] = {};
        GLsizei len = 0;
        GLint size = 0;
        GLenum t = 0;
        glGetActiveUniform_(pr, (GLuint)i, sizeof un, &len, &size, &t, un);
        std::string u = un;
        if (u == name || u.substr(0, u.find('[')) == base) type = t;
    }
    JSHost* h = (JSHost*)newGLObject(ci.vm, c, HK_GLUniform, (GLuint)loc, vmc(ci.vm).uniformProto).asObject();
    h->slotB = ci.arg(0);                  // a location's slotB is the program it belongs to,
    h->ptr = (void*)(uintptr_t)type;       // and ptr its type
    return Value::cell(h);
}
// The location a uniform call is for, -1 for none; one of another program than the current one is an error (5.14.10)
static GLint locOf(CallInfo& ci, GLContext& c, int i = 0) {
    JSHost* h = argObject(ci, i, HK_GLUniform, "WebGLUniformLocation");
    if (!h) return -1;
    if (!(h->slotB == c.program)) {
        synth(c, GL_INVALID_OPERATION);
        return -1;
    }
    return h->slotA.asInt();
}
GLCALL(getUniform) {
    CTX;
    GLuint pr = nameOf(ci.arg(0), HK_GLProgram);
    JSHost* h = argObject(ci, 1, HK_GLUniform, "WebGLUniformLocation");
    if (!pr || !h || !(h->slotB == ci.arg(0))) { synth(c, GL_INVALID_OPERATION); return Value::null(); }
    GLint loc = h->slotA.asInt();
    GLenum type = (GLenum)(uintptr_t)h->ptr;
    // what each type reads back as (5.14.10): a number, a boolean, or an array of them
    int n = 1;
    bool isInt = false, isBool = false;
    switch (type) {
        case 0x8B50: n = 2; break;  // FLOAT_VEC2
        case 0x8B51: n = 3; break;
        case 0x8B52: n = 4; break;
        case 0x8B5A: n = 4; break;  // FLOAT_MAT2
        case 0x8B5B: n = 9; break;
        case 0x8B5C: n = 16; break;
        case 0x1404: case 0x8B5E: case 0x8B60: isInt = true; break;  // INT, SAMPLER_2D, SAMPLER_CUBE
        case 0x8B53: n = 2; isInt = true; break;                      // INT_VEC2
        case 0x8B54: n = 3; isInt = true; break;
        case 0x8B55: n = 4; isInt = true; break;
        case 0x8B56: isBool = true; break;  // BOOL
        case 0x8B57: n = 2; isBool = true; break;
        case 0x8B58: n = 3; isBool = true; break;
        case 0x8B59: n = 4; isBool = true; break;
        default: break;
    }
    GLfloat f[16] = {};
    GLint iv[16] = {};
    if (isInt || isBool) glGetUniformiv_(pr, loc, iv);
    else glGetUniformfv_(pr, loc, f);
    if (isBool) {
        if (n == 1) return Value::boolean(iv[0] != 0);
        JSArray* a = ci.vm.newArray();
        for (int k = 0; k < n; ++k) a->elems.push_back(Value::boolean(iv[k] != 0));
        return Value::cell(a);
    }
    if (n == 1) return isInt ? Value::i32(iv[0]) : Value::num(f[0]);
    return isInt ? typed(ci.vm, TA::I32, iv, n) : typed(ci.vm, TA::F32, f, n);
}
// uniform[1234][fi], the vector forms and the matrices
static Value uniformN(CallInfo& ci, GLContext& c, int n, bool isInt) {
    GLint loc = locOf(ci, c);
    if (loc < 0) return Value::undef();
    if (isInt) {
        GLint v[4] = {};
        for (int i = 0; i < n; ++i) v[i] = ci.vm.toInt32(ci.arg(1 + i));
        (n == 1 ? glUniform1iv_ : n == 2 ? glUniform2iv_ : n == 3 ? glUniform3iv_ : glUniform4iv_)(loc, 1, v);
    } else {
        GLfloat v[4] = {};
        for (int i = 0; i < n; ++i) v[i] = (GLfloat)ci.vm.toNumber(ci.arg(1 + i));
        (n == 1 ? glUniform1fv_ : n == 2 ? glUniform2fv_ : n == 3 ? glUniform3fv_ : glUniform4fv_)(loc, 1, v);
    }
    return Value::undef();
}
// an array of values for vectors: some, and whole vectors (5.14.10)
static Value uniformV(CallInfo& ci, GLContext& c, int n, bool isInt) {
    GLint loc = locOf(ci, c);
    if (loc < 0) return Value::undef();
    if (isInt) {
        std::vector<GLint> v;
        if (!numbersOf(ci, ci.arg(1), v) || v.empty() || v.size() % n) { synth(c, GL_INVALID_VALUE); return Value::undef(); }
        (n == 1 ? glUniform1iv_ : n == 2 ? glUniform2iv_ : n == 3 ? glUniform3iv_ : glUniform4iv_)(loc, (GLsizei)(v.size() / n), v.data());
    } else {
        std::vector<GLfloat> v;
        if (!numbersOf(ci, ci.arg(1), v) || v.empty() || v.size() % n) { synth(c, GL_INVALID_VALUE); return Value::undef(); }
        (n == 1 ? glUniform1fv_ : n == 2 ? glUniform2fv_ : n == 3 ? glUniform3fv_ : glUniform4fv_)(loc, (GLsizei)(v.size() / n), v.data());
    }
    return Value::undef();
}
static Value uniformMatrix(CallInfo& ci, GLContext& c, int n) {
    GLint loc = locOf(ci, c);
    if (loc < 0) return Value::undef();
    if (ci.vm.toBool(ci.arg(1))) { synth(c, GL_INVALID_VALUE); return Value::undef(); }  // transpose must be false in WebGL 1
    std::vector<GLfloat> v;
    if (!numbersOf(ci, ci.arg(2), v) || v.empty() || v.size() % (n * n)) { synth(c, GL_INVALID_VALUE); return Value::undef(); }
    (n == 2 ? glUniformMatrix2fv_ : n == 3 ? glUniformMatrix3fv_ : glUniformMatrix4fv_)(loc, (GLsizei)(v.size() / (n * n)), GL_FALSE, v.data());
    return Value::undef();
}
#define UNIFORMS(N)                                                                  \
    GLCALL(uniform##N##f) { CTX; return uniformN(ci, c, N, false); }                 \
    GLCALL(uniform##N##i) { CTX; return uniformN(ci, c, N, true); }                  \
    GLCALL(uniform##N##fv) { CTX; return uniformV(ci, c, N, false); }                \
    GLCALL(uniform##N##iv) { CTX; return uniformV(ci, c, N, true); }
UNIFORMS(1)
UNIFORMS(2)
UNIFORMS(3)
UNIFORMS(4)
GLCALL(uniformMatrix2fv) { CTX; return uniformMatrix(ci, c, 2); }
GLCALL(uniformMatrix3fv) { CTX; return uniformMatrix(ci, c, 3); }
GLCALL(uniformMatrix4fv) { CTX; return uniformMatrix(ci, c, 4); }

// -- vertex attributes and drawing
GLCALL(enableVertexAttribArray) { CTX; glEnableVertexAttribArray_(en(ci, 0)); return Value::undef(); }
GLCALL(disableVertexAttribArray) { CTX; glDisableVertexAttribArray_(en(ci, 0)); return Value::undef(); }
static int typeBytes(GLenum type) {
    return type == GL_BYTE || type == GL_UNSIGNED_BYTE ? 1 : type == GL_SHORT || type == GL_UNSIGNED_SHORT ? 2 : type == kFloat ? 4 : 0;
}
GLCALL(vertexAttribPointer) {
    CTX;
    GLuint index = en(ci, 0);
    GLint size = inti(ci, 1), stride = inti(ci, 4);
    GLenum type = en(ci, 2);
    double offset = num(ci, 5);
    int tb = typeBytes(type);
    if (!tb) { synth(c, GL_INVALID_ENUM); return Value::undef(); }
    if (index >= (GLuint)c.maxAttribs || size < 1 || size > 4 || stride < 0 || stride > 255 || offset < 0) { synth(c, GL_INVALID_VALUE); return Value::undef(); }
    if ((long long)offset % tb || stride % tb) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
    if (c.arrayBuffer.isNull() && offset != 0) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
    glVertexAttribPointer_(index, size, type, ci.vm.toBool(ci.arg(3)), stride, (const void*)(intptr_t)offset);
    return Value::undef();
}
static Value vertexAttrib(CallInfo& ci, int n, bool vec) {
    GLfloat v[4] = {0, 0, 0, 1};
    if (vec) {
        std::vector<GLfloat> f;
        if (numbersOf(ci, ci.arg(1), f))
            for (int i = 0; i < n && i < (int)f.size(); ++i) v[i] = f[i];
    } else
        for (int i = 0; i < n; ++i) v[i] = (GLfloat)ci.vm.toNumber(ci.arg(1 + i));
    glVertexAttrib4fv_(en(ci, 0), v);
    return Value::undef();
}
GLCALL(vertexAttrib1f) { CTX; return vertexAttrib(ci, 1, false); }
GLCALL(vertexAttrib2f) { CTX; return vertexAttrib(ci, 2, false); }
GLCALL(vertexAttrib3f) { CTX; return vertexAttrib(ci, 3, false); }
GLCALL(vertexAttrib4f) { CTX; return vertexAttrib(ci, 4, false); }
GLCALL(vertexAttrib1fv) { CTX; return vertexAttrib(ci, 1, true); }
GLCALL(vertexAttrib2fv) { CTX; return vertexAttrib(ci, 2, true); }
GLCALL(vertexAttrib3fv) { CTX; return vertexAttrib(ci, 3, true); }
GLCALL(vertexAttrib4fv) { CTX; return vertexAttrib(ci, 4, true); }
GLCALL(getVertexAttrib) {
    CTX;
    GLenum p = en(ci, 1);
    if (p == kCurrentVertexAttrib) {
        GLfloat f[4];
        glGetVertexAttribfv_(en(ci, 0), p, f);
        return typed(ci.vm, TA::F32, f, 4);
    }
    GLint v = 0;
    glGetVertexAttribiv_(en(ci, 0), p, &v);
    if (p == 0x8622 /* ENABLED */ || p == 0x886A /* NORMALIZED */) return Value::boolean(v != 0);
    if (p == kVertexAttribArrayBufferBinding) {
        auto it = c.buffers.find((GLuint)v);
        return it != c.buffers.end() ? it->second.object : Value::null();
    }
    return Value::i32(v);
}
GLCALL(getVertexAttribOffset) { CTX; void* p = nullptr; glGetVertexAttribPointerv_(en(ci, 0), en(ci, 1), &p); return Value::dbl((double)(intptr_t)p); }
GLCALL(clear) { CTX; syncSize(c); glClear(en(ci, 0)); if (c.framebuffer.isNull()) c.dirty = true; return Value::undef(); }
// Whether a draw may go ahead (WebGL 1.0, 6.5 and 6.6): there is a linked program, every enabled attribute array
// has a buffer, and the buffers of the attributes the program reads hold every vertex the draw reaches (`vertices`
// from the first, `instances` for attributes with a divisor). Without this the driver would read past them.
static bool drawable(GLContext& c, GLenum mode, long long vertices, long long instances) {
    if (mode > 6) { synth(c, GL_INVALID_ENUM); return false; }
    JSHost* pr = objOf(c.program, HK_GLProgram);
    auto used = pr ? c.programAttribs.find((GLuint)pr->slotA.asInt()) : c.programAttribs.end();
    if (used == c.programAttribs.end()) { synth(c, GL_INVALID_OPERATION); return false; }
    for (GLint i = 0; i < c.maxAttribs; ++i) {
        GLint on = 0, name = 0;
        glGetVertexAttribiv_((GLuint)i, 0x8622 /* ENABLED */, &on);
        if (!on) continue;
        glGetVertexAttribiv_((GLuint)i, kVertexAttribArrayBufferBinding, &name);
        auto b = c.buffers.find((GLuint)name);
        if (!name || b == c.buffers.end()) { synth(c, GL_INVALID_OPERATION); return false; }
        if (std::find(used->second.begin(), used->second.end(), i) == used->second.end()) continue;
        GLint size = 0, type = 0, stride = 0, divisor = 0;
        void* offset = nullptr;
        glGetVertexAttribiv_((GLuint)i, 0x8623 /* SIZE */, &size);
        glGetVertexAttribiv_((GLuint)i, 0x8625 /* TYPE */, &type);
        glGetVertexAttribiv_((GLuint)i, 0x8624 /* STRIDE */, &stride);
        if (glVertexAttribDivisor_) glGetVertexAttribiv_((GLuint)i, kVertexAttribArrayDivisor, &divisor);
        glGetVertexAttribPointerv_((GLuint)i, 0x8645 /* POINTER */, &offset);
        long long elem = (long long)size * typeBytes((GLenum)type), step = stride ? stride : elem;
        long long count = divisor ? (instances + divisor - 1) / divisor : vertices;
        if (count > 0 && (long long)(intptr_t)offset + (count - 1) * step + elem > (long long)b->second.size) {
            synth(c, GL_INVALID_OPERATION);
            return false;
        }
    }
    return true;
}
// The highest vertex an indexed draw reaches, -1 when it may not go ahead (5.14.11, 6.6)
static long long drawnIndices(GLContext& c, GLsizei count, GLenum type, double offset) {
    int size = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 0;
    if (type == GL_UNSIGNED_INT && std::find(c.extensions.begin(), c.extensions.end(), "OES_element_index_uint") != c.extensions.end()) size = 4;
    if (!size) { synth(c, GL_INVALID_ENUM); return -1; }
    if (count < 0 || offset < 0) { synth(c, GL_INVALID_VALUE); return -1; }
    auto b = c.buffers.find(nameOf(c.elementBuffer, HK_GLBuffer));
    if (c.elementBuffer.isNull() || b == c.buffers.end() || (long long)offset % size ||
        offset + (double)count * size > (double)b->second.size) {
        synth(c, GL_INVALID_OPERATION);
        return -1;
    }
    const uint8_t* p = b->second.indices.data() + (size_t)offset;
    long long top = -1;
    for (GLsizei i = 0; i < count; ++i) {
        uint32_t v = size == 1 ? p[i] : size == 2 ? ((const uint16_t*)p)[i] : ((const uint32_t*)p)[i];
        top = std::max<long long>(top, v);
    }
    return top;
}
static Value drawArraysN(CallInfo& ci, GLContext& c, GLsizei instances, bool instanced) {
    GLenum mode = en(ci, 0);
    GLint first = inti(ci, 1), count = inti(ci, 2);
    if (first < 0 || count < 0 || instances < 0) { synth(c, GL_INVALID_VALUE); return Value::undef(); }
    syncSize(c);
    if (!drawable(c, mode, (long long)first + count, instances) || !count || !instances) return Value::undef();
    if (!instanced) glDrawArrays(mode, first, count);
    else glDrawArraysInstanced_(mode, first, count, instances);
    if (c.framebuffer.isNull()) c.dirty = true;
    return Value::undef();
}
static Value drawElementsN(CallInfo& ci, GLContext& c, GLsizei instances, bool instanced) {
    GLenum mode = en(ci, 0), type = en(ci, 2);
    GLsizei count = inti(ci, 1);
    double offset = num(ci, 3);
    if (instances < 0) { synth(c, GL_INVALID_VALUE); return Value::undef(); }
    syncSize(c);
    long long top = drawnIndices(c, count, type, offset);
    if (top < 0 && count > 0) return Value::undef();
    if (!drawable(c, mode, top + 1, instances) || !count || !instances) return Value::undef();
    if (!instanced) glDrawElements(mode, count, type, (const void*)(intptr_t)offset);
    else glDrawElementsInstanced_(mode, count, type, (const void*)(intptr_t)offset, instances);
    if (c.framebuffer.isNull()) c.dirty = true;
    return Value::undef();
}
GLCALL(drawArrays) { CTX; return drawArraysN(ci, c, 1, false); }
GLCALL(drawElements) { CTX; return drawElementsN(ci, c, 1, false); }
GLCALL(readPixels) {
    CTX;
    syncSize(c);
    uint8_t* p;
    size_t n;
    GLint x = inti(ci, 0), y = inti(ci, 1);
    GLsizei w = inti(ci, 2), h = inti(ci, 3);
    GLenum format = en(ci, 4), type = en(ci, 5);
    TA ta;
    if (!bytesOf(ci.arg(6), p, n, &ta) || ci.arg(6).asObject()->cls != Cls::TypedArray)
        ci.vm.throwError(Err::TypeError, "parameter 7 is not of type 'ArrayBufferView'");
    if (w < 0 || h < 0) { synth(c, GL_INVALID_VALUE); return Value::undef(); }
    bool bytes = format == GL_RGBA && type == GL_UNSIGNED_BYTE && ta == TA::U8;
    bool floats = format == GL_RGBA && type == kFloat && ta == TA::F32;
    if (!bytes && !floats) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
    size_t rowBytes = (size_t)w * (bytes ? 4 : 16), row = (rowBytes + c.packAlign - 1) / c.packAlign * c.packAlign;
    if (w && h && n < row * (h - 1) + rowBytes) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
    if (w && h) glReadPixels(x, y, w, h, format, type, p);
    return Value::undef();
}

// -- framebuffers and renderbuffers
GLCALL(createFramebuffer) { CTX; GLuint f = 0; glGenFramebuffers_(1, &f); return newGLObject(ci.vm, c, HK_GLFramebuffer, f, vmc(ci.vm).framebufferProto); }
GLCALL(deleteFramebuffer) {
    CTX;
    if (JSHost* h = objOf(ci.arg(0), HK_GLFramebuffer); h && !h->flagA) {
        GLuint f = (GLuint)h->slotA.asInt();
        if (c.framebuffer == ci.arg(0)) {
            glBindFramebuffer_(kFramebuffer, c.fbo);
            c.framebuffer = Value::null();
        }
        glDeleteFramebuffers_(1, &f);
        h->flagA = true;
    }
    return Value::undef();
}
GLCALL(bindFramebuffer) {
    CTX;
    Value f = ci.arg(1);
    if (!f.isNull() && !objOf(f, HK_GLFramebuffer)) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
    syncSize(c);
    glBindFramebuffer_(en(ci, 0), f.isNull() ? c.fbo : nameOf(f, HK_GLFramebuffer));  // null: the drawing buffer
    c.framebuffer = f.isNull() ? Value::null() : f;
    return Value::undef();
}
GLCALL(isFramebuffer) { CTX; JSHost* h = objOf(ci.arg(0), HK_GLFramebuffer); return Value::boolean(h && !h->flagA); }
GLCALL(checkFramebufferStatus) { CTX; return Value::i32((int32_t)glCheckFramebufferStatus_(en(ci, 0))); }
GLCALL(framebufferTexture2D) { CTX; glFramebufferTexture2D_(en(ci, 0), en(ci, 1), en(ci, 2), nameOf(ci.arg(3), HK_GLTexture), inti(ci, 4)); return Value::undef(); }
GLCALL(framebufferRenderbuffer) {
    CTX;
    GLenum attach = en(ci, 1);
    GLuint rb = nameOf(ci.arg(3), HK_GLRenderbuffer);
    if (attach == kDepthStencilAttachment) {
        glFramebufferRenderbuffer_(en(ci, 0), kDepthAttachment, en(ci, 2), rb);
        glFramebufferRenderbuffer_(en(ci, 0), kStencilAttachment, en(ci, 2), rb);
    } else
        glFramebufferRenderbuffer_(en(ci, 0), attach, en(ci, 2), rb);
    return Value::undef();
}
GLCALL(getFramebufferAttachmentParameter) { CTX; GLint v = 0; glGetFramebufferAttachmentParameteriv_(en(ci, 0), en(ci, 1), en(ci, 2), &v); return Value::i32(v); }
GLCALL(createRenderbuffer) { CTX; GLuint r = 0; glGenRenderbuffers_(1, &r); return newGLObject(ci.vm, c, HK_GLRenderbuffer, r, vmc(ci.vm).renderbufferProto); }
GLCALL(deleteRenderbuffer) {
    CTX;
    if (JSHost* h = objOf(ci.arg(0), HK_GLRenderbuffer); h && !h->flagA) {
        GLuint r = (GLuint)h->slotA.asInt();
        glDeleteRenderbuffers_(1, &r);
        h->flagA = true;
    }
    return Value::undef();
}
GLCALL(bindRenderbuffer) {
    CTX;
    glBindRenderbuffer_(en(ci, 0), nameOf(ci.arg(1), HK_GLRenderbuffer));
    c.renderbuffer = objOf(ci.arg(1), HK_GLRenderbuffer) ? ci.arg(1) : Value::null();
    return Value::undef();
}
GLCALL(isRenderbuffer) { CTX; JSHost* h = objOf(ci.arg(0), HK_GLRenderbuffer); return Value::boolean(h && !h->flagA); }
GLCALL(renderbufferStorage) {
    CTX;
    GLenum internal = en(ci, 1);
    if (internal == kDepthStencil) internal = kDepth24Stencil8;  // DEPTH_STENCIL of WebGL
    if (internal == kRgba4 || internal == kRgb5A1 || internal == kRgb565) internal = kRgba8;
    JSHost* rb = objOf(c.renderbuffer, HK_GLRenderbuffer);
    if (!rb) { synth(c, GL_INVALID_OPERATION); return Value::undef(); }
    keepDriverErrors(c);
    glRenderbufferStorage_(en(ci, 0), internal, inti(ci, 2), inti(ci, 3));
    if (GLenum e = glGetError()) {
        synth(c, e);
        return Value::undef();
    }
    if (!c.scratchFbo) glGenFramebuffers_(1, &c.scratchFbo);
    GLint prev = 0;
    glGetIntegerv(kFramebufferBinding, &prev);
    glBindFramebuffer_(kFramebuffer, c.scratchFbo);
    GLenum attach = internal == kDepthComponent16 ? kDepthAttachment : internal == kStencilIndex8 ? kStencilAttachment : kColorAttachment0;
    if (internal == kDepth24Stencil8) {
        glFramebufferRenderbuffer_(kFramebuffer, kDepthAttachment, kRenderbuffer, (GLuint)rb->slotA.asInt());
        glFramebufferRenderbuffer_(kFramebuffer, kStencilAttachment, kRenderbuffer, (GLuint)rb->slotA.asInt());
    } else
        glFramebufferRenderbuffer_(kFramebuffer, attach, kRenderbuffer, (GLuint)rb->slotA.asInt());
    if (glCheckFramebufferStatus_(kFramebuffer) == kFramebufferComplete) clearDrawingBuffer();
    for (GLenum a : {kColorAttachment0, kDepthAttachment, kStencilAttachment}) glFramebufferRenderbuffer_(kFramebuffer, a, kRenderbuffer, 0);
    glBindFramebuffer_(kFramebuffer, (GLuint)prev);
    glGetError();  // a check of an incomplete scratch framebuffer is no error of the script's
    return Value::undef();
}
GLCALL(getRenderbufferParameter) { CTX; GLint v = 0; glGetRenderbufferParameteriv_(en(ci, 0), en(ci, 1), &v); return Value::i32(v); }

// -- what the context reports
GLCALL(getParameter) {
    CTX;
    Vm& vm = ci.vm;
    GLenum p = en(ci, 0);
    switch (p) {
        case GL_VERSION: return str(vm, "WebGL 1.0 (OpenGL)");
        case kShadingLanguageVersion: return str(vm, "WebGL GLSL ES 1.0");
        case GL_VENDOR: return str(vm, "Tiny Browser");
        case GL_RENDERER: return str(vm, "Tiny Browser WebGL");
        case kUnmaskedVendor: return str(vm, "Tiny Browser");
        case kUnmaskedRenderer: return str(vm, "Tiny Browser WebGL");
        case kArrayBufferBinding: return c.arrayBuffer;
        case kElementArrayBufferBinding: return c.elementBuffer;
        case kCurrentProgram: return c.program;
        case kFramebufferBinding: return c.framebuffer;
        case kRenderbufferBinding: return c.renderbuffer;
        case kVertexArrayBinding: return c.vertexArray;
        case kTextureBinding2D: return c.unit < (int)c.textures2D.size() ? c.textures2D[c.unit] : Value::null();
        case kTextureBindingCubeMap: return c.unit < (int)c.texturesCube.size() ? c.texturesCube[c.unit] : Value::null();
        case kUnpackFlipY: return Value::boolean(c.flipY);
        case kUnpackPremultiplyAlpha: return Value::boolean(c.premultiply);
        case kUnpackColorspaceConversion: return Value::i32((int32_t)kBrowserDefault);
        case kImplementationColorReadFormat: return Value::i32(GL_RGBA);
        case kImplementationColorReadType: return Value::i32(GL_UNSIGNED_BYTE);
        case kMaxVertexUniformVectors: { GLint v = 0; glGetIntegerv(0x8B4A /* MAX_VERTEX_UNIFORM_COMPONENTS */, &v); return Value::i32(std::max(128, v / 4)); }
        case kMaxFragmentUniformVectors: { GLint v = 0; glGetIntegerv(0x8B49 /* MAX_FRAGMENT_UNIFORM_COMPONENTS */, &v); return Value::i32(std::max(16, v / 4)); }
        case kMaxVaryingVectors: { GLint v = 0; glGetIntegerv(0x8B4B /* MAX_VARYING_FLOATS */, &v); return Value::i32(std::max(8, v / 4)); }
        case GL_COLOR_CLEAR_VALUE:
        case kBlendColor:
        case GL_DEPTH_RANGE:
        case kAliasedLineWidthRange:
        case kAliasedPointSizeRange: {
            GLfloat f[4] = {};
            glGetFloatv(p, f);
            return typed(vm, TA::F32, f, p == GL_DEPTH_RANGE || p == kAliasedLineWidthRange || p == kAliasedPointSizeRange ? 2 : 4);
        }
        case GL_VIEWPORT:
        case GL_SCISSOR_BOX:
        case GL_MAX_VIEWPORT_DIMS: {
            GLint v[4] = {};
            glGetIntegerv(p, v);
            return typed(vm, TA::I32, v, p == GL_MAX_VIEWPORT_DIMS ? 2 : 4);
        }
        case GL_COLOR_WRITEMASK: {
            GLboolean b[4];
            glGetBooleanv(p, b);
            JSArray* a = vm.newArray();
            for (int i = 0; i < 4; ++i) a->elems.push_back(Value::boolean(b[i] != 0));
            return Value::cell(a);
        }
        case GL_BLEND: case GL_CULL_FACE: case GL_DEPTH_TEST: case GL_DITHER: case GL_POLYGON_OFFSET_FILL: case GL_SCISSOR_TEST:
        case GL_STENCIL_TEST: case GL_DEPTH_WRITEMASK: {
            GLboolean b = 0;
            glGetBooleanv(p, &b);
            return Value::boolean(b != 0);
        }
        case GL_DEPTH_CLEAR_VALUE: case GL_LINE_WIDTH: case GL_POLYGON_OFFSET_FACTOR: case GL_POLYGON_OFFSET_UNITS: {
            GLfloat f = 0;
            glGetFloatv(p, &f);
            return Value::dbl(f);
        }
        case kSampleBuffers: return Value::i32(0);
        case kSamples: return Value::i32(0);
        default: {
            GLint v = 0;
            glGetIntegerv(p, &v);
            if (glGetError() != GL_NO_ERROR) { synth(c, GL_INVALID_ENUM); return Value::null(); }
            return Value::dbl((double)(GLuint)v == (double)v ? v : (double)(uint32_t)v);
        }
    }
}
GLCALL(getContextAttributes) {
    CTX;
    Vm& vm = ci.vm;
    JSObject* o = vm.newObject();
    vm.def(o, "alpha", Value::boolean(c.alpha), PF_ALL);
    vm.def(o, "depth", Value::boolean(c.depthOn), PF_ALL);
    vm.def(o, "stencil", Value::boolean(c.stencil), PF_ALL);
    vm.def(o, "antialias", Value::boolean(false), PF_ALL);
    vm.def(o, "premultipliedAlpha", Value::boolean(c.premultiplied), PF_ALL);
    vm.def(o, "preserveDrawingBuffer", Value::boolean(c.preserve), PF_ALL);
    vm.def(o, "powerPreference", str(vm, "default"), PF_ALL);
    vm.def(o, "failIfMajorPerformanceCaveat", Value::boolean(false), PF_ALL);
    vm.def(o, "desynchronized", Value::boolean(false), PF_ALL);
    return Value::cell(o);
}
GLCALL(getShaderPrecisionFormat) {
    CTX;
    GLint range[2] = {127, 127}, precision = 23;
    if (glGetShaderPrecisionFormat_) glGetShaderPrecisionFormat_(en(ci, 0), en(ci, 1), range, &precision);
    GLenum t = en(ci, 1);
    if (t >= 0x8DF3 /* LOW_INT */) range[0] = range[1] = 31, precision = 0;  // desktop drivers: 32-bit ints and floats
    else range[0] = range[1] = 127, precision = 23;
    JSObject* o = ci.vm.newObject(vmc(ci.vm).shaderPrecisionProto);
    ci.vm.def(o, "rangeMin", Value::i32(range[0]), PF_E);
    ci.vm.def(o, "rangeMax", Value::i32(range[1]), PF_E);
    ci.vm.def(o, "precision", Value::i32(precision), PF_E);
    return Value::cell(o);
}
static const char* const kExtensions[] = {"ANGLE_instanced_arrays", "OES_element_index_uint", "OES_standard_derivatives",
                                          "OES_texture_float", "OES_texture_half_float", "OES_vertex_array_object",
                                          "EXT_blend_minmax", "WEBGL_lose_context", "WEBGL_debug_renderer_info",
                                          "OES_texture_float_linear", "OES_texture_half_float_linear"};
GLCALL(getSupportedExtensions) {
    CTX;
    JSArray* a = ci.vm.newArray();
    for (const char* e : kExtensions)
        if ((std::string(e) != "OES_vertex_array_object" || glGenVertexArrays_) &&
            (std::string(e) != "ANGLE_instanced_arrays" || (glDrawArraysInstanced_ && glVertexAttribDivisor_)))
            a->elems.push_back(str(ci.vm, e));
    return Value::cell(a);
}
// -- the extensions' own functions
GLCALL(createVertexArray) {
    CTX;
    GLuint v = 0;
    glGenVertexArrays_(1, &v);
    Value o = newGLObject(ci.vm, c, HK_GLVertexArray, v, vmc(ci.vm).vaoProto);
    ((JSHost*)o.asObject())->slotB = Value::null();  // a vertex array's slotB is its element array buffer
    return o;
}
GLCALL(deleteVertexArray) {
    CTX;
    if (JSHost* h = objOf(ci.arg(0), HK_GLVertexArray); h && !h->flagA) {
        GLuint v = (GLuint)h->slotA.asInt();
        glDeleteVertexArrays_(1, &v);
        h->flagA = true;
        if (c.vertexArray == ci.arg(0)) {
            c.vertexArray = Value::null();
            c.elementBuffer = c.defaultElements;
        }
    }
    return Value::undef();
}
GLCALL(bindVertexArray) {
    CTX;
    JSHost* vao = objOf(ci.arg(0), HK_GLVertexArray);
    glBindVertexArray_(vao && !vao->flagA ? (GLuint)vao->slotA.asInt() : 0);
    c.vertexArray = vao && !vao->flagA ? ci.arg(0) : Value::null();
    c.elementBuffer = vao && !vao->flagA ? vao->slotB : c.defaultElements;
    return Value::undef();
}
GLCALL(isVertexArray) { CTX; JSHost* h = objOf(ci.arg(0), HK_GLVertexArray); return Value::boolean(h && !h->flagA); }
GLCALL(drawArraysInstanced) { CTX; return drawArraysN(ci, c, inti(ci, 3), true); }
GLCALL(drawElementsInstanced) { CTX; return drawElementsN(ci, c, inti(ci, 4), true); }
GLCALL(vertexAttribDivisor) { CTX; glVertexAttribDivisor_(en(ci, 0), en(ci, 1)); return Value::undef(); }
GLCALL(loseContext) { return Value::undef(); }
// The extension's object: the same one each time for a context (WebGL 5.14.14)
static Value extensionCall(CallInfo& ci, NativeFn fn) {  // a method of an extension object calls on its context
    Value ctx = ((JSNative*)ci.callee)->data;
    CallInfo inner{ci.vm, ctx, ci.args, ci.argc, Value::undef(), ci.callee};
    return fn(inner);
}
#define EXTFN(name, target) static Value name(CallInfo& ci) { return extensionCall(ci, target); }
EXTFN(extCreateVertexArray, createVertexArray)
EXTFN(extDeleteVertexArray, deleteVertexArray)
EXTFN(extBindVertexArray, bindVertexArray)
EXTFN(extIsVertexArray, isVertexArray)
EXTFN(extDrawArraysInstanced, drawArraysInstanced)
EXTFN(extDrawElementsInstanced, drawElementsInstanced)
EXTFN(extVertexAttribDivisor, vertexAttribDivisor)
GLCALL(getExtension) {
    CTX;
    Vm& vm = ci.vm;
    std::string name = utf16to8(vm.toStr16(ci.arg(0)));
    std::string lower = name;
    for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
    const char* found = nullptr;
    for (const char* e : kExtensions) {
        std::string l = e;
        for (auto& ch : l) ch = (char)tolower((unsigned char)ch);
        if (l == lower) found = e;
    }
    if (!found) return Value::null();
    std::string ext = found;
    if ((ext == "OES_vertex_array_object" && !glGenVertexArrays_) || (ext == "ANGLE_instanced_arrays" && !glDrawArraysInstanced_)) return Value::null();
    if (auto it = c.extObjects.find(ext); it != c.extObjects.end()) return Value::cell(it->second);
    JSObject* o = vm.newObject();
    auto method = [&](const char* n, NativeFn fn, int len) {
        JSNative* f = vm.newNative(fn, n, len);
        f->data = Value::cell(c.self);
        vm.def(o, n, Value::cell(f), PF_W | PF_C | PF_E);
    };
    if (ext == "OES_vertex_array_object") {
        method("createVertexArrayOES", extCreateVertexArray, 0);
        method("deleteVertexArrayOES", extDeleteVertexArray, 1);
        method("bindVertexArrayOES", extBindVertexArray, 1);
        method("isVertexArrayOES", extIsVertexArray, 1);
        vm.def(o, "VERTEX_ARRAY_BINDING_OES", Value::i32(kVertexArrayBinding), PF_E);
    } else if (ext == "ANGLE_instanced_arrays") {
        method("drawArraysInstancedANGLE", extDrawArraysInstanced, 4);
        method("drawElementsInstancedANGLE", extDrawElementsInstanced, 5);
        method("vertexAttribDivisorANGLE", extVertexAttribDivisor, 2);
        vm.def(o, "VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE", Value::i32(kVertexAttribArrayDivisor), PF_E);
    } else if (ext == "OES_standard_derivatives") {
        vm.def(o, "FRAGMENT_SHADER_DERIVATIVE_HINT_OES", Value::i32(kFragmentShaderDerivativeHint), PF_E);
    } else if (ext == "OES_texture_half_float") {
        vm.def(o, "HALF_FLOAT_OES", Value::i32(kHalfFloatOes), PF_E);
    } else if (ext == "EXT_blend_minmax") {
        vm.def(o, "MIN_EXT", Value::i32(0x8007), PF_E);
        vm.def(o, "MAX_EXT", Value::i32(0x8008), PF_E);
    } else if (ext == "WEBGL_lose_context") {
        method("loseContext", loseContext, 0);
        method("restoreContext", loseContext, 0);
    } else if (ext == "WEBGL_debug_renderer_info") {
        vm.def(o, "UNMASKED_VENDOR_WEBGL", Value::i32(kUnmaskedVendor), PF_E);
        vm.def(o, "UNMASKED_RENDERER_WEBGL", Value::i32(kUnmaskedRenderer), PF_E);
    }
    c.extObjects[ext] = o;
    c.extensions.push_back(ext);
    return Value::cell(o);
}
GLCALL(drawingBufferWidth) { CTX; syncSize(c); return Value::i32(c.w); }
GLCALL(drawingBufferHeight) { CTX; syncSize(c); return Value::i32(c.h); }
GLCALL(canvasGet) { CTX; VmContexts& v = vmc(ci.vm); return v.host ? v.host->wrapNode(c.canvas) : Value::null(); }

// __webglContext(canvas, type, attributes): the canvas's context, made the first time (HTML 4.12.5.1, getContext)
static Value webglContext(CallInfo& ci) {
    Vm& vm = ci.vm;
    VmContexts& v = vmc(vm);
    Node* canvas = v.host ? v.host->nodeOf(ci.arg(0)) : nullptr;
    if (!canvas) return Value::null();
    for (auto& c : v.contexts)
        if (c->canvas == canvas) return Value::cell(c->self);
    auto c = newContext();
    if (!c) return Value::null();
    c->canvas = canvas;
    Value attrs = ci.arg(2);
    auto flag = [&](const char* n, bool dflt) {
        if (!attrs.isObject()) return dflt;
        Value x = vm.get(attrs, n);
        return x.isUndef() ? dflt : vm.toBool(x);
    };
    c->alpha = flag("alpha", true);
    c->depthOn = flag("depth", true);
    c->stencil = flag("stencil", false);
    c->premultiplied = flag("premultipliedAlpha", true);
    c->preserve = flag("preserveDrawingBuffer", false);
    JSHost* h = vm.alloc<JSHost>();
    h->proto = v.ctxProto;
    h->hostKind = HK_GLContext;
    h->ptr = c.get();
    c->self = h;
    sizeDrawingBuffer(*c, canvas_dimension(canvas, false), canvas_dimension(canvas, true));
    glBindFramebuffer_(kFramebuffer, c->fbo);
    glViewport(0, 0, c->w, c->h);
    glScissor(0, 0, c->w, c->h);
    glGetIntegerv(kMaxVertexAttribs, &c->maxAttribs);
    c->maxAttribs = std::min(c->maxAttribs, 16);
    glEnable(0x8642 /* VERTEX_PROGRAM_POINT_SIZE: gl_PointSize works as in GLSL ES */);
    glEnable(0x8861 /* POINT_SPRITE: gl_PointCoord */);
    v.contexts.push_back(std::move(c));
    return Value::cell(h);
}

// the constants of WebGLRenderingContextBase (WebGL 1.0, 5.14)
static const std::pair<const char*, uint32_t> kConstants[] = {
    {"DEPTH_BUFFER_BIT", 0x100}, {"STENCIL_BUFFER_BIT", 0x400}, {"COLOR_BUFFER_BIT", 0x4000}, {"POINTS", 0}, {"LINES", 1},
    {"LINE_LOOP", 2}, {"LINE_STRIP", 3}, {"TRIANGLES", 4}, {"TRIANGLE_STRIP", 5}, {"TRIANGLE_FAN", 6}, {"ZERO", 0}, {"ONE", 1},
    {"SRC_COLOR", 0x300}, {"ONE_MINUS_SRC_COLOR", 0x301}, {"SRC_ALPHA", 0x302}, {"ONE_MINUS_SRC_ALPHA", 0x303}, {"DST_ALPHA", 0x304},
    {"ONE_MINUS_DST_ALPHA", 0x305}, {"DST_COLOR", 0x306}, {"ONE_MINUS_DST_COLOR", 0x307}, {"SRC_ALPHA_SATURATE", 0x308},
    {"FUNC_ADD", 0x8006}, {"BLEND_EQUATION", 0x8009}, {"BLEND_EQUATION_RGB", 0x8009}, {"BLEND_EQUATION_ALPHA", 0x883D},
    {"FUNC_SUBTRACT", 0x800A}, {"FUNC_REVERSE_SUBTRACT", 0x800B}, {"BLEND_DST_RGB", 0x80C8}, {"BLEND_SRC_RGB", 0x80C9},
    {"BLEND_DST_ALPHA", 0x80CA}, {"BLEND_SRC_ALPHA", 0x80CB}, {"CONSTANT_COLOR", 0x8001}, {"ONE_MINUS_CONSTANT_COLOR", 0x8002},
    {"CONSTANT_ALPHA", 0x8003}, {"ONE_MINUS_CONSTANT_ALPHA", 0x8004}, {"BLEND_COLOR", 0x8005}, {"ARRAY_BUFFER", 0x8892},
    {"ELEMENT_ARRAY_BUFFER", 0x8893}, {"ARRAY_BUFFER_BINDING", 0x8894}, {"ELEMENT_ARRAY_BUFFER_BINDING", 0x8895},
    {"STREAM_DRAW", 0x88E0}, {"STATIC_DRAW", 0x88E4}, {"DYNAMIC_DRAW", 0x88E8}, {"BUFFER_SIZE", 0x8764}, {"BUFFER_USAGE", 0x8765},
    {"CURRENT_VERTEX_ATTRIB", 0x8626}, {"FRONT", 0x404}, {"BACK", 0x405}, {"FRONT_AND_BACK", 0x408}, {"CULL_FACE", 0xB44},
    {"BLEND", 0xBE2}, {"DITHER", 0xBD0}, {"STENCIL_TEST", 0xB90}, {"DEPTH_TEST", 0xB71}, {"SCISSOR_TEST", 0xC11},
    {"POLYGON_OFFSET_FILL", 0x8037}, {"SAMPLE_ALPHA_TO_COVERAGE", 0x809E}, {"SAMPLE_COVERAGE", 0x80A0}, {"NO_ERROR", 0},
    {"INVALID_ENUM", 0x500}, {"INVALID_VALUE", 0x501}, {"INVALID_OPERATION", 0x502}, {"OUT_OF_MEMORY", 0x505}, {"CW", 0x900},
    {"CCW", 0x901}, {"LINE_WIDTH", 0xB21}, {"ALIASED_POINT_SIZE_RANGE", 0x846D}, {"ALIASED_LINE_WIDTH_RANGE", 0x846E},
    {"CULL_FACE_MODE", 0xB45}, {"FRONT_FACE", 0xB46}, {"DEPTH_RANGE", 0xB70}, {"DEPTH_WRITEMASK", 0xB72},
    {"DEPTH_CLEAR_VALUE", 0xB73}, {"DEPTH_FUNC", 0xB74}, {"STENCIL_CLEAR_VALUE", 0xB91}, {"STENCIL_FUNC", 0xB92},
    {"STENCIL_FAIL", 0xB94}, {"STENCIL_PASS_DEPTH_FAIL", 0xB95}, {"STENCIL_PASS_DEPTH_PASS", 0xB96}, {"STENCIL_REF", 0xB97},
    {"STENCIL_VALUE_MASK", 0xB93}, {"STENCIL_WRITEMASK", 0xB98}, {"STENCIL_BACK_FUNC", 0x8800}, {"STENCIL_BACK_FAIL", 0x8801},
    {"STENCIL_BACK_PASS_DEPTH_FAIL", 0x8802}, {"STENCIL_BACK_PASS_DEPTH_PASS", 0x8803}, {"STENCIL_BACK_REF", 0x8CA3},
    {"STENCIL_BACK_VALUE_MASK", 0x8CA4}, {"STENCIL_BACK_WRITEMASK", 0x8CA5}, {"VIEWPORT", 0xBA2}, {"SCISSOR_BOX", 0xC10},
    {"COLOR_CLEAR_VALUE", 0xC22}, {"COLOR_WRITEMASK", 0xC23}, {"UNPACK_ALIGNMENT", 0xCF5}, {"PACK_ALIGNMENT", 0xD05},
    {"MAX_TEXTURE_SIZE", 0xD33}, {"MAX_VIEWPORT_DIMS", 0xD3A}, {"SUBPIXEL_BITS", 0xD50}, {"RED_BITS", 0xD52},
    {"GREEN_BITS", 0xD53}, {"BLUE_BITS", 0xD54}, {"ALPHA_BITS", 0xD55}, {"DEPTH_BITS", 0xD56}, {"STENCIL_BITS", 0xD57},
    {"POLYGON_OFFSET_UNITS", 0x2A00}, {"POLYGON_OFFSET_FACTOR", 0x8038}, {"TEXTURE_BINDING_2D", 0x8069},
    {"SAMPLE_BUFFERS", 0x80A8}, {"SAMPLES", 0x80A9}, {"SAMPLE_COVERAGE_VALUE", 0x80AA}, {"SAMPLE_COVERAGE_INVERT", 0x80AB},
    {"COMPRESSED_TEXTURE_FORMATS", 0x86A3}, {"DONT_CARE", 0x1100}, {"FASTEST", 0x1101}, {"NICEST", 0x1102},
    {"GENERATE_MIPMAP_HINT", 0x8192}, {"BYTE", 0x1400}, {"UNSIGNED_BYTE", 0x1401}, {"SHORT", 0x1402}, {"UNSIGNED_SHORT", 0x1403},
    {"INT", 0x1404}, {"UNSIGNED_INT", 0x1405}, {"FLOAT", 0x1406}, {"DEPTH_COMPONENT", 0x1902}, {"ALPHA", 0x1906}, {"RGB", 0x1907},
    {"RGBA", 0x1908}, {"LUMINANCE", 0x1909}, {"LUMINANCE_ALPHA", 0x190A}, {"UNSIGNED_SHORT_4_4_4_4", 0x8033},
    {"UNSIGNED_SHORT_5_5_5_1", 0x8034}, {"UNSIGNED_SHORT_5_6_5", 0x8363}, {"FRAGMENT_SHADER", 0x8B30}, {"VERTEX_SHADER", 0x8B31},
    {"MAX_VERTEX_ATTRIBS", 0x8869}, {"MAX_VERTEX_UNIFORM_VECTORS", 0x8DFB}, {"MAX_VARYING_VECTORS", 0x8DFC},
    {"MAX_COMBINED_TEXTURE_IMAGE_UNITS", 0x8B4D}, {"MAX_VERTEX_TEXTURE_IMAGE_UNITS", 0x8B4C}, {"MAX_TEXTURE_IMAGE_UNITS", 0x8872},
    {"MAX_FRAGMENT_UNIFORM_VECTORS", 0x8DFD}, {"SHADER_TYPE", 0x8B4F}, {"DELETE_STATUS", 0x8B80}, {"LINK_STATUS", 0x8B82},
    {"VALIDATE_STATUS", 0x8B83}, {"ATTACHED_SHADERS", 0x8B85}, {"ACTIVE_UNIFORMS", 0x8B86}, {"ACTIVE_ATTRIBUTES", 0x8B89},
    {"SHADING_LANGUAGE_VERSION", 0x8B8C}, {"CURRENT_PROGRAM", 0x8B8D}, {"NEVER", 0x200}, {"LESS", 0x201}, {"EQUAL", 0x202},
    {"LEQUAL", 0x203}, {"GREATER", 0x204}, {"NOTEQUAL", 0x205}, {"GEQUAL", 0x206}, {"ALWAYS", 0x207}, {"KEEP", 0x1E00},
    {"REPLACE", 0x1E01}, {"INCR", 0x1E02}, {"DECR", 0x1E03}, {"INVERT", 0x150A}, {"INCR_WRAP", 0x8507}, {"DECR_WRAP", 0x8508},
    {"VENDOR", 0x1F00}, {"RENDERER", 0x1F01}, {"VERSION", 0x1F02}, {"NEAREST", 0x2600}, {"LINEAR", 0x2601},
    {"NEAREST_MIPMAP_NEAREST", 0x2700}, {"LINEAR_MIPMAP_NEAREST", 0x2701}, {"NEAREST_MIPMAP_LINEAR", 0x2702},
    {"LINEAR_MIPMAP_LINEAR", 0x2703}, {"TEXTURE_MAG_FILTER", 0x2800}, {"TEXTURE_MIN_FILTER", 0x2801}, {"TEXTURE_WRAP_S", 0x2802},
    {"TEXTURE_WRAP_T", 0x2803}, {"TEXTURE_2D", 0xDE1}, {"TEXTURE", 0x1702}, {"TEXTURE_CUBE_MAP", 0x8513},
    {"TEXTURE_BINDING_CUBE_MAP", 0x8514}, {"TEXTURE_CUBE_MAP_POSITIVE_X", 0x8515}, {"TEXTURE_CUBE_MAP_NEGATIVE_X", 0x8516},
    {"TEXTURE_CUBE_MAP_POSITIVE_Y", 0x8517}, {"TEXTURE_CUBE_MAP_NEGATIVE_Y", 0x8518}, {"TEXTURE_CUBE_MAP_POSITIVE_Z", 0x8519},
    {"TEXTURE_CUBE_MAP_NEGATIVE_Z", 0x851A}, {"MAX_CUBE_MAP_TEXTURE_SIZE", 0x851C}, {"ACTIVE_TEXTURE", 0x84E0},
    {"REPEAT", 0x2901}, {"CLAMP_TO_EDGE", 0x812F}, {"MIRRORED_REPEAT", 0x8370}, {"FLOAT_VEC2", 0x8B50}, {"FLOAT_VEC3", 0x8B51},
    {"FLOAT_VEC4", 0x8B52}, {"INT_VEC2", 0x8B53}, {"INT_VEC3", 0x8B54}, {"INT_VEC4", 0x8B55}, {"BOOL", 0x8B56},
    {"BOOL_VEC2", 0x8B57}, {"BOOL_VEC3", 0x8B58}, {"BOOL_VEC4", 0x8B59}, {"FLOAT_MAT2", 0x8B5A}, {"FLOAT_MAT3", 0x8B5B},
    {"FLOAT_MAT4", 0x8B5C}, {"SAMPLER_2D", 0x8B5E}, {"SAMPLER_CUBE", 0x8B60}, {"VERTEX_ATTRIB_ARRAY_ENABLED", 0x8622},
    {"VERTEX_ATTRIB_ARRAY_SIZE", 0x8623}, {"VERTEX_ATTRIB_ARRAY_STRIDE", 0x8624}, {"VERTEX_ATTRIB_ARRAY_TYPE", 0x8625},
    {"VERTEX_ATTRIB_ARRAY_NORMALIZED", 0x886A}, {"VERTEX_ATTRIB_ARRAY_POINTER", 0x8645},
    {"VERTEX_ATTRIB_ARRAY_BUFFER_BINDING", 0x889F}, {"IMPLEMENTATION_COLOR_READ_TYPE", 0x8B9A},
    {"IMPLEMENTATION_COLOR_READ_FORMAT", 0x8B9B}, {"COMPILE_STATUS", 0x8B81}, {"LOW_FLOAT", 0x8DF0}, {"MEDIUM_FLOAT", 0x8DF1},
    {"HIGH_FLOAT", 0x8DF2}, {"LOW_INT", 0x8DF3}, {"MEDIUM_INT", 0x8DF4}, {"HIGH_INT", 0x8DF5}, {"FRAMEBUFFER", 0x8D40},
    {"RENDERBUFFER", 0x8D41}, {"RGBA4", 0x8056}, {"RGB5_A1", 0x8057}, {"RGB565", 0x8D62}, {"DEPTH_COMPONENT16", 0x81A5},
    {"STENCIL_INDEX8", 0x8D48}, {"DEPTH_STENCIL", 0x84F9}, {"RENDERBUFFER_WIDTH", 0x8D42}, {"RENDERBUFFER_HEIGHT", 0x8D43},
    {"RENDERBUFFER_INTERNAL_FORMAT", 0x8D44}, {"RENDERBUFFER_RED_SIZE", 0x8D50}, {"RENDERBUFFER_GREEN_SIZE", 0x8D51},
    {"RENDERBUFFER_BLUE_SIZE", 0x8D52}, {"RENDERBUFFER_ALPHA_SIZE", 0x8D53}, {"RENDERBUFFER_DEPTH_SIZE", 0x8D54},
    {"RENDERBUFFER_STENCIL_SIZE", 0x8D55}, {"FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE", 0x8CD0},
    {"FRAMEBUFFER_ATTACHMENT_OBJECT_NAME", 0x8CD1}, {"FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL", 0x8CD2},
    {"FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE", 0x8CD3}, {"COLOR_ATTACHMENT0", 0x8CE0}, {"DEPTH_ATTACHMENT", 0x8D00},
    {"STENCIL_ATTACHMENT", 0x8D20}, {"DEPTH_STENCIL_ATTACHMENT", 0x821A}, {"NONE", 0}, {"FRAMEBUFFER_COMPLETE", 0x8CD5},
    {"FRAMEBUFFER_INCOMPLETE_ATTACHMENT", 0x8CD6}, {"FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT", 0x8CD7},
    {"FRAMEBUFFER_INCOMPLETE_DIMENSIONS", 0x8CD9}, {"FRAMEBUFFER_UNSUPPORTED", 0x8CDD}, {"FRAMEBUFFER_BINDING", 0x8CA6},
    {"RENDERBUFFER_BINDING", 0x8CA7}, {"MAX_RENDERBUFFER_SIZE", 0x84E8}, {"INVALID_FRAMEBUFFER_OPERATION", 0x506},
    {"UNPACK_FLIP_Y_WEBGL", 0x9240}, {"UNPACK_PREMULTIPLY_ALPHA_WEBGL", 0x9241}, {"CONTEXT_LOST_WEBGL", 0x9242},
    {"UNPACK_COLORSPACE_CONVERSION_WEBGL", 0x9243}, {"BROWSER_DEFAULT_WEBGL", 0x9244}};

void webgl_install(Vm& vm, JSObject* G, CanvasHost& host) {
    VmContexts& v = vmc(vm);
    v.host = &host;
    // what the contexts keep alive: their own objects, the objects bound to them and the extension objects
    vm.rootTracers.push_back([&vm](Gc& g) {
        auto it = registry().find(&vm);
        if (it == registry().end()) return;
        VmContexts& v = it->second;
        for (JSObject* p : {v.ctxProto, v.bufferProto, v.textureProto, v.programProto, v.shaderProto, v.framebufferProto,
                            v.renderbufferProto, v.uniformProto, v.activeInfoProto, v.shaderPrecisionProto, v.vaoProto})
            if (p) g.mark(p);
        for (auto& c : v.contexts) {
            g.mark(c->self);
            for (Value x : {c->framebuffer, c->arrayBuffer, c->elementBuffer, c->program, c->renderbuffer, c->vertexArray, c->defaultElements}) g.mark(x);
            for (auto& b : c->buffers) g.mark(b.second.object);
            for (Value x : c->textures2D) g.mark(x);
            for (Value x : c->texturesCube) g.mark(x);
            for (auto& e : c->extObjects) g.mark(e.second);
        }
    });
    auto iface = [&](const char* name) {
        JSObject* proto = vm.newObject();
        JSNative* ctor = vm.newNative([](CallInfo& ci) -> Value { ci.vm.throwError(Err::TypeError, "Illegal constructor"); }, name, 0);
        vm.def(ctor, "prototype", Value::cell(proto), 0);
        vm.def(proto, "constructor", Value::cell(ctor), PF_W | PF_C);
        vm.def(G, name, Value::cell(ctor), PF_W | PF_C);
        return std::make_pair(proto, ctor);
    };
    auto [ctxProto, ctxCtor] = iface("WebGLRenderingContext");
    v.ctxProto = ctxProto;
    v.bufferProto = iface("WebGLBuffer").first;
    v.textureProto = iface("WebGLTexture").first;
    v.programProto = iface("WebGLProgram").first;
    v.shaderProto = iface("WebGLShader").first;
    v.framebufferProto = iface("WebGLFramebuffer").first;
    v.renderbufferProto = iface("WebGLRenderbuffer").first;
    v.uniformProto = iface("WebGLUniformLocation").first;
    v.activeInfoProto = iface("WebGLActiveInfo").first;
    v.shaderPrecisionProto = iface("WebGLShaderPrecisionFormat").first;
    v.vaoProto = iface("WebGLVertexArrayObjectOES").first;
    for (auto& [name, value] : kConstants) {
        vm.def(ctxProto, name, Value::i32((int32_t)value), PF_E);
        vm.def(ctxCtor, name, Value::i32((int32_t)value), PF_E);
    }
    struct M {
        const char* name;
        NativeFn fn;
        int len;
    };
    static const M methods[] = {
        {"activeTexture", glActiveTextureJS, 1}, {"attachShader", attachShader, 2}, {"bindAttribLocation", bindAttribLocation, 3},
        {"bindBuffer", bindBuffer, 2}, {"bindFramebuffer", bindFramebuffer, 2}, {"bindRenderbuffer", bindRenderbuffer, 2},
        {"bindTexture", bindTexture, 2}, {"blendColor", blendColor, 4}, {"blendEquation", blendEquation, 1},
        {"blendEquationSeparate", blendEquationSeparate, 2}, {"blendFunc", blendFunc, 2}, {"blendFuncSeparate", blendFuncSeparate, 4},
        {"bufferData", bufferData, 3}, {"bufferSubData", bufferSubData, 3}, {"checkFramebufferStatus", checkFramebufferStatus, 1},
        {"clear", clear, 1}, {"clearColor", clearColor, 4}, {"clearDepth", clearDepth, 1}, {"clearStencil", clearStencil, 1},
        {"colorMask", colorMask, 4}, {"compileShader", compileShader, 1}, {"compressedTexImage2D", compressedTexImage2D, 7},
        {"compressedTexSubImage2D", compressedTexImage2D, 8}, {"copyTexImage2D", copyTexImage2D, 8},
        {"copyTexSubImage2D", copyTexSubImage2D, 8}, {"createBuffer", createBuffer, 0}, {"createFramebuffer", createFramebuffer, 0},
        {"createProgram", createProgram, 0}, {"createRenderbuffer", createRenderbuffer, 0}, {"createShader", createShader, 1},
        {"createTexture", createTexture, 0}, {"cullFace", cullFace, 1}, {"deleteBuffer", deleteBuffer, 1},
        {"deleteFramebuffer", deleteFramebuffer, 1}, {"deleteProgram", deleteProgram, 1}, {"deleteRenderbuffer", deleteRenderbuffer, 1},
        {"deleteShader", deleteShader, 1}, {"deleteTexture", deleteTexture, 1}, {"depthFunc", depthFunc, 1}, {"depthMask", depthMask, 1},
        {"depthRange", depthRange, 2}, {"detachShader", detachShader, 2}, {"disable", disable, 1},
        {"disableVertexAttribArray", disableVertexAttribArray, 1}, {"drawArrays", drawArrays, 3}, {"drawElements", drawElements, 4},
        {"enable", enable, 1}, {"enableVertexAttribArray", enableVertexAttribArray, 1}, {"finish", finish, 0}, {"flush", flush, 0},
        {"framebufferRenderbuffer", framebufferRenderbuffer, 4}, {"framebufferTexture2D", framebufferTexture2D, 5},
        {"frontFace", frontFace, 1}, {"generateMipmap", generateMipmap, 1}, {"getActiveAttrib", getActiveAttrib, 2},
        {"getActiveUniform", getActiveUniform, 2}, {"getAttachedShaders", getAttachedShaders, 1},
        {"getAttribLocation", getAttribLocation, 2}, {"getBufferParameter", getBufferParameter, 2},
        {"getContextAttributes", getContextAttributes, 0}, {"getError", getError, 0}, {"getExtension", getExtension, 1},
        {"getFramebufferAttachmentParameter", getFramebufferAttachmentParameter, 3}, {"getParameter", getParameter, 1},
        {"getProgramInfoLog", getProgramInfoLog, 1}, {"getProgramParameter", getProgramParameter, 2},
        {"getRenderbufferParameter", getRenderbufferParameter, 2}, {"getShaderInfoLog", getShaderInfoLog, 1},
        {"getShaderParameter", getShaderParameter, 2}, {"getShaderPrecisionFormat", getShaderPrecisionFormat, 2},
        {"getShaderSource", getShaderSource, 1}, {"getSupportedExtensions", getSupportedExtensions, 0},
        {"getTexParameter", getTexParameter, 2}, {"getUniform", getUniform, 2}, {"getUniformLocation", getUniformLocation, 2},
        {"getVertexAttrib", getVertexAttrib, 2}, {"getVertexAttribOffset", getVertexAttribOffset, 2}, {"hint", hint, 2},
        {"isBuffer", isBuffer, 1}, {"isContextLost", isContextLost, 0}, {"isEnabled", isEnabled, 1},
        {"isFramebuffer", isFramebuffer, 1}, {"isProgram", isProgram, 1}, {"isRenderbuffer", isRenderbuffer, 1},
        {"isShader", isShader, 1}, {"isTexture", isTexture, 1}, {"lineWidth", lineWidth, 1}, {"linkProgram", linkProgram, 1},
        {"pixelStorei", pixelStorei, 2}, {"polygonOffset", polygonOffset, 2}, {"readPixels", readPixels, 7},
        {"renderbufferStorage", renderbufferStorage, 4}, {"sampleCoverage", sampleCoverage, 2}, {"scissor", scissor, 4},
        {"shaderSource", shaderSource, 2}, {"stencilFunc", stencilFunc, 3}, {"stencilFuncSeparate", stencilFuncSeparate, 4},
        {"stencilMask", stencilMask, 1}, {"stencilMaskSeparate", stencilMaskSeparate, 2}, {"stencilOp", stencilOp, 3},
        {"stencilOpSeparate", stencilOpSeparate, 4}, {"texImage2D", texImage2D, 6}, {"texParameterf", texParameterf, 3},
        {"texParameteri", texParameteri, 3}, {"texSubImage2D", texSubImage2D, 7}, {"uniform1f", uniform1f, 2},
        {"uniform1fv", uniform1fv, 2}, {"uniform1i", uniform1i, 2}, {"uniform1iv", uniform1iv, 2}, {"uniform2f", uniform2f, 3},
        {"uniform2fv", uniform2fv, 2}, {"uniform2i", uniform2i, 3}, {"uniform2iv", uniform2iv, 2}, {"uniform3f", uniform3f, 4},
        {"uniform3fv", uniform3fv, 2}, {"uniform3i", uniform3i, 4}, {"uniform3iv", uniform3iv, 2}, {"uniform4f", uniform4f, 5},
        {"uniform4fv", uniform4fv, 2}, {"uniform4i", uniform4i, 5}, {"uniform4iv", uniform4iv, 2},
        {"uniformMatrix2fv", uniformMatrix2fv, 3}, {"uniformMatrix3fv", uniformMatrix3fv, 3}, {"uniformMatrix4fv", uniformMatrix4fv, 3},
        {"useProgram", useProgram, 1}, {"validateProgram", validateProgram, 1}, {"vertexAttrib1f", vertexAttrib1f, 2},
        {"vertexAttrib1fv", vertexAttrib1fv, 2}, {"vertexAttrib2f", vertexAttrib2f, 3}, {"vertexAttrib2fv", vertexAttrib2fv, 2},
        {"vertexAttrib3f", vertexAttrib3f, 4}, {"vertexAttrib3fv", vertexAttrib3fv, 2}, {"vertexAttrib4f", vertexAttrib4f, 5},
        {"vertexAttrib4fv", vertexAttrib4fv, 2}, {"vertexAttribPointer", vertexAttribPointer, 6}, {"viewport", viewport, 4}};
    for (const M& m : methods) vm.defFn(ctxProto, m.name, m.fn, m.len);
    vm.defGetter(ctxProto, "drawingBufferWidth", drawingBufferWidth);
    vm.defGetter(ctxProto, "drawingBufferHeight", drawingBufferHeight);
    vm.defGetter(ctxProto, "canvas", canvasGet);
    vm.def(ctxProto, "drawingBufferColorSpace", vm.strv(std::string_view("srgb")), PF_W | PF_E);
    vm.defFn(G, "__webglContext", webglContext, 3);
}

void webgl_present(Vm& vm) {
    auto it = registry().find(&vm);
    if (it == registry().end() || !it->second.host) return;
    for (auto& c : it->second.contexts) {
        if (!c->dirty) continue;
        c->dirty = false;
        makeCurrent(*c);
        GLint prev = 0;
        glGetIntegerv(kFramebufferBinding, &prev);
        glBindFramebuffer_(kFramebuffer, c->fbo);
        std::vector<uint8_t> px((size_t)c->w * c->h * 4), rows(px.size());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, c->w, c->h, kBgra, GL_UNSIGNED_BYTE, px.data());
        glPixelStorei(GL_PACK_ALIGNMENT, c->packAlign);
        if (!c->preserve) clearDrawingBuffer();  // once presented, the next frame starts afresh (WebGL 2.2)
        glBindFramebuffer_(kFramebuffer, (GLuint)prev);
        // OpenGL's rows go up from the bottom; a picture's go down from the top
        size_t row = (size_t)c->w * 4;
        for (int y = 0; y < c->h; ++y) memcpy(rows.data() + y * row, px.data() + (size_t)(c->h - 1 - y) * row, row);
        if (!c->alpha)
            for (size_t i = 3; i < rows.size(); i += 4) rows[i] = 255;
        else if (!c->premultiplied)  // the picture wants premultiplied colours
            for (size_t i = 0; i < rows.size(); i += 4)
                for (int k = 0; k < 3; ++k) rows[i + k] = (uint8_t)((rows[i + k] * rows[i + 3] + 127) / 255);
        it->second.host->canvasPresented(c->canvas, c->w, c->h, std::move(rows));
    }
}

bool webgl_canvas_pixels(Vm& vm, Node* canvas, int& w, int& h, std::vector<uint8_t>& rgba) {
    auto it = registry().find(&vm);
    if (it == registry().end()) return false;
    for (auto& c : it->second.contexts) {
        if (c->canvas != canvas) continue;
        makeCurrent(*c);
        GLint prev = 0;
        glGetIntegerv(kFramebufferBinding, &prev);
        glBindFramebuffer_(kFramebuffer, c->fbo);
        w = c->w;
        h = c->h;
        std::vector<uint8_t> px((size_t)w * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glPixelStorei(GL_PACK_ALIGNMENT, c->packAlign);
        glBindFramebuffer_(kFramebuffer, (GLuint)prev);
        rgba.resize(px.size());
        size_t row = (size_t)w * 4;
        for (int y = 0; y < h; ++y) memcpy(rgba.data() + y * row, px.data() + (size_t)(h - 1 - y) * row, row);
        return true;
    }
    return false;
}

void webgl_release(Vm& vm) {
    auto it = registry().find(&vm);
    if (it == registry().end()) return;
    for (auto& c : it->second.contexts) {
        if (g_current == c->rc) {
            wglMakeCurrent(nullptr, nullptr);
            g_current = nullptr;
        }
        wglDeleteContext(c->rc);
        if (c->dc) ReleaseDC(c->wnd, c->dc);
    }
    registry().erase(it);
}

}  // namespace tb
