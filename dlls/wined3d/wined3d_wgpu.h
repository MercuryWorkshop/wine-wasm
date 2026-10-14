/*
 * Copyright 2026 gex contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* The WebGPU backend, on webgpu.h (Wine's webgpu.dll).
 *
 * Unlike Vulkan, WebGPU tracks object lifetimes and hazards itself: an object
 * may be released while recorded commands still use it, and there are no
 * barriers or layouts. What it lacks is mapping a buffer the GPU uses, so a
 * buffer object here is a WGPUBuffer with a CPU copy ("shadow") that maps
 * return; an unmap writes the ranges written with wgpuQueueWriteBuffer().
 * That write is ordered before every command not yet submitted, which is
 * right for NOOVERWRITE and DISCARD maps (the application promises not to
 * touch what earlier commands use; DISCARD renames a busy buffer), and for
 * any other the commands recorded so far are submitted first. */

#ifndef __WINE_WINED3D_WGPU_H
#define __WINE_WINED3D_WGPU_H

#include "webgpu.h"

/* webgpu.h has no undefined vertex format. */
#define WGPU_VERTEX_FORMAT_NONE ((WGPUVertexFormat)0)

#define WINED3D_WGPU_STAGING_CHUNK_SIZE (4 * 1024 * 1024)
/* WebGPU's bytesPerRow alignment for buffer <-> texture copies. */
#define WINED3D_WGPU_ROW_ALIGNMENT 256
/* A constant buffer's GPU buffer is at least this big, so that it covers
 * whatever size a shader declares (D3D allows binding a smaller one). */
#define WINED3D_WGPU_MIN_CONSTANT_BUFFER_SIZE 65536

struct wined3d_wgpu_info
{
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPULimits limits;
    WGPUAdapterInfo adapter_info;

    bool texture_compression_bc;
    bool texture_compression_bc_3d;
    bool depth32float_stencil8;
    bool depth_clip_control;
    bool float32_filterable;
    bool float32_blendable;
    bool rg11b10_renderable;
    bool dual_source_blending;
    bool clip_distances;
    bool texture_component_swizzle;
    bool texture_formats_tier1;
    bool indirect_first_instance;
};

struct wined3d_format_wgpu
{
    struct wined3d_format f;

    WGPUTextureFormat texture_format;
    WGPUVertexFormat vertex_format;
    /* The format of a view of the opposite sRGB-ness, or Undefined. */
    WGPUTextureFormat srgb_toggle_format;
    /* For formats WebGPU lacks (16-bit packed ones), stored as BGRA8: a row
     * of "width" pixels converted to and from it. */
    void (*convert_upload)(const uint8_t *src, uint32_t *dst, unsigned int width);
    void (*convert_download)(const uint32_t *src, uint8_t *dst, unsigned int width);
};

static inline const struct wined3d_format_wgpu *wined3d_format_wgpu(const struct wined3d_format *format)
{
    return CONTAINING_RECORD(format, struct wined3d_format_wgpu, f);
}

struct wined3d_bo_wgpu
{
    struct wined3d_bo b;

    WGPUBuffer buffer;
    uint64_t size;
    WGPUBufferUsage usage;
    /* The CPU copy of the contents; b.map_ptr. */
    uint8_t *shadow;
    /* The encoder that last used the buffer; it is busy while that encoder
     * is open. */
    uint64_t encoder_id;
    /* The GPU wrote the buffer since the shadow was last read back. */
    bool gpu_written;
};

static inline struct wined3d_bo_wgpu *wined3d_bo_wgpu(struct wined3d_bo *bo)
{
    return CONTAINING_RECORD(bo, struct wined3d_bo_wgpu, b);
}

struct wined3d_staging_wgpu
{
    WGPUBuffer buffer;
    uint64_t offset;
};

struct wined3d_bind_group_layout_key_wgpu
{
    WGPUBindGroupLayoutEntry *entries;
    size_t entry_count;
};

struct wined3d_bind_group_layout_wgpu
{
    struct wine_rb_entry entry;
    struct wined3d_bind_group_layout_key_wgpu key;
    WGPUBindGroupLayout layout;
    WGPUPipelineLayout pipeline_layout;
};

/* What a render pipeline is made from, besides the shader modules. */
struct wined3d_graphics_pipeline_key_wgpu
{
    WGPUShaderModule vs, fs;
    WGPUPipelineLayout layout;

    struct
    {
        uint32_t stride;
        uint32_t step_mode;
        uint32_t attribute_count;
        WGPUVertexAttribute attributes[MAX_ATTRIBS];
    } buffers[WINED3D_MAX_STREAMS];
    uint32_t buffer_count;

    WGPUPrimitiveTopology topology;
    WGPUIndexFormat strip_index_format;
    WGPUFrontFace front_face;
    WGPUCullMode cull_mode;
    bool unclipped_depth;

    WGPUTextureFormat ds_format;
    bool depth_write;
    WGPUCompareFunction depth_compare;
    WGPUStencilFaceState stencil_front, stencil_back;
    uint32_t stencil_read_mask, stencil_write_mask;
    int32_t depth_bias;
    float depth_bias_slope_scale;
    float depth_bias_clamp;

    uint32_t sample_count;
    uint32_t sample_mask;
    bool alpha_to_coverage;

    uint32_t rt_count;
    WGPUTextureFormat rt_formats[WINED3D_MAX_RENDER_TARGETS];
    bool blend_enable[WINED3D_MAX_RENDER_TARGETS];
    WGPUBlendState blend[WINED3D_MAX_RENDER_TARGETS];
    WGPUColorWriteMask write_mask[WINED3D_MAX_RENDER_TARGETS];
};

struct wined3d_graphics_pipeline_wgpu
{
    struct wine_rb_entry entry;
    struct wined3d_graphics_pipeline_key_wgpu key;
    WGPURenderPipeline pipeline;
};

enum wined3d_shader_descriptor_type_wgpu
{
    WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_CBV,
    WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV,
    WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV_DEPTH,
    WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_UAV,
    WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SAMPLER,
};

/* A binding of the bind group, and the wined3d state it takes. */
struct wined3d_shader_binding_wgpu
{
    enum wined3d_shader_type shader_type;
    enum wined3d_shader_descriptor_type_wgpu type;
    unsigned int resource_idx;
    enum wined3d_shader_resource_type resource_type;
    /* enum vkd3d_shader_resource_data_type; for samplers, whether it
     * compares. */
    unsigned int data_type;
    uint32_t binding;
    /* For samplers, the SRVs sampled with it. */
    uint32_t srv_mask[4];
};

/* The bindings of the bound shaders, and the bind group layout: its
 * entries as the shaders declare them, and as last made for the bound
 * resources, whose formats may not filter. */
struct wined3d_shader_bindings_wgpu
{
    struct wined3d_shader_binding_wgpu *bindings;
    SIZE_T size, count;

    WGPUBindGroupLayoutEntry *entries;
    SIZE_T entries_size;
    bool *unfilterable;
    SIZE_T unfilterable_size;
    bool layout_valid;
};

struct wined3d_context_wgpu
{
    struct wined3d_context c;

    const struct wined3d_wgpu_info *wgpu_info;

    WGPUCommandEncoder encoder;
    /* The encoder's id; ids increase, and an encoder is submitted when the
     * next is begun. */
    uint64_t encoder_id;
    /* Encoders up to this one have completed on the GPU. */
    uint64_t completed_encoder_id;
    uint64_t submitted_encoder_id;
    unsigned int work_count;

    WGPURenderPassEncoder render_pass;
    WGPUComputePassEncoder compute_pass;

    struct wined3d_staging_wgpu staging;

    struct
    {
        WGPUShaderModule modules[WINED3D_SHADER_TYPE_GRAPHICS_COUNT];
        struct wined3d_graphics_pipeline_key_wgpu key;
        WGPURenderPipeline pipeline;
        WGPUBindGroupLayout bind_group_layout;
        struct wined3d_shader_bindings_wgpu bindings;
        bool valid;
    } graphics;

    struct
    {
        WGPUShaderModule module;
        WGPUComputePipeline pipeline;
        WGPUBindGroupLayout bind_group_layout;
        WGPUPipelineLayout pipeline_layout;
        struct wined3d_shader_bindings_wgpu bindings;
        /* What the pipeline was made from. */
        WGPUShaderModule pipeline_module;
        WGPUBindGroupLayout pipeline_bind_group_layout;
    } compute;

    /* The wined3d stream of each vertex buffer slot, and the slot of zeros
     * for inputs no stream feeds. */
    uint32_t slot_streams[WINED3D_MAX_STREAMS];
    uint32_t null_slot;
    bool vertex_input_valid;
    /* For each slot, the bytes a vertex reads, and the bytes bound; a draw
     * past the end is an error in WebGPU, and is skipped. */
    uint64_t slot_vertex_size[WINED3D_MAX_STREAMS];
    uint64_t slot_bound_size[WINED3D_MAX_STREAMS];
    uint64_t index_bound_size;

    /* Render targets smaller than the depth buffer are drawn in proxies of
     * its size (WebGPU's attachments are all one size), and copied back when
     * the pass ends. */
    struct wined3d_rt_proxy_wgpu
    {
        WGPUTexture proxy;
        WGPUTexture texture;
        unsigned int level, layer, width, height;
    } rt_proxies[WINED3D_MAX_RENDER_TARGETS];
    unsigned int rt_proxy_count;
    struct wined3d_proxy_cache_wgpu
    {
        WGPUTexture texture;
        WGPUTextureFormat format;
        unsigned int width, height, samples;
    } proxy_cache[8];

    unsigned int rt_count;
    unsigned int sample_count;
    /* Whether the pass has the depth/stencil view: not when it is smaller
     * than the render targets, which WebGPU does not take. */
    bool pass_has_ds;
    /* The render pass's attachment size, which viewports and scissors are
     * clamped to. */
    unsigned int fb_width, fb_height;

    struct list active_queries;

    struct wine_rb_tree bind_group_layouts;
    struct wine_rb_tree graphics_pipelines;
};

bool wined3d_wgpu_buffer_srv_range(const struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_shader_resource_view *srv, uint64_t *offset, uint64_t *size,
        unsigned int *first_component);

static inline struct wined3d_context_wgpu *wined3d_context_wgpu(struct wined3d_context *context)
{
    return CONTAINING_RECORD(context, struct wined3d_context_wgpu, c);
}

bool wined3d_context_wgpu_check_draw(const struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, const struct wined3d_draw_parameters *parameters);
bool wined3d_context_wgpu_apply_compute_state(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, struct wined3d_buffer *indirect);
bool wined3d_context_wgpu_apply_draw_state(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, struct wined3d_buffer *indirect, bool indexed);
void wined3d_context_wgpu_cleanup(struct wined3d_context_wgpu *context_wgpu);
bool wined3d_context_wgpu_create_bo(struct wined3d_context_wgpu *context_wgpu, uint64_t size,
        WGPUBufferUsage usage, struct wined3d_bo_wgpu *bo);
void wined3d_context_wgpu_destroy_bo(struct wined3d_context_wgpu *context_wgpu, struct wined3d_bo_wgpu *bo);
void wined3d_context_wgpu_end_passes(struct wined3d_context_wgpu *context_wgpu);
WGPUCommandEncoder wined3d_context_wgpu_get_encoder(struct wined3d_context_wgpu *context_wgpu);
struct wined3d_bind_group_layout_wgpu *wined3d_context_wgpu_get_bind_group_layout(
        struct wined3d_context_wgpu *context_wgpu, const WGPUBindGroupLayoutEntry *entries, size_t entry_count);
HRESULT wined3d_context_wgpu_init(struct wined3d_context_wgpu *context_wgpu, struct wined3d_swapchain *swapchain);
void wined3d_context_wgpu_poll(struct wined3d_context_wgpu *context_wgpu);
bool wined3d_context_wgpu_stage(struct wined3d_context_wgpu *context_wgpu, const void *data,
        uint64_t size, uint64_t alignment, struct wined3d_staging_wgpu *staging);
void wined3d_context_wgpu_submit(struct wined3d_context_wgpu *context_wgpu);
void wined3d_context_wgpu_wait(struct wined3d_context_wgpu *context_wgpu, uint64_t encoder_id);
bool wined3d_context_wgpu_wait_future(struct wined3d_context_wgpu *context_wgpu, WGPUFuture future);

static inline void wined3d_context_wgpu_reference_bo(const struct wined3d_context_wgpu *context_wgpu,
        struct wined3d_bo_wgpu *bo)
{
    bo->encoder_id = context_wgpu->encoder_id;
}

struct wined3d_adapter_wgpu
{
    struct wined3d_adapter a;

    struct wined3d_wgpu_info wgpu_info;
};

static inline struct wined3d_adapter_wgpu *wined3d_adapter_wgpu(struct wined3d_adapter *adapter)
{
    return CONTAINING_RECORD(adapter, struct wined3d_adapter_wgpu, a);
}

static inline const struct wined3d_adapter_wgpu *wined3d_adapter_wgpu_const(const struct wined3d_adapter *adapter)
{
    return CONTAINING_RECORD(adapter, struct wined3d_adapter_wgpu, a);
}

BOOL wined3d_adapter_wgpu_init_format_info(struct wined3d_adapter_wgpu *adapter_wgpu);
void adapter_wgpu_copy_bo_address(struct wined3d_context *context, const struct wined3d_bo_address *dst,
        const struct wined3d_bo_address *src, unsigned int range_count, const struct wined3d_range *ranges,
        uint32_t map_flags);

struct wined3d_null_resources_wgpu
{
    WGPUBuffer buffer;
    WGPUTexture texture_2d, texture_2d_uint, texture_2d_sint;
    WGPUTextureView views[WINED3D_SHADER_RESOURCE_TEXTURE_CUBEARRAY + 1][3];
    WGPUTextureView depth_2d, depth_cube;
    WGPUTexture depth_2d_texture;
    WGPUSampler sampler, comparison_sampler;
    /* A fragment shader with no outputs, for draws with none. */
    WGPUShaderModule fragment_module;
    /* Depth to r32float, for reading back depth formats WebGPU cannot copy
     * (texture_wgpu.c), made when first needed. */
    WGPURenderPipeline depth_read_pipeline;
    /* Mipmap generation, a pipeline per format (texture_wgpu.c). */
    struct
    {
        WGPUTextureFormat format;
        bool is_3d;
        WGPURenderPipeline pipeline;
    } mip_pipelines[8];
    WGPUBindGroupLayout mip_layout, mip_layout_3d;
};

struct wined3d_device_wgpu
{
    struct wined3d_device d;

    struct wined3d_context_wgpu context_wgpu;

    WGPUDevice device;
    WGPUQueue queue;

    struct wined3d_null_resources_wgpu null_resources;
};

static inline struct wined3d_device_wgpu *wined3d_device_wgpu(struct wined3d_device *device)
{
    return CONTAINING_RECORD(device, struct wined3d_device_wgpu, d);
}

bool wined3d_device_wgpu_create_null_resources(struct wined3d_device_wgpu *device_wgpu);
void wined3d_texture_wgpu_generate_mipmaps(struct wined3d_context_wgpu *context_wgpu,
        struct wined3d_shader_resource_view *view);
void wined3d_device_wgpu_push_error_scope(struct wined3d_device_wgpu *device_wgpu);
bool wined3d_device_wgpu_pop_error_scope(struct wined3d_device_wgpu *device_wgpu, const char *what);
void wined3d_device_wgpu_destroy_null_resources(struct wined3d_device_wgpu *device_wgpu);

struct wined3d_texture_wgpu
{
    struct wined3d_texture t;

    WGPUTexture texture;
    WGPUTextureFormat format;
    /* Views of the whole texture: as an attachment, and sampled (with the
     * format's swizzle; of the depth aspect only, for depth formats). */
    WGPUTextureView default_view;
    WGPUTextureView default_srv;
    WGPUTextureViewDimension default_srv_dimension;
};

static inline struct wined3d_texture_wgpu *wined3d_texture_wgpu(struct wined3d_texture *texture)
{
    return CONTAINING_RECORD(texture, struct wined3d_texture_wgpu, t);
}

HRESULT wined3d_texture_wgpu_init(struct wined3d_texture_wgpu *texture_wgpu, struct wined3d_device *device,
        const struct wined3d_resource_desc *desc, unsigned int layer_count, unsigned int level_count,
        uint32_t flags, void *parent, const struct wined3d_parent_ops *parent_ops);
BOOL wined3d_texture_wgpu_prepare_texture(struct wined3d_texture_wgpu *texture_wgpu,
        struct wined3d_context_wgpu *context_wgpu);
WGPUTextureView wined3d_texture_wgpu_get_default_view(struct wined3d_texture_wgpu *texture_wgpu,
        struct wined3d_context_wgpu *context_wgpu, bool srv);
void wined3d_texture_wgpu_release_views(struct wined3d_texture_wgpu *texture_wgpu);
void wined3d_wgpu_blitter_create(struct wined3d_blitter **next);

struct wined3d_sampler_wgpu
{
    struct wined3d_sampler s;

    WGPUSampler sampler;
};

static inline struct wined3d_sampler_wgpu *wined3d_sampler_wgpu(struct wined3d_sampler *sampler)
{
    return CONTAINING_RECORD(sampler, struct wined3d_sampler_wgpu, s);
}

void wined3d_sampler_wgpu_init(struct wined3d_sampler_wgpu *sampler_wgpu,
        struct wined3d_device *device, const struct wined3d_sampler_desc *desc,
        void *parent, const struct wined3d_parent_ops *parent_ops);

struct wined3d_buffer_wgpu
{
    struct wined3d_buffer b;
};

static inline struct wined3d_buffer_wgpu *wined3d_buffer_wgpu(struct wined3d_buffer *buffer)
{
    return CONTAINING_RECORD(buffer, struct wined3d_buffer_wgpu, b);
}

HRESULT wined3d_buffer_wgpu_init(struct wined3d_buffer_wgpu *buffer_wgpu, struct wined3d_device *device,
        const struct wined3d_buffer_desc *desc, const struct wined3d_sub_resource_data *data,
        void *parent, const struct wined3d_parent_ops *parent_ops);

/* A texture view, made when first needed and remade when the texture's
 * WGPUTexture changes (swapchain rotation, unloading). */
struct wined3d_view_wgpu
{
    WGPUTexture source;
    WGPUTextureView view;
    WGPUTextureViewDimension dimension;
    /* A view of the same range in another dimension (2D as 2D array, say),
     * for a shader that declares it so. */
    WGPUTextureView alt_view;
    WGPUTextureViewDimension alt_dimension;
};

struct wined3d_rendertarget_view_wgpu
{
    struct wined3d_rendertarget_view v;
    struct wined3d_view_wgpu view_wgpu;
};

static inline struct wined3d_rendertarget_view_wgpu *wined3d_rendertarget_view_wgpu(
        struct wined3d_rendertarget_view *view)
{
    return CONTAINING_RECORD(view, struct wined3d_rendertarget_view_wgpu, v);
}

WGPUTextureView wined3d_rendertarget_view_wgpu_get_view(struct wined3d_rendertarget_view_wgpu *rtv_wgpu,
        struct wined3d_context_wgpu *context_wgpu);
HRESULT wined3d_rendertarget_view_wgpu_init(struct wined3d_rendertarget_view_wgpu *view_wgpu,
        const struct wined3d_view_desc *desc, struct wined3d_resource *resource,
        void *parent, const struct wined3d_parent_ops *parent_ops);

struct wined3d_shader_resource_view_wgpu
{
    struct wined3d_shader_resource_view v;
    struct wined3d_view_wgpu view_wgpu;
};

static inline struct wined3d_shader_resource_view_wgpu *wined3d_shader_resource_view_wgpu(
        struct wined3d_shader_resource_view *view)
{
    return CONTAINING_RECORD(view, struct wined3d_shader_resource_view_wgpu, v);
}

WGPUTextureView wined3d_shader_resource_view_wgpu_get_view(struct wined3d_shader_resource_view_wgpu *srv_wgpu,
        struct wined3d_context_wgpu *context_wgpu, WGPUTextureViewDimension dimension);
HRESULT wined3d_shader_resource_view_wgpu_init(struct wined3d_shader_resource_view_wgpu *view_wgpu,
        const struct wined3d_view_desc *desc, struct wined3d_resource *resource,
        void *parent, const struct wined3d_parent_ops *parent_ops);

struct wined3d_unordered_access_view_wgpu
{
    struct wined3d_unordered_access_view v;
    struct wined3d_view_wgpu view_wgpu;
};

static inline struct wined3d_unordered_access_view_wgpu *wined3d_unordered_access_view_wgpu(
        struct wined3d_unordered_access_view *view)
{
    return CONTAINING_RECORD(view, struct wined3d_unordered_access_view_wgpu, v);
}

WGPUTextureView wined3d_unordered_access_view_wgpu_get_view(struct wined3d_unordered_access_view_wgpu *uav_wgpu,
        struct wined3d_context_wgpu *context_wgpu);
HRESULT wined3d_unordered_access_view_wgpu_init(struct wined3d_unordered_access_view_wgpu *view_wgpu,
        const struct wined3d_view_desc *desc, struct wined3d_resource *resource,
        void *parent, const struct wined3d_parent_ops *parent_ops);
void wined3d_view_wgpu_cleanup(struct wined3d_view_wgpu *view_wgpu);

struct wined3d_query_wgpu
{
    struct wined3d_query q;

    struct list entry;
    uint64_t encoder_id;
    bool active;
};

HRESULT wined3d_query_wgpu_create(struct wined3d_device *device, enum wined3d_query_type type, void *parent,
        const struct wined3d_parent_ops *parent_ops, struct wined3d_query **query);

struct wined3d_swapchain_wgpu
{
    struct wined3d_swapchain s;
};

static inline struct wined3d_swapchain_wgpu *wined3d_swapchain_wgpu(struct wined3d_swapchain *swapchain)
{
    return CONTAINING_RECORD(swapchain, struct wined3d_swapchain_wgpu, s);
}

void wined3d_swapchain_wgpu_cleanup(struct wined3d_swapchain_wgpu *swapchain_wgpu);
HRESULT wined3d_swapchain_wgpu_init(struct wined3d_swapchain_wgpu *swapchain_wgpu,
        struct wined3d_device *device, const struct wined3d_swapchain_desc *desc,
        struct wined3d_swapchain_state_parent *state_parent, void *parent,
        const struct wined3d_parent_ops *parent_ops);

WGPUCompareFunction wgpu_compare_function_from_wined3d(enum wined3d_cmp_func op);
WGPUTextureViewDimension wgpu_view_dimension_from_wined3d(enum wined3d_resource_type type,
        uint32_t flags, unsigned int layer_count, bool cube);
void wined3d_format_colour_to_wgpu(const struct wined3d_format *format, const struct wined3d_color *c,
        WGPUColor *retval);

static inline WGPUStringView wgpu_string(const char *s)
{
    WGPUStringView view = {s, WGPU_STRLEN};

    return view;
}

const struct wined3d_shader_backend_ops *wined3d_wgsl_shader_backend_init_wgpu(void);
const struct wined3d_vertex_pipe_ops *wined3d_wgsl_vertex_pipe_init_wgpu(void);
const struct wined3d_fragment_pipe_ops *wined3d_wgsl_fragment_pipe_init_wgpu(void);

#endif /* __WINE_WINED3D_WGPU_H */
