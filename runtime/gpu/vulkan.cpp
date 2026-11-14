// Vulkan back end for the D3D8 HLE.
//
// Every render target the game uses (the back buffer, shadow maps, other
// render-to-texture surfaces) is a Vulkan colour or depth image of its size x
// scale, keyed by its guest memory address; a texture whose memory is a render
// target samples that image (depth surfaces as shadow maps). Each guest frame records into one command buffer (texture
// uploads go to a second one that is submitted first); Swap blits the target
// to the window's swapchain, letterboxed to 4:3. Two frames are in flight,
// each with its own upload ring (vertices, indices, uniforms, texture data)
// and descriptor pool.
//
// Vulkan and shaderc are loaded at run time, so the build needs only headers.
// Calls arrive from guest threads, which the scheduler serialises.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gpu.h"
#include "guest.h"
#include "settings.h"
#include "texture.h"
#include "vk.h"
#include "window.h"

#include <shaderc/shaderc.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#ifdef __ANDROID__
#include <cstdio>
#include <filesystem>
#include <string>
namespace tje { std::string android_files_dir(); }
#endif

namespace tje::vk {
#define TJE_VK_DEFINE(f) PFN_##f f = nullptr;
TJE_VK_INSTANCE_FUNCS(TJE_VK_DEFINE)
TJE_VK_DEVICE_FUNCS(TJE_VK_DEFINE)
#undef TJE_VK_DEFINE
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
PFN_vkCreateInstance vkCreateInstance = nullptr;
}  // namespace tje::vk

namespace tje::gpu {

using namespace tje::vk;

namespace {

constexpr int kFrames = 2;
constexpr VkDeviceSize kRingSize = 64ull << 20;
constexpr uint32_t kSurfaceW = 640, kSurfaceH = 480;
constexpr VkFormat kColorFormat = VK_FORMAT_B8G8R8A8_UNORM;

std::recursive_mutex g_mu;
bool g_active = false;
Options g_opt;

VkInstance g_inst = VK_NULL_HANDLE;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
VkPhysicalDeviceProperties g_props{};
VkPhysicalDeviceMemoryProperties g_memprops{};
VkDevice g_dev = VK_NULL_HANDLE;
uint32_t g_qfam = 0;
VkQueue g_queue = VK_NULL_HANDLE;
VkSurfaceKHR g_surface = VK_NULL_HANDLE;

VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
std::vector<VkImage> g_sc_images;
std::vector<VkSemaphore> g_sc_done;
VkFormat g_sc_format = VK_FORMAT_B8G8R8A8_UNORM;
VkExtent2D g_sc_extent{};
bool g_sc_dirty = false;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};
struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* ptr = nullptr;
    VkDeviceSize size = 0;
};

VkFormat g_depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
VkRenderPass g_rp = VK_NULL_HANDLE;

// Render targets.
struct Surface {
    Image img;
    VkImageView sample_view = VK_NULL_HANDLE;    // depth surfaces: depth aspect only
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t addr = 0, nw = 0, nh = 0;           // guest address, size in Xbox pixels
    bool depth = false;
    bool wide = false;                           // stretched sideways for widescreen
    // Shadow maps (depth surfaces sampled as textures): with the shadow_enhance
    // option, casters drawn by animated (matrix-palette) vertex programs go to a
    // second depth layer, so receivers can tell overlapping shadows apart.
    bool shadow = false;
    Surface* alt = nullptr;
    bool needs_clear = false;
};
std::unordered_map<uint64_t, Surface*> g_surfaces;
std::unordered_map<uint64_t, VkFramebuffer> g_framebuffers;
Surface* g_cur_color = nullptr;
Surface* g_pass_depth = nullptr;                 // depth layer of the open render pass
Surface* g_cur_depth = nullptr;
Surface* g_back = nullptr;                       // what Swap shows
uint32_t g_rt_w = 0, g_rt_h = 0;                 // current target, in Vulkan pixels

// Picture options (tje3.ini), re-read every frame so they apply while playing.
struct Video {
    int mode = 0;             // 0 original 4:3, 1 widescreen, 2 stretch, 3 sharp
    int scale = 2;            // render target pixels per Xbox pixel
    float wide = 1.0f;        // screen-sized targets are this many times wider (widescreen)
    bool smooth = true, vsync = false;
} g_video;
struct TargetArgs { uint32_t color = 0, cw = 0, ch = 0, depth = 0, dw = 0, dh = 0; bool set = false; };
TargetArgs g_last_target, g_last_back;
bool g_video_logged = false;
bool g_frame_3d = false;           // something other than 2D was drawn to the screen this frame
VkDescriptorSetLayout g_dsl = VK_NULL_HANDLE;
VkPipelineLayout g_pl = VK_NULL_HANDLE;
Image g_dummy2d, g_dummycube;
VkSampler g_dummy_sampler = VK_NULL_HANDLE;
Buffer g_zero;

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE, upload = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    Buffer ring;
    VkDeviceSize ring_off = 0;
    std::vector<VkDescriptorPool> pools;
    size_t pool_index = 0;
    std::vector<std::function<void()>> garbage;
    bool upload_open = false;
};
Frame g_frames[kFrames];
uint32_t g_fi = 0;
uint64_t g_serial = 1;
uint64_t g_epoch = 1;
float g_shadow_enhance = 0.0f;  // option shadow_enhance (0..1), read once per frame           // bumped whenever guest texture memory may have changed
bool g_frame_open = false, g_in_pass = false;

// shaderc, loaded at run time
struct Shaderc {
    decltype(&shaderc_compiler_initialize) compiler_initialize;
    decltype(&shaderc_compile_options_initialize) options_initialize;
    decltype(&shaderc_compile_options_set_optimization_level) set_optimization_level;
    decltype(&shaderc_compile_into_spv) compile_into_spv;
    decltype(&shaderc_result_get_compilation_status) get_status;
    decltype(&shaderc_result_get_bytes) get_bytes;
    decltype(&shaderc_result_get_length) get_length;
    decltype(&shaderc_result_get_error_message) get_error;
    decltype(&shaderc_result_release) release;
    shaderc_compiler_t compiler;
    shaderc_compile_options_t options;
} g_sc;

std::unordered_map<uint64_t, VkShaderModule> g_vs_cache, g_fs_cache;
std::unordered_map<uint64_t, VkPipeline> g_pipelines;
VkPipelineCache g_pcache = VK_NULL_HANDLE;      // kept on disk on handhelds, where building pipelines is slow
std::unordered_map<uint64_t, VkSampler> g_samplers;

struct Tex {
    Image img;
    bool cube = false;
    uint64_t hash = 0, checked = 0, used = 0;   // checked: g_epoch; used: g_serial
    uint32_t levels = 1;
};
std::unordered_map<uint64_t, Tex> g_textures;

// TJE_GPU_STATS=<n>: every n frames, read the back buffer back and log numbers
// about it (mean colour, coverage), so output can be checked without looking at it.
uint32_t g_stats_every = 0;
Buffer g_readback;

// statistics for the title bar
uint32_t g_stat_draws = 0, g_stat_frames = 0, g_stat_uploads = 0;
double g_stat_cpu_ms = 0;                      // time spent in the renderer (guest thread)
struct CpuTimer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~CpuTimer() { g_stat_cpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
};
std::chrono::steady_clock::time_point g_stat_t0 = std::chrono::steady_clock::now();

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { hle_log("[gpu] %s failed: %d", #x, (int)r_); return false; } } while (0)

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 0x9e3779b97f4a7c15ull) {
    const uint8_t* b = (const uint8_t*)p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, b + i, 8);
        h ^= v;
        h *= 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    h ^= h >> 32;
    h *= 0xd6e8feb86659fd93ull;
    h ^= h >> 32;
    return h;
}

// ---------------------------------------------------------------- loading

#ifdef _WIN32
void* load_lib(const char* name) { return (void*)LoadLibraryA(name); }
void* lib_sym(void* lib, const char* name) { return (void*)GetProcAddress((HMODULE)lib, name); }
#else
void* load_lib(const char* name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void* lib_sym(void* lib, const char* name) { return dlsym(lib, name); }
#endif

bool load_shaderc() {
#ifdef __ANDROID__
#define SC(field, name) g_sc.field = &name;      // linked in
#else
    void* lib = load_lib("shaderc_shared.dll");
    if (!lib) {
        const char* sdk = std::getenv("VULKAN_SDK");
        if (sdk) lib = load_lib((std::string(sdk) + "\\Bin\\shaderc_shared.dll").c_str());
    }
    if (!lib) lib = load_lib("G:\\DevTools\\VulkanSDK\\1.4.363.0\\Bin\\shaderc_shared.dll");
    if (!lib) { hle_log("[gpu] shaderc_shared.dll not found"); return false; }
#define SC(field, name) g_sc.field = (decltype(g_sc.field))lib_sym(lib, #name); if (!g_sc.field) return false;
#endif
    SC(compiler_initialize, shaderc_compiler_initialize)
    SC(options_initialize, shaderc_compile_options_initialize)
    SC(set_optimization_level, shaderc_compile_options_set_optimization_level)
    SC(compile_into_spv, shaderc_compile_into_spv)
    SC(get_status, shaderc_result_get_compilation_status)
    SC(get_bytes, shaderc_result_get_bytes)
    SC(get_length, shaderc_result_get_length)
    SC(get_error, shaderc_result_get_error_message)
    SC(release, shaderc_result_release)
#undef SC
    g_sc.compiler = g_sc.compiler_initialize();
    g_sc.options = g_sc.options_initialize();
#ifdef __ANDROID__
    g_sc.set_optimization_level(g_sc.options, shaderc_optimization_level_zero);   // the driver optimises anyway
#else
    g_sc.set_optimization_level(g_sc.options, shaderc_optimization_level_performance);
#endif
    return g_sc.compiler != nullptr;
}

#ifdef __ANDROID__
// Compiled shaders and pipelines stay on disk, so an effect only ever costs
// time the first time it is seen. Files are written whole or not at all.
std::string cache_dir() {
    static const std::string dir = [] {
        std::string d = tje::android_files_dir() + "/shader_cache";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

std::vector<uint8_t> read_cache(const std::string& path) {
    std::vector<uint8_t> out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        out.resize((size_t)std::ftell(f));
        std::fseek(f, 0, SEEK_SET);
        if (!out.empty() && std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
        std::fclose(f);
    }
    return out;
}

void write_cache(const std::string& path, const void* data, size_t size) {
    std::string temp = path + ".tmp";
    FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f) return;
    bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    std::error_code ec;
    if (ok) std::filesystem::rename(temp, path, ec);
}

std::string shader_cache_path(const std::string& src, bool vertex) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char b : src) { h ^= b; h *= 1099511628211ull; }
    char name[64];
    std::snprintf(name, sizeof name, "/%016llx_%zx.%s", (unsigned long long)h, src.size(), vertex ? "vs" : "fs");
    return cache_dir() + name;
}

size_t g_pcache_saved = 0;

void load_pipeline_cache() {
    std::vector<uint8_t> data = read_cache(cache_dir() + "/pipelines.bin");
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(g_dev, &ci, nullptr, &g_pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0;                 // the driver didn't like the old file: start fresh
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(g_dev, &ci, nullptr, &g_pcache);
    }
    hle_log("[gpu] pipeline cache: %zu bytes from disk", data.size());
}

void save_pipeline_cache() {
    if (!g_pcache || g_pipelines.size() == g_pcache_saved) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(g_dev, g_pcache, &size, nullptr) != VK_SUCCESS || !size) return;
    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(g_dev, g_pcache, &size, data.data()) != VK_SUCCESS) return;
    write_cache(cache_dir() + "/pipelines.bin", data.data(), size);
    g_pcache_saved = g_pipelines.size();
}
#endif

VkShaderModule compile(const std::string& src, bool vertex) {
    VkShaderModule m = VK_NULL_HANDLE;
#ifdef __ANDROID__
    const std::string cached = shader_cache_path(src, vertex);
    {
        std::vector<uint8_t> spv = read_cache(cached);
        if (spv.size() >= 4 && spv.size() % 4 == 0 && *(const uint32_t*)spv.data() == 0x07230203) {
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = spv.size();
            ci.pCode = (const uint32_t*)spv.data();
            if (vkCreateShaderModule(g_dev, &ci, nullptr, &m) == VK_SUCCESS) return m;
            m = VK_NULL_HANDLE;
        }
    }
#endif
    shaderc_compilation_result_t r =
        g_sc.compile_into_spv(g_sc.compiler, src.data(), src.size(),
                              vertex ? shaderc_glsl_vertex_shader : shaderc_glsl_fragment_shader,
                              vertex ? "nv2a.vert" : "nv2a.frag", "main", g_sc.options);
    if (g_sc.get_status(r) != shaderc_compilation_status_success) {
        hle_log("[gpu] shader compile failed: %s\n%s", g_sc.get_error(r), src.c_str());
    } else {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = g_sc.get_length(r);
        ci.pCode = (const uint32_t*)g_sc.get_bytes(r);
        vkCreateShaderModule(g_dev, &ci, nullptr, &m);
#ifdef __ANDROID__
        write_cache(cached, ci.pCode, ci.codeSize);
#endif
        if (log_enabled("shader")) hle_log("[gpu] %s shader:\n%s", vertex ? "vertex" : "fragment", src.c_str());
    }
    g_sc.release(r);
    return m;
}

// ---------------------------------------------------------------- resources

uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& b) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VKCHECK(vkCreateBuffer(g_dev, &ci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(g_dev, &ai, nullptr, &b.mem));
    VKCHECK(vkBindBufferMemory(g_dev, b.buf, b.mem, 0));
    void* p;
    VKCHECK(vkMapMemory(g_dev, b.mem, 0, size, 0, &p));
    b.ptr = (uint8_t*)p;
    b.size = size;
    return true;
}

bool make_image(uint32_t w, uint32_t h, uint32_t levels, bool cube, VkFormat format, VkImageUsageFlags usage,
                VkImageAspectFlags aspect, Image& img) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = cube ? 6 : 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(g_dev, &ci, nullptr, &img.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = mem_type(req.memoryTypeBits, 0);
    VKCHECK(vkAllocateMemory(g_dev, &ai, nullptr, &img.mem));
    VKCHECK(vkBindImageMemory(g_dev, img.image, img.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, levels, 0, cube ? 6u : 1u};
    VKCHECK(vkCreateImageView(g_dev, &vi, nullptr, &img.view));
    img.w = w;
    img.h = h;
    return true;
}

void destroy_image(Image& img) {
    if (img.view) vkDestroyImageView(g_dev, img.view, nullptr);
    if (img.image) vkDestroyImage(g_dev, img.image, nullptr);
    if (img.mem) vkFreeMemory(g_dev, img.mem, nullptr);
    img = Image{};
}

void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, uint32_t levels, uint32_t layers,
             VkImageLayout from, VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access,
             VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, levels, 0, layers};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Run a one-off command buffer synchronously (setup only).
template <class F>
void one_shot(F fn) {
    Frame& f = g_frames[0];
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = f.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(g_dev, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    fn(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_queue);
}

// ---------------------------------------------------------------- setup

bool create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps));
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        int w, h;
        window_client_size(&w, &h);
        ext = {(uint32_t)w, (uint32_t)h};
    }
    if (ext.width == 0 || ext.height == 0) {     // minimised
        g_sc_dirty = true;
        return true;
    }
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
#ifdef __ANDROID__
    // Handheld panels can be physically portrait: draw landscape and let the
    // system turn the picture, rather than rotating every frame here.
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    hle_log("[gpu] panel transform %x, drawing %ux%u", (unsigned)caps.currentTransform, ext.width, ext.height);
#endif
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &n, formats.data());
    VkSurfaceFormatKHR sf = formats[0];
    for (auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { sf = f; break; }
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &n, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!g_video.vsync) {
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
        if (mode == VK_PRESENT_MODE_FIFO_KHR)
            for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = transform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;      // Android drivers often offer only "inherit"
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha))
        ci.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -(int)caps.supportedCompositeAlpha);
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g_swapchain;
    VkSwapchainKHR sc;
    VKCHECK(vkCreateSwapchainKHR(g_dev, &ci, nullptr, &sc));
    if (g_swapchain) vkDestroySwapchainKHR(g_dev, g_swapchain, nullptr);
    for (VkSemaphore s : g_sc_done) vkDestroySemaphore(g_dev, s, nullptr);
    g_swapchain = sc;
    g_sc_format = sf.format;
    g_sc_extent = ext;
    vkGetSwapchainImagesKHR(g_dev, sc, &n, nullptr);
    g_sc_images.resize(n);
    vkGetSwapchainImagesKHR(g_dev, sc, &n, g_sc_images.data());
    g_sc_done.resize(n);
    for (auto& s : g_sc_done) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(g_dev, &si, nullptr, &s);
    }
    g_sc_dirty = false;
    hle_log("[gpu] swapchain %ux%u, %u images, present mode %d", ext.width, ext.height, n, (int)mode);
    return true;
}

bool create_target() {
    VkFormat candidates[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    for (VkFormat f : candidates) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_phys, f, &fp);
        if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
            (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            g_depth_format = f;
            break;
        }
    }
    VkAttachmentDescription att[2]{};
    att[0].format = kColorFormat;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1] = att[0];
    att[1].format = g_depth_format;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].initialLayout = att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dep.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 2;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    VKCHECK(vkCreateRenderPass(g_dev, &rp, nullptr, &g_rp));
    return true;
}

uint64_t surface_key(uint32_t addr, uint32_t nw, uint32_t nh, bool depth) {
    return (uint64_t)addr | ((uint64_t)(nw & 0x3fff) << 32) | ((uint64_t)(nh & 0x3fff) << 46) | ((uint64_t)depth << 60);
}

// The colour or depth image standing for a guest render target (created on first use).
Surface* get_surface(uint32_t addr, uint32_t nw, uint32_t nh, bool depth) {
    uint64_t key = surface_key(addr, nw, nh, depth);
    auto it = g_surfaces.find(key);
    if (it != g_surfaces.end()) return it->second;
    auto* sf = new Surface;
    sf->addr = addr;
    sf->nw = nw;
    sf->nh = nh;
    sf->depth = depth;
    sf->wide = g_video.wide > 1.0f && nw == kSurfaceW && nh == kSurfaceH;
    uint32_t w = nw * g_video.scale, h = nh * g_video.scale;
    if (sf->wide) w = ((uint32_t)(w * g_video.wide + 0.5f) + 1) & ~1u;
    if (depth) {
        if (!make_image(w, h, 1, false, g_depth_format,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                        sf->img))
            return nullptr;
        sf->sample_view = sf->img.view;      // depth aspect: for sampling
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = sf->img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g_depth_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCreateImageView(g_dev, &vi, nullptr, &sf->img.view);   // both aspects: as an attachment
    } else {
        if (!make_image(w, h, 1, false, kColorFormat,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, sf->img))
            return nullptr;
    }
    g_surfaces[key] = sf;
    if (log_enabled("gpu"))
        hle_log("[gpu] render target %08x %ux%u %s", addr, nw, nh, depth ? "depth" : "colour");
    return sf;
}

// A render target used as a texture: same memory, same size.
Surface* find_surface(uint32_t addr, uint32_t nw, uint32_t nh) {
    auto it = g_surfaces.find(surface_key(addr, nw, nh, false));
    if (it != g_surfaces.end()) return it->second;
    it = g_surfaces.find(surface_key(addr, nw, nh, true));
    return it != g_surfaces.end() ? it->second : nullptr;
}

// Layout change outside a render pass (conservative synchronisation).
void layout_to(VkCommandBuffer cb, Surface* sf, VkImageLayout to) {
    if (sf->layout == to) return;
    VkImageAspectFlags aspect = sf->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    barrier(cb, sf->img.image, aspect, 1, 1, sf->layout, to, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    sf->layout = to;
}

VkFramebuffer get_framebuffer(Surface* c, Surface* d) {
    uint64_t key = (uint64_t)(uintptr_t)c * 31 + (uint64_t)(uintptr_t)d;
    auto it = g_framebuffers.find(key);
    if (it != g_framebuffers.end()) return it->second;
    VkImageView views[2] = {c->img.view, d->img.view};
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = g_rp;
    fb.attachmentCount = 2;
    fb.pAttachments = views;
    fb.width = std::min(c->img.w, d->img.w);
    fb.height = std::min(c->img.h, d->img.h);
    fb.layers = 1;
    VkFramebuffer f = VK_NULL_HANDLE;
    vkCreateFramebuffer(g_dev, &fb, nullptr, &f);
    g_framebuffers[key] = f;
    return f;
}

bool create_layouts() {
    VkDescriptorSetLayoutBinding b[5]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    b[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    ci.bindingCount = 5;
    ci.pBindings = b;
    VKCHECK(vkCreateDescriptorSetLayout(g_dev, &ci, nullptr, &g_dsl));
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    VKCHECK(vkCreatePipelineLayout(g_dev, &pl, nullptr, &g_pl));
    return true;
}

VkDescriptorPool new_pool() {
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * 2048},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12 * 2048}};
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = 2048;
    ci.poolSizeCount = 2;
    ci.pPoolSizes = sizes;
    VkDescriptorPool p = VK_NULL_HANDLE;
    vkCreateDescriptorPool(g_dev, &ci, nullptr, &p);
    return p;
}

bool create_frames() {
    for (Frame& f : g_frames) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = g_qfam;
        VKCHECK(vkCreateCommandPool(g_dev, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, &f.cmd));
        VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, &f.upload));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKCHECK(vkCreateFence(g_dev, &fi, nullptr, &f.fence));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(g_dev, &si, nullptr, &f.acquired));
        if (!make_buffer(kRingSize,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         f.ring))
            return false;
        f.pools.push_back(new_pool());
    }
    return true;
}

bool create_dummies() {
    if (!make_image(1, 1, 1, false, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, g_dummy2d))
        return false;
    if (!make_image(1, 1, 1, true, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, g_dummycube))
        return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKCHECK(vkCreateSampler(g_dev, &si, nullptr, &g_dummy_sampler));
    if (!make_buffer(4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, g_zero)) return false;
    std::memset(g_zero.ptr, 0, 4096);
    one_shot([](VkCommandBuffer cb) {
        VkClearColorValue black{};
        black.float32[3] = 1.0f;
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (Image* im : {&g_dummy2d, &g_dummycube}) {
            uint32_t layers = im == &g_dummycube ? 6 : 1;
            r.layerCount = layers;
            barrier(cb, im->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, layers, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdClearColorImage(cb, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
            barrier(cb, im->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
    });
    return true;
}

bool init_vulkan(const NativeWindow& win) {
#ifdef _WIN32
    void* lib = load_lib("vulkan-1.dll");
#else
    void* lib = load_lib("libvulkan.so");
#endif
    if (!lib) { hle_log("[gpu] vulkan-1.dll not found"); return false; }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)lib_sym(lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) return false;
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "tje3";
    app.pEngineName = "tje3";
    app.apiVersion = VK_API_VERSION_1_1;
#ifdef __ANDROID__
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
#else
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
#endif
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    if (std::getenv("TJE_VK_VALIDATION")) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = layers;
    }
    VKCHECK(vkCreateInstance(&ci, nullptr, &g_inst));
#define TJE_VK_LOAD_I(f) f = (PFN_##f)vkGetInstanceProcAddr(g_inst, #f);
    TJE_VK_INSTANCE_FUNCS(TJE_VK_LOAD_I)
#undef TJE_VK_LOAD_I

#ifdef __ANDROID__
    VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    si.window = (ANativeWindow*)win.hwnd;
    VKCHECK(vkCreateAndroidSurfaceKHR(g_inst, &si, nullptr, &g_surface));
#else
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = (HINSTANCE)win.instance;
    si.hwnd = (HWND)win.hwnd;
    VKCHECK(vkCreateWin32SurfaceKHR(g_inst, &si, nullptr, &g_surface));
#endif

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g_inst, &n, devs.data());
    int best = -1;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, q, g_surface, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) {
                best = score;
                g_phys = d;
                g_qfam = q;
                g_props = p;
            }
            break;
        }
    }
    if (!g_phys) { hle_log("[gpu] no Vulkan device can present to the window"); return false; }
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memprops);
    hle_log("[gpu] %s (Vulkan %u.%u.%u)", g_props.deviceName, VK_API_VERSION_MAJOR(g_props.apiVersion),
            VK_API_VERSION_MINOR(g_props.apiVersion), VK_API_VERSION_PATCH(g_props.apiVersion));

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g_qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures have{}, want{};
    vkGetPhysicalDeviceFeatures(g_phys, &have);
    want.fillModeNonSolid = have.fillModeNonSolid;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = dexts;
    di.pEnabledFeatures = &want;
    VKCHECK(vkCreateDevice(g_phys, &di, nullptr, &g_dev));
#define TJE_VK_LOAD_D(f) f = (PFN_##f)vkGetDeviceProcAddr(g_dev, #f);
    TJE_VK_DEVICE_FUNCS(TJE_VK_LOAD_D)
#undef TJE_VK_LOAD_D
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
#ifdef __ANDROID__
    load_pipeline_cache();
#endif

    return create_swapchain() && create_target() && create_layouts() && create_frames() && create_dummies();
}

// ---------------------------------------------------------------- per frame

void begin_frame() {
    if (g_frame_open) return;
    Frame& f = g_frames[g_fi];
    vkWaitForFences(g_dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
    for (auto& fn : f.garbage) fn();
    f.garbage.clear();
    f.ring_off = 0;
    for (VkDescriptorPool p : f.pools) vkResetDescriptorPool(g_dev, p, 0);
    f.pool_index = 0;
    vkResetCommandBuffer(f.cmd, 0);
    vkResetCommandBuffer(f.upload, 0);
    f.upload_open = false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    g_frame_open = true;
    g_in_pass = false;
}

VkCommandBuffer upload_cmd() {
    Frame& f = g_frames[g_fi];
    if (!f.upload_open) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.upload, &bi);
        f.upload_open = true;
    }
    return f.upload;
}

void begin_pass() {
    begin_frame();
    if (g_in_pass) return;
    Frame& f = g_frames[g_fi];
    layout_to(f.cmd, g_cur_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    Surface* depth = g_pass_depth ? g_pass_depth : g_cur_depth;
    layout_to(f.cmd, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g_rp;
    rb.framebuffer = get_framebuffer(g_cur_color, depth);
    rb.renderArea = {{0, 0}, {g_rt_w, g_rt_h}};
    vkCmdBeginRenderPass(f.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)g_rt_w, (float)g_rt_h, 0.0f, 1.0f};
    vkCmdSetViewport(f.cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {g_rt_w, g_rt_h}};
    vkCmdSetScissor(f.cmd, 0, 1, &sc);
    g_in_pass = true;
    if (depth->needs_clear) {                    // a new shadow layer starts empty (far)
        depth->needs_clear = false;
        VkClearAttachment a{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, {}};
        a.clearValue.depthStencil = {1.0f, 0};
        VkClearRect r{{{0, 0}, {g_rt_w, g_rt_h}}, 0, 1};
        vkCmdClearAttachments(f.cmd, 1, &a, 1, &r);
    }
}

void end_pass();

// The second (animated casters) layer of a shadow map.
Surface* shadow_layer(Surface* s) {
    if (!s->alt) {
        s->alt = get_surface(s->addr ^ 0x40000000u, s->nw, s->nh, true);
        if (s->alt) s->alt->needs_clear = true;
    }
    return s->alt;
}

void use_depth_layer(Surface* want) {
    if (want == (g_pass_depth ? g_pass_depth : g_cur_depth)) return;
    end_pass();
    g_pass_depth = want;
}

void end_pass() {
    if (!g_in_pass) return;
    vkCmdEndRenderPass(g_frames[g_fi].cmd);
    g_in_pass = false;
}

// Space in this frame's ring; nullptr when full.
uint8_t* ring_alloc(VkDeviceSize size, VkDeviceSize align, VkDeviceSize* offset) {
    Frame& f = g_frames[g_fi];
    VkDeviceSize o = (f.ring_off + align - 1) & ~(align - 1);
    if (o + size > f.ring.size) {
        static bool warned = false;
        if (!warned) hle_log("[gpu] upload ring full (%llu bytes wanted)", (unsigned long long)size);
        warned = true;
        return nullptr;
    }
    f.ring_off = o + size;
    *offset = o;
    return f.ring.ptr + o;
}

// ---------------------------------------------------------------- textures and samplers

VkSampler get_sampler(const Sampler& s, uint32_t levels) {
    uint64_t key = hash_bytes(&s, sizeof s, levels);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    auto addr = [](uint8_t a) {
        switch (a) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    };
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = s.mag >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = s.min >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = s.mip >= 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = addr(s.address_u);
    ci.addressModeV = addr(s.address_v);
    ci.addressModeW = addr(s.address_w);
    ci.borderColor = (s.border >> 24) >= 0x80 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if ((s.border & 0xffffff) == 0xffffff && (s.border >> 24) >= 0x80) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    ci.minLod = 0;
    ci.maxLod = s.mip == 0 ? 0.0f : (float)levels;
    VkSampler smp = VK_NULL_HANDLE;
    vkCreateSampler(g_dev, &ci, nullptr, &smp);
    g_samplers[key] = smp;
    return smp;
}

// The texture's image, uploaded from guest memory when its bytes changed.
Tex* get_texture(const TextureRef& t) {
    if (!t.addr || !t.width || !t.height) return nullptr;
    uint32_t key_words[8] = {t.addr, t.format, t.width, t.height, t.levels, t.pitch, t.cube, t.palette};
    uint64_t key = hash_bytes(key_words, sizeof key_words);
    Tex& e = g_textures[key];
    if (e.checked == g_epoch && e.img.image) {
        e.used = g_serial;
        return &e;
    }
    e.checked = g_epoch;
    uint32_t bytes = texture_size(t);
    const uint8_t* src = (const uint8_t*)gptr(t.addr);
    uint64_t h = hash_bytes(src, bytes);
    const uint8_t* pal = nullptr;
    if (t.format == 0x0b && t.palette) {
        pal = (const uint8_t*)gptr(t.palette);
        h = hash_bytes(pal, t.palette_entries * 4, h);
    }
    if (e.img.image && h == e.hash) {
        e.used = g_serial;
        return &e;
    }
    // (Re)upload. An image already used by this frame's commands is retired and replaced.
    Frame& f = g_frames[g_fi];
    if (e.img.image) {
        Image old = e.img;
        f.garbage.push_back([old]() mutable { destroy_image(old); });
        e.img = Image{};
    }
    std::vector<uint8_t> rgba;
    std::vector<DecodedLevel> levels;
    if (!decode_texture(t, src, pal, rgba, levels)) {
        static uint32_t warned[256];
        if (!warned[t.format & 0xff]++) hle_log("[gpu] texture format %02x not supported", t.format);
    }
    if (!make_image(t.width, t.height, t.levels, t.cube, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, e.img))
        return nullptr;
    e.cube = t.cube;
    e.levels = t.levels;
    e.hash = h;
    e.used = g_serial;
    VkDeviceSize off;
    uint8_t* dst = ring_alloc(rgba.size(), 16, &off);
    VkCommandBuffer cb = upload_cmd();
    uint32_t layers = t.cube ? 6 : 1;
    barrier(cb, e.img.image, VK_IMAGE_ASPECT_COLOR_BIT, t.levels, layers, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (dst) {
        std::memcpy(dst, rgba.data(), rgba.size());
        std::vector<VkBufferImageCopy> regions;
        for (const DecodedLevel& lv : levels) {
            VkBufferImageCopy r{};
            r.bufferOffset = off + lv.offset;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv.level, lv.face, 1};
            r.imageExtent = {lv.width, lv.height, 1};
            regions.push_back(r);
        }
        vkCmdCopyBufferToImage(cb, g_frames[g_fi].ring.buf, e.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               (uint32_t)regions.size(), regions.data());
    }
    barrier(cb, e.img.image, VK_IMAGE_ASPECT_COLOR_BIT, t.levels, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    g_stat_uploads++;
    if (log_enabled("tex"))
        hle_log("[gpu] upload texture %08x %ux%u format %02x levels %u%s", t.addr, t.width, t.height, t.format,
                t.levels, t.cube ? " cube" : "");
    return &e;
}

// ---------------------------------------------------------------- vertices

struct AttrFormat { VkFormat vk; uint32_t size; bool direct; };

AttrFormat attr_format(uint8_t f) {
    uint32_t n = (f >> 4) & 15, type = f & 15;
    switch (type) {
    case 2:
        if (f == 0x72) return {VK_FORMAT_R32G32B32A32_SFLOAT, 12, false};   // FLOAT2H
        if (n == 1) return {VK_FORMAT_R32_SFLOAT, 4, true};
        if (n == 2) return {VK_FORMAT_R32G32_SFLOAT, 8, true};
        if (n == 3) return {VK_FORMAT_R32G32B32_SFLOAT, 12, true};
        if (n == 4) return {VK_FORMAT_R32G32B32A32_SFLOAT, 16, true};
        break;
    case 0: if (f == 0x40) return {VK_FORMAT_B8G8R8A8_UNORM, 4, true}; break;
    case 1: case 5: return {VK_FORMAT_R32G32B32A32_SFLOAT, 2 * n, false};   // NORMSHORT / SHORT
    case 4: return {VK_FORMAT_R32G32B32A32_SFLOAT, n, false};                // PBYTE
    case 6: return {VK_FORMAT_R32G32B32A32_SFLOAT, 4, false};                // NORMPACKED3
    }
    return {VK_FORMAT_UNDEFINED, 0, false};
}

void convert_attr(uint8_t f, const uint8_t* s, float* o) {
    uint32_t n = (f >> 4) & 15, type = f & 15;
    o[0] = o[1] = o[2] = 0.0f;
    o[3] = 1.0f;
    if (f == 0x72) {
        float v[3];
        std::memcpy(v, s, 12);
        o[0] = v[0]; o[1] = v[1]; o[3] = v[2];
        return;
    }
    for (uint32_t i = 0; i < n && i < 4; i++) {
        if (type == 1 || type == 5) {
            int16_t v;
            std::memcpy(&v, s + 2 * i, 2);
            o[i] = type == 1 ? std::max(-1.0f, v / 32767.0f) : (float)v;
        } else if (type == 4) {
            o[i] = s[i] / 255.0f;
        }
    }
    if (type == 6) {
        uint32_t v;
        std::memcpy(&v, s, 4);
        int32_t x = (int32_t)(v << 21) >> 21, y = (int32_t)(v << 10) >> 21, z = (int32_t)v >> 22;
        o[0] = std::max(-1.0f, x / 1023.0f);
        o[1] = std::max(-1.0f, y / 1023.0f);
        o[2] = std::max(-1.0f, z / 511.0f);
    }
}

// ---------------------------------------------------------------- pipelines

VkCompareOp compare_op(uint32_t gl) { return (VkCompareOp)std::min<uint32_t>(gl >= 0x200 ? gl - 0x200 : 7, 7); }

VkBlendFactor blend_factor(uint32_t gl) {
    switch (gl) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp blend_op(uint32_t gl) {
    switch (gl) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800a: return VK_BLEND_OP_SUBTRACT;
    case 0x800b: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

VkStencilOp stencil_op(uint32_t gl) {
    switch (gl) {
    case 0: return VK_STENCIL_OP_ZERO;
    case 0x1e01: return VK_STENCIL_OP_REPLACE;
    case 0x1e02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1e03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150a: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

struct PipeKey {
    uint64_t vs, fs;
    uint32_t topology;
    uint32_t attr_format[16];
    uint32_t stride[16];
    RasterState rs;
};

VkPipeline get_pipeline(const PipeKey& k, VkShaderModule vs, VkShaderModule fs) {
    uint64_t key = hash_bytes(&k, sizeof k);
    auto it = g_pipelines.find(key);
    if (it != g_pipelines.end()) return it->second;

    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";

    VkVertexInputBindingDescription bind[16];
    VkVertexInputAttributeDescription attr[16];
    for (uint32_t i = 0; i < 16; i++) {
        bind[i] = {i, k.stride[i], VK_VERTEX_INPUT_RATE_VERTEX};
        attr[i] = {i, i, (VkFormat)k.attr_format[i], 0};
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 16;
    vi.pVertexBindingDescriptions = bind;
    vi.vertexAttributeDescriptionCount = 16;
    vi.pVertexAttributeDescriptions = attr;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = (VkPrimitiveTopology)k.topology;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    const RasterState& rs = k.rs;
    VkPipelineRasterizationStateCreateInfo rz{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rz.polygonMode = VK_POLYGON_MODE_FILL;
    rz.cullMode = !rs.cull_enable ? VK_CULL_MODE_NONE
                  : rs.cull_face == 0x404 ? VK_CULL_MODE_FRONT_BIT
                  : rs.cull_face == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK
                                          : VK_CULL_MODE_BACK_BIT;
    rz.frontFace = rs.front_face == 0x901 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rz.depthBiasEnable = rs.poly_offset ? VK_TRUE : VK_FALSE;
    rz.depthBiasSlopeFactor = rs.poly_factor;
    rz.depthBiasConstantFactor = rs.poly_units;
    rz.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = rs.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (rs.depth_test && rs.depth_write) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = compare_op(rs.depth_func);
    ds.stencilTestEnable = rs.stencil_test ? VK_TRUE : VK_FALSE;
    ds.front.failOp = stencil_op(rs.stencil_fail);
    ds.front.passOp = stencil_op(rs.stencil_zpass);
    ds.front.depthFailOp = stencil_op(rs.stencil_zfail);
    ds.front.compareOp = compare_op(rs.stencil_func);
    ds.front.compareMask = rs.stencil_read_mask;
    ds.front.writeMask = rs.stencil_write_mask;
    ds.front.reference = rs.stencil_ref;
    ds.back = ds.front;
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = rs.blend_enable ? VK_TRUE : VK_FALSE;
    cb.srcColorBlendFactor = cb.srcAlphaBlendFactor = blend_factor(rs.blend_src);
    cb.dstColorBlendFactor = cb.dstAlphaBlendFactor = blend_factor(rs.blend_dst);
    cb.colorBlendOp = cb.alphaBlendOp = blend_op(rs.blend_eq);
    // The back buffer is X8R8G8B8: alpha is never written and reads as 1.
    cb.colorWriteMask = ((rs.color_mask >> 16) & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) |
                        ((rs.color_mask >> 8) & 1 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                        (rs.color_mask & 1 ? VK_COLOR_COMPONENT_B_BIT : 0);
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 3;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vps;
    pi.pRasterizationState = &rz;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cbs;
    pi.pDynamicState = &dy;
    pi.layout = g_pl;
    pi.renderPass = g_rp;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &pi, nullptr, &p);
    if (r != VK_SUCCESS) hle_log("[gpu] pipeline creation failed: %d", (int)r);
    g_pipelines[key] = p;
    if (log_enabled("gpu")) hle_log("[gpu] new pipeline %016llx (%zu total)", (unsigned long long)key, g_pipelines.size());
    return p;
}

VkDescriptorSet alloc_set() {
    Frame& f = g_frames[g_fi];
    for (;;) {
        if (f.pool_index >= f.pools.size()) f.pools.push_back(new_pool());
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.pools[f.pool_index];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_dsl;
        VkDescriptorSet s;
        if (vkAllocateDescriptorSets(g_dev, &ai, &s) == VK_SUCCESS) return s;
        f.pool_index++;
    }
}

void rgba_from_d3dcolor(uint32_t c, float* o) {
    o[0] = ((c >> 16) & 0xff) / 255.0f;
    o[1] = ((c >> 8) & 0xff) / 255.0f;
    o[2] = (c & 0xff) / 255.0f;
    o[3] = (c >> 24) / 255.0f;
}

uint64_t fs_key(const Draw& d, uint32_t kinds) {
    if (d.fixed_function_ps) {
        uint32_t k[4][11];
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 10; j++) k[i][j] = d.tss[i][12 + j];
            k[i][10] = d.tss[i][28];
        }
        return hash_bytes(k, sizeof k, 0x1000 + kinds);
    }
    uint32_t p[60];
    std::memcpy(p, d.psdef, sizeof p);
    for (int i = 0x28 / 4; i < 0x68 / 4; i++) p[i] = 0;   // constants live in the uniform buffer
    p[0xac / 4] = p[0xb0 / 4] = 0;
    return hash_bytes(p, sizeof p, 0x2000 + kinds);
}

}  // namespace

// ---------------------------------------------------------------- interface

bool init(const Options& o) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_opt = o;
    if (g_opt.scale < 1) g_opt.scale = 1;
    g_video.scale = g_opt.scale;
    g_video.vsync = g_opt.vsync || setting_bool("vsync");
    NativeWindow win;
    if (!window_create(o.window_width, o.window_height, o.fullscreen, "ToeJam & Earl III", &win)) {
        hle_log("[gpu] no window: running headless");
        return false;
    }
    if (!load_shaderc() || !init_vulkan(win)) {
        hle_log("[gpu] Vulkan unavailable: running headless");
        return false;
    }
    if (const char* e = std::getenv("TJE_GPU_STATS")) {
        g_stats_every = (uint32_t)std::atoi(e);
        if (g_stats_every && !make_buffer((VkDeviceSize)kSurfaceW * kSurfaceH * g_opt.scale * g_opt.scale * 4,
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT, g_readback))
            g_stats_every = 0;
    }
    g_active = true;
    update_video();
    return true;
}

bool active() { return g_active; }

#ifdef __ANDROID__
// The system takes the window away when the player leaves the app, and the
// surface must be gone before it does. Everything else stays alive for the return.
void android_surface_lost() {
    if (!g_active) return;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (!g_surface) return;
    vkDeviceWaitIdle(g_dev);
    save_pipeline_cache();
    if (g_swapchain) vkDestroySwapchainKHR(g_dev, g_swapchain, nullptr);
    g_swapchain = VK_NULL_HANDLE;
    vkDestroySurfaceKHR(g_inst, g_surface, nullptr);
    g_surface = VK_NULL_HANDLE;
    hle_log("[gpu] surface released");
}

// A bar across the middle of the screen, while the game unpacks on first launch.
void android_show_progress(float fraction) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (!g_surface) return;
    if (window_take_resized()) g_sc_dirty = true;
    if (g_sc_dirty || !g_swapchain) {
        vkDeviceWaitIdle(g_dev);
        create_swapchain();
        if (!g_swapchain || g_sc_dirty) return;
    }
    constexpr uint32_t kW = 1000, kH = 24, kBytes = kW * kH * 4;
    static Buffer bar;
    static VkFence fence = VK_NULL_HANDLE;
    if (!bar.buf) {
        if (!make_buffer(kBytes * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, bar)) return;
        std::memset(bar.ptr, 0xff, kBytes);             // the filled part
        std::memset(bar.ptr + kBytes, 0x40, kBytes);    // the empty part
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(g_dev, &fi, nullptr, &fence);
    }
    uint32_t image = 0;
    vkResetFences(g_dev, 1, &fence);
    if (vkAcquireNextImageKHR(g_dev, g_swapchain, UINT64_MAX, VK_NULL_HANDLE, fence, &image) != VK_SUCCESS) {
        g_sc_dirty = true;
        return;
    }
    vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX);
    const VkImage target = g_sc_images[image];
    const uint32_t filled = (uint32_t)(std::min(std::max(fraction, 0.0f), 1.0f) * kW);
    const int32_t x = (int32_t)(g_sc_extent.width - kW) / 2, y = (int32_t)(g_sc_extent.height - kH) / 2;
    one_shot([&](VkCommandBuffer cb) {
        barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        auto copy = [&](VkDeviceSize offset, int32_t left, uint32_t width) {
            if (!width) return;
            VkBufferImageCopy r{};
            r.bufferOffset = offset;
            r.bufferRowLength = kW;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageOffset = {left, y, 0};
            r.imageExtent = {width, kH, 1};
            vkCmdCopyBufferToImage(cb, bar.buf, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        };
        copy(0, x, filled);
        copy(kBytes, x + (int32_t)filled, kW - filled);
        barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    });
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &image;
    if (vkQueuePresentKHR(g_queue, &pi) == VK_ERROR_OUT_OF_DATE_KHR) g_sc_dirty = true;
}

// The size a picture for android_show_pixels must have; false while there is no screen to draw on.
bool android_screen_size(uint32_t* w, uint32_t* h) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (!g_surface) return false;
    if (window_take_resized()) g_sc_dirty = true;
    if (g_sc_dirty || !g_swapchain) {
        vkDeviceWaitIdle(g_dev);
        create_swapchain();
        if (!g_swapchain || g_sc_dirty) return false;
    }
    *w = g_sc_extent.width;
    *h = g_sc_extent.height;
    return true;
}

// Shows a full-screen picture (bytes R, G, B, A), for the first-launch setup screens.
bool android_show_pixels(const uint8_t* rgba, uint32_t w, uint32_t h) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    uint32_t sw, sh;
    if (!android_screen_size(&sw, &sh) || sw != w || sh != h) return false;
    static Buffer pic;
    static uint32_t pic_bytes = 0;
    static VkFence fence = VK_NULL_HANDLE;
    const uint32_t bytes = w * h * 4;
    if (pic_bytes != bytes) {
        vkDeviceWaitIdle(g_dev);
        if (pic.buf) {
            vkDestroyBuffer(g_dev, pic.buf, nullptr);
            vkFreeMemory(g_dev, pic.mem, nullptr);
        }
        pic = Buffer{};
        pic_bytes = 0;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pic)) return false;
        pic_bytes = bytes;
    }
    if (!fence) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(g_dev, &fi, nullptr, &fence);
    }
    if (g_sc_format == VK_FORMAT_B8G8R8A8_UNORM) {
        uint8_t* o = (uint8_t*)pic.ptr;
        for (uint32_t i = 0; i < bytes; i += 4) {
            o[i] = rgba[i + 2]; o[i + 1] = rgba[i + 1]; o[i + 2] = rgba[i]; o[i + 3] = 0xff;
        }
    } else {
        std::memcpy(pic.ptr, rgba, bytes);
    }
    uint32_t image = 0;
    vkResetFences(g_dev, 1, &fence);
    if (vkAcquireNextImageKHR(g_dev, g_swapchain, UINT64_MAX, VK_NULL_HANDLE, fence, &image) != VK_SUCCESS) {
        g_sc_dirty = true;
        return false;
    }
    vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX);
    const VkImage target = g_sc_images[image];
    one_shot([&](VkCommandBuffer cb) {
        barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cb, pic.buf, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    });
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &image;
    if (vkQueuePresentKHR(g_queue, &pi) == VK_ERROR_OUT_OF_DATE_KHR) g_sc_dirty = true;
    return true;
}

void android_surface_ready(void* window) {
    if (!g_active) return;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_surface) return;
    VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    si.window = (ANativeWindow*)window;
    if (vkCreateAndroidSurfaceKHR(g_inst, &si, nullptr, &g_surface) != VK_SUCCESS) {
        hle_log("[gpu] could not recreate the surface");
        g_surface = VK_NULL_HANDLE;
        return;
    }
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_phys, g_qfam, g_surface, &present);
    hle_log("[gpu] surface restored");
    g_sc_dirty = true;
}
#endif

void textures_touched() { g_epoch++; }

void set_title(const std::string& t) { window_set_title(t); }

void set_render_target(uint32_t color, uint32_t cw, uint32_t ch, uint32_t depth, uint32_t dw, uint32_t dh, bool back) {
    if (!g_active) return;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    Surface* c = get_surface(color, cw, ch, false);
    // No depth buffer, or a smaller one than the colour target: a private one of the right size.
    Surface* d = (depth && dw >= cw && dh >= ch) ? get_surface(depth, dw, dh, true) : get_surface(0, cw, ch, true);
    if (!c || !d) return;
    g_last_target = {color, cw, ch, depth, dw, dh, true};
    if (back) { g_back = c; g_last_back = g_last_target; }
    if (c == g_cur_color && d == g_cur_depth) return;
    end_pass();
    g_cur_color = c;
    g_cur_depth = d;
    g_pass_depth = d;
    g_rt_w = c->img.w;
    g_rt_h = c->img.h;
}

float wide_factor() { return g_active ? g_video.wide : 1.0f; }

void clear(uint32_t flags, uint32_t color, float z, uint32_t stencil, const int32_t* rects, uint32_t nrects) {
    if (!g_active || !g_cur_color) return;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    use_depth_layer(g_cur_depth);
    begin_pass();
    VkClearAttachment att[2];
    uint32_t n = 0;
    if (flags & 0xf0) {
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[n].colorAttachment = 0;
        float c[4];
        rgba_from_d3dcolor(color, c);
        att[n].clearValue.color = {{c[0], c[1], c[2], 1.0f}};
        n++;
    }
    if (flags & 3) {
        att[n].aspectMask = ((flags & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((flags & 2) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        att[n].colorAttachment = 0;
        att[n].clearValue.depthStencil = {std::min(std::max(z, 0.0f), 1.0f), stencil & 0xff};
        n++;
    }
    if (!n) return;
    std::vector<VkClearRect> r;
    const float sx = (float)g_rt_w / g_cur_color->nw, sy = (float)g_rt_h / g_cur_color->nh;
    if (!nrects) r.push_back({{{0, 0}, {g_rt_w, g_rt_h}}, 0, 1});
    for (uint32_t i = 0; i < nrects; i++) {
        int32_t x1 = std::max(0, rects[i * 4]), y1 = std::max(0, rects[i * 4 + 1]);
        int32_t x2 = std::min<int32_t>(g_cur_color->nw, rects[i * 4 + 2]), y2 = std::min<int32_t>(g_cur_color->nh, rects[i * 4 + 3]);
        if (x2 <= x1 || y2 <= y1) continue;
        r.push_back({{{(int32_t)(x1 * sx), (int32_t)(y1 * sy)},
                      {(uint32_t)((x2 - x1) * sx), (uint32_t)((y2 - y1) * sy)}}, 0, 1});
    }
    if (!r.empty()) vkCmdClearAttachments(g_frames[g_fi].cmd, n, att, (uint32_t)r.size(), r.data());
    if ((flags & 3) && g_cur_depth->alt && !r.empty()) {        // the shadow layer clears with it
        use_depth_layer(g_cur_depth->alt);
        begin_pass();
        VkClearAttachment da = att[n - 1];
        vkCmdClearAttachments(g_frames[g_fi].cmd, 1, &da, (uint32_t)r.size(), r.data());
        use_depth_layer(g_cur_depth);
    }
}

void draw(const Draw& d) {
    if (!g_active || !g_cur_color || !d.count || !d.program || !d.program_len) return;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    CpuTimer timer;
    begin_frame();

    // Vertex sequence: which source vertices, in which order, for which Vulkan topology.
    std::vector<uint32_t> seq;
    bool indexed = d.index_addr != 0;
    uint32_t vmin = 0, vmax = d.count - 1;
    if (indexed) {
        const uint16_t* idx = (const uint16_t*)gptr(d.index_addr);
        seq.assign(idx, idx + d.count);
        vmin = *std::min_element(seq.begin(), seq.end());
        vmax = *std::max_element(seq.begin(), seq.end());
    }
    VkPrimitiveTopology topo;
    bool need_indices = indexed;
    auto ensure_seq = [&] {
        if (seq.empty()) for (uint32_t i = 0; i < d.count; i++) seq.push_back(i);
        need_indices = true;
    };
    switch (d.prim) {
    case 1: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; ensure_seq(); seq.push_back(seq[0]); break;
    case 4: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 5: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 6: case 9: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case 7: case 10: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 8: {
        topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        ensure_seq();
        std::vector<uint32_t> tris;
        for (size_t q = 0; q + 3 < seq.size(); q += 4) {
            const uint32_t* v = &seq[q];
            uint32_t t[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
            tris.insert(tris.end(), t, t + 6);
        }
        seq.swap(tris);
        break;
    }
    default: return;
    }
    if (seq.empty() && need_indices) return;
    uint32_t nverts = vmax - vmin + 1;

    // Vertex buffers: one binding per attribute.
    Frame& f = g_frames[g_fi];
    PipeKey key;
    std::memset(&key, 0, sizeof key);
    VkBuffer bufs[16];
    VkDeviceSize offs[16];
    uint32_t mask = 0;
    VkDeviceSize stream_off[16];
    bool stream_done[16] = {};
    for (uint32_t i = 0; i < 16; i++) {
        const VertexAttr& a = d.attrs[i];
        AttrFormat af = attr_format(a.format);
        bufs[i] = g_zero.buf;
        offs[i] = 0;
        key.attr_format[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
        key.stride[i] = 0;
        if (a.format == 0x02 || af.vk == VK_FORMAT_UNDEFINED) continue;
        const Stream& s = d.streams[a.stream & 15];
        const uint8_t* base = d.up_data ? d.up_data : (s.addr ? (const uint8_t*)gptr(s.addr) : nullptr);
        if (!base) continue;
        uint32_t stride = s.stride;
        if (af.direct) {
            if (!stream_done[a.stream & 15]) {
                uint32_t bytes = stride ? stride * nverts : 64;
                uint8_t* dst = ring_alloc(bytes, 16, &stream_off[a.stream & 15]);
                if (!dst) return;
                std::memcpy(dst, base + (size_t)vmin * stride, bytes);
                stream_done[a.stream & 15] = true;
            }
            bufs[i] = f.ring.buf;
            offs[i] = stream_off[a.stream & 15] + a.offset;
            key.attr_format[i] = af.vk;
            key.stride[i] = stride;
        } else {
            VkDeviceSize off;
            float* dst = (float*)ring_alloc((VkDeviceSize)nverts * 16, 16, &off);
            if (!dst) return;
            for (uint32_t v = 0; v < nverts; v++)
                convert_attr(a.format, base + (size_t)(vmin + v) * stride + a.offset, dst + v * 4);
            bufs[i] = f.ring.buf;
            offs[i] = off;
            key.stride[i] = 16;
        }
        mask |= 1u << i;
    }

    // Shaders.
    // Shadow layers: casters drawn with relative constant addressing (bone palettes) go to
    // the second layer of a shadow map while the enhancement is on.
    {
        static std::unordered_map<uint64_t, bool> relative;
        uint64_t pk = hash_bytes(d.program, d.program_len * 16, 0x3000);
        auto it = relative.find(pk);
        if (it == relative.end()) {
            bool rel = false;
            for (uint32_t k = 0; k < d.program_len; k++) rel |= (d.program[k * 4 + 3] >> 1) & 1;   // A0X
            it = relative.emplace(pk, rel).first;
        }
        Surface* want = g_cur_depth;
        if (g_shadow_enhance > 0 && g_cur_depth->shadow && it->second) want = shadow_layer(g_cur_depth);
        if (want) use_depth_layer(want);
    }
    uint32_t kinds = 0;
    Tex* tex[4] = {};
    Surface* rtex[4] = {};
    for (int i = 0; i < 4; i++) {
        if (d.tex[i].addr && !d.tex[i].cube) rtex[i] = find_surface(d.tex[i].addr, d.tex[i].width, d.tex[i].height);
        if (rtex[i] == g_cur_color || rtex[i] == g_cur_depth) rtex[i] = nullptr;   // feedback: not sampled
        if (rtex[i]) {
            if (rtex[i]->depth) rtex[i]->shadow = true;
            if (rtex[i]->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                end_pass();
                layout_to(f.cmd, rtex[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            Surface* layer = rtex[i]->depth && g_shadow_enhance > 0 ? rtex[i]->alt : nullptr;
            if (layer && layer->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                end_pass();
                layout_to(f.cmd, layer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            kinds |= (rtex[i]->depth ? 3u : 1u) << (2 * i);
            continue;
        }
        tex[i] = get_texture(d.tex[i]);
        if (tex[i]) kinds |= (tex[i]->cube ? 2u : 1u) << (2 * i);
    }
    uint64_t vkey = hash_bytes(d.program, d.program_len * 16, 0x3000);
    VkShaderModule& vs = g_vs_cache[vkey];
    if (!vs) vs = compile(vertex_glsl(d.program, d.program_len), true);
    uint64_t fkey = fs_key(d, kinds);
    VkShaderModule& fs = g_fs_cache[fkey];
    if (!fs) fs = compile(fragment_glsl(d, kinds), false);
    if (!vs || !fs) return;

    key.vs = vkey;
    key.fs = fkey;
    key.topology = topo;
    key.rs = d.rs;
    VkPipeline pipe = get_pipeline(key, vs, fs);
    if (!pipe) return;

    // Uniforms.
    VkDeviceSize align = std::max<VkDeviceSize>(g_props.limits.minUniformBufferOffsetAlignment, 16);
    VkDeviceSize vs_off, ps_off;
    uint8_t* vu = ring_alloc(192 * 16 + 16 * 16 + 16, align, &vs_off);
    uint8_t* pu = ring_alloc(27 * 16, align, &ps_off);
    if (!vu || !pu) return;
    std::memcpy(vu, d.constants, 192 * 16);
    std::memcpy(vu + 192 * 16, d.attr_default, 16 * 16);
    std::memcpy(vu + 208 * 16, &mask, 4);
    float* p = (float*)pu;
    std::memset(p, 0, 27 * 16);
    if (!d.fixed_function_ps) {
        for (int i = 0; i < 8; i++) {
            rgba_from_d3dcolor(d.psdef[0x28 / 4 + i], p + i * 4);
            rgba_from_d3dcolor(d.psdef[0x48 / 4 + i], p + 32 + i * 4);
        }
        rgba_from_d3dcolor(d.psdef[0xac / 4], p + 64);
        rgba_from_d3dcolor(d.psdef[0xb0 / 4], p + 68);
    }
    rgba_from_d3dcolor(d.texture_factor, p + 72);
    rgba_from_d3dcolor(d.fog_color, p + 76);
    for (int i = 0; i < 4; i++) {
        bool linear = (tex[i] || rtex[i]) && format_info(d.tex[i].format).linear;
        p[80 + i * 4] = linear ? 1.0f / d.tex[i].width : 1.0f;
        p[81 + i * 4] = linear ? 1.0f / d.tex[i].height : 1.0f;
        uint32_t f = d.tex[i].format;   // depth formats: the range of the shadow compare value
        p[82 + i * 4] = (f == 0x2c || f == 0x2d || f == 0x30 || f == 0x31) ? 65535.0f : 16777215.0f;
        p[83 + i * 4] = (rtex[i] && rtex[i]->depth && rtex[i]->alt && g_shadow_enhance > 0) ? 1.0f : 0.0f;
    }
    p[99] = (float)(d.shadow_func >= 0x200 ? d.shadow_func - 0x200 : 0);
    p[100] = d.fog_enable ? 1.0f : 0.0f;
    p[101] = (float)d.fog_mode;
    p[102] = d.fog_start;
    p[103] = d.fog_end;
    p[104] = d.fog_density;
    p[105] = g_shadow_enhance;
    static const bool shadow_debug = std::getenv("TJE_SHADOW_DEBUG") != nullptr;
    p[106] = shadow_debug ? 1.0f : 0.0f;
    p[96] = d.alpha_test ? 1.0f : 0.0f;
    p[97] = (float)(d.alpha_func >= 0x200 ? d.alpha_func - 0x200 : 7);
    p[98] = (float)(d.alpha_ref & 0xff);

    // Descriptors.
    VkDescriptorSet set = alloc_set();
    VkDescriptorBufferInfo ub[2] = {{f.ring.buf, 0, 192 * 16 + 16 * 16 + 16}, {f.ring.buf, 0, 27 * 16}};
    VkDescriptorImageInfo img2d[4], imgcube[4], imglayer[4];
    for (int i = 0; i < 4; i++) {
        img2d[i] = {g_dummy_sampler, g_dummy2d.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        imglayer[i] = img2d[i];
        if (rtex[i] && rtex[i]->depth && rtex[i]->alt && g_shadow_enhance > 0)
            imglayer[i] = {g_dummy_sampler, rtex[i]->alt->sample_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        imgcube[i] = {g_dummy_sampler, g_dummycube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (rtex[i]) {
            Sampler sm = d.samp[i];
            if (rtex[i]->depth) sm.mag = sm.min = 1;    // depth values are compared, not filtered
            sm.mip = 0;
            img2d[i] = {get_sampler(sm, 1), rtex[i]->depth ? rtex[i]->sample_view : rtex[i]->img.view,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            continue;
        }
        if (!tex[i]) continue;
        VkDescriptorImageInfo info{get_sampler(d.samp[i], tex[i]->levels), tex[i]->img.view,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        (tex[i]->cube ? imgcube[i] : img2d[i]) = info;
    }
    VkWriteDescriptorSet w[5]{};
    for (int i = 0; i < 5; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = i;
    }
    w[0].descriptorCount = w[1].descriptorCount = 1;
    w[0].descriptorType = w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w[0].pBufferInfo = &ub[0];
    w[1].pBufferInfo = &ub[1];
    w[2].descriptorCount = w[3].descriptorCount = 4;
    w[2].descriptorType = w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[2].pImageInfo = img2d;
    w[3].pImageInfo = imgcube;
    w[4].descriptorCount = 4;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[4].pImageInfo = imglayer;
    vkUpdateDescriptorSets(g_dev, 5, w, 0, nullptr);

    // Record.
    begin_pass();
    VkCommandBuffer cb = f.cmd;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    uint32_t dyn[2] = {(uint32_t)vs_off, (uint32_t)ps_off};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 0, 1, &set, 2, dyn);
    // 2D in widescreen keeps its shape. Only plain colour (a fade) may reach out to the
    // screen's sides; a picture never does, since that would stretch it. Negative = no reaching.
    if (!d.flat && g_cur_color == g_back) g_frame_3d = true;
    float flat_scale = (d.flat && g_cur_color->wide) ? 1.0f / g_video.wide : 0.0f;
    if (d.tex[0].addr) flat_scale = -flat_scale;
    float pc[4] = {d.surface_w, d.surface_h, d.zmax, flat_scale};
    vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);
    float bc[4];
    rgba_from_d3dcolor(d.rs.blend_color, bc);
    vkCmdSetBlendConstants(cb, bc);
    vkCmdBindVertexBuffers(cb, 0, 16, bufs, offs);
    if (need_indices) {
        VkDeviceSize ioff;
        uint32_t* id = (uint32_t*)ring_alloc(seq.size() * 4, 4, &ioff);
        if (!id) return;
        std::memcpy(id, seq.data(), seq.size() * 4);
        vkCmdBindIndexBuffer(cb, f.ring.buf, ioff, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, (uint32_t)seq.size(), 1, 0, -(int32_t)vmin, 0);
    } else {
        vkCmdDraw(cb, d.count, 1, 0, 0);
    }
    g_stat_draws++;
}

// Reads the picture options; when the size of the render targets changes, they
// are all thrown away (the game redraws everything each frame) and made again.
void update_video() {
    Video v;
    v.mode = setting_int("display_mode");
    v.smooth = setting_bool("smoothing");
    v.vsync = g_opt.vsync || setting_bool("vsync");
    const uint32_t W = g_sc_extent.width, H = g_sc_extent.height;
    int res = setting_int("resolution");
    if (g_opt.scale_forced) res = g_opt.scale;
    if (res <= 0) res = H ? (int)((H + kSurfaceH - 1) / kSurfaceH) : 2;      // match the screen
    v.scale = std::min(std::max(res, 1), 6);
    const double shape = H ? (double)W / H : 4.0 / 3;
    v.wide = (v.mode == 1 && shape > 4.0 / 3 + 0.02) ? (float)(std::min(shape, 3.6) / (4.0 / 3)) : 1.0f;
    const bool resize = v.scale != g_video.scale || std::fabs(v.wide - g_video.wide) > 0.001f;
    const bool swap = v.vsync != g_video.vsync;
    const bool first = !g_video_logged;
    g_video = v;
    if (swap) g_sc_dirty = true;
    if (resize || first) {
        g_video_logged = true;
        hle_log("[gpu] picture: mode %d, %d x the Xbox resolution, %.3f x as wide, %s, vsync %d", v.mode, v.scale,
                v.wide, v.smooth ? "smooth" : "sharp", (int)v.vsync);
    }
    if (!resize || g_surfaces.empty()) return;
    vkDeviceWaitIdle(g_dev);
    for (auto& [key, fb] : g_framebuffers) vkDestroyFramebuffer(g_dev, fb, nullptr);
    g_framebuffers.clear();
    for (auto& [key, sf] : g_surfaces) {
        if (sf->sample_view && sf->sample_view != sf->img.view) vkDestroyImageView(g_dev, sf->sample_view, nullptr);
        destroy_image(sf->img);
        delete sf;
    }
    g_surfaces.clear();
    g_cur_color = g_cur_depth = g_pass_depth = g_back = nullptr;
    const TargetArgs back = g_last_back, last = g_last_target;
    if (back.set) set_render_target(back.color, back.cw, back.ch, back.depth, back.dw, back.dh, true);
    if (last.set) set_render_target(last.color, last.cw, last.ch, last.depth, last.dw, last.dh, false);
}

void present() {
    if (!g_active) return;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    CpuTimer timer;
    begin_frame();
    end_pass();
    Frame& f = g_frames[g_fi];
    VkCommandBuffer cb = f.cmd;
    Surface* back = g_back;
    if (back) layout_to(cb, back, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    if (window_take_resized()) g_sc_dirty = true;
    if (g_sc_dirty && g_surface) {
        vkDeviceWaitIdle(g_dev);
        create_swapchain();
    }
    uint32_t image = UINT32_MAX;
    if (g_swapchain && !g_sc_dirty) {
        VkResult r = vkAcquireNextImageKHR(g_dev, g_swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            g_sc_dirty = true;
            image = UINT32_MAX;
        } else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            image = UINT32_MAX;
        }
    }
    static uint32_t frame_no = 0;
    bool stats = back && g_stats_every && ++frame_no % g_stats_every == 0 &&
                 (VkDeviceSize)back->img.w * back->img.h * 4 <= g_readback.size;
    if (stats) {
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {back->img.w, back->img.h, 1};
        vkCmdCopyImageToBuffer(cb, back->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_readback.buf, 1, &r);
    }
    if (image != UINT32_MAX) {
        VkImage sc = g_sc_images[image];
        barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black{};
        VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
        // Centred, in the shape the display mode asks for.
        uint32_t W = g_sc_extent.width, H = g_sc_extent.height;
        uint32_t w = W, h = H;
        bool smooth = g_video.smooth;
        // A widescreen frame with only 2D in it (title, menus, loading): nothing drew the
        // sides, so show just the 4:3 middle, on black.
        const bool crop = back && back->wide && !g_frame_3d;
        const int32_t side = crop ? (int32_t)((back->img.w - back->img.w / g_video.wide) / 2) : 0;
        g_frame_3d = false;
        if (g_video.mode == 3) {                                 // sharp: a whole number of screen pixels each
            uint32_t n = std::max(1u, std::min(W / kSurfaceW, H / kSurfaceH));
            w = kSurfaceW * n;
            h = kSurfaceH * n;
            smooth = false;
        } else if (g_video.mode != 2 && back) {                  // keep the picture's shape
            const double shape = crop ? 4.0 / 3 : (double)back->img.w / back->img.h;
            w = (uint32_t)(H * shape + 0.5);
            if (w > W) { w = W; h = (uint32_t)(W / shape + 0.5); }
        }
        int32_t x0 = ((int32_t)W - (int32_t)w) / 2, y0 = ((int32_t)H - (int32_t)h) / 2;
        if (back) {
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {side, 0, 0};
            blit.srcOffsets[1] = {(int32_t)back->img.w - side, (int32_t)back->img.h, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {x0, y0, 0};
            blit.dstOffsets[1] = {x0 + (int32_t)w, y0 + (int32_t)h, 1};
            vkCmdBlitImage(cb, back->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
        barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    vkEndCommandBuffer(cb);
    VkCommandBuffer cbs[2];
    uint32_t ncb = 0;
    if (f.upload_open) {
        vkEndCommandBuffer(f.upload);
        cbs[ncb++] = f.upload;
    }
    cbs[ncb++] = cb;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = ncb;
    si.pCommandBuffers = cbs;
    if (image != UINT32_MAX) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &g_sc_done[image];
    }
    vkResetFences(g_dev, 1, &f.fence);
    VkResult sr = vkQueueSubmit(g_queue, 1, &si, f.fence);
    if (sr != VK_SUCCESS) hle_log("[gpu] submit failed: %d", (int)sr);
    if (image != UINT32_MAX) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &g_sc_done[image];
        pi.swapchainCount = 1;
        pi.pSwapchains = &g_swapchain;
        pi.pImageIndices = &image;
        VkResult r = vkQueuePresentKHR(g_queue, &pi);
#ifdef __ANDROID__
        if (r == VK_ERROR_OUT_OF_DATE_KHR) g_sc_dirty = true;   // "suboptimal" just means the system is rotating for us
#else
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_sc_dirty = true;
#endif
    }
#ifdef __ANDROID__
    static uint32_t cache_timer = 0;
    if (++cache_timer % 600 == 0) save_pipeline_cache();
#endif
    if (stats) {
        vkWaitForFences(g_dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
        const uint8_t* px = g_readback.ptr;
        uint64_t sum[3] = {}, lit = 0, yellow = 0, red = 0, green = 0, blue = 0, n = (uint64_t)back->img.w * back->img.h;
        uint32_t colours = 0;
        std::vector<uint8_t> seen(1 << 15);
        for (uint64_t i = 0; i < n; i++) {
            const uint8_t* p = px + i * 4;          // B G R A
            sum[0] += p[2]; sum[1] += p[1]; sum[2] += p[0];
            if (p[0] | p[1] | p[2]) lit++;
            if (p[2] > 160 && p[1] > 140 && p[0] < 90) yellow++;
            if (p[2] == 255 && p[1] == 0 && p[0] == 0) red++;
            if (p[2] == 0 && p[1] == 255 && p[0] == 0) green++;
            if (p[2] == 0 && p[1] == 0 && p[0] == 255) blue++;
            uint32_t q = (p[2] >> 3) << 10 | (p[1] >> 3) << 5 | p[0] >> 3;
            if (!seen[q]) { seen[q] = 1; colours++; }
        }
        if (const char* reg = std::getenv("TJE_GPU_REGION")) {      // x,y,w,h in Xbox pixels: mean colour there
            int rx = 0, ry = 0, rw = 0, rh = 0;
            std::sscanf(reg, "%d,%d,%d,%d", &rx, &ry, &rw, &rh);
            uint32_t sc = back->img.h / back->nh, W = back->img.w;
            double m[3] = {};
            uint64_t cnt = 0;
            for (uint32_t y = ry * sc; y < (uint32_t)(ry + rh) * sc && y < back->img.h; y++)
                for (uint32_t x = rx * sc; x < (uint32_t)(rx + rw) * sc && x < W; x++) {
                    const uint8_t* q = px + ((uint64_t)y * W + x) * 4;
                    m[0] += q[2]; m[1] += q[1]; m[2] += q[0]; cnt++;
                }
            if (cnt) hle_log("[gpu] frame %u region mean rgb %.1f %.1f %.1f", frame_no, m[0] / cnt, m[1] / cnt, m[2] / cnt);
        }
        if (red || green) hle_log("[gpu] frame %u: %.2f%% pure red, %.2f%% pure green, %.2f%% pure blue", frame_no, 100.0 * red / n, 100.0 * green / n, 100.0 * blue / n);
        hle_log("[gpu] frame %u: mean rgb %.1f %.1f %.1f, %.1f%% non-black, %u distinct colours, %llu yellow pixels, %u draws",
                frame_no, sum[0] / (double)n, sum[1] / (double)n, sum[2] / (double)n, 100.0 * lit / n, colours,
                (unsigned long long)yellow, g_stat_draws);
    }
    g_frame_open = false;
    g_shadow_enhance = std::min(std::max(setting_int("shadow_enhance"), 0), 100) / 100.0f;
    g_fi = (g_fi + 1) % kFrames;
    g_serial++;
    g_epoch++;

    g_stat_frames++;
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - g_stat_t0).count();
    if (secs >= 1.0) {
        char t[160];
        std::snprintf(t, sizeof t, "ToeJam & Earl III - %.0f fps, %u draws/frame, %u uploads/s, %zu pipelines, renderer %.1f ms/frame",
                      g_stat_frames / secs, g_stat_frames ? g_stat_draws / g_stat_frames : 0, g_stat_uploads,
                      g_pipelines.size(), g_stat_frames ? g_stat_cpu_ms / g_stat_frames : 0.0);
        g_stat_cpu_ms = 0;
        window_set_title(t);
        static int every = 0;
        if (++every % 5 == 0) hle_log("[gpu] %s", t + 20);
        g_stat_frames = g_stat_draws = g_stat_uploads = 0;
        g_stat_t0 = now;
    }
    update_video();
}

}  // namespace tje::gpu
