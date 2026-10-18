/* Fixed-function pipeline emulation: one GLSL program per distinct
 * combination of vertex format, lighting setup, texture stage ops and fog.
 *
 * Matrices are uploaded as stored by D3D (row-major, row vectors); GLSL
 * reads them column-major, so `M * v` computes D3D's `v * M`.
 */
#include "d3d8_internal.h"
#include <math.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ key */
typedef struct {
    uint8_t cop, ca1, ca2, ca0, aop, aa1, aa2, aa0, result;
    uint8_t tci, texgen, ttff, projected, kind;   /* kind: 0 none, 1 2D, 2 cube */
} StageKey;

typedef struct {
    uint8_t rhw, normal, diffuse, specular, blend, tc_size[8], ntc;
    uint8_t lighting, light_type[8], localviewer, normalize, color_vertex;
    uint8_t src_diff, src_amb, src_spec, src_emis, spec_enable;
    uint8_t fog, fog_table, fog_range;   /* fog: 0 none, 1 exp, 2 exp2, 3 linear */
    uint8_t alpha_func, clip_mask, flat;
    StageKey st[MAX_STAGES];
} Key;

enum {
    U_WORLD, U_VIEW, U_PROJ, U_TEXM, U_RHW, U_HALFPIX, U_MDIFF, U_MAMB, U_MSPEC, U_MEMIS, U_MPOW,
    U_AMBIENT, U_LDIFF, U_LSPEC, U_LAMB, U_LPOS, U_LDIR, U_LATT, U_LSPOT, U_FOG, U_FOGCOLOR,
    U_CLIP, U_TFACTOR, U_AREF, U_SAMPLER, U_COUNT
};

static const char *s_uniform_names[U_COUNT] = {
    "u_world", "u_view", "u_proj", "u_texm", "u_rhw", "u_halfpix", "u_mdiff", "u_mamb", "u_mspec",
    "u_memis", "u_mpow", "u_ambient", "u_ldiff", "u_lspec", "u_lamb", "u_lpos", "u_ldir", "u_latt",
    "u_lspot", "u_fog", "u_fogcolor", "u_clip", "u_tfactor", "u_aref", "u_tex",
};

typedef struct {
    Key key;
    uint64_t hash;
    GLuint prog;
    GLint u[U_COUNT];
} Program;

#define MAX_PROGRAMS 4096
static Program *s_progs[MAX_PROGRAMS];
static int s_nprogs;

static uint64_t hash_key(const Key *k) {
    const uint8_t *p = (const uint8_t *)k;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof *k; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* ------------------------------------------------------------ source gen */
typedef struct {
    char *buf;
    size_t len, cap;
} Str;

static void sp(Str *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sp(Str *s, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
        va_end(ap);
        if ((size_t)n < s->cap - s->len) {
            s->len += (size_t)n;
            return;
        }
        s->cap = s->cap ? s->cap * 2 : 4096;
        s->buf = realloc(s->buf, s->cap);
    }
}

static const char *s_header =
    "#version 330 core\n"
    "precision highp float;\n";

static void gen_vs(Str *s, const Key *k) {
    sp(s, "%s", s_header);
    sp(s, "layout(location=0) in vec4 a_pos;\n"
          "layout(location=1) in vec4 a_weight;\n"
          "layout(location=2) in vec3 a_normal;\n"
          "layout(location=4) in vec4 a_diffuse;\n"
          "layout(location=5) in vec4 a_specular;\n");
    for (int i = 0; i < 8; i++)
        sp(s, "layout(location=%d) in vec4 a_tc%d;\n", 6 + i, i);
    sp(s, "uniform mat4 u_world[4], u_view, u_proj, u_texm[8];\n"
          "uniform vec4 u_rhw;\n"           /* vp x, y, 2/w, 2/h */
          "uniform vec4 u_halfpix;\n"       /* 1/w, 1/h, zscale, zoffset (rhw) */
          "uniform vec4 u_mdiff, u_mamb, u_mspec, u_memis, u_ambient;\n"
          "uniform float u_mpow;\n"
          "uniform vec4 u_ldiff[8], u_lspec[8], u_lamb[8], u_lpos[8], u_ldir[8], u_latt[8], u_lspot[8];\n"
          "uniform vec4 u_fog;\n"
          "uniform vec4 u_clip[6];\n");
    const char *q = k->flat ? "flat " : "";
    sp(s, "%sout vec4 v_diff;\n%sout vec4 v_spec;\nout vec4 v_tc[8];\nout float v_fog;\nout float v_fogz;\n"
          "out vec4 v_clip0;\nout vec2 v_clip1;\n", q, q);
    sp(s, "void main() {\n");
    /* Without normals D3D lights only ambient/emissive. */
    sp(s, "  vec4 pw = vec4(0.0), pv = vec4(0.0);\n  vec3 n = vec3(0.0%s);\n", k->normal || !k->lighting ? ", 0.0, 1.0" : "");
    if (k->rhw) {
        sp(s, "  float w = a_pos.w != 0.0 ? 1.0 / a_pos.w : 1.0;\n"
              "  vec4 p = vec4((a_pos.x - u_rhw.x) * u_rhw.z - 1.0, 1.0 - (a_pos.y - u_rhw.y) * u_rhw.w,\n"
              "                a_pos.z * u_halfpix.z + u_halfpix.w, 1.0);\n"
              "  gl_Position = p * w;\n"
              "  v_fogz = w;\n");
    } else {
        sp(s, "  vec4 p = vec4(a_pos.xyz, 1.0);\n");
        if (k->blend) {
            /* Non-indexed vertex blending over world matrices 0..blend. */
            sp(s, "  float wsum = 0.0;\n  vec3 nw = vec3(0.0);\n");
            for (int i = 0; i < k->blend; i++) {
                sp(s, "  pw += a_weight[%d] * (u_world[%d] * p);\n", i, i);
                if (k->normal)
                    sp(s, "  nw += a_weight[%d] * (mat3(u_world[%d]) * a_normal);\n", i, i);
                sp(s, "  wsum += a_weight[%d];\n", i);
            }
            sp(s, "  pw += (1.0 - wsum) * (u_world[%d] * p);\n", k->blend);
            if (k->normal)
                sp(s, "  nw += (1.0 - wsum) * (mat3(u_world[%d]) * a_normal);\n"
                      "  n = mat3(u_view) * nw;\n", k->blend);
        } else {
            sp(s, "  pw = u_world[0] * p;\n");
            if (k->normal)
                sp(s, "  n = transpose(inverse(mat3(u_view) * mat3(u_world[0]))) * a_normal;\n");
        }
        sp(s, "  pv = u_view * pw;\n"
              "  gl_Position = u_proj * pv;\n"
              "  gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n"
              "  v_fogz = %s;\n", k->fog_range ? "length(pv.xyz)" : "abs(pv.z)");
        if (k->normal)
            sp(s, "  if (dot(n, n) > 0.0) n = normalize(n);\n");
    }
    /* D3D: +y is up in clip space and pixel centres sit on integers. */
    sp(s, "  gl_Position.y = -gl_Position.y;\n"
          "  gl_Position.xy += u_halfpix.xy * gl_Position.w;\n");
    /* Colors */
    sp(s, "  vec4 vdiff = %s;\n  vec4 vspec = %s;\n", k->diffuse ? "a_diffuse" : "vec4(1.0)",
       k->specular ? "a_specular" : "vec4(0.0)");
    if (k->lighting && !k->rhw) {
        const char *src[3] = {"", "vdiff", "vspec"};
#define MSRC(v, def) (k->color_vertex && (v) && ((v) == 1 ? k->diffuse : k->specular) ? src[v] : def)
        sp(s, "  vec4 md = %s, ma = %s, ms = %s, me = %s;\n", MSRC(k->src_diff, "u_mdiff"),
           MSRC(k->src_amb, "u_mamb"), MSRC(k->src_spec, "u_mspec"), MSRC(k->src_emis, "u_memis"));
#undef MSRC
        sp(s, "  vec3 amb = u_ambient.rgb, dif = vec3(0.0), spe = vec3(0.0);\n"
              "  vec3 V = %s;\n", k->localviewer ? "-normalize(pv.xyz)" : "vec3(0.0, 0.0, -1.0)");
        for (int i = 0; i < 8; i++) {
            int t = k->light_type[i];
            if (!t)
                continue;
            sp(s, "  {\n    vec3 L; float att = 1.0;\n");
            if (t == 3) {
                sp(s, "    L = -u_ldir[%d].xyz;\n", i);
            } else {
                sp(s, "    vec3 d = u_lpos[%d].xyz - pv.xyz; float dist = length(d);\n"
                      "    L = d / max(dist, 1e-6);\n"
                      "    att = dist > u_latt[%d].w ? 0.0 : 1.0 / max(u_latt[%d].x + u_latt[%d].y * dist + u_latt[%d].z * dist * dist, 1e-6);\n",
                   i, i, i, i, i);
                if (t == 2)
                    sp(s, "    float rho = dot(normalize(u_ldir[%d].xyz), -L);\n"
                          "    att *= rho > u_lspot[%d].x ? 1.0 : rho <= u_lspot[%d].y ? 0.0 :\n"
                          "           pow(clamp((rho - u_lspot[%d].y) / max(u_lspot[%d].x - u_lspot[%d].y, 1e-6), 0.0, 1.0), u_lspot[%d].z);\n",
                       i, i, i, i, i, i, i);
            }
            sp(s, "    amb += att * u_lamb[%d].rgb;\n"
                  "    float ndl = max(dot(n, L), 0.0);\n"
                  "    dif += att * ndl * u_ldiff[%d].rgb;\n", i, i);
            if (k->spec_enable)
                sp(s, "    if (ndl > 0.0 && u_mpow > 0.0) spe += att * pow(max(dot(n, normalize(L + V)), 0.0), u_mpow) * u_lspec[%d].rgb;\n",
                   i);
            sp(s, "  }\n");
        }
        sp(s, "  v_diff = vec4(clamp(me.rgb + ma.rgb * amb + md.rgb * dif, 0.0, 1.0), clamp(md.a, 0.0, 1.0));\n"
              "  v_spec = vec4(clamp(ms.rgb * spe, 0.0, 1.0), vspec.a);\n");
    } else {
        sp(s, "  v_diff = vdiff;\n  v_spec = vspec;\n");
    }
    /* Vertex fog */
    if (k->fog && !k->fog_table) {
        if (k->rhw)
            sp(s, "  v_fog = vspec.a;\n");
        else if (k->fog == 3)
            sp(s, "  v_fog = clamp((u_fog.y - v_fogz) / max(u_fog.y - u_fog.x, 1e-6), 0.0, 1.0);\n");
        else if (k->fog == 1)
            sp(s, "  v_fog = clamp(exp(-u_fog.z * v_fogz), 0.0, 1.0);\n");
        else
            sp(s, "  v_fog = clamp(exp(-(u_fog.z * v_fogz) * (u_fog.z * v_fogz)), 0.0, 1.0);\n");
    } else {
        sp(s, "  v_fog = 1.0;\n");
    }
    /* Texture coordinates per stage */
    for (int i = 0; i < 8; i++) {
        const StageKey *t = &k->st[i];
        if (!t->kind) {
            sp(s, "  v_tc[%d] = vec4(0.0);\n", i);
            continue;
        }
        if (t->texgen == 1)
            sp(s, "  vec4 tc%d = vec4(n, 1.0);\n", i);
        else if (t->texgen == 2)
            sp(s, "  vec4 tc%d = vec4(pv.xyz, 1.0);\n", i);
        else if (t->texgen == 3)
            sp(s, "  vec4 tc%d = vec4(reflect(normalize(pv.xyz), n), 1.0);\n", i);
        else {
            int sz = t->tci < k->ntc ? k->tc_size[t->tci] : 2;
            int ci = t->tci < 8 ? t->tci : 0;
            if (sz == 1)
                sp(s, "  vec4 tc%d = vec4(a_tc%d.x, 1.0, 0.0, 0.0);\n", i, ci);
            else if (sz == 2)
                sp(s, "  vec4 tc%d = vec4(a_tc%d.xy, 1.0, 0.0);\n", i, ci);
            else if (sz == 3)
                sp(s, "  vec4 tc%d = vec4(a_tc%d.xyz, 1.0);\n", i, ci);
            else
                sp(s, "  vec4 tc%d = a_tc%d;\n", i, ci);
        }
        if (t->ttff)
            sp(s, "  tc%d = u_texm[%d] * tc%d;\n", i, i, i);
        if (t->projected && t->ttff >= 2)
            sp(s, "  tc%d.xyz /= tc%d[%d];\n", i, i, t->ttff - 1);
        sp(s, "  v_tc[%d] = tc%d;\n", i, i);
    }
    /* User clip planes (world space for fixed function). */
    if (k->clip_mask && !k->rhw)
        sp(s, "  v_clip0 = vec4(dot(pw, u_clip[0]), dot(pw, u_clip[1]), dot(pw, u_clip[2]), dot(pw, u_clip[3]));\n"
              "  v_clip1 = vec2(dot(pw, u_clip[4]), dot(pw, u_clip[5]));\n");
    else
        sp(s, "  v_clip0 = vec4(1.0);\n  v_clip1 = vec2(1.0);\n");
    sp(s, "}\n");
}

static void arg_expr(char *out, size_t sz, uint8_t a, int stage) {
    const char *base;
    char tb[16];
    switch (a & 0xf) {
    case 0: base = "v_diff"; break;
    case 1: base = "cur"; break;
    case 2: snprintf(tb, sizeof tb, "t%d", stage); base = tb; break;
    case 3: base = "u_tfactor"; break;
    case 4: base = "v_spec"; break;
    case 5: base = "tmp"; break;
    default: base = "cur"; break;
    }
    char v[64];
    if (a & 0x20)
        snprintf(v, sizeof v, "%s.aaaa", base);
    else
        snprintf(v, sizeof v, "%s", base);
    if (a & 0x10)
        snprintf(out, sz, "(vec4(1.0) - %s)", v);
    else
        snprintf(out, sz, "%s", v);
}

/* Emit one combiner op; `sw` is ".rgb" or ".a". */
static void gen_op(Str *s, uint8_t op, uint8_t a1, uint8_t a2, uint8_t a0, int stage, const char *sw,
                   const char *dst) {
    char A[64], B[64], C[64];
    arg_expr(A, sizeof A, a1, stage);
    arg_expr(B, sizeof B, a2, stage);
    arg_expr(C, sizeof C, a0, stage);
    char x[2048];
    switch (op) {
    case 2: snprintf(x, sizeof x, "%s%s", A, sw); break;
    case 3: snprintf(x, sizeof x, "%s%s", B, sw); break;
    case 4: snprintf(x, sizeof x, "%s%s * %s%s", A, sw, B, sw); break;
    case 5: snprintf(x, sizeof x, "2.0 * %s%s * %s%s", A, sw, B, sw); break;
    case 6: snprintf(x, sizeof x, "4.0 * %s%s * %s%s", A, sw, B, sw); break;
    case 7: snprintf(x, sizeof x, "%s%s + %s%s", A, sw, B, sw); break;
    case 8: snprintf(x, sizeof x, "%s%s + %s%s - 0.5", A, sw, B, sw); break;
    case 9: snprintf(x, sizeof x, "2.0 * (%s%s + %s%s - 0.5)", A, sw, B, sw); break;
    case 10: snprintf(x, sizeof x, "%s%s - %s%s", A, sw, B, sw); break;
    case 11: snprintf(x, sizeof x, "%s%s + %s%s - %s%s * %s%s", A, sw, B, sw, A, sw, B, sw); break;
    case 12: snprintf(x, sizeof x, "mix(%s%s, %s%s, v_diff.a)", B, sw, A, sw); break;
    case 13: snprintf(x, sizeof x, "mix(%s%s, %s%s, t%d.a)", B, sw, A, sw, stage); break;
    case 14: snprintf(x, sizeof x, "mix(%s%s, %s%s, u_tfactor.a)", B, sw, A, sw); break;
    case 15: snprintf(x, sizeof x, "%s%s + %s%s * (1.0 - t%d.a)", A, sw, B, sw, stage); break;
    case 16: snprintf(x, sizeof x, "mix(%s%s, %s%s, cur.a)", B, sw, A, sw); break;
    case 18: snprintf(x, sizeof x, "%s%s + %s.a * %s%s", A, sw, A, B, sw); break;
    case 19: snprintf(x, sizeof x, "%s%s * %s%s + %s.a", A, sw, B, sw, A); break;
    case 20: snprintf(x, sizeof x, "(1.0 - %s.a) * %s%s + %s%s", A, B, sw, A, sw); break;
    case 21: snprintf(x, sizeof x, "(1.0 - %s%s) * %s%s + %s.a", A, sw, B, sw, A); break;
    case 24: snprintf(x, sizeof x, "vec4(4.0 * dot(%s.rgb - 0.5, %s.rgb - 0.5))%s", A, B, sw); break;
    case 25: snprintf(x, sizeof x, "%s%s + %s%s * %s%s", C, sw, A, sw, B, sw); break;
    case 26: snprintf(x, sizeof x, "mix(%s%s, %s%s, %s%s)", B, sw, A, sw, C, sw); break;
    default: snprintf(x, sizeof x, "%s%s", A, sw); break;   /* PREMODULATE, bump: approximate */
    }
    sp(s, "  %s%s = clamp(%s, 0.0, 1.0);\n", dst, sw, x);
}

static void gen_fs(Str *s, const Key *k) {
    sp(s, "%s", s_header);
    const char *q = k->flat ? "flat " : "";
    sp(s, "%sin vec4 v_diff;\n%sin vec4 v_spec;\nin vec4 v_tc[8];\nin float v_fog;\nin float v_fogz;\n"
          "in vec4 v_clip0;\nin vec2 v_clip1;\n", q, q);
    sp(s, "uniform vec4 u_tfactor, u_fog, u_fogcolor, u_aref;\n");
    for (int i = 0; i < 8; i++)
        if (k->st[i].kind)
            sp(s, "uniform %s u_tex%d;\n", k->st[i].kind == 2 ? "samplerCube" : "sampler2D", i);
    sp(s, "out vec4 o_color;\nvoid main() {\n");
    if (k->clip_mask && !k->rhw) {
        for (int i = 0; i < 6; i++)
            if (k->clip_mask >> i & 1)
                sp(s, "  if (%s < 0.0) discard;\n", i < 4 ? (const char *[]){"v_clip0.x", "v_clip0.y", "v_clip0.z", "v_clip0.w"}[i]
                                                         : (i == 4 ? "v_clip1.x" : "v_clip1.y"));
    }
    sp(s, "  vec4 cur = v_diff, tmp = vec4(0.0);\n");
    for (int i = 0; i < MAX_STAGES; i++) {
        const StageKey *t = &k->st[i];
        if (t->cop == 1)
            break;
        if (t->kind == 2)
            sp(s, "  vec4 t%d = texture(u_tex%d, v_tc[%d].xyz);\n", i, i, i);
        else if (t->kind == 1)
            sp(s, "  vec4 t%d = texture(u_tex%d, v_tc[%d].xy);\n", i, i, i);
        else
            sp(s, "  vec4 t%d = vec4(1.0);\n", i);
        const char *dst = t->result == 5 ? "tmp" : "cur";
        sp(s, "  {\n  vec4 r = %s;\n", dst);
        gen_op(s, t->cop, t->ca1, t->ca2, t->ca0, i, ".rgb", "r");
        if (t->aop != 1)
            gen_op(s, t->aop, t->aa1, t->aa2, t->aa0, i, ".a", "r");
        else
            sp(s, "  r.a = cur.a;\n");
        sp(s, "  %s = r;\n  }\n", dst);
    }
    if (k->spec_enable)
        sp(s, "  cur.rgb = clamp(cur.rgb + v_spec.rgb, 0.0, 1.0);\n");
    if (k->fog) {
        if (k->fog_table) {
            if (k->fog == 3)
                sp(s, "  float f = clamp((u_fog.y - v_fogz) / max(u_fog.y - u_fog.x, 1e-6), 0.0, 1.0);\n");
            else if (k->fog == 1)
                sp(s, "  float f = clamp(exp(-u_fog.z * v_fogz), 0.0, 1.0);\n");
            else
                sp(s, "  float f = clamp(exp(-(u_fog.z * v_fogz) * (u_fog.z * v_fogz)), 0.0, 1.0);\n");
        } else {
            sp(s, "  float f = v_fog;\n");
        }
        sp(s, "  cur.rgb = mix(u_fogcolor.rgb, cur.rgb, f);\n");
    }
    static const char *cmp[] = {"true", "false", "<", "==", "<=", ">", "!=", ">=", "true"};
    if (k->alpha_func && k->alpha_func != 8) {
        if (k->alpha_func == 1)
            sp(s, "  discard;\n");
        else
            sp(s, "  if (!(floor(cur.a * 255.0 + 0.5) %s u_aref.x)) discard;\n", cmp[k->alpha_func]);
    }
    const char *dbg = getenv("LR2_FFP_DEBUG");
    if (dbg && *dbg == '2')
        sp(s, "  o_color = vec4(fract(v_tc[0].xy), 0.0, 1.0);\n}\n");
    else if (dbg && *dbg == '3' && k->st[0].kind)
        sp(s, "  o_color = vec4(t0.rgb, 1.0);\n}\n");
    else if (dbg && *dbg == '5' && k->st[0].kind)
        sp(s, "  o_color = vec4(vec3(t0.a), 1.0);\n}\n");
    else if (dbg && *dbg == '6')
        sp(s, "  o_color = vec4(cur.rgb, 1.0);\n}\n");
    else if (dbg && *dbg == '4')
        sp(s, "  o_color = vec4(v_diff.rgb, 1.0);\n}\n");
    else if (dbg)
        sp(s, "  o_color = vec4(1.0, 0.0, 1.0, 1.0);\n}\n");
    else
        sp(s, "  o_color = cur;\n}\n");
}

static GLuint compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        LOGE("shader compile failed:\n%s\n--- source ---\n%s", log, src);
    }
    return sh;
}

static Program *build(const Key *k, uint64_t h) {
    Str vs = {0}, fs = {0};
    gen_vs(&vs, k);
    gen_fs(&fs, k);
    if (getenv("LR2_DUMP_SHADERS"))
        LOGI("ffp program source:\nVS:\n%s\nFS:\n%s", vs.buf, fs.buf);
    GLuint v = compile(GL_VERTEX_SHADER, vs.buf), f = compile(GL_FRAGMENT_SHADER, fs.buf);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindFragDataLocation(p, 0, "o_color");
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        LOGE("program link failed: %s\nVS:\n%s\nFS:\n%s", log, vs.buf, fs.buf);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    free(vs.buf);
    free(fs.buf);
    Program *pr = calloc(1, sizeof *pr);
    pr->key = *k;
    pr->hash = h;
    pr->prog = p;
    for (int i = 0; i < U_COUNT; i++)
        pr->u[i] = i == U_SAMPLER ? -1 : glGetUniformLocation(p, s_uniform_names[i]);
    glUseProgram(p);
    char name[16];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof name, "u_tex%d", i);
        GLint loc = glGetUniformLocation(p, name);
        if (loc >= 0)
            glUniform1i(loc, i);
    }
    LOGD("ffp: built program %d (%d total)", p, s_nprogs + 1);
    return pr;
}

static Program *lookup(const Key *k) {
    uint64_t h = hash_key(k);
    for (int i = 0; i < s_nprogs; i++)
        if (s_progs[i]->hash == h && !memcmp(&s_progs[i]->key, k, sizeof *k))
            return s_progs[i];
    Program *p = build(k, h);
    if (s_nprogs < MAX_PROGRAMS)
        s_progs[s_nprogs++] = p;
    return p;
}

/* -------------------------------------------------------------- per draw */
static Texture *stage_texture(Device *d, int i) {
    uint32_t g = d->st.tex_obj[i];
    return g ? (Texture *)com_get(g) : NULL;
}

static void make_key(Device *d, uint32_t fvf, Key *k) {
    const State *st = &d->st;
    const uint32_t *rs = st->rs;
    memset(k, 0, sizeof *k);
    k->rhw = (fvf & 0xe) == 0x4;
    k->normal = (fvf & 0x10) != 0;
    k->diffuse = (fvf & 0x40) != 0;
    k->specular = (fvf & 0x80) != 0;
    uint32_t pos = fvf & 0xe;
    if (!k->rhw && pos >= 0x6 && rs[151] >= 1 && rs[151] <= 3) {
        int nb = (int)(pos - 0x4) / 2;
        k->blend = (uint8_t)SDL_min((int)rs[151], nb);
    }
    k->ntc = (uint8_t)((fvf >> 8) & 0xf);
    for (int i = 0; i < k->ntc && i < 8; i++) {
        static const uint8_t sz[4] = {2, 3, 4, 1};
        k->tc_size[i] = sz[(fvf >> (16 + 2 * i)) & 3];
    }
    k->lighting = rs[137] && !k->rhw;
    if (k->lighting) {
        int n = 0;
        for (int i = 0; i < MAX_LIGHTS && n < 8; i++)
            if (st->light_on[i])
                k->light_type[n++] = (uint8_t)st->lights[i].type;
        k->localviewer = rs[142] != 0;
        k->color_vertex = rs[141] != 0;
        k->src_diff = (uint8_t)rs[145];
        k->src_spec = (uint8_t)rs[146];
        k->src_amb = (uint8_t)rs[147];
        k->src_emis = (uint8_t)rs[148];
    }
    k->spec_enable = rs[29] != 0;
    if (rs[28]) {
        if (rs[35]) {
            k->fog = (uint8_t)rs[35];
            k->fog_table = 1;
        } else if (rs[140] || k->rhw) {
            k->fog = k->rhw ? 3 : (uint8_t)rs[140];
        }
        k->fog_range = rs[48] != 0;
    }
    k->alpha_func = rs[15] ? (uint8_t)rs[25] : 0;
    k->clip_mask = (uint8_t)(rs[152] & 0x3f);
    k->flat = rs[9] == 1;
    for (int i = 0; i < MAX_STAGES; i++) {
        const uint32_t *t = st->tss[i];
        StageKey *sk = &k->st[i];
        sk->cop = (uint8_t)t[1];
        if (sk->cop == 1)
            break;
        sk->ca1 = (uint8_t)t[2];
        sk->ca2 = (uint8_t)t[3];
        sk->ca0 = (uint8_t)t[26];
        sk->aop = (uint8_t)t[4];
        sk->aa1 = (uint8_t)t[5];
        sk->aa2 = (uint8_t)t[6];
        sk->aa0 = (uint8_t)t[27];
        sk->result = (uint8_t)t[28];
        Texture *tex = stage_texture(d, i);
        if (tex) {
            sk->kind = tex->type == RT_CUBETEXTURE ? 2 : 1;
            sk->tci = (uint8_t)(t[11] & 7);
            sk->texgen = (uint8_t)((t[11] >> 16) & 3);
            sk->ttff = (uint8_t)(t[24] & 7);
            sk->projected = (t[24] & 256) != 0;
        }
    }
}

static GLint gl_wrap(uint32_t a) {
    switch (a) {
    case 2: return GL_MIRRORED_REPEAT;
    case 3: return GL_CLAMP_TO_EDGE;
    case 4: return GL_CLAMP_TO_BORDER;
    case 5: return GL_MIRRORED_REPEAT;
    }
    return GL_REPEAT;
}

static void apply_sampler(Texture *t, const uint32_t *tss) {
    GLenum tg = tex_gl_target(t);
    glTexParameteri(tg, GL_TEXTURE_WRAP_S, gl_wrap(tss[13]));
    glTexParameteri(tg, GL_TEXTURE_WRAP_T, gl_wrap(tss[14]));
    glTexParameteri(tg, GL_TEXTURE_WRAP_R, gl_wrap(tss[25]));
    if (tss[13] == 4 || tss[14] == 4) {
        uint32_t b = tss[15];
        float bc[4] = {(float)(b >> 16 & 0xff) / 255.0f, (float)(b >> 8 & 0xff) / 255.0f,
                       (float)(b & 0xff) / 255.0f, (float)(b >> 24) / 255.0f};
        glTexParameterfv(tg, GL_TEXTURE_BORDER_COLOR, bc);
    }
    bool mag_lin = tss[16] >= 2, min_lin = tss[17] >= 2;
    uint32_t mip = t->levels > 1 ? tss[18] : 0;
    GLint minf = mip == 0 ? (min_lin ? GL_LINEAR : GL_NEAREST)
               : mip == 1 ? (min_lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST)
                          : (min_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
    glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, minf);
    glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, mag_lin ? GL_LINEAR : GL_NEAREST);
    glTexParameterf(tg, GL_TEXTURE_LOD_BIAS, u2f(tss[19]));
    uint32_t base = SDL_max(tss[20], t->lod);
    glTexParameteri(tg, GL_TEXTURE_BASE_LEVEL, (GLint)SDL_min(base, t->levels - 1));
}

static void xform_point(const Mat4 *m, const float *p, float w, float *out) {
    for (int j = 0; j < 4; j++)
        out[j] = p[0] * m->m[j] + p[1] * m->m[4 + j] + p[2] * m->m[8 + j] + w * m->m[12 + j];
}

static void color4(uint32_t c, float *o) {
    o[0] = (float)(c >> 16 & 0xff) / 255.0f;
    o[1] = (float)(c >> 8 & 0xff) / 255.0f;
    o[2] = (float)(c & 0xff) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

void ffp_draw_setup(Device *d, uint32_t fvf, bool *ok) {
    Key k;
    make_key(d, fvf, &k);
    Program *p = lookup(&k);
    if (!p->prog) {
        *ok = false;
        return;
    }
    glUseProgram(p->prog);
    const State *st = &d->st;
    const uint32_t *rs = st->rs;
    const GLint *u = p->u;
    /* Textures */
    for (int i = 0; i < MAX_STAGES; i++) {
        if (!k.st[i].kind)
            continue;
        Texture *t = stage_texture(d, i);
        tex_bind(d, i, t);
        apply_sampler(t, st->tss[i]);
    }
    /* Transforms */
    if (u[U_WORLD] >= 0)
        glUniformMatrix4fv(u[U_WORLD], 4, GL_FALSE, st->world[0].m);
    if (u[U_VIEW] >= 0)
        glUniformMatrix4fv(u[U_VIEW], 1, GL_FALSE, st->view.m);
    if (u[U_PROJ] >= 0)
        glUniformMatrix4fv(u[U_PROJ], 1, GL_FALSE, st->proj.m);
    if (u[U_TEXM] >= 0)
        glUniformMatrix4fv(u[U_TEXM], 8, GL_FALSE, st->tex[0].m);
    const Viewport *vp = &st->vp;
    float vw = vp->w ? (float)vp->w : 1.0f, vh = vp->h ? (float)vp->h : 1.0f;
    float rhw[4] = {(float)vp->x, (float)vp->y, 2.0f / vw, 2.0f / vh};
    if (u[U_RHW] >= 0)
        glUniform4fv(u[U_RHW], 1, rhw);
    float dz = vp->maxz - vp->minz;
    float hp[4] = {1.0f / vw, 1.0f / vh, dz != 0 ? 2.0f / dz : 2.0f, dz != 0 ? -2.0f * vp->minz / dz - 1.0f : -1.0f};
    if (u[U_HALFPIX] >= 0)
        glUniform4fv(u[U_HALFPIX], 1, hp);
    /* Material and lights */
    if (k.lighting) {
        glUniform4fv(u[U_MDIFF], 1, &st->mat.diffuse.r);
        glUniform4fv(u[U_MAMB], 1, &st->mat.ambient.r);
        glUniform4fv(u[U_MSPEC], 1, &st->mat.specular.r);
        glUniform4fv(u[U_MEMIS], 1, &st->mat.emissive.r);
        glUniform1f(u[U_MPOW], st->mat.power);
        float amb[4];
        color4(rs[139], amb);
        glUniform4fv(u[U_AMBIENT], 1, amb);
        float ld[8][4] = {{0}}, ls[8][4] = {{0}}, la[8][4] = {{0}}, lp[8][4] = {{0}}, ldir[8][4] = {{0}};
        float latt[8][4] = {{0}}, lspot[8][4] = {{0}};
        int n = 0;
        for (int i = 0; i < MAX_LIGHTS && n < 8; i++) {
            if (!st->light_on[i])
                continue;
            const Light *l = &st->lights[i];
            memcpy(ld[n], &l->diffuse, 16);
            memcpy(ls[n], &l->specular, 16);
            memcpy(la[n], &l->ambient, 16);
            xform_point(&st->view, l->pos, 1.0f, lp[n]);
            xform_point(&st->view, l->dir, 0.0f, ldir[n]);
            float len = sqrtf(ldir[n][0] * ldir[n][0] + ldir[n][1] * ldir[n][1] + ldir[n][2] * ldir[n][2]);
            if (len > 0)
                for (int j = 0; j < 3; j++)
                    ldir[n][j] /= len;
            latt[n][0] = l->att0;
            latt[n][1] = l->att1;
            latt[n][2] = l->att2;
            latt[n][3] = l->range > 0 ? l->range : 1e30f;
            lspot[n][0] = cosf(l->theta / 2);
            lspot[n][1] = cosf(l->phi / 2);
            lspot[n][2] = l->falloff;
            n++;
        }
        glUniform4fv(u[U_LDIFF], 8, ld[0]);
        if (u[U_LSPEC] >= 0)
            glUniform4fv(u[U_LSPEC], 8, ls[0]);
        glUniform4fv(u[U_LAMB], 8, la[0]);
        if (u[U_LPOS] >= 0)
            glUniform4fv(u[U_LPOS], 8, lp[0]);
        if (u[U_LDIR] >= 0)
            glUniform4fv(u[U_LDIR], 8, ldir[0]);
        if (u[U_LATT] >= 0)
            glUniform4fv(u[U_LATT], 8, latt[0]);
        if (u[U_LSPOT] >= 0)
            glUniform4fv(u[U_LSPOT], 8, lspot[0]);
    }
    if (k.fog) {
        float fog[4] = {u2f(rs[36]), u2f(rs[37]), u2f(rs[38]), 0};
        if (u[U_FOG] >= 0)
            glUniform4fv(u[U_FOG], 1, fog);
        float fc[4];
        color4(rs[34], fc);
        glUniform4fv(u[U_FOGCOLOR], 1, fc);
    }
    if (k.clip_mask && u[U_CLIP] >= 0)
        glUniform4fv(u[U_CLIP], 6, st->clip[0]);
    if (u[U_TFACTOR] >= 0) {
        float tf[4];
        color4(rs[60], tf);
        glUniform4fv(u[U_TFACTOR], 1, tf);
    }
    if (u[U_AREF] >= 0) {
        float ar[4] = {(float)(rs[24] & 0xff), 0, 0, 0};
        glUniform4fv(u[U_AREF], 1, ar);
    }
    glPointSize(SDL_max(u2f(rs[154]), 1.0f));
}

void ffp_init(void) {}
