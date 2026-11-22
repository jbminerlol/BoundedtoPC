/* 3D voxel overworld (F1).
 *
 * The map and buildings are rebuilt as voxels every frame from what the running game has in
 * VRAM (BG1 = ground layer, BG2 = upper layer) and from its collision map. Characters and every
 * other object stay the game's own 2D sprites, drawn pixel-perfect (nearest texels, hard alpha)
 * on upright cards that turn to face the camera, with a soft shadow under each. Text windows
 * (BG3) are drawn on top in 2D. Nothing is added to the game's art: it all comes from VRAM.
 *
 * Height model ("oblique-consistent"): EarthBound is drawn in a 3/4 view where a point h pixels
 * above the ground shows h pixels higher on screen. A pixel given height h is therefore placed
 * h pixels further south, so from the game's own viewing angle every voxel lands on its original
 * pixel; turning the camera shows the depth.
 *   - objects = a column's run of solid tiles plus the tiles right above it that the game draws
 *     over sprites (flags 0x01/0x02); height = distance to the run's bottom edge, where it stands
 *   - outdoors, short objects with such a top (trees, signs, fences, posts) are flat cut-outs,
 *     like the sprites; houses, walls, cliffs and furniture are extruded (WALL_THICK deep, capped
 *     at HMAX where the rest becomes a filled roof block)
 *   - ground colour showing inside object tiles stays on the ground (flood from open ground)
 *   - outdoors the grass stands 2 px above roads, paths and water, tufts and flowers one more;
 *     objects and sprites stand on top of it; objects cast a short contact shadow
 *   - BG2 is the game's copy of BG1's upper parts: only pixels that differ from BG1 are used
 * Sky: blue outdoors (pink in Magicant, darker at night), the game's black indoors and in caves.
 * Post-processing: tilt-shift blur away from the player's row (miniature look) and a vignette.
 *
 * Camera: right-drag or J/L/I/K or the right stick to orbit/tilt, wheel or -/= to zoom,
 * R / Home / F6 / middle button / right stick click to reset.
 */
#include "render3d.h"

#include <math.h>

#include "eb_syms.h"
#include "enhance.h"
#include "game.h"
#include "gl.h"
#include "host.h"
#include "ppu.h"
#include "png.h"
#include "snes.h"

int ppu_bg_tile(int bg, int tx, int ty, uint32_t out_rgb[64], uint8_t out_op[64]);
int ppu_obj_piece(int i, int ext, int *ox, int *oy, int *ow, int *oh, int *oprio, uint32_t *rgba);
void ppu_render_layers(uint32_t *out, int pitch_px, int ext, uint8_t mask);

#define VX_MARGIN 72
#define VW (256 + 2 * VX_MARGIN)
#define VY0 (-8)
#define VH 240
#define HMAX 32
#define HFG 24
#define WALL_THICK 8
#define ATLAS_W 1024
#define ATLAS_H 512
#define DEF_PITCH 56.0f
#define WATER_D 4          /* solid water sits this far below the ground */
#define SHADOW_RES 1536
#define TREE_H 32          /* tall vegetation: front row of trees this high, flat canopy behind */
#define SPRITE_DEPTH_BIAS 6.0f /* sprites sort as if this much closer to the camera than their feet */

typedef struct Voxel {
  int16_t x, dep, z0, z1, len, pad; /* occupies x..x+1, dep-len+1..dep+1, z0..z1 (screen units) */
  uint32_t argb;
} Voxel;

typedef struct CardVert {
  float x, y, z, u, v, sx, sy;
  float ux, uy, uz; /* the same corner on an upright card at the feet: depth and shadow lookup */
} CardVert;


static uint32_t s_c1[VW * VH], s_c2[VW * VH];
static uint8_t s_o1[VW * VH], s_o2[VW * VH], s_coll[VW * VH];
enum { OBJ_PROP = 1, OBJ_BLOCK = 2 };
static uint8_t s_obj[VW * VH];    /* pixel belongs to an object column (OBJ_*) */
static uint8_t s_gnd[VW * VH];    /* object pixel that is really ground showing through (flood depth) */
static uint16_t s_base[VW * VH];  /* object pixel: row its object stands on (exclusive) */
static uint16_t s_otop[VW * VH];  /* object pixel: top row of its column's object */
static uint16_t s_stop[VW * VH];  /* object pixel: top row of its column's solid run (its footprint) */
static uint8_t s_hwall[VW * VH];  /* house pixel: its facade height (0 = unknown) */
static uint8_t s_roofe[VW * VH];  /* house pixel: how many rows its roof's front slope covers */
static uint16_t s_rtop[VW * VH];  /* object pixel: topmost row its column run actually draws (not ground) */
static uint16_t s_comp[VW * VH];  /* leafy cut-out pixel: its tree's index in s_comp_r (0 = none) */
static uint8_t s_comp_r[4096];    /* per tree: its radius (largest distance to its outline), capped */
static uint32_t s_wallc[VW * VH]; /* house pixel: the building's wall colour (alpha set when known) */
static uint8_t s_flat[VW * VH];   /* bits - 1: block pixel of an irregular mass (cliff, rock): flat
                                   * top, no roof; 2: cut-out pixel of a forest (tall vegetation);
                                   * 4: cut-out that is mostly leaves (puffed rounder) */
static uint8_t s_water[VW * VH];  /* solid water: drawn sunken */
static uint32_t s_under[VW * VH]; /* object pixel: colour for the ground texture under it */
static uint8_t s_hg[VW * VH];     /* terrain height of ground pixels */
static int s_queue[VW * VH];
static int s_outdoor, s_setting;
static uint32_t s_ground[VW * VH];
static uint8_t s_lum[VW * VH];
static Voxel *s_vox;
static int s_nvox, s_vcap;
static uint32_t s_atlas[ATLAS_W * ATLAS_H];
static CardVert s_cards[128 * 6];
static int s_ncardv;
static CardVert s_cards_l[128 * 6]; /* the same sprites turned towards the sun (shadow map) */
static int s_ncardv_l;
static uint32_t s_overlay[PPU_MAX_W * PPU_H];
static uint32_t s_piece[64 * 64];

static GLuint s_prog_vox, s_prog_ground, s_prog_card, s_prog_ovl, s_prog_post, s_prog_sky;
static GLuint s_prog_vox_depth, s_prog_card_depth, s_shadow_fbo, s_shadow_tex;
static float s_lmvp[16];
static float s_scene_lum; /* average brightness of the visible map (night / dark areas dim the sky) */
static GLuint s_vao_vox, s_vbo_vox, s_vao_card, s_vbo_card, s_vao_empty;
static GLuint s_tex_ground, s_tex_atlas, s_tex_ovl;
static GLuint s_fbo, s_fbo_color, s_fbo_depth;
static int s_fbo_w, s_fbo_h;
static int s_ready;

static float s_yaw, s_pitch = DEF_PITCH, s_zoom = 1.0f;
static int s_dragging;
int g_render3d_voxels;

static inline uint16_t ram16(uint32_t a) {
  return (uint16_t)(g_wram[a & 0x1FFFF] | (g_wram[(a + 1) & 0x1FFFF] << 8));
}
static inline int floor_div(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
static inline uint32_t scale_rgb(uint32_t c, int num, int den) {
  int r = (int)((c >> 16) & 255) * num / den, g = (int)((c >> 8) & 255) * num / den, b = (int)(c & 255) * num / den;
  return (uint32_t)((r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b));
}

/* ---- shaders ------------------------------------------------------------------------------------- */
/* every lit fragment shader starts with this: fog toward the edges of the decoded area and the
 * sun's shadow map (directional light from the east, a little south: shadows fall west) */
static const char *k_fog =
    "uniform vec4 u_valid; uniform vec3 u_clear; uniform float u_bright;\n"
    "uniform sampler2DShadow u_shadow; uniform vec2 u_stexel; uniform float u_sdark;\n"
    "in vec4 v_lpos;\n"
    "float fogf(vec2 p) {\n"
    "  vec2 a = smoothstep(u_valid.xy, u_valid.xy + 18.0, p);\n"
    "  vec2 b = 1.0 - smoothstep(u_valid.zw - 18.0, u_valid.zw, p);\n"
    "  return a.x * a.y * b.x * b.y;\n"
    "}\n"
    "float shadowf() {\n"
    "  vec3 p = v_lpos.xyz / v_lpos.w * 0.5 + 0.5;\n"
    "  if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0 || p.z >= 1.0) return 1.0;\n"
    "  return mix(u_sdark, 1.0, texture(u_shadow, p));\n"
    "}\n";

/* voxel corner/face tables shared by the lit and the shadow-map voxel shaders */
#define VOX_TABLES                                                     \
  "const vec3 C[24] = vec3[24](\n"                                     \
  "  vec3(0,0,1), vec3(1,0,1), vec3(1,1,1), vec3(0,1,1),\n"            \
  "  vec3(0,0,0), vec3(0,1,0), vec3(1,1,0), vec3(1,0,0),\n"            \
  "  vec3(0,1,0), vec3(1,1,0), vec3(1,1,1), vec3(0,1,1),\n"            \
  "  vec3(0,0,0), vec3(0,0,1), vec3(1,0,1), vec3(1,0,0),\n"            \
  "  vec3(1,0,0), vec3(1,0,1), vec3(1,1,1), vec3(1,1,0),\n"            \
  "  vec3(0,0,0), vec3(0,1,0), vec3(0,1,1), vec3(0,0,1));\n"           \
  "const int T[6] = int[6](0, 1, 2, 0, 2, 3);\n"                       \
  "const vec3 N[6] = vec3[6](vec3(0,0,1), vec3(0,0,-1), vec3(0,-1,0), vec3(0,1,0), vec3(1,0,0), vec3(-1,0,0));\n" \
  "vec3 vox_corner(vec4 box, float len_in, int vid, out int face) {\n" \
  "  face = vid / 6;\n"                                                \
  "  vec3 c = C[face * 4 + T[vid - face * 6]];\n"                      \
  "  float len = max(len_in, 1.0);\n"                                  \
  "  float dep = box.y + 1.0 - len + c.y * len;\n"                     \
  "  return vec3(box.x + c.x, -dep, mix(box.z, box.w, c.z));\n"        \
  "}\n"                                                                \
  "vec3 vox_corner_f(vec4 box, float len_in, int face, int corner) {\n" \
  "  vec3 c = C[face * 4 + corner];\n"                                 \
  "  float len = max(len_in, 1.0);\n"                                  \
  "  float dep = box.y + 1.0 - len + c.y * len;\n"                     \
  "  return vec3(box.x + c.x, -dep, mix(box.z, box.w, c.z));\n"        \
  "}\n"                                                                \
  /* per-voxel mask (Voxel.pad): side faces covered by a neighbouring column (1 S, 2 N, 4 E, 8 W) */ \
  "bool vox_face_hidden(float maskf, int face) {\n"                   \
  "  int m = int(maskf);\n"                                            \
  "  return face >= 2 && (m & (1 << (face - 2))) != 0;\n"              \
  "}\n"                                                                \
  "vec3 vox_face_point(vec4 box, float len_in, int face) {\n"         \
  "  float len = max(len_in, 1.0);\n"                                  \
  "  vec3 ctr = vec3(box.x + 0.5, -(box.y + 1.0 - 0.5 * len), 0.5 * (box.z + box.w));\n" \
  "  return ctr + N[face] * vec3(0.5, 0.5 * len, 0.5 * abs(box.w - box.z));\n" \
  "}\n"

static const char *k_vox_vs =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_box;\n"
    "layout(location=1) in vec2 a_len;\n"
    "layout(location=2) in vec4 a_col;\n"
    "uniform mat4 u_mvp, u_lmvp; uniform vec3 u_eye, u_sun;\n"
    "out vec3 v_col; out vec2 v_scr; out vec4 v_lpos; out float v_lit;\n" VOX_TABLES
    /* faces: top, bottom, south (towards the default camera), north, east, west; sun in the north-west */
    "const float S[6] = float[6](1.0, 0.45, 0.86, 0.74, 0.66, 0.92);\n"
    "void main() {\n"
    /* only the three faces that can be turned to the camera: top, south or north, east or west */
    "  int slot = gl_VertexID / 6;\n"
    "  int face = slot == 0 ? 0 : slot == 1 ? (dot(N[2], u_eye - vox_face_point(a_box, a_len.x, 2)) > 0.0 ? 2 : 3)\n"
    "                                       : (dot(N[4], u_eye - vox_face_point(a_box, a_len.x, 4)) > 0.0 ? 4 : 5);\n"
    "  vec3 w = vox_corner_f(a_box, a_len.x, face, T[gl_VertexID - slot * 6]);\n"
    "  gl_Position = u_mvp * vec4(w, 1.0);\n"
    "  if (vox_face_hidden(a_len.y, face)) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n"
    "  v_lpos = u_lmvp * vec4(w, 1.0);\n"
    "  v_lit = dot(N[face], u_sun) > 0.0 ? 1.0 : 0.0;\n"
    "  float amb = face == 0 ? 1.0 : 0.82 + 0.18 * clamp(w.z / 32.0, 0.0, 1.0);\n"
    "  v_col = a_col.rgb * S[face] * amb;\n"
    "  v_scr = vec2(w.x, a_box.y - w.z);\n"
    "}\n";

static const char *k_vox_fs_body =
    "in vec3 v_col; in vec2 v_scr; in float v_lit; out vec4 o;\n"
    "void main() { float sh = v_lit > 0.5 ? shadowf() : 1.0;\n"
    "  o = vec4(mix(u_clear, v_col * sh, fogf(v_scr)) * u_bright, 1.0); }\n";

/* the flat ground: one textured quad at height u_z; u_layer 0 = land (z 0), 1 = sunken water */
static const char *k_ground_vs =
    "#version 330 core\n"
    "uniform mat4 u_mvp, u_lmvp; uniform vec4 u_rect; uniform float u_z;\n"
    "out vec2 v_uv; out vec2 v_scr; out vec4 v_lpos;\n"
    "void main() {\n"
    "  vec2 t = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  vec2 s = mix(u_rect.xy, u_rect.zw, t);\n"
    "  vec4 w = vec4(s.x, -s.y, u_z, 1.0);\n"
    "  gl_Position = u_mvp * w; v_lpos = u_lmvp * w;\n"
    "  v_uv = t; v_scr = s;\n"
    "}\n";

static const char *k_ground_fs_body =
    "uniform sampler2D u_tex; uniform int u_layer;\n"
    "in vec2 v_uv; in vec2 v_scr; out vec4 o;\n"
    "void main() { vec4 t = texture(u_tex, v_uv);\n"
    "  if ((t.a < 0.5) != (u_layer == 1)) discard;\n"
    "  vec3 c = u_layer == 1 ? t.rgb * 0.9 : t.rgb;\n"
    "  o = vec4(mix(u_clear, c * shadowf(), fogf(v_scr)) * u_bright, 1.0); }\n";

/* sprites are billboards: the picture faces the camera (a_pos), but depth and the sun's shadow
 * come from the same corner on an upright card at the entity's feet (a_upr), so a sprite leaning
 * back with the camera never sinks into the wall or tree behind it */
static const char *k_card_vs =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos; layout(location=1) in vec2 a_uv; layout(location=2) in vec2 a_scr;\n"
    "layout(location=3) in vec3 a_upr;\n"
    "uniform mat4 u_mvp, u_lmvp;\n"
    "out vec2 v_uv; out vec2 v_scr; out vec4 v_lpos;\n"
    "void main() { vec4 cb = u_mvp * vec4(a_pos, 1.0), cu = u_mvp * vec4(a_upr, 1.0);\n"
    "  float zn = cu.w > 0.001 ? cu.z / cu.w : cb.z / cb.w;\n"
    "  gl_Position = vec4(cb.xy, zn * cb.w, cb.w); v_lpos = u_lmvp * vec4(a_upr, 1.0);\n"
    "  v_uv = a_uv; v_scr = a_scr; }\n";

static const char *k_card_fs_body =
    "uniform sampler2D u_tex;\n"
    "in vec2 v_uv; in vec2 v_scr; out vec4 o;\n"
    "void main() { vec4 c = texture(u_tex, v_uv); if (c.a < 0.5) discard;\n"
    "  o = vec4(mix(u_clear, c.rgb * mix(1.0, shadowf(), 0.7), fogf(v_scr)) * u_bright, 1.0); }\n";

/* shadow map passes (depth only, from the sun) */
static const char *k_vox_depth_vs =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_box;\n"
    "layout(location=1) in vec2 a_len;\n"
    "uniform mat4 u_mvp; uniform vec3 u_sun;\n" VOX_TABLES
    "void main() { int slot = gl_VertexID / 6;\n"
    "  int face = slot == 0 ? 0 : slot == 1 ? (u_sun.y > 0.0 ? 3 : 2) : (u_sun.x > 0.0 ? 4 : 5);\n"
    "  gl_Position = u_mvp * vec4(vox_corner_f(a_box, a_len.x, face, T[gl_VertexID - slot * 6]), 1.0);\n"
    "  if (vox_face_hidden(a_len.y, face)) gl_Position = vec4(2.0, 2.0, 2.0, 1.0); }\n";

static const char *k_depth_fs =
    "#version 330 core\n"
    "void main() {}\n";

static const char *k_card_depth_vs =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos; layout(location=1) in vec2 a_uv;\n"
    "uniform mat4 u_mvp; out vec2 v_uv;\n"
    "void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); v_uv = a_uv; }\n";

static const char *k_card_depth_fs =
    "#version 330 core\n"
    "uniform sampler2D u_tex; in vec2 v_uv;\n"
    "void main() { if (texture(u_tex, v_uv).a < 0.5) discard; }\n";

static const char *k_ovl_vs =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() { vec2 p = vec2(float(gl_VertexID & 1) * 2.0 - 1.0, float(gl_VertexID >> 1) * 2.0 - 1.0);\n"
    "  v_uv = vec2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5); gl_Position = vec4(p, 0.0, 1.0); }\n";

static const char *k_ovl_fs =
    "#version 330 core\n"
    "uniform sampler2D u_tex; uniform vec2 u_uvscale;\n"
    "in vec2 v_uv; out vec4 o;\n"
    "void main() { o = texture(u_tex, v_uv * u_uvscale); if (o.a < 0.5) discard; }\n";

/* sky behind the map: deep blue at the top of the window to a pale haze lower down */
static const char *k_sky_fs =
    "#version 330 core\n"
    "uniform vec3 u_top, u_hor; uniform float u_bright;\n"
    "in vec2 v_uv; out vec4 o;\n"
    "void main() { float t = smoothstep(0.0, 0.65, v_uv.y);\n"
    "  o = vec4(mix(u_top, u_hor, t) * u_bright, 1.0); }\n";

/* tilt-shift + vignette over the rendered scene (v_uv here is bottom-up like GL textures) */
static const char *k_post_fs =
    "#version 330 core\n"
    "uniform sampler2D u_tex; uniform vec2 u_texel; uniform float u_focus, u_band, u_maxblur;\n"
    "in vec2 v_uv; out vec4 o;\n"
    "void main() {\n"
    "  vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);\n"
    "  float d = abs(uv.y - u_focus);\n"
    "  float r = clamp((d - u_band) / max(0.5 - u_band, 0.05), 0.0, 1.0);\n"
    "  r = r * r * u_maxblur;\n"
    "  vec3 acc = texture(u_tex, uv).rgb; float w = 1.0;\n"
    "  if (r > 0.35) {\n"
    "    for (int i = 0; i < 12; i++) {\n"
    "      float a = float(i) * 0.5235988;\n"
    "      vec2 off = vec2(cos(a), sin(a)) * r * u_texel;\n"
    "      acc += texture(u_tex, uv + off).rgb + texture(u_tex, uv + off * 0.5).rgb;\n"
    "      w += 2.0;\n"
    "    }\n"
    "  }\n"
    "  vec2 q = uv - 0.5;\n"
    "  float vig = 1.0 - dot(q, q) * 0.55;\n"
    "  o = vec4(acc / w * vig, 1.0);\n"
    "}\n";

static GLuint make_prog(const char *vs, const char *fs_body, int with_fog, const char *what) {
  char fs[4096];
  SDL_snprintf(fs, sizeof fs, "#version 330 core\n%s%s", with_fog ? k_fog : "", fs_body);
  return gl_program(vs, fs, what);
}

static GLuint make_tex(int w, int h, GLenum filter) {
  GLuint t;
  gl.GenTextures(1, &t);
  gl.BindTexture(GL_TEXTURE_2D, t);
  gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return t;
}

static void ensure_fbo(int w, int h) {
  if (s_fbo && w == s_fbo_w && h == s_fbo_h) return;
  if (!s_fbo) {
    gl.GenFramebuffers(1, &s_fbo);
    gl.GenRenderbuffers(1, &s_fbo_depth);
  } else {
    gl.DeleteTextures(1, &s_fbo_color);
  }
  s_fbo_color = make_tex(w, h, GL_LINEAR);
  gl.BindRenderbuffer(GL_RENDERBUFFER, s_fbo_depth);
  gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
  gl.BindFramebuffer(GL_FRAMEBUFFER, s_fbo);
  gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fbo_color, 0);
  gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_fbo_depth);
  if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) eb_log("3D: framebuffer incomplete");
  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  s_fbo_w = w;
  s_fbo_h = h;
}

void render3d_init(void) {
  s_prog_vox = make_prog(k_vox_vs, k_vox_fs_body, 1, "voxels");
  s_prog_ground = make_prog(k_ground_vs, k_ground_fs_body, 1, "ground");
  s_prog_card = make_prog(k_card_vs, k_card_fs_body, 1, "sprites");
  s_prog_vox_depth = gl_program(k_vox_depth_vs, k_depth_fs, "voxel shadows");
  s_prog_card_depth = gl_program(k_card_depth_vs, k_card_depth_fs, "sprite shadows");
  s_prog_ovl = gl_program(k_ovl_vs, k_ovl_fs, "overlay");
  s_prog_post = gl_program(k_ovl_vs, k_post_fs, "post");
  s_prog_sky = gl_program(k_ovl_vs, k_sky_fs, "sky");
  gl.GenVertexArrays(1, &s_vao_empty);
  gl.GenVertexArrays(1, &s_vao_vox);
  gl.GenBuffers(1, &s_vbo_vox);
  gl.BindVertexArray(s_vao_vox);
  gl.BindBuffer(GL_ARRAY_BUFFER, s_vbo_vox);
  gl.VertexAttribPointer(0, 4, GL_SHORT, GL_FALSE, sizeof(Voxel), (void *)0);
  gl.VertexAttribPointer(1, 2, GL_SHORT, GL_FALSE, sizeof(Voxel), (void *)8);
  gl.VertexAttribPointer(2, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Voxel), (void *)12);
  for (int a = 0; a < 3; a++) {
    gl.EnableVertexAttribArray((GLuint)a);
    gl.VertexAttribDivisor((GLuint)a, 1);
  }
  gl.GenVertexArrays(1, &s_vao_card);
  gl.GenBuffers(1, &s_vbo_card);
  gl.BindVertexArray(s_vao_card);
  gl.BindBuffer(GL_ARRAY_BUFFER, s_vbo_card);
  gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(CardVert), (void *)0);
  gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(CardVert), (void *)12);
  gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(CardVert), (void *)20);
  gl.VertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(CardVert), (void *)28);
  gl.EnableVertexAttribArray(0);
  gl.EnableVertexAttribArray(1);
  gl.EnableVertexAttribArray(2);
  gl.EnableVertexAttribArray(3);
  gl.BindVertexArray(0);
  /* the sun's shadow map: a depth texture compared in the shaders (hardware PCF) */
  gl.GenTextures(1, &s_shadow_tex);
  gl.BindTexture(GL_TEXTURE_2D, s_shadow_tex);
  gl.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SHADOW_RES, SHADOW_RES, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  gl.GenFramebuffers(1, &s_shadow_fbo);
  gl.BindFramebuffer(GL_FRAMEBUFFER, s_shadow_fbo);
  gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_shadow_tex, 0);
  gl.DrawBuffer(GL_NONE);
  gl.ReadBuffer(GL_NONE);
  if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) eb_log("3D: shadow map framebuffer incomplete");
  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  s_tex_ground = make_tex(VW, VH, GL_NEAREST);
  s_tex_atlas = make_tex(ATLAS_W, ATLAS_H, GL_NEAREST);
  s_tex_ovl = make_tex(PPU_MAX_W, PPU_H, GL_NEAREST);
  s_ready = 1;
}

static void reset_camera(void) {
  s_yaw = 0.0f;
  s_pitch = DEF_PITCH;
  s_zoom = 1.0f;
  host_note("camera reset");
}

void render3d_reset_camera(void) { reset_camera(); }

void render3d_event(const SDL_Event *e) {
  if (!g_cfg.voxel3d) return;
  switch (e->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (e->button.button == SDL_BUTTON_RIGHT) s_dragging = 1;
      if (e->button.button == SDL_BUTTON_MIDDLE) reset_camera();
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (e->button.button == SDL_BUTTON_RIGHT) s_dragging = 0;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (s_dragging) {
        s_yaw += e->motion.xrel * 0.3f;
        s_pitch += e->motion.yrel * 0.3f;
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL: s_zoom *= powf(0.9f, e->wheel.y); break;
    case SDL_EVENT_KEY_DOWN:
      /* F6, R or Home (and the middle mouse button / right stick click) put the camera back */
      if (e->key.scancode == SDL_SCANCODE_F6 || e->key.scancode == SDL_SCANCODE_R ||
          e->key.scancode == SDL_SCANCODE_HOME)
        reset_camera();
      break;
  }
}

/* keyboard (J/L rotate, I/K tilt, -/= zoom) and the gamepad's right stick, once per frame */
static void poll_camera_keys(void) {
  {
    /* tests: EB_CAM="yaw,pitch[,zoom]" fixes the camera */
    const char *cam = SDL_getenv("EB_CAM");
    if (cam) {
      float y = 0, p = DEF_PITCH, z = 1;
      if (SDL_sscanf(cam, "%f,%f,%f", &y, &p, &z) >= 2) {
        s_yaw = y;
        s_pitch = p;
        s_zoom = z;
        return;
      }
    }
  }
  const bool *k = SDL_GetKeyboardState(NULL);
  if (k[SDL_SCANCODE_J]) s_yaw -= 2.0f;
  if (k[SDL_SCANCODE_L]) s_yaw += 2.0f;
  if (k[SDL_SCANCODE_I]) s_pitch += 1.2f;
  if (k[SDL_SCANCODE_K]) s_pitch -= 1.2f;
  if (k[SDL_SCANCODE_MINUS]) s_zoom *= 1.02f;
  if (k[SDL_SCANCODE_EQUALS]) s_zoom /= 1.02f;
  SDL_Gamepad *pad = host_gamepad();
  if (pad) {
    float rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
    float ry = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
    if (fabsf(rx) > 0.2f) s_yaw += rx * 2.5f;
    if (fabsf(ry) > 0.2f) s_pitch -= ry * 1.5f;
    static int r3_was;
    int r3 = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    if (r3 && !r3_was) reset_camera();
    r3_was = r3;
  }
  if (s_pitch < 18.0f) s_pitch = 18.0f;
  if (s_pitch > 88.0f) s_pitch = 88.0f;
  if (s_yaw > 180.0f) s_yaw -= 360.0f;
  if (s_yaw < -180.0f) s_yaw += 360.0f;
  if (s_zoom < 0.35f) s_zoom = 0.35f;
  if (s_zoom > 2.5f) s_zoom = 2.5f;
}

/* ---- scene extraction -------------------------------------------------------------------------- */
static void decode_layer(int bg, int hofs, int vofs, uint32_t *col, uint8_t *op) {
  uint32_t rgb[64];
  uint8_t o[64];
  int tx0 = floor_div(-VX_MARGIN + hofs, 8), tx1 = floor_div(-VX_MARGIN + VW - 1 + hofs, 8);
  int ty0 = floor_div(VY0 + vofs, 8), ty1 = floor_div(VY0 + VH - 1 + vofs, 8);
  for (int ty = ty0; ty <= ty1; ty++) {
    for (int tx = tx0; tx <= tx1; tx++) {
      ppu_bg_tile(bg, tx, ty, rgb, o);
      for (int py = 0; py < 8; py++) {
        int iy = ty * 8 + py - vofs - VY0;
        if (iy < 0 || iy >= VH) continue;
        for (int px = 0; px < 8; px++) {
          int ix = tx * 8 + px - hofs + VX_MARGIN;
          if (ix < 0 || ix >= VW) continue;
          col[iy * VW + ix] = rgb[py * 8 + px];
          op[iy * VW + ix] = o[py * 8 + px];
        }
      }
    }
  }
}

static Voxel s_pend[VW];
static uint8_t s_pend_on[VW];
/* leaf colour: hue 70-192 and at least 30% saturated (greens, olive, the teal of trees at night;
 * not the pale cream of siding, hue 60 and 19%) -
 * returns 1 for leaves, 0 otherwise; *body is set when the pixel counts at all (not dark outline) */
static int leaf_colour(uint32_t c, int *body) {
  int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
  int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  *body = 0;
  if (mx - mn >= 36 && mx >= 40) {
    int hue = mx == r ? (60 * (g - b) / (mx - mn) + 360) % 360
            : mx == g ? 60 * (b - r) / (mx - mn) + 120 : 60 * (r - g) / (mx - mn) + 240;
    *body = 1;
    return hue >= 70 && hue <= 192 && (mx - mn) * 100 >= 30 * mx;
  }
  if (mx >= 64) *body = 1; /* light grey/white */
  return 0;
}

static void push_vox(int sx, int dep, int z0, int z1, int len, uint32_t rgb);
static void flush_column(int ix) {
  if (!s_pend_on[ix]) return;
  Voxel *p = &s_pend[ix];
  s_pend_on[ix] = 0;
  push_vox(p->x, p->dep, p->z0, p->z1, p->len, p->argb & 0xFFFFFF);
}
/* rows are emitted top to bottom, so a voxel right under the column's pending one with the same
 * place, depth and colour just extends it downwards (fewer voxels for walls and cut-outs) */
static void push_vox_column(int ix, int sx, int dep, int z0, int z1, int len, uint32_t rgb) {
  Voxel *p = &s_pend[ix];
  uint32_t argb = 0xFF000000u | rgb;
  if (s_pend_on[ix] && p->x == sx && p->dep == dep && p->len == len && p->argb == argb && p->z0 == z1) {
    p->z0 = (int16_t)z0;
    return;
  }
  flush_column(ix);
  p->x = (int16_t)sx;
  p->dep = (int16_t)dep;
  p->z0 = (int16_t)z0;
  p->z1 = (int16_t)z1;
  p->len = (int16_t)len;
  p->pad = 0;
  p->argb = argb;
  s_pend_on[ix] = 1;
}

static void push_vox(int sx, int dep, int z0, int z1, int len, uint32_t rgb) {
  if (s_nvox >= s_vcap) {
    s_vcap = s_vcap ? s_vcap * 2 : 131072;
    s_vox = (Voxel *)SDL_realloc(s_vox, (size_t)s_vcap * sizeof(Voxel));
  }
  Voxel *v = &s_vox[s_nvox++];
  v->x = (int16_t)sx;
  v->dep = (int16_t)dep;
  v->z0 = (int16_t)z0;
  v->z1 = (int16_t)z1;
  v->len = (int16_t)len;
  v->pad = 0;
  v->argb = 0xFF000000u | rgb;
}

/* The map sector's "setting" (MAP_DATA_PER_SECTOR_ATTRIBUTES_TABLE at $D7B200, one 2-byte entry
 * per 256x128 px sector, 32 sectors across): 0 none, 1 indoors, 2 dungeon (Exit Mouse works),
 * 3 Lost Underworld, 4 Magicant, 5 Cave of the Past, 6/7 magic butterflies. */
static int sector_setting(void) {
  int x = ram16(SYM_BG1_X_POS) + 128, y = ram16(SYM_BG1_Y_POS) + 112;
  int idx = (y >> 7) * 32 + (x >> 8);
  if (x < 0 || idx < 0 || idx >= 80 * 32) return 0;
  return g_rom[(0x17B200 + idx * 2) & 0x3FFFFF] & 7;
}

static uint32_t backdrop_rgb(void) {
  uint16_t c = g_cgram[0];
  int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  return (uint32_t)(((r << 3) | (r >> 2)) << 16) | (uint32_t)(((g << 3) | (g >> 2)) << 8) |
         (uint32_t)((b << 3) | (b >> 2));
}

/* One object pixel as voxels. Props: a puffed-up cut-out standing on the object's foot line
 * (thickness from the distance to the outline). Indoors: furniture boxes and flat walls. Outdoors:
 * houses with a gable roof over their footprint; cliffs, rocks and bare walls as flat-topped
 * masses. From the game's own angle every pixel stays where it was. */
static void emit_object_pixel(int k, int ix, int iy, uint32_t c, int d) {
  int sx = ix - VX_MARGIN, sy = iy + VY0;
  int base = s_base[k], h = base - iy;
  if (s_obj[k] == OBJ_PROP) {
    if (h > TREE_H && (s_flat[k] & 2)) {
      /* a forest: whatever is above the front row of trees is the canopy, a solid mass TREE_H
       * high receding behind it (row = depth - height still holds) */
      push_vox(sx, iy + TREE_H - 1 + VY0, 0, TREE_H, 1, c);
      return;
    }
    if ((s_flat[k] & 4) && s_comp[k]) {
      /* leaves: round like a ball of radius R (the tree's largest distance to its outline) -
       * a pixel d in from the outline is 2*sqrt(2Rd - d*d) through - but at most 3 px of it in
       * front, so from the game's angle the canopy stays on its rows, and behind it no deeper
       * than the rows above it in this column, so its top never shows above the outline. The
       * dark outline stays one voxel, so the tree's sides aren't outline-coloured. */
      int R = s_comp_r[s_comp[k]];
      int r_ = (c >> 16) & 255, g_ = (c >> 8) & 255, b_ = c & 255;
      int dark = (r_ > g_ ? (r_ > b_ ? r_ : b_) : (g_ > b_ ? g_ : b_)) < 64;
      int front = 0, back = 0;
      if (!dark && d > 1 && R > 1) {
        int dd = d > R ? R : d;
        int hc = (int)sqrtf((float)(2 * R * dd - dd * dd));
        int above = (s_rtop[k] ? iy - s_rtop[k] : 0) + 1;
        front = hc < 3 ? hc : 3;
        back = hc < above ? hc : above;
      }
      push_vox_column(ix, sx, base - 1 + VY0 + front, h - 1, h, front + back + 1, c);
      return;
    }
    int t = d <= 1 ? 1 : 1 + 2 * (d - 1 > 4 ? 4 : d - 1); /* signs, posts, fences: up to 9 deep */
    push_vox_column(ix, sx, base - 1 + VY0 + (t - 1) / 2, h - 1, h, t, c);
    return;
  }
  int et = s_otop[k];
  if (!s_outdoor) {
    /* indoors there are no roofs. Furniture is solid tiles (its footprint on the floor) with
     * tiles the game draws over sprites above them: a box as tall as those rows, its front face
     * at the foot and a flat top across the footprint (row = depth - height still holds).
     * Everything that is solid all the way up is a wall: one flat vertical face, full height. */
    int top = s_stop[k], ext = top - et;
    if (ext > 0) {
      if (h <= ext) push_vox_column(ix, sx, base - 1 + VY0, h - 1, h, base - top, c);
      else push_vox(sx, iy + ext + VY0, 0, ext, 1, c);
      return;
    }
    push_vox_column(ix, sx, base - 1 + VY0, h - 1, h, WALL_THICK, c);
    return;
  }
  int top = s_stop[k], ext = top - et, D = base - top;
  int hw = base - et <= HMAX ? base - et : HMAX; /* face height of a mass */
  if (s_flat[k] || ext <= 0 || D < 4) {
    /* cliffs, rocks, and walls with nothing drawn above them: a face up to HMAX high and a flat
     * top receding behind it. The face is one voxel deep and, where nothing tops it, a body in
     * the object's own colour goes behind it, so the sides aren't extruded outline */
    uint32_t mc = (s_wallc[k] >> 24) ? (s_wallc[k] & 0xFFFFFF) : 0;
    if (h <= hw) {
      if (!mc) {
        push_vox_column(ix, sx, base - 1 + VY0, h - 1, h, WALL_THICK, c);
        return;
      }
      push_vox_column(ix, sx, base - 1 + VY0, h - 1, h, 1, c);
      if (h == 1 && base - et <= hw) push_vox(sx, base - 2 + VY0, 0, hw, WALL_THICK - 1, mc);
    } else {
      push_vox(sx, iy + hw - 1 + VY0, 0, hw, 1, c);
    }
    return;
  }
  /* houses: the facade is the solid rows (s_hwall) standing at the foot; the roof's rows
   * (s_roofe) lie on a 45-degree front slope up to the ridge, and a back slope falls from it 2:1
   * - steeper than the camera's line of sight, so from the game's angle nothing shows behind the
   * ridge, as in 2D. A body in the wall colour fills the house, but each column only as tall as
   * its facade is drawn and only as deep as its roof is drawn: where a roof's sloped end leaves
   * grass in the tile, there's no house behind it either. Every pixel keeps its row from the
   * game's angle (row = depth + 1 - height); rows above the roof (a chimney) stand on the ridge. */
  int Hw = s_hwall[k] ? s_hwall[k] : D, Er = s_hwall[k] ? s_roofe[k] : ext;
  uint32_t wc = (s_wallc[k] >> 24) ? (s_wallc[k] & 0xFFFFFF) : c;
  static int dbg = -1; /* tests: EB_DEBUG_HOUSE=1 paints body red, gable fill blue, back slope yellow, chimney magenta */
  if (dbg < 0) dbg = SDL_getenv("EB_DEBUG_HOUSE") != NULL;
  uint32_t c_core = dbg ? 0xE02020 : wc, c_fill = dbg ? 0x2040FF : wc, c_chim = dbg ? 0xFF20FF : c;
  int F = base - 1;
  int run = Er / 2;
  if (run > D - 1) run = D - 1 > 0 ? D - 1 : 0;
  int trow = s_rtop[k] ? s_rtop[k] : et;
  int Hf = base - trow < Hw ? base - trow : Hw;    /* facade height drawn in this column */
  int kmax = F - Hw - trow + 1;                    /* roof rows drawn in this column */
  if (kmax < 0) kmax = 0;
  int full = Er > 0 && kmax >= Er;
  if (h <= Hw) {
    push_vox_column(ix, sx, F + VY0, h - 1, h, 1, c); /* the facade, as drawn */
    if (h == 1) {
      int ridge_dz = Er > 0 ? (Er - 1) - ((Er - 1) * run + Er / 2) / Er : 0; /* height of the ridge above the eaves - 1 */
      int ztop = Hw + 1 + ridge_dz, runb = (ztop - Hw + 1) / 2, len;
      if (full) len = run + 1 + runb;
      else if (kmax > 0) len = ((kmax - 1) * run + Er / 2) / Er + 1;
      else len = WALL_THICK;
      if (len > 1 && Hf > 0) push_vox(sx, F - 1 + VY0, 0, Hf, len - 1, c_core);
      if (full) {
        /* the back slope, in the roof's main colour (the commonest of this column's roof rows
         * that isn't outline), a little darker; each 2-high step shows roof colour on its face */
        uint32_t rc = c;
        {
          uint32_t cs[8];
          int cn[8], nn = 0, bi = -1;
          for (int q = 0; q < Er; q++) {
            int rr = F - Hw - q;
            if (rr < 0 || rr >= VH || !s_o1[rr * VW + ix]) continue;
            uint32_t cc = s_c1[rr * VW + ix];
            int r = (cc >> 16) & 255, g = (cc >> 8) & 255, b = cc & 255;
            if ((r > g ? (r > b ? r : b) : (g > b ? g : b)) < 64) continue;
            int j = 0;
            while (j < nn && cs[j] != cc) j++;
            if (j == nn) {
              if (nn == 8) continue;
              cs[nn] = cc;
              cn[nn++] = 0;
            }
            if (++cn[j] > (bi >= 0 ? cn[bi] : 0)) bi = j;
          }
          if (bi >= 0) rc = cs[bi];
        }
        uint32_t c_back = dbg ? 0xFFE020 : scale_rgb(rc, 13, 16);
        for (int t = 1; t <= runb; t++) {
          int zt = ztop - 2 * t + 1, dpt = F - run - t, zr = zt - 2 > Hw ? zt - 2 : Hw;
          if (zt <= Hw) break;
          if (zr > Hw) push_vox(sx, dpt + VY0, Hw, zr, 1, c_fill);
          push_vox(sx, dpt + VY0, zr, zt, 1, c_back);
        }
      }
    }
    return;
  }
  int kr = F - Hw - iy; /* roof row counted up from the eaves */
  if (kr < Er && Er > 0) {
    int da = (kr * run + Er / 2) / Er, dz = kr - da; /* each row steps back or up, never both */
    int da_prev = kr ? ((kr - 1) * run + Er / 2) / Er : -1;
    int dd = F - da, z = Hw + 1 + dz;
    if (da != da_prev && z - 1 > Hw) push_vox(sx, dd + VY0, Hw, z - 1, 1, c_fill); /* gable fill under the roof */
    push_vox(sx, dd + VY0, z - 1, z, 1, c);
    return;
  }
  {
    int ridge_dz = Er > 0 ? (Er - 1) - ((Er - 1) * run + Er / 2) / Er : 0;
    int z = Hw + 1 + ridge_dz + (kr - Er) + 1;
    push_vox(sx, F - run + VY0, z - 1, z, 3, c_chim);
  }
  (void)sy;
}

/* the commonest colour that isn't dark outline among the block pixels in s_queue[0..n) that lie
 * in the lowest `band` rows of their column (a house's facade, a cliff's face): alpha set, or 0 */
static uint32_t block_colour_rows(int n, int lo, int hi) {
  uint32_t cols[24];
  int cnt[24], nc = 0, cbest = -1;
  for (int i = 0; i < n && hi >= lo; i++) {
    int kk = s_queue[i];
    if (s_obj[kk] != OBJ_BLOCK || !s_o1[kk]) continue;
    int hh = s_base[kk] - kk / VW;
    if (hh < lo || hh > hi) continue;
    uint32_t c = s_c1[kk];
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if ((r > g ? (r > b ? r : b) : (g > b ? g : b)) < 64) continue;
    int j = 0;
    while (j < nc && cols[j] != c) j++;
    if (j == nc) {
      if (nc == 24) continue;
      cols[nc] = c;
      cnt[nc++] = 0;
    }
    if (++cnt[j] > (cbest >= 0 ? cnt[cbest] : 0)) cbest = j;
  }
  return cbest >= 0 ? 0xFF000000u | cols[cbest] : 0;
}
static uint32_t block_colour(int n, int band) { return block_colour_rows(n, 1, band); }

static int colour_dist(uint32_t a, uint32_t b) {
  int dr = (int)((a >> 16) & 255) - (int)((b >> 16) & 255), dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255),
      db = (int)(a & 255) - (int)(b & 255);
  return (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
}

static uint64_t s_tw[8]; /* EB_PROF: time spent in build_world's stages */
static void build_world(void) {
  uint64_t t0 = SDL_GetTicksNS();
  int hofs1 = g_ppu.hofs[0], vofs1 = g_ppu.vofs[0];
  decode_layer(0, hofs1, vofs1, s_c1, s_o1);
  decode_layer(1, g_ppu.hofs[1], g_ppu.vofs[1], s_c2, s_o2);
  uint32_t back = backdrop_rgb();
  for (int iy = 0; iy < VH; iy++) {
    int sy = iy + VY0;
    int cty = ((vofs1 + sy) >> 3) & 63;
    for (int ix = 0; ix < VW; ix++) {
      int sx = ix - VX_MARGIN;
      int ctx = ((hofs1 + sx) >> 3) & 63;
      int k = iy * VW + ix;
      s_coll[k] = g_wram[(SYM_LOADED_COLLISION_TILES + (cty << 6) + ctx) & 0x1FFFF];
      uint32_t c = s_o1[k] ? s_c1[k] : back;
      s_lum[k] = (uint8_t)((((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10);
    }
  }
  {
    long sum = 0;
    for (int k = 0; k < VW * VH; k += 7) sum += s_lum[k];
    s_scene_lum = (float)sum / (float)((VW * VH + 6) / 7);
  }
  {
    /* test aid: EB_DUMP_LAYERS=frame:prefix writes the decoded layers and collision as PNGs */
    const char *dl = SDL_getenv("EB_DUMP_LAYERS");
    if (dl && SDL_atoi(dl) == (int)g_frame_count && SDL_strchr(dl, ':')) {
      static uint32_t img[VW * VH];
      char path[600];
      const char *pre = SDL_strchr(dl, ':') + 1;
      for (int k = 0; k < VW * VH; k++) img[k] = s_o1[k] ? (0xFF000000u | s_c1[k]) : 0xFFFF00FFu;
      SDL_snprintf(path, sizeof path, "%s_bg1.png", pre);
      png_write_bgra(path, (const uint8_t *)img, VW, VH, VW * 4, 0);
      for (int k = 0; k < VW * VH; k++) img[k] = s_o2[k] ? (0xFF000000u | s_c2[k]) : 0xFFFF00FFu;
      SDL_snprintf(path, sizeof path, "%s_bg2.png", pre);
      png_write_bgra(path, (const uint8_t *)img, VW, VH, VW * 4, 0);
      for (int k = 0; k < VW * VH; k++) {
        uint8_t cl = s_coll[k];
        uint32_t c = s_o1[k] ? s_c1[k] : back;
        uint32_t dim = 0xFF000000u | scale_rgb(c, 6, 16);
        img[k] = (cl & 0x80) ? 0xFFFF4040u : (cl & 0x08) ? 0xFF4060FFu : (cl & 0x10) ? 0xFFFFE040u : cl ? 0xFF40FF40u : dim;
        if (s_o2[k] && !(cl & 0x80)) img[k] = (img[k] & 0xFFFEFEFEu) / 2 + 0x40404040u;
      }
      SDL_snprintf(path, sizeof path, "%s_coll.png", pre);
      png_write_bgra(path, (const uint8_t *)img, VW, VH, VW * 4, 0);
      eb_log("dumped layers to %s_*.png (hofs %d vofs %d, sector setting %d)", pre, hofs1, vofs1, sector_setting());
    }
  }
  {
    /* test aid: EB_DUMP_COLL=frame logs the visible screen's solid tiles */
    const char *dc = SDL_getenv("EB_DUMP_COLL");
    if (dc && SDL_atoi(dc) == (int)g_frame_count) {
      eb_log("collision (# solid, ~ water, : other flags, . none) at hofs %d vofs %d:", hofs1, vofs1);
      for (int ty = 0; ty < 28; ty++) {
        char line[80];
        int n = 0;
        for (int tx = 0; tx < 32; tx++) {
          uint8_t c = s_coll[(ty * 8 + 4 - VY0) * VW + (tx * 8 + 4 + VX_MARGIN)];
          line[n++] = (c & 0x80) ? '#' : (c & 0x08) ? '~' : (c ? ':' : '.');
        }
        line[n] = 0;
        eb_log("  %s", line);
      }
      for (int ty = 0; ty < 28; ty++) {
        char line[200];
        int n = 0;
        for (int tx = 0; tx < 32; tx++)
          n += SDL_snprintf(line + n, sizeof line - (size_t)n, "%02x ",
                            s_coll[(ty * 8 + 4 - VY0) * VW + (tx * 8 + 4 + VX_MARGIN)]);
        eb_log("  %s", line);
      }
    }
  }
  int outdoor = s_outdoor;
#define COL1(k) (s_o1[k] ? s_c1[k] : back)

  uint64_t t1 = SDL_GetTicksNS();
  /* 1. objects, per column: a run of solid tiles plus the tiles right above it that the game
   *    draws over sprites (flags 0x01/0x02: tree tops, sign boards, fence tops). The run's lower
   *    edge is where the object stands. Outdoors, short objects with such a top (trees, signs,
   *    fences, posts) are props and stand up as flat cut-outs; everything else (houses, walls,
   *    cliffs, furniture) is extruded as before. */
  memset(s_obj, 0, sizeof s_obj);
  memset(s_gnd, 0, sizeof s_gnd);
  /* solid water (rivers, the sea) stays flat: a solid pixel in a mostly blue neighbourhood */
  static uint8_t solid[VW * VH];
  {
    static int bsat[(VW + 1) * (VH + 1)];
    for (int i = 0; i <= VW; i++) bsat[i] = 0;
    for (int iy = 0; iy < VH; iy++) {
      bsat[(iy + 1) * (VW + 1)] = 0;
      for (int ix = 0; ix < VW; ix++) {
        uint32_t c = COL1(iy * VW + ix);
        int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        /* blue = blue is the strongest channel, and the hue is past ~197 degrees (the sea, 215+)
         * or it is bright (Twoson's cyan river, hue 194, b 255) - not the dark teal that night
         * turns trees and grass into (hue 184-190, b <= 115) */
        int mn = r < g ? r : g;
        int blue = b >= r && b >= g && b - mn >= 40 && ((g - r) * 1000 <= 717 * (b - mn) || b >= 160);
        bsat[(iy + 1) * (VW + 1) + ix + 1] =
            blue + bsat[iy * (VW + 1) + ix + 1] + bsat[(iy + 1) * (VW + 1) + ix] - bsat[iy * (VW + 1) + ix];
      }
    }
    for (int iy = 0; iy < VH; iy++)
      for (int ix = 0; ix < VW; ix++) {
        int k = iy * VW + ix;
        solid[k] = (s_coll[k] & 0x80) != 0;
        if (!solid[k] || !outdoor) continue;
        int x0 = ix - 4 < 0 ? 0 : ix - 4, x1 = ix + 5 > VW ? VW : ix + 5;
        int y0 = iy - 4 < 0 ? 0 : iy - 4, y1 = iy + 5 > VH ? VH : iy + 5;
        int n = (x1 - x0) * (y1 - y0);
        int s = bsat[y1 * (VW + 1) + x1] - bsat[y0 * (VW + 1) + x1] - bsat[y1 * (VW + 1) + x0] + bsat[y0 * (VW + 1) + x0];
        if (s * 10 >= n * 6) solid[k] = 0;
      }
  }
  for (int ix = 0; ix < VW; ix++) {
    int iy = 0;
    while (iy < VH) {
      if (!solid[iy * VW + ix]) {
        iy++;
        continue;
      }
      int top = iy;
      while (iy < VH && solid[iy * VW + ix]) iy++;
      int bot = iy;
      int et = top;
      while (et > 0 && top - et < 64) {
        uint8_t cl = s_coll[(et - 1) * VW + ix];
        if (solid[(et - 1) * VW + ix] || !(cl & 0x03) || s_obj[(et - 1) * VW + ix]) break;
        et--;
      }
      /* outdoors every short object is a cut-out (trees, signs, posts, bushes, rocks) */
      uint8_t kind = (outdoor && bot - top <= HMAX && bot < VH) ? OBJ_PROP : OBJ_BLOCK;
      int nk = bot < VH ? bot * VW + ix : (et > 0 ? (et - 1) * VW + ix : -1);
      uint32_t under = nk >= 0 ? COL1(nk) : back;
      if (kind == OBJ_BLOCK) {
        int fk = et > 0 ? (et - 1) * VW + ix : nk; /* floor seen behind a wall */
        under = scale_rgb(fk >= 0 ? COL1(fk) : back, 12, 16);
      }
      for (int r = et; r < bot; r++) {
        int k = r * VW + ix;
        s_obj[k] = kind;
        s_base[k] = (uint16_t)bot;
        s_otop[k] = (uint16_t)et;
        s_stop[k] = (uint16_t)top;
        s_under[k] = under;
      }
    }
  }
  /* 1b. per connected object: one made mostly of tiles the game draws over sprites (tree tops)
   *     is a tree or a forest, however tall - a cut-out, with a flat canopy behind its front row
   *     (emit_object_pixel), not a tower. Other blocks whose foot is not one straight line are
   *     cliffs and rocks, not houses: flat-topped instead of a pitched roof. */
  memset(s_flat, 0, sizeof s_flat);
  memset(s_hwall, 0, sizeof s_hwall);
  memset(s_roofe, 0, sizeof s_roofe);
  memset(s_wallc, 0, sizeof s_wallc);
  static int no_shape = -1; /* tests: EB_NOSHAPE=1 skips the per-object passes (1b, 3a) to time them */
  if (no_shape < 0) no_shape = SDL_getenv("EB_NOSHAPE") != NULL;
  if (outdoor && !no_shape) {
    static uint8_t seen[VW * VH];
    static int foot_n[VH + 1];
    memset(seen, 0, sizeof seen);
    for (int k0 = 0; k0 < VW * VH; k0++) {
      if (s_obj[k0] != OBJ_BLOCK || seen[k0]) continue;
      int qh = 0, qt = 0, over = 0, blocks = 0, leaf = 0, body = 0;
      s_queue[qt++] = k0;
      seen[k0] = 1;
      while (qh < qt) {
        int k = s_queue[qh++];
        int ix = k % VW, iy = k / VW;
        uint8_t cl = s_coll[k];
        if ((cl & 0x03) && !(cl & 0x80)) over++;
        if (s_obj[k] == OBJ_BLOCK) blocks++;
        if (s_o1[k]) {
          int bd;
          leaf += leaf_colour(s_c1[k], &bd);
          body += bd;
        }
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int xx = ix + dx, yy = iy + dy;
            if (xx < 0 || xx >= VW || yy < 0 || yy >= VH) continue;
            int j = yy * VW + xx;
            if (s_obj[j] != OBJ_BLOCK || seen[j]) continue; /* blocks only: a house next to trees stays a house */
            seen[j] = 1;
            s_queue[qt++] = j;
          }
      }
      /* a forest: largely tiles drawn over sprites AND mostly leaf-coloured (a house has a big
       * roof drawn over sprites too, but it isn't green) */
      {
        const char *dl = SDL_getenv("EB_DUMP_LAYERS");
        if (dl && SDL_atoi(dl) == (int)g_frame_count && qt > 200)
          eb_log("object at (%d,%d): %d px, %d block, %d over-sprite, %d leaf of %d", k0 % VW - VX_MARGIN, k0 / VW + VY0, qt,
                 blocks, over, leaf, body);
      }
      if (blocks && over * 100 >= qt * 35 && body && leaf * 2 >= body) {
        for (int i = 0; i < qt; i++)
          if (s_obj[s_queue[i]] == OBJ_BLOCK) {
            s_obj[s_queue[i]] = OBJ_PROP;
            s_flat[s_queue[i]] = 2;
          }
      } else if (blocks) {
        int best = 0, bestf = 0;
        for (int i = 0; i < qt; i++)
          if (s_obj[s_queue[i]] == OBJ_BLOCK) {
            int f = s_base[s_queue[i]];
            if (++foot_n[f] > best) {
              best = foot_n[f];
              bestf = f;
            }
          }
        for (int i = 0; i < qt; i++) foot_n[s_base[s_queue[i]]] = 0;
        if (best * 2 < blocks) {
          /* a cliff or a rock: flat-topped, its body in its commonest face colour */
          uint32_t mc = block_colour(qt, HMAX);
          for (int i = 0; i < qt; i++)
            if (s_obj[s_queue[i]] == OBJ_BLOCK) {
              s_flat[s_queue[i]] = 1;
              s_wallc[s_queue[i]] = mc;
            }
        } else {
          /* short wall runs right beside a house (no doorstep row under them, a porch post) are
           * part of it, not cut-outs: absorb whole cut-out runs that stand on nearly the same
           * foot line and aren't leaves, then their neighbours in turn */
          for (int i = 0; i < qt; i++) {
            int kk = s_queue[i], ix0 = kk % VW;
            for (int side = -1; side <= 1; side += 2) {
              int jx = ix0 + side, j = kk + side;
              if (jx < 0 || jx >= VW || s_obj[j] != OBJ_PROP || seen[j]) continue;
              int fb = s_base[j];
              if (fb - bestf > 16 || bestf - fb > 16) continue;
              int r0 = s_otop[j], lf = 0, bd = 0;
              for (int r = r0; r < fb; r++) {
                int q = r * VW + jx, b2;
                if (s_obj[q] != OBJ_PROP || !s_o1[q]) continue;
                lf += leaf_colour(s_c1[q], &b2);
                bd += b2;
              }
              if (bd && lf * 10 >= bd * 4) continue; /* 40% or more leaves: a tree */
              for (int r = r0; r < fb; r++) {
                int q = r * VW + jx;
                if (s_obj[q] != OBJ_PROP || seen[q]) continue;
                s_obj[q] = OBJ_BLOCK;
                seen[q] = 1;
                s_queue[qt++] = q;
                blocks++;
              }
            }
          }
          /* a house. Its solid rows are the facade (the player can't walk into it) and the
           * rows drawn over sprites above them are the roof: take the building's usual counts
           * (a chimney or a porch column doesn't reshape it); a part whose solid rows differ a
           * lot (a wing set further back) keeps its own */
          static int d_n[256], e_n[256];
          int d_best = 0, dm = 0, e_best = 0, em = 0;
          for (int i = 0; i < qt; i++) {
            int kk = s_queue[i];
            if (s_obj[kk] != OBJ_BLOCK) continue;
            int dd = s_base[kk] - s_stop[kk], e = s_stop[kk] - s_otop[kk];
            if (dd > 0 && dd < 256 && ++d_n[dd] > d_best) {
              d_best = d_n[dd];
              dm = dd;
            }
            if (e > 0 && e < 256 && ++e_n[e] > e_best) {
              e_best = e_n[e];
              em = e;
            }
          }
          for (int i = 0; i < qt; i++) {
            int kk = s_queue[i];
            int dd = s_base[kk] - s_stop[kk], e = s_stop[kk] - s_otop[kk];
            if (dd > 0 && dd < 256) d_n[dd] = 0;
            if (e > 0 && e < 256) e_n[e] = 0;
          }
          uint32_t wcol = block_colour(qt, dm);
          /* a pitched roof only for a clear house: standing inside the decoded area, with roof
           * rows above its facade in a colour clearly unlike its walls (Ness's purple on cream).
           * Flat-topped buildings (adobe, city blocks: top and walls alike) and anything cut off
           * at the edge are masses with a flat top instead */
          uint32_t rcol = block_colour_rows(qt, dm + 1, dm + em);
          int gable = em >= 8 && dm >= 12 && bestf < VH - 1 && (wcol >> 24) && (rcol >> 24) &&
                      colour_dist(wcol, rcol) >= 120;
          if (!gable) {
            for (int i = 0; i < qt; i++)
              if (s_obj[s_queue[i]] == OBJ_BLOCK) {
                s_flat[s_queue[i]] = 1;
                s_wallc[s_queue[i]] = wcol;
              }
            continue;
          }
          for (int i = 0; i < qt; i++) {
            int kk = s_queue[i];
            if (s_obj[kk] != OBJ_BLOCK) continue;
            int dd = s_base[kk] - s_stop[kk], e = s_stop[kk] - s_otop[kk];
            int own = dd - dm > 8 || dm - dd > 8;
            int fh = own ? dd : dm, re = own ? e : em;
            s_hwall[kk] = (uint8_t)(fh < 1 ? 1 : fh > 250 ? 250 : fh);
            s_roofe[kk] = (uint8_t)(re < 0 ? 0 : re > 250 ? 250 : re);
            s_wallc[kk] = wcol;
          }
          {
            const char *dl = SDL_getenv("EB_DUMP_LAYERS");
            if (dl && SDL_atoi(dl) == (int)g_frame_count)
              eb_log("house at (%d,%d): %d px, facade %d, roof rows %d, wall colour %06X", s_queue[0] % VW - VX_MARGIN,
                     s_queue[0] / VW + VY0, blocks, dm, em, wcol & 0xFFFFFF);
          }
        }
      }
    }
  }
  for (int k = 0; k < VW * VH; k++) s_water[k] = (s_coll[k] & 0x80) && !solid[k];
  {
    /* test aid: EB_DUMP_COL=frame:ix logs one column's classification row by row */
    const char *dc = SDL_getenv("EB_DUMP_COL");
    if (dc && SDL_atoi(dc) == (int)g_frame_count && SDL_strchr(dc, ':')) {
      int cx = SDL_atoi(SDL_strchr(dc, ':') + 1);
      for (int iy = 0; iy < VH && cx >= 0 && cx < VW; iy += 2) {
        int k = iy * VW + cx;
        uint32_t c = COL1(k);
        eb_log("col %d row %3d: coll %02x solid %d water %d obj %d base %d top %d et %d hw %d re %d flat %d rgb %06X", cx, iy,
               s_coll[k], solid[k], s_water[k], s_obj[k], s_base[k], s_stop[k], s_otop[k], s_hwall[k], s_roofe[k], s_flat[k], c);
      }
    }
  }

  uint64_t t2 = SDL_GetTicksNS();
  /* 2. ground showing inside object tiles (the grass around a tree, the pavement at a wall's
   *    foot): spread from open ground into object pixels of exactly the same colour (10 px max),
   *    then hand specks of fewer than 6 pixels back to the ground too */
  {
    int qh = 0, qt = 0;
    for (int iy = 0; iy < VH; iy++)
      for (int ix = 0; ix < VW; ix++) {
        int k = iy * VW + ix;
        if (!s_obj[k]) continue;
        uint32_t c = COL1(k);
        int seed = (ix > 0 && !s_obj[k - 1] && COL1(k - 1) == c) || (ix < VW - 1 && !s_obj[k + 1] && COL1(k + 1) == c) ||
                   (iy > 0 && !s_obj[k - VW] && COL1(k - VW) == c) || (iy < VH - 1 && !s_obj[k + VW] && COL1(k + VW) == c);
        if (seed) {
          s_gnd[k] = 1;
          s_queue[qt++] = k;
        }
      }
    while (qh < qt) {
      int k = s_queue[qh++];
      if (s_gnd[k] >= (s_obj[k] == OBJ_PROP ? 7 : 10)) continue; /* tree tops: just the tile corners */
      int ix = k % VW;
      uint32_t c = COL1(k);
      int nb[4] = {ix > 0 ? k - 1 : -1, ix < VW - 1 ? k + 1 : -1, k >= VW ? k - VW : -1, k + VW < VW * VH ? k + VW : -1};
      for (int i = 0; i < 4; i++) {
        int j = nb[i];
        if (j < 0 || !s_obj[j] || s_gnd[j] || COL1(j) != c) continue;
        s_gnd[j] = (uint8_t)(s_gnd[k] + 1);
        s_queue[qt++] = j;
      }
    }
    /* small leftover specks (grass tufts inside object tiles) */
    static uint8_t seen[VW * VH];
    memset(seen, 0, sizeof seen);
    for (int k0 = 0; k0 < VW * VH; k0++) {
      if (!s_obj[k0] || s_gnd[k0] || seen[k0]) continue;
      int h0 = 0, t0 = 0;
      s_queue[t0++] = k0;
      seen[k0] = 1;
      while (h0 < t0 && t0 < 64) {
        int k = s_queue[h0++];
        int ix = k % VW, iy = k / VW;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int xx = ix + dx, yy = iy + dy;
            if (xx < 0 || xx >= VW || yy < 0 || yy >= VH) continue;
            int j = yy * VW + xx;
            if (!s_obj[j] || s_gnd[j] || seen[j]) continue;
            seen[j] = 1;
            if (t0 < 64) s_queue[t0++] = j;
          }
      }
      if (t0 < 6)
        for (int i = 0; i < t0; i++) s_gnd[s_queue[i]] = 1;
    }
  }

  uint64_t t3 = SDL_GetTicksNS();
  /* 3. the ground is flat (like the GB/GBA voxel looks); solid water (rivers, ponds, the sea)
   *    sits WATER_D lower with banks around it. Behind a cut-out the ground carries on: each row's
   *    run of cut-out pixels takes the nearest ground colour on either side (skipping dark
   *    outlines), so nothing shows a hole. */
  memset(s_hg, 0, sizeof s_hg);
  for (int iy = 0; iy < VH; iy++) {
    int ix = 0;
    while (ix < VW) {
      int k = iy * VW + ix;
      if (s_obj[k] != OBJ_PROP || s_gnd[k]) {
        ix++;
        continue;
      }
      int a = ix;
      while (ix < VW && s_obj[iy * VW + ix] == OBJ_PROP && !s_gnd[iy * VW + ix]) ix++;
      int b = ix; /* run a..b-1 */
#define IS_GROUND(x) (!s_obj[iy * VW + (x)] || s_gnd[iy * VW + (x)])
      int L = a - 1, R = b;
      while (L > 0 && a - L < 4 && IS_GROUND(L) && s_lum[iy * VW + L] < 60 && IS_GROUND(L - 1)) L--;
      while (R < VW - 1 && R - b < 3 && IS_GROUND(R) && s_lum[iy * VW + R] < 60 && IS_GROUND(R + 1)) R++;
      int okL = L >= 0 && IS_GROUND(L), okR = R < VW && IS_GROUND(R);
#undef IS_GROUND
      for (int x = a; x < b; x++) {
        int src = -1;
        if (okL && (!okR || x - L <= R - x)) src = iy * VW + L;
        else if (okR) src = iy * VW + R;
        if (src >= 0) s_under[iy * VW + x] = COL1(src);
      }
    }
  }

  /* 3a. cut-outs that are mostly leaf-coloured (hue 55-192: greens, olive, the teal of trees at
   *     night; not the grey/white/brown of signs, fences and posts) are puffed rounder */
  int ncomp = 0;
  memset(s_comp, 0, sizeof s_comp);
  if (!no_shape) {
    static uint8_t seen[VW * VH];
    memset(seen, 0, sizeof seen);
    for (int k0 = 0; k0 < VW * VH; k0++) {
      if (s_obj[k0] != OBJ_PROP || s_gnd[k0] || seen[k0]) continue;
      int qh = 0, qt = 0, leaf = 0, body = 0;
      s_queue[qt++] = k0;
      seen[k0] = 1;
      while (qh < qt) {
        int k = s_queue[qh++];
        int ix = k % VW, iy = k / VW;
        int bd;
        leaf += leaf_colour(COL1(k), &bd);
        body += bd;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int xx = ix + dx, yy = iy + dy;
            if (xx < 0 || xx >= VW || yy < 0 || yy >= VH) continue;
            int j = yy * VW + xx;
            if (s_obj[j] != OBJ_PROP || s_gnd[j] || seen[j]) continue;
            seen[j] = 1;
            s_queue[qt++] = j;
          }
      }
      if (body && leaf * 2 >= body) {
        int id = ncomp < 4095 ? ++ncomp : 0;
        if (id) s_comp_r[id] = 0;
        for (int i = 0; i < qt; i++) {
          s_flat[s_queue[i]] |= 4;
          s_comp[s_queue[i]] = (uint16_t)id;
        }
      }
    }
  }

  /* 3b. props are puffed up from their outline: thickness grows with the distance to the
   *     silhouette's edge (1 px at the rim, up to 9 in the middle, 15 for leaves), so trees
   *     come out round */
  static uint8_t dist[VW * VH];
  for (int k = 0; k < VW * VH; k++) dist[k] = (s_obj[k] == OBJ_PROP && !s_gnd[k] && s_o1[k]) ? 255 : 0;
  for (int iy = 0; iy < VH; iy++)
    for (int ix = 0; ix < VW; ix++) {
      int k = iy * VW + ix;
      if (!dist[k]) continue;
      int d = 255;
      if (ix > 0 && dist[k - 1] < d) d = dist[k - 1];
      if (iy > 0) {
        if (dist[k - VW] < d) d = dist[k - VW];
        if (ix > 0 && dist[k - VW - 1] < d) d = dist[k - VW - 1];
        if (ix < VW - 1 && dist[k - VW + 1] < d) d = dist[k - VW + 1];
      } else {
        d = 0;
      }
      if (ix == 0) d = 0;
      if (d + 1 < dist[k]) dist[k] = (uint8_t)(d + 1);
    }
  for (int iy = VH - 1; iy >= 0; iy--)
    for (int ix = VW - 1; ix >= 0; ix--) {
      int k = iy * VW + ix;
      if (!dist[k]) continue;
      int d = 255;
      if (ix < VW - 1 && dist[k + 1] < d) d = dist[k + 1];
      if (iy < VH - 1) {
        if (dist[k + VW] < d) d = dist[k + VW];
        if (ix < VW - 1 && dist[k + VW + 1] < d) d = dist[k + VW + 1];
        if (ix > 0 && dist[k + VW - 1] < d) d = dist[k + VW - 1];
      } else {
        d = 0;
      }
      if (ix == VW - 1) d = 0;
      if (d + 1 < dist[k]) dist[k] = (uint8_t)(d + 1);
    }

  {
    /* test aid: with EB_DUMP_LAYERS, also write how each pixel was classified: green = cut-out,
     * cyan = cut-out pixel handed to the ground, red = block, magenta = block pixel handed to the
     * ground, orange = flat-topped mass (cliff/rock), blue = sunken water, grey = ground */
    const char *dl = SDL_getenv("EB_DUMP_LAYERS");
    if (dl && SDL_atoi(dl) == (int)g_frame_count && SDL_strchr(dl, ':')) {
      static uint32_t img[VW * VH];
      char path[600];
      for (int k = 0; k < VW * VH; k++) {
        uint32_t c = 0xFF000000u | scale_rgb(s_o1[k] ? s_c1[k] : back, 5, 16);
        if (s_obj[k] == OBJ_PROP) c = s_gnd[k] ? 0xFF00C0C0u : 0xFF20E020u;
        else if (s_obj[k] == OBJ_BLOCK) c = s_gnd[k] ? 0xFFC000C0u : s_flat[k] ? 0xFFFF9020u : 0xFFE02020u;
        else if (s_water[k]) c = 0xFF2040FFu;
        img[k] = c;
      }
      SDL_snprintf(path, sizeof path, "%s_class.png", SDL_strchr(dl, ':') + 1);
      png_write_bgra(path, (const uint8_t *)img, VW, VH, VW * 4, 0);
    }
  }

  for (int k = 0; k < VW * VH; k++)
    if (s_comp[k] && dist[k] > s_comp_r[s_comp[k]]) s_comp_r[s_comp[k]] = dist[k] > 16 ? 16 : dist[k];

  /* 3c. every object column run's topmost row that is actually drawn (not handed to the ground) */
  for (int ix = 0; ix < VW; ix++) {
    int iy = 0;
    while (iy < VH) {
      int k = iy * VW + ix;
      if (!s_obj[k]) {
        iy++;
        continue;
      }
      int b = s_base[k], kind = s_obj[k], r = iy, t = -1;
      for (; r < VH; r++) {
        int q = r * VW + ix;
        if (s_obj[q] != kind || s_base[q] != b) break;
        if (t < 0 && !s_gnd[q] && s_o1[q]) t = r;
      }
      for (int q = iy; q < r; q++) s_rtop[q * VW + ix] = (uint16_t)(t < 0 ? b : t);
      iy = r;
    }
  }

  uint64_t t4 = SDL_GetTicksNS();
  /* 4. voxels */
  s_nvox = 0;
  for (int iy = 0; iy < VH; iy++) {
    int sy = iy + VY0;
    for (int ix = 0; ix < VW; ix++) {
      int k = iy * VW + ix;
      int sx = ix - VX_MARGIN;
      uint32_t c = COL1(k);
      if (!s_obj[k] || s_gnd[k]) {
        if (s_water[k]) {
          s_ground[k] = 0x01000000u | c; /* alpha marks the sunken water layer */
          continue;
        }
        /* soft contact shading just south of an object's foot */
        int shade = 16;
        for (int d = 1; d <= 3; d++) {
          int r = iy - d;
          if (r >= 0 && s_obj[r * VW + ix] && !s_gnd[r * VW + ix]) {
            shade = 12 + d;
            break;
          }
        }
        if (shade < 16) c = scale_rgb(c, shade, 16);
        s_ground[k] = 0xFF000000u | c;
        /* bank down to the water */
        if ((ix > 0 && s_water[k - 1]) || (ix < VW - 1 && s_water[k + 1]) || (iy > 0 && s_water[k - VW]) ||
            (iy < VH - 1 && s_water[k + VW]))
          push_vox(sx, sy, -WATER_D, 0, 1, scale_rgb(c, 11, 16));
        continue;
      }
      /* indoors, solid areas with nothing drawn (the black around a room) stay black, not a
       * dimmed copy of the floor */
      s_ground[k] = 0xFF000000u | ((!outdoor && !s_o1[k]) ? back : s_under[k]);
      if (s_o1[k]) emit_object_pixel(k, ix, iy, c, dist[k]);
    }
  }
  for (int ix = 0; ix < VW; ix++) flush_column(ix);
  /* BG2 is mostly the game's copy of BG1's upper parts (drawn over sprites): only pixels that
   * differ from BG1 add anything */
  for (int iy = 0; iy < VH; iy++) {
    int sy = iy + VY0;
    for (int ix = 0; ix < VW; ix++) {
      int k = iy * VW + ix;
      if (!s_o2[k] || (s_o1[k] && s_c1[k] == s_c2[k])) continue;
      int sx = ix - VX_MARGIN;
      if (s_obj[k]) emit_object_pixel(k, ix, iy, s_c2[k], 1);
      else push_vox(sx, sy + HFG - 1, HFG - 3, HFG, 3, s_c2[k]);
    }
  }
  for (int ix = 0; ix < VW; ix++) flush_column(ix);
  uint64_t t5 = SDL_GetTicksNS();
  s_tw[0] += t1 - t0;
  s_tw[1] += t2 - t1;
  s_tw[2] += t3 - t2;
  s_tw[3] += t4 - t3;
  s_tw[4] += t5 - t4;
  s_tw[5]++;
  if (SDL_getenv("EB_PROF") && s_tw[5] == 60) {
    eb_log("3D world stages (ms/frame): decode %.2f, objects %.2f, ground-in-objects %.2f, shapes %.2f, voxels %.2f",
           s_tw[0] / 6e7, s_tw[1] / 6e7, s_tw[2] / 6e7, s_tw[3] / 6e7, s_tw[4] / 6e7);
    memset(s_tw, 0, sizeof s_tw);
  }
#undef COL1
}

/* Side faces fully covered by the neighbouring column (both standing on the ground, the
 * neighbour at least as tall) are never seen: mark them in Voxel.pad so the shaders skip them.
 * Roofs and big blocks are rows of thin columns, so this removes most of their overdraw. */
#define MAP_D0 (VY0 - 8)
#define MAP_DH (VH + HMAX + 96)
static int16_t s_colmax[VW * MAP_DH];
static void compute_face_masks(void) {
  for (int i = 0; i < VW * MAP_DH; i++) s_colmax[i] = -32768;
  for (int i = 0; i < s_nvox; i++) {
    Voxel *v = &s_vox[i];
    v->pad = 0;
    if (v->len != 1 || v->z0 > 0) continue;
    int ix = v->x + VX_MARGIN, d = v->dep - MAP_D0;
    if (ix < 0 || ix >= VW || d < 0 || d >= MAP_DH) continue;
    if (v->z1 > s_colmax[d * VW + ix]) s_colmax[d * VW + ix] = v->z1;
  }
  for (int i = 0; i < s_nvox; i++) {
    Voxel *v = &s_vox[i];
    if (v->len != 1 || v->z0 < 0) continue;
    int ix = v->x + VX_MARGIN, d = v->dep - MAP_D0;
    if (ix < 1 || ix >= VW - 1 || d < 1 || d >= MAP_DH - 1) continue;
    int m = 0;
    if (s_colmax[(d + 1) * VW + ix] >= v->z1) m |= 1;
    if (s_colmax[(d - 1) * VW + ix] >= v->z1) m |= 2;
    if (s_colmax[d * VW + ix + 1] >= v->z1) m |= 4;
    if (s_colmax[d * VW + ix - 1] >= v->z1) m |= 8;
    v->pad = (int16_t)m;
  }
}

/* ---- sprites ------------------------------------------------------------------------------------- */
static inline uint16_t rom16(uint32_t a) {
  uint32_t off = ((a >> 16) & 0x3F) << 16 | (a & 0xFFFF);
  return (uint16_t)(g_rom[off & 0x3FFFFF] | g_rom[(off + 1) & 0x3FFFFF] << 8);
}
static inline uint8_t rom8(uint32_t a) {
  uint32_t off = ((a >> 16) & 0x3F) << 16 | (a & 0xFFFF);
  return g_rom[off & 0x3FFFFF];
}

/* The entity's sprite as the game would upload it for direction `dir` (0 up .. 7 up-left), in the
 * animation frame it shows now, straight from its sprite group in ROM (the same lookup as
 * UPDATE_ENTITY_SPRITE_OFFSET: 4-direction mapping, 4 bytes per direction, +2 for frame 2, bit 0
 * = mirrored, and the deep-water sink of one or two tile rows). Writes w x h pixels into dst
 * (row pitch ATLAS_W). Returns 0 if the entity has no ordinary sprite group. */
static int decode_entity_sprite(int slot, int dir, int pal, uint32_t *dst, int *w_out, int *h_out) {
  /* SPRITE_DIRECTION_MAPPING_4_DIRECTION for everyone else; the party (slots 24-29, updated by
   * UNKNOWN_C0A794) uses SPRITE_DIRECTION_MAPPING_8_DIRECTION and the diagonal sprites after the
   * first eight (SPRITES2) */
  static const uint8_t map4[8] = {0, 0, 1, 2, 2, 2, 3, 0}, map8[8] = {0, 4, 1, 5, 2, 6, 3, 7};
  const uint8_t *map = slot >= 24 ? map8 : map4;
  uint32_t gptr = ram16(SYM_ENTITY_GRAPHICS_PTR_LOW + 2 * slot) |
                  ((uint32_t)(ram16(SYM_ENTITY_GRAPHICS_PTR_HIGH + 2 * slot) & 0xFF) << 16);
  int bank = ram16(SYM_ENTITY_GRAPHICS_SPRITE_BANK + 2 * slot) & 0xFF;
  int rowb = ram16(SYM_ENTITY_BYTE_WIDTHS + 2 * slot), rows = ram16(SYM_ENTITY_TILE_HEIGHTS + 2 * slot);
  int cur = ram16(SYM_ENTITY_DIRECTIONS + 2 * slot);
  if (!(gptr >> 16) || rowb < 32 || rowb > 256 || (rowb & 31) || rows < 1 || rows > 8 || cur > 7 || dir > 7) return 0;
  int tw = rowb / 32;
  if (tw * 8 > 64 || rows * 8 > 64) return 0;
  uint16_t shown = ram16(SYM_ENTITY_CURRENT_DISPLAYED_SPRITES + 2 * slot);
  uint32_t mine = gptr + (uint32_t)map[cur] * 4;
  int frame;
  if (rom16(mine) == shown) frame = 0;
  else if (rom16(mine + 2) == shown) frame = 1;
  else return 0; /* a pose that is not one of its walking frames: keep the game's own picture */
  uint16_t wd = rom16(gptr + (uint32_t)map[dir] * 4 + (uint32_t)frame * 2);
  uint32_t src = ((uint32_t)bank << 16) | (wd & 0xFFF0);
  int flip = wd & 1;
  int sink = 0;
  uint16_t sf = ram16(SYM_ENTITY_SURFACE_FLAGS + 2 * slot);
  if (!(wd & 2) && (sf & 8)) sink = (sf & 4) ? 2 : 1;
  int W = tw * 8, H = rows * 8;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) dst[y * ATLAS_W + x] = 0;
  for (int r = 0; r + sink < rows; r++) {
    for (int t = 0; t < tw; t++) {
      uint32_t ta = src + (uint32_t)(r * rowb + t * 32);
      for (int py = 0; py < 8; py++) {
        uint8_t p0 = rom8(ta + py * 2), p1 = rom8(ta + py * 2 + 1), p2 = rom8(ta + 16 + py * 2), p3 = rom8(ta + 17 + py * 2);
        for (int px = 0; px < 8; px++) {
          int b = 7 - px;
          int ci = ((p0 >> b) & 1) | ((p1 >> b) & 1) << 1 | ((p2 >> b) & 1) << 2 | ((p3 >> b) & 1) << 3;
          if (!ci) continue;
          uint16_t c = g_cgram[128 + pal * 16 + ci];
          int R = c & 31, G = (c >> 5) & 31, B = (c >> 10) & 31;
          uint32_t argb = 0xFF000000u | (uint32_t)((R << 3) | (R >> 2)) << 16 | (uint32_t)((G << 3) | (G >> 2)) << 8 |
                          (uint32_t)((B << 3) | (B >> 2));
          int x = t * 8 + px, y = (r + sink) * 8 + py;
          if (flip) x = W - 1 - x;
          dst[y * ATLAS_W + x] = argb;
        }
      }
    }
  }
  *w_out = W;
  *h_out = H;
  return 1;
}

/* one card: x0..x1 across (relative to the anchor), ztop..zbot up, turned by (rx, ry); its picture
 * stands along `up` (the camera's up vector for a billboard, straight up for the sun's shadow map)
 * and the upright twin of every corner goes along for depth and shadows */
static void emit_card(CardVert *o, float ax, float dep, float x0, float x1, float ztop, float zbot, float u0, float u1,
                      float v0, float v1, float rx, float ry, const float *up) {
  CardVert c[4];
  float xs[2] = {x0, x1};
  /* a billboard sorts a few px toward the camera, like the game drawing sprites over the
   * background: Ness on a doorstep stays in front of the door, a house in front still hides him */
  float bx = 0.0f, by = 0.0f, hl = sqrtf(up[0] * up[0] + up[1] * up[1]);
  if (hl > 1e-4f) {
    bx = -up[0] / hl * SPRITE_DEPTH_BIAS;
    by = -up[1] / hl * SPRITE_DEPTH_BIAS;
  }
  for (int k = 0; k < 4; k++) {
    float ox = xs[k & 1];
    float z = (k >> 1) ? zbot : ztop;
    c[k].ux = ax + ox * rx + bx;
    c[k].uy = -dep + ox * ry + by;
    c[k].uz = z;
    c[k].x = ax + ox * rx + z * up[0];
    c[k].y = -dep + ox * ry + z * up[1];
    c[k].z = z * up[2];
    c[k].u = (k & 1) ? u1 : u0;
    c[k].v = (k >> 1) ? v1 : v0;
    c[k].sx = ax + ox;
    c[k].sy = dep - z;
  }
  o[0] = c[0];
  o[1] = c[1];
  o[2] = c[3];
  o[3] = c[0];
  o[4] = c[3];
  o[5] = c[2];
}

/* The game's sprites as billboards that always face the camera, pixel perfect. Pieces of one
 * entity stand on the entity's feet (the lowest edge of its pieces). Once the camera is turned
 * 22.5 degrees or more, entities show the direction they are seen from in 45-degree steps, like
 * a 2D game would (their sprite for that side comes from ROM; 8 directions for the party). A
 * second set of upright cards faces the sun for the shadow map. */
static void build_sprites(float right_x, float right_y, const float *cam_up, int oct, float sun_rx, float sun_ry) {
  static const float upright[3] = {0.0f, 0.0f, 1.0f};
  s_ncardv = 0;
  s_ncardv_l = 0;
  static int px_[128], py_[128], pw_[128], ph_[128], ok_[128];
  int feet_of[128];
  for (int i = 0; i < 128; i++) {
    int prio;
    ok_[i] = ppu_obj_piece(i, VX_MARGIN, &px_[i], &py_[i], &pw_[i], &ph_[i], &prio, s_piece);
    if (!ok_[i]) continue;
    int slot_x = (i & 15) * 64, slot_y = (i >> 4) * 64;
    for (int r = 0; r < ph_[i]; r++)
      memcpy(&s_atlas[(slot_y + r) * ATLAS_W + slot_x], &s_piece[r * 64], (size_t)pw_[i] * 4);
  }
  for (int i = 0; i < 128; i++) {
    feet_of[i] = ok_[i] ? py_[i] + ph_[i] : 0;
    if (!ok_[i] || !g_obj_frame.valid[i]) continue;
    for (int j = 0; j < 128; j++)
      if (j != i && ok_[j] && g_obj_frame.valid[j] && g_obj_frame.group[j] == g_obj_frame.group[i] &&
          py_[j] + ph_[j] > feet_of[i])
        feet_of[i] = py_[j] + ph_[j];
  }
  /* camera-relative sprites: one card per entity, replacing its pieces */
  static uint8_t replaced[128];
  memset(replaced, 0, sizeof replaced);
  if (oct) {
    for (int i = 0; i < 128; i++) {
      if (!ok_[i] || !g_obj_frame.valid[i] || replaced[i] || g_obj_frame.slot[i] < 0) continue;
      int slot = g_obj_frame.slot[i];
      int dir = ram16(SYM_ENTITY_DIRECTIONS + 2 * slot);
      if (dir > 7) continue;
      int view = (dir + oct) & 7;
      int pal = (g_oam[i * 4 + 3] >> 1) & 7;
      int slot_x = (i & 15) * 64, slot_y = (i >> 4) * 64;
      int w, h;
      if (!decode_entity_sprite(slot, view, pal, &s_atlas[slot_y * ATLAS_W + slot_x], &w, &h)) continue;
      for (int j = 0; j < 128; j++)
        if (ok_[j] && g_obj_frame.valid[j] && g_obj_frame.group[j] == g_obj_frame.group[i]) replaced[j] = 1;
      float ax = g_obj_frame.group_x[i], ay = (float)feet_of[i];
      float u0 = (float)slot_x / ATLAS_W, u1 = (float)(slot_x + w) / ATLAS_W;
      float v0 = (float)slot_y / ATLAS_H, v1 = (float)(slot_y + h) / ATLAS_H;
      if (s_ncardv + 6 <= (int)(sizeof s_cards / sizeof s_cards[0])) {
        emit_card(&s_cards[s_ncardv], ax, ay + 0.4f, -w * 0.5f, w * 0.5f, (float)h, 0.0f, u0, u1, v0, v1, right_x, right_y,
                  cam_up);
        s_ncardv += 6;
        emit_card(&s_cards_l[s_ncardv_l], ax, ay + 0.4f, -w * 0.5f, w * 0.5f, (float)h, 0.0f, u0, u1, v0, v1, sun_rx, sun_ry,
                  upright);
        s_ncardv_l += 6;
      }
    }
  }
  for (int i = 0; i < 128; i++) {
    if (!ok_[i] || replaced[i]) continue;
    int x = px_[i], y = py_[i], w = pw_[i], h = ph_[i];
    int slot_x = (i & 15) * 64, slot_y = (i >> 4) * 64;
    float ax, ay;
    if (g_obj_frame.valid[i]) {
      ax = g_obj_frame.group_x[i];
      ay = (float)feet_of[i];
    } else {
      ax = x + w * 0.5f;
      ay = (float)(y + h);
    }
    float u0 = (float)slot_x / ATLAS_W, u1 = (float)(slot_x + w) / ATLAS_W;
    float v0 = (float)slot_y / ATLAS_H, v1 = (float)(slot_y + h) / ATLAS_H;
    float ztop = ay - y, zbot = ay - (y + h);
    if (s_ncardv + 6 > (int)(sizeof s_cards / sizeof s_cards[0])) break;
    emit_card(&s_cards[s_ncardv], ax, ay + 0.4f, (float)x - ax, (float)(x + w) - ax, ztop, zbot, u0, u1, v0, v1, right_x,
              right_y, cam_up);
    s_ncardv += 6;
    emit_card(&s_cards_l[s_ncardv_l], ax, ay + 0.4f, (float)x - ax, (float)(x + w) - ax, ztop, zbot, u0, u1, v0, v1,
              sun_rx, sun_ry, upright);
    s_ncardv_l += 6;
  }
}

/* ---- matrices ---------------------------------------------------------------------------------------- */
static void mat_mul(float *o, const float *a, const float *b) {
  float r[16];
  for (int c = 0; c < 4; c++)
    for (int rr = 0; rr < 4; rr++)
      r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] + a[2 * 4 + rr] * b[c * 4 + 2] +
                      a[3 * 4 + rr] * b[c * 4 + 3];
  memcpy(o, r, sizeof r);
}

static void look_at(float *m, const float *eye, const float *ctr) {
  float f[3] = {ctr[0] - eye[0], ctr[1] - eye[1], ctr[2] - eye[2]};
  float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  for (int i = 0; i < 3; i++) f[i] /= fl;
  float up[3] = {0, 0, 1};
  float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
  float sl = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
  for (int i = 0; i < 3; i++) s[i] /= sl;
  float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
  memset(m, 0, 16 * sizeof(float));
  m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
  m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
  m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
  m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
  m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
  m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
  m[15] = 1;
}

static void look_at_up(float *m, const float *eye, const float *ctr, const float *up) {
  float f[3] = {ctr[0] - eye[0], ctr[1] - eye[1], ctr[2] - eye[2]};
  float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  for (int i = 0; i < 3; i++) f[i] /= fl;
  float sv[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
  float sl = sqrtf(sv[0] * sv[0] + sv[1] * sv[1] + sv[2] * sv[2]);
  for (int i = 0; i < 3; i++) sv[i] /= sl;
  float u[3] = {sv[1] * f[2] - sv[2] * f[1], sv[2] * f[0] - sv[0] * f[2], sv[0] * f[1] - sv[1] * f[0]};
  memset(m, 0, 16 * sizeof(float));
  m[0] = sv[0]; m[4] = sv[1]; m[8] = sv[2];
  m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
  m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
  m[12] = -(sv[0] * eye[0] + sv[1] * eye[1] + sv[2] * eye[2]);
  m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
  m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
  m[15] = 1;
}

static void ortho(float *m, float l, float r, float b, float t, float n, float f) {
  memset(m, 0, 16 * sizeof(float));
  m[0] = 2.0f / (r - l);
  m[5] = 2.0f / (t - b);
  m[10] = -2.0f / (f - n);
  m[12] = -(r + l) / (r - l);
  m[13] = -(t + b) / (t - b);
  m[14] = -(f + n) / (f - n);
  m[15] = 1.0f;
}

static void perspective(float *m, float fovy_deg, float aspect, float zn, float zf) {
  float t = 1.0f / tanf(fovy_deg * 3.14159265f / 360.0f);
  memset(m, 0, 16 * sizeof(float));
  m[0] = t / aspect;
  m[5] = t;
  m[10] = (zf + zn) / (zn - zf);
  m[11] = -1;
  m[14] = 2 * zf * zn / (zn - zf);
}

static float s_sdark = 0.58f;
static void set_common(GLuint p, const float *mvp, float bright, uint32_t clear) {
  gl.UseProgram(p);
  gl.UniformMatrix4fv(gl.GetUniformLocation(p, "u_mvp"), 1, GL_FALSE, mvp);
  gl.UniformMatrix4fv(gl.GetUniformLocation(p, "u_lmvp"), 1, GL_FALSE, s_lmvp);
  gl.Uniform4f(gl.GetUniformLocation(p, "u_valid"), (float)(-VX_MARGIN + 2), (float)(VY0 + 2),
               (float)(256 + VX_MARGIN - 2), (float)(VY0 + VH - 2));
  gl.Uniform3f(gl.GetUniformLocation(p, "u_clear"), ((clear >> 16) & 255) / 255.0f, ((clear >> 8) & 255) / 255.0f,
               (clear & 255) / 255.0f);
  gl.Uniform1f(gl.GetUniformLocation(p, "u_bright"), bright);
  gl.Uniform1i(gl.GetUniformLocation(p, "u_shadow"), 1);
  gl.Uniform2f(gl.GetUniformLocation(p, "u_stexel"), 1.0f / SHADOW_RES, 1.0f / SHADOW_RES);
  gl.Uniform1f(gl.GetUniformLocation(p, "u_sdark"), s_sdark);
}

/* ---- the frame --------------------------------------------------------------------------------------- */
int render3d_frame(SDL_Window *win, unsigned target_fbo, int ext) {
  if (!s_ready || !g_cfg.voxel3d) return 0;
  if (game_scene() != SCENE_OVERWORLD) return 0;
  if (ram16(SYM_BATTLE_SWIRL_COUNTDOWN)) return 0;
  if (g_ppu.inidisp & 0x80) return 0;

  uint64_t prof_t0 = SDL_GetTicksNS();
  poll_camera_keys();
  s_setting = sector_setting();
  s_outdoor = s_setting == 0 || s_setting == 3 || s_setting == 4 || s_setting == 6 || s_setting == 7;
  build_world();
  compute_face_masks();
  uint64_t prof_t1 = SDL_GetTicksNS();
  g_render3d_voxels = s_nvox;
  float yaw = s_yaw * 3.14159265f / 180.0f, pitch_r = s_pitch * 3.14159265f / 180.0f;
  /* camera turned by 22.5 degrees or more: entities show the side they are seen from */
  int oct = ((int)floorf((s_yaw + 22.5f) / 45.0f)) & 7;
  /* the camera's up vector (sprites face the camera like billboards) */
  float cam_up[3] = {-sinf(yaw) * sinf(pitch_r), cosf(yaw) * sinf(pitch_r), cosf(pitch_r)};
  /* the sun: high in the north-west, so shadows fall to the south-east (lower right) */
  float sun[3] = {-0.55f, 0.45f, 1.0f};
  {
    float l = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    for (int i = 0; i < 3; i++) sun[i] /= l;
  }
  float sun_h = sqrtf(sun[0] * sun[0] + sun[1] * sun[1]);
  build_sprites(cosf(yaw), sinf(yaw), cam_up, oct, -sun[1] / sun_h, sun[0] / sun_h);
  {
    float ctr[3] = {128.0f, -112.0f, 0.0f}, leye[3], up[3] = {0.0f, 1.0f, 0.0f}, lview[16], lproj[16];
    for (int i = 0; i < 3; i++) leye[i] = ctr[i] + sun[i] * 800.0f;
    look_at_up(lview, leye, ctr, up);
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    for (int c = 0; c < 8; c++) {
      float p[3] = {(c & 1) ? 256.0f + VX_MARGIN + 8 : -VX_MARGIN - 8.0f, (c & 2) ? -VY0 + 8.0f : -(VY0 + VH) - 8.0f,
                    (c & 4) ? HMAX + 64.0f : -WATER_D - 2.0f};
      for (int r = 0; r < 3; r++) {
        float v = lview[0 * 4 + r] * p[0] + lview[1 * 4 + r] * p[1] + lview[2 * 4 + r] * p[2] + lview[3 * 4 + r];
        if (v < mn[r]) mn[r] = v;
        if (v > mx[r]) mx[r] = v;
      }
    }
    ortho(lproj, mn[0], mx[0], mn[1], mx[1], -mx[2] - 4.0f, -mn[2] + 4.0f);
    mat_mul(s_lmvp, lproj, lview);
  }
  uint64_t prof_t2 = SDL_GetTicksNS();
  ppu_render_layers(s_overlay, PPU_MAX_W, ext, 0x04);

  int ww, wh;
  SDL_GetWindowSizeInPixels(win, &ww, &wh);
  if (ww < 1 || wh < 1) return 1;
  ensure_fbo(ww, wh);
  float aspect = (float)ww / (float)wh;
  const float fovy = 30.0f;
  /* frame ~300 px of the map across the window at the default angle */
  float dist = 300.0f / (2.0f * tanf(fovy * 3.14159265f / 360.0f) * aspect) * s_zoom;
  if (dist < 90.0f) dist = 90.0f;
  float tgt[3] = {128.0f, -112.0f, 8.0f};
  float eye[3] = {tgt[0] + dist * sinf(yaw) * cosf(pitch_r), tgt[1] - dist * cosf(yaw) * cosf(pitch_r),
                  tgt[2] + dist * sinf(pitch_r)};
  float view[16], proj[16], mvp[16];
  look_at(view, eye, tgt);
  perspective(proj, fovy, aspect, 10.0f, 4000.0f);
  mat_mul(mvp, proj, view);

  float bright = (g_ppu.inidisp & 15) / 15.0f;
  uint32_t clear = backdrop_rgb();
  /* sky: blue outdoors (pink in Magicant), dimmed with the map's own brightness so nights and
   * dark places get a dark sky; indoors and in caves the surroundings stay the game's black */
  float top[3], hor[3];
  int setting = s_setting;
  if (s_outdoor) {
    static const float day_top[3] = {0.30f, 0.56f, 0.93f}, day_hor[3] = {0.74f, 0.87f, 0.98f};
    static const float mag_top[3] = {0.78f, 0.52f, 0.86f}, mag_hor[3] = {1.00f, 0.84f, 0.93f};
    const float *t = setting == 4 ? mag_top : day_top, *hz = setting == 4 ? mag_hor : day_hor;
    float k = s_scene_lum / 125.0f;
    if (k > 1.0f) k = 1.0f;
    if (k < 0.12f) k = 0.12f;
    for (int c = 0; c < 3; c++) {
      top[c] = t[c] * k;
      hor[c] = hz[c] * k;
    }
    clear = (uint32_t)(hor[0] * 255.0f) << 16 | (uint32_t)(hor[1] * 255.0f) << 8 | (uint32_t)(hor[2] * 255.0f);
  } else {
    for (int c = 0; c < 3; c++) top[c] = hor[c] = ((clear >> (16 - 8 * c)) & 255) / 255.0f;
  }
  /* shadows are softer indoors and when the map is dark (night) */
  {
    float k = s_scene_lum / 125.0f;
    if (k > 1.0f) k = 1.0f;
    s_sdark = s_outdoor ? 0.86f - 0.30f * k : 0.80f;
  }
  /* upload this frame's voxels and sprites once */
  gl.BindVertexArray(s_vao_vox);
  gl.BindBuffer(GL_ARRAY_BUFFER, s_vbo_vox);
  gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_nvox ? s_nvox : 1) * (GLsizeiptr)sizeof(Voxel), s_vox, GL_STREAM_DRAW);
  gl.ActiveTexture(GL_TEXTURE0);
  gl.BindTexture(GL_TEXTURE_2D, s_tex_atlas);
  gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ATLAS_W, ATLAS_H, GL_BGRA, GL_UNSIGNED_BYTE, s_atlas);

  /* the sun's shadow map */
  gl.BindFramebuffer(GL_FRAMEBUFFER, s_shadow_fbo);
  gl.Viewport(0, 0, SHADOW_RES, SHADOW_RES);
  gl.Enable(GL_DEPTH_TEST);
  gl.DepthFunc(GL_LEQUAL);
  gl.DepthMask(GL_TRUE);
  gl.Disable(GL_BLEND);
  gl.Disable(GL_CULL_FACE);
  gl.Clear(GL_DEPTH_BUFFER_BIT);
  gl.Enable(GL_POLYGON_OFFSET_FILL);
  gl.PolygonOffset(1.5f, 3.0f);
  if (s_nvox && !SDL_getenv("EB_NO_VOX_SHADOW")) {
    gl.UseProgram(s_prog_vox_depth);
    gl.UniformMatrix4fv(gl.GetUniformLocation(s_prog_vox_depth, "u_mvp"), 1, GL_FALSE, s_lmvp);
    gl.Uniform3f(gl.GetUniformLocation(s_prog_vox_depth, "u_sun"), sun[0], sun[1], sun[2]);
    gl.BindVertexArray(s_vao_vox);
    gl.DrawArraysInstanced(GL_TRIANGLES, 0, 18, s_nvox);
  }
  if (s_ncardv_l && !SDL_getenv("EB_NO_CARD_SHADOW")) {
    gl.UseProgram(s_prog_card_depth);
    gl.UniformMatrix4fv(gl.GetUniformLocation(s_prog_card_depth, "u_mvp"), 1, GL_FALSE, s_lmvp);
    gl.Uniform1i(gl.GetUniformLocation(s_prog_card_depth, "u_tex"), 0);
    gl.BindVertexArray(s_vao_card);
    gl.BindBuffer(GL_ARRAY_BUFFER, s_vbo_card);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)s_ncardv_l * (GLsizeiptr)sizeof(CardVert), s_cards_l, GL_STREAM_DRAW);
    gl.DrawArrays(GL_TRIANGLES, 0, s_ncardv_l);
  }
  gl.Disable(GL_POLYGON_OFFSET_FILL);

  /* the scene: sky, ground (land, then the sunken water), voxels, sprites */
  gl.BindFramebuffer(GL_FRAMEBUFFER, s_fbo);
  gl.Viewport(0, 0, ww, wh);
  gl.ClearColor(hor[0] * bright, hor[1] * bright, hor[2] * bright, 1.0f);
  gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  gl.Disable(GL_DEPTH_TEST);
  gl.UseProgram(s_prog_sky);
  gl.Uniform3f(gl.GetUniformLocation(s_prog_sky, "u_top"), top[0], top[1], top[2]);
  gl.Uniform3f(gl.GetUniformLocation(s_prog_sky, "u_hor"), hor[0], hor[1], hor[2]);
  gl.Uniform1f(gl.GetUniformLocation(s_prog_sky, "u_bright"), bright);
  gl.BindVertexArray(s_vao_empty);
  gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  gl.Enable(GL_DEPTH_TEST);
  gl.DepthFunc(GL_LEQUAL);
  gl.DepthMask(GL_TRUE);

  gl.ActiveTexture(GL_TEXTURE1);
  gl.BindTexture(GL_TEXTURE_2D, s_shadow_tex);
  gl.ActiveTexture(GL_TEXTURE0);
  gl.BindTexture(GL_TEXTURE_2D, s_tex_ground);
  gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VW, VH, GL_BGRA, GL_UNSIGNED_BYTE, s_ground);
  set_common(s_prog_ground, mvp, bright, clear);
  gl.Uniform1i(gl.GetUniformLocation(s_prog_ground, "u_tex"), 0);
  gl.Uniform4f(gl.GetUniformLocation(s_prog_ground, "u_rect"), (float)-VX_MARGIN, (float)VY0,
               (float)(256 + VX_MARGIN), (float)(VY0 + VH));
  gl.BindVertexArray(s_vao_empty);
  gl.Uniform1i(gl.GetUniformLocation(s_prog_ground, "u_layer"), 0);
  gl.Uniform1f(gl.GetUniformLocation(s_prog_ground, "u_z"), 0.0f);
  gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  gl.Uniform1i(gl.GetUniformLocation(s_prog_ground, "u_layer"), 1);
  gl.Uniform1f(gl.GetUniformLocation(s_prog_ground, "u_z"), (float)-WATER_D);
  gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  if (s_nvox) {
    set_common(s_prog_vox, mvp, bright, clear);
    gl.Uniform3f(gl.GetUniformLocation(s_prog_vox, "u_eye"), eye[0], eye[1], eye[2]);
    gl.Uniform3f(gl.GetUniformLocation(s_prog_vox, "u_sun"), sun[0], sun[1], sun[2]);
    gl.BindVertexArray(s_vao_vox);
    gl.DrawArraysInstanced(GL_TRIANGLES, 0, 18, s_nvox);
  }

  if (s_ncardv) {
    gl.BindTexture(GL_TEXTURE_2D, s_tex_atlas);
    set_common(s_prog_card, mvp, bright, clear);
    gl.Uniform1i(gl.GetUniformLocation(s_prog_card, "u_tex"), 0);
    gl.BindVertexArray(s_vao_card);
    gl.BindBuffer(GL_ARRAY_BUFFER, s_vbo_card);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)s_ncardv * (GLsizeiptr)sizeof(CardVert), s_cards, GL_STREAM_DRAW);
    gl.DrawArrays(GL_TRIANGLES, 0, s_ncardv);
  }

  if (SDL_getenv("EB_PROF") && (g_frame_count % 60) == 0) {
    gl.Finish();
    uint64_t t3 = SDL_GetTicksNS();
    eb_log("3D prof f%llu: world %.2f ms, sprites %.2f ms, gl %.2f ms, voxels %d, cards %d", (unsigned long long)g_frame_count,
           (prof_t1 - prof_t0) / 1e6, (prof_t2 - prof_t1) / 1e6, (t3 - prof_t2) / 1e6, s_nvox, s_ncardv / 6);
    int mz = -99999, ml = 0, mzi = 0, mli = 0, mnz = 99999, mnzi = 0;
    for (int i = 0; i < s_nvox; i++) {
      if (s_vox[i].z1 > mz) mz = s_vox[i].z1, mzi = i;
      if (s_vox[i].len > ml) ml = s_vox[i].len, mli = i;
      if (s_vox[i].z0 < mnz) mnz = s_vox[i].z0, mnzi = i;
    }
    if (s_nvox)
      eb_log("  tallest voxel z1 %d at x %d dep %d; longest len %d at x %d dep %d z %d..%d; lowest z0 %d at x %d dep %d", mz,
             s_vox[mzi].x, s_vox[mzi].dep, ml, s_vox[mli].x, s_vox[mli].dep, s_vox[mli].z0, s_vox[mli].z1, mnz,
             s_vox[mnzi].x, s_vox[mnzi].dep);
  }
  /* post: tilt-shift around the player's row + vignette, to the window */
  gl.Disable(GL_DEPTH_TEST);
  gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)target_fbo);
  gl.Viewport(0, 0, ww, wh);
  gl.BindTexture(GL_TEXTURE_2D, s_fbo_color);
  gl.UseProgram(s_prog_post);
  gl.Uniform1i(gl.GetUniformLocation(s_prog_post, "u_tex"), 0);
  gl.Uniform2f(gl.GetUniformLocation(s_prog_post, "u_texel"), 1.0f / ww, 1.0f / wh);
  {
    /* the player stands at the camera target: focus on its row on screen */
    float clip[4];
    float p[4] = {128.0f, -112.0f, 8.0f, 1.0f};
    for (int r = 0; r < 4; r++) clip[r] = mvp[0 * 4 + r] * p[0] + mvp[1 * 4 + r] * p[1] + mvp[2 * 4 + r] * p[2] + mvp[3 * 4 + r];
    float fy = clip[3] != 0.0f ? 0.5f - 0.5f * clip[1] / clip[3] : 0.5f;
    gl.Uniform1f(gl.GetUniformLocation(s_prog_post, "u_focus"), fy);
  }
  gl.Uniform1f(gl.GetUniformLocation(s_prog_post, "u_band"), 0.16f);
  /* tests: EB_NOBLUR=1 turns the tilt-shift off for sharp captures */
  gl.Uniform1f(gl.GetUniformLocation(s_prog_post, "u_maxblur"), SDL_getenv("EB_NOBLUR") ? 0.0f : 5.0f * (float)wh / 720.0f);
  gl.BindVertexArray(s_vao_empty);
  gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  /* text windows on top, where the 2D picture would be */
  int tex_w = 256 + 2 * ext;
  double par = g_cfg.square_pixels ? 1.0 : 8.0 / 7.0;
  double aw = tex_w * par, ah = PPU_H;
  double sc = SDL_min(ww / aw, wh / ah);
  if (g_cfg.integer_scale && g_cfg.square_pixels && sc >= 1.0) sc = SDL_floor(sc);
  int vw = (int)(aw * sc + 0.5), vh = (int)(ah * sc + 0.5);
  gl.Viewport((ww - vw) / 2, (wh - vh) / 2, vw, vh);
  gl.BindTexture(GL_TEXTURE_2D, s_tex_ovl);
  gl.PixelStorei(GL_UNPACK_ROW_LENGTH, PPU_MAX_W);
  gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_w, PPU_H, GL_BGRA, GL_UNSIGNED_BYTE, s_overlay);
  gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  gl.UseProgram(s_prog_ovl);
  gl.Uniform1i(gl.GetUniformLocation(s_prog_ovl, "u_tex"), 0);
  gl.Uniform2f(gl.GetUniformLocation(s_prog_ovl, "u_uvscale"), (float)tex_w / PPU_MAX_W, 1.0f);
  gl.BindVertexArray(s_vao_empty);
  gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  gl.BindVertexArray(0);
  return 1;
}
