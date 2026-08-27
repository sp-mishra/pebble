#pragma once
// ============================================================================
// kalpana/backend/instanced_pipeline.hpp — GPU Hardware Instanced Particle Pipeline
// ============================================================================
// Provides zero-allocation, single-draw-call GPU instancing for massive particle
// systems (100k+ particles) via Metal / Sokol GFX.
// Template quad mesh is uploaded once; per-instance (x, y, radius, r, g, b, a)
// stream is updated and drawn in a single hardware invocation: sg_draw(0, 6, N).
// ============================================================================

#include "sokol_backend.hpp"
#include <vector>
#include <span>
#include <cstdint>

#if defined(KALPANA_HAS_SOKOL_GFX)

namespace kalpana {

struct GPUInstanceData {
    float x, y;         // Screen space center position
    float radius;       // Radius in pixels
    float pad;          // 16-byte alignment
    float r, g, b, a;   // Instance color (RGBA float)
};

static const char* INSTANCED_VS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct Uniforms { float2 screen_size; };\n"
    "struct VertexIn {\n"
    "    float2 pos     [[attribute(0)]];\n"
    "    float2 center  [[attribute(1)]];\n"
    "    float  radius  [[attribute(2)]];\n"
    "    float  pad     [[attribute(3)]];\n"
    "    float4 color   [[attribute(4)]];\n"
    "};\n"
    "struct VertexOut {\n"
    "    float4 pos [[position]];\n"
    "    float2 uv;\n"
    "    float4 color;\n"
    "};\n"
    "vertex VertexOut vs(VertexIn in [[stage_in]], constant Uniforms& u [[buffer(0)]]) {\n"
    "    VertexOut out;\n"
    "    float2 world_pos = in.center + in.pos * in.radius;\n"
    "    float2 clip = (world_pos / u.screen_size) * 2.0 - 1.0;\n"
    "    clip.y = -clip.y;\n"
    "    out.pos = float4(clip, 0.0, 1.0);\n"
    "    out.uv = in.pos;\n"
    "    out.color = in.color;\n"
    "    return out;\n"
    "}\n";

static const char* INSTANCED_FS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct FragmentIn {\n"
    "    float4 pos [[position]];\n"
    "    float2 uv;\n"
    "    float4 color;\n"
    "};\n"
    "fragment float4 fs(FragmentIn in [[stage_in]]) {\n"
    "    float dist2 = dot(in.uv, in.uv);\n"
    "    if (dist2 > 1.0) discard_fragment();\n"
    "    float alpha = 1.0 - smoothstep(0.85, 1.0, dist2);\n"
    "    return float4(in.color.rgb, in.color.a * alpha);\n"
    "}\n";

class InstancedParticlePipeline {
public:
    InstancedParticlePipeline() = default;

    void init(std::size_t max_instances = 65536) {
        max_instances_ = max_instances;
        instances_.reserve(max_instances);

        // 1. Template Unit Quad Vertices (attribute 0)
        struct UnitVert { float x, y; };
        static const UnitVert unit_quad[4] = {
            {-1.0f, -1.0f},
            { 1.0f, -1.0f},
            { 1.0f,  1.0f},
            {-1.0f,  1.0f}
        };
        static const uint16_t quad_indices[6] = {0, 1, 2, 0, 2, 3};

        {
            sg_buffer_desc d{};
            d.data = SG_RANGE(unit_quad);
            d.label = "unit_quad_vbuf";
            vbuf_template_ = sg_make_buffer(d);
        }
        {
            sg_buffer_desc d{};
            d.usage.index_buffer = true;
            d.data = SG_RANGE(quad_indices);
            d.label = "unit_quad_ibuf";
            ibuf_template_ = sg_make_buffer(d);
        }
        {
            sg_buffer_desc d{};
            d.size = max_instances_ * sizeof(GPUInstanceData);
            d.usage.stream_update = true;
            d.label = "particle_instance_stream_buffer";
            instance_buf_ = sg_make_buffer(d);
        }

        bind_.vertex_buffers[0] = vbuf_template_;
        bind_.vertex_buffers[1] = instance_buf_;
        bind_.index_buffer = ibuf_template_;

        // 2. Setup Shader and Instanced Pipeline
        sg_shader_desc shd{};
#if defined(SOKOL_METAL)
        shd.vertex_func.source = INSTANCED_VS_METAL;
        shd.vertex_func.entry = "vs";
        shd.fragment_func.source = INSTANCED_FS_METAL;
        shd.fragment_func.entry = "fs";
#endif
        shd.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
        shd.uniform_blocks[0].size = sizeof(float) * 2; // screen_size (w, h)

        sg_shader shdr = sg_make_shader(shd);

        sg_pipeline_desc pd{};
        pd.shader = shdr;
        pd.index_type = SG_INDEXTYPE_UINT16;
        
        // Template mesh attributes (slot 0, step per vertex)
        pd.layout.buffers[0].step_func = SG_VERTEXSTEP_PER_VERTEX;
        pd.layout.attrs[0].buffer_index = 0;
        pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2; // Unit Quad pos

        // Per-Instance Stream Attributes (slot 1, step per instance)
        pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
        pd.layout.attrs[1].buffer_index = 1;
        pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2; // Center (x, y)
        pd.layout.attrs[2].buffer_index = 1;
        pd.layout.attrs[2].format = SG_VERTEXFORMAT_FLOAT;  // Radius
        pd.layout.attrs[3].buffer_index = 1;
        pd.layout.attrs[3].format = SG_VERTEXFORMAT_FLOAT;  // Pad
        pd.layout.attrs[4].buffer_index = 1;
        pd.layout.attrs[4].format = SG_VERTEXFORMAT_FLOAT4; // Color RGBA

        pd.colors[0].blend.enabled = true;
        pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

        pip_ = sg_make_pipeline(pd);
    }

    void begin() {
        instances_.clear();
    }

    void add_instance(float x, float y, float radius, Color c) {
        if (instances_.size() >= max_instances_) return;
        instances_.push_back({
            .x = x,
            .y = y,
            .radius = radius,
            .pad = 0.0f,
            .r = c.r,
            .g = c.g,
            .b = c.b,
            .a = c.a
        });
    }

    void render(float screen_w, float screen_h) {
        if (instances_.empty()) return;

        // 1. Upload dynamic instance buffer in single DMA transfer
        sg_range ir = {instances_.data(), instances_.size() * sizeof(GPUInstanceData)};
        sg_update_buffer(instance_buf_, ir);

        // 2. Uniform screen dimension
        float uniforms[2] = {screen_w, screen_h};
        sg_range ur = {uniforms, sizeof(uniforms)};

        // 3. Single Instanced Draw Call for all instances
        sg_apply_pipeline(pip_);
        sg_apply_bindings(bind_);
        sg_apply_uniforms(0, ur);
        sg_draw(0, 6, static_cast<int>(instances_.size()));
    }

    [[nodiscard]] std::size_t count() const noexcept { return instances_.size(); }

private:
    std::size_t max_instances_ = 65536;
    sg_pipeline pip_{};
    sg_bindings bind_{};
    sg_buffer   vbuf_template_{};
    sg_buffer   ibuf_template_{};
    sg_buffer   instance_buf_{};
    std::vector<GPUInstanceData> instances_;
};

} // namespace kalpana

#endif // KALPANA_HAS_SOKOL_GFX
