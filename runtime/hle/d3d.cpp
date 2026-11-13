// HLE of the Xbox Direct3D 8 (XDK 4928) entry points the game calls.
//
// Resources are real guest structures laid out like the XDK ones (the game
// reads them directly) and their memory comes from the contiguous region.
// State calls record what the push buffer would have told the NV2A: render
// state by NV2A method, the 136-slot vertex program store, the 192 vertex
// constants, the vertex attribute format, the pixel shader, the bound
// textures. Each draw hands a snapshot to the Vulkan renderer (runtime/gpu).
// Texture stage states are written by the game straight into
// D3D__DeferredTextureState, so draws read them from there.
//
// XDK layouts: D3DResource { Common, Data, Lock }; D3DPixelContainer adds
// { Format, Size }; D3DSurface adds Parent. Data holds a physical offset
// (guest address - 0x80000000) once registered.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "gpu/gpu.h"
#include "gpu/window.h"
#include "guest.h"
#include "kobj.h"

namespace tje {

constexpr uint32_t kActiveCamera = 0x263880;

void log_backtrace(Cpu*, const char*);

namespace {

#define D3DLOG(c, name) do { if (log_enabled("d3dcall")) hle_log("[d3d] %s(%08x %08x %08x %08x %08x %08x) ecx %08x edx %08x", name, A(c,0), A(c,1), A(c,2), A(c,3), A(c,4), A(c,5), c->ecx, c->edx); } while (0)

enum : uint32_t {
    COMMON_TYPE_VB = 0x00000000, COMMON_TYPE_PALETTE = 0x00030000, COMMON_TYPE_TEXTURE = 0x00040000,
    COMMON_TYPE_SURFACE = 0x00050000, COMMON_D3DCREATED = 0x01000000,
};
constexpr uint32_t XG_SET_TEXTURE_HEADER = 0x001ab608;   // lifted XGRAPHC::XGSetTextureHeader

// D3D globals the game touches directly.
constexpr uint32_t D3D_TEXTURE_STATE = 0x00185b20;       // D3D__DeferredTextureState[4][32]
constexpr uint32_t D3D_RENDER_STATE = 0x00185d20;        // D3D__RenderState[]
constexpr uint32_t D3D_VSHADER_CONSTANTS = 0x00183b28;   // shadow of the 192 vertex constants
constexpr uint32_t RS_ZENABLE = 143, RS_FRONTFACE = 146, RS_CULLMODE = 147;

uint32_t g_device = 0, g_backbuffer = 0, g_depth = 0, g_render_target = 0, g_zbuffer = 0;
uint32_t g_frames = 0, g_draws = 0, g_vblank_waits = 0;
}  // namespace
std::atomic<uint32_t> g_swaps{0};         // read by the stall detector (main.cpp)
namespace {

// ---- NV2A-side state
uint32_t g_method[0x800];                 // render state by NV2A method / 4
float g_vconst[192][4];                   // vertex constants, biased register index
uint32_t g_program[136][4];               // vertex program store
uint32_t g_program_start = 0;
gpu::VertexAttr g_vaf[16];
gpu::Stream g_streams[16];
float g_vertex_data[16][4];               // current value of attributes without a stream
uint32_t g_textures[4] = {};
uint32_t g_pixel_shader = 0;              // handle from CreatePixelShader (0 = fixed function)
uint32_t g_texture_factor = 0, g_fog_color = 0, g_shadow_func = 0x203;
uint32_t g_ps_constants[8];               // SetPixelShaderConstant values (D3DCOLOR), per register
uint8_t g_ps_constants_set = 0;           // registers set since SetPixelShader
uint32_t g_dump_frame = 0;                // TJE_DRAW_FRAME: log every draw of this frame
uint32_t g_constant_mode = 0;

struct VertexShader { uint32_t function = 0; };
std::map<uint32_t, VertexShader> g_vshaders;

// Immediate mode (Begin / SetVertexData* / End).
bool g_in_begin = false;
uint32_t g_begin_prim = 0;
std::vector<float> g_immediate;           // 16 x vec4 per vertex
uint32_t g_immediate_mask = 0;

// Bits per pixel and whether the format is linear (pitch in the Size word).
struct Fmt { int bpp; bool linear; bool compressed; };
Fmt fmt_info(uint32_t f) {
    switch (f) {
    case 0x00: case 0x01: case 0x0b: case 0x19: return {8, false, false};
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1a: case 0x24: case 0x25: case 0x27: case 0x28:
    case 0x29: case 0x2c: case 0x2d: case 0x32: case 0x38: case 0x39: return {16, false, false};
    case 0x06: case 0x07: case 0x2a: case 0x2b: case 0x33: case 0x3a: case 0x3b: return {32, false, false};
    case 0x0c: return {4, false, true};
    case 0x0e: case 0x0f: return {8, false, true};
    case 0x13: case 0x1b: case 0x1f: return {8, true, false};
    case 0x10: case 0x11: case 0x16: case 0x17: case 0x1c: case 0x1d: case 0x20: case 0x30: case 0x31:
    case 0x35: case 0x37: return {16, true, false};
    case 0x12: case 0x1e: case 0x2e: case 0x2f: case 0x36: case 0x3f: return {32, true, false};
    default: return {32, false, false};
    }
}

struct Desc { uint32_t format, width, height, depth, levels, pitch; };

Desc describe(uint32_t res) {
    uint32_t fw = R32(res + 0x0c), size = R32(res + 0x10);
    Desc d{};
    d.format = (fw >> 8) & 0xff;
    d.levels = (fw >> 16) & 0xf;
    if (size) {                                   // linear: explicit size and pitch
        d.width = (size & 0xfff) + 1;
        d.height = ((size >> 12) & 0xfff) + 1;
        d.pitch = (((size >> 24) & 0xff) + 1) * 64;
        d.depth = 1;
    } else {
        d.width = 1u << ((fw >> 20) & 0xf);
        d.height = 1u << ((fw >> 24) & 0xf);
        d.depth = 1u << ((fw >> 28) & 0xf);
        d.pitch = d.width * fmt_info(d.format).bpp / 8;
    }
    if (!d.levels) d.levels = 1;
    return d;
}

// Depth buffer range of a depth format (D3D scales viewport z by it).
float depth_range(uint32_t format) {
    return (format == 0x2c || format == 0x2d || format == 0x30 || format == 0x31) ? 65535.0f : 16777215.0f;
}

uint32_t level_bytes(const Desc& d, uint32_t w, uint32_t h) {
    Fmt f = fmt_info(d.format);
    if (f.compressed) return ((w + 3) / 4) * ((h + 3) / 4) * (f.bpp == 4 ? 8 : 16);
    if (f.linear) return d.pitch * h;
    return w * h * f.bpp / 8;
}

uint32_t total_bytes(const Desc& d) {
    uint32_t total = 0, w = d.width, h = d.height;
    for (uint32_t l = 0; l < d.levels; l++) {
        total += level_bytes(d, w, h) * d.depth;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

uint32_t data_ptr(uint32_t res) {                // guest address of a resource's data
    uint32_t data = R32(res + 4);
    return data < 0x80000000u ? data + 0x80000000u : data;
}

uint32_t new_resource(uint32_t size, uint32_t common) {
    uint32_t r = guest_alloc(size);
    W32(r, common | COMMON_D3DCREATED | 1);
    return r;
}

// A linear surface of its own memory (render targets).
uint32_t make_surface(uint32_t width, uint32_t height, uint32_t format, uint32_t bpp) {
    uint32_t s = new_resource(0x18, COMMON_TYPE_SURFACE);
    uint32_t pitch = (width * bpp / 8 + 63) & ~63u;
    uint32_t mem = guest_alloc_contiguous(pitch * height, 0x1000);
    W32(s + 4, mem - 0x80000000u);
    W32(s + 0x0c, 0x00010001u | (format << 8));
    W32(s + 0x10, (width - 1) | ((height - 1) << 12) | ((pitch / 64 - 1) << 24));
    return s;
}

uint32_t data_ptr(uint32_t res);

void select_render_target() {
    Desc c = describe(g_render_target);
    Desc z = g_zbuffer ? describe(g_zbuffer) : Desc{};
    gpu::set_render_target(data_ptr(g_render_target), c.width, c.height, g_zbuffer ? data_ptr(g_zbuffer) : 0, z.width,
                           z.height, g_render_target == g_backbuffer);
}

void addref(uint32_t r) { if (r) W32(r, (R32(r) & 0xffff0000u) | ((R32(r) + 1) & 0xffff)); }

uint32_t& method(uint32_t m) { return g_method[(m >> 2) & 0x7ff]; }

float f32(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

// D3D's state after CreateDevice, as far as the renderer reads it.
void reset_state() {
    std::memset(g_method, 0, sizeof g_method);
    method(0x304) = 0;           // blend
    method(0x344) = 1;           // ONE
    method(0x348) = 0;           // ZERO
    method(0x350) = 0x8006;      // FUNC_ADD
    method(0x30c) = 1;           // depth test
    method(0x354) = 0x203;       // LEQUAL
    method(0x35c) = 1;           // depth write
    method(0x32c) = 0;           // stencil test
    method(0x364) = 0x207;
    method(0x36c) = 0xff;
    method(0x360) = 0xff;
    method(0x370) = method(0x374) = method(0x378) = 0x1e00;   // KEEP
    method(0x358) = 0x01010101;  // colour mask
    method(0x308) = 1;           // cull CCW
    method(0x39c) = 0x405;
    method(0x3a0) = 0x900;
    method(0x300) = 0;           // alpha test
    method(0x33c) = 0x207;
    W32(D3D_RENDER_STATE + RS_ZENABLE * 4, 1);
    W32(D3D_RENDER_STATE + RS_FRONTFACE * 4, 0x900);
    W32(D3D_RENDER_STATE + RS_CULLMODE * 4, 0x901);
    // Texture stages: D3D defaults (Xbox D3DTOP numbering: DISABLE 1, SELECTARG1 2, MODULATE 4).
    for (uint32_t s = 0; s < 4; s++) {
        uint32_t b = D3D_TEXTURE_STATE + s * 0x80;
        for (uint32_t i = 0; i < 32; i++) W32(b + i * 4, 0);
        W32(b + 0 * 4, 1); W32(b + 1 * 4, 1); W32(b + 2 * 4, 1);          // ADDRESSU/V/W = WRAP
        W32(b + 3 * 4, 1); W32(b + 4 * 4, 1);                              // MAG/MIN = POINT
        W32(b + 8 * 4, 1);                                                 // MAXANISOTROPY
        W32(b + 12 * 4, s == 0 ? 4 : 1);                                   // COLOROP
        W32(b + 13 * 4, 1); W32(b + 14 * 4, 2); W32(b + 15 * 4, 1);        // COLORARG0/1/2
        W32(b + 16 * 4, s == 0 ? 2 : 1);                                   // ALPHAOP
        W32(b + 17 * 4, 1); W32(b + 18 * 4, 2); W32(b + 19 * 4, 1);        // ALPHAARG0/1/2
        W32(b + 20 * 4, 1);                                                // RESULTARG = CURRENT
        W32(b + 28 * 4, s);                                                // TEXCOORDINDEX
    }
    for (int i = 0; i < 16; i++) {
        g_vaf[i] = gpu::VertexAttr{};
        g_vertex_data[i][0] = g_vertex_data[i][1] = g_vertex_data[i][2] = 0.0f;
        g_vertex_data[i][3] = 1.0f;
    }
    for (int k = 0; k < 4; k++) g_vertex_data[3][k] = g_vertex_data[4][k] = 1.0f;   // diffuse, specular
}

// D3DVSDT_* size in bytes.
uint32_t vsdt_size(uint32_t t) {
    uint32_t n = (t >> 4) & 15;
    switch (t & 15) {
    case 2: return t == 0x72 ? 12 : 4 * n;
    case 0: return t == 0x40 ? 4 : 0;
    case 1: case 5: return 2 * n;
    case 4: return n;
    case 6: return 4;
    default: return 0;
    }
}

// Vertex attribute format from a vertex shader object (handle & ~1) + 0x14.
void load_vaf(uint32_t obj) {
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t e = obj + 0x14 + i * 16;
        g_vaf[i].stream = (uint8_t)R32(e);
        g_vaf[i].offset = (uint16_t)R32(e + 4);
        g_vaf[i].format = (uint8_t)R32(e + 8);
    }
}

void load_program(uint32_t function, uint32_t address) {
    uint32_t n = R16(function + 2);
    for (uint32_t i = 0; i < n && address + i < 136; i++)
        for (int k = 0; k < 4; k++) g_program[address + i][k] = R32(function + 4 + i * 16 + k * 4);
}

// Copies `dwords` dwords of constant data starting at (biased) register `reg`.
void set_constants(uint32_t reg, uint32_t src, uint32_t dwords) {
    for (uint32_t i = 0; i < dwords && reg * 4 + i < 192 * 4; i++) {
        uint32_t v = R32(src + i * 4);
        std::memcpy(&g_vconst[0][0] + reg * 4 + i, &v, 4);
        W32(D3D_VSHADER_CONSTANTS + reg * 16 + i * 4, v);
    }
}

gpu::TextureRef texture_ref(uint32_t t) {
    gpu::TextureRef r;
    if (!t) return r;
    Desc d = describe(t);
    r.addr = data_ptr(t);
    r.format = d.format;
    r.width = d.width;
    r.height = d.height;
    r.depth = d.depth;
    r.levels = d.levels;
    r.pitch = fmt_info(d.format).linear ? d.pitch : 0;
    r.cube = (R32(t + 0x0c) & 4) != 0;
    return r;
}

// Snapshot of everything a draw needs.
void build_draw(gpu::Draw& d, uint32_t prim, uint32_t count) {
    d.prim = prim;
    d.count = count;
    for (int i = 0; i < 16; i++) {
        d.attrs[i] = g_vaf[i];
        d.streams[i] = g_streams[i];
        std::memcpy(d.attr_default[i], g_vertex_data[i], 16);
    }
    uint32_t len = 0;
    while (g_program_start + len < 136) {
        len++;
        if (g_program[g_program_start + len - 1][3] & 1) break;
    }
    d.program = &g_program[g_program_start][0];
    d.program_len = len;
    d.constants = &g_vconst[0][0];
    d.fixed_function_ps = g_pixel_shader == 0;
    if (g_pixel_shader) {
        std::memcpy(d.psdef, gptr(R32(g_pixel_shader + 8)), 0xf0);
        // Constant registers set by SetPixelShaderConstant go where the shader maps them.
        uint32_t c0map = d.psdef[0xe4 / 4], c1map = d.psdef[0xe8 / 4], fcc = d.psdef[0xec / 4];
        for (uint32_t r = 0; r < 8; r++) {
            if (!(g_ps_constants_set & (1u << r))) continue;
            for (int i = 0; i < 8; i++) {
                if (((c0map >> (4 * i)) & 15) == r) d.psdef[0x28 / 4 + i] = g_ps_constants[r];
                if (((c1map >> (4 * i)) & 15) == r) d.psdef[0x48 / 4 + i] = g_ps_constants[r];
            }
            if ((fcc & 15) == r) d.psdef[0xac / 4] = g_ps_constants[r];
            if (((fcc >> 4) & 15) == r) d.psdef[0xb0 / 4] = g_ps_constants[r];
        }
    }
    std::memcpy(d.tss, gptr(D3D_TEXTURE_STATE), sizeof d.tss);
    for (int i = 0; i < 4; i++) {
        d.tex[i] = texture_ref(g_textures[i]);
        const uint32_t* t = d.tss[i];
        d.samp[i].address_u = (uint8_t)t[0];
        d.samp[i].address_v = (uint8_t)t[1];
        d.samp[i].address_w = (uint8_t)t[2];
        d.samp[i].mag = (uint8_t)t[3];
        d.samp[i].min = (uint8_t)t[4];
        d.samp[i].mip = (uint8_t)t[5];
        d.samp[i].border = t[29];
    }
    gpu::RasterState& rs = d.rs;
    rs.blend_enable = method(0x304);
    rs.blend_src = method(0x344);
    rs.blend_dst = method(0x348);
    rs.blend_eq = method(0x350);
    rs.blend_color = method(0x34c);
    rs.depth_test = method(0x30c);
    rs.depth_func = method(0x354);
    rs.depth_write = method(0x35c);
    rs.stencil_test = method(0x32c);
    rs.stencil_func = method(0x364);
    rs.stencil_ref = method(0x368);
    rs.stencil_read_mask = method(0x36c);
    rs.stencil_write_mask = method(0x360);
    rs.stencil_fail = method(0x370);
    rs.stencil_zfail = method(0x374);
    rs.stencil_zpass = method(0x378);
    rs.color_mask = method(0x358);
    rs.cull_enable = method(0x308);
    rs.cull_face = method(0x39c);
    rs.front_face = method(0x3a0);
    rs.poly_offset = method(0x338);
    rs.poly_factor = f32(method(0x384));
    rs.poly_units = f32(method(0x388));
    // The constant offset counts the depth buffer's steps: a 16-bit target's are
    // 256 of the renderer's 24-bit ones.
    if (g_zbuffer && depth_range(describe(g_zbuffer).format) == 65535.0f) rs.poly_units *= 256.0f;
    d.alpha_test = method(0x300);
    d.alpha_func = method(0x33c);
    d.alpha_ref = method(0x340);
    d.texture_factor = g_texture_factor;
    d.shadow_func = g_shadow_func;
    // Deferred render states 92.. : FOGENABLE, FOGTABLEMODE, FOGSTART, FOGEND, FOGDENSITY
    d.fog_enable = R32(D3D_RENDER_STATE + 92 * 4);
    d.fog_mode = R32(D3D_RENDER_STATE + 93 * 4);
    if (!d.fog_mode) d.fog_mode = 3;
    d.fog_start = f32(R32(D3D_RENDER_STATE + 94 * 4));
    d.fog_end = f32(R32(D3D_RENDER_STATE + 95 * 4));
    d.fog_density = f32(R32(D3D_RENDER_STATE + 96 * 4));
    Desc rt = describe(g_render_target);
    d.surface_w = (float)rt.width;
    d.surface_h = (float)rt.height;
    d.flat = R32(kActiveCamera + 0x3c) == 0x3f800000;     // an orthographic projection: the game's 2D camera
    d.zmax = g_zbuffer ? depth_range(describe(g_zbuffer).format) : 16777215.0f;
    d.fog_color = g_fog_color;
}

void log_draw_once(const char* what, const gpu::Draw& d) {
    if (g_dump_frame && g_frames == g_dump_frame && std::getenv("TJE_DUMP_TEX")) {
        // Raw texture bytes of this draw, for offline numeric checks of the decoders.
        static int n = 0;
        for (int i = 0; i < 4; i++) {
            if (!d.tex[i].addr) continue;
            char name[128];
            std::snprintf(name, sizeof name, "%s/tex_%03d_%d_%02x_%ux%u.bin", std::getenv("TJE_DUMP_TEX"), n, i,
                          d.tex[i].format, d.tex[i].width, d.tex[i].height);
            if (FILE* f = std::fopen(name, "wb")) {
                Desc dd = describe(g_textures[i]);
                std::fwrite(gptr(d.tex[i].addr), 1, total_bytes(dd), f);
                std::fclose(f);
            }
        }
        n++;
    }
    if (g_dump_frame && g_frames == g_dump_frame) {
        hle_log("[dump] %s prim %u count %u idx %08x vs@%u/%u ps %08x ff %d rt %08x fog %u/%u %g-%g color %08x", what, d.prim, d.count, d.index_addr,
                g_program_start, d.program_len, g_pixel_shader, d.fixed_function_ps, data_ptr(g_render_target),
                R32(0x185e90), R32(0x185e94), f32(R32(0x185e98)), f32(R32(0x185e9c)), g_fog_color);
        for (int i = 0; i < 16; i++)
            if (d.attrs[i].format != 2)
                hle_log("[dump]   attr %d: stream %u +%u fmt %02x (stream addr %08x stride %u)", i, d.attrs[i].stream,
                        d.attrs[i].offset, d.attrs[i].format, d.streams[d.attrs[i].stream & 15].addr,
                        d.streams[d.attrs[i].stream & 15].stride);
        for (int i = 0; i < 4; i++)
            if (d.tex[i].addr)
                hle_log("[dump]   tex%d %08x fmt %02x %ux%u levels %u pitch %u cube %d | tss addr %u %u filt %u %u %u op %u %u/%u %u %u/%u tci %u",
                        i, d.tex[i].addr, d.tex[i].format, d.tex[i].width, d.tex[i].height, d.tex[i].levels,
                        d.tex[i].pitch, d.tex[i].cube, d.tss[i][0], d.tss[i][1], d.tss[i][3], d.tss[i][4], d.tss[i][5],
                        d.tss[i][12], d.tss[i][14], d.tss[i][15], d.tss[i][16], d.tss[i][18], d.tss[i][19], d.tss[i][28]);
        static std::set<uint32_t> full_dumped;
        if (!d.fixed_function_ps && full_dumped.insert(g_pixel_shader).second) {
            std::string w;
            for (int i = 0; i < 60; i++) { char b[12]; std::snprintf(b, sizeof b, " %08x", d.psdef[i]); w += b; }
            hle_log("[dump]   psdef-full %08x:%s", g_pixel_shader, w.c_str());
        }
        if (!d.fixed_function_ps)
            hle_log("[dump]   psdef cnt %08x modes %08x c0map %08x c1map %08x fcc %08x final %08x %08x c0 %08x c1 %08x",
                    d.psdef[0xd4 / 4], d.psdef[0xd8 / 4], d.psdef[0xe4 / 4], d.psdef[0xe8 / 4], d.psdef[0xec / 4],
                    d.psdef[8], d.psdef[9], d.psdef[0x28 / 4], d.psdef[0x48 / 4]);
        for (int r : {14, 191})
            hle_log("[dump]   c%d: %g %g %g %g", r, g_vconst[r][0], g_vconst[r][1], g_vconst[r][2], g_vconst[r][3]);
        if (d.up_data && d.streams[0].stride >= 36)
            for (uint32_t v = 0; v < d.count && v < 4; v++) {
                const float* f = (const float*)(d.up_data + v * d.streams[0].stride);
                hle_log("[dump]   v%u: %g %g %g | %g %g %g %g | %g %g", v, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]);
            }
        const gpu::RasterState& r = d.rs;
        if (r.stencil_test)
            hle_log("[dump]   stencil func %x ref %u mask %x/%x fail %x zfail %x zpass %x", r.stencil_func, r.stencil_ref,
                    r.stencil_read_mask, r.stencil_write_mask, r.stencil_fail, r.stencil_zfail, r.stencil_zpass);
        if (r.poly_offset) hle_log("[dump]   polygon offset %g %g", r.poly_factor, r.poly_units);
        hle_log("[dump]   blend %u %x %x %x depth %u %x %u stencil %u cull %u %x %x mask %08x atest %u %x %u",
                r.blend_enable, r.blend_src, r.blend_dst, r.blend_eq, r.depth_test, r.depth_func, r.depth_write,
                r.stencil_test, r.cull_enable, r.cull_face, r.front_face, r.color_mask, d.alpha_test, d.alpha_func,
                d.alpha_ref);
    }
    if (!log_enabled("draw")) return;
    hle_log("[d3d] %s prim %u count %u vs@%u (%u instr) ps %08x tex %08x/%02x %08x %08x %08x", what, d.prim, d.count,
            g_program_start, d.program_len, g_pixel_shader, d.tex[0].addr, d.tex[0].format, d.tex[1].addr,
            d.tex[2].addr, d.tex[3].addr);
}

}  // namespace

// Direct3D_CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice)
void hle_0016b3e0(Cpu* c) { D3DLOG(c, "Direct3D_CreateDevice");
    uint32_t pp = A(c, 4), out = A(c, 5);
    uint32_t w = R32(pp) ? R32(pp) : 640, h = R32(pp + 4) ? R32(pp + 4) : 480;
    if (!g_device) {
        g_device = guest_alloc(0x1000);
        g_backbuffer = make_surface(w, h, 0x1e, 32);         // LIN_X8R8G8B8
        g_depth = make_surface(w, h, 0x2e, 32);              // LIN_D24S8
        g_render_target = g_backbuffer;
        g_zbuffer = g_depth;
        g_zbuffer = g_depth;
        reset_state();
        select_render_target();
        if (const char* e = std::getenv("TJE_DRAW_FRAME")) g_dump_frame = (uint32_t)std::atoi(e);
    }
    hle_log("[d3d] CreateDevice %ux%u format %x flags %x", w, h, R32(pp + 8), R32(pp + 0x28));
    if (out) W32(out, g_device);
    RET(c, 0, 6);
}

// D3DDevice_Reset(pPresentationParameters)
void hle_00166550(Cpu* c) { D3DLOG(c, "D3DDevice_Reset"); RET(c, 0, 1); }

// D3DDevice_CreateTexture2(Width, Height, Depth, Levels, Usage, Format, ResourceType) -> resource
void hle_001652e0(Cpu* c) { D3DLOG(c, "D3DDevice_CreateTexture2");
    uint32_t w = A(c, 0), h = A(c, 1), depth = A(c, 2), levels = A(c, 3), usage = A(c, 4), fmt = A(c, 5),
             type = A(c, 6);
    uint32_t t = new_resource(0x14, COMMON_TYPE_TEXTURE);
    // Fill Format/Size exactly as the library would.
    guest_call(c, XG_SET_TEXTURE_HEADER, {w, h, levels, usage, fmt, 0, t, 0, 0});
    W32(t, COMMON_TYPE_TEXTURE | COMMON_D3DCREATED | 1);
    if (depth > 1) W32(t + 0x0c, (R32(t + 0x0c) & 0x0fffffffu) | ((uint32_t)__builtin_ctz(depth) << 28));
    if (type == 5) W32(t + 0x0c, R32(t + 0x0c) | 4);        // cube map
    Desc d = describe(t);
    uint32_t bytes = total_bytes(d) * (type == 5 ? 6 : 1);
    uint32_t mem = guest_alloc_contiguous(bytes ? bytes : 64, 0x80);
    W32(t + 4, mem - 0x80000000u);
    if (log_enabled("d3d"))
        hle_log("[d3d] CreateTexture %ux%ux%u levels %u format %x -> %08x (%u bytes)", w, h, depth, levels, fmt, t, bytes);
    RET(c, t, 7);
}

// D3DTexture_GetSurfaceLevel2(pTexture, Level) -> surface
void hle_001653c0(Cpu* c) { D3DLOG(c, "D3DTexture_GetSurfaceLevel2");
    uint32_t t = A(c, 0), level = A(c, 1);
    Desc d = describe(t);
    uint32_t off = 0, w = d.width, h = d.height;
    for (uint32_t l = 0; l < level && l < d.levels; l++) {
        off += level_bytes(d, w, h) * d.depth;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    uint32_t s = new_resource(0x18, COMMON_TYPE_SURFACE);
    W32(s + 4, R32(t + 4) + off);
    uint32_t fw = R32(t + 0x0c);
    if (R32(t + 0x10)) {
        W32(s + 0x0c, fw);
        W32(s + 0x10, R32(t + 0x10));
    } else {
        uint32_t lw = __builtin_ctz(w), lh = __builtin_ctz(h);
        W32(s + 0x0c, (fw & 0x0000ffffu) | (1u << 16) | (lw << 20) | (lh << 24));
    }
    W32(s + 0x14, t);
    addref(t);
    RET(c, s, 2);
}

// D3DTexture_LockRect(pTexture, Level, pLockedRect, pRect, Flags)
void hle_00165410(Cpu* c) { D3DLOG(c, "D3DTexture_LockRect"); gpu::textures_touched();
    uint32_t t = A(c, 0), level = A(c, 1), out = A(c, 2);
    Desc d = describe(t);
    uint32_t off = 0, w = d.width, h = d.height;
    for (uint32_t l = 0; l < level && l < d.levels; l++) {
        off += level_bytes(d, w, h) * d.depth;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    Fmt f = fmt_info(d.format);
    W32(out, f.linear ? d.pitch : (f.compressed ? ((w + 3) / 4) * (f.bpp == 4 ? 8 : 16) : w * f.bpp / 8));
    W32(out + 4, data_ptr(t) + off);
    RET(c, 0, 5);
}

// D3DCubeTexture_LockRect(pTexture, Face, Level, pLockedRect, pRect, Flags)
void hle_001654a0(Cpu* c) { D3DLOG(c, "D3DCubeTexture_LockRect"); gpu::textures_touched();
    uint32_t t = A(c, 0), face = A(c, 1), level = A(c, 2), out = A(c, 3);
    Desc d = describe(t);
    uint32_t face_bytes = (total_bytes(d) + 127) & ~127u;
    uint32_t off = face * face_bytes, w = d.width, h = d.height;
    for (uint32_t l = 0; l < level && l < d.levels; l++) {
        off += level_bytes(d, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    W32(out, w * fmt_info(d.format).bpp / 8);
    W32(out + 4, data_ptr(t) + off);
    RET(c, 0, 6);
}

// D3DVolumeTexture_LockBox(pTexture, Level, pLockedBox, pBox, Flags)
void hle_00165510(Cpu* c) { D3DLOG(c, "D3DVolumeTexture_LockBox"); gpu::textures_touched();
    uint32_t t = A(c, 0), out = A(c, 2);
    Desc d = describe(t);
    uint32_t row = d.width * fmt_info(d.format).bpp / 8;
    W32(out, row);
    W32(out + 4, row * d.height);
    W32(out + 8, data_ptr(t));
    RET(c, 0, 5);
}

// D3DSurface_GetDesc(pSurface, pDesc): {Format, Type, Usage, Size, MultiSampleType, Width, Height}
void hle_0016e680(Cpu* c) { D3DLOG(c, "D3DSurface_GetDesc");
    uint32_t s = A(c, 0), out = A(c, 1);
    Desc d = describe(s);
    W32(out, d.format);
    W32(out + 4, 1);                                  // D3DRTYPE_SURFACE
    W32(out + 8, 0);
    W32(out + 12, level_bytes(d, d.width, d.height));
    W32(out + 16, 0);
    W32(out + 20, d.width);
    W32(out + 24, d.height);
    RET(c, 0, 2);
}

// D3DResource_Register(pResource, pBase): Data becomes an offset into pBase.
void hle_00168570(Cpu* c) { D3DLOG(c, "D3DResource_Register"); gpu::textures_touched();
    uint32_t r = A(c, 0), base = A(c, 1);
    uint32_t phys = base >= 0x80000000u ? base - 0x80000000u : base;
    if (log_enabled("cmem")) hle_log("[cmem] Register %08x (common %08x data %08x format %08x) base %08x from %08x", r,
                                     R32(r), R32(r + 4), R32(r + 12), base, R32(c->esp));
    W32(r + 4, R32(r + 4) + phys);
    RET(c, 0, 2);
}

// D3DResource_Release(pResource) -> remaining references
void hle_001683e0(Cpu* c) { D3DLOG(c, "D3DResource_Release");
    uint32_t r = A(c, 0);
    uint32_t refs = R32(r) & 0xffff;
    if (refs) refs--;
    W32(r, (R32(r) & 0xffff0000u) | refs);
    RET(c, refs, 1);
}

// D3D_AllocContiguousMemory(Size, Alignment)
void hle_0016c5a0(Cpu* c) { D3DLOG(c, "D3D_AllocContiguousMemory");
    uint32_t a = guest_alloc_contiguous(A(c, 0), A(c, 1) ? A(c, 1) : 16);
    if (log_enabled("cmem")) hle_log("[cmem] alloc %x align %x -> %08x from %08x", A(c, 0), A(c, 1), a, R32(c->esp));
    RET(c, a, 2);
}
// D3D_FreeContiguousMemory(pMemory)
void hle_0016c5c0(Cpu* c) { D3DLOG(c, "MmFreeContiguousMemory");
    if (log_enabled("cmem")) hle_log("[cmem] free %08x from %08x", A(c, 0), R32(c->esp));
    guest_free(A(c, 0));
    RET(c, 0, 1);
}

// D3DDevice_CreateVertexBuffer2(Length) -> vertex buffer
void hle_0016c680(Cpu* c) { D3DLOG(c, "D3DDevice_CreateVertexBuffer2");
    uint32_t vb = new_resource(0x0c, COMMON_TYPE_VB);
    W32(vb + 4, guest_alloc_contiguous(A(c, 0) ? A(c, 0) : 16, 16) - 0x80000000u);
    RET(c, vb, 1);
}
// D3DVertexBuffer_Lock2(pVertexBuffer, Flags) -> data
void hle_0016c6d0(Cpu* c) { D3DLOG(c, "D3DVertexBuffer_Lock2"); gpu::textures_touched(); RET(c, data_ptr(A(c, 0)), 2); }
// D3DDevice_CreatePalette2(Size) -> palette (256, 128, 64 or 32 entries)
void hle_0016c760(Cpu* c) { D3DLOG(c, "D3DDevice_CreatePalette2");
    static const uint32_t entries[] = {256, 128, 64, 32};
    uint32_t p = new_resource(0x0c, COMMON_TYPE_PALETTE | (A(c, 0) << 30));
    W32(p + 4, guest_alloc_contiguous(entries[A(c, 0) & 3] * 4, 64) - 0x80000000u);
    RET(c, p, 1);
}
// D3DPalette_Lock2(pPalette, Flags) -> entries
void hle_0016c7c0(Cpu* c) { D3DLOG(c, "D3DPalette_Lock2"); gpu::textures_touched(); RET(c, data_ptr(A(c, 0)), 2); }

// Render targets.
void hle_00166ce0(Cpu* c) { D3DLOG(c, "D3DDevice_GetRenderTarget2"); addref(g_render_target); RET(c, g_render_target, 0); }
void hle_00166d00(Cpu* c) { D3DLOG(c, "D3DDevice_GetDepthStencilSurface2"); addref(g_zbuffer); RET(c, g_zbuffer, 0); }
void hle_00165590(Cpu* c) { D3DLOG(c, "D3DDevice_SetRenderTarget");                   // SetRenderTarget(pRT, pZ)
    if (A(c, 0)) g_render_target = A(c, 0);
    g_zbuffer = A(c, 1);
    select_render_target();
    if (log_enabled("rt")) {
        auto d = [](uint32_t s) { return s ? describe(s) : Desc{}; };
        Desc a = d(A(c, 0)), z = d(A(c, 1));
        hle_log("[rt] SetRenderTarget(%08x data %08x fmt %02x %ux%u, z %08x data %08x fmt %02x %ux%u) frame %u from %08x",
                A(c, 0), A(c, 0) ? data_ptr(A(c, 0)) : 0, a.format, a.width, a.height, A(c, 1),
                A(c, 1) ? data_ptr(A(c, 1)) : 0, z.format, z.width, z.height, g_frames, R32(c->esp));
    }
    RET(c, 0, 2);
}

// Vertical blank: a 59.94 Hz clock from boot. Every waiter wakes at the next
// tick, however many threads wait (CRI's server threads wait on it too).
static const auto g_vblank_t0 = std::chrono::steady_clock::now();
#ifdef __ANDROID__
static const std::chrono::nanoseconds g_vblank_period(16666667);      // 60 Hz, the handheld's own screen
#else
static const std::chrono::nanoseconds g_vblank_period(16683333);
#endif

static int64_t vblank_now() { return (std::chrono::steady_clock::now() - g_vblank_t0) / g_vblank_period; }

static double g_wait_ms = 0, g_late_ms = 0, g_main_wait_ms = 0;      // TJE_LOG=frametime: vblank waits, and lateness getting the CPU back
static void wait_for_tick(int64_t tick) {
    auto until = g_vblank_t0 + tick * g_vblank_period;
    auto t0 = std::chrono::steady_clock::now();
    if (until <= t0) return;
    without_gil([until] { std::this_thread::sleep_until(until); });
    auto t1 = std::chrono::steady_clock::now();
    g_wait_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (current_thread()->id == 2) g_main_wait_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    g_late_ms += std::chrono::duration<double, std::milli>(t1 - until).count();
}

static void wait_vblank() { wait_for_tick(vblank_now() + 1); }

// D3DDevice_BlockUntilVerticalBlank()
void hle_00165b60(Cpu* c) { D3DLOG(c, "D3DDevice_BlockUntilVerticalBlank");
    g_vblank_waits++;
    wait_vblank();
    RET(c, 0, 0);
}

// D3DDevice_Swap(Flags)
void hle_00169470(Cpu* c) { D3DLOG(c, "D3DDevice_Swap");
    // TJE_DRAW_FRAME=busy: dump the frame after the first one with over 200 draws.
    static uint32_t frame_draws = 0, draws_before = 0;
    frame_draws = g_draws >= draws_before ? g_draws - draws_before : g_draws;   // g_draws resets every 600 frames
    draws_before = g_draws;
    static bool busy_dumped = false;
    if (!busy_dumped && frame_draws > 200 && std::getenv("TJE_DRAW_FRAME") &&
        !std::strcmp(std::getenv("TJE_DRAW_FRAME"), "busy")) {
        busy_dumped = true;
        g_dump_frame = g_frames + 2;
    }
    // TJE_DRAW_FRAME=t<seconds>: dump the first frame after that time.
    static auto boot = std::chrono::steady_clock::now();
    if (!busy_dumped && std::getenv("TJE_DRAW_FRAME") && std::getenv("TJE_DRAW_FRAME")[0] == 't' &&
        std::chrono::steady_clock::now() - boot > std::chrono::milliseconds((long long)(std::atof(std::getenv("TJE_DRAW_FRAME") + 1) * 1000))) {
        busy_dumped = true;
        g_dump_frame = g_frames + 2;
    }
    g_frames++;
    g_swaps++;
    if (log_enabled("frametime")) {
        static auto last = std::chrono::steady_clock::now();
        static double total = 0, ready0 = 0;
        static int n = 0;
        auto now = std::chrono::steady_clock::now();
        total += std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        if (++n == 60) {
            GuestThread* me = current_thread();
            hle_log("[d3d] frame %.1f ms: vblank waits %.1f ms (%.1f ms late getting the CPU back), main thread "
                    "vblank waits %.1f ms, waiting for its turn %.1f ms", total / n, g_wait_ms / n, g_late_ms / n,
                    g_main_wait_ms / n, (me->ready_wait_ms - ready0) / n);
            ready0 = me->ready_wait_ms;
            total = g_wait_ms = g_late_ms = g_main_wait_ms = 0;
            n = 0;
        }
    }
#ifdef __ANDROID__
    gpu::window_wait_foreground();              // the whole game waits here while the player is away
    if (static const bool pace = std::getenv("TJE_PACE") != nullptr; pace) {    // frame-to-frame timing
        static auto prev = std::chrono::steady_clock::now();
        static int n = 0, over20 = 0, over34 = 0, over50 = 0;
        static double worst = 0;
        auto t = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t - prev).count();
        prev = t;
        if (ms < 2000) {
            n++;
            over20 += ms > 20;
            over34 += ms > 34;
            over50 += ms > 50;
            if (ms > worst) worst = ms;
        }
        if (n == 300) {
            hle_log("[pace] 300 frames: %d over 20 ms, %d over 34 ms, %d over 50 ms, worst %.0f ms, %u draws last frame",
                    over20, over34, over50, worst, g_draws);
            n = over20 = over34 = over50 = 0;
            worst = 0;
        }
    }
#endif
    gpu::present();
    static auto last = std::chrono::steady_clock::now();
    if (log_enabled("frame") || g_frames % 600 == 0) {
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - last).count();
        uint32_t n = log_enabled("frame") ? 1 : 600;
        hle_log("[d3d] frame %u: %.1f fps, %u draws, %.1f vblank waits per frame, %zu files open", g_frames, n / secs,
                g_draws / n, g_vblank_waits / (double)n, handle_open_files());
        g_vblank_waits = 0;
        last = now;
        g_draws = 0;
    }
    // Swap queues a flip for the next vblank and only waits while a flip is
    // already pending: the game runs at most one frame ahead of the display.
    static int64_t pending_flip = 0;
    int64_t now = vblank_now();
    if (pending_flip > now) wait_for_tick(pending_flip);
    pending_flip = vblank_now() + 1;
    RET(c, g_frames, 1);
}

// D3DDevice_Clear(Count, pRects, Flags, Color, Z, Stencil)
void hle_001697a0(Cpu* c) { D3DLOG(c, "D3DDevice_Clear");
    uint32_t n = A(c, 0), rects = A(c, 1);
    std::vector<int32_t> r;
    for (uint32_t i = 0; rects && i < n; i++)
        for (int k = 0; k < 4; k++) r.push_back((int32_t)R32(rects + i * 16 + k * 4));
    gpu::clear(A(c, 2), A(c, 3), f32(A(c, 4)), A(c, 5), r.data(), rects ? n : 0);
    RET(c, 0, 6);
}

// ---------------------------------------------------------------- drawing

// D3DDevice_DrawVerticesUP(PrimitiveType, VertexCount, pVertexStreamZeroData, VertexStreamZeroStride)
void hle_0016bcb0(Cpu* c) { D3DLOG(c, "D3DDevice_DrawVerticesUP");
    g_draws++;
    gpu::Draw d;
    build_draw(d, A(c, 0), A(c, 1));
    d.up_data = (const uint8_t*)gptr(A(c, 2));
    d.streams[0].stride = A(c, 3);
    log_draw_once("DrawVerticesUP", d);
    static uint32_t watched[4];
    if (std::getenv("TJE_WATCH_MOVIE") && d.tex[0].format == 0x1e && d.tex[0].width == 640 &&
        watched[0] != d.tex[0].addr && watched[1] != d.tex[0].addr && !watched[3]) {
        for (auto& w : watched) if (!w) { w = d.tex[0].addr; break; }
        uint32_t nz = 0;
        for (uint32_t i = 0; i < 640 * 480 * 4; i++) nz += *(const uint8_t*)gptr(d.tex[0].addr + i) != 0;
        hle_log("[watch] texture %08x (header %08x) has %u non-zero bytes", d.tex[0].addr, g_textures[0], nz);
        log_backtrace(c, "movie draw");
        mem_watch_writes(d.tex[0].addr, 640 * 480 * 4);
        hle_log("[watch] movie texture %08x", d.tex[0].addr);
    }
    static uint32_t nth = 0;
    nth++;
    if (log_enabled("draw") && (nth % 25 == 0)) {
        for (uint32_t v = 0; v < A(c, 1) && v < 4; v++) {
            const float* f = (const float*)gptr(A(c, 2) + v * A(c, 3));
            hle_log("[d3d]   v%u: %g %g %g | %g %g %g %g | %g %g", v, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]);
        }
        if (g_textures[0])
            hle_log("[d3d]   tex0 %08x: %08x %08x %08x %08x %08x  w %u h %u", g_textures[0], R32(g_textures[0]),
                    R32(g_textures[0] + 4), R32(g_textures[0] + 8), R32(g_textures[0] + 12), R32(g_textures[0] + 16),
                    d.tex[0].width, d.tex[0].height);
        if (g_textures[0]) {
            uint32_t nz = 0, n = d.tex[0].pitch ? d.tex[0].pitch * d.tex[0].height : d.tex[0].width * d.tex[0].height;
            for (uint32_t i = 0; i < n; i++) nz += *(const uint8_t*)gptr(d.tex[0].addr + i) != 0;
            hle_log("[d3d]   tex0 data: %u of %u bytes non-zero", nz, n);
        }
        for (int r : {14, 20, 21, 22, 23, 58, 59, 182, 183, 184, 185, 191})
            hle_log("[d3d]   c%d: %g %g %g %g", r, g_vconst[r][0], g_vconst[r][1], g_vconst[r][2], g_vconst[r][3]);
        for (int i = 0; i < 16; i++)
            if (d.attrs[i].format != 2) hle_log("[d3d]   attr %d: stream %u offset %u format %02x", i, d.attrs[i].stream, d.attrs[i].offset, d.attrs[i].format);
    }
    gpu::draw(d);
    RET(c, 0, 4);
}

// D3DDevice_DrawIndexedVerticesUP(PrimitiveType, VertexCount, pIndexData, pVertexStreamZeroData, Stride)
void hle_0016be10(Cpu* c) { D3DLOG(c, "D3DDevice_DrawIndexedVerticesUP");
    g_draws++;
    gpu::Draw d;
    build_draw(d, A(c, 0), A(c, 1));
    d.index_addr = A(c, 2);
    d.up_data = (const uint8_t*)gptr(A(c, 3));
    d.streams[0].stride = A(c, 4);
    log_draw_once("DrawIndexedVerticesUP", d);
    gpu::draw(d);
    RET(c, 0, 5);
}

// D3DDevice_DrawVertices(PrimitiveType, StartVertex, VertexCount)
void hle_0016bf50(Cpu* c) { D3DLOG(c, "D3DDevice_DrawVertices");
    g_draws++;
    gpu::Draw d;
    build_draw(d, A(c, 0), A(c, 2));
    for (auto& s : d.streams) if (s.addr) s.addr += A(c, 1) * s.stride;
    log_draw_once("DrawVertices", d);
    gpu::draw(d);
    RET(c, 0, 3);
}

// D3DDevice_DrawIndexedVertices(PrimitiveType, VertexCount, pIndexData)
void hle_0016bff0(Cpu* c) { D3DLOG(c, "D3DDevice_DrawIndexedVertices");
    g_draws++;
    gpu::Draw d;
    build_draw(d, A(c, 0), A(c, 1));
    d.index_addr = A(c, 2);
    log_draw_once("DrawIndexedVertices", d);
    gpu::draw(d);
    RET(c, 0, 3);
}

// D3DDevice_Begin(PrimitiveType)
void hle_0016c4d0(Cpu* c) { D3DLOG(c, "D3DDevice_Begin");
    g_in_begin = true;
    g_begin_prim = A(c, 0);
    g_immediate.clear();
    g_immediate_mask = 0;
    RET(c, 0, 1);
}

// Immediate-mode vertex data: outside Begin/End it sets the attribute's current
// value; inside, writing register 0 (position) emits a vertex.
static void vertex_data(uint32_t reg, const float v[4]) {
    reg &= 15;
    std::memcpy(g_vertex_data[reg], v, 16);
    if (!g_in_begin) return;
    g_immediate_mask |= 1u << reg;
    if (reg == 0) g_immediate.insert(g_immediate.end(), &g_vertex_data[0][0], &g_vertex_data[0][0] + 64);
}

// D3DDevice_End()
void hle_0016c510(Cpu* c) { D3DLOG(c, "D3DDevice_End");
    g_in_begin = false;
    uint32_t n = (uint32_t)(g_immediate.size() / 64);
    if (n) {
        g_draws++;
        gpu::Draw d;
        build_draw(d, g_begin_prim, n);
        d.up_data = (const uint8_t*)g_immediate.data();
        d.streams[0].stride = 256;
        for (uint32_t i = 0; i < 16; i++) {
            d.attrs[i].stream = 0;
            d.attrs[i].offset = (uint16_t)(i * 16);
            d.attrs[i].format = (g_immediate_mask >> i) & 1 ? 0x42 : 0x02;   // FLOAT4 / none
        }
        log_draw_once("Begin/End", d);
        gpu::draw(d);
    }
    RET(c, 0, 0);
}

// D3DDevice_SetVertexData4f(Register, a, b, c, d)
void hle_0016c340(Cpu* c) { D3DLOG(c, "D3DDevice_SetVertexData4f");
    float v[4] = {f32(A(c, 1)), f32(A(c, 2)), f32(A(c, 3)), f32(A(c, 4))};
    vertex_data(A(c, 0), v);
    RET(c, 0, 5);
}
// D3DDevice_SetVertexData2f(Register, a, b)
void hle_0016c300(Cpu* c) { D3DLOG(c, "D3DDevice_SetVertexData2f");
    float v[4] = {f32(A(c, 1)), f32(A(c, 2)), 0.0f, 1.0f};
    vertex_data(A(c, 0), v);
    RET(c, 0, 3);
}

// ---------------------------------------------------------------- state

// The game's active camera (a copy made by 0xf2410 before it draws with one):
// +0x00 projection, +0x40 view, +0x88 width and height, +0xb0 pixel shape,
// +0xc8 the side clipping plane used for culling.
// Widescreen keeps the height of the view and widens it: a narrower X scale,
// and side planes to match so nothing at the new edges is culled. Computed
// from the Y scale each time, so a camera that was saved and restored isn't
// widened twice.
void wrap_000f2410(Cpu* c) {
    static const GuestFn set_camera = lookup(0xf2410);
    set_camera(c);
    const float wide = gpu::wide_factor();
    const uint32_t cam = kActiveCamera;
    if (R32(cam + 0x3c) != 0 || f32(R32(cam + 0x38)) != -1.0f) return;           // not a perspective camera
    const float w = f32(R32(cam + 0x88)), h = f32(R32(cam + 0x8c)), shape = f32(R32(cam + 0xb0));
    if (w != 640.0f || h != 480.0f || shape <= 0.0f) return;                     // not drawing the screen
    const float ys = f32(R32(cam + 0x14));
    const float xs = ys / ((w / h) * shape) / wide;
    if (!(xs > 0.0f)) return;
    const float t = 1.0f / xs, r = 1.0f / std::sqrt(t * t + 1.0f);
    auto put = [](uint32_t a, float v) { uint32_t u; std::memcpy(&u, &v, 4); W32(a, u); };
    put(cam + 0x00, xs);
    put(cam + 0xc8, r * t);
    put(cam + 0xcc, -r);
}

// D3DDevice_SetViewport(pViewport {X, Y, Width, Height, MinZ, MaxZ})
void hle_00166d20(Cpu* c) { D3DLOG(c, "D3DDevice_SetViewport");
    uint32_t vp = A(c, 0);
    float x = (float)R32(vp), y = (float)R32(vp + 4), w = (float)R32(vp + 8), h = (float)R32(vp + 12);
    float minz = f32(R32(vp + 16)), maxz = f32(R32(vp + 20));
    const float zscale = g_zbuffer ? depth_range(describe(g_zbuffer).format) : 16777215.0f;
    if (!(g_constant_mode & 0x10)) {           // D3D writes the viewport transform to c[-38], c[-37]
        float scale[4] = {w / 2, -h / 2, (maxz - minz) * zscale, 0.0f};
        float offset[4] = {x + w / 2, y + h / 2, minz * zscale, 0.0f};
        std::memcpy(g_vconst[96 - 38], scale, 16);
        std::memcpy(g_vconst[96 - 37], offset, 16);
    }
    RET(c, 0, 1);
}

// D3DDevice_SetTexture(Stage, pTexture)
void hle_00167180(Cpu* c) { D3DLOG(c, "D3DDevice_SetTexture");
    g_textures[A(c, 0) & 3] = A(c, 1);
    RET(c, 0, 2);
}
void hle_001679d0(Cpu* c) { D3DLOG(c, "D3DDevice_SetTile"); RET(c, 0, 2); }               // SetTile(Index, pTile)
void hle_00167bf0(Cpu* c) { D3DLOG(c, "D3DDevice_SetScissors"); RET(c, 0, 3); }           // SetScissors(Count, Exclusive, pRects)

// D3DDevice_SetStreamSource(StreamNumber, pStreamData, Stride)
void hle_0016a6f0(Cpu* c) { D3DLOG(c, "D3DDevice_SetStreamSource");
    uint32_t s = A(c, 0) & 15, vb = A(c, 1);
    g_streams[s].addr = vb ? data_ptr(vb) : 0;
    g_streams[s].stride = A(c, 2);
    RET(c, 0, 3);
}

// D3DDevice_SetRenderState_Simple: ecx = (count << 18) | NV2A method, edx = value
void hle_0016c820(Cpu* c) { D3DLOG(c, "D3DDevice_SetRenderState_Simple");
    method(c->ecx & 0x3fffc) = c->edx;
    c->esp += 4;
}

// D3DDevice_SetRenderStateNotInline(State, Value)
void hle_0016c850(Cpu* c) { D3DLOG(c, "D3DDevice_SetRenderStateNotInline");
    static const uint16_t simple[82] = {
        // PS* states (0-56) program the combiners; the renderer takes them from the pixel shader
        0x260, 0x264, 0x268, 0x26c, 0x270, 0x274, 0x278, 0x27c, 0x288, 0x28c,
        0xa60, 0xa64, 0xa68, 0xa6c, 0xa70, 0xa74, 0xa78, 0xa7c, 0xa80, 0xa84, 0xa88, 0xa8c, 0xa90, 0xa94, 0xa98, 0xa9c,
        0xaa0, 0xaa4, 0xaa8, 0xaac, 0xab0, 0xab4, 0xab8, 0xabc, 0xac0, 0xac4, 0xac8, 0xacc, 0xad0, 0xad4, 0xad8, 0xadc,
        0x17f8, 0x1e20, 0x1e24, 0x1e40, 0x1e44, 0x1e48, 0x1e4c, 0x1e50, 0x1e54, 0x1e58, 0x1e5c, 0x1e60, 0x1e64, 0x1e68,
        0x1e6c,
        0x354, 0x33c, 0x304, 0x300, 0x340, 0x344, 0x348, 0x35c, 0x310, 0x37c, 0x358, 0x374, 0x378, 0x364, 0x368,
        0x36c, 0x360, 0x350, 0x34c, 0x9f8, 0x384, 0x388, 0x330, 0x334, 0x338};
    uint32_t state = A(c, 0), value = A(c, 1);
    if (state < 82) method(simple[state]) = value;
    if (state < 256) W32(D3D_RENDER_STATE + state * 4, value);
    if (log_enabled("d3d")) hle_log("[d3d] SetRenderStateNotInline(%u, %08x)", state, value);
    RET(c, 0, 2);
}

void hle_0016cde0(Cpu* c) { D3DLOG(c, "D3DDevice_SetRenderState_ShadowFunc");
    if (log_enabled("d3d") && A(c, 0) != g_shadow_func) hle_log("[d3d] ShadowFunc %x", A(c, 0));
    g_shadow_func = A(c, 0);
    RET(c, 0, 1);
}
void hle_0016ce20(Cpu* c) { D3DLOG(c, "D3DDevice_SetRenderState_FogColor"); g_fog_color = A(c, 0); RET(c, 0, 1); }
// D3DDevice_SetRenderState_CullMode(Value): NV2A cull enable + cull face relative to the front face
void hle_0016ce70(Cpu* c) { D3DLOG(c, "D3DDevice_SetRenderState_CullMode");
    uint32_t v = A(c, 0);
    method(0x308) = v != 0;
    if (v) method(0x39c) = 0x404 + (v != R32(D3D_RENDER_STATE + RS_FRONTFACE * 4) ? 1 : 0);
    W32(D3D_RENDER_STATE + RS_CULLMODE * 4, v);
    RET(c, 0, 1);
}
void hle_0016d240(Cpu* c) { D3DLOG(c, "D3DDevice_SetRenderState_TwoSidedLighting"); RET(c, 0, 1); }
// D3DDevice_SetRenderState_ZEnable(Value)
void hle_0016e000(Cpu* c) { D3DLOG(c, "D3DDevice_SetRenderState_ZEnable");
    method(0x30c) = A(c, 0) != 0;
    W32(D3D_RENDER_STATE + RS_ZENABLE * 4, A(c, 0));
    RET(c, 0, 1);
}
void hle_0016cf60(Cpu* c) { D3DLOG(c, "D3DDevice_SetRenderState_TextureFactor"); g_texture_factor = A(c, 0); RET(c, 0, 1); }
void hle_0016d480(Cpu* c) { D3DLOG(c, "D3DDevice_SetTextureState_BorderColor");         // (Stage, Value)
    W32(D3D_TEXTURE_STATE + (A(c, 0) & 3) * 0x80 + 29 * 4, A(c, 1));
    RET(c, 0, 2);
}
void hle_0016d300(Cpu* c) { D3DLOG(c, "D3DDevice_SetTextureState_TexCoordIndex");       // (Stage, Value)
    W32(D3D_TEXTURE_STATE + (A(c, 0) & 3) * 0x80 + 28 * 4, A(c, 1));
    RET(c, 0, 2);
}
// D3DDevice_SetTextureStageStateNotInline(Stage, Type, Value)
void hle_0016d510(Cpu* c) { D3DLOG(c, "D3DDevice_SetTextureStageStateNotInline");
    W32(D3D_TEXTURE_STATE + (A(c, 0) & 3) * 0x80 + (A(c, 1) & 31) * 4, A(c, 2));
    RET(c, 0, 3);
}

// ---------------------------------------------------------------- shaders

// D3DDevice_CreateVertexShader(pDeclaration, pFunction, pHandle, Usage)
void hle_0016a100(Cpu* c) { D3DLOG(c, "D3DDevice_CreateVertexShader");
    uint32_t decl = A(c, 0), function = A(c, 1);
    uint32_t obj = guest_alloc(0x14 + 16 * 16 + 0x40);
    for (uint32_t i = 0; i < 16; i++) {
        W32(obj + 0x14 + i * 16, 0);
        W32(obj + 0x14 + i * 16 + 4, 0);
        W32(obj + 0x14 + i * 16 + 8, 0x02);       // D3DVSDT_NONE
        W32(obj + 0x14 + i * 16 + 12, 0);
    }
    // Declaration tokens: STREAM(n), REG(reg, type), SKIP(count), END.
    uint32_t stream = 0, offset = 0;
    for (uint32_t p = decl; p; p += 4) {
        uint32_t t = R32(p), kind = t & 0xe0000000u;
        if (kind > 0x60000000u) break;                          // END / constants / extensions
        if (kind == 0x20000000u) { stream = t & 0xf; offset = 0; continue; }
        if (kind != 0x40000000u) continue;
        if (t & 0x10000000u) {                                  // skip
            uint32_t n = (t >> 16) & 0xf;
            offset += (t & 0x08000000u) ? n : n * 4;
            continue;
        }
        uint32_t reg = t & 0x1f, type = (t >> 16) & 0xff;
        if (reg < 16) {
            W32(obj + 0x14 + reg * 16, stream);
            W32(obj + 0x14 + reg * 16 + 4, offset);
            W32(obj + 0x14 + reg * 16 + 8, type);
        }
        offset += vsdt_size(type);
    }
    uint32_t h = obj | 1;
    g_vshaders[h].function = function;
    if (A(c, 2)) W32(A(c, 2), h);
    RET(c, 0, 4);
}
void hle_0016a260(Cpu* c) { D3DLOG(c, "D3DDevice_SetVertexShaderConstant1"); set_constants(c->ecx, c->edx, 4); c->esp += 4; }
void hle_0016a310(Cpu* c) { D3DLOG(c, "D3DDevice_SetVertexShaderConstant4"); set_constants(c->ecx, c->edx, 16); c->esp += 4; }
void hle_0016a4a0(Cpu* c) { D3DLOG(c, "D3DDevice_SetVertexShaderConstantNotInline"); set_constants(c->ecx, c->edx, A(c, 0)); RET(c, 0, 1); }
void hle_0016a3c0(Cpu* c) { D3DLOG(c, "D3DDevice_SetVertexShaderConstantNotInlineFast"); set_constants(c->ecx, c->edx, A(c, 0)); RET(c, 0, 1); }
// D3DDevice_LoadVertexShader(Handle, Address)
void hle_0016a820(Cpu* c) { D3DLOG(c, "D3DDevice_LoadVertexShader");
    auto it = g_vshaders.find(A(c, 0));
    if (it != g_vshaders.end() && it->second.function) load_program(it->second.function, A(c, 1));
    RET(c, 0, 2);
}
// D3DDevice_LoadVertexShaderProgram(pFunction, Address)
void hle_0016a880(Cpu* c) { D3DLOG(c, "D3DDevice_LoadVertexShaderProgram");
    load_program(A(c, 0), A(c, 1));
    RET(c, 0, 2);
}
// D3DDevice_SelectVertexShader(Handle, Address)
void hle_0016a8f0(Cpu* c) { D3DLOG(c, "D3DDevice_SelectVertexShader");
    if (A(c, 0)) load_vaf(A(c, 0) & ~1u);
    g_program_start = A(c, 1) < 136 ? A(c, 1) : 0;
    RET(c, 0, 2);
}
// D3DDevice_SelectVertexShaderDirect(pVAF, Address)
void hle_00169e00(Cpu* c) { D3DLOG(c, "D3DDevice_SelectVertexShaderDirect");
    if (A(c, 0)) load_vaf(A(c, 0) - 0x14);
    g_program_start = A(c, 1) < 136 ? A(c, 1) : 0;
    RET(c, 0, 2);
}
// D3DDevice_SetVertexShader(Handle): programmable handles load and select at 0
void hle_0016ac20(Cpu* c) { D3DLOG(c, "D3DDevice_SetVertexShader");
    uint32_t h = A(c, 0);
    auto it = g_vshaders.find(h);
    if (it != g_vshaders.end()) {
        load_program(it->second.function, 0);
        load_vaf(h & ~1u);
        g_program_start = 0;
    } else {
        hle_unimplemented("D3DDevice_SetVertexShader (fixed-function FVF)", c);
    }
    RET(c, 0, 1);
}
void hle_0016a970(Cpu* c) { D3DLOG(c, "D3DDevice_GetVertexShaderSize");               // (Handle, pSize)
    auto it = g_vshaders.find(A(c, 0));
    uint32_t n = it != g_vshaders.end() && it->second.function ? R16(it->second.function + 2) : 0;
    if (A(c, 1)) W32(A(c, 1), n);
    RET(c, 0, 2);
}
void hle_0016a9c0(Cpu* c) { D3DLOG(c, "D3DDevice_SetShaderConstantMode"); g_constant_mode = A(c, 0); RET(c, 0, 1); }
// D3DDevice_CreatePixelShader(pDef, pHandle): { refcount, flags, pDef -> copy of the definition }
void hle_0016e770(Cpu* c) { D3DLOG(c, "D3DDevice_CreatePixelShader");
    uint32_t obj = guest_alloc(0xfc);
    W32(obj, 1);
    W32(obj + 4, 1);
    W32(obj + 8, obj + 0xc);
    std::memcpy(gptr(obj + 0xc), gptr(A(c, 0)), 0xf0);
    if (A(c, 1)) W32(A(c, 1), obj);
    RET(c, 0, 2);
}
void hle_0016ec20(Cpu* c) { D3DLOG(c, "D3DDevice_SetPixelShader");
    g_pixel_shader = A(c, 0);
    g_ps_constants_set = 0;
    RET(c, 0, 1);
}
// D3DDevice_SetPixelShaderConstant(Register, pConstantData, ConstantCount): float4s, stored as D3DCOLOR
void hle_0016ee20(Cpu* c) { D3DLOG(c, "D3DDevice_SetPixelShaderConstant");
    uint32_t reg = A(c, 0), src = A(c, 1), n = A(c, 2);
    for (uint32_t i = 0; i < n && reg + i < 8; i++) {
        uint32_t col = 0;
        for (int k = 0; k < 4; k++) {
            float v = f32(R32(src + i * 16 + k * 4));
            v = v < 0 ? 0 : v > 1 ? 1 : v;
            uint32_t b = (uint32_t)(v * 255.0f + 0.5f);
            col |= b << (k == 3 ? 24 : 16 - 8 * k);           // r g b a -> ARGB
        }
        g_ps_constants[reg + i] = col;
        g_ps_constants_set |= 1u << (reg + i);
    }
    RET(c, 0, 3);
}

}  // namespace tje
