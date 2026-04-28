// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/memory_detect.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_memory_util.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/ScreenStreamer.h"
#include <iostream>
#include <cerrno>
#include "video_core/host_shaders/vulkan_present_anaglyph_frag.h"
#include "video_core/host_shaders/vulkan_present_frag.h"
#include "video_core/host_shaders/vulkan_present_interlaced_frag.h"
#include "video_core/host_shaders/vulkan_present_vert.h"

#include "video_core/host_shaders/vulkan_cursor_frag.h"
#include "video_core/host_shaders/vulkan_cursor_vert.h"

#include <vk_mem_alloc.h>

#if defined(__APPLE__) && !defined(HAVE_LIBRETRO)
#include "common/apple_utils.h"
#endif

#ifdef ENABLE_SDL2
#include <SDL.h>
#endif

MICROPROFILE_DEFINE(Vulkan_RenderFrame, "Vulkan", "Render Frame", MP_RGB(128, 128, 64));

namespace Vulkan {
vk::Format MakeFormat(VideoCore::PixelFormat format);

struct ScreenRectVertex {
    ScreenRectVertex() = default;
    ScreenRectVertex(float x, float y, float u, float v)
        : position{Common::MakeVec(x, y)}, tex_coord{Common::MakeVec(u, v)} {}

    Common::Vec2f position;
    Common::Vec2f tex_coord;
};

constexpr u32 VERTEX_BUFFER_SIZE = sizeof(ScreenRectVertex) * 8192;

constexpr std::array<f32, 4 * 4> MakeOrthographicMatrix(u32 width, u32 height) {
    // clang-format off
    return { 2.f / width, 0.f,         0.f, -1.f,
            0.f,         2.f / height, 0.f, -1.f,
            0.f,         0.f,          1.f,  0.f,
            0.f,         0.f,          0.f,  1.f};
    // clang-format on
}

constexpr static std::array<vk::DescriptorSetLayoutBinding, 1> PRESENT_BINDINGS = {{
    {0, vk::DescriptorType::eCombinedImageSampler, 3, vk::ShaderStageFlagBits::eFragment},
}};

namespace {
static bool IsLowRefreshRate() {
#if (defined(__APPLE__) || defined(ENABLE_SDL2)) && !defined(HAVE_LIBRETRO)
    if (!Settings::values.use_display_refresh_rate_detection) {
        LOG_INFO(Render_Vulkan, "Refresh rate detection is currently disabled via settings");
        return false;
    }
#ifdef __APPLE__
    // Apple's low power mode sometimes limits applications to 30fps without changing the refresh
    // rate, meaning the above code doesn't catch it.
    if (AppleUtils::IsLowPowerModeEnabled()) {
        LOG_WARNING(Render_Vulkan, "Apple's low power mode is enabled, assuming low application "
                                   "framerate. FIFO will be disabled");
        return true;
    }

    const auto cur_refresh_rate = AppleUtils::GetRefreshRate();
#elif defined(ENABLE_SDL2)
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        LOG_ERROR(Render_Vulkan, "Attempted to check refresh rate via SDL, but failed because "
                                 "SDL_INIT_VIDEO wasn't initialized");
        return false;
    }

    SDL_DisplayMode cur_display_mode;
    SDL_GetCurrentDisplayMode(0, &cur_display_mode); // TODO: Multimonitor handling. -OS

    const auto cur_refresh_rate = cur_display_mode.refresh_rate;
#endif // ENABLE_SDL2

    if (cur_refresh_rate < SCREEN_REFRESH_RATE) {
        LOG_WARNING(Render_Vulkan,
                    "Detected refresh rate lower than the emulated 3DS screen: {}hz. FIFO will "
                    "be disabled",
                    cur_refresh_rate);
        return true;
    } else {
        LOG_INFO(Render_Vulkan, "Refresh rate is above emulated 3DS screen: {}hz. Good.",
                 cur_refresh_rate);
    }
#endif // (defined(__APPLE__) || defined(ENABLE_SDL2)) && !defined(HAVE_LIBRETRO)

    // We have no available method of checking refresh rate. Just assume that everything is fine :)
    return false;
}
} // Anonymous namespace

RendererVulkan::RendererVulkan(Core::System& system, Pica::PicaCore& pica_,
                               Frontend::EmuWindow& window, Frontend::EmuWindow* secondary_window)
    : RendererBase{system, window, secondary_window}, memory{system.Memory()}, pica{pica_},
      instance{window, Settings::values.physical_device.GetValue()}, scheduler{instance},
      renderpass_cache{instance, scheduler},
      main_present_window{window, instance, scheduler, IsLowRefreshRate()},
      vertex_buffer{instance, scheduler, vk::BufferUsageFlagBits::eVertexBuffer,
                    VERTEX_BUFFER_SIZE},
      update_queue{instance}, rasterizer{memory,
                                         pica,
                                         system.CustomTexManager(),
                                         *this,
                                         render_window,
                                         instance,
                                         scheduler,
                                         renderpass_cache,
                                         update_queue,
                                         main_present_window.ImageCount()},
      present_heap{instance, scheduler.GetMasterSemaphore(), PRESENT_BINDINGS, 32} {
    CompileShaders();
    BuildLayouts();
    BuildPipelines();

    screen_streamer = new ScreenStreamer(5000, &system);
    if (secondary_window) {
        secondary_present_window_ptr = std::make_unique<PresentWindow>(
            *secondary_window, instance, scheduler, IsLowRefreshRate());
    }
}

RendererVulkan::~RendererVulkan() {
    vk::Device device = instance.GetDevice();
    scheduler.Finish();
    main_present_window.WaitPresent();
    device.waitIdle();

    device.destroyShaderModule(present_vertex_shader);
    for (u32 i = 0; i < PRESENT_PIPELINES; i++) {
        device.destroyPipeline(present_pipelines[i]);
        device.destroyShaderModule(present_shaders[i]);
    }

    for (auto& sampler : present_samplers) {
        device.destroySampler(sampler);
    }

    // Aspetta tutti gli slot pendenti prima di distruggere
    for (auto& slot : stream_slots_) {
        if (slot.pending)
            scheduler.Wait(slot.tick);
        if (slot.buf != VK_NULL_HANDLE)
            vmaDestroyBuffer(instance.GetAllocator(), slot.buf, slot.buf_alloc);
    }

    for (auto& info : screen_infos) {
        device.destroyImageView(info.texture.image_view);
        vmaDestroyImage(instance.GetAllocator(), info.texture.image, info.texture.allocation);
    }

    device.destroyPipeline(cursor_pipeline);
    device.destroyShaderModule(cursor_vertex_shader);
    device.destroyShaderModule(cursor_fragment_shader);
}

void RendererVulkan::PrepareRendertarget() {
    const auto& framebuffer_config = pica.regs.framebuffer_config;
    const auto& regs_lcd = pica.regs_lcd;
    for (u32 i = 0; i < 3; i++) {
        const u32 fb_id = i == 2 ? 1 : 0;
        const auto& framebuffer = framebuffer_config[fb_id];
        auto& texture = screen_infos[i].texture;

        const auto color_fill = fb_id == 0 ? regs_lcd.color_fill_top : regs_lcd.color_fill_bottom;
        if (color_fill.is_enabled) {
            screen_infos[i].image_view = texture.image_view;
            FillScreen(color_fill.AsVector(), texture);
            continue;
        }

        if (texture.width != framebuffer.width || texture.height != framebuffer.height ||
            texture.format != framebuffer.color_format) {
            ConfigureFramebufferTexture(texture, framebuffer);
        }

        LoadFBToScreenInfo(framebuffer, screen_infos[i], i == 1);
    }
}

void RendererVulkan::EnsureStreamTexture(u32 w, u32 h) {
    if (stream_tex.initialized && stream_tex.width == w && stream_tex.height == h)
        return;

    DestroyStreamTexture();

    const VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_B8G8R8A8_UNORM,  // ✅ formato fisso noto
        .extent        = {w, h, 1},
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo alloc_ci = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    vmaCreateImage(instance.GetAllocator(), &img_ci, &alloc_ci,
                   &stream_tex.image, &stream_tex.alloc, nullptr);

    // Transizione iniziale a eTransferDstOptimal
    scheduler.Record([img = stream_tex.image](vk::CommandBuffer cmdbuf) {
        vk::ImageMemoryBarrier barrier = {
            .srcAccessMask       = {},
            .dstAccessMask       = vk::AccessFlagBits::eTransferWrite,
            .oldLayout           = vk::ImageLayout::eUndefined,
            .newLayout           = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = img,
            .subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eTransfer,
            {}, {}, {}, barrier);
    });
    scheduler.Finish();

    stream_tex.width       = w;
    stream_tex.height      = h;
    stream_tex.initialized = true;
}

void RendererVulkan::DestroyStreamTexture() {
    if (!stream_tex.initialized) return;
    vmaDestroyImage(instance.GetAllocator(), stream_tex.image, stream_tex.alloc);
    stream_tex = {};
}

void RendererVulkan::PrepareDraw(Frame* frame, const Layout::FramebufferLayout& layout) {
    const auto sampler = present_samplers[!Settings::values.filter_mode.GetValue()];
    const auto present_set = present_heap.Commit();
    for (u32 index = 0; index < screen_infos.size(); index++) {
        update_queue.AddImageSampler(present_set, 0, index, screen_infos[index].image_view,
                                     sampler);
    }

    renderpass_cache.EndRendering();
    scheduler.Record([this, layout, frame, present_set,
                      renderpass = main_present_window.Renderpass(),
                      index = current_pipeline](vk::CommandBuffer cmdbuf) {
        const vk::Viewport viewport = {
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(layout.width),
            .height = static_cast<float>(layout.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };

        const vk::Rect2D scissor = {
            .offset = {0, 0},
            .extent = {layout.width, layout.height},
        };

        cmdbuf.setViewport(0, viewport);
        cmdbuf.setScissor(0, scissor);

        const vk::ClearValue clear{.color = clear_color};
        const vk::PipelineLayout layout{*present_pipeline_layout};
        const vk::RenderPassBeginInfo renderpass_begin_info = {
            .renderPass = renderpass,
            .framebuffer = frame->framebuffer,
            .renderArea =
                vk::Rect2D{
                    .offset = {0, 0},
                    .extent = {frame->width, frame->height},
                },
            .clearValueCount = 1,
            .pClearValues = &clear,
        };

        cmdbuf.beginRenderPass(renderpass_begin_info, vk::SubpassContents::eInline);
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, present_pipelines[index]);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, present_set, {});
    });
}

void RendererVulkan::RenderToWindow(PresentWindow& window, const Layout::FramebufferLayout& layout,
                                    bool flipped) {
    Frame* frame = window.GetRenderFrame();

    if (layout.width != frame->width || layout.height != frame->height) {
        window.WaitPresent();
        scheduler.Finish();
        window.RecreateFrame(frame, layout.width, layout.height);
    }

    clear_color.float32[0] = Settings::values.bg_red.GetValue();
    clear_color.float32[1] = Settings::values.bg_green.GetValue();
    clear_color.float32[2] = Settings::values.bg_blue.GetValue();
    clear_color.float32[3] = 1.0f;

    DrawScreens(frame, layout, flipped);
    scheduler.Flush(frame->render_ready);

    window.Present(frame);
}

void RendererVulkan::LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer,
                                        ScreenInfo& screen_info, bool right_eye) {

    if (framebuffer.address_right1 == 0 || framebuffer.address_right2 == 0) {
        right_eye = false;
    }

    const PAddr framebuffer_addr =
        framebuffer.active_fb == 0
            ? (right_eye ? framebuffer.address_right1 : framebuffer.address_left1)
            : (right_eye ? framebuffer.address_right2 : framebuffer.address_left2);

    LOG_TRACE(Render_Vulkan, "0x{:08x} bytes from 0x{:08x}({}x{}), fmt {:x}",
              framebuffer.stride * framebuffer.height, framebuffer_addr, framebuffer.width.Value(),
              framebuffer.height.Value(), framebuffer.format);

    const u32 bpp = Pica::BytesPerPixel(framebuffer.color_format);
    const std::size_t pixel_stride = framebuffer.stride / bpp;

    ASSERT(pixel_stride * bpp == framebuffer.stride);
    ASSERT(pixel_stride % 4 == 0);

    if (!rasterizer.AccelerateDisplay(framebuffer, framebuffer_addr, static_cast<u32>(pixel_stride),
                                      screen_info)) {
        // Reset the screen info's display texture to its own permanent texture
        screen_info.image_view = screen_info.texture.image_view;
        screen_info.texcoords = {0.f, 0.f, 1.f, 1.f};

        ASSERT(false);
    }
}

void RendererVulkan::CompileShaders() {
    const vk::Device device = instance.GetDevice();
    const std::string_view preamble =
        instance.IsImageArrayDynamicIndexSupported() ? "#define ARRAY_DYNAMIC_INDEX" : "";
    present_vertex_shader =
        Compile(HostShaders::VULKAN_PRESENT_VERT, vk::ShaderStageFlagBits::eVertex, device);
    present_shaders[0] = Compile(HostShaders::VULKAN_PRESENT_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);
    present_shaders[1] = Compile(HostShaders::VULKAN_PRESENT_ANAGLYPH_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);
    present_shaders[2] = Compile(HostShaders::VULKAN_PRESENT_INTERLACED_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);

    cursor_vertex_shader =
        Compile(HostShaders::VULKAN_CURSOR_VERT, vk::ShaderStageFlagBits::eVertex, device);
    cursor_fragment_shader =
        Compile(HostShaders::VULKAN_CURSOR_FRAG, vk::ShaderStageFlagBits::eFragment, device);

    auto properties = instance.GetPhysicalDevice().getProperties();
    for (std::size_t i = 0; i < present_samplers.size(); i++) {
        const vk::Filter filter_mode = i == 0 ? vk::Filter::eLinear : vk::Filter::eNearest;
        const vk::SamplerCreateInfo sampler_info = {
            .magFilter = filter_mode,
            .minFilter = filter_mode,
            .mipmapMode = vk::SamplerMipmapMode::eLinear,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .anisotropyEnable = instance.IsAnisotropicFilteringSupported(),
            .maxAnisotropy = properties.limits.maxSamplerAnisotropy,
            .compareEnable = false,
            .compareOp = vk::CompareOp::eAlways,
            .borderColor = vk::BorderColor::eIntOpaqueBlack,
            .unnormalizedCoordinates = false,
        };

        present_samplers[i] = device.createSampler(sampler_info);
    }
}

void RendererVulkan::BuildLayouts() {
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(PresentUniformData),
    };

    const auto descriptor_set_layout = present_heap.Layout();
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };
    present_pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(layout_info);

    const vk::PipelineLayoutCreateInfo cursor_layout_info = {};
    cursor_pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(cursor_layout_info);
}

void RendererVulkan::BuildPipelines() {
    const vk::VertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(ScreenRectVertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };

    const std::array attributes = {
        vk::VertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, position),
        },
        vk::VertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, tex_coord),
        },
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleStrip,
        .primitiveRestartEnable = false,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state = {
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = false,
    };

    const vk::PipelineColorBlendAttachmentState colorblend_attachment = {
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eConstantAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eConstantAlpha,
        .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = false,
        .attachmentCount = 1,
        .pAttachments = &colorblend_attachment,
    };

    const vk::Viewport placeholder_viewport = vk::Viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const vk::Rect2D placeholder_scissor = vk::Rect2D{{0, 0}, {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .viewportCount = 1,
        .pViewports = &placeholder_viewport,
        .scissorCount = 1,
        .pScissors = &placeholder_scissor,
    };

    const std::array dynamic_states = {
        vk::DynamicState::eBlendConstants,
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::PipelineDepthStencilStateCreateInfo depth_info = {
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .depthCompareOp = vk::CompareOp::eAlways,
        .depthBoundsTestEnable = false,
        .stencilTestEnable = false,
    };

    for (u32 i = 0; i < PRESENT_PIPELINES; i++) {
        const std::array shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = present_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = present_shaders[i],
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo pipeline_info = {
            .stageCount = static_cast<u32>(shader_stages.size()),
            .pStages = shader_stages.data(),
            .pVertexInputState = &vertex_input_info,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport_info,
            .pRasterizationState = &raster_state,
            .pMultisampleState = &multisampling,
            .pDepthStencilState = &depth_info,
            .pColorBlendState = &color_blending,
            .pDynamicState = &dynamic_info,
            .layout = *present_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build present pipelines");
        present_pipelines[i] = pipeline;
    }

    // Build cursor pipeline (simple position-only, inverted color blending)
    {
        const vk::VertexInputBindingDescription cursor_binding = {
            .binding = 0,
            .stride = sizeof(float) * 2,
            .inputRate = vk::VertexInputRate::eVertex,
        };

        const vk::VertexInputAttributeDescription cursor_attribute = {
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = 0,
        };

        const vk::PipelineVertexInputStateCreateInfo cursor_vertex_input = {
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &cursor_binding,
            .vertexAttributeDescriptionCount = 1,
            .pVertexAttributeDescriptions = &cursor_attribute,
        };

        const vk::PipelineInputAssemblyStateCreateInfo cursor_input_assembly = {
            .topology = vk::PrimitiveTopology::eTriangleList,
            .primitiveRestartEnable = false,
        };

        const vk::PipelineRasterizationStateCreateInfo cursor_raster = {
            .depthClampEnable = false,
            .rasterizerDiscardEnable = false,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eClockwise,
            .depthBiasEnable = false,
            .lineWidth = 1.0f,
        };

        const vk::PipelineMultisampleStateCreateInfo cursor_multisample = {
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
            .sampleShadingEnable = false,
        };

        const vk::PipelineColorBlendAttachmentState cursor_blend_attachment = {
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eOneMinusDstColor,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eZero,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };

        const vk::PipelineColorBlendStateCreateInfo cursor_color_blending = {
            .logicOpEnable = false,
            .attachmentCount = 1,
            .pAttachments = &cursor_blend_attachment,
        };

        const vk::Viewport placeholder_vp = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        const vk::Rect2D placeholder_sc = {{0, 0}, {1, 1}};
        const vk::PipelineViewportStateCreateInfo cursor_viewport = {
            .viewportCount = 1,
            .pViewports = &placeholder_vp,
            .scissorCount = 1,
            .pScissors = &placeholder_sc,
        };

        const std::array cursor_dynamic_states = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
        };

        const vk::PipelineDynamicStateCreateInfo cursor_dynamic = {
            .dynamicStateCount = static_cast<u32>(cursor_dynamic_states.size()),
            .pDynamicStates = cursor_dynamic_states.data(),
        };

        const vk::PipelineDepthStencilStateCreateInfo cursor_depth = {
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .depthCompareOp = vk::CompareOp::eAlways,
            .depthBoundsTestEnable = false,
            .stencilTestEnable = false,
        };

        const std::array cursor_shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = cursor_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = cursor_fragment_shader,
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo cursor_pipeline_info = {
            .stageCount = static_cast<u32>(cursor_shader_stages.size()),
            .pStages = cursor_shader_stages.data(),
            .pVertexInputState = &cursor_vertex_input,
            .pInputAssemblyState = &cursor_input_assembly,
            .pViewportState = &cursor_viewport,
            .pRasterizationState = &cursor_raster,
            .pMultisampleState = &cursor_multisample,
            .pDepthStencilState = &cursor_depth,
            .pColorBlendState = &cursor_color_blending,
            .pDynamicState = &cursor_dynamic,
            .layout = *cursor_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, cursor_pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build cursor pipeline");
        cursor_pipeline = pipeline;
    }
}

void RendererVulkan::ConfigureFramebufferTexture(TextureInfo& texture,
                                                 const Pica::FramebufferConfig& framebuffer) {
    vk::Device device = instance.GetDevice();
    if (texture.image_view) {
        device.destroyImageView(texture.image_view);
    }
    if (texture.image) {
        vmaDestroyImage(instance.GetAllocator(), texture.image, texture.allocation);
    }

    const VideoCore::PixelFormat pixel_format =
        VideoCore::PixelFormatFromGPUPixelFormat(framebuffer.color_format);
    const vk::Format format = instance.GetTraits(pixel_format).native;
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {framebuffer.width, framebuffer.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &texture.allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }
    texture.image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = texture.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    texture.image_view = device.createImageView(view_info);

    texture.width = framebuffer.width;
    texture.height = framebuffer.height;
    texture.format = framebuffer.color_format;
}

void RendererVulkan::FillScreen(Common::Vec3<u8> color, const TextureInfo& texture) {
    const vk::ClearColorValue clear_color = {
        .float32 =
            std::array{
                color.r() / 255.0f,
                color.g() / 255.0f,
                color.b() / 255.0f,
                1.0f,
            },
    };

    renderpass_cache.EndRendering();
    scheduler.Record([image = texture.image, clear_color](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };

        const vk::ImageMemoryBarrier pre_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };

        const vk::ImageMemoryBarrier post_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

        cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, clear_color, range);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eFragmentShader,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);
    });
}

void RendererVulkan::ReloadPipeline(Settings::StereoRenderOption render_3d) {
    switch (render_3d) {
    case Settings::StereoRenderOption::Anaglyph:
        current_pipeline = 1;
        break;
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced:
        current_pipeline = 2;
        draw_info.reverse_interlaced = render_3d == Settings::StereoRenderOption::ReverseInterlaced;
        break;
    default:
        current_pipeline = 0;
        break;
    }
}

void RendererVulkan::DrawSingleScreen(u32 screen_id, float x, float y, float w, float h,
                                      Layout::DisplayOrientation orientation) {
    const ScreenInfo& screen_info = screen_infos[screen_id];
    const auto& texcoords = screen_info.texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    const u32 scale_factor = GetResolutionScaleFactor();
    draw_info.i_resolution =
        Common::MakeVec(static_cast<f32>(screen_info.texture.width * scale_factor),
                        static_cast<f32>(screen_info.texture.height * scale_factor),
                        1.0f / static_cast<f32>(screen_info.texture.width * scale_factor),
                        1.0f / static_cast<f32>(screen_info.texture.height * scale_factor));
    draw_info.o_resolution = Common::MakeVec(h, w, 1.0f / h, 1.0f / w);
    draw_info.screen_id_l = screen_id;

    scheduler.Record([this, offset = offset, info = draw_info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);

        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::DrawSingleScreenStereo(u32 screen_id_l, u32 screen_id_r, float x, float y,
                                            float w, float h,
                                            Layout::DisplayOrientation orientation) {
    const ScreenInfo& screen_info_l = screen_infos[screen_id_l];
    const auto& texcoords = screen_info_l.texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    const u32 scale_factor = GetResolutionScaleFactor();
    draw_info.i_resolution =
        Common::MakeVec(static_cast<f32>(screen_info_l.texture.width * scale_factor),
                        static_cast<f32>(screen_info_l.texture.height * scale_factor),
                        1.0f / static_cast<f32>(screen_info_l.texture.width * scale_factor),
                        1.0f / static_cast<f32>(screen_info_l.texture.height * scale_factor));
    draw_info.o_resolution = Common::MakeVec(h, w, 1.0f / h, 1.0f / w);
    draw_info.screen_id_l = screen_id_l;
    draw_info.screen_id_r = screen_id_r;

    scheduler.Record([this, offset = offset, info = draw_info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);

        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::ApplySecondLayerOpacity(float alpha) {
    scheduler.Record([alpha](vk::CommandBuffer cmdbuf) {
        const std::array<float, 4> blend_constants = {0.0f, 0.0f, 0.0f, alpha};
        cmdbuf.setBlendConstants(blend_constants.data());
    });
}

void RendererVulkan::DrawTopScreen(const Layout::FramebufferLayout& layout,
                                   const Common::Rectangle<u32>& top_screen) {
    if (!layout.top_screen_enabled) {
        return;
    }
    int leftside, rightside;
    leftside = Settings::values.swap_eyes_3d.GetValue() ? 1 : 0;
    rightside = Settings::values.swap_eyes_3d.GetValue() ? 0 : 1;
    const float top_screen_left = static_cast<float>(top_screen.left);
    const float top_screen_top = static_cast<float>(top_screen.top);
    const float top_screen_width = static_cast<float>(top_screen.GetWidth());
    const float top_screen_height = static_cast<float>(top_screen.GetHeight());

    const auto orientation = layout.is_rotated ? Layout::DisplayOrientation::Landscape
                                               : Layout::DisplayOrientation::Portrait;
    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        const int eye = static_cast<int>(Settings::values.mono_render_option.GetValue());
        DrawSingleScreen(eye, top_screen_left, top_screen_top, top_screen_width, top_screen_height,
                         orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySide: {
        DrawSingleScreen(leftside, top_screen_left / 2, top_screen_top, top_screen_width / 2,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(rightside, static_cast<float>((top_screen_left / 2) + (layout.width / 2)),
                         top_screen_top, top_screen_width / 2, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(leftside, top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(rightside, top_screen_left + layout.width / 2, top_screen_top,
                         top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(leftside, top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(
            rightside,
            static_cast<float>(layout.cardboard.top_screen_right_eye + (layout.width / 2)),
            top_screen_top, top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(leftside, rightside, top_screen_left, top_screen_top,
                               top_screen_width, top_screen_height, orientation);
        break;
    }
    }
}

void RendererVulkan::DrawBottomScreen(const Layout::FramebufferLayout& layout,
                                      const Common::Rectangle<u32>& bottom_screen) {
    if (!layout.bottom_screen_enabled) {
        return;
    }

    const float bottom_screen_left = static_cast<float>(bottom_screen.left);
    const float bottom_screen_top = static_cast<float>(bottom_screen.top);
    const float bottom_screen_width = static_cast<float>(bottom_screen.GetWidth());
    const float bottom_screen_height = static_cast<float>(bottom_screen.GetHeight());

    const auto orientation = layout.is_rotated ? Layout::DisplayOrientation::Landscape
                                               : Layout::DisplayOrientation::Portrait;

    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);

        break;
    }
    case Settings::StereoRenderOption::SideBySide: // Bottom screen is identical on both sides
    {
        DrawSingleScreen(2, bottom_screen_left / 2, bottom_screen_top, bottom_screen_width / 2,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(2, static_cast<float>((bottom_screen_left / 2) + (layout.width / 2)),
                         bottom_screen_top, bottom_screen_width / 2, bottom_screen_height,
                         orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(2, bottom_screen_left + layout.width / 2, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(
            2, static_cast<float>(layout.cardboard.bottom_screen_right_eye + (layout.width / 2)),
            bottom_screen_top, bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(2, 2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                               bottom_screen_height, orientation);
        break;
    }
    }
}

void RendererVulkan::DrawScreens(Frame* frame,
                                 const Layout::FramebufferLayout& layout,
                                 bool flipped)
{
    // --- Settings updates ---
    if (settings.bg_color_update_requested.exchange(false)) {
        clear_color.float32[0] = Settings::values.bg_red.GetValue();
        clear_color.float32[1] = Settings::values.bg_green.GetValue();
        clear_color.float32[2] = Settings::values.bg_blue.GetValue();
    }
    if (settings.shader_update_requested.exchange(false)) {
        ReloadPipeline(layout.render_3d_mode);
    }

    // --- Draw ---
    PrepareDraw(frame, layout);

    draw_info.modelview = MakeOrthographicMatrix(layout.width, layout.height);
    draw_info.layer = 0;
    ApplySecondLayerOpacity(1.0f);

    if (!Settings::values.swap_screen.GetValue()) {
        DrawTopScreen(layout, layout.top_screen);
        draw_info.layer = 0;
        if (layout.bottom_opacity < 1.0f)
            ApplySecondLayerOpacity(layout.bottom_opacity);
        DrawBottomScreen(layout, layout.bottom_screen);
    } else {
        DrawBottomScreen(layout, layout.bottom_screen);
        draw_info.layer = 0;
        if (layout.top_opacity < 1.0f)
            ApplySecondLayerOpacity(layout.top_opacity);
        DrawTopScreen(layout, layout.top_screen);
    }

    if (layout.additional_screen_enabled) {
        if (!Settings::values.swap_screen.GetValue())
            DrawTopScreen(layout, layout.additional_screen);
        else
            DrawBottomScreen(layout, layout.additional_screen);
    }

    DrawCursor(layout);

    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.endRenderPass();
    });

    // --- Streaming ---
    TryStreamBottomScreen();
}

// ----------------------------------------------------------------
//  DestroyStreamSlot
//  Distrugge tutte le risorse di uno slot (dopo aver aspettato il tick)
// ----------------------------------------------------------------
void RendererVulkan::DestroyStreamSlot(u32 idx) {
    auto& slot = stream_slots_[idx];

    if (slot.pending) {
        scheduler.Wait(slot.tick);
        slot.pending = false;
    }

    const vk::Device dev = instance.GetDevice();

    if (slot.frame_fb != VK_NULL_HANDLE) {
        dev.destroyFramebuffer(vk::Framebuffer{slot.frame_fb});
        slot.frame_fb = VK_NULL_HANDLE;
    }
    if (slot.frame_view != VK_NULL_HANDLE) {
        dev.destroyImageView(vk::ImageView{slot.frame_view});
        slot.frame_view = VK_NULL_HANDLE;
    }
    if (slot.frame_image != VK_NULL_HANDLE) {
        vmaDestroyImage(instance.GetAllocator(),
                        slot.frame_image, slot.frame_alloc);
        slot.frame_image = VK_NULL_HANDLE;
        slot.frame_alloc = VK_NULL_HANDLE;
    }
    if (slot.buf != VK_NULL_HANDLE) {
        vmaDestroyBuffer(instance.GetAllocator(), slot.buf, slot.buf_alloc);
        slot.buf       = VK_NULL_HANDLE;
        slot.buf_alloc = VK_NULL_HANDLE;
        slot.mapped    = nullptr;
    }
}

// ----------------------------------------------------------------
//  EnsureStreamSlots
//  Crea/ricrea entrambi gli slot se la dimensione cambia
// ----------------------------------------------------------------
void RendererVulkan::EnsureStreamSlots(u64 required_size) {
    if (required_size <= stream_slot_size_)
        return;

    // Distruggi gli slot esistenti
    for (u32 i = 0; i < STREAM_BUF_COUNT; i++)
        DestroyStreamSlot(i);
    stream_slot_size_ = 0;

    const VkBufferCreateInfo buf_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = required_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };

    for (u32 i = 0; i < STREAM_BUF_COUNT; i++) {
        auto& slot = stream_slots_[i];
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buf_ci, &alloc_ci,
                            &slot.buf, &slot.buf_alloc, &info) != VK_SUCCESS) {
            std::cerr << "[Stream] ERRORE: vmaCreateBuffer fallito per slot " << i << "\n";
            return;
        }
        slot.mapped  = info.pMappedData;
        slot.pending = false;
        slot.tick    = 0;
    }

    stream_slot_size_ = required_size;
    std::cerr << "[Stream] Allocati " << STREAM_BUF_COUNT
              << " slot da " << required_size << " bytes\n";
}

// ----------------------------------------------------------------
//  TryStreamBottomScreen
//  Chiamata ogni frame da DrawScreens
// ----------------------------------------------------------------
void RendererVulkan::TryStreamBottomScreen() {
    if (!screen_streamer || !screen_streamer->IsDirectMode())
        return;

    stream_frame_counter_++;
    if ((stream_frame_counter_ % 2) != 0)
        return;

    // Dimensioni native bottom screen 3DS
    constexpr u32 SW = 240;  // larghezza del frame = larghezza portrait
    constexpr u32 SH = 320;  // altezza del frame = altezza portrait
    constexpr u64 required = static_cast<u64>(SW) * SH * 4;

    EnsureStreamSlots(required);
    if (stream_slot_size_ == 0)
        return;

    const u32 write_idx = stream_write_idx_;
    const u32 read_idx  = (stream_write_idx_ + STREAM_BUF_COUNT - 1) % STREAM_BUF_COUNT;

    auto& write_slot = stream_slots_[write_idx];
    auto& read_slot  = stream_slots_[read_idx];

    // --- Leggi lo slot precedente (read) ---
    // Aspetta che la GPU abbia finito di scrivere su di esso
    if (read_slot.pending) {
        scheduler.Wait(read_slot.tick);
        read_slot.pending = false;

        // Ora è sicuro leggere dalla CPU
        vmaInvalidateAllocation(instance.GetAllocator(),
                                read_slot.buf_alloc, 0, VK_WHOLE_SIZE);
        screen_streamer->sendFrame(read_slot.mapped, SW, SH);
    }

    // --- Distruggi il frame del write_slot del ciclo precedente ---
    // (la GPU ha finito con esso — era il read_slot due cicli fa,
    //  e abbiamo già aspettato il suo tick)
    const vk::Device dev = instance.GetDevice();
    if (write_slot.frame_fb != VK_NULL_HANDLE) {
        dev.destroyFramebuffer(vk::Framebuffer{write_slot.frame_fb});
        write_slot.frame_fb = VK_NULL_HANDLE;
    }
    if (write_slot.frame_view != VK_NULL_HANDLE) {
        dev.destroyImageView(vk::ImageView{write_slot.frame_view});
        write_slot.frame_view = VK_NULL_HANDLE;
    }
    if (write_slot.frame_image != VK_NULL_HANDLE) {
        vmaDestroyImage(instance.GetAllocator(),
                        write_slot.frame_image, write_slot.frame_alloc);
        write_slot.frame_image = VK_NULL_HANDLE;
        write_slot.frame_alloc = VK_NULL_HANDLE;
    }

    // --- Crea il frame dedicato 320x240 per questo ciclo ---
    Frame sf{};
    main_present_window.RecreateFrame(&sf, SW, SH);

    // Setup descriptor
    const auto sampler     = present_samplers[!Settings::values.filter_mode.GetValue()];
    const auto present_set = present_heap.Commit();
    for (u32 i = 0; i < screen_infos.size(); i++)
        update_queue.AddImageSampler(present_set, 0, i,
                                     screen_infos[i].image_view, sampler);

    // --- Renderpass sul frame dedicato ---
    renderpass_cache.EndRendering();
    scheduler.Record([this, sf_fb   = sf.framebuffer,
                            sf_w    = sf.width,
                            sf_h    = sf.height,
                            present_set,
                            renderpass = main_present_window.Renderpass(),
                            idx        = current_pipeline](vk::CommandBuffer cmdbuf)
    {
        const vk::Viewport vp = {
            .x = 0, .y = 0,
            .width = (float)sf_w, .height = (float)sf_h,
            .minDepth = 0, .maxDepth = 1,
        };
        const vk::Rect2D sc = {{0, 0}, {sf_w, sf_h}};
        cmdbuf.setViewport(0, vp);
        cmdbuf.setScissor(0, sc);

        const vk::ClearValue clear{.color = clear_color};
        const vk::RenderPassBeginInfo rp = {
            .renderPass      = renderpass,
            .framebuffer     = sf_fb,
            .renderArea      = {{0, 0}, {sf_w, sf_h}},
            .clearValueCount = 1,
            .pClearValues    = &clear,
        };
        cmdbuf.beginRenderPass(rp, vk::SubpassContents::eInline);
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, present_pipelines[idx]);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                  *present_pipeline_layout, 0, present_set, {});
    });

    // Disegna screen_infos[2] = bottom screen, riempie tutto il frame
    draw_info.modelview = MakeOrthographicMatrix(SW, SH);
    draw_info.layer     = 0;
    ApplySecondLayerOpacity(1.0f);
    DrawSingleScreen(2, 0.0f, 0.0f, (float)SW, (float)SH,
                 Layout::DisplayOrientation::Portrait);

    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.endRenderPass();
    });

    // --- Copia frame -> staging buffer ---
    scheduler.Record([src = sf.image,
                      buf = write_slot.buf](vk::CommandBuffer cmdbuf)
    {
        const vk::ImageMemoryBarrier to_src = {
            .srcAccessMask       = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask       = vk::AccessFlagBits::eTransferRead,
            .oldLayout           = vk::ImageLayout::eGeneral,
            .newLayout           = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = src,
            .subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
            vk::PipelineStageFlagBits::eTransfer,
            {}, {}, {}, to_src);

        cmdbuf.copyImageToBuffer(
            src, vk::ImageLayout::eTransferSrcOptimal,
            vk::Buffer{buf},
            vk::BufferImageCopy{
                .bufferRowLength   = 0,
                .bufferImageHeight = 0,
                .imageSubresource  = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageOffset       = {0, 0, 0},
                .imageExtent       = {SW, SH, 1},
            });
    });

    // Flush asincrono — la GPU continua, noi no aspettiamo
    const u64 tick = scheduler.CurrentTick();
    scheduler.Flush();
    write_slot.tick    = tick;
    write_slot.pending = true;

    // Salva il frame nello slot — verrà distrutto al prossimo ciclo
    write_slot.frame_image = sf.image;
    write_slot.frame_alloc = sf.allocation;
    write_slot.frame_view  = sf.image_view;
    write_slot.frame_fb    = sf.framebuffer;

    // Avanza al prossimo slot
    stream_write_idx_ = (stream_write_idx_ + 1) % STREAM_BUF_COUNT;
}

void RendererVulkan::DrawCursor(const Layout::FramebufferLayout& layout) {
    const auto cursor = render_window.GetCursorInfo();
    if (!cursor.visible) {
        return;
    }

    const float buf_w = static_cast<float>(layout.width);
    const float buf_h = static_cast<float>(layout.height);

    // Convert from bottom-screen-local to layout-absolute, then to NDC
    const float abs_x = layout.bottom_screen.left + cursor.projected_x;
    const float abs_y = layout.bottom_screen.top + cursor.projected_y;
    const float cx = (abs_x / buf_w) * 2.0f - 1.0f;
    const float cy = (abs_y / buf_h) * 2.0f - 1.0f;
    const float ratio = static_cast<float>(layout.bottom_screen.GetHeight()) / 30.0f;
    const float rw = ratio / buf_w;
    const float rh = ratio / buf_h;

    // Bottom screen bounds in NDC
    const float bl = (layout.bottom_screen.left / buf_w) * 2.0f - 1.0f;
    const float bt = (layout.bottom_screen.top / buf_h) * 2.0f - 1.0f;
    const float br = (layout.bottom_screen.right / buf_w) * 2.0f - 1.0f;
    const float bb = (layout.bottom_screen.bottom / buf_h) * 2.0f - 1.0f;

    // Crosshair geometry clamped to bottom screen bounds
    const float vl = std::fmax(cx - rw / 5.0f, bl);
    const float vr = std::fmin(cx + rw / 5.0f, br);
    const float vt = std::fmax(cy - rh, bt);
    const float vb = std::fmin(cy + rh, bb);

    const float hl = std::fmax(cx - rw, bl);
    const float hr = std::fmin(cx + rw, br);
    const float ht = std::fmax(cy - rh / 5.0f, bt);
    const float hb = std::fmin(cy + rh / 5.0f, bb);

    // 12 vertices = 4 triangles (2 for vertical bar, 2 for horizontal bar)
    // clang-format off
    const float vertices[] = {
        // Vertical bar
        vl, vt,  vr, vt,  vr, vb,
        vl, vt,  vr, vb,  vl, vb,
        // Horizontal bar
        hl, ht,  hr, ht,  hr, hb,
        hl, ht,  hr, hb,  hl, hb,
    };
    // clang-format on

    const u64 size = sizeof(vertices);
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices, size);
    vertex_buffer.Commit(size);

    scheduler.Record([this, offset = offset, pipeline = cursor_pipeline](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        const u32 first_vertex = static_cast<u32>(offset) / (sizeof(float) * 2);
        cmdbuf.draw(12, 1, first_vertex, 0);
    });
}

void RendererVulkan::SwapBuffers() {
    system.perf_stats->StartSwap();
    const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();
    PrepareRendertarget();

    RenderScreenshot();
    RenderToWindow(main_present_window, layout, false);
#ifndef ANDROID
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        if (!secondary_present_window_ptr) {
            secondary_present_window_ptr = std::make_unique<PresentWindow>(
                *secondary_window, instance, scheduler, IsLowRefreshRate());
        }
        RenderToWindow(*secondary_present_window_ptr, secondary_layout, false);
        secondary_window->PollEvents();
    }
#endif

#ifdef ANDROID
    if (secondary_window) {
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        if (!secondary_present_window_ptr) {
            secondary_present_window_ptr = std::make_unique<PresentWindow>(
                *secondary_window, instance, scheduler, IsLowRefreshRate());
        }
        RenderToWindow(*secondary_present_window_ptr, secondary_layout, false);
        secondary_window->PollEvents();
    }
#endif

    system.perf_stats->EndSwap();
    rasterizer.TickFrame();
    EndFrame();
}

void RendererVulkan::RenderScreenshot() {
    if (!settings.screenshot_requested.exchange(false)) {
        return;
    }

    if (!TryRenderScreenshotWithHostMemory()) {
        RenderScreenshotWithStagingCopy();
    }

    settings.screenshot_complete_callback(false);
}

void RendererVulkan::RenderScreenshotWithStagingCopy() {
    const vk::Device device = instance.GetDevice();

    const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};
    const u32 width = layout.width;
    const u32 height = layout.height;

    const vk::BufferCreateInfo staging_buffer_info = {
        .size = width * height * 4,
        .usage = vk::BufferUsageFlagBits::eTransferDst,
    };

    const VmaAllocationCreateInfo alloc_create_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkBuffer unsafe_buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo alloc_info;
    VkBufferCreateInfo unsafe_buffer_info = static_cast<VkBufferCreateInfo>(staging_buffer_info);

    VkResult result = vmaCreateBuffer(instance.GetAllocator(), &unsafe_buffer_info,
                                      &alloc_create_info, &unsafe_buffer, &allocation, &alloc_info);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }

    vk::Buffer staging_buffer{unsafe_buffer};

    Frame frame{};
    main_present_window.RecreateFrame(&frame, width, height);

    DrawScreens(&frame, layout, false);

    scheduler.Record(
        [width, height, source_image = frame.image, staging_buffer](vk::CommandBuffer cmdbuf) {
            const vk::ImageMemoryBarrier read_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            const vk::ImageMemoryBarrier write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            static constexpr vk::MemoryBarrier memory_write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            };

            const vk::BufferImageCopy image_copy = {
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource =
                    {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .imageOffset = {0, 0, 0},
                .imageExtent = {width, height, 1},
            };

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);
            cmdbuf.copyImageToBuffer(source_image, vk::ImageLayout::eTransferSrcOptimal,
                                     staging_buffer, image_copy);
            cmdbuf.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
                vk::DependencyFlagBits::eByRegion, memory_write_barrier, {}, write_barrier);
        });

    // Ensure the copy is fully completed before saving the screenshot
    scheduler.Finish();

    // Copy backing image data to the QImage screenshot buffer
    std::memcpy(settings.screenshot_bits, alloc_info.pMappedData, staging_buffer_info.size);

    // Destroy allocated resources
    vmaDestroyBuffer(instance.GetAllocator(), staging_buffer, allocation);
    vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
    device.destroyFramebuffer(frame.framebuffer);
    device.destroyImageView(frame.image_view);
}

bool RendererVulkan::TryRenderScreenshotWithHostMemory() {
    // If the host-memory import alignment matches the allocation granularity of the platform, then
    // the entire span of memory can be trivially imported
    const bool trivial_import =
        instance.IsExternalMemoryHostSupported() &&
        instance.GetMinImportedHostPointerAlignment() == Common::GetPageSize();
    if (!trivial_import) {
        return false;
    }

    const vk::Device device = instance.GetDevice();

    const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};
    const u32 width = layout.width;
    const u32 height = layout.height;

    // For a span of memory [x, x + s], import [AlignDown(x, alignment), AlignUp(x + s, alignment)]
    // and maintain an offset to the start of the data
    const u64 import_alignment = instance.GetMinImportedHostPointerAlignment();
    const uintptr_t address = reinterpret_cast<uintptr_t>(settings.screenshot_bits);
    void* aligned_pointer = reinterpret_cast<void*>(Common::AlignDown(address, import_alignment));
    const u64 offset = address % import_alignment;
    const u64 aligned_size = Common::AlignUp(offset + width * height * 4ull, import_alignment);

    // Buffer<->Image mapping for the imported imported buffer
    const vk::BufferImageCopy buffer_image_copy = {
        .bufferOffset = offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };

    const vk::MemoryHostPointerPropertiesEXT import_properties =
        device.getMemoryHostPointerPropertiesEXT(
            vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, aligned_pointer);

    if (!import_properties.memoryTypeBits) {
        // Could not import memory
        return false;
    }

    const std::optional<u32> memory_type_index = FindMemoryType(
        instance.GetPhysicalDevice().getMemoryProperties(),
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        import_properties.memoryTypeBits);

    if (!memory_type_index.has_value()) {
        // Could not find memory type index
        return false;
    }

    const vk::StructureChain<vk::MemoryAllocateInfo, vk::ImportMemoryHostPointerInfoEXT>
        allocation_chain = {
            vk::MemoryAllocateInfo{
                .allocationSize = aligned_size,
                .memoryTypeIndex = memory_type_index.value(),
            },
            vk::ImportMemoryHostPointerInfoEXT{
                .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
                .pHostPointer = aligned_pointer,
            },
        };

    // Import host memory
    const vk::UniqueDeviceMemory imported_memory =
        device.allocateMemoryUnique(allocation_chain.get());

    const vk::StructureChain<vk::BufferCreateInfo, vk::ExternalMemoryBufferCreateInfo> buffer_info =
        {
            vk::BufferCreateInfo{
                .size = aligned_size,
                .usage = vk::BufferUsageFlagBits::eTransferDst,
                .sharingMode = vk::SharingMode::eExclusive,
            },
            vk::ExternalMemoryBufferCreateInfo{
                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
            },
        };

    // Bind imported memory to buffer
    const vk::UniqueBuffer imported_buffer = device.createBufferUnique(buffer_info.get());
    device.bindBufferMemory(imported_buffer.get(), imported_memory.get(), 0);

    Frame frame{};
    main_present_window.RecreateFrame(&frame, width, height);

    DrawScreens(&frame, layout, false);

    scheduler.Record([buffer_image_copy, source_image = frame.image,
                      imported_buffer = imported_buffer.get()](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier read_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        const vk::ImageMemoryBarrier write_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        static constexpr vk::MemoryBarrier memory_write_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);
        cmdbuf.copyImageToBuffer(source_image, vk::ImageLayout::eTransferSrcOptimal,
                                 imported_buffer, buffer_image_copy);
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
            vk::DependencyFlagBits::eByRegion, memory_write_barrier, {}, write_barrier);
    });

    // Ensure the copy is fully completed before saving the screenshot
    scheduler.Finish();

    // Image data has been copied directly to host memory
    device.destroyFramebuffer(frame.framebuffer);
    device.destroyImageView(frame.image_view);

    return true;
}

void RendererVulkan::NotifySurfaceChanged(bool is_second_window) {
    if (is_second_window) {
        if (secondary_present_window_ptr) {
            secondary_present_window_ptr->NotifySurfaceChanged();
        }
    } else {
        main_present_window.NotifySurfaceChanged();
    }
}

} // namespace Vulkan