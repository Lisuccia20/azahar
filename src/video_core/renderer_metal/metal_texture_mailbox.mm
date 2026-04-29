#ifdef __APPLE__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "metal_texture_mailbox.h"
#include "common/logging/log.h"

namespace Metal {

MetalTextureMailbox::MetalTextureMailbox(void* device, void* metal_layer) {
    device_opaque = device;
    layer_opaque  = metal_layer;

    id<MTLDevice> dev = (__bridge id<MTLDevice>)device_opaque;
    CAMetalLayer* lay = (__bridge CAMetalLayer*)layer_opaque;

    const u32 w = static_cast<u32>(lay.drawableSize.width);
    const u32 h = static_cast<u32>(lay.drawableSize.height);
    for (auto& frame : frames)
        CreateFrameTexture(frame, w > 0 ? w : 800, h > 0 ? h : 480);
}

MetalTextureMailbox::~MetalTextureMailbox() {
    for (auto& frame : frames) {
        if (frame.color_tex) {
            id<MTLTexture> tex = (__bridge_transfer id<MTLTexture>)frame.color_tex;
            (void)tex;
            frame.color_tex = nullptr;
        }
    }
}

void MetalTextureMailbox::CreateFrameTexture(MetalFrame& frame, u32 width, u32 height) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device_opaque;

    // Rilascia la texture precedente se esiste
    if (frame.color_tex) {
        id<MTLTexture> old = (__bridge_transfer id<MTLTexture>)frame.color_tex;
        (void)old;
        frame.color_tex = nullptr;
    }

    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;

    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    frame.color_tex      = (__bridge_retained void*)tex;
    frame.width          = width;
    frame.height         = height;
    frame.color_reloaded = true;
}

Frontend::Frame* MetalTextureMailbox::GetRenderFrame() {
    std::unique_lock lock{mutex};
    return reinterpret_cast<Frontend::Frame*>(&frames[render_frame_idx]);
}

void MetalTextureMailbox::ReleaseRenderFrame(Frontend::Frame* f) {
    std::unique_lock lock{mutex};
    present_frame_idx = render_frame_idx;
    render_frame_idx  = (render_frame_idx + 1) % FRAME_COUNT;
    has_present_frame = true;
    lock.unlock();
    present_cv.notify_one();
}

Frontend::Frame* MetalTextureMailbox::TryGetPresentFrame(int timeout_ms) {
    std::unique_lock lock{mutex};
    const bool ready = present_cv.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [this] { return has_present_frame; });

    if (!ready) {
        LOG_DEBUG(Render, "TryGetPresentFrame: timeout dopo {}ms", timeout_ms);
        return nullptr;
    }

    has_present_frame = false;
    return reinterpret_cast<Frontend::Frame*>(&frames[present_frame_idx]);
}

void MetalTextureMailbox::ReloadRenderFrame(Frontend::Frame* f, u32 w, u32 h) {
    auto& frame = *reinterpret_cast<MetalFrame*>(f);
    std::unique_lock lock{mutex};
    CreateFrameTexture(frame, w, h);
    LOG_DEBUG(Render, "ReloadRenderFrame: {}x{}", w, h);
}

void MetalTextureMailbox::ReloadPresentFrame(Frontend::Frame* f, u32 w, u32 h) {
    ReloadRenderFrame(f, w, h);
}

} // namespace Metal
#endif // __APPLE__