/* The GL subset port/render/r_mesh.c uses, for the Nintendo 3DS build.
 *
 * The 3DS renderer (r_c3d.c) is citro3d; r_mesh.c (POL pack parsing, the
 * mesh/clay tables, VU0 maths, cameras) stays as it is and talks to this
 * shim: buffers and uniforms are kept in memory, shaders and programs are
 * names, and glDrawElements hands the bound vertex/index buffers, the
 * uniforms and the bound texture to r3ds_mesh_draw (r_c3d.c), which draws
 * the mesh with citro3d.  GLuint texture names are the 3DS renderer's
 * texture ids (r_tex_gl / r_white_texture / r_pal_texture).
 */
#ifndef PORT_GL_SHIM_H
#define PORT_GL_SHIM_H
#include <stddef.h>
#include <stdint.h>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_TRIANGLES 0x0004
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_SCISSOR_BOX 0x0C10
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_DEPTH_FUNC 0x0B74
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_EQUATION_RGB 0x8009

/* ---- buffers / vertex arrays ---- */
void glGenBuffers(GLsizei n, GLuint *ids);
void glDeleteBuffers(GLsizei n, const GLuint *ids);
void glBindBuffer(GLenum target, GLuint id);
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data);
void glGenVertexArrays(GLsizei n, GLuint *ids);
void glDeleteVertexArrays(GLsizei n, const GLuint *ids);
void glBindVertexArray(GLuint id);
void glEnableVertexAttribArray(GLuint index);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);

/* ---- shaders / programs (names only) ---- */
GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint s, GLsizei n, const GLchar *const *src, const GLint *len);
void glCompileShader(GLuint s);
void glGetShaderiv(GLuint s, GLenum pname, GLint *out);
void glGetShaderInfoLog(GLuint s, GLsizei max, GLsizei *len, GLchar *log);
void glDeleteShader(GLuint s);
GLuint glCreateProgram(void);
void glAttachShader(GLuint p, GLuint s);
void glBindAttribLocation(GLuint p, GLuint index, const GLchar *name);
void glLinkProgram(GLuint p);
void glGetProgramiv(GLuint p, GLenum pname, GLint *out);
void glGetProgramInfoLog(GLuint p, GLsizei max, GLsizei *len, GLchar *log);
void glUseProgram(GLuint p);
void glDeleteProgram(GLuint p);

/* ---- uniforms (one table; r_mesh.c has one program) ---- */
GLint glGetUniformLocation(GLuint p, const GLchar *name);
void glUniform1i(GLint loc, GLint v);
void glUniform1f(GLint loc, GLfloat v);
void glUniform2f(GLint loc, GLfloat a, GLfloat b);
void glUniform2fv(GLint loc, GLsizei n, const GLfloat *v);
void glUniform3fv(GLint loc, GLsizei n, const GLfloat *v);
void glUniform4fv(GLint loc, GLsizei n, const GLfloat *v);
void glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean transpose, const GLfloat *v);

/* ---- textures (ids of the 3DS renderer) ---- */
void glActiveTexture(GLenum unit);
void glBindTexture(GLenum target, GLuint id);
void glTexParameteri(GLenum target, GLenum pname, GLint v);

/* ---- queries (debug output in r_mesh.c only) ---- */
void glGetIntegerv(GLenum pname, GLint *out);
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *out);
static inline GLenum glGetError(void) { return GL_NO_ERROR; }

/* port/platform/gl.h helpers */
const char *gl_shader_header(void);
void gl_clear_depth(float d);

/* ---- what glDrawElements hands to the 3DS renderer ---- */
#define GLS_MAX_UNIFORMS 32
#define GLS_MAX_ATTRIBS 4
typedef struct GlsUniform {
    char name[24];
    float f[3 * 4];      /* up to 3 vec4s (u_ldir[3] / u_lcol[3]) or one mat4 below */
    float m[16];
    int i;
    int count;           /* vectors written */
} GlsUniform;
typedef struct GlsAttrib {
    int enabled, size;
    GLenum type;
    GLsizei stride;
    uintptr_t offset;
} GlsAttrib;
typedef struct GlsDraw {
    const uint8_t *vbuf;       /* bound ARRAY_BUFFER contents */
    size_t vsize;
    const GlsAttrib *attr;     /* GLS_MAX_ATTRIBS, by attribute index */
    const void *indices;       /* ELEMENT_ARRAY_BUFFER contents + offset */
    size_t isize;              /* bytes from there to the end of that buffer */
    GLenum itype;
    GLsizei count;
    GLuint tex[2];             /* bound textures, units 0 and 1 */
    int tex_linear, tex_clamp; /* last glTexParameteri on unit 0 */
} GlsDraw;
/* the uniform `name` as stored (NULL if never set) */
const GlsUniform *gls_uniform(const char *name);
/* its slot (stable once glGetUniformLocation made it; NULL if it has none) */
const GlsUniform *gls_uniform_slot(const char *name);
/* the GPU never reads the buffers (the renderer's CPU mesh path reads them
 * at draw time): writes go in place, without copies or cache flushes */
void gls_set_cpu_only(int on);
/* r_c3d.c: the mesh jobs that read the buffers are done */
void r3ds_mesh_sync(void);
void r3ds_mesh_sync_buf(const void *p, size_t n); /* only if a job reads [p, p + n) */
/* r_c3d.c: draw one mesh */
void r3ds_mesh_draw(const GlsDraw *d);
/* r3ds_frame_start: free the buffers retired before this frame */
void gls_frame_start(void);
#endif
