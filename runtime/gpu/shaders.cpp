// GLSL generation for the two programmable NV2A stages.
//
// Vertex: the NV2A vertex program microcode (what LoadVertexShaderProgram puts
// in the 136-slot program store) is translated instruction by instruction. Each
// instruction pairs a MAC op (vector) and an ILU op (scalar) that read their
// operands before either writes; when both are active the ILU result goes to R1.
// The XDK epilogue leaves oPos in screen space (divided by w), so the generated
// main() maps it back to Vulkan clip space.
//
// Fragment: the register combiners of a D3DPIXELSHADERDEF (up to 8 general
// stages + the final combiner), or, without a pixel shader, the D3D texture
// stage operations that the XDK would have turned into combiner settings.
//
// Field layout and semantics follow the NV2A as documented by the xemu project.
#include <cstdarg>
#include <cstdio>
#include <string>

#include "gpu.h"

namespace tje::gpu {

namespace {

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// ---------------------------------------------------------------- vertex program

uint32_t field(const uint32_t* t, int dw, int bit, int size) { return (t[dw] >> bit) & ((1u << size) - 1); }

enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT,
       MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };

const char* kOutNames[13] = {"R[12]", "oUnused1", "oUnused2", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
                             "oT0", "oT1", "oT2", "oT3"};

std::string vsh_source(const uint32_t* t, int which) {   // which: 0 A, 1 B, 2 C
    static const int neg_bit[3][2] = {{1, 8}, {2, 25}, {2, 10}};
    static const int swz_bit[3][2] = {{1, 6}, {2, 23}, {2, 8}};
    static const int mux_bit[3][2] = {{2, 26}, {2, 11}, {3, 28}};
    uint32_t mux = field(t, mux_bit[which][0], mux_bit[which][1], 2);
    uint32_t reg;
    if (which == 0) reg = field(t, 2, 28, 4);
    else if (which == 1) reg = field(t, 2, 13, 4);
    else reg = (field(t, 2, 0, 2) << 2) | field(t, 3, 30, 2);
    std::string base;
    if (mux == MUX_R) base = fmt("R[%u]", reg > 12 ? 12 : reg);
    else if (mux == MUX_V) base = fmt("v%u", field(t, 1, 9, 4));
    else if (mux == MUX_C) {
        uint32_t ci = field(t, 1, 13, 8);
        base = field(t, 3, 1, 1) ? fmt("C[clamp(A0 + %u, 0, 191)]", ci) : fmt("C[%u]", ci);
    } else base = "vec4(0.0)";
    const char comp[4] = {'x', 'y', 'z', 'w'};
    std::string swz = ".";
    int sw = swz_bit[which][1];
    for (int i = 0; i < 4; i++) swz += comp[field(t, swz_bit[which][0], sw - 2 * i, 2)];
    std::string s = "(" + base + swz + ")";
    if (field(t, neg_bit[which][0], neg_bit[which][1], 1)) s = "(-" + s + ")";
    return s;
}

std::string mask_str(uint32_t m) {
    std::string s;
    if (m & 8) s += 'x';
    if (m & 4) s += 'y';
    if (m & 2) s += 'z';
    if (m & 1) s += 'w';
    return s;
}

}  // namespace

std::string vertex_glsl(const uint32_t* program, uint32_t len) {
    bool writes_c = false;
    for (uint32_t i = 0; i < len; i++) {
        const uint32_t* t = program + i * 4;
        if (field(t, 3, 12, 4) && !field(t, 3, 11, 1)) writes_c = true;
    }
    std::string s;
    s += "#version 450\n";
    s += "layout(push_constant) uniform PC { vec4 surface; } pc;\n";   // w, h, zmax, 2D width scale (0 = none)
    s += "layout(set = 0, binding = 0, std140) uniform VS { vec4 c[192]; vec4 vdef[16]; uint mask; } vs;\n";
    for (int i = 0; i < 16; i++) s += fmt("layout(location = %d) in vec4 in_v%d;\n", i, i);
    s += "layout(location = 0) out vec4 out_d0;\nlayout(location = 1) out vec4 out_d1;\n"
         "layout(location = 2) out vec4 out_b0;\nlayout(location = 3) out vec4 out_b1;\n"
         "layout(location = 4) out vec4 out_fog;\n";
    for (int i = 0; i < 4; i++) s += fmt("layout(location = %d) out vec4 out_t%d;\n", 5 + i, i);
    s += "void main() {\n";
    for (int i = 0; i < 16; i++)
        s += fmt("  vec4 v%d = (vs.mask & %uu) != 0u ? in_v%d : vs.vdef[%d];\n", i, 1u << i, i, i);
    if (writes_c) s += "  vec4 C[192] = vs.c;\n";
    else s += "#define C vs.c\n";
    s += "  vec4 R[13];\n  for (int i = 0; i < 13; i++) R[i] = vec4(0.0, 0.0, 0.0, 1.0);\n";
    s += "  vec4 oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = oD0, oB0 = oD0, oB1 = oD0, oFog = oD0, oPts = oD0;\n"
         "  vec4 oT0 = oD0, oT1 = oD0, oT2 = oD0, oT3 = oD0, oUnused1, oUnused2;\n  int A0 = 0;\n";
    for (uint32_t i = 0; i < len; i++) {
        const uint32_t* t = program + i * 4;
        uint32_t ilu = field(t, 1, 25, 3), mac = field(t, 1, 21, 4);
        uint32_t mac_mask = field(t, 3, 24, 4), out_r = field(t, 3, 20, 4), ilu_mask = field(t, 3, 16, 4);
        uint32_t o_mask = field(t, 3, 12, 4), orb = field(t, 3, 11, 1), addr = field(t, 3, 3, 8),
                 omux = field(t, 3, 2, 1);
        s += fmt("  { // %u: %08x %08x %08x\n", i, t[1], t[2], t[3]);
        std::string A = vsh_source(t, 0), B = vsh_source(t, 1), C = vsh_source(t, 2);
        std::string out_name;
        if (o_mask) {
            if (orb) out_name = addr < 13 ? kOutNames[addr] : "oUnused1";
            else out_name = fmt("C[%u]", addr & 0xff);
        }
        if (mac) {
            const char* e = "vec4(0.0)";
            std::string ex;
            switch (mac) {
            case MAC_MOV: ex = A; break;
            case MAC_MUL: ex = A + " * " + B; break;
            case MAC_ADD: ex = A + " + " + C; break;
            case MAC_MAD: ex = A + " * " + B + " + " + C; break;
            case MAC_DP3: ex = "vec4(dot(" + A + ".xyz, " + B + ".xyz))"; break;
            case MAC_DPH: ex = "vec4(dot(" + A + ".xyz, " + B + ".xyz) + " + B + ".w)"; break;
            case MAC_DP4: ex = "vec4(dot(" + A + ", " + B + "))"; break;
            case MAC_DST: ex = "vec4(1.0, " + A + ".y * " + B + ".y, " + A + ".z, " + B + ".w)"; break;
            case MAC_MIN: ex = "min(" + A + ", " + B + ")"; break;
            case MAC_MAX: ex = "max(" + A + ", " + B + ")"; break;
            case MAC_SLT: ex = "vec4(lessThan(" + A + ", " + B + "))"; break;
            case MAC_SGE: ex = "vec4(greaterThanEqual(" + A + ", " + B + "))"; break;
            case MAC_ARL: ex = A; break;
            default: ex = e; break;
            }
            s += "    vec4 mac = " + ex + ";\n";
        }
        if (ilu) {
            std::string x = C + ".x";
            std::string ex;
            switch (ilu) {
            case ILU_MOV: ex = C; break;
            case ILU_RCP: ex = "vec4(1.0 / " + x + ")"; break;
            case ILU_RCC:
                ex = "vec4(sign(" + x + ") * clamp(1.0 / abs(" + x + "), 5.42101e-20, 1.884467e19))";
                break;
            case ILU_RSQ: ex = "vec4(inversesqrt(abs(" + x + ")))"; break;
            case ILU_EXP: ex = "vec4(exp2(floor(" + x + ")), fract(" + x + "), exp2(" + x + "), 1.0)"; break;
            case ILU_LOG:
                ex = "vec4(floor(log2(abs(" + x + "))), abs(" + x + ") / exp2(floor(log2(abs(" + x + ")))), log2(abs(" +
                     x + ")), 1.0)";
                break;
            case ILU_LIT:
                s += "    vec4 lt = " + C + ";\n";
                ex = "vec4(1.0, max(lt.x, 0.0), (lt.x > 0.0 && lt.y > 0.0) ? pow(lt.y, clamp(lt.w, -127.9961, "
                     "127.9961)) : 0.0, 1.0)";
                break;
            default: ex = "vec4(0.0)"; break;
            }
            s += "    vec4 ilu = " + ex + ";\n";
        }
        // Writes, after every read.
        if (mac == MAC_ARL) s += "    A0 = int(floor(mac.x + 0.0001));\n";
        else if (mac) {
            if (mac_mask) s += fmt("    R[%u].%s = mac.%s;\n", out_r > 12 ? 12 : out_r, mask_str(mac_mask).c_str(),
                                   mask_str(mac_mask).c_str());
            if (o_mask && omux == 0)
                s += "    " + out_name + "." + mask_str(o_mask) + " = mac." + mask_str(o_mask) + ";\n";
        }
        if (ilu) {
            uint32_t r = mac ? 1 : out_r;
            if (ilu_mask) s += fmt("    R[%u].%s = ilu.%s;\n", r > 12 ? 12 : r, mask_str(ilu_mask).c_str(),
                                   mask_str(ilu_mask).c_str());
            if (o_mask && omux == 1)
                s += "    " + out_name + "." + mask_str(o_mask) + " = ilu." + mask_str(o_mask) + ";\n";
        }
        s += "  }\n";
        if (field(t, 3, 0, 1)) break;
    }
    // Screen space (x, y in pixels, z in depth-buffer units, already divided by w) -> clip space.
    s += "  vec4 p = R[12];\n"
         "  float w = p.w;\n"
         "  if (w == 0.0 || isinf(w) || isnan(w)) w = 1.0;\n"
         "  vec3 ndc = vec3(p.x * 2.0 / pc.surface.x - 1.0, p.y * 2.0 / pc.surface.y - 1.0, p.z / pc.surface.z);\n"
         // Widescreen: 2D keeps its shape in the middle; edges that touch the screen's sides stay there,
         // so full-screen fades and backdrops still cover everything.
         "  if (pc.surface.w < 0.0 || (pc.surface.w > 0.0 && abs(abs(ndc.x) - 1.0) > 0.004)) ndc.x *= abs(pc.surface.w);\n"
         "  gl_Position = vec4(ndc * w, w);\n"
         "  gl_PointSize = 1.0;\n"
         "  out_d0 = clamp(oD0, 0.0, 1.0);\n  out_d1 = clamp(oD1, 0.0, 1.0);\n"
         "  out_b0 = clamp(oB0, 0.0, 1.0);\n  out_b1 = clamp(oB1, 0.0, 1.0);\n"
         "  out_fog = oFog;\n  out_t0 = oT0;\n  out_t1 = oT1;\n  out_t2 = oT2;\n  out_t3 = oT3;\n"
         "}\n";
    return s;
}

// ---------------------------------------------------------------- fragment

namespace {

const char* kRegNames[16] = {"vec4(0.0)", "c0", "c1", "fogr", "v0", "v1", "vec4(0.0)", "vec4(0.0)",
                             "t0", "t1", "t2", "t3", "r0", "r1", "v1r0", "efprod"};

std::string map_input(const std::string& x, uint32_t m) {
    switch (m) {
    case 0: return "max(" + x + ", 0.0)";
    case 1: return "(1.0 - clamp(" + x + ", 0.0, 1.0))";
    case 2: return "(2.0 * max(" + x + ", 0.0) - 1.0)";
    case 3: return "(-2.0 * max(" + x + ", 0.0) + 1.0)";
    case 4: return "(max(" + x + ", 0.0) - 0.5)";
    case 5: return "(-max(" + x + ", 0.0) + 0.5)";
    case 6: return x;
    default: return "(-" + x + ")";
    }
}

// One combiner input byte: register bits 0-3, alpha bit 4, mapping bits 5-7.
std::string comb_input(uint32_t b, bool rgb) {
    uint32_t reg = b & 15, alpha = (b >> 4) & 1, map = (b >> 5) & 7;
    std::string r = kRegNames[reg];
    std::string x = rgb ? (alpha ? r + ".aaa" : r + ".rgb") : (alpha ? r + ".a" : r + ".b");
    return map_input(x, map);
}

std::string out_map(const std::string& x, uint32_t m) {
    switch (m) {
    case 1: return "(" + x + " - 0.5)";
    case 2: return "(" + x + " * 2.0)";
    case 3: return "((" + x + " - 0.5) * 2.0)";
    case 4: return "(" + x + " * 4.0)";
    case 6: return "(" + x + " / 2.0)";
    default: return x;
    }
}

// Texture fetch helpers shared by both fragment flavours.
std::string tex_fetch(int i, uint32_t kind, const std::string& coord_xy, const std::string& coord_xyz) {
    if (kind == 3)                   // depth texture: shadow compare of the projected r against the stored depth
        return fmt("vec4(shadow_cmp(clamp((%s).z / ps.texscale[%d].z, 0.0, 1.0), texture(t2d[%d], (%s) * ps.texscale[%d].xy).r))",
                   coord_xyz.c_str(), i, i, coord_xy.c_str(), i);
    if (kind == 2) return fmt("texture(tcube[%d], %s)", i, coord_xyz.c_str());
    if (kind == 1) return fmt("texture(t2d[%d], (%s) * ps.texscale[%d].xy)", i, coord_xy.c_str(), i);
    return "vec4(0.0, 0.0, 0.0, 1.0)";
}

std::string fragment_header() {
    return "#version 450\n"
           "layout(location = 0) in vec4 in_d0;\nlayout(location = 1) in vec4 in_d1;\n"
           "layout(location = 2) in vec4 in_b0;\nlayout(location = 3) in vec4 in_b1;\n"
           "layout(location = 4) in vec4 in_fog;\n"
           "layout(location = 5) in vec4 in_t0;\nlayout(location = 6) in vec4 in_t1;\n"
           "layout(location = 7) in vec4 in_t2;\nlayout(location = 8) in vec4 in_t3;\n"
           "layout(location = 0) out vec4 frag;\n"
           "layout(set = 0, binding = 1, std140) uniform PS { vec4 c0[8]; vec4 c1[8]; vec4 fc0; vec4 fc1;"
           " vec4 tfactor; vec4 fogcolor; vec4 texscale[4]; vec4 alpha; vec4 fog; vec4 fog2; } ps;\n"
           "layout(set = 0, binding = 2) uniform sampler2D t2d[4];\n"
           "layout(set = 0, binding = 3) uniform samplerCube tcube[4];\n"
           "layout(set = 0, binding = 4) uniform sampler2D tlayer[4];\n"
           // NV2A fog unit: factor 1 = no fog; fog off = 1.
           "float fog_factor() {\n"
           "  if (ps.fog.x == 0.0) return 1.0;\n"
           "  float c = abs(in_fog.x);\n"
           "  int m = int(ps.fog.y);\n"
           "  if (m == 1) return clamp(exp(-ps.fog2.x * c), 0.0, 1.0);\n"
           "  if (m == 2) return clamp(exp(-(ps.fog2.x * c) * (ps.fog2.x * c)), 0.0, 1.0);\n"
           "  return clamp((ps.fog.w - c) / max(ps.fog.w - ps.fog.z, 1e-6), 0.0, 1.0);\n"
           "}\n"
           // NV2A shadow compare: the stored depth FUNC the projected r (in that order).
           "float shadow_cmp(float r, float d) {\n"
           "  int f = int(ps.alpha.w);\n"
           "  bool pass = f == 0 ? false : f == 1 ? d < r : f == 2 ? d == r : f == 3 ? d <= r :\n"
           "              f == 4 ? d > r : f == 5 ? d != r : f == 6 ? d >= r : true;\n"
           "  return pass ? 1.0 : 0.0;\n"
           "}\n";
}

std::string fragment_footer() {
    // Alpha test (NV2A ALPHA_FUNC is an OpenGL compare function, the reference 0..255).
    return "  if (ps.alpha.x != 0.0) {\n"
           "    float a = floor(clamp(frag.a, 0.0, 1.0) * 255.0 + 0.5), r = ps.alpha.z;\n"
           "    int f = int(ps.alpha.y);\n"
           "    bool pass = f == 0 ? false : f == 1 ? a < r : f == 2 ? a == r : f == 3 ? a <= r :\n"
           "                f == 4 ? a > r : f == 5 ? a != r : f == 6 ? a >= r : true;\n"
           "    if (!pass) discard;\n"
           "  }\n"
           "}\n";
}

std::string combiners(const Draw& d, uint32_t kinds) {
    const uint32_t* p = d.psdef;
    auto PSAlphaInputs = [&](int i) { return p[0x00 / 4 + i]; };
    uint32_t final_abcd = p[0x20 / 4], final_efg = p[0x24 / 4];
    auto PSAlphaOutputs = [&](int i) { return p[0x68 / 4 + i]; };
    auto PSRGBInputs = [&](int i) { return p[0x88 / 4 + i]; };
    uint32_t compare = p[0xa8 / 4];
    auto PSRGBOutputs = [&](int i) { return p[0xb4 / 4 + i]; };
    uint32_t count_word = p[0xd4 / 4], tex_modes = p[0xd8 / 4];
    uint32_t stages = count_word & 0xff;
    if (stages > 8) stages = 8;
    bool unique_c0 = count_word & 0x10000, unique_c1 = count_word & 0x100000, mux_msb = count_word & 0x100;

    std::string s = fragment_header();
    s += "void main() {\n";
    s += "  vec4 v0 = in_d0, v1 = in_d1;\n";
    s += "  vec4 fogr = vec4(ps.fogcolor.rgb, fog_factor());\n";
    for (int i = 0; i < 4; i++) {
        uint32_t mode = (tex_modes >> (5 * i)) & 31;
        uint32_t kind = (kinds >> (2 * i)) & 3;
        std::string tc = fmt("in_t%d", i);
        switch (mode) {
        case 0: s += fmt("  vec4 t%d = vec4(0.0);\n", i); break;
        case 1: case 2: case 6: case 7:     // project 2D/3D; bump-env stages fetch plainly for now
            if (kind == 3) {                 // shadow map: lit only where both layers are lit
                s += fmt("  vec2 suv%d = (%s.xy / %s.w) * ps.texscale[%d].xy;\n", i, tc.c_str(), tc.c_str(), i);
                s += fmt("  float sref%d = clamp((%s.z / %s.w) / ps.texscale[%d].z, 0.0, 1.0);\n", i, tc.c_str(), tc.c_str(), i);
                s += fmt("  float sa%d = shadow_cmp(sref%d, texture(t2d[%d], suv%d).r);\n", i, i, i, i);
                s += fmt("  float sb%d = ps.texscale[%d].w > 0.5 ? shadow_cmp(sref%d, texture(tlayer[%d], suv%d).r) : 1.0;\n",
                         i, i, i, i, i);
                s += fmt("  vec4 t%d = vec4(sa%d * sb%d);\n", i, i, i);
                break;
            }
            s += fmt("  vec4 t%d = %s;\n", i, tex_fetch(i, kind == 2 ? 1 : kind, tc + ".xy / " + tc + ".w", tc + ".xyz / " + tc + ".w").c_str());
            break;
        case 3: s += fmt("  vec4 t%d = %s;\n", i, tex_fetch(i, kind ? 2 : 0, "", tc + ".xyz").c_str()); break;
        case 4: s += fmt("  vec4 t%d = clamp(%s, 0.0, 1.0);\n", i, tc.c_str()); break;
        case 5: {
            s += fmt("  vec4 t%d = vec4(0.0);\n", i);
            const char comp[4] = {'x', 'y', 'z', 'w'};
            for (int k = 0; k < 4; k++) {
                bool ge = (compare >> (4 * i + k)) & 1;
                s += fmt("  if (%s.%c %s 0.0) discard;\n", tc.c_str(), comp[k], ge ? ">=" : "<");
            }
            break;
        }
        default: s += fmt("  vec4 t%d = vec4(0.0); // texture mode %u not implemented\n", i, mode); break;
        }
    }
    // Shadow-map stages: their result also feeds the optional shadow enhancement.
    std::string shadow_lit, overlap;
    for (int i = 0; i < 4; i++) {
        uint32_t mode = (tex_modes >> (5 * i)) & 31;
        if (((kinds >> (2 * i)) & 3) == 3 && (mode == 1 || mode == 2 || mode == 6 || mode == 7)) {
            shadow_lit = shadow_lit.empty() ? fmt("t%d.r", i) : fmt("min(%s, t%d.r)", shadow_lit.c_str(), i);
            std::string ov = fmt("(ps.texscale[%d].w > 0.5 ? (1.0 - sa%d) * (1.0 - sb%d) : 0.0)", i, i, i);
            overlap = overlap.empty() ? ov : "max(" + overlap + ", " + ov + ")";
        }
    }
    s += "  vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a), r1 = vec4(0.0);\n";
    s += "  vec4 c0, c1, efprod = vec4(0.0), v1r0 = vec4(0.0);\n";
    for (uint32_t i = 0; i < stages; i++) {
        s += fmt("  { // stage %u\n", i);
        s += fmt("    c0 = ps.c0[%u]; c1 = ps.c1[%u];\n", unique_c0 ? i : 0, unique_c1 ? i : 0);
        for (int part = 0; part < 2; part++) {   // 0 rgb, 1 alpha
            bool rgb = part == 0;
            uint32_t in = rgb ? PSRGBInputs(i) : PSAlphaInputs(i);
            uint32_t out = rgb ? PSRGBOutputs(i) : PSAlphaOutputs(i);
            const char* ty = rgb ? "vec3" : "float";
            const char* pf = rgb ? "rgb" : "a";
            std::string A = comb_input(in >> 24, rgb), B = comb_input(in >> 16, rgb), C = comb_input(in >> 8, rgb),
                        D = comb_input(in, rgb);
            bool cd_dot = (out >> 12) & 1, ab_dot = (out >> 13) & 1, mux = (out >> 14) & 1;
            uint32_t map = (out >> 15) & 7;
            std::string ab = (rgb && ab_dot) ? "vec3(dot(" + A + ", " + B + "))" : A + " * " + B;
            std::string cd = (rgb && cd_dot) ? "vec3(dot(" + C + ", " + D + "))" : C + " * " + D;
            s += fmt("    %s %s_ab = %s;\n", ty, pf, ab.c_str());
            s += fmt("    %s %s_cd = %s;\n", ty, pf, cd.c_str());
            if (mux)
                s += fmt("    %s %s_sum = %s ? %s_cd : %s_ab;\n", ty, pf,
                         mux_msb ? "(r0.a >= 0.5)" : "((int(r0.a * 255.0 + 0.5) & 1) != 0)", pf, pf);
            else s += fmt("    %s %s_sum = %s_ab + %s_cd;\n", ty, pf, pf, pf);
            for (const char* n : {"ab", "cd", "sum"})
                s += fmt("    %s_%s = clamp(%s, -1.0, 1.0);\n", pf, n, out_map(std::string(pf) + "_" + n, map).c_str());
        }
        // Writes after both halves have read their inputs.
        for (int part = 0; part < 2; part++) {
            bool rgb = part == 0;
            uint32_t out = rgb ? PSRGBOutputs(i) : PSAlphaOutputs(i);
            const char* pf = rgb ? "rgb" : "a";
            uint32_t cd_reg = out & 15, ab_reg = (out >> 4) & 15, sum_reg = (out >> 8) & 15;
            auto write = [&](uint32_t reg, const char* what) {
                if (reg == 0 || reg == 1 || reg == 2 || reg == 3 || reg == 6 || reg == 7 || reg > 13) return;
                s += fmt("    %s.%s = %s_%s;\n", kRegNames[reg], pf, pf, what);
            };
            write(ab_reg, "ab");
            write(cd_reg, "cd");
            write(sum_reg, "sum");
            if (rgb) {
                if ((out >> 19) & 1 && ab_reg) s += fmt("    %s.a = rgb_ab.b;\n", kRegNames[ab_reg]);
                if ((out >> 18) & 1 && cd_reg) s += fmt("    %s.a = rgb_cd.b;\n", kRegNames[cd_reg]);
            }
        }
        s += "  }\n";
    }
    if (final_abcd || final_efg) {
        s += "  c0 = ps.fc0; c1 = ps.fc1;\n";
        uint32_t flags = final_efg & 0xff;
        std::string r0x = (flags & 0x20) ? "(1.0 - clamp(r0.rgb, 0.0, 1.0))" : "r0.rgb";
        std::string v1x = (flags & 0x40) ? "(1.0 - clamp(v1.rgb, 0.0, 1.0))" : "v1.rgb";
        s += "  v1r0 = vec4(" + r0x + " + " + v1x + ", 0.0);\n";
        if (flags & 0x80) s += "  v1r0 = clamp(v1r0, 0.0, 1.0);\n";
        s += "  efprod = vec4(" + comb_input(final_efg >> 24, true) + " * " + comb_input(final_efg >> 16, true) +
             ", 0.0);\n";
        std::string A = comb_input(final_abcd >> 24, true), B = comb_input(final_abcd >> 16, true),
                    C = comb_input(final_abcd >> 8, true), D = comb_input(final_abcd, true);
        s += "  frag.rgb = " + A + " * " + B + " + (1.0 - " + A + ") * " + C + " + " + D + ";\n";
        s += "  frag.a = " + comb_input(final_efg >> 8, false) + ";\n";
        s += "  frag = clamp(frag, 0.0, 1.0);\n";
    } else {
        // No final combiner in the definition: D3D's default, r0 blended with fog when fog is on.
        s += "  frag = clamp(r0, 0.0, 1.0);\n";
        s += "  if (ps.fog.x != 0.0) frag.rgb = mix(ps.fogcolor.rgb, frag.rgb, fogr.a);\n";
    }
    // Enhancement (option shadow_enhance, off by default): darken whatever is in the
    // dynamic shadow on top of the game's result, so overlapping shadows deepen.
    if (!shadow_lit.empty()) {
        // Enhancement: where a character's shadow falls inside a scenery shadow, darken.
        s += "  if (ps.fog2.y > 0.0 && ps.alpha.w > 0.5 && ps.alpha.w < 6.5)\n"
             "    frag.rgb *= 1.0 - ps.fog2.y * " + overlap + ";\n";
        // Debugging (TJE_SHADOW_DEBUG): overlapping shadows pure blue, other shadowed
        // pixels pure red, lit ones pure green.
        s += "  if (ps.fog2.z > 0.0 && ps.alpha.w > 0.5 && ps.alpha.w < 6.5)\n"
             "    frag = " + overlap + " > 0.5 ? vec4(0.0, 0.0, 1.0, 1.0) : " + shadow_lit +
             " < 0.5 ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, 0.0, 1.0);\n";
    }
    s += fragment_footer();
    return s;
}

// Fixed-function texture stages (no pixel shader set). D3DTOP values are the
// Xbox ones (DISABLE = 1, SELECTARG1 = 2, ...).
std::string stage_arg(uint32_t a) {
    static const char* sel[8] = {"in_d0", "cur", "tex", "ps.tfactor", "in_d1", "tmp", "vec4(0.0)", "vec4(0.0)"};
    std::string x = sel[a & 7];
    if (a & 0x20) x = "vec4(" + x + ".a)";
    if (a & 0x10) x = "(1.0 - " + x + ")";
    return x;
}

std::string stage_op(uint32_t op, const std::string& a0, const std::string& a1, const std::string& a2) {
    switch (op) {
    case 2: return a1;
    case 3: return a2;
    case 4: return a1 + " * " + a2;
    case 5: return a1 + " * " + a2 + " * 2.0";
    case 6: return a1 + " * " + a2 + " * 4.0";
    case 7: return a1 + " + " + a2;
    case 8: return a1 + " + " + a2 + " - 0.5";
    case 9: return "(" + a1 + " + " + a2 + " - 0.5) * 2.0";
    case 10: return a1 + " - " + a2;
    case 11: return a1 + " + " + a2 + " - " + a1 + " * " + a2;
    case 12: return "mix(" + a2 + ", " + a1 + ", in_d0.a)";
    case 13: return "mix(" + a2 + ", " + a1 + ", cur.a)";
    case 14: return "mix(" + a2 + ", " + a1 + ", tex.a)";
    case 15: return "mix(" + a2 + ", " + a1 + ", ps.tfactor.a)";
    case 16: return a1 + " + " + a2 + " * (1.0 - tex.a)";
    case 17: return a1;
    case 18: return "vec4(" + a1 + ".rgb + " + a1 + ".a * " + a2 + ".rgb, " + a1 + ".a)";
    case 19: return "vec4(" + a1 + ".rgb * " + a2 + ".rgb + " + a1 + ".a, " + a1 + ".a)";
    case 20: return "vec4((1.0 - " + a1 + ".a) * " + a2 + ".rgb + " + a1 + ".rgb, " + a1 + ".a)";
    case 21: return "vec4((1.0 - " + a1 + ".rgb) * " + a2 + ".rgb + " + a1 + ".a, " + a1 + ".a)";
    case 22: return "vec4(dot((" + a1 + ".rgb - 0.5) * 2.0, (" + a2 + ".rgb - 0.5) * 2.0))";
    case 23: return a0 + " + " + a1 + " * " + a2;
    case 24: return "mix(" + a2 + ", " + a1 + ", " + a0 + ")";
    default: return "cur";
    }
}

std::string fixed_function(const Draw& d, uint32_t kinds) {
    std::string s = fragment_header();
    s += "void main() {\n  vec4 cur = in_d0, tmp = vec4(0.0), tex, r;\n";
    for (int i = 0; i < 4; i++) {
        const uint32_t* t = d.tss[i];
        uint32_t cop = t[12], aop = t[16];
        if (cop <= 1) break;
        uint32_t kind = (kinds >> (2 * i)) & 3;
        uint32_t tci = t[28] & 3;
        bool projected = (t[21] & 0x100) != 0;
        std::string tc = fmt("in_t%u", tci);
        std::string xy = projected ? tc + ".xy / " + tc + ".w" : tc + ".xy";
        s += fmt("  // stage %d: colorop %u alphaop %u\n", i, cop, aop);
        s += "  tex = " + tex_fetch(i, kind, xy, tc + ".xyz") + ";\n";
        s += "  r.rgb = clamp((" + stage_op(cop, stage_arg(t[13]), stage_arg(t[14]), stage_arg(t[15])) +
             ").rgb, 0.0, 1.0);\n";
        if (aop > 1)
            s += "  r.a = clamp((" + stage_op(aop, stage_arg(t[17]), stage_arg(t[18]), stage_arg(t[19])) +
                 ").a, 0.0, 1.0);\n";
        else s += "  r.a = cur.a;\n";
        s += (t[20] == 5) ? "  tmp = r;\n" : "  cur = r;\n";
    }
    s += "  frag = clamp(cur, 0.0, 1.0);\n";
    s += "  if (ps.fog.x != 0.0) frag.rgb = mix(ps.fogcolor.rgb, frag.rgb, fog_factor());\n";
    s += fragment_footer();
    return s;
}

}  // namespace

std::string fragment_glsl(const Draw& d, uint32_t kinds) {
    return d.fixed_function_ps ? fixed_function(d, kinds) : combiners(d, kinds);
}

}  // namespace tje::gpu
