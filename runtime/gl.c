// Hand-written OpenGL shims: pointer translation, buffer mapping, strings, CGL/AGL queries,
// and the renderer's Objective-C bridge (Present / VRAM).
#define GL_SILENCE_DEPRECATION
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/OpenGL.h>

#define RET(v) (c->eax = (uint32_t)(v))

static int gl_tracing = -1;
void gl_trace(const char *name) {
    if (gl_tracing < 0) gl_tracing = getenv("JP_TRACE_GL") != NULL;
    if (gl_tracing) {
        GLenum e = glGetError();
        fprintf(stderr, "[gl] %s%s\n", name, e ? " (previous call raised an error)" : "");
    }
}

// JP_TRACE_GLARGS=glTexImage2D,glTexParameteri,...: log selected calls with raw argument words
void gl_trace_args(const char *name, CPU *c, int nslots) {
    static const char *sel = (const char *)-1;
    if (sel == (const char *)-1) sel = getenv("JP_TRACE_GLARGS");
    if (!sel || !strstr(sel, name)) return;
    static int budget = 4000; if (budget-- <= 0) return;
    fprintf(stderr, "[glargs] %s(", name);
    for (int i = 0; i < nslots; i++) fprintf(stderr, "%s%#x", i ? ", " : "", ARG(i));
    fprintf(stderr, ")\n");
}

// gl*Pointer: with a buffer object bound the "pointer" is a byte offset into it
void *gl_ptr_array(uint32_t g) {
    GLint b = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &b);
    return b ? (void *)(uintptr_t)g : (g ? G2H(g) : NULL);
}
void *gl_ptr_element(uint32_t g) {
    GLint b = 0; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &b);
    return b ? (void *)(uintptr_t)g : (g ? G2H(g) : NULL);
}

// glGetString: host strings live outside guest memory; copy each into a guest buffer once
void I__glGetString(CPU *c) {
    static uint32_t cache[16]; static GLenum keys[16];
    GLenum e = ARG(0);
    for (int i = 0; i < 16; i++) if (keys[i] == e && cache[i]) { RET(cache[i]); return; }
    const char *s = (const char *)glGetString(e);
    if (!s) { RET(0); return; }
    for (int i = 0; i < 16; i++) if (!keys[i]) { keys[i] = e; cache[i] = gstrdup(s); RET(cache[i]); return; }
    RET(gstrdup(s));
}

// glShaderSource(shader, count, const char **strings, const GLint *lengths): array of guest pointers
void I__glShaderSource(CPU *c) {
    GLsizei n = (GLsizei)ARG(1);
    const char **strs = calloc(n ? n : 1, sizeof *strs);
    for (GLsizei i = 0; i < n; i++) strs[i] = (const char *)G2H(rd32(ARG(2) + 4 * i));
    glShaderSource(ARG(0), n, strs, ARG(3) ? (const GLint *)G2H(ARG(3)) : NULL);
    free(strs);
}

// glMapBuffer: hand the guest a shadow copy in guest memory; write it back on unmap
typedef struct { GLenum target; GLuint buf; uint32_t shadow; GLint size; GLenum access; } Mapping;
static Mapping maps[64];
static GLenum binding_query(GLenum target) {
    return target == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING :
           target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING : 0;
}
void I__glMapBuffer(CPU *c) {
    GLenum target = ARG(0), access = ARG(1);
    GLint buf = 0, size = 0;
    glGetIntegerv(binding_query(target), &buf);
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    if (!buf || size <= 0) { RET(0); return; }
    for (int i = 0; i < 64; i++) if (!maps[i].shadow) {
        Mapping *m = &maps[i];
        m->target = target; m->buf = (GLuint)buf; m->size = size; m->access = access;
        m->shadow = gmalloc((uint32_t)size);
        if (access != GL_WRITE_ONLY) glGetBufferSubData(target, 0, size, M + m->shadow);
        RET(m->shadow); return;
    }
    fprintf(stderr, "[gl] too many mapped buffers\n"); abort();
}
void I__glUnmapBuffer(CPU *c) {
    GLenum target = ARG(0);
    GLint buf = 0; glGetIntegerv(binding_query(target), &buf);
    for (int i = 0; i < 64; i++) if (maps[i].shadow && maps[i].buf == (GLuint)buf && maps[i].target == target) {
        if (maps[i].access != GL_READ_ONLY) glBufferSubData(target, 0, maps[i].size, M + maps[i].shadow);
        gfree(maps[i].shadow); maps[i].shadow = 0;
        RET(GL_TRUE); return;
    }
    RET(GL_FALSE);
}

// CGL: the guest only ever sees "the current context" as handle 1
void I__CGLGetCurrentContext(CPU *c) { RET(CGLGetCurrentContext() ? 1 : 0); }
void I__CGLGetParameter(CPU *c) {
    GLint v[4] = {0};
    CGLError e = CGLGetParameter(CGLGetCurrentContext(), (CGLContextParameter)ARG(1), v);
    if (ARG(2)) wr32(ARG(2), (uint32_t)v[0]);
    RET(e);
}
void I__CGLSetParameter(CPU *c) {
    GLint v = ARG(2) ? (GLint)rd32(ARG(2)) : 0;
    RET(CGLSetParameter(CGLGetCurrentContext(), (CGLContextParameter)ARG(1), &v));
}

// Renderer Objective-C bridge (HLE)
extern void host_present(void);
void H_RenderDeviceGL_ObjC_Present(CPU *c) { (void)c; host_present(); }
void H_RenderDeviceGL_ObjC_GetTotalVram(CPU *c) {
    GLint mem = 0;
    CGLRendererInfoObj info; GLint n = 0;
    if (CGLQueryRendererInfo(0xFFFFFFFF, &info, &n) == kCGLNoError) {
        for (GLint i = 0; i < n; i++) {
            GLint v = 0;
            if (CGLDescribeRenderer(info, i, kCGLRPVideoMemoryMegabytes, &v) == kCGLNoError && v > mem) mem = v;
        }
        CGLDestroyRendererInfo(info);
    }
    if (mem <= 0 || mem > 2047) mem = 2047;          // 32-bit engine: report at most ~2 GiB
    RET((uint32_t)mem * 1024u * 1024u);
}

// Window-surface mapping: the engine renders at its own resolution; on the default framebuffer
// its viewports/scissors are mapped onto the real drawable with a uniform scale and centering
// offset (letterbox/pillarbox), recomputed by the host every frame.
float gl_win_scale = 1, gl_win_off_x = 0, gl_win_off_y = 0;
static int on_window_fb(void) { GLint fb = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fb); return fb == 0; }
#define MAPX(v) ((GLint)(gl_win_off_x + (v) * gl_win_scale))
#define MAPY(v) ((GLint)(gl_win_off_y + (v) * gl_win_scale))
#define MAPS(v) ((GLsizei)((v) * gl_win_scale + 0.5f))
void I__glViewport(CPU *c) {
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1); GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    if (on_window_fb()) glViewport(MAPX(x), MAPY(y), MAPS(w), MAPS(h)); else glViewport(x, y, w, h);
}
void I__glScissor(CPU *c) {
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1); GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    if (on_window_fb()) glScissor(MAPX(x), MAPY(y), MAPS(w), MAPS(h)); else glScissor(x, y, w, h);
}

// glGetProgramivARB(target, pname, int *out)
// Apple's Metal-backed GL reports some NATIVE_* program counters as 0 even for programs it runs fine.
// The engine treats "instructions > 0 but native instructions == 0" as "not supported in hardware",
// deletes the program and drops to a lower-quality effect (e.g. blocky shadows). Report native
// counters as the plain counters when the driver says the program is under native limits.
void I__glGetProgramivARB(CPU *c) {
    GLenum target = ARG(0), pname = ARG(1); GLint v = 0;
    glGetProgramivARB(target, pname, &v);
    static const GLenum native_to_plain[][2] = {
        { GL_PROGRAM_NATIVE_INSTRUCTIONS_ARB, GL_PROGRAM_INSTRUCTIONS_ARB },
        { GL_PROGRAM_NATIVE_TEMPORARIES_ARB, GL_PROGRAM_TEMPORARIES_ARB },
        { GL_PROGRAM_NATIVE_PARAMETERS_ARB, GL_PROGRAM_PARAMETERS_ARB },
        { GL_PROGRAM_NATIVE_ATTRIBS_ARB, GL_PROGRAM_ATTRIBS_ARB },
        { GL_PROGRAM_NATIVE_ALU_INSTRUCTIONS_ARB, GL_PROGRAM_ALU_INSTRUCTIONS_ARB },
        { GL_PROGRAM_NATIVE_TEX_INSTRUCTIONS_ARB, GL_PROGRAM_TEX_INSTRUCTIONS_ARB },
        { GL_PROGRAM_NATIVE_TEX_INDIRECTIONS_ARB, GL_PROGRAM_TEX_INDIRECTIONS_ARB },
    };
    int fixed = 0;
    for (size_t i = 0; i < sizeof native_to_plain / sizeof native_to_plain[0]; i++)
        if (pname == native_to_plain[i][0] && v == 0) {
            GLint under = 0, plain = 0;
            glGetProgramivARB(target, GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB, &under);
            glGetProgramivARB(target, native_to_plain[i][1], &plain);
            if (under && plain > 0) { v = plain; fixed = 1; }
        }
    if (getenv("JP_TRACE_PROGIV")) fprintf(stderr, "[gl] glGetProgramivARB(%#x, %#x) = %d%s\n", target, pname, v, fixed ? " (native count substituted)" : "");
    if (ARG(2)) wr32(ARG(2), (uint32_t)v);
}
