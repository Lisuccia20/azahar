#include "common/logging/log.h"
#include "common/settings.h"
#include "video_core/gpu.h"
#ifdef ENABLE_OPENGL
#include "video_core/renderer_opengl/renderer_opengl.h"
#endif
#ifdef ENABLE_SOFTWARE_RENDERER
#include "video_core/renderer_software/renderer_software.h"
#endif
#ifdef ENABLE_VULKAN
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#endif
// ← AGGIUNGI QUESTO
#ifdef ENABLE_METAL
#include "video_core/renderer_metal/renderer_metal.h"
#endif
#include "video_core/video_core.h"

#ifdef ENABLE_SDL2
#include <SDL.h>
#endif

namespace VideoCore {

std::unique_ptr<RendererBase> CreateRenderer(Frontend::EmuWindow& emu_window,
                                             Frontend::EmuWindow* secondary_window,
                                             Pica::PicaCore& pica, Core::System& system) {
    const Settings::GraphicsAPI graphics_api = Settings::values.graphics_api.GetValue();
    switch (graphics_api) {
#ifdef ENABLE_SOFTWARE_RENDERER
    case Settings::GraphicsAPI::Software:
        return std::make_unique<SwRenderer::RendererSoftware>(system, pica, emu_window);
#endif
#ifdef ENABLE_VULKAN
    case Settings::GraphicsAPI::Vulkan:
#if defined(ENABLE_SDL2) && !defined(__APPLE__)
        if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
            SDL_Init(SDL_INIT_VIDEO);
        }
#endif
        return std::make_unique<Vulkan::RendererVulkan>(system, pica, emu_window, secondary_window);
#endif
#ifdef ENABLE_OPENGL
    case Settings::GraphicsAPI::OpenGL:
        return std::make_unique<OpenGL::RendererOpenGL>(system, pica, emu_window, secondary_window);
#endif
    // ← AGGIUNGI QUESTO BLOCCO
#ifdef ENABLE_METAL
    case Settings::GraphicsAPI::Metal:
#if defined(__APPLE__) && defined(ENABLE_METAL)
    return std::make_unique<Metal::RendererMetal>(system, pica, emu_window, secondary_window);
#else
    LOG_ERROR(Render, "Metal non disponibile, fallback a Vulkan");
    return std::make_unique<Vulkan::RendererVulkan>(system, pica, emu_window, secondary_window);
#endif
#endif
    default:
        LOG_CRITICAL(Render,
                     "Unknown or unsupported graphics API {}, falling back to available default",
                     graphics_api);
#ifdef ENABLE_METAL
        return std::make_unique<Metal::RendererMetal>(system, pica, emu_window, secondary_window);
#elif ENABLE_OPENGL
        return std::make_unique<OpenGL::RendererOpenGL>(system, pica, emu_window, secondary_window);
#elif ENABLE_VULKAN
        return std::make_unique<Vulkan::RendererVulkan>(system, pica, emu_window, secondary_window);
#elif ENABLE_SOFTWARE_RENDERER
        return std::make_unique<SwRenderer::RendererSoftware>(system, pica, emu_window);
#else
#error "At least one renderer must be enabled."
#endif
    }
}

} // namespace VideoCore