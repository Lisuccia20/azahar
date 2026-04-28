// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"
#include "common/math_util.h"
#include "video_core/renderer_base.h"
#ifdef HAVE_LIBRETRO
#include "citra_libretro/libretro_vk.h"
#else
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_present_window.h"
#endif
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_render_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/ScreenStreamer.h"

namespace Core {
class System;
}

namespace Memory {
class MemorySystem;
}

namespace Pica {
class PicaCore;
}

namespace Layout {
struct FramebufferLayout;
}

namespace VideoCore {
class GPU;
}

namespace Vulkan {

struct TextureInfo {
    u32 width;
    u32 height;
    Pica::PixelFormat format;
    vk::Image image;
    vk::ImageView image_view;
    VmaAllocation allocation;
};

struct ScreenInfo {
    TextureInfo texture;
    Common::Rectangle<f32> texcoords;
    vk::ImageView image_view;
};

struct PresentUniformData {
    std::array<f32, 4 * 4> modelview;
    Common::Vec4f i_resolution;
    Common::Vec4f o_resolution;
    int screen_id_l = 0;
    int screen_id_r = 0;
    int layer = 0;
    int reverse_interlaced = 0;
};
static_assert(sizeof(PresentUniformData) == 112,
              "PresentUniformData does not structure in shader!");

class RendererVulkan : public VideoCore::RendererBase {
    static constexpr std::size_t PRESENT_PIPELINES = 3;

public:
    explicit RendererVulkan(Core::System& system, Pica::PicaCore& pica, Frontend::EmuWindow& window,
                            Frontend::EmuWindow* secondary_window);
    ~RendererVulkan() override;

    [[nodiscard]] VideoCore::RasterizerInterface* Rasterizer() override {
        return &rasterizer;
    }

    void NotifySurfaceChanged(bool second) override;

    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override {}

    struct StreamTexture {
        VkImage       image      = VK_NULL_HANDLE;
        VkImageView   image_view = VK_NULL_HANDLE;
        VmaAllocation alloc      = VK_NULL_HANDLE;
        u32           width      = 0;
        u32           height     = 0;
        bool          initialized = false;
    };
    StreamTexture stream_tex;
    void EnsureStreamTexture(u32 w, u32 h);
    void DestroyStreamTexture();

private:
    // All'interno della classe RendererVulkan, sezione private:
    ScreenStreamer* screen_streamer = nullptr;
    void ReloadPipeline(Settings::StereoRenderOption render_3d);
    void CompileShaders();
    void BuildLayouts();
    void BuildPipelines();
    void ConfigureFramebufferTexture(TextureInfo& texture,
                                     const Pica::FramebufferConfig& framebuffer);
    void ConfigureRenderPipeline();
    void PrepareRendertarget();
    void RenderScreenshot();
    void RenderScreenshotWithStagingCopy();
    bool TryRenderScreenshotWithHostMemory();
    void PrepareDraw(Frame* frame, const Layout::FramebufferLayout& layout);
    void RenderToWindow(PresentWindow& window, const Layout::FramebufferLayout& layout,
                        bool flipped);

    void DrawScreens(Frame* frame, const Layout::FramebufferLayout& layout, bool flipped);
    void DrawBottomScreen(const Layout::FramebufferLayout& layout,
                          const Common::Rectangle<u32>& bottom_screen);

    void DrawTopScreen(const Layout::FramebufferLayout& layout,
                       const Common::Rectangle<u32>& top_screen);
    void DrawSingleScreen(u32 screen_id, float x, float y, float w, float h,
                          Layout::DisplayOrientation orientation);
    void DrawSingleScreenStereo(u32 screen_id_l, u32 screen_id_r, float x, float y, float w,
                                float h, Layout::DisplayOrientation orientation);

    void ApplySecondLayerOpacity(float alpha);

    void DrawCursor(const Layout::FramebufferLayout& layout);

    void LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer, ScreenInfo& screen_info,
                            bool right_eye);
    void FillScreen(Common::Vec3<u8> color, const TextureInfo& texture);

private:
    // --- Streaming ---
    void TryStreamBottomScreen();
    void EnsureStreamSlots(u64 required_size);
    void DestroyStreamSlot(u32 idx);

    static constexpr u32 STREAM_BUF_COUNT = 2;

    struct StreamStagingSlot {
        // Staging buffer CPU-visible
        VkBuffer      buf             = VK_NULL_HANDLE;
        VmaAllocation buf_alloc       = VK_NULL_HANDLE;
        void*         mapped          = nullptr;
        // Frame dedicato per il rendering
        VkImage       frame_image     = VK_NULL_HANDLE;
        VmaAllocation frame_alloc     = VK_NULL_HANDLE;
        VkImageView   frame_view      = VK_NULL_HANDLE;
        VkFramebuffer frame_fb        = VK_NULL_HANDLE;
        // Sincronizzazione
        u64           tick            = 0;
        bool          pending         = false;
    };

    StreamStagingSlot stream_slots_[STREAM_BUF_COUNT];
    u32               stream_write_idx_      = 0;
    u64               stream_slot_size_      = 0;
    u32               stream_frame_counter_  = 0;

    Memory::MemorySystem& memory;
    Pica::PicaCore& pica;

#ifdef HAVE_LIBRETRO
    LibRetroVKInstance instance;
#else
    Instance instance;
#endif
    Scheduler scheduler;
    RenderManager renderpass_cache;
    PresentWindow main_present_window;
    StreamBuffer vertex_buffer;
    DescriptorUpdateQueue update_queue;
    RasterizerVulkan rasterizer;
    std::unique_ptr<PresentWindow> secondary_present_window_ptr;

    DescriptorHeap present_heap;
    vk::UniquePipelineLayout present_pipeline_layout;
    std::array<vk::Pipeline, PRESENT_PIPELINES> present_pipelines;
    std::array<vk::ShaderModule, PRESENT_PIPELINES> present_shaders;
    std::array<vk::Sampler, 2> present_samplers;
    vk::ShaderModule present_vertex_shader;
    u32 current_pipeline = 0;

    std::array<ScreenInfo, 3> screen_infos{};
    PresentUniformData draw_info{};
    vk::ClearColorValue clear_color{};

    vk::ShaderModule cursor_vertex_shader{};
    vk::ShaderModule cursor_fragment_shader{};
    vk::Pipeline cursor_pipeline{};
    vk::UniquePipelineLayout cursor_pipeline_layout{};
};

} // namespace Vulkan
