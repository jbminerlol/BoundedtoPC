#include "gl.h"

#include <SDL3/SDL.h>

#include "snes.h"

GlApi gl;

int gl_load(void) {
#define EB_GL_LOAD(ret, name, args)                                       \
  gl.name = (ret(APIENTRY *) args)SDL_GL_GetProcAddress("gl" #name);      \
  if (!gl.name) {                                                         \
    eb_log("OpenGL: missing gl" #name);                                   \
    return 0;                                                             \
  }
  EB_GL_FUNCS(EB_GL_LOAD)
#undef EB_GL_LOAD
  return 1;
}

static GLuint compile(GLenum type, const char *src, const char *what) {
  GLuint s = gl.CreateShader(type);
  gl.ShaderSource(s, 1, &src, NULL);
  gl.CompileShader(s);
  GLint ok = 0;
  gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    gl.GetShaderInfoLog(s, sizeof log, NULL, log);
    eb_log("shader %s (%s) failed: %s", what, type == GL_VERTEX_SHADER ? "vs" : "fs", log);
  }
  return s;
}

GLuint gl_program(const char *vs, const char *fs, const char *what) {
  GLuint p = gl.CreateProgram();
  GLuint a = compile(GL_VERTEX_SHADER, vs, what), b = compile(GL_FRAGMENT_SHADER, fs, what);
  gl.AttachShader(p, a);
  gl.AttachShader(p, b);
  gl.LinkProgram(p);
  GLint ok = 0;
  gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    gl.GetProgramInfoLog(p, sizeof log, NULL, log);
    eb_log("program %s link failed: %s", what, log);
  }
  gl.DeleteShader(a);
  gl.DeleteShader(b);
  return p;
}
