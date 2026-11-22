/* Minimal OpenGL 3.3 core loader (function pointers fetched through SDL). */
#pragma once
#include <SDL3/SDL_opengl.h>

#ifndef APIENTRY
#define APIENTRY
#endif

#define EB_GL_FUNCS(X)                                                                        \
  X(void, GenTextures, (GLsizei n, GLuint *t))                                                \
  X(void, DeleteTextures, (GLsizei n, const GLuint *t))                                       \
  X(void, BindTexture, (GLenum target, GLuint t))                                             \
  X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
  X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
  X(void, TexImage3D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
  X(void, TexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
  X(void, TexParameteri, (GLenum, GLenum, GLint))                                             \
  X(void, PixelStorei, (GLenum, GLint))                                                       \
  X(void, ActiveTexture, (GLenum))                                                            \
  X(GLuint, CreateShader, (GLenum))                                                           \
  X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))              \
  X(void, CompileShader, (GLuint))                                                            \
  X(void, GetShaderiv, (GLuint, GLenum, GLint *))                                             \
  X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                           \
  X(void, DeleteShader, (GLuint))                                                             \
  X(GLuint, CreateProgram, (void))                                                            \
  X(void, AttachShader, (GLuint, GLuint))                                                     \
  X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *))                               \
  X(void, LinkProgram, (GLuint))                                                              \
  X(void, GetProgramiv, (GLuint, GLenum, GLint *))                                            \
  X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                          \
  X(void, UseProgram, (GLuint))                                                               \
  X(GLint, GetUniformLocation, (GLuint, const GLchar *))                                      \
  X(void, Uniform1i, (GLint, GLint))                                                          \
  X(void, Uniform1f, (GLint, GLfloat))                                                        \
  X(void, Uniform2f, (GLint, GLfloat, GLfloat))                                               \
  X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat))                                      \
  X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                             \
  X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))                     \
  X(void, GenVertexArrays, (GLsizei, GLuint *))                                               \
  X(void, BindVertexArray, (GLuint))                                                          \
  X(void, GenBuffers, (GLsizei, GLuint *))                                                    \
  X(void, DeleteBuffers, (GLsizei, const GLuint *))                                           \
  X(void, BindBuffer, (GLenum, GLuint))                                                       \
  X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum))                             \
  X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *))                        \
  X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))     \
  X(void, VertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void *))               \
  X(void, VertexAttribDivisor, (GLuint, GLuint))                                              \
  X(void, EnableVertexAttribArray, (GLuint))                                                  \
  X(void, DrawArrays, (GLenum, GLint, GLsizei))                                               \
  X(void, DrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))                             \
  X(void, DrawElements, (GLenum, GLsizei, GLenum, const void *))                              \
  X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                         \
  X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                          \
  X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                   \
  X(void, Clear, (GLbitfield))                                                                \
  X(void, Enable, (GLenum))                                                                   \
  X(void, Disable, (GLenum))                                                                  \
  X(void, BlendFunc, (GLenum, GLenum))                                                        \
  X(void, DepthFunc, (GLenum))                                                                \
  X(void, DepthMask, (GLboolean))                                                             \
  X(void, CullFace, (GLenum))                                                                 \
  X(void, GenFramebuffers, (GLsizei, GLuint *))                                               \
  X(void, BindFramebuffer, (GLenum, GLuint))                                                  \
  X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                      \
  X(void, GenRenderbuffers, (GLsizei, GLuint *))                                              \
  X(void, BindRenderbuffer, (GLenum, GLuint))                                                 \
  X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                            \
  X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                          \
  X(GLenum, CheckFramebufferStatus, (GLenum))                                                 \
  X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))               \
  X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
  X(void, DrawBuffer, (GLenum))                                                               \
  X(void, ReadBuffer, (GLenum))                                                               \
  X(void, PolygonOffset, (GLfloat, GLfloat))                                                  \
  X(void, Finish, (void))                                                                     \
  X(GLenum, GetError, (void))

typedef struct GlApi {
#define EB_GL_DECL(ret, name, args) ret(APIENTRY *name) args;
  EB_GL_FUNCS(EB_GL_DECL)
#undef EB_GL_DECL
} GlApi;

extern GlApi gl;
int gl_load(void);  /* returns 0 and logs the first missing function on failure */
GLuint gl_program(const char *vs, const char *fs, const char *what);
