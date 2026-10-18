/* OpenGL 3.3 core entry points, loaded through SDL so nothing links
   against a system GL library directly. */
#ifndef LR2_GL_H
#define LR2_GL_H

#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>

#define GL_FUNCS(X)                                                                          \
    X(void, Enable, (GLenum))                                                                \
    X(void, Disable, (GLenum))                                                               \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                      \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                       \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                \
    X(void, ClearDepth, (GLdouble))                                                          \
    X(void, ClearStencil, (GLint))                                                           \
    X(void, Clear, (GLbitfield))                                                             \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                         \
    X(void, DepthMask, (GLboolean))                                                          \
    X(void, DepthFunc, (GLenum))                                                             \
    X(void, DepthRange, (GLdouble, GLdouble))                                                \
    X(void, StencilMask, (GLuint))                                                           \
    X(void, StencilFunc, (GLenum, GLint, GLuint))                                            \
    X(void, StencilOp, (GLenum, GLenum, GLenum))                                             \
    X(void, BlendFunc, (GLenum, GLenum))                                                     \
    X(void, BlendEquation, (GLenum))                                                         \
    X(void, CullFace, (GLenum))                                                              \
    X(void, FrontFace, (GLenum))                                                             \
    X(void, PolygonMode, (GLenum, GLenum))                                                   \
    X(void, PolygonOffset, (GLfloat, GLfloat))                                               \
    X(void, PixelStorei, (GLenum, GLint))                                                    \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))            \
    X(GLenum, GetError, (void))                                                              \
    X(void, GetIntegerv, (GLenum, GLint *))                                                  \
    X(const GLubyte *, GetString, (GLenum))                                                  \
    X(void, Finish, (void))                                                                  \
    X(void, Flush, (void))                                                                   \
    X(void, GenTextures, (GLsizei, GLuint *))                                                \
    X(void, DeleteTextures, (GLsizei, const GLuint *))                                       \
    X(void, BindTexture, (GLenum, GLuint))                                                   \
    X(void, ActiveTexture, (GLenum))                                                         \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, CompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                          \
    X(void, TexParameterf, (GLenum, GLenum, GLfloat))                                        \
    X(void, TexParameterfv, (GLenum, GLenum, const GLfloat *))                               \
    X(void, CopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
    X(void, GenBuffers, (GLsizei, GLuint *))                                                 \
    X(void, DeleteBuffers, (GLsizei, const GLuint *))                                        \
    X(void, BindBuffer, (GLenum, GLuint))                                                    \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum))                          \
    X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *))                     \
    X(void, GenVertexArrays, (GLsizei, GLuint *))                                            \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint *))                                   \
    X(void, BindVertexArray, (GLuint))                                                       \
    X(void, EnableVertexAttribArray, (GLuint))                                               \
    X(void, DisableVertexAttribArray, (GLuint))                                              \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))  \
    X(void, VertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat))                    \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                                            \
    X(void, DrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void *, GLint))          \
    X(GLuint, CreateShader, (GLenum))                                                        \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))           \
    X(void, CompileShader, (GLuint))                                                         \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *))                                          \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                        \
    X(void, DeleteShader, (GLuint))                                                          \
    X(GLuint, CreateProgram, (void))                                                         \
    X(void, AttachShader, (GLuint, GLuint))                                                  \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *))                            \
    X(void, BindFragDataLocation, (GLuint, GLuint, const GLchar *))                          \
    X(void, LinkProgram, (GLuint))                                                           \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *))                                         \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                       \
    X(void, UseProgram, (GLuint))                                                            \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *))                                   \
    X(void, Uniform1i, (GLint, GLint))                                                       \
    X(void, Uniform1f, (GLint, GLfloat))                                                     \
    X(void, Uniform1fv, (GLint, GLsizei, const GLfloat *))                                   \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat))                                            \
    X(void, Uniform3fv, (GLint, GLsizei, const GLfloat *))                                   \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat *))                                   \
    X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))                  \
    X(void, GenFramebuffers, (GLsizei, GLuint *))                                            \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *))                                   \
    X(void, BindFramebuffer, (GLenum, GLuint))                                               \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                   \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                       \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                              \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, GenRenderbuffers, (GLsizei, GLuint *))                                           \
    X(void, DeleteRenderbuffers, (GLsizei, const GLuint *))                                  \
    X(void, BindRenderbuffer, (GLenum, GLuint))                                              \
    X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                         \
    X(void, ProvokingVertex, (GLenum))                                                       \
    X(void, DrawBuffer, (GLenum))                                                            \
    X(void, ReadBuffer, (GLenum))                                                            \
    X(void, PointSize, (GLfloat))                                                            \
    X(void, GenerateMipmap, (GLenum))

#define X(ret, name, args) typedef ret(APIENTRY *PFN_gl##name) args; extern PFN_gl##name p_gl##name;
GL_FUNCS(X)
#undef X

#define X(ret, name, args)
#define glEnable p_glEnable
#define glDisable p_glDisable
#define glViewport p_glViewport
#define glScissor p_glScissor
#define glClearColor p_glClearColor
#define glClearDepth p_glClearDepth
#define glClearStencil p_glClearStencil
#define glClear p_glClear
#define glColorMask p_glColorMask
#define glDepthMask p_glDepthMask
#define glDepthFunc p_glDepthFunc
#define glDepthRange p_glDepthRange
#define glStencilMask p_glStencilMask
#define glStencilFunc p_glStencilFunc
#define glStencilOp p_glStencilOp
#define glBlendFunc p_glBlendFunc
#define glBlendEquation p_glBlendEquation
#define glCullFace p_glCullFace
#define glFrontFace p_glFrontFace
#define glPolygonMode p_glPolygonMode
#define glPolygonOffset p_glPolygonOffset
#define glPixelStorei p_glPixelStorei
#define glReadPixels p_glReadPixels
#define glGetError p_glGetError
#define glGetIntegerv p_glGetIntegerv
#define glGetString p_glGetString
#define glFinish p_glFinish
#define glFlush p_glFlush
#define glGenTextures p_glGenTextures
#define glDeleteTextures p_glDeleteTextures
#define glBindTexture p_glBindTexture
#define glActiveTexture p_glActiveTexture
#define glTexImage2D p_glTexImage2D
#define glTexSubImage2D p_glTexSubImage2D
#define glCompressedTexImage2D p_glCompressedTexImage2D
#define glTexParameteri p_glTexParameteri
#define glTexParameterf p_glTexParameterf
#define glTexParameterfv p_glTexParameterfv
#define glCopyTexSubImage2D p_glCopyTexSubImage2D
#define glGenBuffers p_glGenBuffers
#define glDeleteBuffers p_glDeleteBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glGenVertexArrays p_glGenVertexArrays
#define glDeleteVertexArrays p_glDeleteVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glVertexAttribPointer p_glVertexAttribPointer
#define glVertexAttrib4f p_glVertexAttrib4f
#define glDrawArrays p_glDrawArrays
#define glDrawElementsBaseVertex p_glDrawElementsBaseVertex
#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glDeleteShader p_glDeleteShader
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glBindAttribLocation p_glBindAttribLocation
#define glBindFragDataLocation p_glBindFragDataLocation
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform1i p_glUniform1i
#define glUniform1f p_glUniform1f
#define glUniform1fv p_glUniform1fv
#define glUniform2f p_glUniform2f
#define glUniform3fv p_glUniform3fv
#define glUniform4fv p_glUniform4fv
#define glUniformMatrix4fv p_glUniformMatrix4fv
#define glGenFramebuffers p_glGenFramebuffers
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glBlitFramebuffer p_glBlitFramebuffer
#define glGenRenderbuffers p_glGenRenderbuffers
#define glDeleteRenderbuffers p_glDeleteRenderbuffers
#define glBindRenderbuffer p_glBindRenderbuffer
#define glRenderbufferStorage p_glRenderbufferStorage
#define glProvokingVertex p_glProvokingVertex
#define glDrawBuffer p_glDrawBuffer
#define glReadBuffer p_glReadBuffer
#define glPointSize p_glPointSize
#define glGenerateMipmap p_glGenerateMipmap
#undef X

/* Load all entry points; false (with a log) if any is missing. */
bool gl_load(void);

#endif
