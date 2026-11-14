// Renderer interface between the D3D8 HLE (runtime/hle/d3d.cpp) and the Vulkan
// back end (runtime/gpu/vulkan.cpp).
//
// The HLE keeps the Xbox-side state (NV2A render state, vertex program store,
// vertex constants, pixel shader, texture stages) and hands the renderer one
// self-contained Draw per draw call. Guest memory is read by address; nothing
// here owns guest data.
#pragma once
#include <cstdint>
#include <string>

namespace tje::gpu {

struct Options {
    int scale = 2;            // internal resolution = 640x480 * scale
    bool scale_forced = false;  // --scale was given: it wins over the resolution option
    bool vsync = false;       // the game paces itself with BlockUntilVerticalBlank
    bool linear_filter = true;  // filter when scaling to the window
    int window_width = 1280, window_height = 960;
    bool fullscreen = false;  // borderless, the size of the monitor (window_width/height unused)
};

// Texture as the NV2A sees it (from the resource's Format/Size words).
struct TextureRef {
    uint32_t addr = 0;        // guest address of the data (0 = no texture)
    uint32_t format = 0;      // NV2A colour format code (D3DFMT bits 8-15)
    uint32_t width = 0, height = 0, depth = 1, levels = 1, pitch = 0;
    bool cube = false;
    uint32_t palette = 0;     // guest address of D3DCOLOR entries (P8 textures)
    uint32_t palette_entries = 0;
};

struct Sampler {             // D3DTSS values (Xbox numbering)
    uint8_t address_u = 1, address_v = 1, address_w = 1;   // 1 wrap 2 mirror 3 clamp 4 border 5 clamp-to-edge
    uint8_t mag = 2, min = 2, mip = 0;                       // 0 none 1 point 2 linear 3+ linear
    uint32_t border = 0;
};

struct VertexAttr {
    uint8_t stream = 0;
    uint8_t format = 0x02;    // D3DVSDT_*: 0x02 = none
    uint16_t offset = 0;
};

struct Stream {
    uint32_t addr = 0;        // guest address of vertex 0
    uint32_t stride = 0;
};

// Fixed-function state, NV2A (OpenGL-valued) semantics.
struct RasterState {
    uint32_t blend_enable = 0, blend_src = 1, blend_dst = 0, blend_eq = 0x8006, blend_color = 0;
    uint32_t depth_test = 1, depth_func = 0x203, depth_write = 1;
    uint32_t stencil_test = 0, stencil_func = 0x207, stencil_ref = 0, stencil_read_mask = 0xff,
             stencil_write_mask = 0xff, stencil_fail = 0x1e00, stencil_zfail = 0x1e00, stencil_zpass = 0x1e00;
    uint32_t color_mask = 0x01010101;    // A R G B bytes
    uint32_t cull_enable = 1, cull_face = 0x405, front_face = 0x900;
    uint32_t fill_mode = 0x1b02;
    uint32_t poly_offset = 0;
    float poly_factor = 0, poly_units = 0;
};

struct Draw {
    uint32_t prim = 0;           // Xbox D3DPRIMITIVETYPE (1 points .. 10 polygon)
    uint32_t count = 0;          // vertices (or indices)
    uint32_t index_addr = 0;     // guest address of u16 indices; 0 = not indexed
    const uint8_t* up_data = nullptr;   // DrawVerticesUP: stream 0 data in guest memory

    VertexAttr attrs[16];
    Stream streams[16];
    float attr_default[16][4];   // value of attributes the layout leaves out

    const uint32_t* program = nullptr;  // NV2A vertex program, 4 dwords per instruction
    uint32_t program_len = 0;           // instructions, up to and including FINAL
    const float* constants = nullptr;   // 192 x vec4, biased (index = c + 96)

    bool fixed_function_ps = true;
    uint32_t psdef[60] = {};     // D3DPIXELSHADERDEF when !fixed_function_ps
    uint32_t tss[4][32] = {};    // texture stage states (fixed-function combiners)

    TextureRef tex[4];
    Sampler samp[4];

    RasterState rs;
    uint32_t alpha_test = 0, alpha_func = 0x207, alpha_ref = 0;
    uint32_t texture_factor = 0, fog_color = 0;
    uint32_t shadow_func = 0x203;
    // Fog (deferred render states): mode 1 exp, 2 exp2, 3 linear; fog coordinate = the vertex program's oFog.x.
    uint32_t fog_enable = 0, fog_mode = 0;
    float fog_start = 0, fog_end = 1, fog_density = 1;  // D3DRS_SHADOWFUNC (OpenGL compare function) for depth textures

    float surface_w = 640, surface_h = 480, zmax = 16777215.0f;
    bool flat = false;        // drawn by the game's 2D camera (menus, HUD): keeps its shape in widescreen
};

// Widescreen: how many times wider than 4:3 the picture is now (1 = not widescreen).
float wide_factor();
void update_video();             // re-reads the picture options (every frame)

bool init(const Options& o);     // window + Vulkan; false = stay headless
bool active();
void set_title(const std::string& t);

// Render target selection: colour and depth surfaces by guest address and size
// (Xbox pixels); depth 0 = none. `back` marks the colour surface Swap shows.
void set_render_target(uint32_t color, uint32_t cw, uint32_t ch, uint32_t depth, uint32_t dw, uint32_t dh, bool back);

// Clear(Count, pRects, Flags, Color, Z, Stencil); rects are {x1, y1, x2, y2} in surface pixels.
void clear(uint32_t flags, uint32_t color, float z, uint32_t stencil, const int32_t* rects, uint32_t nrects);
void draw(const Draw& d);
void present();                  // end of frame: show the back buffer
#ifdef __ANDROID__
bool android_unpack_game_files();
bool android_import_game(const std::string& files);     // android_import.cpp
#endif
// The guest may have written texture memory (LockRect, Register): textures are
// re-checked against guest memory at their next use. Also implied by present().
void textures_touched();

// Shader generation (shaders.cpp).
std::string vertex_glsl(const uint32_t* program, uint32_t len);
std::string fragment_glsl(const Draw& d, uint32_t tex_kinds);   // tex_kinds: 2 bits per stage (0 none 1 2D 2 cube 3 depth)

}  // namespace tje::gpu
