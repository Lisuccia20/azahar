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
};

vertex VertexOut present_vertex(VertexIn in [[stage_in]],
                                constant PresentUniforms& u [[buffer(1)]])
{
    VertexOut out;
    float2 p = u.modelview * float3(in.position, 1.0);
    out.position  = float4(p, 0.0, 1.0);
    out.tex_coord = in.tex_coord;
    return out;
}

fragment float4 present_fragment_flat(VertexOut in [[stage_in]],
                                      texture2d<float> tex [[texture(0)]],
                                      sampler smp          [[sampler(0)]],
                                      constant PresentUniforms& u [[buffer(1)]])
{
    float4 color = tex.sample(smp, in.tex_coord);
    color.a *= u.opacity;
    return color;
}

fragment float4 present_fragment_anaglyph(VertexOut in [[stage_in]],
                                          texture2d<float> tex_l [[texture(0)]],
                                          texture2d<float> tex_r [[texture(1)]],
                                          sampler smp            [[sampler(0)]])
{
    float4 l = tex_l.sample(smp, in.tex_coord);
    float4 r = tex_r.sample(smp, in.tex_coord);
    // Dubois anaglyph (red-cyan)
    return float4(
        dot(l.rgb, float3(0.4561, 0.500484, 0.176381)),
        dot(r.rgb, float3(-0.0434706, -0.0879388, 0.00155529)),
        dot(r.rgb, float3(-0.0103, -0.0901, 0.725123)),
        1.0
    );
}

fragment float4 present_fragment_interlaced(VertexOut in [[stage_in]],
                                            texture2d<float> tex_l [[texture(0)]],
                                            texture2d<float> tex_r [[texture(1)]],
                                            sampler smp            [[sampler(0)]],
                                            constant PresentUniforms& u [[buffer(1)]])
{
    uint row = uint(in.position.y);
    if ((row & 1u) == 0u)
        return tex_l.sample(smp, in.tex_coord);
    else
        return tex_r.sample(smp, in.tex_coord);
}