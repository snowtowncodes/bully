#include "gl_core.h"
#include "logger.h"

namespace OpenBully::GL {

PFN_glCreateShader CreateShader = nullptr;
PFN_glShaderSource ShaderSource = nullptr;
PFN_glCompileShader CompileShader = nullptr;
PFN_glGetShaderiv GetShaderiv = nullptr;
PFN_glGetShaderInfoLog GetShaderInfoLog = nullptr;
PFN_glDeleteShader DeleteShader = nullptr;
PFN_glCreateProgram CreateProgram = nullptr;
PFN_glAttachShader AttachShader = nullptr;
PFN_glLinkProgram LinkProgram = nullptr;
PFN_glGetProgramiv GetProgramiv = nullptr;
PFN_glGetProgramInfoLog GetProgramInfoLog = nullptr;
PFN_glDeleteProgram DeleteProgram = nullptr;
PFN_glUseProgram UseProgram = nullptr;
PFN_glGenVertexArrays GenVertexArrays = nullptr;
PFN_glBindVertexArray BindVertexArray = nullptr;
PFN_glGenBuffers GenBuffers = nullptr;
PFN_glBindBuffer BindBuffer = nullptr;
PFN_glBufferData BufferData = nullptr;
PFN_glVertexAttribPointer VertexAttribPointer = nullptr;
PFN_glEnableVertexAttribArray EnableVertexAttribArray = nullptr;
PFN_glClearColor ClearColor = nullptr;
PFN_glClear Clear = nullptr;
PFN_glDrawArrays DrawArrays = nullptr;
PFN_glEnable Enable = nullptr;
PFN_glViewport Viewport = nullptr;
PFN_glGetString GetString = nullptr;

namespace {

template <typename T>
bool LoadFn(T& fn, const char* name) {
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    if (!fn) {
        Logger::Error("Failed to load GL function: %s", name);
        return false;
    }
    return true;
}

} // namespace

bool Load() {
    bool ok = true;
    ok &= LoadFn(CreateShader, "glCreateShader");
    ok &= LoadFn(ShaderSource, "glShaderSource");
    ok &= LoadFn(CompileShader, "glCompileShader");
    ok &= LoadFn(GetShaderiv, "glGetShaderiv");
    ok &= LoadFn(GetShaderInfoLog, "glGetShaderInfoLog");
    ok &= LoadFn(DeleteShader, "glDeleteShader");
    ok &= LoadFn(CreateProgram, "glCreateProgram");
    ok &= LoadFn(AttachShader, "glAttachShader");
    ok &= LoadFn(LinkProgram, "glLinkProgram");
    ok &= LoadFn(GetProgramiv, "glGetProgramiv");
    ok &= LoadFn(GetProgramInfoLog, "glGetProgramInfoLog");
    ok &= LoadFn(DeleteProgram, "glDeleteProgram");
    ok &= LoadFn(UseProgram, "glUseProgram");
    ok &= LoadFn(GenVertexArrays, "glGenVertexArrays");
    ok &= LoadFn(BindVertexArray, "glBindVertexArray");
    ok &= LoadFn(GenBuffers, "glGenBuffers");
    ok &= LoadFn(BindBuffer, "glBindBuffer");
    ok &= LoadFn(BufferData, "glBufferData");
    ok &= LoadFn(VertexAttribPointer, "glVertexAttribPointer");
    ok &= LoadFn(EnableVertexAttribArray, "glEnableVertexAttribArray");
    ok &= LoadFn(ClearColor, "glClearColor");
    ok &= LoadFn(Clear, "glClear");
    ok &= LoadFn(DrawArrays, "glDrawArrays");
    ok &= LoadFn(Enable, "glEnable");
    ok &= LoadFn(Viewport, "glViewport");
    ok &= LoadFn(GetString, "glGetString");

    if (ok) {
        Logger::Info("OpenGL entry points loaded");
    }
    return ok;
}

} // namespace OpenBully::GL
