#pragma once
#ifdef __APPLE__

#include "common/common_types.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"  // ← usa il rasterizer Vulkan
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <memory>

namespace Pica {
class PicaCore;
}

namespace Metal {
struct MetalRendererImpl;
}

namespace Metal {

class RendererMetal final : public VideoCore::RendererBase {
public:
    RendererMetal(Core::System& system, Pica::PicaCore& pica,
                  Frontend::EmuWindow& window,
                  Frontend::EmuWindow* secondary_window);
    ~RendererMetal() override;

    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override;
    void PrepareVideoDumping() override;
    void CleanupVideoDumping() override;

    // Delega al rasterizer Vulkan sottostante
    VideoCore::RasterizerInterface* Rasterizer() override {
        return rasterizer.get();
    }

private:
    std::unique_ptr<MetalRendererImpl> impl;
    Pica::PicaCore& pica;

    // Rasterizer Vulkan per il rendering 3DS — Metal gestisce solo la presentazione
    Vulkan::Instance              vk_instance;
    Vulkan::Scheduler             vk_scheduler;
    std::unique_ptr<Vulkan::RasterizerVulkan> rasterizer;
};

} // namespace Metal
#endif // __APPLE__