#pragma once
#ifdef __APPLE__

#include "core/frontend/emu_window.h"
#include <mutex>
#include <condition_variable>
#include <array>
#include "common/common_types.h"

namespace Metal {

// Frame in volo tra thread render e thread presentazione
// Usa void* per nascondere i tipi Metal al compilatore C++
struct MetalFrame {
    void* color_tex   = nullptr; // id<MTLTexture>
    void* drawable    = nullptr; // id<CAMetalDrawable>
    u32   width       = 0;
    u32   height      = 0;
    bool  color_reloaded = false;
};

class MetalTextureMailbox final : public Frontend::TextureMailbox {
public:
    // device e layer passati come void* per non esporre Metal nell'header
    MetalTextureMailbox(void* device, void* metal_layer);
    ~MetalTextureMailbox() override;

    Frontend::Frame* GetRenderFrame()                                    override;
    void             ReleaseRenderFrame(Frontend::Frame* frame)          override;
    Frontend::Frame* TryGetPresentFrame(int timeout_ms)                  override;
    void             ReloadRenderFrame(Frontend::Frame* f, u32 w, u32 h) override;
    void             ReloadPresentFrame(Frontend::Frame* f, u32 w, u32 h)override;

private:
    void* device_opaque = nullptr;
    void* layer_opaque  = nullptr;

    static constexpr u32 FRAME_COUNT = 2;
    std::array<MetalFrame, FRAME_COUNT> frames{};

    u32  render_frame_idx  = 0;
    u32  present_frame_idx = 0;

    std::mutex              mutex;
    std::condition_variable present_cv;
    bool                    has_present_frame = false;

    void CreateFrameTexture(MetalFrame& frame, u32 width, u32 height);
};

} // namespace Metal
#endif // __APPLE__