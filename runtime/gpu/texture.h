#pragma once
#include <cstdint>
#include <vector>

#include "gpu.h"

namespace tje::gpu {

struct FormatInfo { uint32_t bpp; bool linear; bool compressed; bool color; };
FormatInfo format_info(uint32_t format);
uint32_t level_size(uint32_t format, uint32_t w, uint32_t h, uint32_t pitch);
uint32_t texture_size(const TextureRef& t);          // bytes of guest data, all levels (and faces)

struct DecodedLevel { uint32_t face, level, width, height; size_t offset; };
// All faces and levels to RGBA8, tightly packed, in `levels` order. False if the
// format is not supported (those texels come out magenta).
bool decode_texture(const TextureRef& t, const uint8_t* src, const uint8_t* palette, std::vector<uint8_t>& out,
                    std::vector<DecodedLevel>& levels);

}  // namespace tje::gpu
