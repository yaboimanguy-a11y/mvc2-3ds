/* GL shim for r_mesh.c on the Nintendo 3DS (see gl_shim.h). */
#include "gl_shim.h"
#include <3ds.h>
#include <stdlib.h>
#include <string.h>

uint32_t r3ds_frame_number(void);

/* ---- buffers ----------------------------------------------------------
 * In linear memory: the GPU mesh paths read vertices and indices from them.
 * For those, a buffer drawn in a frame the GPU may still draw is not changed
 * in place: a write gets a new copy and the old one is freed once those
 * frames are done (gls_frame_start).  The CPU mesh path (the default,
 * gls_set_cpu_only) reads them only while the draw is recorded, so writes
 * go in place with no copy and no cache flush. */
#define MAX_BUF 4096
#define MAX_RETIRED 1024
typedef struct Buf { uint8_t *data; size_t size; int used; uint32_t drawn_frame; } Buf;
static struct { void *p; uint32_t frame; } retired[MAX_RETIRED];
static int nretired;
static int cpu_only;

static void flush_all_buffers(void);
void gls_set_cpu_only(int on)
{
    /* back to the GPU paths: what was written in place reaches the GPU */
    if (cpu_only && !on) flush_all_buffers();
    cpu_only = on != 0;
}

/* drawn in a frame the GPU may still draw (this one or, deferred, the last) */
static int in_flight(uint32_t drawn)
{
    if (cpu_only || drawn == 0xFFFFFFFFu) return 0;
    return drawn + 1 >= r3ds_frame_number();
}

static void lin_free(void *p, int drawn_now)
{
    if (!p) return;
    if (drawn_now && nretired < MAX_RETIRED) {
        retired[nretired].p = p;
        retired[nretired].frame = r3ds_frame_number();
        nretired++;
        return;
    }
    if (!drawn_now) linearFree(p);
    /* (retired list full: leak rather than free under the GPU) */
}

void gls_frame_start(void)
{
    uint32_t now = r3ds_frame_number();
    int k = 0;
    for (int i = 0; i < nretired; i++) {
        if (retired[i].frame + 2 <= now) linearFree(retired[i].p);
        else retired[k++] = retired[i];
    }
    nretired = k;
}
static Buf bufs[MAX_BUF];

static void flush_all_buffers(void)
{
    for (int i = 1; i < MAX_BUF; i++)
        if (bufs[i].used && bufs[i].data) GSPGPU_FlushDataCache(bufs[i].data, bufs[i].size);
}
static GLuint bound_array, bound_element;

void glGenBuffers(GLsizei n, GLuint *ids)
{
    GLsizei k = 0;
    for (GLuint i = 1; i < MAX_BUF && k < n; i++)
        if (!bufs[i].used) {
            bufs[i].used = 1;
            bufs[i].data = NULL;
            bufs[i].size = 0;
            bufs[i].drawn_frame = 0xFFFFFFFFu; /* never drawn (frame 0 is the boot) */
            ids[k++] = i;
        }
    while (k < n) ids[k++] = 0;
}

void glDeleteBuffers(GLsizei n, const GLuint *ids)
{
    /* (a mesh job may still read them: r3ds_mesh_sync_buf below) */
    for (GLsizei k = 0; k < n; k++) {
        GLuint i = ids[k];
        if (i == 0 || i >= MAX_BUF || !bufs[i].used) continue;
        r3ds_mesh_sync_buf(bufs[i].data, bufs[i].size);
        lin_free(bufs[i].data, in_flight(bufs[i].drawn_frame));
        memset(&bufs[i], 0, sizeof bufs[i]);
        if (bound_array == i) bound_array = 0;
        if (bound_element == i) bound_element = 0;
    }
}

static GLuint bound_vao;
static void vao_set_element(GLuint id);

void glBindBuffer(GLenum target, GLuint id)
{
    if (target == GL_ARRAY_BUFFER) bound_array = id;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        bound_element = id;
        vao_set_element(id); /* the element binding is VAO state */
    }
}

static Buf *bound(GLenum target)
{
    GLuint i = target == GL_ARRAY_BUFFER ? bound_array : target == GL_ELEMENT_ARRAY_BUFFER ? bound_element : 0;
    return (i && i < MAX_BUF && bufs[i].used) ? &bufs[i] : NULL;
}

void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    Buf *b = bound(target);
    (void)usage;
    if (b) r3ds_mesh_sync_buf(b->data, b->size);
    if (!b || size < 0) return;
    int drawn_now = in_flight(b->drawn_frame);
    if ((size_t)size != b->size || drawn_now) {
        lin_free(b->data, drawn_now);
        b->data = size ? (uint8_t *)linearAlloc((size_t)size) : NULL;
        b->size = b->data ? (size_t)size : 0;
        b->drawn_frame = 0xFFFFFFFFu;
    }
    if (data && b->data) memcpy(b->data, data, b->size);
    else if (b->data) memset(b->data, 0, b->size);
    if (b->data && !cpu_only) GSPGPU_FlushDataCache(b->data, b->size);
}

void glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data)
{
    Buf *b = bound(target);
    if (!b || off < 0 || size < 0 || (size_t)(off + size) > b->size) return;
    r3ds_mesh_sync_buf(b->data, b->size);
    if (in_flight(b->drawn_frame)) {
        /* copy on write: the draws in flight keep the old contents */
        uint8_t *n = (uint8_t *)linearAlloc(b->size);
        if (!n) return;
        memcpy(n, b->data, b->size);
        lin_free(b->data, 1);
        b->data = n;
        b->drawn_frame = 0xFFFFFFFFu;
    }
    memcpy(b->data + off, data, (size_t)size);
    if (!cpu_only) GSPGPU_FlushDataCache(b->data + off, (u32)size);
}

/* ---- vertex arrays: the element buffer and the attributes are VAO state -- */
#define MAX_VAO 4096
typedef struct Vao { int used; GLuint element; GLuint array[GLS_MAX_ATTRIBS]; GlsAttrib attr[GLS_MAX_ATTRIBS]; } Vao;
static Vao vaos[MAX_VAO];

static void vao_set_element(GLuint id)
{
    if (bound_vao && bound_vao < MAX_VAO) vaos[bound_vao].element = id;
}

void glGenVertexArrays(GLsizei n, GLuint *ids)
{
    GLsizei k = 0;
    for (GLuint i = 1; i < MAX_VAO && k < n; i++)
        if (!vaos[i].used) {
            memset(&vaos[i], 0, sizeof vaos[i]);
            vaos[i].used = 1;
            ids[k++] = i;
        }
    while (k < n) ids[k++] = 0;
}

void glDeleteVertexArrays(GLsizei n, const GLuint *ids)
{
    for (GLsizei k = 0; k < n; k++)
        if (ids[k] && ids[k] < MAX_VAO) {
            memset(&vaos[ids[k]], 0, sizeof vaos[ids[k]]);
            if (bound_vao == ids[k]) bound_vao = 0;
        }
}

void glBindVertexArray(GLuint id)
{
    bound_vao = (id < MAX_VAO && vaos[id].used) ? id : 0;
    bound_element = bound_vao ? vaos[bound_vao].element : 0;
}

void glEnableVertexAttribArray(GLuint index)
{
    if (bound_vao && index < GLS_MAX_ATTRIBS) vaos[bound_vao].attr[index].enabled = 1;
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr)
{
    (void)norm;
    if (!bound_vao || index >= GLS_MAX_ATTRIBS) return;
    GlsAttrib *a = &vaos[bound_vao].attr[index];
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->offset = (uintptr_t)ptr;
    vaos[bound_vao].array[index] = bound_array;
}

/* ---- textures -------------------------------------------------------- */
static GLuint tex_unit[2];
static int active_unit, tex_linear, tex_clamp;

void glActiveTexture(GLenum unit) { active_unit = unit == GL_TEXTURE1 ? 1 : 0; }
void glBindTexture(GLenum target, GLuint id) { (void)target; tex_unit[active_unit] = id; }
void glTexParameteri(GLenum target, GLenum pname, GLint v)
{
    (void)target;
    if (active_unit != 0) return;
    if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) tex_linear = v == GL_LINEAR;
    else if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) tex_clamp = v == GL_CLAMP_TO_EDGE;
}

/* ---- shaders / programs: names only ----------------------------------- */
static GLuint next_name = 1;
GLuint glCreateShader(GLenum type) { (void)type; return next_name++; }
void glShaderSource(GLuint s, GLsizei n, const GLchar *const *src, const GLint *len) { (void)s; (void)n; (void)src; (void)len; }
void glCompileShader(GLuint s) { (void)s; }
void glGetShaderiv(GLuint s, GLenum pname, GLint *out) { (void)s; (void)pname; *out = GL_TRUE; }
void glGetShaderInfoLog(GLuint s, GLsizei max, GLsizei *len, GLchar *log) { (void)s; if (len) *len = 0; if (max > 0) log[0] = 0; }
void glDeleteShader(GLuint s) { (void)s; }
GLuint glCreateProgram(void) { return next_name++; }
void glAttachShader(GLuint p, GLuint s) { (void)p; (void)s; }
void glBindAttribLocation(GLuint p, GLuint index, const GLchar *name) { (void)p; (void)index; (void)name; }
void glLinkProgram(GLuint p) { (void)p; }
void glGetProgramiv(GLuint p, GLenum pname, GLint *out) { (void)p; (void)pname; *out = GL_TRUE; }
void glGetProgramInfoLog(GLuint p, GLsizei max, GLsizei *len, GLchar *log) { (void)p; if (len) *len = 0; if (max > 0) log[0] = 0; }
void glUseProgram(GLuint p) { (void)p; }
void glDeleteProgram(GLuint p) { (void)p; }

/* ---- uniforms ------------------------------------------------------- */
static GlsUniform unis[GLS_MAX_UNIFORMS];
static int nunis;

GLint glGetUniformLocation(GLuint p, const GLchar *name)
{
    (void)p;
    for (int i = 0; i < nunis; i++)
        if (!strcmp(unis[i].name, name)) return i;
    if (nunis == GLS_MAX_UNIFORMS || strlen(name) >= sizeof unis[0].name) return -1;
    memset(&unis[nunis], 0, sizeof unis[nunis]);
    strcpy(unis[nunis].name, name);
    return nunis++;
}

const GlsUniform *gls_uniform_slot(const char *name)
{
    for (int i = 0; i < nunis; i++)
        if (!strcmp(unis[i].name, name)) return &unis[i];
    return NULL;
}

const GlsUniform *gls_uniform(const char *name)
{
    for (int i = 0; i < nunis; i++)
        if (!strcmp(unis[i].name, name)) return unis[i].count ? &unis[i] : NULL;
    return NULL;
}

static GlsUniform *uni(GLint loc) { return (loc >= 0 && loc < nunis) ? &unis[loc] : NULL; }

static void set_vecs(GLint loc, GLsizei n, int comps, const GLfloat *v)
{
    GlsUniform *u = uni(loc);
    if (!u) return;
    if (n > 3) n = 3;
    for (GLsizei k = 0; k < n; k++)
        for (int c = 0; c < 4; c++) u->f[4 * k + c] = c < comps ? v[comps * k + c] : 0.f;
    u->count = n;
}

void glUniform1i(GLint loc, GLint v) { GlsUniform *u = uni(loc); if (u) { u->i = v; u->f[0] = (float)v; u->count = 1; } }
void glUniform1f(GLint loc, GLfloat v) { set_vecs(loc, 1, 1, &v); }
void glUniform2f(GLint loc, GLfloat a, GLfloat b) { GLfloat v[2] = { a, b }; set_vecs(loc, 1, 2, v); }
void glUniform2fv(GLint loc, GLsizei n, const GLfloat *v) { set_vecs(loc, n, 2, v); }
void glUniform3fv(GLint loc, GLsizei n, const GLfloat *v) { set_vecs(loc, n, 3, v); }
void glUniform4fv(GLint loc, GLsizei n, const GLfloat *v) { set_vecs(loc, n, 4, v); }
void glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean transpose, const GLfloat *v)
{
    GlsUniform *u = uni(loc);
    (void)n;
    if (!u) return;
    if (transpose) {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) u->m[4 * c + r] = v[4 * r + c];
    } else {
        memcpy(u->m, v, sizeof u->m);
    }
    u->count = 1;
}

/* ---- draw ------------------------------------------------------------- */
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    if (mode != GL_TRIANGLES || !bound_vao || count <= 0) return;
    Vao *va = &vaos[bound_vao];
    GLuint vb = va->array[0];
    Buf *eb = (bound_element && bound_element < MAX_BUF && bufs[bound_element].used) ? &bufs[bound_element] : NULL;
    if (!vb || vb >= MAX_BUF || !bufs[vb].used || !eb || !eb->data) return;
    GlsDraw d;
    if (!bufs[vb].data || !bufs[vb].size) return;
    d.vbuf = bufs[vb].data;
    d.vsize = bufs[vb].size;
    d.isize = eb->size - (size_t)(uintptr_t)indices;
    if ((uintptr_t)indices >= eb->size) return;
    d.attr = va->attr;
    d.indices = eb->data + (uintptr_t)indices;
    d.itype = type;
    d.count = count;
    d.tex[0] = tex_unit[0];
    d.tex[1] = tex_unit[1];
    d.tex_linear = tex_linear;
    d.tex_clamp = tex_clamp;
    bufs[vb].drawn_frame = eb->drawn_frame = r3ds_frame_number();
    r3ds_mesh_draw(&d);
}

/* ---- queries -------------------------------------------------------- */
void glGetIntegerv(GLenum pname, GLint *out)
{
    out[0] = 0;
    if (pname == GL_SCISSOR_BOX) out[1] = out[2] = out[3] = 0;
}
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *out)
{
    (void)x; (void)y; (void)fmt; (void)type;
    memset(out, 0, (size_t)w * (size_t)h * 4);
}

const char *gl_shader_header(void) { return ""; }
void gl_clear_depth(float d) { (void)d; }
