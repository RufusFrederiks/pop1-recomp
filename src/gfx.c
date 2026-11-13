/* gfx.c - the enhanced renderer: a recomp-only effect, never in the Original
 * look. Lighting "Enhanced" in the menu; "Classic" is light.c, unchanged.
 *
 * It needs shaders, so it only works when SDL's renderer is its OpenGL one
 * (platform.c asks for that; `gpu = 0` in pop1.cfg asks for the old default).
 * The GL calls here are made between SDL's own (after SDL_RenderFlush), and
 * every bit of GL state touched is put back, so SDL's renderer never notices.
 * OpenGL 2.1 with GLSL 1.20, framebuffer objects and float textures: any
 * card since about 2008.
 *
 * Where the facts come from: light.c already knows the room on screen, its
 * tiles, and which torches and potions are lit (light_sources, light_room).
 *
 * Per frame (gfx_frame), all at the game's own size (640x480), so it costs
 * the same on a 4K screen as in a small window:
 *  1. occluders: a small picture of what stops light, drawn from the room's
 *     tiles: wall blocks and pillars (red), floor slabs (green);
 *  2. light pass: for every pixel, the light of every torch and potion
 *     reaching it: distance falloff, a shadow from marching through the
 *     occluders towards the light (light gets a little way into stone, so
 *     the faces of blocks near a torch are lit and those behind are not),
 *     and relief: a normal from the picture's own brightness, so the light
 *     rakes across the stones. The Prince and the guard throw shadows too:
 *     each is a soft upright capsule where the game says they stand. A
 *     second output is the light hanging in the air round a flame, shadowed
 *     the same way, and how much open space is darkened by stone close by
 *     (corners, under ledges);
 *  3. glow: the lit frame's brightest parts (the flames above all), blurred
 *     down and up through five sizes (bloom); sparks and dust are drawn into
 *     it too.
 * Per target (gfx_apply), at the target's size (the 4x stage, or the 1x frame
 * for the CRT looks): the picture times its light, the black background
 * faintly lit as a wall, flames brighter than white, heat haze above the
 * flames, plus the glow, through a tone curve that leaves ordinary colours
 * as they are and rolls the bright ones off; then sparks and dust on top.
 *
 * Things that happen: the game's own sound effects (pop_gfx_sound, from the
 * hook in traps.c that also drives the rumble) set off dust where the Prince
 * lands and sparks with a flash where swords meet; a loose floor that drops
 * out of the room's tiles leaves dust, and more where it lands.
 *
 * POPGFXDEBUG=1 tints the occluders and the characters' capsules.
 * POPGFXTEST=puff@6 (or clash@6) sets that effect off at the Prince.
 *
 * Everything to tune is in T below. */
#include "gfx.h"
#include "light.h"
#include "pop1.h"
#include <SDL_opengl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what to tune ---- */
static const struct {
    float amb_torch[3];     /* the room's own light when a torch is lit in it (1 = as drawn) */
    float amb_dark[3];      /* ... and in a room without one: stays easy to see */
    float torch[3], torch_i, torch_r;   /* a torch: colour, strength, reach in tiles */
    float potion_i, potion_r;           /* a potion's glow: strength, reach in tiles */
    float pen;              /* how far light gets into stone, in pixels */
    float lz;               /* how far in front of the wall the light sits (pixels): lower = more raking */
    float relief;           /* strength of the relief */
    float back;             /* how bright the black background wall gets in torch light */
    float air;              /* light hanging in the air round a flame */
    float flame;            /* how much brighter than drawn the flames are */
    float bloom, thresh;    /* strength of the glow, and how bright a pixel must be to glow */
    float haze;             /* heat haze, in pixels */
    float vignette;         /* darkening towards the corners */
    float flicker;          /* +- of a torch's strength */
    float char_shadow;      /* how dark the Prince's and the guard's shadows are (0-1) */
    float contact;          /* the dark patch under their feet (0-1) */
    float ao;               /* how much darker open space gets in corners, next to stone (0-1) */
} T = {
    { 0.46f, 0.50f, 0.66f },
    { 0.82f, 0.84f, 0.92f },
    { 1.00f, 0.62f, 0.30f }, 1.55f, 5.0f,
    0.55f, 1.3f,
    34.0f,
    46.0f,
    2.6f,
    0.030f,
    0.085f,
    1.0f,
    0.95f, 1.60f,
    0.9f,
    0.20f,
    0.11f,
    0.80f,
    0.38f,
    0.55f,
};

#define MAXL 12
#define NB 5

/* GL names newer than 1.1 that some systems' headers (macOS) leave out */
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_COLOR_ATTACHMENT1
#define GL_COLOR_ATTACHMENT1 0x8CE1
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_PROGRAM_OBJECT_ARB
#define GL_PROGRAM_OBJECT_ARB 0x8B40
#endif

/* ---- the GL functions used, loaded by name (Windows only exports 1.1) ---- */
#ifndef APIENTRY
#define APIENTRY
#endif
#define GLFUNCS(X) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, Enable, (GLenum)) X(void, Disable, (GLenum)) \
    X(void, BlendFunc, (GLenum, GLenum)) \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, ClearColor, (GLclampf, GLclampf, GLclampf, GLclampf)) X(void, Clear, (GLbitfield)) \
    X(void, Begin, (GLenum)) X(void, End, (void)) \
    X(void, Vertex2f, (GLfloat, GLfloat)) X(void, TexCoord2f, (GLfloat, GLfloat)) \
    X(void, Color4f, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, GetIntegerv, (GLenum, GLint *)) X(const GLubyte *, GetString, (GLenum)) \
    X(void, PushAttrib, (GLbitfield)) X(void, PopAttrib, (void)) \
    X(void, PushClientAttrib, (GLbitfield)) X(void, PopClientAttrib, (void)) \
    X(void, PixelStorei, (GLenum, GLint)) \
    X(void, GenTextures, (GLsizei, GLuint *)) X(void, DeleteTextures, (GLsizei, const GLuint *)) \
    X(void, BindTexture, (GLenum, GLuint)) X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, CopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
    X(GLenum, GetError, (void)) \
    X(void, ActiveTexture, (GLenum)) \
    X(GLuint, CreateShader, (GLenum)) X(void, ShaderSource, (GLuint, GLsizei, const char **, const GLint *)) \
    X(void, CompileShader, (GLuint)) X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *)) X(void, DeleteShader, (GLuint)) \
    X(GLuint, CreateProgram, (void)) X(void, AttachShader, (GLuint, GLuint)) X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *)) \
    X(void, DeleteProgram, (GLuint)) X(void, UseProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const char *)) \
    X(void, Uniform1i, (GLint, GLint)) X(void, Uniform1f, (GLint, GLfloat)) \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat)) X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) X(void, DrawBuffers, (GLsizei, const GLenum *))
/* (named qgl...: the gl... names are SDL_opengl.h's own) */
#define X(ret, name, args) static ret (APIENTRY *qgl##name) args;
GLFUNCS(X)
#undef X
/* SDL's renderer uses the old ARB shader objects; its program is put back
 * through the same door where that exists (on macOS the two differ). */
static GLhandleARB (APIENTRY *glGetHandleARB_)(GLenum);
static void (APIENTRY *glUseProgramObjectARB_)(GLhandleARB);

static int load_gl(void){
    int ok = 1;
#define X(ret, name, args) \
    qgl##name = (ret (APIENTRY *) args)SDL_GL_GetProcAddress("gl" #name); \
    if(!qgl##name){ fprintf(stderr, "[gfx] no gl" #name "\n"); ok = 0; }
    GLFUNCS(X)
#undef X
    glGetHandleARB_ = (GLhandleARB (APIENTRY *)(GLenum))SDL_GL_GetProcAddress("glGetHandleARB");
    glUseProgramObjectARB_ = (void (APIENTRY *)(GLhandleARB))SDL_GL_GetProcAddress("glUseProgramObjectARB");
    return ok;
}

/* ---- shaders ---- */
static const char *VS =
"#version 120\n"
"varying vec2 vUV; varying vec4 vCol;\n"
"void main(){ gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0); vUV = gl_MultiTexCoord0.xy; vCol = gl_Color; }\n";

/* 2: the light reaching every pixel of the game frame */
static const char *FS_LIGHT =
"#version 120\n"
"#define MAXL 12\n"
"uniform sampler2D uFrame, uOcc;\n"
"uniform vec2 uSize;\n"
"uniform int uN;\n"
"uniform vec4 uLpos[MAXL];\n"      /* x, y, reach, strength */
"uniform vec4 uLcol[MAXL];\n"      /* rgb, kind */
"uniform float uRelief, uShadow, uPen, uLz, uStatusY, uAir, uTileW, uCharK, uAO;\n"
"uniform int uNC;\n"
"uniform vec4 uChar[2];\n"        /* x, feet y, height, half width */
"float segd(vec2 p, vec2 a, vec2 b){ vec2 ab = b - a; float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 0.0001), 0.0, 1.0); return length(p - a - ab * t); }\n"
"varying vec2 vUV;\n"
"float luma(vec2 p){ return dot(texture2D(uFrame, p / uSize).rgb, vec3(0.299, 0.587, 0.114)); }\n"
"float hash(vec2 p){ return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }\n"
"void main(){\n"
"  vec2 p = vUV * uSize;\n"
"  vec3 surf = vec3(0.0), air = vec3(0.0); float zone = 0.0, ao = 1.0;\n"
"  if(p.y < uStatusY + 4.0){\n"
"    float hx = luma(p + vec2(1.0, 0.0)) - luma(p - vec2(1.0, 0.0));\n"
"    float hy = luma(p + vec2(0.0, 1.0)) - luma(p - vec2(0.0, 1.0));\n"
"    vec3 n = normalize(vec3(-hx * uRelief, -hy * uRelief, 1.0));\n"
"    float j = hash(floor(p));\n"
/* corners: open space is darker the more stone there is close by */
"    float near = 0.0, r1 = uTileW * 0.16, r2 = uTileW * 0.34;\n"
"    for(int k = 0; k < 8; k++){\n"
"      float an = (float(k) + 0.5) * 0.7853982; vec2 dir = vec2(cos(an), sin(an));\n"
"      vec4 o1 = texture2D(uOcc, (p + dir * r1) / uSize), o2 = texture2D(uOcc, (p + dir * r2) / uSize);\n"
"      near += max(o1.r, o1.g) * 0.6 + max(o2.r, o2.g) * 0.4;\n"
"    }\n"
"    vec4 here = texture2D(uOcc, p / uSize);\n"
"    ao = 1.0 - uAO * (near / 8.0) * (1.0 - max(here.r, here.g));\n"
"    for(int i = 0; i < MAXL; i++){\n"
"      if(i >= uN) break;\n"
"      vec2 d = uLpos[i].xy - p; float R = uLpos[i].z, dist = length(d);\n"
"      if(dist < R){\n"
"        float q = dist / R, w = 1.0 - q * q; w *= w;\n"
"        float core = uLcol[i].w > 0.5 ? 0.30 : 0.40;\n"
"        float att = w / (1.0 + q * q / (core * core));\n"
"        float od = 0.0, sl = dist / 24.0;\n"
"        for(int k = 0; k < 24; k++){\n"
"          vec4 o = texture2D(uOcc, (p + d * ((float(k) + j) / 24.0)) / uSize);\n"
"          od += (o.r + 3.0 * o.g) * sl;\n"
"        }\n"
"        float sh = mix(1.0, exp(-od / uPen), uShadow);\n"
"        for(int c = 0; c < 2; c++){\n"
"          if(c >= uNC) break;\n"
"          float hw = uChar[c].w;\n"
"          vec2 a = vec2(uChar[c].x, uChar[c].y - hw), b = vec2(uChar[c].x, uChar[c].y - uChar[c].z + hw);\n"
/* the wall is a little behind the figure: its shadow there is the figure
 * moved away from the light, a short way, softer the further it falls */
"          vec2 away = (a + b) * 0.5 - uLpos[i].xy; float al = length(away);\n"
"          float shift = min(al * 0.30, uTileW * 0.40);\n"
"          vec2 off = away / max(al, 0.001) * shift;\n"
"          float soft = hw * (0.7 + shift / (uTileW * 0.4));\n"
"          float s = smoothstep(hw * 0.4, hw * 0.4 + soft, segd(p, a + off, b + off));\n"
"          s = mix(1.0, s, smoothstep(hw * 0.9, hw * 1.6, segd(p, a, b)));\n"   /* not on the figure itself */
"          sh *= mix(1.0, s, uCharK * uShadow);\n"
"        }\n"
"        vec3 l3 = normalize(vec3(d, uLz));\n"
"        float nl = clamp(dot(n, l3) / l3.z, 0.0, 1.4);\n"
"        vec3 c = uLcol[i].rgb * uLpos[i].w;\n"
"        surf += c * att * sh * nl;\n"
"        float a2 = dist / (uTileW * 0.9);\n"
"        air += c * sh * w * uAir / (1.0 + a2 * a2 * 3.0);\n"
"        if(uLcol[i].w < 0.5){\n"
"          float z = length(d / (uTileW * vec2(0.30, 0.44)));\n"
"          zone = max(zone, 1.0 - smoothstep(0.75, 1.0, z));\n"
"        }\n"
"      }\n"
"    }\n"
"    surf /= 1.0 + 0.9 * max(surf.r, max(surf.g, surf.b));\n"   /* close to a flame the light levels off */
"    float st = 1.0 - smoothstep(uStatusY - 1.0, uStatusY + 3.0, p.y);\n"
"    surf *= st; air *= st; zone *= st; ao = mix(1.0, ao, st);\n"
"  }\n"
"  gl_FragData[0] = vec4(surf, zone);\n"
"  gl_FragData[1] = vec4(air, ao);\n"
"}\n";

/* The picture with its light: shared by the glow's source and the final pass. */
static const char *FS_COMMON =
"#version 120\n"
"uniform sampler2D uPic, uLight, uAir;\n"
"uniform vec3 uAmb;\n"
"uniform float uBack, uFlame, uStatusY, uSizeY, uContact;\n"
"uniform int uNB;\n"
"uniform vec4 uBlob[2];\n"        /* x, y, half width, half height */
"vec3 lin(vec3 c){ return pow(c, vec3(2.2)); }\n"
"float gFl;\n"
"float h21(vec2 p){ return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
"float vnoise(vec2 p){ vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);\n"
"  return mix(mix(h21(i), h21(i + vec2(1.0, 0.0)), f.x), mix(h21(i + vec2(0.0, 1.0)), h21(i + vec2(1.0, 1.0)), f.x), f.y); }\n"
"vec3 lit(vec3 s, vec2 uv, out vec3 a){\n"
"  a = lin(s);\n"
"  vec4 L = texture2D(uLight, uv);\n"
"  float room = 1.0 - smoothstep(uStatusY - 1.0, uStatusY + 3.0, uv.y * uSizeY);\n"
"  vec3 amb = mix(vec3(1.0), uAmb, room);\n"
"  float lum = dot(a, vec3(0.2126, 0.7152, 0.0722));\n"
"  float empty = 1.0 - smoothstep(0.0, 0.012, lum);\n"
"  float fl = smoothstep(0.45, 0.85, s.r) * smoothstep(0.25, 0.45, s.g) * (1.0 - smoothstep(0.08, 0.35, s.b)) * L.a;\n"
"  vec2 gp = uv * vec2(uSizeY * 4.0 / 3.0, uSizeY);\n"
"  float ao = texture2D(uAir, uv).a;\n"
"  amb *= ao; L.rgb *= ao;\n"
"  for(int i = 0; i < 2; i++){\n"
"    if(i >= uNB) break;\n"
"    vec2 d = (gp - uBlob[i].xy) / uBlob[i].zw;\n"
"    float k = 1.0 - uContact * exp(-dot(d, d) * 1.6);\n"
"    amb *= k; L.rgb *= k;\n"
"  }\n"
"  vec3 c = a * (amb + L.rgb * (1.0 - fl));\n"
"  vec2 g = uv * vec2(uSizeY * 4.0 / 3.0, uSizeY);\n"
"  float grain = 0.8 + 0.4 * vnoise(g / 7.0) * vnoise(g / 23.0 + 9.0) * 2.0;\n"
"  c += empty * uBack * grain * L.rgb * vec3(0.80, 0.92, 1.0);\n"
"  c += a * fl * uFlame;\n"
"  c += texture2D(uAir, uv).rgb * (1.0 - fl) * mix(0.25, 1.0, empty);\n"
"  gFl = fl;\n"
"  return c;\n"
"}\n";

/* 3a: what glows */
static const char *FS_SRC =
"uniform float uThresh;\n"
"varying vec2 vUV;\n"
"void main(){\n"
"  vec3 a; vec3 c = lit(texture2D(uPic, vUV).rgb, vUV, a);\n"
"  float m = max(c.r, max(c.g, c.b));\n"
"  float k = max(m - uThresh, 0.0); k = k * k / (k + 0.25);\n"
"  k = max(k, gFl * m * 0.45);\n"
"  gl_FragColor = vec4(c * (k / max(m, 0.0001)), 1.0);\n"
"}\n";

/* 3b: half the size (13 taps, Call of Duty's filter: no flicker from single bright pixels) */
static const char *FS_DOWN =
"#version 120\n"
"uniform sampler2D uPic; uniform vec2 uTexel;\n"
"varying vec2 vUV;\n"
"void main(){\n"
"  vec2 t = uTexel;\n"
"  vec3 a = texture2D(uPic, vUV + t * vec2(-2.0, -2.0)).rgb, b = texture2D(uPic, vUV + t * vec2(0.0, -2.0)).rgb;\n"
"  vec3 c = texture2D(uPic, vUV + t * vec2(2.0, -2.0)).rgb, d = texture2D(uPic, vUV + t * vec2(-2.0, 0.0)).rgb;\n"
"  vec3 e = texture2D(uPic, vUV).rgb, f = texture2D(uPic, vUV + t * vec2(2.0, 0.0)).rgb;\n"
"  vec3 g = texture2D(uPic, vUV + t * vec2(-2.0, 2.0)).rgb, h = texture2D(uPic, vUV + t * vec2(0.0, 2.0)).rgb;\n"
"  vec3 i = texture2D(uPic, vUV + t * vec2(2.0, 2.0)).rgb, j = texture2D(uPic, vUV + t * vec2(-1.0, -1.0)).rgb;\n"
"  vec3 k = texture2D(uPic, vUV + t * vec2(1.0, -1.0)).rgb, l = texture2D(uPic, vUV + t * vec2(-1.0, 1.0)).rgb;\n"
"  vec3 m = texture2D(uPic, vUV + t * vec2(1.0, 1.0)).rgb;\n"
"  gl_FragColor = vec4(e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125, 1.0);\n"
"}\n";

/* 3c: back up (a 3x3 tent), added to the size above */
static const char *FS_UP =
"#version 120\n"
"uniform sampler2D uPic; uniform vec2 uTexel;\n"
"varying vec2 vUV;\n"
"void main(){\n"
"  vec2 t = uTexel;\n"
"  vec3 s = texture2D(uPic, vUV).rgb * 4.0;\n"
"  s += (texture2D(uPic, vUV + t * vec2(-1.0, 0.0)).rgb + texture2D(uPic, vUV + t * vec2(1.0, 0.0)).rgb\n"
"      + texture2D(uPic, vUV + t * vec2(0.0, -1.0)).rgb + texture2D(uPic, vUV + t * vec2(0.0, 1.0)).rgb) * 2.0;\n"
"  s += texture2D(uPic, vUV + t * vec2(-1.0, -1.0)).rgb + texture2D(uPic, vUV + t * vec2(1.0, -1.0)).rgb\n"
"     + texture2D(uPic, vUV + t * vec2(-1.0, 1.0)).rgb + texture2D(uPic, vUV + t * vec2(1.0, 1.0)).rgb;\n"
"  gl_FragColor = vec4(s / 16.0, 1.0);\n"
"}\n";

/* the final picture */
static const char *FS_FINAL =
"#define MAXL 12\n"
"uniform sampler2D uBloom, uOcc;\n"
"uniform float uDebug;\n"
"uniform vec4 uChar[2];\n"
"uniform int uNC;\n"
"uniform vec2 uSize, uCentre;\n"
"uniform int uN;\n"
"uniform vec4 uLpos[MAXL];\n"       /* x, y, kind, phase */
"uniform float uLevel, uBloomK, uHaze, uVig, uTime, uTileW;\n"
"varying vec2 vUV;\n"
"float hash(vec2 p){ return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }\n"
"void main(){\n"
"  vec2 p = vUV * uSize, off = vec2(0.0);\n"
"  if(uHaze > 0.0) for(int i = 0; i < MAXL; i++){\n"
"    if(i >= uN) break;\n"
"    if(uLpos[i].z > 0.5) continue;\n"
"    vec2 d = p - uLpos[i].xy; float up = -d.y, hw = uTileW * 0.20;\n"
"    float w = exp(-d.x * d.x / (hw * hw)) * smoothstep(6.0, 22.0, up) * (1.0 - smoothstep(uTileW * 0.7, uTileW * 1.8, up));\n"
"    float ph = uLpos[i].w;\n"
"    off.x += w * (sin(up * 0.33 - uTime * 6.5 + ph) + 0.5 * sin(up * 0.71 - uTime * 10.0 + 2.0 * ph + d.x * 0.3));\n"
"    off.y += w * 0.5 * sin(up * 0.21 - uTime * 5.0 + 3.0 * ph);\n"
"  }\n"
"  vec2 uv = vUV + off * uHaze / uSize;\n"
"  vec3 s = texture2D(uPic, uv).rgb, a;\n"
"  vec3 c = lit(s, uv, a);\n"
"  c += texture2D(uBloom, uv).rgb * uBloomK;\n"
"  float room = 1.0 - smoothstep(uStatusY - 1.0, uStatusY + 3.0, p.y);\n"
"  vec2 v = (p - uCentre) / uCentre;\n"
"  c *= 1.0 - uVig * room * smoothstep(0.45, 1.25, length(v * vec2(1.0, 0.8)));\n"
/* tone curve: as drawn up to 0.75, then rolled off towards white */
"  float m = max(c.r, max(c.g, c.b));\n"
"  if(m > 0.75){\n"
"    float t = 0.75 + 0.25 * (1.0 - exp(-(m - 0.75) / 0.25));\n"
"    c *= t / m;\n"
"    c = mix(c, t * vec3(1.0, 0.95, 0.85), 0.35 * smoothstep(1.2, 4.0, m));\n"
"  }\n"
"  c = mix(a, c, uLevel);\n"
"  c = pow(max(c, vec3(0.0)), vec3(1.0 / 2.2));\n"
"  c += (hash(gl_FragCoord.xy + fract(uTime) * 61.0) - 0.5) * (1.2 / 255.0) * room * uLevel;\n"
"  if(uDebug > 0.5){\n"
"    vec4 o = texture2D(uOcc, vUV); c = mix(c, vec3(1.0, 0.0, 0.0), 0.35 * o.r); c = mix(c, vec3(0.0, 1.0, 0.0), 0.5 * o.g);\n"
"    for(int i = 0; i < 2; i++){ if(i >= uNC) break;\n"
"      vec2 q = p - vec2(uChar[i].x, uChar[i].y - uChar[i].z * 0.5);\n"
"      if(abs(q.x) < uChar[i].w && abs(q.y) < uChar[i].z * 0.5) c = mix(c, vec3(0.2, 0.4, 1.0), 0.5); }\n"
"  }\n"
"  gl_FragColor = vec4(c, 1.0);\n"
"}\n";

/* sparks and dust: soft round dots, added */
static const char *FS_PART =
"#version 120\n"
"varying vec2 vUV; varying vec4 vCol;\n"
"void main(){ float r = length(vUV); float a = 1.0 - smoothstep(0.35, 1.0, r); gl_FragColor = vec4(vCol.rgb * vCol.a * a, 1.0); }\n";

/* ---- state ---- */
static struct {
    int tried, ok;
    GLuint pLight, pSrc, pDown, pUp, pFinal, pPart;
    GLuint tFrame, tOcc, tLight, tAir, tSrc, tBloom[NB], tCopy[2];
    GLuint fLight, fSrc, fBloom[NB];
    int w, h, cw[2], ch[2];
    int bw[NB], bh[NB];
} X;
static GfxOpts g_opt;
static int g_live;                         /* gfx_frame found a room to light */
static float g_level, g_amb[3], g_dark = 1;
static float g_tile_w = 64, g_top = 40, g_row_h = 126, g_status_y = 425;
static unsigned char g_tiles[40];
static int g_room_key = -1;
static struct { float x, y, reach, strength, r, g, b, phase; int kind; } g_l[MAXL];
static int g_nl;
static uint32_t g_last_ms;
static double g_time;
static LightChar g_ch[2];
static float g_chw = 11;                  /* half the width of a character's capsule */
static int g_ev[16], g_nev;                /* sound effects since the last frame */
static struct { float x, y, age; int on; } g_flash;   /* sword on sword */
void pop_gfx_sound(int n){ if(g_nev < 16) g_ev[g_nev++] = n; }

/* ---- particles ---- */
typedef struct { float x, y, vx, vy, age, life, size, seed, lr, lg, lb; int kind; } Part;   /* kind 0 torch spark, 1 dust in the air, 2 a puff of dust, 3 a sword spark */
#define MAXP 320
#define NDUST 60
static Part g_p[MAXP];
static int g_np;
static float g_spawn[MAXL];
static uint32_t g_rng = 0x1234567;
static float frand(void){ g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) / 16777216.0f; }

static float noise1(float t, float seed){
    float i = floorf(t), f = t - i, u = f * f * (3 - 2 * f);
    float a = sinf((i + seed) * 12.9898f) * 43758.5453f, b = sinf((i + 1 + seed) * 12.9898f) * 43758.5453f;
    a -= floorf(a); b -= floorf(b);
    return (a + (b - a) * u) * 2 - 1;
}

/* ---- GL helpers ---- */
static GLuint compile(GLenum type, const char *a, const char *b, const char *name){
    GLuint s = qglCreateShader(type);
    const char *src[2] = { a, b ? b : "" };
    qglShaderSource(s, 2, src, NULL);
    qglCompileShader(s);
    GLint ok = 0; qglGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if(!ok){ char log[2048] = ""; qglGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[gfx] shader %s: %s\n", name, log); qglDeleteShader(s); return 0; }
    return s;
}
static GLuint program(const char *fa, const char *fb, const char *name){
    GLuint v = compile(GL_VERTEX_SHADER, VS, NULL, "vertex"), f = compile(GL_FRAGMENT_SHADER, fa, fb, name);
    if(!v || !f) return 0;
    GLuint p = qglCreateProgram();
    qglAttachShader(p, v); qglAttachShader(p, f); qglLinkProgram(p);
    qglDeleteShader(v); qglDeleteShader(f);
    GLint ok = 0; qglGetProgramiv(p, GL_LINK_STATUS, &ok);
    if(!ok){ char log[2048] = ""; qglGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "[gfx] link %s: %s\n", name, log); qglDeleteProgram(p); return 0; }
    return p;
}
static GLuint texture(int w, int h, GLint internal, GLenum fmt, GLenum type, GLint filter){
    GLuint t; qglGenTextures(1, &t);
    qglBindTexture(GL_TEXTURE_2D, t);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    qglTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, type, NULL);
    return t;
}
static GLuint fbo(GLuint t0, GLuint t1){
    GLuint f; qglGenFramebuffers(1, &f);
    qglBindFramebuffer(GL_FRAMEBUFFER, f);
    qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t0, 0);
    if(t1){
        qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t1, 0);
        const GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        qglDrawBuffers(2, bufs);
    }
    if(qglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){ qglDeleteFramebuffers(1, &f); return 0; }
    return f;
}
static void quad(void){
    qglBegin(GL_TRIANGLE_STRIP);
    qglTexCoord2f(0, 0); qglVertex2f(-1, -1);
    qglTexCoord2f(1, 0); qglVertex2f( 1, -1);
    qglTexCoord2f(0, 1); qglVertex2f(-1,  1);
    qglTexCoord2f(1, 1); qglVertex2f( 1,  1);
    qglEnd();
}
static void bind(int unit, GLuint tex){ qglActiveTexture(GL_TEXTURE0 + unit); qglBindTexture(GL_TEXTURE_2D, tex); }
#define U(p, n) qglGetUniformLocation(p, n)

/* Everything of GL's state that is touched, saved and put back. */
typedef struct { GLint fb, prog; GLhandleARB harb; } Saved;
static void save(Saved *s){
    qglGetIntegerv(GL_FRAMEBUFFER_BINDING, &s->fb);
    if(glGetHandleARB_ && glUseProgramObjectARB_) s->harb = glGetHandleARB_(GL_PROGRAM_OBJECT_ARB);
    else qglGetIntegerv(GL_CURRENT_PROGRAM, &s->prog);
    qglPushAttrib(GL_ALL_ATTRIB_BITS);
    qglPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    qglDisable(GL_BLEND); qglDisable(GL_SCISSOR_TEST); qglDisable(GL_DEPTH_TEST); qglDisable(GL_CULL_FACE);
    qglDisable(GL_ALPHA_TEST); qglDisable(GL_STENCIL_TEST);
    qglColorMask(1, 1, 1, 1);
    qglColor4f(1, 1, 1, 1);
}
static void restore(const Saved *s){
    qglUseProgram(0);
    if(glGetHandleARB_ && glUseProgramObjectARB_) glUseProgramObjectARB_(s->harb);
    else qglUseProgram((GLuint)s->prog);
    qglBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s->fb);
    qglActiveTexture(GL_TEXTURE0);
    qglPopClientAttrib();
    qglPopAttrib();
    while(qglGetError() != GL_NO_ERROR){}
}

static void free_targets(void){
    GLuint t[] = { X.tFrame, X.tOcc, X.tLight, X.tAir, X.tSrc };
    for(unsigned i = 0; i < sizeof t / sizeof *t; i++) if(t[i]) qglDeleteTextures(1, &t[i]);
    for(int i = 0; i < NB; i++){ if(X.tBloom[i]) qglDeleteTextures(1, &X.tBloom[i]); if(X.fBloom[i]) qglDeleteFramebuffers(1, &X.fBloom[i]); }
    if(X.fLight) qglDeleteFramebuffers(1, &X.fLight);
    if(X.fSrc) qglDeleteFramebuffers(1, &X.fSrc);
    X.tFrame = X.tOcc = X.tLight = X.tAir = X.tSrc = X.fLight = X.fSrc = 0;
    memset(X.tBloom, 0, sizeof X.tBloom); memset(X.fBloom, 0, sizeof X.fBloom);
    X.w = X.h = 0;
}
/* The textures and framebuffers for a w x h game frame. Called inside save(). */
static int make_targets(int w, int h){
    if(X.w == w && X.h == h) return 1;
    free_targets();
    X.tFrame = texture(w, h, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, GL_LINEAR);
    X.tOcc   = texture(w / 2, h / 2, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
    X.tLight = texture(w, h, GL_RGBA16F, GL_RGBA, GL_FLOAT, GL_LINEAR);
    X.tAir   = texture(w, h, GL_RGBA16F, GL_RGBA, GL_FLOAT, GL_LINEAR);
    X.tSrc   = texture(w, h, GL_RGBA16F, GL_RGBA, GL_FLOAT, GL_LINEAR);
    X.fLight = fbo(X.tLight, X.tAir);
    X.fSrc   = fbo(X.tSrc, 0);
    int ok = X.fLight && X.fSrc;
    for(int i = 0, bw = w, bh = h; i < NB; i++){
        bw = bw / 2 > 1 ? bw / 2 : 1; bh = bh / 2 > 1 ? bh / 2 : 1;
        X.bw[i] = bw; X.bh[i] = bh;
        X.tBloom[i] = texture(bw, bh, GL_RGBA16F, GL_RGBA, GL_FLOAT, GL_LINEAR);
        X.fBloom[i] = fbo(X.tBloom[i], 0);
        ok = ok && X.fBloom[i];
    }
    if(!ok){ fprintf(stderr, "[gfx] no float framebuffers\n"); free_targets(); return 0; }
    X.w = w; X.h = h;
    g_room_key = -1;
    return 1;
}

int gfx_available(void){ return X.ok; }

int gfx_init(SDL_Renderer *ren){
    if(X.tried) return X.ok;
    X.tried = 1;
    SDL_RendererInfo ri;
    if(SDL_GetRendererInfo(ren, &ri) != 0 || strcmp(ri.name, "opengl") != 0){
        fprintf(stderr, "[gfx] enhanced lighting off: the renderer is %s, not opengl\n", ri.name ? ri.name : "?");
        return 0;
    }
    if(!SDL_RenderTargetSupported(ren)){ fprintf(stderr, "[gfx] enhanced lighting off: no render targets\n"); return 0; }
    SDL_RenderFlush(ren);
    if(!load_gl()){ fprintf(stderr, "[gfx] enhanced lighting off: OpenGL too old\n"); return 0; }
    fprintf(stderr, "[gfx] OpenGL %s, %s\n", (const char *)qglGetString(GL_VERSION), (const char *)qglGetString(GL_RENDERER));
    Saved s; save(&s);
    X.pLight = program(FS_LIGHT, NULL, "light");
    X.pSrc   = program(FS_COMMON, FS_SRC, "glow source");
    X.pDown  = program(FS_DOWN, NULL, "down");
    X.pUp    = program(FS_UP, NULL, "up");
    X.pFinal = program(FS_COMMON, FS_FINAL, "final");
    X.pPart  = program(FS_PART, NULL, "particles");
    int ok = X.pLight && X.pSrc && X.pDown && X.pUp && X.pFinal && X.pPart;
    if(ok) ok = make_targets(640, 480);
    restore(&s);
    if(!ok){ fprintf(stderr, "[gfx] enhanced lighting off: shaders or framebuffers failed\n"); return 0; }
    X.ok = 1;
    fprintf(stderr, "[gfx] enhanced lighting ready\n");
    return 1;
}

void gfx_free(void){
    /* The GL context goes with SDL's renderer, and everything made in it. */
    memset(&X, 0, sizeof X);
    g_room_key = -1; g_np = 0; g_live = 0; g_last_ms = 0;
}

/* ---- the room ---- */
static int has_floor(int t){ return !(t == 0 || t == 9 || t == 12 || t == 20 || (t >= 26 && t <= 29) || t == 255); }
/* tile at a spot on the frame (255 outside the room) */
static int tile_at(float x, float y){
    int col = (int)floorf(x / g_tile_w), row = (int)floorf((y - g_top) / g_row_h);
    if(col < 0 || col > 9 || row < -1 || row > 2) return 255;
    return g_tiles[(row + 1) * 10 + col];
}
static int solid_at(float x, float y){ return tile_at(x, y - 4) == 20; }

/* 1: what stops light, at half the frame's size. Red: walls and pillars.
 * Green: floor slabs (counted three times as dense: they are thin). */
static void build_occluders(void){
    int ow = X.w / 2, oh = X.h / 2;
    uint8_t *px = calloc((size_t)ow * oh, 4);
    if(!px) return;
    float s = g_tile_w / 64.0f;
    #define OCC(x0, y0, x1, y1, ch, v) do{ \
        int ax = (int)((x0) / 2), ay = (int)((y0) / 2), bx = (int)((x1) / 2), by = (int)((y1) / 2); \
        if(ax < 0) ax = 0; if(ay < 0) ay = 0; if(bx > ow) bx = ow; if(by > oh) by = oh; \
        for(int y = ay; y < by; y++) for(int x = ax; x < bx; x++) if(px[(y * ow + x) * 4 + (ch)] < (v)) px[(y * ow + x) * 4 + (ch)] = (v); }while(0)
    for(int row = -1; row < 3; row++) for(int col = 0; col < 10; col++){
        int t = g_tiles[(row + 1) * 10 + col];
        if(t == 255) continue;
        float x0 = g_tile_w * col, y0 = g_top + g_row_h * row, bot = y0 + g_row_h;
        if(t == 20) OCC(x0, y0 + 3 * s, x0 + g_tile_w, bot + 6 * s, 0, 255);
        else if(has_floor(t)) OCC(x0, bot - 2 * s, x0 + g_tile_w, bot + 6 * s, 1, 255);
        if(t == 3 || t == 8 || t == 9) OCC(x0 + 16 * s, y0 + 3 * s, x0 + 50 * s, bot - 2 * s, 0, 150);
    }
    #undef RECT
    /* a soft edge: one 3x3 blur */
    uint8_t *b = malloc((size_t)ow * oh * 4);
    if(b){
        for(int y = 0; y < oh; y++) for(int x = 0; x < ow; x++) for(int c = 0; c < 2; c++){
            int sum = 0;
            for(int dy = -1; dy <= 1; dy++) for(int dx = -1; dx <= 1; dx++){
                int xx = x + dx, yy = y + dy;
                if(xx < 0) xx = 0; if(xx >= ow) xx = ow - 1; if(yy < 0) yy = 0; if(yy >= oh) yy = oh - 1;
                sum += px[(yy * ow + xx) * 4 + c];
            }
            b[(y * ow + x) * 4 + c] = (uint8_t)(sum / 9);
        }
        qglBindTexture(GL_TEXTURE_2D, X.tOcc);
        qglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, b);
        free(b);
    }
    free(px);
}

static float falloff(float dist, float reach, float core){
    if(dist >= reach) return 0;
    float q = dist / reach, w = 1 - q * q; w *= w;
    return w / (1 + q * q / (core * core));
}

/* sparks rise from the torches; dust drifts through the room and shows where
 * the light is */
static void light_at(float x, float y, float *r, float *g, float *b);
static void puff(float x, float y, int n, float spread){
    float s = g_tile_w / 64.0f, lr, lg, lb; light_at(x, y - 4, &lr, &lg, &lb);
    for(int i = 0; i < n && g_np < MAXP; i++){
        Part *p = &g_p[g_np++];
        memset(p, 0, sizeof *p);
        p->kind = 2; p->x = x + (frand() - 0.5f) * spread * s; p->y = y - frand() * 3 * s;
        p->vx = (frand() - 0.5f) * 46 * s; p->vy = -(3 + frand() * 14) * s;
        p->life = 0.45f + frand() * 0.5f; p->size = (2.2f + frand() * 2.2f) * s; p->seed = frand() * 100;
        p->lr = lr; p->lg = lg; p->lb = lb;
    }
}
static void clash(float x, float y, int n){
    float s = g_tile_w / 64.0f;
    for(int i = 0; i < n && g_np < MAXP; i++){
        Part *p = &g_p[g_np++];
        memset(p, 0, sizeof *p);
        float a = frand() * 6.2832f, v = (40 + frand() * 110) * s;
        p->kind = 3; p->x = x; p->y = y; p->vx = cosf(a) * v; p->vy = sinf(a) * v - 30 * s;
        p->life = 0.18f + frand() * 0.3f; p->size = (0.5f + frand() * 0.5f) * s; p->seed = frand() * 100;
    }
    g_flash.x = x; g_flash.y = y; g_flash.age = 0; g_flash.on = 1;
}
/* What just happened, from the game's sound effects. */
static void events(void){
    const LightChar *k = &g_ch[0], *gd = &g_ch[1];
    for(int i = 0; i < g_nev; i++) switch(g_ev[i]){
    case 0:  if(k->there) puff(k->x, k->y, 16, 30); break;    /* land hard */
    case 16: if(k->there) puff(k->x, k->y, 9, 24); break;     /* land med */
    case 17: if(k->there) puff(k->x, k->y, 4, 18); break;     /* land soft */
    case 10: case 12: case 13:                                /* sword on sword, hit guard, hit kid */
        if(k->there && gd->there){
            float w = g_ev[i] == 10 ? 0.5f : g_ev[i] == 12 ? 0.85f : 0.15f;
            clash(k->x + (gd->x - k->x) * w, k->y - 0.44f * g_row_h, g_ev[i] == 10 ? 12 : 7);
        }
        break;
    }
    g_nev = 0;
}

static void particles_step(float dt, int room_changed){
    if(room_changed){
        g_np = 0;
        for(int i = 0; i < NDUST; i++){
            Part *p = &g_p[g_np++];
            memset(p, 0, sizeof *p);
            p->kind = 1; p->x = frand() * X.w; p->y = g_top + frand() * (g_status_y - g_top);
            p->seed = frand() * 100; p->size = 0.45f + frand() * 0.45f; p->life = 1e9f;
        }
    }
    for(int i = 0; i < g_nl; i++){
        if(g_l[i].kind != 0) continue;
        g_spawn[i] += dt * 3.0f;
        while(g_spawn[i] >= 1){
            g_spawn[i] -= 1;
            if(g_np >= MAXP) break;
            Part *p = &g_p[g_np++];
            memset(p, 0, sizeof *p);
            float s = g_tile_w / 64.0f;
            p->kind = 0; p->x = g_l[i].x + (frand() - 0.5f) * 10 * s; p->y = g_l[i].y - (2 + frand() * 12) * s;
            p->vx = (frand() - 0.5f) * 14 * s; p->vy = -(18 + frand() * 34) * s;
            p->life = 0.5f + frand() * 1.0f; p->seed = frand() * 100; p->size = 0.6f + frand() * 0.6f;
        }
    }
    for(int i = 0; i < g_np; ){
        Part *p = &g_p[i];
        p->age += dt;
        if(p->kind == 2){
            p->vx -= p->vx * 3.0f * dt; p->vy -= p->vy * 2.0f * dt;
            p->x += p->vx * dt; p->y += p->vy * dt; p->size += 5.0f * dt;
            if(p->age >= p->life){ *p = g_p[--g_np]; continue; }
        } else if(p->kind == 3){
            p->vy += 260 * dt * g_tile_w / 64.0f;
            p->x += p->vx * dt; p->y += p->vy * dt;
            if(p->age >= p->life){ *p = g_p[--g_np]; continue; }
        } else if(p->kind == 0){
            p->vy -= 10 * dt; p->vx += (noise1(p->age * 3 + p->seed, p->seed) * 30 - p->vx * 0.8f) * dt;
            p->x += p->vx * dt; p->y += p->vy * dt;
            if(p->age >= p->life || solid_at(p->x, p->y)){ *p = g_p[--g_np]; continue; }
        } else {
            float t = (float)g_time * 0.25f;
            p->vx = noise1(t + p->seed, p->seed) * 5; p->vy = noise1(t + p->seed + 37, p->seed + 11) * 3.5f - 0.6f;
            p->x += p->vx * dt; p->y += p->vy * dt;
            if(p->x < 0) p->x += X.w; if(p->x >= X.w) p->x -= X.w;
            if(p->y < g_top) p->y = g_status_y - 2; if(p->y > g_status_y - 2) p->y = g_top;
            /* the light on it */
            p->lr = p->lg = p->lb = 0;
            if(!solid_at(p->x, p->y)) light_at(p->x, p->y, &p->lr, &p->lg, &p->lb);
        }
        i++;
    }
}
/* the torch light at a spot (no shadows) */
static void light_at(float x, float y, float *r, float *g, float *b){
    *r = *g = *b = 0;
    for(int k = 0; k < g_nl; k++){
        float dx = g_l[k].x - x, dy = g_l[k].y - y, a = falloff(sqrtf(dx * dx + dy * dy), g_l[k].reach * 0.75f, 0.35f) * g_l[k].strength;
        *r += a * g_l[k].r; *g += a * g_l[k].g; *b += a * g_l[k].b;
    }
}
/* target: 0 the glow's source (HDR), 1 the picture */
static void particles_draw(int target){
    if(!g_np) return;
    qglUseProgram(X.pPart);
    qglEnable(GL_BLEND); qglBlendFunc(GL_ONE, GL_ONE);
    qglBegin(GL_QUADS);
    for(int i = 0; i < g_np; i++){
        const Part *p = &g_p[i];
        float r, g, b, a, sz = p->size;
        if(p->kind == 0){
            float u = p->age / p->life;
            r = 1.0f; g = 0.62f - 0.42f * u; b = 0.18f - 0.16f * u;
            a = (u < 0.1f ? u / 0.1f : 1 - (u - 0.1f) / 0.9f) * (0.75f + 0.25f * sinf(p->age * 23 + p->seed));
            a *= target ? 0.9f : 2.2f;
            sz *= 1 - 0.4f * u;
        } else if(p->kind == 2){
            float u = p->age / p->life;
            if(!target) continue;                          /* dust does not glow */
            r = 0.62f * (g_amb[0] + p->lr); g = 0.58f * (g_amb[1] + p->lg); b = 0.52f * (g_amb[2] + p->lb);
            a = 0.42f * (1 - u) * (u < 0.15f ? u / 0.15f : 1);
        } else if(p->kind == 3){
            float u = p->age / p->life;
            r = 1.0f; g = 0.9f - 0.3f * u; b = 0.6f - 0.5f * u; a = (1 - u) * (target ? 1.0f : 3.0f);
        } else {
            float tw = 0.6f + 0.4f * sinf((float)g_time * 1.7f + p->seed * 7);
            r = p->lr; g = p->lg; b = p->lb; a = tw * (target ? 0.32f : 0.3f);
            if(r + g + b < 0.02f) continue;
        }
        a *= g_level;
        float cx = p->x / X.w * 2 - 1, cy = p->y / X.h * 2 - 1, hx = sz * 1.6f / X.w * 2, hy = sz * 1.6f / X.h * 2;
        qglColor4f(r, g, b, a);
        qglTexCoord2f(-1, -1); qglVertex2f(cx - hx, cy - hy);
        qglTexCoord2f( 1, -1); qglVertex2f(cx + hx, cy - hy);
        qglTexCoord2f( 1,  1); qglVertex2f(cx + hx, cy + hy);
        qglTexCoord2f(-1,  1); qglVertex2f(cx - hx, cy + hy);
    }
    qglEnd();
    qglDisable(GL_BLEND);
    qglColor4f(1, 1, 1, 1);
}

static void set_common(GLuint p){
    qglUniform1i(U(p, "uPic"), 0); qglUniform1i(U(p, "uLight"), 1); qglUniform1i(U(p, "uAir"), 2);
    qglUniform3f(U(p, "uAmb"), g_amb[0], g_amb[1], g_amb[2]);
    qglUniform1f(U(p, "uBack"), T.back); qglUniform1f(U(p, "uFlame"), T.flame);
    qglUniform1f(U(p, "uStatusY"), g_status_y); qglUniform1f(U(p, "uSizeY"), (float)X.h);
    float bl[8]; int nb = 0;
    for(int i = 0; i < 2; i++) if(g_ch[i].there && g_ch[i].grounded && g_ch[i].h > 0 && g_opt.shadows){
        bl[nb * 4] = g_ch[i].x; bl[nb * 4 + 1] = g_ch[i].y + 2 * g_tile_w / 64; bl[nb * 4 + 2] = g_chw * 1.7f; bl[nb * 4 + 3] = g_chw * 0.5f; nb++; }
    qglUniform1i(U(p, "uNB"), nb);
    if(nb) qglUniform4fv(U(p, "uBlob"), nb, bl);
    qglUniform1f(U(p, "uContact"), T.contact);
}

int gfx_frame(SDL_Renderer *ren, const uint32_t *frame, int w, int h, const GfxOpts *o){
    g_live = 0;
    if(!X.ok) return 0;
    g_opt = *o;
    uint32_t now = SDL_GetTicks();
    float dt = g_last_ms ? (now - g_last_ms) / 1000.0f : 0;
    if(dt > 0.1f) dt = 0.1f;
    g_last_ms = now; g_time += dt;
    g_level = light_level();
    unsigned char tiles[40]; float tw, top, rh;
    int key = light_room(tiles, &tw, &top, &rh);
    if(g_level <= 0.004f || !key){ g_room_key = -1; return 0; }

    /* the lights now */
    LightSrc src[MAXL]; int n = light_sources(src, MAXL), torches = 0;
    g_nl = n;
    for(int i = 0; i < n; i++){
        float ph = src[i].phase, t = (float)g_time;
        float f = 1 + T.flicker * (0.5f * sinf(t * 6.9f + ph) + 0.3f * sinf(t * 17.0f + 1.7f * ph) + 0.45f * noise1(t * 9, ph * 3.1f));
        g_l[i].kind = src[i].kind; g_l[i].phase = ph;
        g_l[i].x = src[i].x + noise1(t * 6, ph + 5) * 1.5f; g_l[i].y = src[i].y + noise1(t * 6, ph + 9) * 1.5f;
        if(src[i].kind == 0){
            g_l[i].r = T.torch[0]; g_l[i].g = T.torch[1]; g_l[i].b = T.torch[2];
            g_l[i].strength = T.torch_i * f; g_l[i].reach = T.torch_r * tw; torches++;
        } else {
            /* the liquid's colour, brightened to full */
            float m = src[i].r > src[i].g ? (src[i].r > src[i].b ? src[i].r : src[i].b) : (src[i].g > src[i].b ? src[i].g : src[i].b);
            if(m < 0.01f) m = 1;
            g_l[i].r = src[i].r / m; g_l[i].g = src[i].g / m; g_l[i].b = src[i].b / m;
            g_l[i].strength = T.potion_i * (1 + 0.08f * sinf(t * 8.8f + ph)); g_l[i].reach = T.potion_r * tw;
        }
    }
    /* the characters, and what just happened */
    light_chars(g_ch);
    g_chw = 11 * tw / 64;
    if(g_flash.on){
        g_flash.age += dt;
        if(g_flash.age > 0.16f) g_flash.on = 0;
        else if(n < MAXL){
            float u = 1 - g_flash.age / 0.16f;
            g_l[n].kind = 2; g_l[n].phase = 0; g_l[n].x = g_flash.x; g_l[n].y = g_flash.y;
            g_l[n].r = 1.0f; g_l[n].g = 0.9f; g_l[n].b = 0.7f; g_l[n].strength = 1.3f * u * u; g_l[n].reach = 2.6f * tw;
            n++; g_nl = n;
        }
    }
    /* a room without a torch keeps more of its own light */
    float want = torches ? 0 : 1, step = dt / 0.5f;
    if(g_room_key < 0) g_dark = want;
    else g_dark = g_dark < want ? (g_dark + step > want ? want : g_dark + step) : (g_dark - step < want ? want : g_dark - step);
    for(int c = 0; c < 3; c++) g_amb[c] = T.amb_torch[c] + (T.amb_dark[c] - T.amb_torch[c]) * g_dark;

    SDL_RenderFlush(ren);
    Saved s; save(&s);
    if(!make_targets(w, h)){ restore(&s); X.ok = 0; return 0; }
    int room_changed = key != g_room_key, tiles_changed = room_changed || memcmp(tiles, g_tiles, 40) != 0;
    unsigned char old[40]; int old_ok = g_room_key >= 0;
    memcpy(old, g_tiles, 40);
    g_tile_w = tw; g_top = top; g_row_h = rh; g_status_y = top + 3 * rh + 7 * tw / 64;
    memcpy(g_tiles, tiles, 40); g_room_key = key;
    qglActiveTexture(GL_TEXTURE0);
    qglPixelStorei(GL_UNPACK_ALIGNMENT, 4); qglPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    qglPixelStorei(GL_UNPACK_SKIP_PIXELS, 0); qglPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    if(tiles_changed) build_occluders();
    if(g_opt.particles){
        /* a loose floor gone from its place: dust there; debris where there was none: more */
        if(!room_changed && old_ok) for(int i = 10; i < 40; i++){
            float x = g_tile_w * ((i - 10) % 10 + 0.5f), y = g_top + g_row_h * ((i - 10) / 10 + 1) - 4;
            if(old[i] == 11 && tiles[i] == 0) puff(x, y, 10, 50);
            if(old[i] != 14 && tiles[i] == 14) puff(x, y - 6, 22, 56);
        }
        events();
        {   /* POPGFXTEST=puff@6 or clash@6: that effect at the Prince, at that second (headless checks) */
            static int done; const char *e = getenv("POPGFXTEST"), *at = e ? strchr(e, '@') : NULL;
            if(at && !done && SDL_GetTicks() >= (uint32_t)(1000 * atof(at + 1)) && g_ch[0].there){ done = 1;
                if(e[0] == 'p') puff(g_ch[0].x, g_ch[0].y, 16, 30);
                else clash(g_ch[0].x + 20, g_ch[0].y - 0.44f * g_row_h, 12); }
        }
    } else g_nev = 0;
    qglBindTexture(GL_TEXTURE_2D, X.tFrame);
    qglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, frame);
    if(g_opt.particles) particles_step(dt, room_changed); else g_np = 0;

    /* 2: light */
    float lpos[MAXL * 4], lcol[MAXL * 4];
    for(int i = 0; i < n; i++){
        lpos[i * 4] = g_l[i].x; lpos[i * 4 + 1] = g_l[i].y; lpos[i * 4 + 2] = g_l[i].reach; lpos[i * 4 + 3] = g_l[i].strength;
        lcol[i * 4] = g_l[i].r; lcol[i * 4 + 1] = g_l[i].g; lcol[i * 4 + 2] = g_l[i].b; lcol[i * 4 + 3] = (float)g_l[i].kind;
    }
    qglBindFramebuffer(GL_FRAMEBUFFER, X.fLight);
    qglViewport(0, 0, w, h);
    qglUseProgram(X.pLight);
    bind(0, X.tFrame); bind(1, X.tOcc);
    qglUniform1i(U(X.pLight, "uFrame"), 0); qglUniform1i(U(X.pLight, "uOcc"), 1);
    qglUniform2f(U(X.pLight, "uSize"), (float)w, (float)h);
    qglUniform1i(U(X.pLight, "uN"), n);
    if(n){ qglUniform4fv(U(X.pLight, "uLpos"), n, lpos); qglUniform4fv(U(X.pLight, "uLcol"), n, lcol); }
    qglUniform1f(U(X.pLight, "uRelief"), g_opt.relief ? T.relief : 0);
    qglUniform1f(U(X.pLight, "uShadow"), g_opt.shadows ? 1.0f : 0.0f);
    qglUniform1f(U(X.pLight, "uPen"), T.pen * tw / 64); qglUniform1f(U(X.pLight, "uLz"), T.lz * tw / 64);
    qglUniform1f(U(X.pLight, "uStatusY"), g_status_y); qglUniform1f(U(X.pLight, "uAir"), T.air);
    qglUniform1f(U(X.pLight, "uTileW"), tw);
    {   float ch[8]; int nc = 0;
        for(int i = 0; i < 2; i++) if(g_ch[i].there && g_ch[i].h > 0){
            ch[nc * 4] = g_ch[i].x; ch[nc * 4 + 1] = g_ch[i].y; ch[nc * 4 + 2] = g_ch[i].h; ch[nc * 4 + 3] = g_chw; nc++; }
        qglUniform1i(U(X.pLight, "uNC"), nc);
        if(nc) qglUniform4fv(U(X.pLight, "uChar"), nc, ch);
        qglUniform1f(U(X.pLight, "uCharK"), T.char_shadow);
        qglUniform1f(U(X.pLight, "uAO"), g_opt.shadows ? T.ao : 0);
    }
    quad();

    /* 3: glow */
    if(g_opt.bloom){
        qglBindFramebuffer(GL_FRAMEBUFFER, X.fSrc);
        qglUseProgram(X.pSrc);
        bind(0, X.tFrame); bind(1, X.tLight); bind(2, X.tAir);
        set_common(X.pSrc);
        qglUniform1f(U(X.pSrc, "uThresh"), T.thresh);
        quad();
        particles_draw(0);
        qglUseProgram(X.pDown);
        qglUniform1i(U(X.pDown, "uPic"), 0);
        for(int i = 0; i < NB; i++){
            qglBindFramebuffer(GL_FRAMEBUFFER, X.fBloom[i]);
            qglViewport(0, 0, X.bw[i], X.bh[i]);
            bind(0, i ? X.tBloom[i - 1] : X.tSrc);
            qglUniform2f(U(X.pDown, "uTexel"), 1.0f / (i ? X.bw[i - 1] : w), 1.0f / (i ? X.bh[i - 1] : h));
            quad();
        }
        qglUseProgram(X.pUp);
        qglUniform1i(U(X.pUp, "uPic"), 0);
        qglEnable(GL_BLEND); qglBlendFunc(GL_ONE, GL_ONE);
        for(int i = NB - 2; i >= 0; i--){
            qglBindFramebuffer(GL_FRAMEBUFFER, X.fBloom[i]);
            qglViewport(0, 0, X.bw[i], X.bh[i]);
            bind(0, X.tBloom[i + 1]);
            qglUniform2f(U(X.pUp, "uTexel"), 1.0f / X.bw[i + 1], 1.0f / X.bh[i + 1]);
            quad();
        }
        qglDisable(GL_BLEND);
    }
    restore(&s);
    g_live = 1;
    return 1;
}

void gfx_apply(SDL_Renderer *ren, SDL_Texture *target){
    if(!X.ok || !g_live || !target) return;
    int tw, th; float fw, fh;
    if(SDL_QueryTexture(target, NULL, NULL, &tw, &th) != 0) return;
    SDL_RenderFlush(ren);
    /* SDL's texture, by its GL name */
    if(SDL_GL_BindTexture(target, &fw, &fh) != 0) return;
    GLint stage = 0; qglGetIntegerv(GL_TEXTURE_BINDING_2D, &stage);
    SDL_GL_UnbindTexture(target);
    if(!stage || fw != 1.0f || fh != 1.0f) return;          /* a rectangle texture: not handled */
    Saved s; save(&s);
    if(!s.fb){ restore(&s); return; }                        /* the target is not bound */
    int ci = (tw == X.cw[0] && th == X.ch[0]) ? 0 : (tw == X.cw[1] && th == X.ch[1]) ? 1 : (X.tCopy[0] ? 1 : 0);
    qglActiveTexture(GL_TEXTURE0);
    if(X.cw[ci] != tw || X.ch[ci] != th){
        if(X.tCopy[ci]) qglDeleteTextures(1, &X.tCopy[ci]);
        X.tCopy[ci] = texture(tw, th, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
        X.cw[ci] = tw; X.ch[ci] = th;
    }
    /* the picture as it is, copied out of the target, then drawn back lit */
    qglBindTexture(GL_TEXTURE_2D, X.tCopy[ci]);
    qglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, tw, th);
    qglViewport(0, 0, tw, th);
    GLuint p = X.pFinal;
    qglUseProgram(p);
    bind(0, X.tCopy[ci]); bind(1, X.tLight); bind(2, X.tAir); bind(3, X.tBloom[0]);
    set_common(p);
    qglUniform1i(U(p, "uBloom"), 3);
    qglUniform2f(U(p, "uSize"), (float)X.w, (float)X.h);
    qglUniform2f(U(p, "uCentre"), X.w * 0.5f, (g_top + g_status_y) * 0.5f);
    float lpos[MAXL * 4];
    for(int i = 0; i < g_nl; i++){ lpos[i * 4] = g_l[i].x; lpos[i * 4 + 1] = g_l[i].y; lpos[i * 4 + 2] = (float)g_l[i].kind; lpos[i * 4 + 3] = g_l[i].phase; }
    qglUniform1i(U(p, "uN"), g_nl);
    if(g_nl) qglUniform4fv(U(p, "uLpos"), g_nl, lpos);
    qglUniform1f(U(p, "uLevel"), g_level);
    qglUniform1f(U(p, "uBloomK"), g_opt.bloom ? T.bloom : 0);
    qglUniform1f(U(p, "uHaze"), g_opt.haze ? T.haze : 0);
    qglUniform1f(U(p, "uVig"), g_opt.vignette ? T.vignette : 0);
    qglUniform1f(U(p, "uTime"), (float)fmod(g_time, 1000.0));
    qglUniform1f(U(p, "uTileW"), g_tile_w);
    {   static int dbg = -1; if(dbg < 0) dbg = getenv("POPGFXDEBUG") != NULL;
        qglUniform1f(U(p, "uDebug"), (float)dbg);
        if(dbg){
            bind(4, X.tOcc); qglUniform1i(U(p, "uOcc"), 4);
            float ch[8]; int nc = 0;
            for(int i = 0; i < 2; i++) if(g_ch[i].there && g_ch[i].h > 0){
                ch[nc * 4] = g_ch[i].x; ch[nc * 4 + 1] = g_ch[i].y; ch[nc * 4 + 2] = g_ch[i].h; ch[nc * 4 + 3] = g_chw; nc++; }
            qglUniform1i(U(p, "uNC"), nc);
            if(nc) qglUniform4fv(U(p, "uChar"), nc, ch);
        }
    }
    quad();
    if(g_opt.particles) particles_draw(1);
    restore(&s);
}
