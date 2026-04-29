#ifdef __APPLE__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>
#import <Foundation/Foundation.h>

#include "renderer_metal.h"
#include "metal_device.h"
#include "metal_texture_mailbox.h"
#include "common/settings.h"
#include "common/logging/log.h"
#include "video_core/pica/pica_core.h"
#include "core/frontend/emu_window.h"
#include "video_core/pica/pica_core.h"
#include "core/core.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Metal {

// ---------------------------------------------------------------
// Strutture dati
// ---------------------------------------------------------------

struct ScreenRectVertex {
    simd::float2 position;
    simd::float2 tex_coord;
};

struct PresentUniforms {
    simd::float3x2 modelview;
    simd::float4   i_resolution;
    simd::float4   o_resolution;
    int            layer;
    float          opacity;
    int            reverse_interlaced;
    float          _pad;
};

// ---------------------------------------------------------------
// MetalRendererImpl — tutto lo stato Metal qui, .mm only
// ---------------------------------------------------------------

struct MetalRendererImpl {
    id<MTLDevice>              device        = nil;
    id<MTLCommandQueue>        command_queue = nil;
    CAMetalLayer*              metal_layer   = nil;
    CAMetalLayer*              secondary_layer = nil;

    static constexpr int PIPELINE_FLAT       = 0;
    static constexpr int PIPELINE_ANAGLYPH   = 1;
    static constexpr int PIPELINE_INTERLACED = 2;

    id<MTLRenderPipelineState> present_pipelines[3] = {};
    id<MTLSamplerState>        samplers[2]           = {}; // 0=nearest 1=linear
    id<MTLBuffer>              vertex_buffer         = nil;
    id<MTLBuffer>              uniform_buffer        = nil;
    NSUInteger                 uniform_offset        = 0;

    struct ScreenInfo {
        id<MTLTexture>           texture     = nil;
        id<MTLTexture>           display_tex = nil;
        Common::Rectangle<float> texcoords{0.f, 0.f, 1.f, 1.f};
        u32 width = 0, height = 0;
    };
    std::array<ScreenInfo, 3> screen_infos{};

    static constexpr u32 STREAM_BUF_COUNT = 2;
    struct StreamSlot {
        id<MTLBuffer>  pbo        = nil;
        id<MTLTexture> render_tex = nil;
        bool           pending    = false;
    };
    std::array<StreamSlot, STREAM_BUF_COUNT> stream_slots{};
    u32  stream_write_idx     = 0;
    u32  stream_frame_counter = 0;
    u32  stream_tex_w         = 0;
    u32  stream_tex_h         = 0;
};

// ---------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------

static simd::float3x2 MakeOrtho(float w, float h, bool flipped) {
    if (flipped) {
        return simd::float3x2{
            simd::float2{ 2.f / w,  0.f     },
            simd::float2{ 0.f,      2.f / h },
            simd::float2{-1.f,     -1.f     }
        };
    }
    return simd::float3x2{
        simd::float2{ 2.f / w,  0.f      },
        simd::float2{ 0.f,     -2.f / h  },
        simd::float2{-1.f,      1.f      }
    };
}

// ---------------------------------------------------------------
// Shader MSL inline
// ---------------------------------------------------------------

static const char* METAL_SHADER_SRC = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 position  [[attribute(0)]];
    float2 tex_coord [[attribute(1)]];
};
struct VertexOut {
    float4 position [[position]];
    float2 tex_coord;
};
struct PresentUniforms {
    float3x2 modelview;
    float4   i_resolution;
    float4   o_resolution;
    int      layer;
    float    opacity;
    int      reverse_interlaced;
    float    pad;
};

vertex VertexOut present_vertex(VertexIn in [[stage_in]],
                                constant PresentUniforms& u [[buffer(1)]])
{
    VertexOut out;
    float2 p     = u.modelview * float3(in.position, 1.0);
    out.position  = float4(p.x, p.y, 0.0, 1.0);
    out.tex_coord = in.tex_coord;
    return out;
}

fragment float4 present_fragment_flat(
    VertexOut in [[stage_in]],
    texture2d<float> tex [[texture(0)]],
    sampler smp          [[sampler(0)]],
    constant PresentUniforms& u [[buffer(1)]])
{
    float4 color = tex.sample(smp, in.tex_coord);
    color.a *= u.opacity;
    return color;
}

fragment float4 present_fragment_anaglyph(
    VertexOut in [[stage_in]],
    texture2d<float> tex_l [[texture(0)]],
    texture2d<float> tex_r [[texture(1)]],
    sampler smp            [[sampler(0)]])
{
    float4 l = tex_l.sample(smp, in.tex_coord);
    float4 r = tex_r.sample(smp, in.tex_coord);
    return float4(
        dot(l.rgb, float3(0.4561f,  0.500484f,  0.176381f)),
        dot(r.rgb, float3(-0.0434706f, -0.0879388f, 0.00155529f)),
        dot(r.rgb, float3(-0.0103f, -0.0901f, 0.725123f)),
        1.0f
    );
}

fragment float4 present_fragment_interlaced(
    VertexOut in [[stage_in]],
    texture2d<float> tex_l [[texture(0)]],
    texture2d<float> tex_r [[texture(1)]],
    sampler smp            [[sampler(0)]],
    constant PresentUniforms& u [[buffer(1)]])
{
    uint row = uint(in.position.y);
    bool use_right = (row & 1u) == (u.reverse_interlaced ? 0u : 1u);
    return use_right ? tex_r.sample(smp, in.tex_coord)
                     : tex_l.sample(smp, in.tex_coord);
}
)MSL";

// ---------------------------------------------------------------
// DrawSingleScreen — funzione libera, usa MetalRendererImpl direttamente
// ---------------------------------------------------------------

static void DrawSingleScreen(MetalRendererImpl* p,
                             id<MTLRenderCommandEncoder> enc,
                             const MetalRendererImpl::ScreenInfo& info,
                             float x, float y, float w, float h,
                             Layout::DisplayOrientation orient)
{
    if (!info.display_tex) return;

    const auto& tc = info.texcoords;
    std::array<ScreenRectVertex, 4> verts;

    switch (orient) {
    case Layout::DisplayOrientation::Landscape:
        verts = {{
            {{x,     y    }, {tc.bottom, tc.left }},
            {{x + w, y    }, {tc.bottom, tc.right}},
            {{x,     y + h}, {tc.top,    tc.left }},
            {{x + w, y + h}, {tc.top,    tc.right}},
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        verts = {{
            {{x,     y    }, {tc.bottom, tc.right}},
            {{x + w, y    }, {tc.top,    tc.right}},
            {{x,     y + h}, {tc.bottom, tc.left }},
            {{x + w, y + h}, {tc.top,    tc.left }},
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        verts = {{
            {{x,     y    }, {tc.top,    tc.right}},
            {{x + w, y    }, {tc.top,    tc.left }},
            {{x,     y + h}, {tc.bottom, tc.right}},
            {{x + w, y + h}, {tc.bottom, tc.left }},
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        verts = {{
            {{x,     y    }, {tc.top,    tc.left }},
            {{x + w, y    }, {tc.bottom, tc.left }},
            {{x,     y + h}, {tc.top,    tc.right}},
            {{x + w, y + h}, {tc.bottom, tc.right}},
        }};
        std::swap(h, w);
        break;
    default:
        return;
    }

    NSUInteger voff = p->uniform_offset; // riuso offset come ring index semplificato
    memcpy((char*)p->vertex_buffer.contents, verts.data(), sizeof(verts));

    auto* uni = (PresentUniforms*)((char*)p->uniform_buffer.contents + p->uniform_offset);
    uni->i_resolution = {(float)info.width, (float)info.height,
                         info.width  > 0 ? 1.f / (float)info.width  : 0.f,
                         info.height > 0 ? 1.f / (float)info.height : 0.f};
    uni->o_resolution = {h, w,
                         h > 0.f ? 1.f / h : 0.f,
                         w > 0.f ? 1.f / w : 0.f};

    [enc setVertexBuffer:p->vertex_buffer   offset:0                  atIndex:0];
    [enc setVertexBuffer:p->uniform_buffer  offset:p->uniform_offset  atIndex:1];
    [enc setFragmentBuffer:p->uniform_buffer offset:p->uniform_offset atIndex:1];

    const int si = Settings::values.filter_mode.GetValue() ? 1 : 0;
    [enc setFragmentTexture:info.display_tex atIndex:0];
    [enc setFragmentSamplerState:p->samplers[si] atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];

    p->uniform_offset += sizeof(PresentUniforms);
}

// ---------------------------------------------------------------
// DrawScreens
// ---------------------------------------------------------------

static void DrawScreens(MetalRendererImpl* p,
                        id<MTLCommandBuffer> cmd,
                        id<MTLTexture> target,
                        const Layout::FramebufferLayout& layout,
                        bool flipped)
{
    p->uniform_offset = 0;

    const auto& bg = Settings::values;
    MTLClearColor clear = MTLClearColorMake(bg.bg_red.GetValue(),
                                            bg.bg_green.GetValue(),
                                            bg.bg_blue.GetValue(), 1.0);

    MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor new];
    rpd.colorAttachments[0].texture     = target;
    rpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor  = clear;

    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rpd];

    const int pipeline_idx =
        layout.render_3d_mode == Settings::StereoRenderOption::Anaglyph ? 1 :
        (layout.render_3d_mode == Settings::StereoRenderOption::Interlaced ||
         layout.render_3d_mode == Settings::StereoRenderOption::ReverseInterlaced) ? 2 : 0;
    [enc setRenderPipelineState:p->present_pipelines[pipeline_idx]];

    MTLViewport vp{0, 0, (double)layout.width, (double)layout.height, 0, 1};
    [enc setViewport:vp];

    // Uniform globali
    auto* uni = (PresentUniforms*)p->uniform_buffer.contents;
    uni->modelview = MakeOrtho((float)layout.width, (float)layout.height, flipped);
    uni->layer     = 0;
    uni->opacity   = 1.0f;
    uni->reverse_interlaced =
        layout.render_3d_mode == Settings::StereoRenderOption::ReverseInterlaced ? 1 : 0;

    const auto orientation = layout.is_rotated
        ? Layout::DisplayOrientation::Landscape
        : Layout::DisplayOrientation::Portrait;

    const bool swap = Settings::values.swap_screen.GetValue();
    auto draw_top = [&]() {
        if (layout.top_screen_enabled)
            DrawSingleScreen(p, enc, p->screen_infos[0],
                             (float)layout.top_screen.left,
                             (float)layout.top_screen.top,
                             (float)layout.top_screen.GetWidth(),
                             (float)layout.top_screen.GetHeight(),
                             orientation);
    };
    auto draw_bottom = [&]() {
        if (layout.bottom_screen_enabled)
            DrawSingleScreen(p, enc, p->screen_infos[2],
                             (float)layout.bottom_screen.left,
                             (float)layout.bottom_screen.top,
                             (float)layout.bottom_screen.GetWidth(),
                             (float)layout.bottom_screen.GetHeight(),
                             orientation);
    };

    if (!swap) { draw_top(); draw_bottom(); }
    else        { draw_bottom(); draw_top(); }

    [enc endEncoding];
}

// ---------------------------------------------------------------
// TryStreamBottomScreen
// ---------------------------------------------------------------

static void TryStreamBottomScreen(MetalRendererImpl* p,
                                  Core::System& system,
                                  id<MTLCommandBuffer> cmd)
{
    auto* streamer = system.GetScreenStreamer();
    if (!streamer || !streamer->IsDirectMode()) return;
    if ((++p->stream_frame_counter % 2) != 0) return;

    constexpr u32 SW = 320, SH = 240;

    const u32 write_idx = p->stream_write_idx;
    const u32 read_idx  = (write_idx + MetalRendererImpl::STREAM_BUF_COUNT - 1)
                          % MetalRendererImpl::STREAM_BUF_COUNT;

    auto& ws = p->stream_slots[write_idx];
    auto& rs = p->stream_slots[read_idx];

    if (rs.pending && rs.pbo) {
        streamer->sendFrame(rs.pbo.contents, SW, SH);
        rs.pending = false;
    }

    if (p->stream_tex_w != SW || p->stream_tex_h != SH || ws.render_tex == nil) {
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:SW height:SH mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        ws.render_tex = [p->device newTextureWithDescriptor:td];
        ws.pbo = [p->device newBufferWithLength:SW * SH * 4
                                        options:MTLResourceStorageModeShared];
        p->stream_tex_w = SW;
        p->stream_tex_h = SH;
    }

    MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor new];
    rpd.colorAttachments[0].texture     = ws.render_tex;
    rpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1);

    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rpd];
    [enc setRenderPipelineState:p->present_pipelines[MetalRendererImpl::PIPELINE_FLAT]];
    [enc setViewport:(MTLViewport){0, 0, SW, SH, 0, 1}];

    auto* uni = (PresentUniforms*)p->uniform_buffer.contents;
    uni->modelview = MakeOrtho(SW, SH, false);
    uni->opacity   = 1.0f;
    uni->layer     = 0;

    std::array<ScreenRectVertex, 4> verts = {{
        {{0.f,  0.f },             {0.f, 0.f}},
        {{(float)SW, 0.f },        {1.f, 0.f}},
        {{0.f,  (float)SH},        {0.f, 1.f}},
        {{(float)SW, (float)SH},   {1.f, 1.f}},
    }};
    memcpy(p->vertex_buffer.contents, verts.data(), sizeof(verts));

    [enc setVertexBuffer:p->vertex_buffer  offset:0 atIndex:0];
    [enc setVertexBuffer:p->uniform_buffer offset:0 atIndex:1];
    [enc setFragmentBuffer:p->uniform_buffer offset:0 atIndex:1];
    [enc setFragmentTexture:p->screen_infos[2].display_tex atIndex:0];
    [enc setFragmentSamplerState:p->samplers[0] atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];

    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit copyFromTexture:ws.render_tex
             sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(SW, SH, 1)
                toBuffer:ws.pbo
       destinationOffset:0
  destinationBytesPerRow:SW * 4
destinationBytesPerImage:SW * SH * 4];
    [blit endEncoding];

    ws.pending = true;
    p->stream_write_idx = (p->stream_write_idx + 1) % MetalRendererImpl::STREAM_BUF_COUNT;
}

// ---------------------------------------------------------------
// Costruttore
// ---------------------------------------------------------------

static void BuildPipelines(MetalRendererImpl* p) {
    NSError* err = nil;
    NSString* src = [NSString stringWithUTF8String:METAL_SHADER_SRC];
    id<MTLLibrary> lib = [p->device newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        LOG_CRITICAL(Render, "Metal shader compile error: {}",
                     [[err localizedDescription] UTF8String]);
        return;
    }

    MTLVertexDescriptor* vd = [MTLVertexDescriptor new];
    vd.attributes[0].format      = MTLVertexFormatFloat2;
    vd.attributes[0].offset      = offsetof(ScreenRectVertex, position);
    vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format      = MTLVertexFormatFloat2;
    vd.attributes[1].offset      = offsetof(ScreenRectVertex, tex_coord);
    vd.attributes[1].bufferIndex = 0;
    vd.layouts[0].stride         = sizeof(ScreenRectVertex);

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction  = [lib newFunctionWithName:@"present_vertex"];
    pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat                 = MTLPixelFormatBGRA8Unorm;
    pd.colorAttachments[0].blendingEnabled             = YES;
    pd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
    pd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
    pd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
    pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;

    pd.fragmentFunction = [lib newFunctionWithName:@"present_fragment_flat"];
    p->present_pipelines[0] = [p->device newRenderPipelineStateWithDescriptor:pd error:&err];

    pd.fragmentFunction = [lib newFunctionWithName:@"present_fragment_anaglyph"];
    p->present_pipelines[1] = [p->device newRenderPipelineStateWithDescriptor:pd error:&err];

    pd.fragmentFunction = [lib newFunctionWithName:@"present_fragment_interlaced"];
    p->present_pipelines[2] = [p->device newRenderPipelineStateWithDescriptor:pd error:&err];
}

RendererMetal::RendererMetal(Core::System& system_, Pica::PicaCore& pica_,
                             Frontend::EmuWindow& window,
                             Frontend::EmuWindow* secondary_window)
    : VideoCore::RendererBase{system_, window, secondary_window},
      pica{pica_},
      impl{std::make_unique<MetalRendererImpl>()},
      vk_instance{window, Settings::values.physical_device.GetValue()},  // ← init Vulkan
      vk_scheduler{vk_instance}
{
    // Init Metal per la presentazione
    const auto& winfo = window.GetWindowInfo();
    impl->device        = MTLCreateSystemDefaultDevice();
    impl->command_queue = [impl->device newCommandQueue];
    impl->metal_layer   = (__bridge CAMetalLayer*)winfo.render_surface;
    impl->metal_layer.device      = impl->device;
    impl->metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;

    BuildPipelines(impl.get());

    // Sampler nearest
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter    = MTLSamplerMinMagFilterNearest;
    sd.magFilter    = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    impl->samplers[0] = [impl->device newSamplerStateWithDescriptor:sd];

    // Sampler linear
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    impl->samplers[1] = [impl->device newSamplerStateWithDescriptor:sd];

    impl->vertex_buffer =
        [impl->device newBufferWithLength:sizeof(ScreenRectVertex) * 4 * 32
                                  options:MTLResourceStorageModeShared];
    impl->uniform_buffer =
        [impl->device newBufferWithLength:sizeof(PresentUniforms) * 64
                                  options:MTLResourceStorageModeShared];

    LOG_INFO(Render, "Metal renderer inizializzato: {}", [[impl->device name] UTF8String]);
}

RendererMetal::~RendererMetal() = default;

// ---------------------------------------------------------------
// SwapBuffers
// ---------------------------------------------------------------

void RendererMetal::SwapBuffers() {
    system.perf_stats->StartSwap();

    id<CAMetalDrawable> drawable = [impl->metal_layer nextDrawable];
    if (!drawable) {
        system.perf_stats->EndSwap();
        return;
    }

    id<MTLCommandBuffer> cmd = [impl->command_queue commandBuffer];
    const auto& layout = render_window.GetFramebufferLayout();

    DrawScreens(impl.get(), cmd, drawable.texture, layout, false);
    TryStreamBottomScreen(impl.get(), system, cmd);

    [cmd presentDrawable:drawable];
    [cmd commit];

    system.perf_stats->EndSwap();
    EndFrame();
}

// ---------------------------------------------------------------
// Stub
// ---------------------------------------------------------------

void RendererMetal::TryPresent(int timeout_ms, bool is_secondary) {}
void RendererMetal::PrepareVideoDumping() {}
void RendererMetal::CleanupVideoDumping() {}

} // namespace Metal
#endif // __APPLE__