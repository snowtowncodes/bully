#pragma once

// OpenBully: Minimal OpenGL 3.3 core function loader.
// Loads GL entry points via SDL_GL_GetProcAddress. Phase 1 only needs the
// subset below; replace with a full loader (glad/glew) when the renderer
// grows beyond this.

#include <SDL2/SDL.h>

// Calling convention for GL entry points (defined by <GL/gl.h> elsewhere;
// we avoid pulling that header to keep the loader self-contained).
#ifndef GLAPIENTRY
#  ifdef _WIN32
#    define GLAPIENTRY __stdcall
#  else
#    define GLAPIENTRY
#  endif
#endif

namespace OpenBully::GL {

// GL types (subset)
using GLchar = char;
using GLint = int;
using GLsizei = int;
using GLuint = unsigned int;
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLsizeiptr = signed long long;
using GLboolean2 = unsigned char;
using GLfloat = float;

// GL constants (subset used in Phase 1)
constexpr GLenum GL_FALSE = 0;
constexpr GLenum GL_TRUE = 1;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum GL_DEPTH_BUFFER_BIT = 0x00000100;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;

// Function pointer types
using PFN_glCreateShader = GLuint(GLAPIENTRY*)(GLenum);
using PFN_glShaderSource = void(GLAPIENTRY*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void(GLAPIENTRY*)(GLuint);
using PFN_glGetShaderiv = void(GLAPIENTRY*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderInfoLog = void(GLAPIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glDeleteShader = void(GLAPIENTRY*)(GLuint);
using PFN_glCreateProgram = GLuint(GLAPIENTRY*)(void);
using PFN_glAttachShader = void(GLAPIENTRY*)(GLuint, GLuint);
using PFN_glLinkProgram = void(GLAPIENTRY*)(GLuint);
using PFN_glGetProgramiv = void(GLAPIENTRY*)(GLuint, GLenum, GLint*);
using PFN_glGetProgramInfoLog = void(GLAPIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glDeleteProgram = void(GLAPIENTRY*)(GLuint);
using PFN_glUseProgram = void(GLAPIENTRY*)(GLuint);
using PFN_glGenVertexArrays = void(GLAPIENTRY*)(GLsizei, GLuint*);
using PFN_glBindVertexArray = void(GLAPIENTRY*)(GLuint);
using PFN_glGenBuffers = void(GLAPIENTRY*)(GLsizei, GLuint*);
using PFN_glBindBuffer = void(GLAPIENTRY*)(GLenum, GLuint);
using PFN_glBufferData = void(GLAPIENTRY*)(GLenum, GLsizeiptr, const void*, GLenum);
using PFN_glVertexAttribPointer = void(GLAPIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_glEnableVertexAttribArray = void(GLAPIENTRY*)(GLuint);
using PFN_glClearColor = void(GLAPIENTRY*)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glClear = void(GLAPIENTRY*)(GLenum);
using PFN_glDrawArrays = void(GLAPIENTRY*)(GLenum, GLint, GLsizei);
using PFN_glEnable = void(GLAPIENTRY*)(GLenum);
using PFN_glViewport = void(GLAPIENTRY*)(GLint, GLint, GLsizei, GLsizei);
using PFN_glGetString = const GLchar* (GLAPIENTRY*)(GLenum);

extern PFN_glCreateShader CreateShader;
extern PFN_glShaderSource ShaderSource;
extern PFN_glCompileShader CompileShader;
extern PFN_glGetShaderiv GetShaderiv;
extern PFN_glGetShaderInfoLog GetShaderInfoLog;
extern PFN_glDeleteShader DeleteShader;
extern PFN_glCreateProgram CreateProgram;
extern PFN_glAttachShader AttachShader;
extern PFN_glLinkProgram LinkProgram;
extern PFN_glGetProgramiv GetProgramiv;
extern PFN_glGetProgramInfoLog GetProgramInfoLog;
extern PFN_glDeleteProgram DeleteProgram;
extern PFN_glUseProgram UseProgram;
extern PFN_glGenVertexArrays GenVertexArrays;
extern PFN_glBindVertexArray BindVertexArray;
extern PFN_glGenBuffers GenBuffers;
extern PFN_glBindBuffer BindBuffer;
extern PFN_glBufferData BufferData;
extern PFN_glVertexAttribPointer VertexAttribPointer;
extern PFN_glEnableVertexAttribArray EnableVertexAttribArray;
extern PFN_glClearColor ClearColor;
extern PFN_glClear Clear;
extern PFN_glDrawArrays DrawArrays;
extern PFN_glEnable Enable;
extern PFN_glViewport Viewport;
extern PFN_glGetString GetString;

// Load all entry points. Returns false if any failed (logs via Logger).
bool Load();

} // namespace OpenBully::GL
