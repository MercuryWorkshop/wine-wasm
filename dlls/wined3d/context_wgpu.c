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

#include "wined3d_private.h"
#include "wined3d_wgpu.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d);

/* Submitted encoders whose completion is awaited. */
struct wined3d_submission_wgpu
{
    uint64_t encoder_id;
    WGPUFuture future;
};

static struct wined3d_submission_wgpu submissions[64];
static unsigned int submission_head, submission_count;

static WGPUBlendFactor wgpu_blend_factor_from_wined3d(enum wined3d_blend blend,
        const struct wined3d_format *dst_format, bool alpha)
{
    switch (blend)
    {
        case WINED3D_BLEND_ZERO:
            return WGPUBlendFactor_Zero;
        case WINED3D_BLEND_ONE:
            return WGPUBlendFactor_One;
        case WINED3D_BLEND_SRCCOLOR:
            return WGPUBlendFactor_Src;
        case WINED3D_BLEND_INVSRCCOLOR:
            return WGPUBlendFactor_OneMinusSrc;
        case WINED3D_BLEND_SRCALPHA:
            return WGPUBlendFactor_SrcAlpha;
        case WINED3D_BLEND_INVSRCALPHA:
            return WGPUBlendFactor_OneMinusSrcAlpha;
        case WINED3D_BLEND_DESTALPHA:
            if (dst_format->alpha_size)
                return WGPUBlendFactor_DstAlpha;
            return WGPUBlendFactor_One;
        case WINED3D_BLEND_INVDESTALPHA:
            if (dst_format->alpha_size)
                return WGPUBlendFactor_OneMinusDstAlpha;
            return WGPUBlendFactor_Zero;
        case WINED3D_BLEND_DESTCOLOR:
            return WGPUBlendFactor_Dst;
        case WINED3D_BLEND_INVDESTCOLOR:
            return WGPUBlendFactor_OneMinusDst;
        case WINED3D_BLEND_SRCALPHASAT:
            return WGPUBlendFactor_SrcAlphaSaturated;
        case WINED3D_BLEND_BLENDFACTOR:
            /* WebGPU's constant factor is per channel, like Direct3D's. */
            return WGPUBlendFactor_Constant;
        case WINED3D_BLEND_INVBLENDFACTOR:
            return WGPUBlendFactor_OneMinusConstant;
        case WINED3D_BLEND_SRC1COLOR:
            return WGPUBlendFactor_Src1;
        case WINED3D_BLEND_INVSRC1COLOR:
            return WGPUBlendFactor_OneMinusSrc1;
        case WINED3D_BLEND_SRC1ALPHA:
            return WGPUBlendFactor_Src1Alpha;
        case WINED3D_BLEND_INVSRC1ALPHA:
            return WGPUBlendFactor_OneMinusSrc1Alpha;
        default:
            FIXME("Unhandled blend %#x.\n", blend);
            return WGPUBlendFactor_Zero;
    }
}

static WGPUBlendOperation wgpu_blend_op_from_wined3d(enum wined3d_blend_op op)
{
    switch (op)
    {
        case WINED3D_BLEND_OP_ADD:
            return WGPUBlendOperation_Add;
        case WINED3D_BLEND_OP_SUBTRACT:
            return WGPUBlendOperation_Subtract;
        case WINED3D_BLEND_OP_REVSUBTRACT:
            return WGPUBlendOperation_ReverseSubtract;
        case WINED3D_BLEND_OP_MIN:
            return WGPUBlendOperation_Min;
        case WINED3D_BLEND_OP_MAX:
            return WGPUBlendOperation_Max;
        default:
            FIXME("Unhandled blend op %#x.\n", op);
            return WGPUBlendOperation_Add;
    }
}

static WGPUColorWriteMask wgpu_colour_write_mask_from_wined3d(uint32_t wined3d_mask)
{
    WGPUColorWriteMask mask = 0;

    if (wined3d_mask & WINED3DCOLORWRITEENABLE_RED)
        mask |= WGPUColorWriteMask_Red;
    if (wined3d_mask & WINED3DCOLORWRITEENABLE_GREEN)
        mask |= WGPUColorWriteMask_Green;
    if (wined3d_mask & WINED3DCOLORWRITEENABLE_BLUE)
        mask |= WGPUColorWriteMask_Blue;
    if (wined3d_mask & WINED3DCOLORWRITEENABLE_ALPHA)
        mask |= WGPUColorWriteMask_Alpha;

    return mask;
}

static WGPUCullMode wgpu_cull_mode_from_wined3d(enum wined3d_cull mode)
{
    switch (mode)
    {
        case WINED3D_CULL_NONE:
            return WGPUCullMode_None;
        case WINED3D_CULL_FRONT:
            return WGPUCullMode_Front;
        case WINED3D_CULL_BACK:
            return WGPUCullMode_Back;
        default:
            FIXME("Unhandled cull mode %#x.\n", mode);
            return WGPUCullMode_None;
    }
}

static WGPUPrimitiveTopology wgpu_topology_from_wined3d(enum wined3d_primitive_type t)
{
    switch (t)
    {
        case WINED3D_PT_POINTLIST:
            return WGPUPrimitiveTopology_PointList;
        case WINED3D_PT_LINELIST:
            return WGPUPrimitiveTopology_LineList;
        case WINED3D_PT_LINESTRIP:
            return WGPUPrimitiveTopology_LineStrip;
        case WINED3D_PT_TRIANGLELIST:
            return WGPUPrimitiveTopology_TriangleList;
        case WINED3D_PT_TRIANGLESTRIP:
            return WGPUPrimitiveTopology_TriangleStrip;
        default:
            FIXME("Unhandled primitive type %s.\n", debug_d3dprimitivetype(t));
            return WGPUPrimitiveTopology_Undefined;
    }
}

static WGPUStencilOperation wgpu_stencil_op_from_wined3d(enum wined3d_stencil_op op)
{
    switch (op)
    {
        case WINED3D_STENCIL_OP_KEEP:
            return WGPUStencilOperation_Keep;
        case WINED3D_STENCIL_OP_ZERO:
            return WGPUStencilOperation_Zero;
        case WINED3D_STENCIL_OP_REPLACE:
            return WGPUStencilOperation_Replace;
        case WINED3D_STENCIL_OP_INCR_SAT:
            return WGPUStencilOperation_IncrementClamp;
        case WINED3D_STENCIL_OP_DECR_SAT:
            return WGPUStencilOperation_DecrementClamp;
        case WINED3D_STENCIL_OP_INVERT:
            return WGPUStencilOperation_Invert;
        case WINED3D_STENCIL_OP_INCR:
            return WGPUStencilOperation_IncrementWrap;
        case WINED3D_STENCIL_OP_DECR:
            return WGPUStencilOperation_DecrementWrap;
        default:
            if (!op)
                WARN("Unhandled stencil operation %#x.\n", op);
            else
                FIXME("Unhandled stencil operation %#x.\n", op);
            return WGPUStencilOperation_Keep;
    }
}

/* Submission. */

WGPUCommandEncoder wined3d_context_wgpu_get_encoder(struct wined3d_context_wgpu *context_wgpu)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);

    if (!context_wgpu->encoder && !(context_wgpu->encoder = wgpuDeviceCreateCommandEncoder(device_wgpu->device, NULL)))
        ERR("Failed to create a command encoder.\n");
    return context_wgpu->encoder;
}

void wined3d_context_wgpu_end_passes(struct wined3d_context_wgpu *context_wgpu)
{
    unsigned int i;

    if (context_wgpu->render_pass)
    {
        wgpuRenderPassEncoderEnd(context_wgpu->render_pass);
        wgpuRenderPassEncoderRelease(context_wgpu->render_pass);
        context_wgpu->render_pass = NULL;

        for (i = 0; i < context_wgpu->rt_proxy_count; ++i)
        {
            const struct wined3d_rt_proxy_wgpu *p = &context_wgpu->rt_proxies[i];
            WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            WGPUExtent3D extent = {p->width, p->height, 1};

            src.texture = p->proxy;
            dst.texture = p->texture;
            dst.mipLevel = p->level;
            dst.origin.z = p->layer;
            wgpuCommandEncoderCopyTextureToTexture(context_wgpu->encoder, &src, &dst, &extent);
        }
        context_wgpu->rt_proxy_count = 0;
    }
    if (context_wgpu->compute_pass)
    {
        wgpuComputePassEncoderEnd(context_wgpu->compute_pass);
        wgpuComputePassEncoderRelease(context_wgpu->compute_pass);
        context_wgpu->compute_pass = NULL;
    }
}

static void wgpu_work_done_callback(WGPUQueueWorkDoneStatus status, WGPUStringView message,
        void *userdata1, void *userdata2)
{
    struct wined3d_context_wgpu *context_wgpu = userdata1;
    uint64_t id = (uintptr_t)userdata2;

    if (status != WGPUQueueWorkDoneStatus_Success)
        WARN("Work done with status %#x.\n", status);
    if (id > context_wgpu->completed_encoder_id)
        context_wgpu->completed_encoder_id = id;
}

static void wined3d_context_wgpu_retire_submissions(struct wined3d_context_wgpu *context_wgpu)
{
    while (submission_count && submissions[submission_head].encoder_id <= context_wgpu->completed_encoder_id)
    {
        submission_head = (submission_head + 1) % ARRAY_SIZE(submissions);
        --submission_count;
    }
}

void wined3d_context_wgpu_submit(struct wined3d_context_wgpu *context_wgpu)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPUQueueWorkDoneCallbackInfo callback_info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    WGPUCommandBuffer command_buffer;
    unsigned int idx;

    if (!context_wgpu->encoder)
        return;

    wined3d_context_wgpu_end_passes(context_wgpu);

    TRACE("Submitting encoder %s.\n", wine_dbgstr_longlong(context_wgpu->encoder_id));

    command_buffer = wgpuCommandEncoderFinish(context_wgpu->encoder, NULL);
    wgpuCommandEncoderRelease(context_wgpu->encoder);
    context_wgpu->encoder = NULL;
    wgpuQueueSubmit(device_wgpu->queue, 1, &command_buffer);
    wgpuCommandBufferRelease(command_buffer);

    /* Wait on the oldest when the ring is full. */
    wined3d_context_wgpu_retire_submissions(context_wgpu);
    if (submission_count == ARRAY_SIZE(submissions))
        wined3d_context_wgpu_wait(context_wgpu, submissions[submission_head].encoder_id);

    callback_info.mode = WGPUCallbackMode_AllowProcessEvents;
    callback_info.callback = wgpu_work_done_callback;
    callback_info.userdata1 = context_wgpu;
    callback_info.userdata2 = (void *)(uintptr_t)context_wgpu->encoder_id;
    idx = (submission_head + submission_count++) % ARRAY_SIZE(submissions);
    submissions[idx].encoder_id = context_wgpu->encoder_id;
    submissions[idx].future = wgpuQueueOnSubmittedWorkDone(device_wgpu->queue, callback_info);

    context_wgpu->submitted_encoder_id = context_wgpu->encoder_id++;
    context_wgpu->work_count = 0;

    if (context_wgpu->staging.buffer)
    {
        wgpuBufferRelease(context_wgpu->staging.buffer);
        context_wgpu->staging.buffer = NULL;
        context_wgpu->staging.offset = 0;
    }
}

void wined3d_context_wgpu_poll(struct wined3d_context_wgpu *context_wgpu)
{
    wgpuInstanceProcessEvents(context_wgpu->wgpu_info->instance);
    wined3d_context_wgpu_retire_submissions(context_wgpu);
}

bool wined3d_context_wgpu_wait_future(struct wined3d_context_wgpu *context_wgpu, WGPUFuture future)
{
    WGPUFutureWaitInfo info = {future, FALSE};

    return wgpuInstanceWaitAny(context_wgpu->wgpu_info->instance, 1, &info, UINT64_MAX)
            == WGPUWaitStatus_Success && info.completed;
}

void wined3d_context_wgpu_wait(struct wined3d_context_wgpu *context_wgpu, uint64_t encoder_id)
{
    if (encoder_id >= context_wgpu->encoder_id)
        wined3d_context_wgpu_submit(context_wgpu);

    while (context_wgpu->completed_encoder_id < encoder_id && submission_count)
    {
        struct wined3d_submission_wgpu *s = &submissions[submission_head];

        if (!wined3d_context_wgpu_wait_future(context_wgpu, s->future))
            wined3d_context_wgpu_poll(context_wgpu);
        /* The callback may have fired in an earlier poll. */
        if (context_wgpu->completed_encoder_id < s->encoder_id)
            context_wgpu->completed_encoder_id = s->encoder_id;
        wined3d_context_wgpu_retire_submissions(context_wgpu);
    }
}

bool wined3d_context_wgpu_stage(struct wined3d_context_wgpu *context_wgpu, const void *data,
        uint64_t size, uint64_t alignment, struct wined3d_staging_wgpu *staging)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    uint64_t offset, padded = (size + 3) & ~(uint64_t)3;
    struct wined3d_staging_wgpu *chunk = &context_wgpu->staging;
    uint8_t *copy;

    offset = (chunk->offset + alignment - 1) & ~(alignment - 1);
    if (!chunk->buffer || offset + padded > wgpuBufferGetSize(chunk->buffer))
    {
        if (chunk->buffer)
            wgpuBufferRelease(chunk->buffer);
        desc.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
        desc.size = max(padded, WINED3D_WGPU_STAGING_CHUNK_SIZE);
        if (!(chunk->buffer = wgpuDeviceCreateBuffer(device_wgpu->device, &desc)))
            return false;
        offset = 0;
    }

    if (padded != size)
    {
        if (!(copy = calloc(1, padded)))
            return false;
        memcpy(copy, data, size);
        wgpuQueueWriteBuffer(device_wgpu->queue, chunk->buffer, offset, copy, padded);
        free(copy);
    }
    else
    {
        wgpuQueueWriteBuffer(device_wgpu->queue, chunk->buffer, offset, data, size);
    }

    chunk->offset = offset + padded;
    staging->buffer = chunk->buffer;
    staging->offset = offset;
    return true;
}

bool wined3d_context_wgpu_create_bo(struct wined3d_context_wgpu *context_wgpu, uint64_t size,
        WGPUBufferUsage usage, struct wined3d_bo_wgpu *bo)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    uint64_t padded;

    padded = (size + 3) & ~(uint64_t)3;
    if (usage & WGPUBufferUsage_Uniform)
        padded = max(padded, WINED3D_WGPU_MIN_CONSTANT_BUFFER_SIZE);

    desc.usage = usage;
    desc.size = padded;
    if (!(bo->buffer = wgpuDeviceCreateBuffer(device_wgpu->device, &desc)))
    {
        ERR("Failed to create a buffer of size %s.\n", wine_dbgstr_longlong(size));
        return false;
    }
    if (!(bo->shadow = calloc(1, padded)))
    {
        wgpuBufferRelease(bo->buffer);
        return false;
    }

    bo->size = padded;
    bo->usage = usage;
    bo->encoder_id = 0;
    bo->gpu_written = false;
    bo->b.refcount = 1;
    list_init(&bo->b.users);
    bo->b.map_ptr = bo->shadow;
    bo->b.buffer_offset = 0;
    bo->b.memory_offset = 0;
    bo->b.client_map_count = 0;
    bo->b.coherent = false;

    TRACE("Created buffer %p, size %s, usage %#x.\n", bo->buffer, wine_dbgstr_longlong(padded), (unsigned int)usage);

    return true;
}

void wined3d_context_wgpu_destroy_bo(struct wined3d_context_wgpu *context_wgpu, struct wined3d_bo_wgpu *bo)
{
    TRACE("context_wgpu %p, bo %p.\n", context_wgpu, bo);

    if (bo->buffer)
        wgpuBufferRelease(bo->buffer);
    free(bo->shadow);
    bo->buffer = NULL;
    bo->shadow = NULL;
}

/* Caches. */

static int wined3d_bind_group_layout_wgpu_compare(const void *key, const struct wine_rb_entry *entry)
{
    const struct wined3d_bind_group_layout_wgpu *layout = WINE_RB_ENTRY_VALUE(entry,
            struct wined3d_bind_group_layout_wgpu, entry);
    const struct wined3d_bind_group_layout_key_wgpu *a = key, *b = &layout->key;

    if (a->entry_count != b->entry_count)
        return a->entry_count < b->entry_count ? -1 : 1;
    return memcmp(a->entries, b->entries, a->entry_count * sizeof(*a->entries));
}

static int wined3d_graphics_pipeline_wgpu_compare(const void *key, const struct wine_rb_entry *entry)
{
    const struct wined3d_graphics_pipeline_wgpu *pipeline = WINE_RB_ENTRY_VALUE(entry,
            struct wined3d_graphics_pipeline_wgpu, entry);

    return memcmp(key, &pipeline->key, sizeof(pipeline->key));
}

static int wgpu_layout_entry_compare(const void *a, const void *b)
{
    const WGPUBindGroupLayoutEntry *x = a, *y = b;

    return x->binding < y->binding ? -1 : x->binding > y->binding;
}

struct wined3d_bind_group_layout_wgpu *wined3d_context_wgpu_get_bind_group_layout(
        struct wined3d_context_wgpu *context_wgpu, const WGPUBindGroupLayoutEntry *entries, size_t entry_count)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPUPipelineLayoutDescriptor pipeline_layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    struct wined3d_bind_group_layout_key_wgpu key;
    struct wined3d_bind_group_layout_wgpu *layout;
    struct wine_rb_entry *entry;

    if (!(layout = calloc(1, sizeof(*layout))))
        return NULL;
    if (!(layout->key.entries = malloc(max(entry_count, 1) * sizeof(*entries))))
    {
        free(layout);
        return NULL;
    }
    memcpy(layout->key.entries, entries, entry_count * sizeof(*entries));
    layout->key.entry_count = entry_count;
    qsort(layout->key.entries, entry_count, sizeof(*entries), wgpu_layout_entry_compare);

    key = layout->key;
    if ((entry = wine_rb_get(&context_wgpu->bind_group_layouts, &key)))
    {
        free(layout->key.entries);
        free(layout);
        return WINE_RB_ENTRY_VALUE(entry, struct wined3d_bind_group_layout_wgpu, entry);
    }

    layout_desc.entryCount = entry_count;
    layout_desc.entries = layout->key.entries;
    if (!(layout->layout = wgpuDeviceCreateBindGroupLayout(device_wgpu->device, &layout_desc)))
    {
        ERR("Failed to create a bind group layout.\n");
        goto fail;
    }

    pipeline_layout_desc.bindGroupLayoutCount = 1;
    pipeline_layout_desc.bindGroupLayouts = &layout->layout;
    if (!(layout->pipeline_layout = wgpuDeviceCreatePipelineLayout(device_wgpu->device, &pipeline_layout_desc)))
    {
        ERR("Failed to create a pipeline layout.\n");
        wgpuBindGroupLayoutRelease(layout->layout);
        goto fail;
    }

    if (wine_rb_put(&context_wgpu->bind_group_layouts, &layout->key, &layout->entry) == -1)
        ERR("Failed to insert bind group layout.\n");

    return layout;

fail:
    free(layout->key.entries);
    free(layout);
    return NULL;
}

static void wined3d_context_wgpu_destroy_bind_group_layout(struct wine_rb_entry *entry, void *ctx)
{
    struct wined3d_bind_group_layout_wgpu *layout = WINE_RB_ENTRY_VALUE(entry,
            struct wined3d_bind_group_layout_wgpu, entry);

    wgpuPipelineLayoutRelease(layout->pipeline_layout);
    wgpuBindGroupLayoutRelease(layout->layout);
    free(layout->key.entries);
    free(layout);
}

static void wined3d_context_wgpu_destroy_graphics_pipeline(struct wine_rb_entry *entry, void *ctx)
{
    struct wined3d_graphics_pipeline_wgpu *pipeline = WINE_RB_ENTRY_VALUE(entry,
            struct wined3d_graphics_pipeline_wgpu, entry);

    if (pipeline->pipeline)
        wgpuRenderPipelineRelease(pipeline->pipeline);
    free(pipeline);
}

static WGPURenderPipeline wined3d_context_wgpu_create_graphics_pipeline(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_graphics_pipeline_key_wgpu *key)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    WGPUColorTargetState targets[WINED3D_MAX_RENDER_TARGETS];
    WGPUVertexBufferLayout buffers[WINED3D_MAX_STREAMS];
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    unsigned int i;

    desc.layout = key->layout;
    desc.vertex.module = key->vs;
    desc.vertex.entryPoint = wgpu_string("shader_entry");
    for (i = 0; i < key->buffer_count; ++i)
    {
        buffers[i].nextInChain = NULL;
        buffers[i].stepMode = key->buffers[i].step_mode;
        buffers[i].arrayStride = key->buffers[i].stride;
        buffers[i].attributeCount = key->buffers[i].attribute_count;
        buffers[i].attributes = key->buffers[i].attributes;
    }
    desc.vertex.bufferCount = key->buffer_count;
    desc.vertex.buffers = buffers;

    desc.primitive.topology = key->topology;
    desc.primitive.stripIndexFormat = key->strip_index_format;
    desc.primitive.frontFace = key->front_face;
    desc.primitive.cullMode = key->cull_mode;
    desc.primitive.unclippedDepth = key->unclipped_depth;

    if (key->ds_format)
    {
        ds.format = key->ds_format;
        ds.depthWriteEnabled = key->depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        ds.depthCompare = key->depth_compare;
        ds.stencilFront = key->stencil_front;
        ds.stencilBack = key->stencil_back;
        ds.stencilReadMask = key->stencil_read_mask;
        ds.stencilWriteMask = key->stencil_write_mask;
        ds.depthBias = key->depth_bias;
        ds.depthBiasSlopeScale = key->depth_bias_slope_scale;
        ds.depthBiasClamp = key->depth_bias_clamp;
        desc.depthStencil = &ds;
    }

    desc.multisample.count = key->sample_count;
    desc.multisample.mask = key->sample_mask;
    desc.multisample.alphaToCoverageEnabled = key->alpha_to_coverage;

    fragment.module = key->fs ? key->fs : device_wgpu->null_resources.fragment_module;
    fragment.entryPoint = wgpu_string("shader_entry");
    for (i = 0; i < key->rt_count; ++i)
    {
        targets[i].nextInChain = NULL;
        targets[i].format = key->rt_formats[i];
        targets[i].blend = key->blend_enable[i] ? &key->blend[i] : NULL;
        targets[i].writeMask = key->write_mask[i];
    }
    fragment.targetCount = key->rt_count;
    fragment.targets = targets;
    desc.fragment = &fragment;

    return wgpuDeviceCreateRenderPipeline(device_wgpu->device, &desc);
}

static WGPURenderPipeline wined3d_context_wgpu_get_graphics_pipeline(struct wined3d_context_wgpu *context_wgpu)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    struct wined3d_graphics_pipeline_key_wgpu *key = &context_wgpu->graphics.key;
    struct wined3d_graphics_pipeline_wgpu *pipeline;
    struct wine_rb_entry *entry;

    if ((entry = wine_rb_get(&context_wgpu->graphics_pipelines, key)))
        return WINE_RB_ENTRY_VALUE(entry, struct wined3d_graphics_pipeline_wgpu, entry)->pipeline;

    if (!(pipeline = malloc(sizeof(*pipeline))))
        return NULL;
    pipeline->key = *key;

    wined3d_device_wgpu_push_error_scope(device_wgpu);
    pipeline->pipeline = wined3d_context_wgpu_create_graphics_pipeline(context_wgpu, key);
    if (!wined3d_device_wgpu_pop_error_scope(device_wgpu, "a render pipeline") && pipeline->pipeline)
    {
        /* Remembered as failed, so as not to be made again. */
        wgpuRenderPipelineRelease(pipeline->pipeline);
        pipeline->pipeline = NULL;
    }

    if (wine_rb_put(&context_wgpu->graphics_pipelines, &pipeline->key, &pipeline->entry) == -1)
        ERR("Failed to insert pipeline.\n");

    return pipeline->pipeline;
}

/* Draw state. */

static void wined3d_context_wgpu_update_vertex_input(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, struct wined3d_graphics_pipeline_key_wgpu *key,
        uint32_t *slot_streams, uint32_t *null_slot)
{
    const struct wined3d_d3d_info *d3d_info = context_wgpu->c.d3d_info;
    uint32_t stream_slots[WINED3D_MAX_STREAMS];
    struct wined3d_stream_info stream_info;
    struct wined3d_shader *vertex_shader;
    unsigned int i, slot;
    uint32_t mask;

    memset(key->buffers, 0, sizeof(key->buffers));
    key->buffer_count = 0;
    *null_slot = ~0u;
    memset(stream_slots, 0xff, sizeof(stream_slots));

    wined3d_stream_info_from_declaration(&stream_info, state, d3d_info);
    for (i = 0; i < ARRAY_SIZE(stream_info.elements); ++i)
    {
        struct wined3d_stream_info_element *e;
        WGPUVertexAttribute *a;

        if (!(stream_info.use_map & (1u << i)))
            continue;
        e = &stream_info.elements[i];

        if ((slot = stream_slots[e->stream_idx]) == ~0u)
        {
            if (key->buffer_count >= context_wgpu->wgpu_info->limits.maxVertexBuffers)
            {
                FIXME("Too many vertex buffers.\n");
                continue;
            }
            slot = stream_slots[e->stream_idx] = key->buffer_count++;
            slot_streams[slot] = e->stream_idx;
            key->buffers[slot].stride = e->stride;
            key->buffers[slot].step_mode = e->instanced ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
            if (e->instanced && e->divisor > 1)
                FIXME("Unhandled instance divisor %u.\n", e->divisor);
            if (e->stride & 3)
                FIXME("Unhandled stride %u.\n", e->stride);
        }

        if (!wined3d_format_wgpu(e->format)->vertex_format)
        {
            FIXME("Unhandled vertex format %s.\n", debug_d3dformat(e->format->id));
            continue;
        }
        a = &key->buffers[slot].attributes[key->buffers[slot].attribute_count++];
        a->format = wined3d_format_wgpu(e->format)->vertex_format;
        a->offset = (UINT_PTR)e->data.addr - state->streams[e->stream_idx].offset;
        a->shaderLocation = i;
    }

    /* The shader's inputs that no element feeds read zero, from a buffer
     * of stride zero. */
    vertex_shader = state->shader[WINED3D_SHADER_TYPE_VERTEX];
    if (vertex_shader && (mask = ~stream_info.use_map & vertex_shader->reg_maps.input_registers)
            && key->buffer_count < context_wgpu->wgpu_info->limits.maxVertexBuffers)
    {
        const struct wined3d_shader_signature *signature = &vertex_shader->input_signature;

        slot = key->buffer_count;
        for (i = 0; i < signature->element_count; ++i)
        {
            const struct wined3d_shader_signature_element *element = &signature->elements[i];
            WGPUVertexAttribute *a;

            if (!(mask & (1u << element->register_idx)) || element->sysval_semantic)
                continue;
            mask &= ~(1u << element->register_idx);

            a = &key->buffers[slot].attributes[key->buffers[slot].attribute_count++];
            a->offset = 0;
            a->shaderLocation = element->register_idx;
            if (element->component_type == WINED3D_TYPE_UINT)
                a->format = WGPUVertexFormat_Uint32x4;
            else if (element->component_type == WINED3D_TYPE_INT)
                a->format = WGPUVertexFormat_Sint32x4;
            else
                a->format = WGPUVertexFormat_Float32x4;
        }
        if (key->buffers[slot].attribute_count)
        {
            key->buffers[slot].stride = 0;
            key->buffers[slot].step_mode = WGPUVertexStepMode_Vertex;
            *null_slot = slot;
            ++key->buffer_count;
        }
    }
}

static void wined3d_context_wgpu_update_rasterizer_state(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, struct wined3d_graphics_pipeline_key_wgpu *key)
{
    const struct wined3d_rasterizer_state_desc *r;
    const struct wined3d_rendertarget_view *dsv;
    union
    {
        uint32_t u32;
        float f32;
    } const_bias;
    bool triangles;

    key->depth_bias = 0;
    key->depth_bias_slope_scale = 0.0f;
    key->depth_bias_clamp = 0.0f;
    key->unclipped_depth = false;

    if (!(r = state->rasterizer_state ? &state->rasterizer_state->desc : NULL))
    {
        key->front_face = WGPUFrontFace_CW;
        key->cull_mode = WGPUCullMode_Back;
        return;
    }

    key->front_face = r->front_ccw ? WGPUFrontFace_CCW : WGPUFrontFace_CW;
    key->cull_mode = wgpu_cull_mode_from_wined3d(r->cull_mode);
    if (r->fill_mode != WINED3D_FILL_SOLID)
        FIXME("Unhandled fill mode %#x.\n", r->fill_mode);
    if (!r->depth_clip && context_wgpu->wgpu_info->depth_clip_control)
        key->unclipped_depth = true;

    /* WebGPU takes a depth bias only for triangles. */
    triangles = key->topology == WGPUPrimitiveTopology_TriangleList
            || key->topology == WGPUPrimitiveTopology_TriangleStrip;
    const_bias.f32 = r->depth_bias;
    if (!triangles || (!r->scale_bias && !const_bias.f32) || !(dsv = state->fb.depth_stencil))
        return;

    if (context_wgpu->c.d3d_info->wined3d_creation_flags & WINED3D_LEGACY_DEPTH_BIAS)
    {
        key->depth_bias = -(int32_t)(const_bias.u32 / dsv->format->depth_bias_scale);
        key->depth_bias_slope_scale = -(float)const_bias.u32;
    }
    else
    {
        key->depth_bias = (int32_t)const_bias.f32;
        key->depth_bias_slope_scale = r->scale_bias;
    }
    key->depth_bias_clamp = r->depth_bias_clamp;
}

static void wined3d_context_wgpu_update_depth_stencil_state(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, struct wined3d_graphics_pipeline_key_wgpu *key)
{
    const struct wined3d_depth_stencil_state *d = state->depth_stencil_state;
    const struct wined3d_rendertarget_view *dsv = state->fb.depth_stencil;
    static const WGPUStencilFaceState default_face =
            {WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep};

    key->stencil_front = default_face;
    key->stencil_back = default_face;
    key->stencil_read_mask = 0xffffffff;
    key->stencil_write_mask = 0xffffffff;
    key->depth_write = false;
    key->depth_compare = WGPUCompareFunction_Always;
    key->ds_format = WGPUTextureFormat_Undefined;

    if (!dsv)
        return;
    key->ds_format = wined3d_texture_wgpu(texture_from_resource(dsv->resource))->format;

    if (!d)
    {
        key->depth_write = true;
        key->depth_compare = WGPUCompareFunction_Less;
        return;
    }

    if (d->desc.depth)
    {
        key->depth_write = d->desc.depth_write;
        key->depth_compare = wgpu_compare_function_from_wined3d(d->desc.depth_func);
    }

    if (d->desc.stencil && dsv->format->stencil_size)
    {
        key->stencil_front.compare = wgpu_compare_function_from_wined3d(d->desc.front.func);
        key->stencil_front.failOp = wgpu_stencil_op_from_wined3d(d->desc.front.fail_op);
        key->stencil_front.depthFailOp = wgpu_stencil_op_from_wined3d(d->desc.front.depth_fail_op);
        key->stencil_front.passOp = wgpu_stencil_op_from_wined3d(d->desc.front.pass_op);
        key->stencil_back.compare = wgpu_compare_function_from_wined3d(d->desc.back.func);
        key->stencil_back.failOp = wgpu_stencil_op_from_wined3d(d->desc.back.fail_op);
        key->stencil_back.depthFailOp = wgpu_stencil_op_from_wined3d(d->desc.back.depth_fail_op);
        key->stencil_back.passOp = wgpu_stencil_op_from_wined3d(d->desc.back.pass_op);
        key->stencil_read_mask = d->desc.stencil_read_mask;
        key->stencil_write_mask = d->desc.stencil_write_mask;
    }
}

static void wined3d_context_wgpu_update_blend_state(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, struct wined3d_graphics_pipeline_key_wgpu *key)
{
    const struct wined3d_blend_state_desc *b = state->blend_state ? &state->blend_state->desc : NULL;
    const struct wined3d_shader *ps = state->shader[WINED3D_SHADER_TYPE_PIXEL];
    unsigned int i;

    memset(key->blend_enable, 0, sizeof(key->blend_enable));
    memset(key->blend, 0, sizeof(key->blend));
    memset(key->write_mask, 0, sizeof(key->write_mask));

    for (i = 0; i < key->rt_count; ++i)
    {
        const struct wined3d_rendertarget_view *rtv = state->fb.render_targets[i];
        const struct wined3d_rendertarget_blend_state_desc *rt;
        enum wined3d_blend src_blend, dst_blend;
        WGPUBlendState *blend = &key->blend[i];

        if (!key->rt_formats[i])
            continue;

        /* A target the pixel shader does not write must not be written. */
        if (!ps || !ps->function || !(ps->reg_maps.rt_mask & (1u << i)))
            continue;

        if (!b)
        {
            key->write_mask[i] = WGPUColorWriteMask_All;
            continue;
        }

        rt = &b->rt[b->independent ? i : 0];
        key->write_mask[i] = wgpu_colour_write_mask_from_wined3d(rt->writemask);
        if (!rt->enable || !(rtv->format->caps[WINED3D_GL_RES_TYPE_TEX_2D]
                & WINED3D_FORMAT_CAP_POSTPIXELSHADER_BLENDING))
            continue;

        key->blend_enable[i] = true;
        src_blend = rt->src;
        dst_blend = rt->dst;
        if (src_blend == WINED3D_BLEND_BOTHSRCALPHA)
        {
            src_blend = WINED3D_BLEND_SRCALPHA;
            dst_blend = WINED3D_BLEND_INVSRCALPHA;
        }
        else if (src_blend == WINED3D_BLEND_BOTHINVSRCALPHA)
        {
            src_blend = WINED3D_BLEND_INVSRCALPHA;
            dst_blend = WINED3D_BLEND_SRCALPHA;
        }
        blend->color.operation = wgpu_blend_op_from_wined3d(rt->op);
        blend->color.srcFactor = wgpu_blend_factor_from_wined3d(src_blend, rtv->format, false);
        blend->color.dstFactor = wgpu_blend_factor_from_wined3d(dst_blend, rtv->format, false);
        blend->alpha.operation = wgpu_blend_op_from_wined3d(rt->op_alpha);
        blend->alpha.srcFactor = wgpu_blend_factor_from_wined3d(rt->src_alpha, rtv->format, true);
        blend->alpha.dstFactor = wgpu_blend_factor_from_wined3d(rt->dst_alpha, rtv->format, true);

        /* Min and max take no factors in WebGPU. */
        if (blend->color.operation == WGPUBlendOperation_Min || blend->color.operation == WGPUBlendOperation_Max)
            blend->color.srcFactor = blend->color.dstFactor = WGPUBlendFactor_One;
        if (blend->alpha.operation == WGPUBlendOperation_Min || blend->alpha.operation == WGPUBlendOperation_Max)
            blend->alpha.srcFactor = blend->alpha.dstFactor = WGPUBlendFactor_One;
    }
}

static void wined3d_context_wgpu_prepare_used_rtv(struct wined3d_context_wgpu *context_wgpu,
        struct wined3d_rendertarget_view *rtv)
{
    if (wined3d_rendertarget_view_get_locations(rtv) & WINED3D_LOCATION_CLEARED)
    {
        /* The clear is made when the render pass begins. */
        if (context_wgpu->render_pass)
            wined3d_context_wgpu_end_passes(context_wgpu);
        wined3d_rendertarget_view_prepare_location(rtv, &context_wgpu->c, rtv->resource->draw_binding);
    }
    else
    {
        wined3d_rendertarget_view_load_location(rtv, &context_wgpu->c, rtv->resource->draw_binding);
    }
}

static WGPUTexture wined3d_context_wgpu_get_proxy(struct wined3d_context_wgpu *context_wgpu,
        WGPUTextureFormat format, unsigned int width, unsigned int height, unsigned int samples)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    struct wined3d_proxy_cache_wgpu *entry;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(context_wgpu->proxy_cache); ++i)
    {
        entry = &context_wgpu->proxy_cache[i];
        if (entry->texture && entry->format == format && entry->width == width
                && entry->height == height && entry->samples == samples)
            return entry->texture;
    }

    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    desc.size.width = width;
    desc.size.height = height;
    desc.size.depthOrArrayLayers = 1;
    desc.format = format;
    desc.sampleCount = samples;

    /* Replace the oldest. */
    entry = &context_wgpu->proxy_cache[ARRAY_SIZE(context_wgpu->proxy_cache) - 1];
    if (entry->texture)
        wgpuTextureRelease(entry->texture);
    memmove(&context_wgpu->proxy_cache[1], &context_wgpu->proxy_cache[0],
            (ARRAY_SIZE(context_wgpu->proxy_cache) - 1) * sizeof(*entry));
    entry = &context_wgpu->proxy_cache[0];
    entry->format = format;
    entry->width = width;
    entry->height = height;
    entry->samples = samples;
    entry->texture = wgpuDeviceCreateTexture(device_wgpu->device, &desc);
    return entry->texture;
}

static bool wined3d_context_wgpu_begin_render_pass(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state)
{
    WGPUTextureView proxy_views[WINED3D_MAX_RENDER_TARGETS] = {0};
    unsigned int ds_width = 0, ds_height = 0;
    WGPURenderPassColorAttachment colour[WINED3D_MAX_RENDER_TARGETS];
    WGPURenderPassDepthStencilAttachment ds = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    struct wined3d_rendertarget_view *view;
    unsigned int i, width = ~0u, height = ~0u;
    WGPUCommandEncoder encoder;
    bool any = false;

    if (context_wgpu->render_pass)
        return true;

    if (!(encoder = wined3d_context_wgpu_get_encoder(context_wgpu)))
        return false;
    wined3d_context_wgpu_end_passes(context_wgpu);

    if ((view = state->fb.depth_stencil))
    {
        ds_width = view->width;
        ds_height = view->height;
    }

    context_wgpu->rt_count = 0;
    for (i = 0; i < ARRAY_SIZE(state->fb.render_targets); ++i)
    {
        WGPURenderPassColorAttachment *a = &colour[i];

        memset(a, 0, sizeof(*a));
        a->depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

        if (!(view = state->fb.render_targets[i]) || view->format->id == WINED3DFMT_NULL)
            continue;
        if (view->resource->type == WINED3D_RTYPE_BUFFER)
        {
            FIXME("Buffer render targets are not supported.\n");
            continue;
        }

        if (!(a->view = wined3d_rendertarget_view_wgpu_get_view(wined3d_rendertarget_view_wgpu(view),
                context_wgpu)))
            continue;
        if (view->resource->type == WINED3D_RTYPE_TEXTURE_3D)
            a->depthSlice = view->desc.u.texture.layer_idx;
        a->storeOp = WGPUStoreOp_Store;

        if (ds_width && (view->width < ds_width || view->height < ds_height)
                && view->resource->type == WINED3D_RTYPE_TEXTURE_2D)
        {
            struct wined3d_texture_wgpu *texture_wgpu = wined3d_texture_wgpu(texture_from_resource(view->resource));
            struct wined3d_rt_proxy_wgpu *p = &context_wgpu->rt_proxies[context_wgpu->rt_proxy_count];
            unsigned int samples = max(1, wined3d_resource_get_sample_count(view->resource)) > 1 ? 4 : 1;
            WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            WGPUExtent3D extent = {view->width, view->height, 1};
            WGPUTexture proxy;

            if ((proxy = wined3d_context_wgpu_get_proxy(context_wgpu, texture_wgpu->format,
                    ds_width, ds_height, samples)))
            {
                p->proxy = proxy;
                p->texture = texture_wgpu->texture;
                p->level = view->sub_resource_idx % texture_wgpu->t.level_count;
                p->layer = view->sub_resource_idx / texture_wgpu->t.level_count;
                p->width = view->width;
                p->height = view->height;
                ++context_wgpu->rt_proxy_count;

                if (!(wined3d_rendertarget_view_get_locations(view) & WINED3D_LOCATION_CLEARED))
                {
                    src.texture = p->texture;
                    src.mipLevel = p->level;
                    src.origin.z = p->layer;
                    dst.texture = proxy;
                    wgpuCommandEncoderCopyTextureToTexture(encoder, &src, &dst, &extent);
                }
                a->view = proxy_views[i] = wgpuTextureCreateView(proxy, NULL);
                a->depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            }
        }
        if (wined3d_rendertarget_view_get_locations(view) & WINED3D_LOCATION_CLEARED)
        {
            struct wined3d_texture *texture = texture_from_resource(view->resource);

            a->loadOp = WGPULoadOp_Clear;
            wined3d_format_colour_to_wgpu(view->format,
                    &texture->sub_resources[view->sub_resource_idx].clear_value.colour, &a->clearValue);
        }
        else
        {
            a->loadOp = WGPULoadOp_Load;
        }

        width = min(width, view->width);
        height = min(height, view->height);
        context_wgpu->rt_count = i + 1;
        any = true;
    }
    desc.colorAttachmentCount = context_wgpu->rt_count;
    desc.colorAttachments = colour;

    if ((view = state->fb.depth_stencil))
    {
        struct wined3d_texture *texture = texture_from_resource(view->resource);
        bool cleared = wined3d_rendertarget_view_get_locations(view) & WINED3D_LOCATION_CLEARED;

        if ((ds.view = wined3d_rendertarget_view_wgpu_get_view(wined3d_rendertarget_view_wgpu(view), context_wgpu)))
        {
            if (view->format->depth_size)
            {
                ds.depthLoadOp = cleared ? WGPULoadOp_Clear : WGPULoadOp_Load;
                ds.depthStoreOp = WGPUStoreOp_Store;
                ds.depthClearValue = texture->sub_resources[view->sub_resource_idx].clear_value.depth;
            }
            if (view->format->stencil_size)
            {
                ds.stencilLoadOp = cleared ? WGPULoadOp_Clear : WGPULoadOp_Load;
                ds.stencilStoreOp = WGPUStoreOp_Store;
                ds.stencilClearValue = texture->sub_resources[view->sub_resource_idx].clear_value.stencil;
            }
            desc.depthStencilAttachment = &ds;
            width = min(width, view->width);
            height = min(height, view->height);
            any = true;
        }
    }

    if (!any)
    {
        FIXME("Draws with no attachments are not supported.\n");
        return false;
    }

    context_wgpu->render_pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
    for (i = 0; i < ARRAY_SIZE(proxy_views); ++i)
    {
        if (proxy_views[i])
            wgpuTextureViewRelease(proxy_views[i]);
    }
    if (!context_wgpu->render_pass)
        return false;
    context_wgpu->fb_width = width;
    context_wgpu->fb_height = height;

    /* A pass starts with no state. */
    context_wgpu->graphics.pipeline = NULL;
    context_invalidate_state(&context_wgpu->c, STATE_STREAMSRC);
    context_invalidate_state(&context_wgpu->c, STATE_INDEXBUFFER);
    context_invalidate_state(&context_wgpu->c, STATE_VIEWPORT);
    context_invalidate_state(&context_wgpu->c, STATE_BLEND_FACTOR);
    context_invalidate_state(&context_wgpu->c, STATE_STENCIL_REF);
    context_wgpu->c.update_shader_resource_bindings = 1;

    return true;
}

/* Whether a sampled view of the resource cannot be filtered: WebGPU then
 * wants the layout to say so, and the samplers used with it not to
 * filter. */
static bool wined3d_srv_wgpu_is_unfilterable(const struct wined3d_shader_resource_view *srv)
{
    if (!srv || srv->resource->type == WINED3D_RTYPE_BUFFER)
        return false;
    if (srv->resource->bind_flags & WINED3D_BIND_DEPTH_STENCIL)
        return true;
    return !(srv->format->caps[WINED3D_GL_RES_TYPE_TEX_2D] & WINED3D_FORMAT_CAP_FILTERING);
}

static bool wined3d_context_wgpu_update_layout(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, struct wined3d_shader_bindings_wgpu *bindings,
        WGPUBindGroupLayout *bind_group_layout, WGPUPipelineLayout *pipeline_layout)
{
    struct wined3d_bind_group_layout_wgpu *layout;
    WGPUBindGroupLayoutEntry *entries;
    bool changed = !bindings->layout_valid;
    size_t i, j;

    if (!wined3d_array_reserve((void **)&bindings->unfilterable, &bindings->unfilterable_size,
            max(bindings->count, 1), sizeof(*bindings->unfilterable)))
        return false;

    for (i = 0; i < bindings->count; ++i)
    {
        const struct wined3d_shader_binding_wgpu *b = &bindings->bindings[i];
        bool unfilterable = false;

        if (b->type == WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV && b->resource_type != WINED3D_SHADER_RESOURCE_BUFFER
                && bindings->entries[i].texture.sampleType == WGPUTextureSampleType_Float)
            unfilterable = wined3d_srv_wgpu_is_unfilterable(state->shader_resource_view[b->shader_type][b->resource_idx]);
        if (bindings->unfilterable[i] != unfilterable)
            changed = true;
        bindings->unfilterable[i] = unfilterable;
    }
    for (i = 0; i < bindings->count; ++i)
    {
        const struct wined3d_shader_binding_wgpu *b = &bindings->bindings[i];
        bool unfilterable = false;

        if (b->type != WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SAMPLER || b->data_type)
            continue;
        for (j = 0; j < bindings->count; ++j)
        {
            const struct wined3d_shader_binding_wgpu *t = &bindings->bindings[j];

            if (bindings->unfilterable[j] && t->shader_type == b->shader_type && t->resource_idx < 128
                    && (b->srv_mask[t->resource_idx / 32] & (1u << (t->resource_idx % 32))))
                unfilterable = true;
        }
        if (bindings->unfilterable[i] != unfilterable)
            changed = true;
        bindings->unfilterable[i] = unfilterable;
    }

    if (!changed && *bind_group_layout)
        return true;

    if (!(entries = malloc(max(bindings->count, 1) * sizeof(*entries))))
        return false;
    memcpy(entries, bindings->entries, bindings->count * sizeof(*entries));
    for (i = 0; i < bindings->count; ++i)
    {
        if (!bindings->unfilterable[i])
            continue;
        if (bindings->bindings[i].type == WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SAMPLER)
            entries[i].sampler.type = WGPUSamplerBindingType_NonFiltering;
        else
            entries[i].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    }
    layout = wined3d_context_wgpu_get_bind_group_layout(context_wgpu, entries, bindings->count);
    free(entries);
    if (!layout)
        return false;

    *bind_group_layout = layout->layout;
    *pipeline_layout = layout->pipeline_layout;
    bindings->layout_valid = true;
    return true;
}

static void wined3d_context_wgpu_load_resources(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, const struct wined3d_shader_bindings_wgpu *bindings,
        enum wined3d_pipeline pipeline)
{
    const struct wined3d_shader_binding_wgpu *binding;
    struct wined3d_unordered_access_view *uav;
    struct wined3d_shader_resource_view *srv;
    struct wined3d_buffer *buffer;
    size_t i;

    for (i = 0; i < bindings->count; ++i)
    {
        binding = &bindings->bindings[i];

        switch (binding->type)
        {
            case WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_CBV:
                if ((buffer = state->cb[binding->shader_type][binding->resource_idx].buffer))
                    wined3d_buffer_load(buffer, &context_wgpu->c, state);
                break;

            case WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV:
            case WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV_DEPTH:
                if (!(srv = state->shader_resource_view[binding->shader_type][binding->resource_idx]))
                    break;
                if (srv->resource->type == WINED3D_RTYPE_BUFFER)
                    wined3d_buffer_load(buffer_from_resource(srv->resource), &context_wgpu->c, state);
                else
                    wined3d_texture_load(texture_from_resource(srv->resource), &context_wgpu->c, FALSE);
                break;

            case WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_UAV:
                if (!(uav = state->unordered_access_view[pipeline][binding->resource_idx]))
                    break;
                if (uav->resource->type == WINED3D_RTYPE_BUFFER)
                {
                    buffer = buffer_from_resource(uav->resource);
                    wined3d_buffer_acquire_bo_for_write(buffer, &context_wgpu->c);
                    wined3d_buffer_load(buffer, &context_wgpu->c, state);
                    wined3d_bo_wgpu(buffer->buffer_object)->gpu_written = true;
                    wined3d_unordered_access_view_invalidate_location(uav, ~WINED3D_LOCATION_BUFFER);
                }
                else
                {
                    wined3d_texture_load(texture_from_resource(uav->resource), &context_wgpu->c, FALSE);
                    wined3d_unordered_access_view_invalidate_location(uav, ~WINED3D_LOCATION_TEXTURE_RGB);
                }
                break;

            case WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SAMPLER:
                break;
        }
    }
}

static WGPUTextureView wined3d_context_wgpu_null_view(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_shader_binding_wgpu *binding)
{
    const struct wined3d_null_resources_wgpu *r = &wined3d_device_wgpu(context_wgpu->c.device)->null_resources;
    unsigned int type = binding->resource_type, idx;

    if (binding->type == WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV_DEPTH)
        return type == WINED3D_SHADER_RESOURCE_TEXTURE_CUBE ? r->depth_cube : r->depth_2d;

    idx = binding->data_type == VKD3D_SHADER_RESOURCE_DATA_UINT ? 1
            : binding->data_type == VKD3D_SHADER_RESOURCE_DATA_INT ? 2 : 0;
    if (type == WINED3D_SHADER_RESOURCE_TEXTURE_1D || type == WINED3D_SHADER_RESOURCE_TEXTURE_1DARRAY)
        FIXME("No null 1D texture.\n");
    if (type == WINED3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY)
        type = WINED3D_SHADER_RESOURCE_TEXTURE_2DMS;
    if (type >= ARRAY_SIZE(r->views) || !r->views[type][idx])
        type = WINED3D_SHADER_RESOURCE_TEXTURE_2D;
    return r->views[type][idx];
}

static bool wined3d_context_wgpu_bind_resources(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, const struct wined3d_shader_bindings_wgpu *bindings,
        WGPUBindGroupLayout layout, enum wined3d_pipeline pipeline)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    const struct wined3d_null_resources_wgpu *r = &device_wgpu->null_resources;
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBindGroupEntry entries_buffer[64], *entries = entries_buffer;
    const struct wined3d_shader_binding_wgpu *binding;
    WGPUBindGroup bind_group;
    size_t i;

    if (bindings->count > ARRAY_SIZE(entries_buffer) && !(entries = calloc(bindings->count, sizeof(*entries))))
        return false;

    for (i = 0; i < bindings->count; ++i)
    {
        WGPUBindGroupEntry *e = &entries[i];

        binding = &bindings->bindings[i];
        memset(e, 0, sizeof(*e));
        e->binding = binding->binding;

        switch (binding->type)
        {
            case WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_CBV:
            {
                const struct wined3d_constant_buffer_state *cb
                        = &state->cb[binding->shader_type][binding->resource_idx];
                struct wined3d_bo_wgpu *bo;

                if (cb->buffer && cb->buffer->buffer_object)
                {
                    bo = wined3d_bo_wgpu(cb->buffer->buffer_object);
                    wined3d_context_wgpu_reference_bo(context_wgpu, bo);
                    e->buffer = bo->buffer;
                    e->offset = bo->b.buffer_offset + cb->offset;
                    e->size = min(bo->size - e->offset, WINED3D_WGPU_MIN_CONSTANT_BUFFER_SIZE);
                }
                else
                {
                    e->buffer = r->buffer;
                    e->size = WINED3D_WGPU_MIN_CONSTANT_BUFFER_SIZE;
                }
                break;
            }

            case WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV:
            case WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV_DEPTH:
            {
                struct wined3d_shader_resource_view *srv
                        = state->shader_resource_view[binding->shader_type][binding->resource_idx];

                if (binding->resource_type == WINED3D_SHADER_RESOURCE_BUFFER)
                {
                    if (srv && srv->resource->type == WINED3D_RTYPE_BUFFER
                            && buffer_from_resource(srv->resource)->buffer_object)
                    {
                        struct wined3d_bo_wgpu *bo = wined3d_bo_wgpu(buffer_from_resource(srv->resource)->buffer_object);

                        wined3d_context_wgpu_reference_bo(context_wgpu, bo);
                        e->buffer = bo->buffer;
                        e->size = bo->size;
                    }
                    else
                    {
                        e->buffer = r->buffer;
                        e->size = WINED3D_WGPU_MIN_CONSTANT_BUFFER_SIZE;
                    }
                    break;
                }

                if (srv && srv->resource->type != WINED3D_RTYPE_BUFFER)
                {
                    if (binding->type == WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV_DEPTH
                            && !srv->resource->format->depth_size)
                        FIXME("Comparison with a non-depth texture.\n");
                    else
                        e->textureView = wined3d_shader_resource_view_wgpu_get_view(
                                wined3d_shader_resource_view_wgpu(srv), context_wgpu);
                }
                if (!e->textureView)
                    e->textureView = wined3d_context_wgpu_null_view(context_wgpu, binding);
                break;
            }

            case WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_UAV:
            {
                struct wined3d_unordered_access_view *uav
                        = state->unordered_access_view[pipeline][binding->resource_idx];

                if (!uav)
                {
                    FIXME("NULL unordered access views are not implemented.\n");
                    goto fail;
                }
                if (uav->resource->type == WINED3D_RTYPE_BUFFER)
                {
                    struct wined3d_bo_wgpu *bo = wined3d_bo_wgpu(buffer_from_resource(uav->resource)->buffer_object);

                    wined3d_context_wgpu_reference_bo(context_wgpu, bo);
                    e->buffer = bo->buffer;
                    e->size = bo->size;
                }
                else if (!(e->textureView = wined3d_unordered_access_view_wgpu_get_view(
                        wined3d_unordered_access_view_wgpu(uav), context_wgpu)))
                {
                    goto fail;
                }
                break;
            }

            case WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SAMPLER:
            {
                struct wined3d_sampler *sampler = state->sampler[binding->shader_type][binding->resource_idx];
                bool comparison = binding->data_type;

                if (!sampler)
                    sampler = context_wgpu->c.device->null_sampler;
                if (sampler && wined3d_sampler_wgpu(sampler)->sampler
                        && (sampler->desc.reduction_mode == WINED3D_FILTER_REDUCTION_COMPARISON) == comparison
                        && (!bindings->unfilterable[i] || (sampler->desc.mag_filter != WINED3D_TEXF_LINEAR
                        && sampler->desc.min_filter != WINED3D_TEXF_LINEAR
                        && sampler->desc.mip_filter != WINED3D_TEXF_LINEAR
                        && sampler->desc.mag_filter != WINED3D_TEXF_ANISOTROPIC
                        && sampler->desc.min_filter != WINED3D_TEXF_ANISOTROPIC)))
                    e->sampler = wined3d_sampler_wgpu(sampler)->sampler;
                else
                    e->sampler = comparison ? r->comparison_sampler : r->sampler;
                break;
            }
        }
    }

    desc.layout = layout;
    desc.entryCount = bindings->count;
    desc.entries = entries;
    bind_group = wgpuDeviceCreateBindGroup(device_wgpu->device, &desc);
    if (entries != entries_buffer)
        free(entries);
    if (!bind_group)
        return false;

    if (pipeline == WINED3D_PIPELINE_GRAPHICS)
        wgpuRenderPassEncoderSetBindGroup(context_wgpu->render_pass, 0, bind_group, 0, NULL);
    else
        wgpuComputePassEncoderSetBindGroup(context_wgpu->compute_pass, 0, bind_group, 0, NULL);
    wgpuBindGroupRelease(bind_group);
    return true;

fail:
    if (entries != entries_buffer)
        free(entries);
    return false;
}

static void wined3d_context_wgpu_bind_vertex_buffers(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, const uint32_t *slot_streams, uint32_t null_slot)
{
    const struct wined3d_null_resources_wgpu *r = &wined3d_device_wgpu(context_wgpu->c.device)->null_resources;
    const struct wined3d_graphics_pipeline_key_wgpu *key = &context_wgpu->graphics.key;
    const struct wined3d_stream_state *stream;
    struct wined3d_bo_wgpu *bo;
    unsigned int slot;

    for (slot = 0; slot < key->buffer_count; ++slot)
    {
        if (slot == null_slot)
        {
            wgpuRenderPassEncoderSetVertexBuffer(context_wgpu->render_pass, slot, r->buffer, 0, WGPU_WHOLE_SIZE);
            continue;
        }

        stream = &state->streams[slot_streams[slot]];
        if (!stream->buffer || !stream->buffer->buffer_object)
        {
            wgpuRenderPassEncoderSetVertexBuffer(context_wgpu->render_pass, slot, r->buffer, 0, WGPU_WHOLE_SIZE);
            continue;
        }
        bo = wined3d_bo_wgpu(stream->buffer->buffer_object);
        wined3d_context_wgpu_reference_bo(context_wgpu, bo);
        wgpuRenderPassEncoderSetVertexBuffer(context_wgpu->render_pass, slot, bo->buffer,
                bo->b.buffer_offset + stream->offset, WGPU_WHOLE_SIZE);
    }
}

static void wined3d_context_wgpu_set_viewport(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state)
{
    float x = 0.0f, y = 0.0f, width = context_wgpu->fb_width, height = context_wgpu->fb_height;
    float min_z = 0.0f, max_z = 1.0f, limit;
    int left = 0, top = 0, right = context_wgpu->fb_width, bottom = context_wgpu->fb_height;

    if (state->viewport_count)
    {
        const struct wined3d_viewport *vp = &state->viewports[0];

        x = vp->x;
        y = vp->y;
        width = vp->width;
        height = vp->height;
        wined3d_viewport_get_z_range(vp, &min_z, &max_z);
        min_z = min(max(min_z, 0.0f), 1.0f);
        max_z = min(max(max_z, 0.0f), 1.0f);
        if (min_z > max_z)
            min_z = max_z;

        left = max(x, 0.0f);
        top = max(y, 0.0f);
        right = min(x + width, (float)context_wgpu->fb_width);
        bottom = min(y + height, (float)context_wgpu->fb_height);

        /* Direct3D 9's pixel centres are at integer coordinates. */
        if (context_wgpu->c.d3d_info->wined3d_creation_flags & WINED3D_PIXEL_CENTER_INTEGER)
        {
            x += 0.5f;
            y += 0.5f;
        }
    }
    if (state->viewport_count > 1)
        FIXME("Multiple viewports are not supported.\n");

    limit = 2.0f * context_wgpu->wgpu_info->limits.maxTextureDimension2D;
    x = min(max(x, -limit), limit);
    y = min(max(y, -limit), limit);
    width = min(max(width, 0.0f), limit - x);
    height = min(max(height, 0.0f), limit - y);
    wgpuRenderPassEncoderSetViewport(context_wgpu->render_pass, x, y, width, height, min_z, max_z);

    if (state->rasterizer_state && state->rasterizer_state->desc.scissor && state->scissor_rect_count)
    {
        const RECT *s = &state->scissor_rects[0];

        left = max(left, s->left);
        top = max(top, s->top);
        right = min(right, s->right);
        bottom = min(bottom, s->bottom);
    }
    left = min(max(left, 0), (int)context_wgpu->fb_width);
    top = min(max(top, 0), (int)context_wgpu->fb_height);
    right = max(min(right, (int)context_wgpu->fb_width), left);
    bottom = max(min(bottom, (int)context_wgpu->fb_height), top);
    wgpuRenderPassEncoderSetScissorRect(context_wgpu->render_pass, left, top, right - left, bottom - top);
}

bool wined3d_context_wgpu_apply_draw_state(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, struct wined3d_buffer *indirect, bool indexed)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    struct wined3d_graphics_pipeline_key_wgpu *key = &context_wgpu->graphics.key;
    const struct wined3d_blend_state *b = state->blend_state;
    bool dual_source_blend = b && b->dual_source;
    unsigned int i, invalidate_rt = 0, sample_count;
    struct wined3d_rendertarget_view *dsv, *rtv;
    WGPUPrimitiveTopology topology;
    WGPUIndexFormat index_format;
    bool invalidate_ds = false;
    WGPURenderPipeline pipeline;
    struct wined3d_buffer *buffer;

    if (wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_SHADER(WINED3D_SHADER_TYPE_PIXEL))
            || wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_FRAMEBUFFER)
            || dual_source_blend != context_wgpu->c.last_was_dual_source_blend)
    {
        context_wgpu->c.shader_update_mask |= (1u << WINED3D_SHADER_TYPE_PIXEL);
        context_wgpu->c.last_was_dual_source_blend = dual_source_blend;
    }
    /* A shader model 1-3 vertex shader's inputs depend on the declaration. */
    if (wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_SHADER(WINED3D_SHADER_TYPE_VERTEX))
            || wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_VDECL))
        context_wgpu->c.shader_update_mask |= (1u << WINED3D_SHADER_TYPE_VERTEX);
    if (wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_SHADER(WINED3D_SHADER_TYPE_GEOMETRY)))
        context_wgpu->c.shader_update_mask |= (1u << WINED3D_SHADER_TYPE_GEOMETRY);

    if ((topology = wgpu_topology_from_wined3d(state->primitive_type)) == WGPUPrimitiveTopology_Undefined)
        return false;

    /* Render targets: clears are made when the pass begins. */
    sample_count = 0;
    for (i = 0; i < ARRAY_SIZE(state->fb.render_targets); ++i)
    {
        if (!(rtv = state->fb.render_targets[i]) || rtv->format->id == WINED3DFMT_NULL)
            continue;

        if (wined3d_blend_state_get_writemask(state->blend_state, i))
        {
            wined3d_context_wgpu_prepare_used_rtv(context_wgpu, rtv);
            invalidate_rt |= (1u << i);
        }
        else
        {
            wined3d_rendertarget_view_prepare_location(rtv, &context_wgpu->c, rtv->resource->draw_binding);
        }

        if (!sample_count)
            sample_count = max(1, wined3d_resource_get_sample_count(rtv->resource));
    }

    if ((dsv = state->fb.depth_stencil))
    {
        if (wined3d_state_uses_depth_buffer(state))
            wined3d_context_wgpu_prepare_used_rtv(context_wgpu, dsv);
        else
            wined3d_rendertarget_view_prepare_location(dsv, &context_wgpu->c, dsv->resource->draw_binding);

        if (!state->depth_stencil_state || state->depth_stencil_state->writes_ds)
            invalidate_ds = true;
        if (!sample_count)
            sample_count = max(1, wined3d_resource_get_sample_count(dsv->resource));
    }
    sample_count = sample_count > 1 ? 4 : 1;
    if (context_wgpu->sample_count != sample_count)
    {
        context_wgpu->sample_count = sample_count;
        context_wgpu->c.shader_update_mask |= (1u << WINED3D_SHADER_TYPE_PIXEL);
    }

    if (context_wgpu->c.shader_update_mask & ~(1u << WINED3D_SHADER_TYPE_COMPUTE))
    {
        device_wgpu->d.shader_backend->shader_apply_draw_state(device_wgpu->d.shader_priv,
                &context_wgpu->c, state);
        context_wgpu->c.shader_update_mask &= 1u << WINED3D_SHADER_TYPE_COMPUTE;
        context_wgpu->c.update_shader_resource_bindings = 1;
    }
    if (!context_wgpu->graphics.valid || !context_wgpu->graphics.modules[WINED3D_SHADER_TYPE_VERTEX])
    {
        WARN("No shaders.\n");
        context_wgpu->c.shader_update_mask |= ~(1u << WINED3D_SHADER_TYPE_COMPUTE);
        return false;
    }
    if (!wined3d_context_wgpu_update_layout(context_wgpu, state, &context_wgpu->graphics.bindings,
            &context_wgpu->graphics.bind_group_layout, &context_wgpu->graphics.key.layout))
        return false;

    /* Loading may copy, which ends the render pass; load first. */
    wined3d_context_wgpu_load_resources(context_wgpu, state, &context_wgpu->graphics.bindings,
            WINED3D_PIPELINE_GRAPHICS);
    for (i = 0; i < ARRAY_SIZE(state->streams); ++i)
    {
        if ((buffer = state->streams[i].buffer))
            wined3d_buffer_load(buffer, &context_wgpu->c, state);
    }
    if (indexed && state->index_buffer)
        wined3d_buffer_load(state->index_buffer, &context_wgpu->c, state);
    if (indirect)
        wined3d_buffer_load(indirect, &context_wgpu->c, state);

    if (wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_FRAMEBUFFER))
        wined3d_context_wgpu_end_passes(context_wgpu);
    if (!wined3d_context_wgpu_begin_render_pass(context_wgpu, state))
        return false;

    while (invalidate_rt)
    {
        i = wined3d_bit_scan(&invalidate_rt);
        rtv = state->fb.render_targets[i];
        wined3d_rendertarget_view_validate_location(rtv, rtv->resource->draw_binding);
        wined3d_rendertarget_view_invalidate_location(rtv, ~rtv->resource->draw_binding);
    }
    if (invalidate_ds)
    {
        wined3d_rendertarget_view_validate_location(dsv, dsv->resource->draw_binding);
        wined3d_rendertarget_view_invalidate_location(dsv, ~dsv->resource->draw_binding);
    }

    /* The pipeline. */
    key->vs = context_wgpu->graphics.modules[WINED3D_SHADER_TYPE_VERTEX];
    key->fs = context_wgpu->graphics.modules[WINED3D_SHADER_TYPE_PIXEL];
    if (wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_VDECL)
            || wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_STREAMSRC)
            || wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_SHADER(WINED3D_SHADER_TYPE_VERTEX))
            || !context_wgpu->vertex_input_valid)
    {
        wined3d_context_wgpu_update_vertex_input(context_wgpu, state, key,
                context_wgpu->slot_streams, &context_wgpu->null_slot);
        context_wgpu->vertex_input_valid = true;
    }
    key->topology = topology;
    index_format = state->index_format == WINED3DFMT_R16_UINT ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
    key->strip_index_format = indexed && (topology == WGPUPrimitiveTopology_TriangleStrip
            || topology == WGPUPrimitiveTopology_LineStrip) ? index_format : WGPUIndexFormat_Undefined;
    wined3d_context_wgpu_update_rasterizer_state(context_wgpu, state, key);
    wined3d_context_wgpu_update_depth_stencil_state(context_wgpu, state, key);
    key->sample_count = context_wgpu->sample_count;
    key->sample_mask = state->sample_mask;
    key->alpha_to_coverage = b && b->desc.alpha_to_coverage;
    key->rt_count = context_wgpu->rt_count;
    memset(key->rt_formats, 0, sizeof(key->rt_formats));
    for (i = 0; i < key->rt_count; ++i)
    {
        if ((rtv = state->fb.render_targets[i]) && rtv->format->id != WINED3DFMT_NULL
                && rtv->resource->type != WINED3D_RTYPE_BUFFER)
            key->rt_formats[i] = wined3d_format_wgpu(rtv->format)->texture_format;
    }
    wined3d_context_wgpu_update_blend_state(context_wgpu, state, key);

    if (!(pipeline = wined3d_context_wgpu_get_graphics_pipeline(context_wgpu)))
        return false;
    if (pipeline != context_wgpu->graphics.pipeline)
    {
        wgpuRenderPassEncoderSetPipeline(context_wgpu->render_pass, pipeline);
        context_wgpu->graphics.pipeline = pipeline;
    }

    if (wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_STREAMSRC)
            || wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_VDECL)
            || wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_SHADER(WINED3D_SHADER_TYPE_VERTEX)))
        wined3d_context_wgpu_bind_vertex_buffers(context_wgpu, state,
                context_wgpu->slot_streams, context_wgpu->null_slot);

    if ((indexed || indirect) && state->index_buffer && state->index_buffer->buffer_object
            && (wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_INDEXBUFFER) || indexed))
    {
        struct wined3d_bo_wgpu *bo = wined3d_bo_wgpu(state->index_buffer->buffer_object);

        wined3d_context_wgpu_reference_bo(context_wgpu, bo);
        wgpuRenderPassEncoderSetIndexBuffer(context_wgpu->render_pass, bo->buffer, index_format,
                bo->b.buffer_offset + state->index_offset, WGPU_WHOLE_SIZE);
    }

    /* Resources are bound for every draw: a bind group is cheap, and which
     * state dirties which binding is not tracked. */
    if (!wined3d_context_wgpu_bind_resources(context_wgpu, state, &context_wgpu->graphics.bindings,
            context_wgpu->graphics.bind_group_layout, WINED3D_PIPELINE_GRAPHICS))
    {
        ERR("Failed to bind resources.\n");
        return false;
    }
    context_wgpu->c.update_shader_resource_bindings = 0;

    if (wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_BLEND_FACTOR))
    {
        WGPUColor colour = {state->blend_factor.r, state->blend_factor.g, state->blend_factor.b,
                state->blend_factor.a};

        wgpuRenderPassEncoderSetBlendConstant(context_wgpu->render_pass, &colour);
    }

    if (wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_STENCIL_REF) && dsv)
        wgpuRenderPassEncoderSetStencilReference(context_wgpu->render_pass,
                state->stencil_ref & wined3d_mask_from_size(dsv->format->stencil_size));

    if (wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_VIEWPORT)
            || wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_SCISSORRECT)
            || wined3d_context_is_graphics_state_dirty(&context_wgpu->c, STATE_RASTERIZER))
        wined3d_context_wgpu_set_viewport(context_wgpu, state);

    memset(context_wgpu->c.dirty_graphics_states, 0, sizeof(context_wgpu->c.dirty_graphics_states));

    return true;
}

bool wined3d_context_wgpu_apply_compute_state(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_state *state, struct wined3d_buffer *indirect)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPUCommandEncoder encoder;

    if (context_wgpu->render_pass)
        wined3d_context_wgpu_end_passes(context_wgpu);

    if (wined3d_context_is_compute_state_dirty(&context_wgpu->c, STATE_COMPUTE_SHADER))
        context_wgpu->c.shader_update_mask |= 1u << WINED3D_SHADER_TYPE_COMPUTE;

    if (context_wgpu->c.shader_update_mask & (1u << WINED3D_SHADER_TYPE_COMPUTE))
    {
        device_wgpu->d.shader_backend->shader_apply_compute_state(device_wgpu->d.shader_priv,
                &context_wgpu->c, state);
        context_wgpu->c.shader_update_mask &= ~(1u << WINED3D_SHADER_TYPE_COMPUTE);
    }
    if (!context_wgpu->compute.module)
    {
        WARN("No compute shader.\n");
        return false;
    }
    if (!context_wgpu->compute.bindings.layout_valid)
    {
        context_wgpu->compute.pipeline = NULL;
        context_wgpu->compute.bind_group_layout = NULL;
    }
    if (!wined3d_context_wgpu_update_layout(context_wgpu, state, &context_wgpu->compute.bindings,
            &context_wgpu->compute.bind_group_layout, &context_wgpu->compute.pipeline_layout))
        return false;
    if (!context_wgpu->compute.pipeline || context_wgpu->compute.pipeline_module != context_wgpu->compute.module
            || context_wgpu->compute.pipeline_bind_group_layout != context_wgpu->compute.bind_group_layout)
    {
        WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;

        if (context_wgpu->compute.pipeline)
            wgpuComputePipelineRelease(context_wgpu->compute.pipeline);
        desc.layout = context_wgpu->compute.pipeline_layout;
        desc.compute.module = context_wgpu->compute.module;
        desc.compute.entryPoint = wgpu_string("shader_entry");
        wined3d_device_wgpu_push_error_scope(device_wgpu);
        context_wgpu->compute.pipeline = wgpuDeviceCreateComputePipeline(device_wgpu->device, &desc);
        if (!wined3d_device_wgpu_pop_error_scope(device_wgpu, "a compute pipeline") && context_wgpu->compute.pipeline)
        {
            wgpuComputePipelineRelease(context_wgpu->compute.pipeline);
            context_wgpu->compute.pipeline = NULL;
        }
        context_wgpu->compute.pipeline_module = context_wgpu->compute.module;
        context_wgpu->compute.pipeline_bind_group_layout = context_wgpu->compute.bind_group_layout;
        if (!context_wgpu->compute.pipeline)
            return false;
    }

    wined3d_context_wgpu_load_resources(context_wgpu, state, &context_wgpu->compute.bindings,
            WINED3D_PIPELINE_COMPUTE);
    if (indirect)
        wined3d_buffer_load_location(indirect, &context_wgpu->c, WINED3D_LOCATION_BUFFER);

    if (!(encoder = wined3d_context_wgpu_get_encoder(context_wgpu)))
        return false;
    if (!context_wgpu->compute_pass)
        context_wgpu->compute_pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);

    wgpuComputePassEncoderSetPipeline(context_wgpu->compute_pass, context_wgpu->compute.pipeline);
    if (!wined3d_context_wgpu_bind_resources(context_wgpu, state, &context_wgpu->compute.bindings,
            context_wgpu->compute.bind_group_layout, WINED3D_PIPELINE_COMPUTE))
    {
        ERR("Failed to bind resources.\n");
        return false;
    }

    memset(context_wgpu->c.dirty_compute_states, 0, sizeof(context_wgpu->c.dirty_compute_states));

    return true;
}

HRESULT wined3d_context_wgpu_init(struct wined3d_context_wgpu *context_wgpu, struct wined3d_swapchain *swapchain)
{
    struct wined3d_adapter_wgpu *adapter_wgpu;

    TRACE("context_wgpu %p, swapchain %p.\n", context_wgpu, swapchain);

    memset(context_wgpu, 0, sizeof(*context_wgpu));
    wined3d_context_init(&context_wgpu->c, swapchain);
    adapter_wgpu = wined3d_adapter_wgpu(swapchain->device->adapter);
    context_wgpu->wgpu_info = &adapter_wgpu->wgpu_info;
    context_wgpu->encoder_id = 1;
    context_wgpu->sample_count = 1;

    list_init(&context_wgpu->active_queries);
    wine_rb_init(&context_wgpu->bind_group_layouts, wined3d_bind_group_layout_wgpu_compare);
    wine_rb_init(&context_wgpu->graphics_pipelines, wined3d_graphics_pipeline_wgpu_compare);

    return WINED3D_OK;
}

void wined3d_context_wgpu_cleanup(struct wined3d_context_wgpu *context_wgpu)
{
    unsigned int i;

    wined3d_context_wgpu_submit(context_wgpu);
    wined3d_context_wgpu_wait(context_wgpu, context_wgpu->submitted_encoder_id);

    free(context_wgpu->graphics.bindings.bindings);
    free(context_wgpu->graphics.bindings.entries);
    free(context_wgpu->graphics.bindings.unfilterable);
    free(context_wgpu->compute.bindings.bindings);
    free(context_wgpu->compute.bindings.entries);
    free(context_wgpu->compute.bindings.unfilterable);
    if (context_wgpu->compute.pipeline)
        wgpuComputePipelineRelease(context_wgpu->compute.pipeline);
    wine_rb_destroy(&context_wgpu->graphics_pipelines, wined3d_context_wgpu_destroy_graphics_pipeline, NULL);
    wine_rb_destroy(&context_wgpu->bind_group_layouts, wined3d_context_wgpu_destroy_bind_group_layout, NULL);
    for (i = 0; i < ARRAY_SIZE(context_wgpu->proxy_cache); ++i)
    {
        if (context_wgpu->proxy_cache[i].texture)
            wgpuTextureRelease(context_wgpu->proxy_cache[i].texture);
    }

    wined3d_context_cleanup(&context_wgpu->c);
}
