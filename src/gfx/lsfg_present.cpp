/*
 * Lossless Scaling frame generation on SDL's GPU device: see lsfg_present.h.
 *
 * Everything is recorded into SDL's own command buffers between its passes
 * (patches/SDL gives out the Vulkan objects). SDL keeps each texture in one
 * layout between passes, its "default usage": COLOR_ATTACHMENT_OPTIMAL for
 * a color target without SAMPLER, and for a swap chain image once
 * acquired. What is recorded here moves the textures it uses out of that
 * layout and back.
 *
 * The flow follows BattleTanx: Global Assault Recompiled's RT64 present
 * queue (its lib/lsfg/rt64), with timed pacing: SDL gives no present wait.
 */

#include "lsfg_present.h"

#include <volk.h>

#include "lsfg/lsfg.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

/* patches/SDL/0001-vulkan-handles-for-frame-generation.patch */
extern "C" {
bool SDLCALL PDO_GetGPUVulkanDevice(SDL_GPUDevice *device, VkInstance *instance, VkPhysicalDevice *physical_device,
                                    VkDevice *logical_device, Uint32 *queue_family_index);
VkImage SDLCALL PDO_GetGPUVulkanImage(SDL_GPUTexture *texture);
VkCommandBuffer SDLCALL PDO_GetGPUVulkanCommandBuffer(SDL_GPUCommandBuffer *command_buffer);
}

namespace {

struct State {
    bool running = false;
    std::string status;
    SDL_GPUDevice *dev = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    lsfg::ShaderSet shaders;
    std::unique_ptr<lsfg::FrameGenerator> generator;
    int multiplier = 2;
    bool performance = false;
    bool generator_failed = false;      /* until the settings or the size change */
    VkExtent2D failed_extent{};
    bool input_pending = false;         /* a frame went in: scene_cut may look */

    /* Between game frames, smoothed, for spacing the generated ones. */
    Uint64 last_frame = 0;
    double interval = 0.0;
} S;

void set_status(const std::string &why)
{
    if (S.status != why && !why.empty())
        SDL_Log("lsfg: %s", why.c_str());
    S.status = why;
}

std::filesystem::path fs_path(const char *utf8)
{
    std::u8string s(reinterpret_cast<const char8_t *>(utf8));
    return std::filesystem::path(s);
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src_stage,
             VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

/* SDL's color target layout to a transfer one, and back. */
void to_transfer(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, VkAccessFlags access)
{
    barrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, layout, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, access);
}

void from_transfer(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, VkAccessFlags access)
{
    barrier(cmd, image, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, access,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

/* The generator for the picture's size and the settings; none (the frame
 * shown alone) when the multiplier is 1 or it cannot be made. */
void make_generator(uint32_t w, uint32_t h)
{
    uint32_t want = uint32_t(std::clamp(S.multiplier, 1, 8) - 1);

    if (S.generator) {
        const lsfg::Settings &have = S.generator->settings();
        if (have.generated_frames == want && have.performance == S.performance &&
            S.generator->extent().width == w && S.generator->extent().height == h)
            return;
        /* Its objects may be in work the GPU has not done yet. */
        SDL_WaitForGPUIdle(S.dev);
        S.generator.reset();
        S.input_pending = false;
    }
    if (S.generator_failed && S.failed_extent.width == w && S.failed_extent.height == h)
        return;
    S.generator_failed = false;
    if (want == 0)
        return;
    lsfg::Settings ls;
    std::string error;
    ls.generated_frames = want;
    ls.performance = S.performance;
    ls.flow_scale = lsfg::default_flow_scale(h);
    S.generator = lsfg::FrameGenerator::create(S.physical, S.device, S.shaders, ls, { w, h }, error);
    if (!S.generator) {
        S.generator_failed = true;
        S.failed_extent = { w, h };
        set_status(error);
        return;
    }
    S.last_frame = 0;
    S.interval = 0.0;
    SDL_Log("lsfg: generating %u frame%s per game frame at %ux%u (%s mode)", want, want == 1 ? "" : "s", w, h,
            S.performance ? "performance" : "quality");
    set_status("");
}

bool start(SDL_GPUDevice *dev, const char *dll, const char *cache)
{
    VkInstance instance = VK_NULL_HANDLE;
    Uint32 family = 0;
    std::string why;

    S.dev = dev;
    if (SDL_strcmp(SDL_GetGPUDeviceDriver(dev), "vulkan") != 0) {
        set_status("Frame generation needs the Vulkan GPU driver.");
        return false;
    }
    if (!PDO_GetGPUVulkanDevice(dev, &instance, &S.physical, &S.device, &family)) {
        set_status("SDL gave out no Vulkan device (patches/SDL not applied?).");
        return false;
    }
    {
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (!gipa) {
            set_status("No Vulkan loader.");
            return false;
        }
        volkInitializeCustom(gipa);
        volkLoadInstance(instance);
        volkLoadDevice(S.device);
    }
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(S.physical, &props);
        SDL_Log("lsfg: on %s", props.deviceName);
    }
    if (!lsfg::check_device(S.physical, why)) {
        set_status(why);
        return false;
    }
    if (!dll || !*dll) {
        set_status("Lossless.dll was not found: choose it in Options > Advanced, or put it in the "
                   "lossless_scaling folder next to the settings.");
        return false;
    }
    {
        std::string error;
        std::error_code ec;
        Uint64 t0 = SDL_GetTicksNS();
        std::filesystem::create_directories(fs_path(cache).parent_path(), ec);
        if (!lsfg::load_shaders(fs_path(dll), fs_path(cache), S.shaders, error)) {
            set_status("Lossless.dll can't be used: " + error);
            return false;
        }
        SDL_Log("lsfg: Lossless Scaling's shaders ready in %.1f s", (SDL_GetTicksNS() - t0) / 1e9);
    }
    S.running = true;
    set_status("");
    return true;
}

}   // namespace

extern "C" SDL_GPUDevice *pdo_lsfg_create_device(void)
{
    VkPhysicalDeviceVulkan12Features vk12{};
    VkPhysicalDeviceFeatures vk10{};
    SDL_GPUVulkanOptions options;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_GPUDevice *dev;

    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk12.vulkanMemoryModel = VK_TRUE;
    vk10.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    SDL_zero(options);
    options.feature_list = &vk12;
    options.vulkan_10_physical_device_features = &vk10;
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
    SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &options);
    /* The translated shaders are SPIR-V 1.6, Vulkan 1.3's; drivers take
     * them on 1.2 as well, so 1.2 where 1.3 is not to be had. */
    options.vulkan_api_version = VK_API_VERSION_1_3;
    dev = SDL_CreateGPUDeviceWithProperties(props);
    if (!dev) {
        options.vulkan_api_version = VK_API_VERSION_1_2;
        dev = SDL_CreateGPUDeviceWithProperties(props);
    }
    SDL_DestroyProperties(props);
    if (!dev)
        set_status("No Vulkan 1.2 GPU with the memory model and format-less storage writes.");
    return dev;
}

extern "C" bool pdo_lsfg_start(SDL_GPUDevice *dev, const char *dll, const char *cache)
{
    bool ok;

    if (S.running)
        return true;
    try {
        ok = start(dev, dll, cache);
    } catch (const std::exception &e) {
        set_status(std::string("Frame generation failed to start: ") + e.what());
        ok = false;
    }
    if (!ok) {
        std::string why = S.status;
        pdo_lsfg_stop();
        S.status = why;
    }
    return ok;
}

extern "C" void pdo_lsfg_stop(void)
{
    S = State();
}

extern "C" void pdo_lsfg_configure(int multiplier, bool performance)
{
    if (multiplier != S.multiplier || performance != S.performance)
        S.generator_failed = false;
    S.multiplier = multiplier;
    S.performance = performance;
}

extern "C" const char *pdo_lsfg_status(void)
{
    return S.status.c_str();
}

extern "C" uint32_t pdo_lsfg_record(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, uint32_t w, uint32_t h)
{
    uint32_t generated = 0;

    if (!S.running || !cmd || !src || !w || !h)
        return 0;
    try {
        make_generator(w, h);
        if (!S.generator)
            return 0;
        VkCommandBuffer vk = PDO_GetGPUVulkanCommandBuffer(cmd);
        VkImage image = PDO_GetGPUVulkanImage(src);
        to_transfer(vk, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
        S.generator->record_input(vk, image, { w, h });
        if (S.generator->record_generate(vk))
            generated = S.generator->settings().generated_frames;
        from_transfer(vk, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0);
        S.input_pending = true;
    } catch (const std::exception &e) {
        set_status(std::string("Frame generation stopped: ") + e.what());
        SDL_WaitForGPUIdle(S.dev);
        S.generator.reset();
        S.generator_failed = true;
        S.failed_extent = { w, h };
        return 0;
    }
    return generated;
}

extern "C" bool pdo_lsfg_scene_cut(void)
{
    if (!S.generator || !S.input_pending)
        return false;
    S.input_pending = false;
    return S.generator->scene_cut();
}

extern "C" uint64_t pdo_lsfg_spacing(uint32_t shown)
{
    Uint64 now = SDL_GetTicksNS();
    if (S.last_frame) {
        double d = double(now - S.last_frame);
        if (d < 250e6)      /* past that a hitch or a pause, not the frame rate */
            S.interval = S.interval > 0.0 ? S.interval * 0.75 + d * 0.25 : d;
    }
    S.last_frame = now;
    if (S.interval <= 0.0 || shown == 0)
        return 0;
    return uint64_t(S.interval / shown);
}

extern "C" void pdo_lsfg_record_output(SDL_GPUCommandBuffer *cmd, int index, SDL_GPUTexture *src, uint32_t sw,
                                       uint32_t sh, SDL_GPUTexture *dst, uint32_t w, uint32_t h)
{
    if (!S.running || !cmd || !dst || !w || !h)
        return;
    VkCommandBuffer vk = PDO_GetGPUVulkanCommandBuffer(cmd);
    VkImage out = PDO_GetGPUVulkanImage(dst);

    to_transfer(vk, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (index >= 0 && S.generator) {
        S.generator->record_output(vk, uint32_t(index), out, { 0, 0 }, { w, h });
    } else if (src && sw && sh) {
        VkImage in = PDO_GetGPUVulkanImage(src);
        VkImageBlit r{};
        r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.srcOffsets[1] = { int32_t(sw), int32_t(sh), 1 };
        r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.dstOffsets[1] = { int32_t(w), int32_t(h), 1 };
        to_transfer(vk, in, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdBlitImage(vk, in, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                       sw == w && sh == h ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        from_transfer(vk, in, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0);
    }
    from_transfer(vk, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
}

extern "C" bool pdo_lsfg_find_dll(const char *data_dir, char *out, size_t len)
{
    std::vector<std::string> where;
    const char *home = SDL_getenv("HOME");
    const char *base = SDL_GetBasePath();

    if (data_dir)
        where.push_back(std::string(data_dir) + "lossless_scaling/Lossless.dll");
    if (base)
        where.push_back(std::string(base) + "Lossless.dll");
#ifdef _WIN32
    (void)home;
    where.push_back("C:/Program Files (x86)/Steam/steamapps/common/Lossless Scaling/Lossless.dll");
    where.push_back("C:/Program Files/Steam/steamapps/common/Lossless Scaling/Lossless.dll");
#else
    if (home) {
        where.push_back(std::string(home) + "/.local/share/Steam/steamapps/common/Lossless Scaling/Lossless.dll");
        where.push_back(std::string(home) + "/.steam/steam/steamapps/common/Lossless Scaling/Lossless.dll");
    }
#endif
    for (const std::string &p : where) {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_FILE) {
            SDL_strlcpy(out, p.c_str(), len);
            return true;
        }
    }
    return false;
}
