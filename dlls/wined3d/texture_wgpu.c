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
WINE_DECLARE_DEBUG_CHANNEL(d3d_perf);

static bool wgpu_format_is_depth(WGPUTextureFormat format)
{
    switch (format)
    {
        case WGPUTextureFormat_Stencil8:
        case WGPUTextureFormat_Depth16Unorm:
        case WGPUTextureFormat_Depth24Plus:
        case WGPUTextureFormat_Depth24PlusStencil8:
        case WGPUTextureFormat_Depth32Float:
        case WGPUTextureFormat_Depth32FloatStencil8:
            return true;
        default:
            return false;
    }
}

/* Whether buffer <-> texture copies take the format: WebGPU cannot copy
 * depth24plus at all, nor write depth32float from a buffer. */
static bool wgpu_format_is_copyable(WGPUTextureFormat format, bool to_texture)
{
    switch (format)
    {
        case WGPUTextureFormat_Depth24Plus:
        case WGPUTextureFormat_Depth24PlusStencil8:
        case WGPUTextureFormat_Depth32FloatStencil8:
            return false;
        case WGPUTextureFormat_Depth32Float:
            return !to_texture;
        default:
            return true;
    }
}

BOOL wined3d_texture_wgpu_prepare_texture(struct wined3d_texture_wgpu *texture_wgpu,
        struct wined3d_context_wgpu *context_wgpu)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    struct wined3d_resource *resource = &texture_wgpu->t.resource;
    const struct wined3d_format_wgpu *format_wgpu;
    unsigned int caps, sample_count;
    WGPUTextureFormat view_format;

    if (texture_wgpu->t.flags & WINED3D_TEXTURE_RGB_ALLOCATED)
        return TRUE;

    format_wgpu = wined3d_format_wgpu(resource->format);
    if (!(desc.format = format_wgpu->texture_format))
    {
        FIXME("Unsupported texture format %s.\n", debug_d3dformat(resource->format->id));
        return FALSE;
    }

    switch (resource->type)
    {
        case WINED3D_RTYPE_TEXTURE_1D:
            /* WebGPU's 1D textures have neither mipmaps nor layers, and
             * cannot be rendered to: 1D textures are 2D, one texel high,
             * and shaders read them so. */
            desc.dimension = WGPUTextureDimension_2D;
            desc.size.width = resource->width;
            desc.size.height = 1;
            desc.size.depthOrArrayLayers = texture_wgpu->t.layer_count;
            break;
        case WINED3D_RTYPE_TEXTURE_2D:
            desc.dimension = WGPUTextureDimension_2D;
            desc.size.width = resource->width;
            desc.size.height = resource->height;
            desc.size.depthOrArrayLayers = texture_wgpu->t.layer_count;
            break;
        case WINED3D_RTYPE_TEXTURE_3D:
            desc.dimension = WGPUTextureDimension_3D;
            desc.size.width = resource->width;
            desc.size.height = resource->height;
            desc.size.depthOrArrayLayers = resource->depth;
            break;
        default:
            ERR("Invalid resource type %s.\n", debug_d3dresourcetype(resource->type));
            return FALSE;
    }

    sample_count = max(1, wined3d_resource_get_sample_count(resource));
    desc.sampleCount = sample_count > 1 ? 4 : 1;
    desc.mipLevelCount = texture_wgpu->t.level_count;

    caps = resource->format->caps[WINED3D_GL_RES_TYPE_TEX_2D];
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    /* Clears and blits render, whether or not the application does. */
    if ((resource->bind_flags & (WINED3D_BIND_RENDER_TARGET | WINED3D_BIND_DEPTH_STENCIL))
            || (caps & (WINED3D_FORMAT_CAP_RENDERTARGET | WINED3D_FORMAT_CAP_DEPTH_STENCIL)))
        desc.usage |= WGPUTextureUsage_RenderAttachment;
    if ((resource->bind_flags & WINED3D_BIND_UNORDERED_ACCESS) && desc.sampleCount == 1)
        desc.usage |= WGPUTextureUsage_StorageBinding;

    if ((view_format = format_wgpu->srgb_toggle_format))
    {
        desc.viewFormatCount = 1;
        desc.viewFormats = &view_format;
    }

    wined3d_device_wgpu_push_error_scope(device_wgpu);
    texture_wgpu->texture = wgpuDeviceCreateTexture(device_wgpu->device, &desc);
    if (!wined3d_device_wgpu_pop_error_scope(device_wgpu, "a texture") && texture_wgpu->texture)
    {
        wgpuTextureRelease(texture_wgpu->texture);
        texture_wgpu->texture = NULL;
    }
    if (!texture_wgpu->texture)
        return FALSE;
    texture_wgpu->format = desc.format;
    texture_wgpu->t.flags |= WINED3D_TEXTURE_RGB_ALLOCATED;

    TRACE("Created texture %p for wined3d texture %p.\n", texture_wgpu->texture, texture_wgpu);

    return TRUE;
}

void wined3d_texture_wgpu_release_views(struct wined3d_texture_wgpu *texture_wgpu)
{
    if (texture_wgpu->default_view)
        wgpuTextureViewRelease(texture_wgpu->default_view);
    if (texture_wgpu->default_srv)
        wgpuTextureViewRelease(texture_wgpu->default_srv);
    texture_wgpu->default_view = NULL;
    texture_wgpu->default_srv = NULL;
}

WGPUTextureView wined3d_texture_wgpu_get_default_view(struct wined3d_texture_wgpu *texture_wgpu,
        struct wined3d_context_wgpu *context_wgpu, bool srv)
{
    WGPUTextureComponentSwizzleDescriptor swizzle_desc = WGPU_TEXTURE_COMPONENT_SWIZZLE_DESCRIPTOR_INIT;
    const struct wined3d_resource *resource = &texture_wgpu->t.resource;
    WGPUTextureViewDescriptor desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    struct color_fixup_desc fixup = resource->format->color_fixup;
    WGPUTextureView *view;
    uint32_t flags = 0;

    view = srv ? &texture_wgpu->default_srv : &texture_wgpu->default_view;
    if (*view)
        return *view;

    if (!wined3d_texture_wgpu_prepare_texture(texture_wgpu, context_wgpu))
        return NULL;

    if (texture_wgpu->t.layer_count > 1)
        flags |= WINED3D_VIEW_TEXTURE_ARRAY;
    desc.format = texture_wgpu->format;
    desc.dimension = wgpu_view_dimension_from_wined3d(resource->type, flags, texture_wgpu->t.layer_count,
            srv && (resource->usage & WINED3DUSAGE_LEGACY_CUBEMAP));
    if (!srv && desc.dimension == WGPUTextureViewDimension_3D)
        desc.dimension = WGPUTextureViewDimension_2DArray;
    if (srv && wgpu_format_is_depth(desc.format))
    {
        desc.aspect = WGPUTextureAspect_DepthOnly;
        desc.format = WGPUTextureFormat_Undefined;
    }

    if (srv && !is_identity_fixup(fixup) && context_wgpu->wgpu_info->texture_component_swizzle)
    {
        static const WGPUComponentSwizzle swizzles[] =
        {
            WGPUComponentSwizzle_Zero, WGPUComponentSwizzle_One,
            WGPUComponentSwizzle_R, WGPUComponentSwizzle_G, WGPUComponentSwizzle_B, WGPUComponentSwizzle_A,
        };

        swizzle_desc.swizzle.r = swizzles[fixup.x_source];
        swizzle_desc.swizzle.g = swizzles[fixup.y_source];
        swizzle_desc.swizzle.b = swizzles[fixup.z_source];
        swizzle_desc.swizzle.a = swizzles[fixup.w_source];
        desc.nextInChain = &swizzle_desc.chain;
    }

    if (!(*view = wgpuTextureCreateView(texture_wgpu->texture, &desc)))
        ERR("Failed to create a texture view.\n");
    if (srv)
        texture_wgpu->default_srv_dimension = desc.dimension;

    return *view;
}

/* The layout of a sub-resource's data in a buffer that a copy can take:
 * rows of blocks, each row padded to 256 bytes. */
struct wgpu_copy_layout
{
    unsigned int block_width, block_height, block_byte_count;
    unsigned int row_bytes, row_pitch, row_count, depth;
    unsigned int copy_width, copy_height;
    uint64_t size;
};

static void wgpu_copy_layout_init(struct wgpu_copy_layout *layout, const struct wined3d_format *format,
        unsigned int width, unsigned int height, unsigned int depth)
{
    layout->block_width = format->block_width;
    layout->block_height = format->block_height;
    layout->block_byte_count = format->block_byte_count;
    layout->row_bytes = ((width + layout->block_width - 1) / layout->block_width) * layout->block_byte_count;
    layout->row_pitch = (layout->row_bytes + WINED3D_WGPU_ROW_ALIGNMENT - 1) & ~(WINED3D_WGPU_ROW_ALIGNMENT - 1);
    layout->row_count = (height + layout->block_height - 1) / layout->block_height;
    layout->depth = depth;
    /* Copies of block-compressed textures cover whole blocks. */
    layout->copy_width = layout->row_bytes / layout->block_byte_count * layout->block_width;
    layout->copy_height = layout->row_count * layout->block_height;
    layout->size = (uint64_t)layout->row_pitch * layout->row_count * depth;
}

static void wined3d_texture_wgpu_upload_data(struct wined3d_context *context,
        const struct wined3d_const_bo_address *src_bo_addr, const struct wined3d_format *src_format,
        const struct wined3d_box *src_box, unsigned int src_row_pitch, unsigned int src_slice_pitch,
        struct wined3d_texture *dst_texture, unsigned int dst_sub_resource_idx, unsigned int dst_location,
        unsigned int dst_x, unsigned int dst_y, unsigned int dst_z)
{
    struct wined3d_texture_wgpu *dst_texture_wgpu = wined3d_texture_wgpu(dst_texture);
    struct wined3d_context_wgpu *context_wgpu = wined3d_context_wgpu(context);
    WGPUTexelCopyBufferInfo src = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    unsigned int src_width, src_height, src_depth, y, z;
    void (*convert)(const uint8_t *src, uint32_t *dst, unsigned int width);
    struct wined3d_staging_wgpu staging;
    unsigned int packed_row_bytes = 0;
    struct wgpu_copy_layout layout;
    struct wined3d_bo_address addr;
    WGPUCommandEncoder encoder;
    WGPUExtent3D extent;
    const uint8_t *data;
    uint8_t *packed;
    size_t src_size;

    TRACE("context %p, src_bo_addr %s, src_format %s, src_box %s, src_row_pitch %u, src_slice_pitch %u, "
            "dst_texture %p, dst_sub_resource_idx %u, dst_location %s, dst_x %u, dst_y %u, dst_z %u.\n",
            context, debug_const_bo_address(src_bo_addr), debug_d3dformat(src_format->id), debug_box(src_box),
            src_row_pitch, src_slice_pitch, dst_texture, dst_sub_resource_idx,
            wined3d_debug_location(dst_location), dst_x, dst_y, dst_z);

    if (src_format->id != dst_texture->resource.format->id
            && wined3d_format_wgpu(src_format)->texture_format != dst_texture_wgpu->format)
    {
        FIXME("Unhandled format conversion (%s -> %s).\n",
                debug_d3dformat(src_format->id), debug_d3dformat(dst_texture->resource.format->id));
        return;
    }

    if (dst_location != WINED3D_LOCATION_TEXTURE_RGB)
    {
        FIXME("Unhandled location %s.\n", wined3d_debug_location(dst_location));
        return;
    }

    if (wined3d_resource_get_sample_count(&dst_texture->resource) > 1)
    {
        FIXME("Not supported for multisample textures.\n");
        return;
    }

    if (!wined3d_texture_wgpu_prepare_texture(dst_texture_wgpu, context_wgpu))
        return;

    if (!wgpu_format_is_copyable(dst_texture_wgpu->format, true))
    {
        FIXME("Cannot upload to format %s.\n", debug_d3dformat(dst_texture->resource.format->id));
        return;
    }

    src_width = src_box->right - src_box->left;
    src_height = src_box->bottom - src_box->top;
    src_depth = src_box->back - src_box->front;
    if (dst_texture->resource.type == WINED3D_RTYPE_TEXTURE_1D)
        src_row_pitch = 0;
    if (dst_texture->resource.type != WINED3D_RTYPE_TEXTURE_3D)
        src_slice_pitch = 0;
    wgpu_copy_layout_init(&layout, src_format, src_width, src_height, src_depth);

    src_size = (size_t)src_slice_pitch * (src_depth - 1) + (size_t)src_row_pitch * (layout.row_count - 1)
            + layout.row_bytes;
    /* A format WebGPU lacks is converted, row by row, to BGRA8. */
    if ((convert = wined3d_format_wgpu(src_format)->convert_upload))
    {
        packed_row_bytes = layout.row_bytes;
        layout.row_bytes = src_width * 4;
        layout.row_pitch = (layout.row_bytes + WINED3D_WGPU_ROW_ALIGNMENT - 1) & ~(WINED3D_WGPU_ROW_ALIGNMENT - 1);
        layout.size = (uint64_t)layout.row_pitch * layout.row_count * src_depth;
    }
    addr.buffer_object = src_bo_addr->buffer_object;
    addr.addr = (uint8_t *)src_bo_addr->addr + src_box->front * src_slice_pitch
            + (src_box->top / src_format->block_height) * src_row_pitch
            + (src_box->left / src_format->block_width) * src_format->block_byte_count;
    if (!(data = wined3d_context_map_bo_address(context, &addr, src_size, WINED3D_MAP_READ)))
    {
        ERR("Failed to map the source data.\n");
        return;
    }

    if (!(packed = malloc(layout.size)))
    {
        wined3d_context_unmap_bo_address(context, &addr, 0, NULL);
        return;
    }
    for (z = 0; z < src_depth; ++z)
    {
        for (y = 0; y < layout.row_count; ++y)
        {
            if (convert)
                convert(data + z * src_slice_pitch + y * src_row_pitch,
                        (uint32_t *)(packed + (z * layout.row_count + y) * layout.row_pitch), src_width);
            else
                memcpy(packed + (z * layout.row_count + y) * layout.row_pitch,
                        data + z * src_slice_pitch + y * src_row_pitch, layout.row_bytes);
        }
    }
    (void)packed_row_bytes;
    wined3d_context_unmap_bo_address(context, &addr, 0, NULL);

    if (!wined3d_context_wgpu_stage(context_wgpu, packed, layout.size, 256, &staging))
    {
        ERR("Failed to stage %s bytes.\n", wine_dbgstr_longlong(layout.size));
        free(packed);
        return;
    }
    free(packed);

    src.buffer = staging.buffer;
    src.layout.offset = staging.offset;
    src.layout.bytesPerRow = layout.row_pitch;
    src.layout.rowsPerImage = layout.row_count;
    dst.texture = dst_texture_wgpu->texture;
    dst.mipLevel = dst_sub_resource_idx % dst_texture->level_count;
    dst.origin.x = dst_x;
    dst.origin.y = dst_y;
    if (dst_texture->resource.type == WINED3D_RTYPE_TEXTURE_3D)
        dst.origin.z = dst_z;
    else
        dst.origin.z = dst_sub_resource_idx / dst_texture->level_count;
    extent.width = convert ? src_width : layout.copy_width;
    extent.height = dst_texture->resource.type == WINED3D_RTYPE_TEXTURE_1D ? 1 : layout.copy_height;
    extent.depthOrArrayLayers = src_depth;

    encoder = wined3d_context_wgpu_get_encoder(context_wgpu);
    wined3d_context_wgpu_end_passes(context_wgpu);
    wgpuCommandEncoderCopyBufferToTexture(encoder, &src, &dst, &extent);
}

static void wgpu_readback_callback(WGPUMapAsyncStatus status, WGPUStringView message, void *userdata1, void *userdata2)
{
    *(WGPUMapAsyncStatus *)userdata1 = status;
}

static WGPURenderPipeline wgpu_depth_read_pipeline(struct wined3d_device_wgpu *device_wgpu)
{
    static const char code[] =
        "@group(0) @binding(0) var d: texture_depth_2d;\n"
        "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
        "    let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
        "    return vec4f(p * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);\n"
        "}\n"
        "@fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f {\n"
        "    return vec4f(textureLoad(d, vec2i(p.xy), 0), 0.0, 0.0, 0.0);\n"
        "}\n";
    struct wined3d_null_resources_wgpu *r = &device_wgpu->null_resources;
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    WGPUShaderModule module;

    if (r->depth_read_pipeline)
        return r->depth_read_pipeline;

    wgsl.code = wgpu_string(code);
    module_desc.nextInChain = &wgsl.chain;
    if (!(module = wgpuDeviceCreateShaderModule(device_wgpu->device, &module_desc)))
        return NULL;
    desc.vertex.module = module;
    desc.vertex.entryPoint = wgpu_string("vs");
    target.format = WGPUTextureFormat_R32Float;
    fragment.module = module;
    fragment.entryPoint = wgpu_string("fs");
    fragment.targetCount = 1;
    fragment.targets = &target;
    desc.fragment = &fragment;
    r->depth_read_pipeline = wgpuDeviceCreateRenderPipeline(device_wgpu->device, &desc);
    wgpuShaderModuleRelease(module);
    return r->depth_read_pipeline;
}

/* Reads back a depth/stencil sub-resource WebGPU will not copy to a buffer
 * (depth24plus and its stencil variant): the depth drawn into an r32float
 * texture and copied from there, the stencil aspect copied as is, both
 * packed into the format asked for. */
static bool wined3d_texture_wgpu_download_depth(struct wined3d_context_wgpu *context_wgpu,
        struct wined3d_texture_wgpu *texture_wgpu, unsigned int sub_resource_idx, const struct wined3d_box *box,
        const struct wined3d_bo_address *dst_bo_addr, const struct wined3d_format *dst_format,
        unsigned int dst_row_pitch)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPUBufferMapCallbackInfo callback_info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUTextureDescriptor texture_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
    unsigned int level = sub_resource_idx % texture_wgpu->t.level_count;
    unsigned int layer = sub_resource_idx / texture_wgpu->t.level_count;
    unsigned int width = wined3d_texture_get_level_width(&texture_wgpu->t, level);
    unsigned int height = wined3d_texture_get_level_height(&texture_wgpu->t, level);
    bool stencil = texture_wgpu->format == WGPUTextureFormat_Depth24PlusStencil8
            || texture_wgpu->format == WGPUTextureFormat_Depth32FloatStencil8;
    unsigned int depth_pitch = (width * 4 + 255) & ~255u, stencil_pitch = (width + 255) & ~255u;
    WGPUMapAsyncStatus status = 0;
    WGPURenderPassEncoder pass;
    WGPUCommandEncoder encoder;
    WGPUTextureView depth_view, colour_view;
    WGPURenderPipeline pipeline;
    WGPUBuffer readback = NULL;
    WGPUTexture colour = NULL;
    WGPUBindGroup group;
    WGPUExtent3D extent;
    const uint8_t *data;
    uint8_t *dst_ptr;
    size_t dst_size;
    uint64_t size;
    unsigned int x, y;
    bool ret = false;

    switch (dst_format->id)
    {
        case WINED3DFMT_D24_UNORM_S8_UINT:
        case WINED3DFMT_X8D24_UNORM:
        case WINED3DFMT_R24G8_TYPELESS:
        case WINED3DFMT_R24_UNORM_X8_TYPELESS:
        case WINED3DFMT_D32_FLOAT:
        case WINED3DFMT_R32_FLOAT:
        case WINED3DFMT_R32_TYPELESS:
        case WINED3DFMT_D32_FLOAT_S8X24_UINT:
        case WINED3DFMT_R32G8X24_TYPELESS:
            break;
        default:
            return false;
    }
    if (!(pipeline = wgpu_depth_read_pipeline(device_wgpu)))
        return false;

    texture_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    texture_desc.size.width = width;
    texture_desc.size.height = height;
    texture_desc.size.depthOrArrayLayers = 1;
    texture_desc.format = WGPUTextureFormat_R32Float;
    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    buffer_desc.size = size = (uint64_t)depth_pitch * height + (stencil ? (uint64_t)stencil_pitch * height : 0);
    if (!(colour = wgpuDeviceCreateTexture(device_wgpu->device, &texture_desc))
            || !(readback = wgpuDeviceCreateBuffer(device_wgpu->device, &buffer_desc)))
        goto done;

    view_desc.dimension = WGPUTextureViewDimension_2D;
    view_desc.aspect = WGPUTextureAspect_DepthOnly;
    view_desc.baseMipLevel = level;
    view_desc.mipLevelCount = 1;
    view_desc.baseArrayLayer = layer;
    view_desc.arrayLayerCount = 1;
    depth_view = wgpuTextureCreateView(texture_wgpu->texture, &view_desc);
    colour_view = wgpuTextureCreateView(colour, NULL);
    entry.binding = 0;
    entry.textureView = depth_view;
    group_desc.layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
    group_desc.entryCount = 1;
    group_desc.entries = &entry;
    group = wgpuDeviceCreateBindGroup(device_wgpu->device, &group_desc);
    wgpuBindGroupLayoutRelease(group_desc.layout);

    encoder = wined3d_context_wgpu_get_encoder(context_wgpu);
    wined3d_context_wgpu_end_passes(context_wgpu);
    attachment.view = colour_view;
    attachment.loadOp = WGPULoadOp_Clear;
    attachment.storeOp = WGPUStoreOp_Store;
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &attachment;
    pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    extent.width = width;
    extent.height = height;
    extent.depthOrArrayLayers = 1;
    src.texture = colour;
    dst.buffer = readback;
    dst.layout.bytesPerRow = depth_pitch;
    dst.layout.rowsPerImage = height;
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);
    if (stencil)
    {
        src.texture = texture_wgpu->texture;
        src.mipLevel = level;
        src.origin.z = layer;
        src.aspect = WGPUTextureAspect_StencilOnly;
        dst.layout.offset = (uint64_t)depth_pitch * height;
        dst.layout.bytesPerRow = stencil_pitch;
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);
    }
    wined3d_context_wgpu_submit(context_wgpu);
    wgpuBindGroupRelease(group);
    wgpuTextureViewRelease(colour_view);
    wgpuTextureViewRelease(depth_view);

    callback_info.mode = WGPUCallbackMode_WaitAnyOnly;
    callback_info.callback = wgpu_readback_callback;
    callback_info.userdata1 = &status;
    if (!wined3d_context_wgpu_wait_future(context_wgpu, wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, size, callback_info))
            || status != WGPUMapAsyncStatus_Success || !(data = wgpuBufferGetConstMappedRange(readback, 0, size)))
    {
        ERR("Failed to map the readback buffer, status %#x.\n", status);
        goto done;
    }

    dst_size = (size_t)dst_row_pitch * (box->bottom - box->top - 1) + (box->right - box->left) * dst_format->byte_count;
    if ((dst_ptr = wined3d_context_map_bo_address(&context_wgpu->c, dst_bo_addr, dst_size, WINED3D_MAP_WRITE)))
    {
        struct wined3d_range range = {0, dst_size};

        for (y = box->top; y < box->bottom; ++y)
        {
            const float *d = (const float *)(data + (size_t)y * depth_pitch);
            const uint8_t *st = stencil ? data + (size_t)depth_pitch * height + (size_t)y * stencil_pitch : NULL;
            uint8_t *row = dst_ptr + (size_t)(y - box->top) * dst_row_pitch;

            for (x = box->left; x < box->right; ++x)
            {
                float z = min(max(d[x], 0.0f), 1.0f);
                unsigned int s = st ? st[x] : 0;

                switch (dst_format->byte_count)
                {
                    case 4:
                        if (dst_format->id == WINED3DFMT_D32_FLOAT || dst_format->id == WINED3DFMT_R32_FLOAT
                                || dst_format->id == WINED3DFMT_R32_TYPELESS)
                            ((float *)row)[x - box->left] = d[x];
                        else
                            ((uint32_t *)row)[x - box->left] = ((uint32_t)(z * 16777215.0 + 0.5) & 0xffffff) | (s << 24);
                        break;
                    case 8:
                        ((float *)row)[2 * (x - box->left)] = d[x];
                        ((uint32_t *)row)[2 * (x - box->left) + 1] = s;
                        break;
                }
            }
        }
        wined3d_context_unmap_bo_address(&context_wgpu->c, dst_bo_addr, 1, &range);
        ret = true;
    }
    wgpuBufferUnmap(readback);

done:
    if (readback)
        wgpuBufferRelease(readback);
    if (colour)
        wgpuTextureRelease(colour);
    return ret;
}

static void wined3d_texture_wgpu_download_data(struct wined3d_context *context,
        struct wined3d_texture *src_texture, unsigned int src_sub_resource_idx, unsigned int src_location,
        const struct wined3d_box *src_box, const struct wined3d_bo_address *dst_bo_addr,
        const struct wined3d_format *dst_format, unsigned int dst_x, unsigned int dst_y, unsigned int dst_z,
        unsigned int dst_row_pitch, unsigned int dst_slice_pitch)
{
    struct wined3d_texture_wgpu *src_texture_wgpu = wined3d_texture_wgpu(src_texture);
    struct wined3d_context_wgpu *context_wgpu = wined3d_context_wgpu(context);
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context->device);
    WGPUBufferMapCallbackInfo callback_info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    unsigned int src_width, src_height, src_depth, y, z;
    void (*convert)(const uint32_t *src, uint8_t *dst, unsigned int width);
    unsigned int packed_row_bytes = 0;
    WGPUMapAsyncStatus status = 0;
    struct wgpu_copy_layout layout;
    WGPUCommandEncoder encoder;
    WGPUBuffer readback;
    WGPUExtent3D extent;
    const uint8_t *data;
    uint8_t *dst_ptr;
    size_t dst_size;

    TRACE("context %p, src_texture %p, src_sub_resource_idx %u, src_location %s, src_box %s, dst_bo_addr %s, "
            "dst_format %s, dst_x %u, dst_y %u, dst_z %u, dst_row_pitch %u, dst_slice_pitch %u.\n",
            context, src_texture, src_sub_resource_idx, wined3d_debug_location(src_location),
            debug_box(src_box), debug_bo_address(dst_bo_addr), debug_d3dformat(dst_format->id),
            dst_x, dst_y, dst_z, dst_row_pitch, dst_slice_pitch);

    if (src_location != WINED3D_LOCATION_TEXTURE_RGB)
    {
        FIXME("Unhandled location %s.\n", wined3d_debug_location(src_location));
        return;
    }

    if (dst_format->id != src_texture->resource.format->id
            && wined3d_format_wgpu(dst_format)->texture_format != src_texture_wgpu->format)
    {
        FIXME("Unhandled format conversion (%s -> %s).\n",
                debug_d3dformat(src_texture->resource.format->id), debug_d3dformat(dst_format->id));
        return;
    }

    if (dst_x || dst_y || dst_z)
    {
        FIXME("Unhandled destination (%u, %u, %u).\n", dst_x, dst_y, dst_z);
        return;
    }

    if (wined3d_resource_get_sample_count(&src_texture->resource) > 1)
    {
        FIXME("Not supported for multisample textures.\n");
        return;
    }

    if (!wgpu_format_is_copyable(src_texture_wgpu->format, false))
    {
        if (!wgpu_format_is_depth(src_texture_wgpu->format) || src_texture->resource.type == WINED3D_RTYPE_TEXTURE_3D
                || !wined3d_texture_wgpu_download_depth(context_wgpu, src_texture_wgpu, src_sub_resource_idx,
                src_box, dst_bo_addr, dst_format, dst_row_pitch))
            FIXME("Cannot read back format %s.\n", debug_d3dformat(src_texture->resource.format->id));
        return;
    }

    src_width = src_box->right - src_box->left;
    src_height = src_box->bottom - src_box->top;
    src_depth = src_box->back - src_box->front;
    if (src_texture->resource.type == WINED3D_RTYPE_TEXTURE_1D)
        dst_row_pitch = 0;
    if (src_texture->resource.type != WINED3D_RTYPE_TEXTURE_3D)
        dst_slice_pitch = 0;
    wgpu_copy_layout_init(&layout, dst_format, src_width, src_height, src_depth);
    if ((convert = wined3d_format_wgpu(dst_format)->convert_download))
    {
        packed_row_bytes = layout.row_bytes;
        layout.row_bytes = src_width * 4;
        layout.row_pitch = (layout.row_bytes + WINED3D_WGPU_ROW_ALIGNMENT - 1) & ~(WINED3D_WGPU_ROW_ALIGNMENT - 1);
        layout.size = (uint64_t)layout.row_pitch * layout.row_count * src_depth;
        layout.copy_width = src_width;
    }

    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    buffer_desc.size = layout.size;
    if (!(readback = wgpuDeviceCreateBuffer(device_wgpu->device, &buffer_desc)))
    {
        ERR("Failed to create a readback buffer.\n");
        return;
    }

    src.texture = src_texture_wgpu->texture;
    src.mipLevel = src_sub_resource_idx % src_texture->level_count;
    src.origin.x = src_box->left;
    src.origin.y = src_box->top;
    if (src_texture->resource.type == WINED3D_RTYPE_TEXTURE_3D)
        src.origin.z = src_box->front;
    else
        src.origin.z = src_sub_resource_idx / src_texture->level_count;
    if (wgpu_format_is_depth(src_texture_wgpu->format))
        src.aspect = WGPUTextureAspect_DepthOnly;
    dst.buffer = readback;
    dst.layout.bytesPerRow = layout.row_pitch;
    dst.layout.rowsPerImage = layout.row_count;
    extent.width = layout.copy_width;
    extent.height = src_texture->resource.type == WINED3D_RTYPE_TEXTURE_1D ? 1 : layout.copy_height;
    extent.depthOrArrayLayers = src_depth;

    encoder = wined3d_context_wgpu_get_encoder(context_wgpu);
    wined3d_context_wgpu_end_passes(context_wgpu);
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);
    wined3d_context_wgpu_submit(context_wgpu);

    callback_info.mode = WGPUCallbackMode_WaitAnyOnly;
    callback_info.callback = wgpu_readback_callback;
    callback_info.userdata1 = &status;
    if (!wined3d_context_wgpu_wait_future(context_wgpu,
            wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, layout.size, callback_info))
            || status != WGPUMapAsyncStatus_Success
            || !(data = wgpuBufferGetConstMappedRange(readback, 0, layout.size)))
    {
        ERR("Failed to map the readback buffer, status %#x.\n", status);
        wgpuBufferRelease(readback);
        return;
    }

    dst_size = (size_t)dst_slice_pitch * (src_depth - 1) + (size_t)dst_row_pitch * (layout.row_count - 1)
            + (convert ? packed_row_bytes : layout.row_bytes);
    if ((dst_ptr = wined3d_context_map_bo_address(context, dst_bo_addr, dst_size, WINED3D_MAP_WRITE)))
    {
        struct wined3d_range range = {0, dst_size};

        for (z = 0; z < src_depth; ++z)
        {
            for (y = 0; y < layout.row_count; ++y)
            {
                if (convert)
                    convert((const uint32_t *)(data + (z * layout.row_count + y) * layout.row_pitch),
                            dst_ptr + z * dst_slice_pitch + y * dst_row_pitch, src_width);
                else
                    memcpy(dst_ptr + z * dst_slice_pitch + y * dst_row_pitch,
                            data + (z * layout.row_count + y) * layout.row_pitch, layout.row_bytes);
            }
        }
        wined3d_context_unmap_bo_address(context, dst_bo_addr, 1, &range);
    }

    wgpuBufferUnmap(readback);
    wgpuBufferRelease(readback);
}

/* Clears a sub-resource to its clear value with a render pass. */
static bool wined3d_texture_wgpu_clear(struct wined3d_texture_wgpu *texture_wgpu,
        unsigned int sub_resource_idx, struct wined3d_context *context)
{
    struct wined3d_texture_sub_resource *sub_resource = &texture_wgpu->t.sub_resources[sub_resource_idx];
    struct wined3d_context_wgpu *context_wgpu = wined3d_context_wgpu(context);
    WGPURenderPassDepthStencilAttachment ds_attachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    WGPURenderPassColorAttachment colour_attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    const struct wined3d_format *format = texture_wgpu->t.resource.format;
    unsigned int level, layer, slice, slice_count;
    WGPURenderPassEncoder pass;
    WGPUCommandEncoder encoder;
    WGPUTextureView view;

    if ((texture_wgpu->t.resource.format_attrs & WINED3D_FORMAT_ATTR_COMPRESSED)
            || !(format->caps[WINED3D_GL_RES_TYPE_TEX_2D]
            & (WINED3D_FORMAT_CAP_RENDERTARGET | WINED3D_FORMAT_CAP_DEPTH_STENCIL)))
    {
        struct wined3d_color *c = &sub_resource->clear_value.colour;
        struct wined3d_bo_address addr;

        if (c->r || c->g || c->b || c->a)
            FIXME("Unrenderable resource %p is cleared to a non-zero colour.\n", &texture_wgpu->t);

        if (!wined3d_texture_prepare_location(&texture_wgpu->t, sub_resource_idx, context, WINED3D_LOCATION_SYSMEM))
            return false;
        wined3d_texture_get_bo_address(&texture_wgpu->t, sub_resource_idx, &addr, WINED3D_LOCATION_SYSMEM);
        memset(addr.addr, 0, sub_resource->size);
        wined3d_texture_validate_location(&texture_wgpu->t, sub_resource_idx, WINED3D_LOCATION_SYSMEM);
        return true;
    }

    if (!wined3d_texture_wgpu_prepare_texture(texture_wgpu, context_wgpu))
        return false;

    level = sub_resource_idx % texture_wgpu->t.level_count;
    layer = sub_resource_idx / texture_wgpu->t.level_count;
    slice_count = texture_wgpu->t.resource.type == WINED3D_RTYPE_TEXTURE_3D
            ? wined3d_texture_get_level_depth(&texture_wgpu->t, level) : 1;

    view_desc.format = texture_wgpu->format;
    view_desc.dimension = WGPUTextureViewDimension_2D;
    view_desc.baseMipLevel = level;
    view_desc.mipLevelCount = 1;
    view_desc.baseArrayLayer = texture_wgpu->t.resource.type == WINED3D_RTYPE_TEXTURE_3D ? 0 : layer;
    view_desc.arrayLayerCount = 1;
    if (texture_wgpu->t.resource.type == WINED3D_RTYPE_TEXTURE_3D)
        view_desc.dimension = WGPUTextureViewDimension_3D;
    if (!(view = wgpuTextureCreateView(texture_wgpu->texture, &view_desc)))
        return false;

    encoder = wined3d_context_wgpu_get_encoder(context_wgpu);
    wined3d_context_wgpu_end_passes(context_wgpu);

    for (slice = 0; slice < slice_count; ++slice)
    {
        pass_desc.colorAttachmentCount = 0;
        pass_desc.depthStencilAttachment = NULL;
        if (format->depth_size || format->stencil_size)
        {
            ds_attachment.view = view;
            if (format->depth_size)
            {
                ds_attachment.depthLoadOp = WGPULoadOp_Clear;
                ds_attachment.depthStoreOp = WGPUStoreOp_Store;
                ds_attachment.depthClearValue = sub_resource->clear_value.depth;
            }
            if (format->stencil_size)
            {
                ds_attachment.stencilLoadOp = WGPULoadOp_Clear;
                ds_attachment.stencilStoreOp = WGPUStoreOp_Store;
                ds_attachment.stencilClearValue = sub_resource->clear_value.stencil;
            }
            pass_desc.depthStencilAttachment = &ds_attachment;
        }
        else
        {
            colour_attachment.view = view;
            if (texture_wgpu->t.resource.type == WINED3D_RTYPE_TEXTURE_3D)
                colour_attachment.depthSlice = slice;
            colour_attachment.loadOp = WGPULoadOp_Clear;
            colour_attachment.storeOp = WGPUStoreOp_Store;
            wined3d_format_colour_to_wgpu(format, &sub_resource->clear_value.colour, &colour_attachment.clearValue);
            pass_desc.colorAttachmentCount = 1;
            pass_desc.colorAttachments = &colour_attachment;
        }
        pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }
    wgpuTextureViewRelease(view);

    wined3d_texture_validate_location(&texture_wgpu->t, sub_resource_idx, WINED3D_LOCATION_TEXTURE_RGB);
    return true;
}

static BOOL wined3d_texture_wgpu_load_texture(struct wined3d_texture_wgpu *texture_wgpu,
        unsigned int sub_resource_idx, struct wined3d_context *context)
{
    struct wined3d_texture_sub_resource *sub_resource;
    unsigned int level, row_pitch, slice_pitch;
    struct wined3d_bo_address data;
    struct wined3d_box src_box;

    sub_resource = &texture_wgpu->t.sub_resources[sub_resource_idx];

    if (sub_resource->locations & WINED3D_LOCATION_CLEARED)
    {
        if (!wined3d_texture_wgpu_clear(texture_wgpu, sub_resource_idx, context))
            return FALSE;

        if (sub_resource->locations & WINED3D_LOCATION_TEXTURE_RGB)
            return TRUE;
    }

    if (!(sub_resource->locations & wined3d_texture_sysmem_locations))
    {
        ERR("Unimplemented load from %s.\n", wined3d_debug_location(sub_resource->locations));
        return FALSE;
    }

    level = sub_resource_idx % texture_wgpu->t.level_count;
    wined3d_texture_get_memory(&texture_wgpu->t, sub_resource_idx, context, &data);
    wined3d_texture_get_level_box(&texture_wgpu->t, level, &src_box);
    wined3d_texture_get_pitch(&texture_wgpu->t, level, &row_pitch, &slice_pitch);
    wined3d_texture_wgpu_upload_data(context, wined3d_const_bo_address(&data), texture_wgpu->t.resource.format,
            &src_box, row_pitch, slice_pitch, &texture_wgpu->t, sub_resource_idx,
            WINED3D_LOCATION_TEXTURE_RGB, 0, 0, 0);

    return TRUE;
}

static BOOL wined3d_texture_wgpu_load_sysmem(struct wined3d_texture_wgpu *texture_wgpu,
        unsigned int sub_resource_idx, struct wined3d_context *context, unsigned int location)
{
    struct wined3d_texture_sub_resource *sub_resource;
    unsigned int level, row_pitch, slice_pitch;
    struct wined3d_bo_address data;
    struct wined3d_box src_box;

    sub_resource = &texture_wgpu->t.sub_resources[sub_resource_idx];
    /* A clear not made yet is made on the GPU, and read back from there. */
    if (!(sub_resource->locations & WINED3D_LOCATION_TEXTURE_RGB)
            && (sub_resource->locations & WINED3D_LOCATION_CLEARED)
            && wined3d_texture_wgpu_load_texture(texture_wgpu, sub_resource_idx, context))
        wined3d_texture_validate_location(&texture_wgpu->t, sub_resource_idx, WINED3D_LOCATION_TEXTURE_RGB);
    if (!(sub_resource->locations & WINED3D_LOCATION_TEXTURE_RGB))
    {
        ERR("Unimplemented load from %s.\n", wined3d_debug_location(sub_resource->locations));
        return FALSE;
    }

    level = sub_resource_idx % texture_wgpu->t.level_count;
    wined3d_texture_get_bo_address(&texture_wgpu->t, sub_resource_idx, &data, location);
    wined3d_texture_get_level_box(&texture_wgpu->t, level, &src_box);
    wined3d_texture_get_pitch(&texture_wgpu->t, level, &row_pitch, &slice_pitch);
    wined3d_texture_wgpu_download_data(context, &texture_wgpu->t, sub_resource_idx, WINED3D_LOCATION_TEXTURE_RGB,
            &src_box, &data, texture_wgpu->t.resource.format, 0, 0, 0, row_pitch, slice_pitch);

    return TRUE;
}

static BOOL wined3d_texture_wgpu_prepare_location(struct wined3d_texture *texture,
        unsigned int sub_resource_idx, struct wined3d_context *context, unsigned int location)
{
    switch (location)
    {
        case WINED3D_LOCATION_SYSMEM:
            return texture->sub_resources[sub_resource_idx].user_memory ? TRUE
                    : wined3d_resource_prepare_sysmem(&texture->resource);

        case WINED3D_LOCATION_TEXTURE_RGB:
            return wined3d_texture_wgpu_prepare_texture(wined3d_texture_wgpu(texture), wined3d_context_wgpu(context));

        default:
            FIXME("Unhandled location %s.\n", wined3d_debug_location(location));
            return FALSE;
    }
}

static BOOL wined3d_texture_wgpu_load_location(struct wined3d_texture *texture,
        unsigned int sub_resource_idx, struct wined3d_context *context, uint32_t location)
{
    if (!wined3d_texture_wgpu_prepare_location(texture, sub_resource_idx, context, location))
        return FALSE;

    switch (location)
    {
        case WINED3D_LOCATION_TEXTURE_RGB:
            return wined3d_texture_wgpu_load_texture(wined3d_texture_wgpu(texture), sub_resource_idx, context);

        case WINED3D_LOCATION_SYSMEM:
            return wined3d_texture_wgpu_load_sysmem(wined3d_texture_wgpu(texture), sub_resource_idx, context, location);

        default:
            FIXME("Unimplemented location %s.\n", wined3d_debug_location(location));
            return FALSE;
    }
}

static void wined3d_texture_wgpu_unload_location(struct wined3d_texture *texture,
        struct wined3d_context *context, unsigned int location)
{
    struct wined3d_texture_wgpu *texture_wgpu = wined3d_texture_wgpu(texture);

    TRACE("texture %p, context %p, location %s.\n", texture, context, wined3d_debug_location(location));

    switch (location)
    {
        case WINED3D_LOCATION_TEXTURE_RGB:
            wined3d_texture_wgpu_release_views(texture_wgpu);
            if (texture_wgpu->texture)
            {
                wgpuTextureRelease(texture_wgpu->texture);
                texture_wgpu->texture = NULL;
            }
            break;

        case WINED3D_LOCATION_BUFFER:
        case WINED3D_LOCATION_TEXTURE_SRGB:
        case WINED3D_LOCATION_RB_MULTISAMPLE:
        case WINED3D_LOCATION_RB_RESOLVED:
            break;

        default:
            ERR("Unhandled location %s.\n", wined3d_debug_location(location));
            break;
    }
}

static const struct wined3d_texture_ops wined3d_texture_wgpu_ops =
{
    wined3d_texture_wgpu_prepare_location,
    wined3d_texture_wgpu_load_location,
    wined3d_texture_wgpu_unload_location,
    wined3d_texture_wgpu_upload_data,
    wined3d_texture_wgpu_download_data,
};

HRESULT wined3d_texture_wgpu_init(struct wined3d_texture_wgpu *texture_wgpu, struct wined3d_device *device,
        const struct wined3d_resource_desc *desc, unsigned int layer_count, unsigned int level_count,
        uint32_t flags, void *parent, const struct wined3d_parent_ops *parent_ops)
{
    TRACE("texture_wgpu %p, device %p, desc %p, layer_count %u, "
            "level_count %u, flags %#x, parent %p, parent_ops %p.\n",
            texture_wgpu, device, desc, layer_count,
            level_count, flags, parent, parent_ops);

    return wined3d_texture_init(&texture_wgpu->t, desc, layer_count, level_count,
            flags, device, parent, parent_ops, &texture_wgpu[1], &wined3d_texture_wgpu_ops);
}

/* The blitter: clears are made at the next render pass, or by the CPU
 * blitter when partial; blits copy when they can, and the CPU blitter does
 * the rest. */

static void wgpu_blitter_destroy(struct wined3d_blitter *blitter, struct wined3d_context *context)
{
    struct wined3d_blitter *next;

    TRACE("blitter %p, context %p.\n", blitter, context);

    if ((next = blitter->next))
        next->ops->blitter_destroy(next, context);

    free(blitter);
}

/* Clears the depth or stencil of a whole view, keeping the other: a render
 * pass that loads one aspect and clears the other. */
static bool wgpu_clear_ds_aspect(struct wined3d_device *device, struct wined3d_rendertarget_view *view,
        uint32_t flags, float depth, unsigned int stencil)
{
    WGPURenderPassDepthStencilAttachment ds = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    struct wined3d_context_wgpu *context_wgpu;
    WGPURenderPassEncoder pass;
    WGPUCommandEncoder encoder;

    if (view->resource->type == WINED3D_RTYPE_BUFFER)
        return false;

    context_wgpu = wined3d_context_wgpu(context_acquire(device, NULL, 0));
    wined3d_rendertarget_view_load_location(view, &context_wgpu->c, view->resource->draw_binding);
    if (!(ds.view = wined3d_rendertarget_view_wgpu_get_view(wined3d_rendertarget_view_wgpu(view), context_wgpu))
            || !(encoder = wined3d_context_wgpu_get_encoder(context_wgpu)))
    {
        context_release(&context_wgpu->c);
        return false;
    }
    wined3d_context_wgpu_end_passes(context_wgpu);

    if (view->format->depth_size)
    {
        ds.depthLoadOp = flags & WINED3DCLEAR_ZBUFFER ? WGPULoadOp_Clear : WGPULoadOp_Load;
        ds.depthStoreOp = WGPUStoreOp_Store;
        ds.depthClearValue = depth;
    }
    if (view->format->stencil_size)
    {
        ds.stencilLoadOp = flags & WINED3DCLEAR_STENCIL ? WGPULoadOp_Clear : WGPULoadOp_Load;
        ds.stencilStoreOp = WGPUStoreOp_Store;
        ds.stencilClearValue = stencil;
    }
    desc.depthStencilAttachment = &ds;
    pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    wined3d_rendertarget_view_validate_location(view, view->resource->draw_binding);
    wined3d_rendertarget_view_invalidate_location(view, ~view->resource->draw_binding);
    context_release(&context_wgpu->c);
    return true;
}

/* A full colour clear made now, in a pass of its own, through the view's
 * own format: for what a delayed clear cannot express (a typeless texture's
 * clear depends on the view's format). */
static bool wgpu_clear_rt_now(struct wined3d_device *device, struct wined3d_rendertarget_view *view,
        const struct wined3d_color *colour)
{
    WGPURenderPassColorAttachment a = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    struct wined3d_context_wgpu *context_wgpu;
    WGPURenderPassEncoder pass;
    WGPUCommandEncoder encoder;

    if (view->resource->type == WINED3D_RTYPE_BUFFER || view->resource->type == WINED3D_RTYPE_TEXTURE_3D)
        return false;

    context_wgpu = wined3d_context_wgpu(context_acquire(device, NULL, 0));
    wined3d_rendertarget_view_prepare_location(view, &context_wgpu->c, view->resource->draw_binding);
    if (!(a.view = wined3d_rendertarget_view_wgpu_get_view(wined3d_rendertarget_view_wgpu(view), context_wgpu))
            || !(encoder = wined3d_context_wgpu_get_encoder(context_wgpu)))
    {
        context_release(&context_wgpu->c);
        return false;
    }
    wined3d_context_wgpu_end_passes(context_wgpu);

    a.loadOp = WGPULoadOp_Clear;
    a.storeOp = WGPUStoreOp_Store;
    wined3d_format_colour_to_wgpu(view->format, colour, &a.clearValue);
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &a;
    pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    wined3d_rendertarget_view_validate_location(view, view->resource->draw_binding);
    wined3d_rendertarget_view_invalidate_location(view, ~view->resource->draw_binding);
    context_release(&context_wgpu->c);
    return true;
}

/* A pipeline that writes the blend constant to a colour attachment, or the
 * viewport's depth and the stencil reference to a depth/stencil one, where
 * the scissor lets it: a clear of rectangles. */
static WGPURenderPipeline wgpu_clear_pipeline(struct wined3d_device_wgpu *device_wgpu, WGPUTextureFormat format,
        unsigned int samples, uint32_t flags)
{
    static const char code[] =
        "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
        "    let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
        "    return vec4f(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n"
        "@fragment fn fs() -> @location(0) vec4f {\n"
        "    return vec4f(1.0);\n"
        "}\n";
    struct wined3d_null_resources_wgpu *r = &device_wgpu->null_resources;
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    WGPURenderPipeline pipeline;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(r->clear_pipelines); ++i)
    {
        if (r->clear_pipelines[i].pipeline && r->clear_pipelines[i].format == format
                && r->clear_pipelines[i].samples == samples && r->clear_pipelines[i].flags == flags)
            return r->clear_pipelines[i].pipeline;
    }

    if (!r->clear_module)
    {
        WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;

        wgsl.code = wgpu_string(code);
        module_desc.nextInChain = &wgsl.chain;
        if (!(r->clear_module = wgpuDeviceCreateShaderModule(device_wgpu->device, &module_desc)))
            return NULL;
    }

    desc.vertex.module = r->clear_module;
    desc.vertex.entryPoint = wgpu_string("vs");
    desc.multisample.count = samples;
    if (flags & WINED3DCLEAR_TARGET)
    {
        blend.color.srcFactor = WGPUBlendFactor_Constant;
        blend.color.dstFactor = WGPUBlendFactor_Zero;
        blend.alpha = blend.color;
        target.format = format;
        target.blend = &blend;
        fragment.module = r->clear_module;
        fragment.entryPoint = wgpu_string("fs");
        fragment.targetCount = 1;
        fragment.targets = &target;
        desc.fragment = &fragment;
    }
    else
    {
        ds.format = format;
        ds.depthWriteEnabled = (flags & WINED3DCLEAR_ZBUFFER) ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        ds.depthCompare = WGPUCompareFunction_Always;
        /* Stencil state only where there is a stencil to write. */
        if (flags & WINED3DCLEAR_STENCIL)
        {
            ds.stencilFront.compare = WGPUCompareFunction_Always;
            ds.stencilFront.passOp = WGPUStencilOperation_Replace;
            ds.stencilBack = ds.stencilFront;
            ds.stencilWriteMask = 0xff;
        }
        desc.depthStencil = &ds;
    }

    wined3d_device_wgpu_push_error_scope(device_wgpu);
    pipeline = wgpuDeviceCreateRenderPipeline(device_wgpu->device, &desc);
    if (!wined3d_device_wgpu_pop_error_scope(device_wgpu, "a clear pipeline") && pipeline)
    {
        wgpuRenderPipelineRelease(pipeline);
        pipeline = NULL;
    }
    if (!pipeline)
        return NULL;

    i = ARRAY_SIZE(r->clear_pipelines) - 1;
    if (r->clear_pipelines[i].pipeline)
        wgpuRenderPipelineRelease(r->clear_pipelines[i].pipeline);
    memmove(&r->clear_pipelines[1], &r->clear_pipelines[0], i * sizeof(r->clear_pipelines[0]));
    r->clear_pipelines[0].format = format;
    r->clear_pipelines[0].samples = samples;
    r->clear_pipelines[0].flags = flags;
    r->clear_pipelines[0].pipeline = pipeline;
    return pipeline;
}

/* A clear of rectangles of one view, in a pass of its own. */
static bool wgpu_clear_rects(struct wined3d_device *device, struct wined3d_rendertarget_view *view,
        unsigned int rect_count, const RECT *clear_rects, const RECT *draw_rect, uint32_t flags,
        const struct wined3d_color *colour, float depth, unsigned int stencil)
{
    WGPURenderPassDepthStencilAttachment ds = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    WGPURenderPassColorAttachment a = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(device);
    struct wined3d_context_wgpu *context_wgpu;
    unsigned int samples, i, drawn = 0;
    WGPURenderPipeline pipeline;
    WGPURenderPassEncoder pass;
    WGPUCommandEncoder encoder;
    WGPUTextureFormat format;
    WGPUTextureView attachment;
    RECT bounds, r;

    if (view->resource->type == WINED3D_RTYPE_BUFFER || view->resource->type == WINED3D_RTYPE_TEXTURE_3D)
        return false;
    /* Integer formats do not blend. */
    if ((flags & WINED3DCLEAR_TARGET) && (view->format->attrs & WINED3D_FORMAT_ATTR_INTEGER))
        return false;

    context_wgpu = wined3d_context_wgpu(context_acquire(device, NULL, 0));
    wined3d_rendertarget_view_load_location(view, &context_wgpu->c, view->resource->draw_binding);
    if (!(attachment = wined3d_rendertarget_view_wgpu_get_view(wined3d_rendertarget_view_wgpu(view), context_wgpu))
            || !(encoder = wined3d_context_wgpu_get_encoder(context_wgpu)))
    {
        context_release(&context_wgpu->c);
        return false;
    }
    format = wgpuTextureGetFormat(wined3d_texture_wgpu(texture_from_resource(view->resource))->texture);
    if (flags & WINED3DCLEAR_TARGET)
    {
        WGPUTextureFormat view_format = wined3d_format_wgpu(view->format)->texture_format;

        if (view_format == wined3d_format_wgpu(view->resource->format)->srgb_toggle_format)
            format = view_format;
    }
    samples = max(1, wined3d_resource_get_sample_count(view->resource)) > 1 ? 4 : 1;
    if (!(pipeline = wgpu_clear_pipeline(device_wgpu, format, samples, flags)))
    {
        context_release(&context_wgpu->c);
        return false;
    }
    wined3d_context_wgpu_end_passes(context_wgpu);

    if (flags & WINED3DCLEAR_TARGET)
    {
        a.view = attachment;
        a.loadOp = WGPULoadOp_Load;
        a.storeOp = WGPUStoreOp_Store;
        desc.colorAttachmentCount = 1;
        desc.colorAttachments = &a;
    }
    else
    {
        ds.view = attachment;
        if (view->format->depth_size)
        {
            ds.depthLoadOp = WGPULoadOp_Load;
            ds.depthStoreOp = WGPUStoreOp_Store;
        }
        if (view->format->stencil_size)
        {
            ds.stencilLoadOp = WGPULoadOp_Load;
            ds.stencilStoreOp = WGPUStoreOp_Store;
        }
        desc.depthStencilAttachment = &ds;
    }
    pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    if (flags & WINED3DCLEAR_TARGET)
    {
        WGPUColor c;

        wined3d_format_colour_to_wgpu(view->format, colour, &c);
        wgpuRenderPassEncoderSetBlendConstant(pass, &c);
    }
    else
    {
        /* The triangle's z is 0: its depth, the viewport's range. */
        wgpuRenderPassEncoderSetViewport(pass, 0.0f, 0.0f, view->width, view->height,
                min(max(depth, 0.0f), 1.0f), min(max(depth, 0.0f), 1.0f));
        wgpuRenderPassEncoderSetStencilReference(pass, stencil);
    }
    SetRect(&bounds, 0, 0, view->width, view->height);
    if (draw_rect)
        IntersectRect(&bounds, &bounds, draw_rect);
    for (i = 0; i < rect_count; ++i)
    {
        if (!IntersectRect(&r, &clear_rects[i], &bounds))
            continue;
        wgpuRenderPassEncoderSetScissorRect(pass, r.left, r.top, r.right - r.left, r.bottom - r.top);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        ++drawn;
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    wined3d_rendertarget_view_validate_location(view, view->resource->draw_binding);
    wined3d_rendertarget_view_invalidate_location(view, ~view->resource->draw_binding);
    context_release(&context_wgpu->c);
    TRACE("Cleared %u of %u rectangles.\n", drawn, rect_count);
    return true;
}

static void wgpu_blitter_clear(struct wined3d_blitter *blitter, struct wined3d_device *device,
        unsigned int rt_count, const struct wined3d_fb_state *fb, unsigned int rect_count, const RECT *clear_rects,
        const RECT *draw_rect, uint32_t flags, const struct wined3d_color *colour, float depth, unsigned int stencil)
{
    struct wined3d_rendertarget_view *view;
    struct wined3d_fb_state next_fb = {0};
    unsigned int i, next_rt_count = 0;
    struct wined3d_blitter *next;
    uint32_t next_flags = 0;

    TRACE("blitter %p, device %p, rt_count %u, fb %p, rect_count %u, clear_rects %p, "
            "draw_rect %s, flags %#x, colour %s, depth %.8e, stencil %#x.\n",
            blitter, device, rt_count, fb, rect_count, clear_rects,
            wine_dbgstr_rect(draw_rect), flags, debug_color(colour), depth, stencil);

    if (!rect_count)
    {
        rect_count = 1;
        clear_rects = draw_rect;
    }

    if (flags & WINED3DCLEAR_TARGET)
    {
        for (i = 0; i < rt_count; ++i)
        {
            if (!(view = fb->render_targets[i]))
                continue;

            /* A typeless texture's bytes depend on the view's format, which
             * a delayed clear does not keep; zero is zero in all of them. */
            if (view->resource->type != WINED3D_RTYPE_BUFFER
                    && wined3d_rendertarget_view_is_full_clear(view, draw_rect, clear_rects)
                    && !wined3d_rendertarget_view_use_cpu_clear(view)
                    && (!wined3d_format_is_typeless(view->resource->format)
                    || (!colour->r && !colour->g && !colour->b && !colour->a)))
            {
                struct wined3d_texture *texture = texture_from_resource(view->resource);

                wined3d_rendertarget_view_validate_location(view, WINED3D_LOCATION_CLEARED);
                wined3d_rendertarget_view_invalidate_location(view, ~WINED3D_LOCATION_CLEARED);
                texture->sub_resources[view->sub_resource_idx].clear_value.colour = *colour;
                continue;
            }
            if (view->resource->type != WINED3D_RTYPE_BUFFER
                    && wined3d_rendertarget_view_is_full_clear(view, draw_rect, clear_rects)
                    && !wined3d_rendertarget_view_use_cpu_clear(view)
                    && wgpu_clear_rt_now(device, view, colour))
                continue;

            if (!wined3d_rendertarget_view_use_cpu_clear(view)
                    && wgpu_clear_rects(device, view, rect_count, clear_rects, draw_rect, WINED3DCLEAR_TARGET,
                    colour, depth, stencil))
                continue;

            TRACE_(d3d_perf)("Partial clear of render target %u.\n", i);
            next_fb.render_targets[i] = view;
            next_rt_count = i + 1;
            next_flags |= WINED3DCLEAR_TARGET;
        }
    }

    if ((flags & (WINED3DCLEAR_ZBUFFER | WINED3DCLEAR_STENCIL)) && (view = fb->depth_stencil))
    {
        uint32_t full_flags = 0;

        if (view->format->depth_size)
            full_flags |= WINED3DCLEAR_ZBUFFER;
        if (view->format->stencil_size)
            full_flags |= WINED3DCLEAR_STENCIL;

        if (wined3d_rendertarget_view_is_full_clear(view, draw_rect, clear_rects)
                && (flags & full_flags) == full_flags)
        {
            struct wined3d_texture *texture = texture_from_resource(view->resource);

            texture->sub_resources[view->sub_resource_idx].clear_value.depth = depth;
            texture->sub_resources[view->sub_resource_idx].clear_value.stencil = stencil;
            wined3d_rendertarget_view_validate_location(view, WINED3D_LOCATION_CLEARED);
            wined3d_rendertarget_view_invalidate_location(view, ~WINED3D_LOCATION_CLEARED);
        }
        else if ((wined3d_rendertarget_view_is_full_clear(view, draw_rect, clear_rects)
                && wgpu_clear_ds_aspect(device, view, flags, depth, stencil))
                || wgpu_clear_rects(device, view, rect_count, clear_rects, draw_rect,
                flags & (WINED3DCLEAR_ZBUFFER | WINED3DCLEAR_STENCIL), colour, depth, stencil))
        {
        }
        else
        {
            FIXME("Partial depth/stencil clear, flags %#x.\n", flags);
            next_fb.depth_stencil = view;
            next_flags |= flags & (WINED3DCLEAR_ZBUFFER | WINED3DCLEAR_STENCIL);
        }
    }

    if (!next_flags)
        return;

    if (!(next = blitter->next))
    {
        ERR("No blitter to handle clear.\n");
        return;
    }

    TRACE("Forwarding to blitter %p.\n", next);
    next->ops->blitter_clear(next, device, next_rt_count, &next_fb, rect_count,
            clear_rects, draw_rect, next_flags, colour, depth, stencil);
}

static DWORD wgpu_blitter_blit(struct wined3d_blitter *blitter, enum wined3d_blit_op op,
        struct wined3d_context *context, struct wined3d_texture *src_texture, unsigned int src_sub_resource_idx,
        DWORD src_location, const RECT *src_rect, struct wined3d_texture *dst_texture,
        unsigned int dst_sub_resource_idx, DWORD dst_location, const RECT *dst_rect,
        const struct wined3d_color_key *colour_key, enum wined3d_texture_filter_type filter,
        const struct wined3d_format *resolve_format)
{
    struct wined3d_texture_wgpu *src_texture_wgpu = wined3d_texture_wgpu(src_texture);
    struct wined3d_texture_wgpu *dst_texture_wgpu = wined3d_texture_wgpu(dst_texture);
    struct wined3d_context_wgpu *context_wgpu = wined3d_context_wgpu(context);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    const struct wined3d_format_wgpu *src_format, *dst_format;
    unsigned int src_level, dst_level, src_samples, dst_samples;
    WGPUCommandEncoder encoder;
    struct wined3d_blitter *next;
    WGPUExtent3D extent;

    TRACE("blitter %p, op %#x, context %p, src_texture %p, src_sub_resource_idx %u, src_location %s, "
            "src_rect %s, dst_texture %p, dst_sub_resource_idx %u, dst_location %s, dst_rect %s, "
            "colour_key %p, filter %s, resolve format %p.\n",
            blitter, op, context, src_texture, src_sub_resource_idx, wined3d_debug_location(src_location),
            wine_dbgstr_rect(src_rect), dst_texture, dst_sub_resource_idx, wined3d_debug_location(dst_location),
            wine_dbgstr_rect(dst_rect), colour_key, debug_d3dtexturefiltertype(filter), resolve_format);

    src_format = wined3d_format_wgpu(src_texture->resource.format);
    dst_format = wined3d_format_wgpu(dst_texture->resource.format);
    src_level = src_sub_resource_idx % src_texture->level_count;
    dst_level = dst_sub_resource_idx % dst_texture->level_count;
    src_samples = max(1, wined3d_resource_get_sample_count(&src_texture->resource));
    dst_samples = max(1, wined3d_resource_get_sample_count(&dst_texture->resource));

    if (!(src_texture->resource.access & WINED3D_RESOURCE_ACCESS_GPU)
            || !(dst_texture->resource.access & WINED3D_RESOURCE_ACCESS_GPU)
            || (op != WINED3D_BLIT_OP_COLOR_BLIT && op != WINED3D_BLIT_OP_RAW_BLIT
            && op != WINED3D_BLIT_OP_DEPTH_BLIT)
            || src_rect->right - src_rect->left != dst_rect->right - dst_rect->left
            || src_rect->bottom - src_rect->top != dst_rect->bottom - dst_rect->top
            || !src_format->texture_format || !dst_format->texture_format
            || (src_format->texture_format != dst_format->texture_format
            && src_format->srgb_toggle_format != dst_format->texture_format)
            || (op != WINED3D_BLIT_OP_RAW_BLIT && (!is_identity_fixup(src_texture->resource.format->color_fixup)
            || !is_identity_fixup(dst_texture->resource.format->color_fixup))))
        goto next;

    if (src_samples != dst_samples)
    {
        /* A resolve: a render pass with the destination as the resolve
         * target, which WebGPU takes only for a whole single sub-resource
         * of the same format. */
        WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        WGPURenderPassEncoder pass;

        if (dst_samples != 1 || src_format->texture_format != dst_format->texture_format
                || !wined3d_texture_is_full_rect(src_texture, src_level, src_rect)
                || !wined3d_texture_is_full_rect(dst_texture, dst_level, dst_rect)
                || src_texture->resource.width != dst_texture->resource.width
                || src_texture->resource.height != dst_texture->resource.height
                || src_format->f.depth_size || src_format->f.stencil_size)
        {
            FIXME("Unhandled resolve.\n");
            goto next;
        }

        if (!wined3d_texture_load_location(src_texture, src_sub_resource_idx, context, WINED3D_LOCATION_TEXTURE_RGB)
                || !wined3d_texture_prepare_location(dst_texture, dst_sub_resource_idx, context,
                WINED3D_LOCATION_TEXTURE_RGB))
            goto next;

        encoder = wined3d_context_wgpu_get_encoder(context_wgpu);
        wined3d_context_wgpu_end_passes(context_wgpu);

        view_desc.format = src_format->texture_format;
        view_desc.dimension = WGPUTextureViewDimension_2D;
        view_desc.baseMipLevel = src_level;
        view_desc.mipLevelCount = 1;
        view_desc.baseArrayLayer = src_sub_resource_idx / src_texture->level_count;
        view_desc.arrayLayerCount = 1;
        attachment.view = wgpuTextureCreateView(src_texture_wgpu->texture, &view_desc);
        view_desc.baseMipLevel = dst_level;
        view_desc.baseArrayLayer = dst_sub_resource_idx / dst_texture->level_count;
        attachment.resolveTarget = wgpuTextureCreateView(dst_texture_wgpu->texture, &view_desc);
        attachment.loadOp = WGPULoadOp_Load;
        attachment.storeOp = WGPUStoreOp_Store;
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &attachment;
        pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        wgpuTextureViewRelease(attachment.view);
        wgpuTextureViewRelease(attachment.resolveTarget);
    }
    else
    {
        /* Depth textures copy only whole sub-resources, and depth24plus not
         * at all. */
        if ((src_format->f.depth_size || src_format->f.stencil_size)
                && (!wgpu_format_is_copyable(src_format->texture_format, false)
                || !wined3d_texture_is_full_rect(src_texture, src_level, src_rect)
                || !wined3d_texture_is_full_rect(dst_texture, dst_level, dst_rect)))
            goto next;

        if (!wined3d_texture_load_location(src_texture, src_sub_resource_idx, context, WINED3D_LOCATION_TEXTURE_RGB))
            goto next;
        if (wined3d_texture_is_full_rect(dst_texture, dst_level, dst_rect))
        {
            if (!wined3d_texture_prepare_location(dst_texture, dst_sub_resource_idx, context,
                    WINED3D_LOCATION_TEXTURE_RGB))
                goto next;
        }
        else if (!wined3d_texture_load_location(dst_texture, dst_sub_resource_idx, context,
                WINED3D_LOCATION_TEXTURE_RGB))
        {
            goto next;
        }

        src.texture = src_texture_wgpu->texture;
        src.mipLevel = src_level;
        src.origin.x = src_rect->left;
        src.origin.y = src_rect->top;
        src.origin.z = src_texture->resource.type == WINED3D_RTYPE_TEXTURE_3D
                ? 0 : src_sub_resource_idx / src_texture->level_count;
        dst.texture = dst_texture_wgpu->texture;
        dst.mipLevel = dst_level;
        dst.origin.x = dst_rect->left;
        dst.origin.y = dst_rect->top;
        dst.origin.z = dst_texture->resource.type == WINED3D_RTYPE_TEXTURE_3D
                ? 0 : dst_sub_resource_idx / dst_texture->level_count;
        extent.width = src_rect->right - src_rect->left;
        extent.height = src_rect->bottom - src_rect->top;
        extent.depthOrArrayLayers = 1;

        encoder = wined3d_context_wgpu_get_encoder(context_wgpu);
        wined3d_context_wgpu_end_passes(context_wgpu);
        wgpuCommandEncoderCopyTextureToTexture(encoder, &src, &dst, &extent);
    }

    wined3d_texture_validate_location(dst_texture, dst_sub_resource_idx, WINED3D_LOCATION_TEXTURE_RGB);
    wined3d_texture_invalidate_location(dst_texture, dst_sub_resource_idx, ~WINED3D_LOCATION_TEXTURE_RGB);
    if (!wined3d_texture_load_location(dst_texture, dst_sub_resource_idx, context, dst_location))
        ERR("Failed to load the destination sub-resource into %s.\n", wined3d_debug_location(dst_location));

    return dst_location | WINED3D_LOCATION_TEXTURE_RGB;

next:
    if (!(next = blitter->next))
    {
        ERR("No blitter to handle blit op %#x.\n", op);
        return dst_location;
    }

    TRACE("Forwarding to blitter %p.\n", next);
    return next->ops->blitter_blit(next, op, context, src_texture, src_sub_resource_idx, src_location,
            src_rect, dst_texture, dst_sub_resource_idx, dst_location, dst_rect, colour_key, filter, resolve_format);
}

static const struct wined3d_blitter_ops wgpu_blitter_ops =
{
    .blitter_destroy = wgpu_blitter_destroy,
    .blitter_clear = wgpu_blitter_clear,
    .blitter_blit = wgpu_blitter_blit,
};

void wined3d_wgpu_blitter_create(struct wined3d_blitter **next)
{
    struct wined3d_blitter *blitter;

    if (!(blitter = malloc(sizeof(*blitter))))
        return;

    TRACE("Created blitter %p.\n", blitter);

    blitter->ops = &wgpu_blitter_ops;
    blitter->next = *next;
    *next = blitter;
}

/* Null resources: what an unbound SRV, CBV or vertex buffer reads, zeros. */

static WGPUTexture wgpu_create_null_texture(WGPUDevice device, WGPUTextureFormat format,
        WGPUTextureDimension dimension, unsigned int layers, unsigned int samples)
{
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;

    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment;
    desc.dimension = dimension;
    desc.size.width = 1;
    desc.size.height = 1;
    desc.size.depthOrArrayLayers = layers;
    desc.format = format;
    desc.sampleCount = samples;
    return wgpuDeviceCreateTexture(device, &desc);
}

static WGPUTextureView wgpu_create_null_view(WGPUTexture texture, WGPUTextureViewDimension dimension,
        unsigned int layers, bool depth)
{
    WGPUTextureViewDescriptor desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;

    desc.dimension = dimension;
    desc.arrayLayerCount = layers;
    if (depth)
        desc.aspect = WGPUTextureAspect_DepthOnly;
    return wgpuTextureCreateView(texture, &desc);
}

bool wined3d_device_wgpu_create_null_resources(struct wined3d_device_wgpu *device_wgpu)
{
    struct wined3d_null_resources_wgpu *r = &device_wgpu->null_resources;
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    static const WGPUTextureFormat formats[] =
            {WGPUTextureFormat_RGBA8Unorm, WGPUTextureFormat_RGBA8Uint, WGPUTextureFormat_RGBA8Sint};
    WGPUTexture *textures[3] = {&r->texture_2d, &r->texture_2d_uint, &r->texture_2d_sint};
    WGPUDevice device = device_wgpu->device;
    unsigned int i;

    buffer_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_Storage | WGPUBufferUsage_Vertex
            | WGPUBufferUsage_CopyDst;
    buffer_desc.size = WINED3D_WGPU_MIN_CONSTANT_BUFFER_SIZE;
    if (!(r->buffer = wgpuDeviceCreateBuffer(device, &buffer_desc)))
        return false;

    /* Each sample type has a 2D texture of six layers, which 2D, 2D array
     * and cube views share, and a 3D and a multisampled one, whose views
     * keep them. WebGPU zeroes new textures. */
    for (i = 0; i < ARRAY_SIZE(formats); ++i)
    {
        WGPUTexture t2d, t3d, tms;

        if (!(t2d = wgpu_create_null_texture(device, formats[i], WGPUTextureDimension_2D, 6, 1)))
            return false;
        t3d = wgpu_create_null_texture(device, formats[i], WGPUTextureDimension_3D, 1, 1);
        tms = wgpu_create_null_texture(device, formats[i], WGPUTextureDimension_2D, 1, 4);
        *textures[i] = t2d;

        r->views[WINED3D_SHADER_RESOURCE_TEXTURE_2D][i] = wgpu_create_null_view(t2d,
                WGPUTextureViewDimension_2D, 1, false);
        r->views[WINED3D_SHADER_RESOURCE_TEXTURE_2DARRAY][i] = wgpu_create_null_view(t2d,
                WGPUTextureViewDimension_2DArray, 6, false);
        r->views[WINED3D_SHADER_RESOURCE_TEXTURE_CUBE][i] = wgpu_create_null_view(t2d,
                WGPUTextureViewDimension_Cube, 6, false);
        r->views[WINED3D_SHADER_RESOURCE_TEXTURE_CUBEARRAY][i] = wgpu_create_null_view(t2d,
                WGPUTextureViewDimension_CubeArray, 6, false);
        r->views[WINED3D_SHADER_RESOURCE_TEXTURE_3D][i] = wgpu_create_null_view(t3d,
                WGPUTextureViewDimension_3D, 1, false);
        r->views[WINED3D_SHADER_RESOURCE_TEXTURE_2DMS][i] = wgpu_create_null_view(tms,
                WGPUTextureViewDimension_2D, 1, false);
        if (t3d)
            wgpuTextureRelease(t3d);
        if (tms)
            wgpuTextureRelease(tms);
    }

    {
        WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
        WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;

        r->sampler = wgpuDeviceCreateSampler(device, &sampler_desc);
        sampler_desc.compare = WGPUCompareFunction_Never;
        r->comparison_sampler = wgpuDeviceCreateSampler(device, &sampler_desc);

        wgsl.code = wgpu_string("@fragment fn shader_entry() {}\n");
        module_desc.nextInChain = &wgsl.chain;
        r->fragment_module = wgpuDeviceCreateShaderModule(device, &module_desc);
    }

    r->depth_2d_texture = wgpu_create_null_texture(device, WGPUTextureFormat_Depth32Float,
            WGPUTextureDimension_2D, 6, 1);
    r->depth_2d = wgpu_create_null_view(r->depth_2d_texture, WGPUTextureViewDimension_2D, 1, true);
    r->depth_cube = wgpu_create_null_view(r->depth_2d_texture, WGPUTextureViewDimension_Cube, 6, true);

    return true;
}

void wined3d_device_wgpu_destroy_null_resources(struct wined3d_device_wgpu *device_wgpu)
{
    struct wined3d_null_resources_wgpu *r = &device_wgpu->null_resources;
    unsigned int i, j;

    for (i = 0; i < ARRAY_SIZE(r->views); ++i)
    {
        for (j = 0; j < ARRAY_SIZE(r->views[i]); ++j)
        {
            if (r->views[i][j])
                wgpuTextureViewRelease(r->views[i][j]);
        }
    }
    if (r->depth_read_pipeline)
        wgpuRenderPipelineRelease(r->depth_read_pipeline);
    for (i = 0; i < ARRAY_SIZE(r->mip_pipelines); ++i)
    {
        if (r->mip_pipelines[i].pipeline)
            wgpuRenderPipelineRelease(r->mip_pipelines[i].pipeline);
    }
    for (i = 0; i < ARRAY_SIZE(r->clear_pipelines); ++i)
    {
        if (r->clear_pipelines[i].pipeline)
            wgpuRenderPipelineRelease(r->clear_pipelines[i].pipeline);
    }
    if (r->clear_module)
        wgpuShaderModuleRelease(r->clear_module);
    if (r->mip_layout)
        wgpuBindGroupLayoutRelease(r->mip_layout);
    if (r->mip_layout_3d)
        wgpuBindGroupLayoutRelease(r->mip_layout_3d);
    if (r->depth_2d)
        wgpuTextureViewRelease(r->depth_2d);
    if (r->depth_cube)
        wgpuTextureViewRelease(r->depth_cube);
    if (r->depth_2d_texture)
        wgpuTextureRelease(r->depth_2d_texture);
    if (r->texture_2d)
        wgpuTextureRelease(r->texture_2d);
    if (r->texture_2d_uint)
        wgpuTextureRelease(r->texture_2d_uint);
    if (r->texture_2d_sint)
        wgpuTextureRelease(r->texture_2d_sint);
    if (r->buffer)
        wgpuBufferRelease(r->buffer);
    if (r->sampler)
        wgpuSamplerRelease(r->sampler);
    if (r->comparison_sampler)
        wgpuSamplerRelease(r->comparison_sampler);
    if (r->fragment_module)
        wgpuShaderModuleRelease(r->fragment_module);
    memset(r, 0, sizeof(*r));
}

static WGPURenderPipeline wgpu_mip_pipeline(struct wined3d_device_wgpu *device_wgpu, WGPUTextureFormat format,
        bool is_3d)
{
    /* For a 3D texture, the slice drawn (a constant: one pipeline makes all
     * slices, the source slice pair given by the instance index). */
    static const char code_3d[] =
        "@group(0) @binding(0) var src: texture_3d<f32>;\n"
        "struct V { @builtin(position) p: vec4f, @location(0) @interpolate(flat) z: u32 };\n"
        "@vertex fn vs(@builtin(vertex_index) i: u32, @builtin(instance_index) z: u32) -> V {\n"
        "    let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
        "    return V(vec4f(p * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0), z);\n"
        "}\n"
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "    let m = vec3i(textureDimensions(src)) - vec3i(1, 1, 1);\n"
        "    let c = vec3i(vec2i(v.p.xy) * 2, i32(v.z) * 2);\n"
        "    var sum = vec4f(0.0);\n"
        "    for (var i = 0; i < 8; i++) {\n"
        "        sum += textureLoad(src, min(c + vec3i(i & 1, (i >> 1) & 1, i >> 2), m), 0);\n"
        "    }\n"
        "    return sum * 0.125;\n"
        "}\n";
    static const char code[] =
        "@group(0) @binding(0) var src: texture_2d<f32>;\n"
        "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
        "    let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
        "    return vec4f(p * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);\n"
        "}\n"
        /* A box filter of the 2x2 texels under the pixel, the last row and
         * column repeated for odd sizes: loads, so any float format works. */
        "@fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f {\n"
        "    let m = vec2i(textureDimensions(src)) - vec2i(1, 1);\n"
        "    let c = vec2i(p.xy) * 2;\n"
        "    return (textureLoad(src, min(c, m), 0) + textureLoad(src, min(c + vec2i(1, 0), m), 0)\n"
        "            + textureLoad(src, min(c + vec2i(0, 1), m), 0) + textureLoad(src, min(c + vec2i(1, 1), m), 0))\n"
        "            * 0.25;\n"
        "}\n";
    struct wined3d_null_resources_wgpu *r = &device_wgpu->null_resources;
    WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayoutDescriptor pipeline_layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    WGPURenderPipeline pipeline;
    WGPUShaderModule module;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(r->mip_pipelines); ++i)
    {
        if (r->mip_pipelines[i].pipeline && r->mip_pipelines[i].format == format
                && r->mip_pipelines[i].is_3d == is_3d)
            return r->mip_pipelines[i].pipeline;
    }

    if (!(is_3d ? r->mip_layout_3d : r->mip_layout))
    {
        entry.binding = 0;
        entry.visibility = WGPUShaderStage_Fragment;
        entry.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        entry.texture.viewDimension = is_3d ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
        layout_desc.entryCount = 1;
        layout_desc.entries = &entry;
        if (!(*(is_3d ? &r->mip_layout_3d : &r->mip_layout)
                = wgpuDeviceCreateBindGroupLayout(device_wgpu->device, &layout_desc)))
            return NULL;
    }

    wgsl.code = wgpu_string(is_3d ? code_3d : code);
    module_desc.nextInChain = &wgsl.chain;
    if (!(module = wgpuDeviceCreateShaderModule(device_wgpu->device, &module_desc)))
        return NULL;
    pipeline_layout_desc.bindGroupLayoutCount = 1;
    pipeline_layout_desc.bindGroupLayouts = is_3d ? &r->mip_layout_3d : &r->mip_layout;
    desc.layout = wgpuDeviceCreatePipelineLayout(device_wgpu->device, &pipeline_layout_desc);
    desc.vertex.module = module;
    desc.vertex.entryPoint = wgpu_string("vs");
    target.format = format;
    fragment.module = module;
    fragment.entryPoint = wgpu_string("fs");
    fragment.targetCount = 1;
    fragment.targets = &target;
    desc.fragment = &fragment;
    wined3d_device_wgpu_push_error_scope(device_wgpu);
    pipeline = wgpuDeviceCreateRenderPipeline(device_wgpu->device, &desc);
    if (!wined3d_device_wgpu_pop_error_scope(device_wgpu, "a mipmap pipeline") && pipeline)
    {
        wgpuRenderPipelineRelease(pipeline);
        pipeline = NULL;
    }
    wgpuPipelineLayoutRelease(desc.layout);
    wgpuShaderModuleRelease(module);
    if (!pipeline)
        return NULL;

    /* Replace the oldest. */
    i = ARRAY_SIZE(r->mip_pipelines) - 1;
    if (r->mip_pipelines[i].pipeline)
        wgpuRenderPipelineRelease(r->mip_pipelines[i].pipeline);
    memmove(&r->mip_pipelines[1], &r->mip_pipelines[0], i * sizeof(r->mip_pipelines[0]));
    r->mip_pipelines[0].format = format;
    r->mip_pipelines[0].is_3d = is_3d;
    r->mip_pipelines[0].pipeline = pipeline;
    return pipeline;
}

/* GenerateMips: each level after the view's first made from the one before,
 * by a render pass per level and layer. */
void wined3d_texture_wgpu_generate_mipmaps(struct wined3d_context_wgpu *context_wgpu,
        struct wined3d_shader_resource_view *view)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    unsigned int base, count, first_layer, layer_count, level, layer, sub_resource_idx;
    struct wined3d_texture_wgpu *texture_wgpu;
    struct wined3d_texture *texture;
    WGPUTextureFormat format;
    WGPURenderPipeline pipeline;
    WGPUCommandEncoder encoder;

    bool is_3d = view->resource->type == WINED3D_RTYPE_TEXTURE_3D;

    if (view->resource->type != WINED3D_RTYPE_TEXTURE_2D && !is_3d)
    {
        FIXME("Unhandled resource type %s.\n", debug_d3dresourcetype(view->resource->type));
        return;
    }
    texture = texture_from_resource(view->resource);
    texture_wgpu = wined3d_texture_wgpu(texture);
    base = view->desc.u.texture.level_idx;
    count = view->desc.u.texture.level_count;
    first_layer = view->desc.u.texture.layer_idx;
    layer_count = view->desc.u.texture.layer_count;

    wined3d_texture_load(texture, &context_wgpu->c, FALSE);
    /* A pass on a texture that cannot be rendered to would be an error, and
     * one error discards the encoder's every command. */
    if (!texture_wgpu->texture || !(wgpuTextureGetUsage(texture_wgpu->texture) & WGPUTextureUsage_RenderAttachment))
    {
        FIXME("Cannot render to the texture of format %s.\n", debug_d3dformat(view->format->id));
        return;
    }
    format = wined3d_format_wgpu(view->format)->texture_format;
    if (format != texture_wgpu->format && format != wined3d_format_wgpu(view->resource->format)->srgb_toggle_format)
        format = texture_wgpu->format;
    if (is_3d)
    {
        first_layer = 0;
        layer_count = 1;
    }
    if (!(pipeline = wgpu_mip_pipeline(device_wgpu, format, is_3d)))
    {
        FIXME("Cannot generate mipmaps of format %s.\n", debug_d3dformat(view->format->id));
        return;
    }

    encoder = wined3d_context_wgpu_get_encoder(context_wgpu);
    wined3d_context_wgpu_end_passes(context_wgpu);
    for (layer = first_layer; layer < first_layer + layer_count; ++layer)
    {
        for (level = base + 1; level < base + count; ++level)
        {
            WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
            WGPUTextureView src, dst;
            WGPURenderPassEncoder pass;
            unsigned int slice, slice_count;
            WGPUBindGroup group;

            sub_resource_idx = layer * texture->level_count + level;
            slice_count = is_3d ? wined3d_texture_get_level_depth(texture, level) : 1;
            view_desc.format = format;
            view_desc.dimension = is_3d ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
            view_desc.baseMipLevel = level - 1;
            view_desc.mipLevelCount = 1;
            view_desc.baseArrayLayer = layer;
            view_desc.arrayLayerCount = 1;
            src = wgpuTextureCreateView(texture_wgpu->texture, &view_desc);
            view_desc.baseMipLevel = level;
            dst = wgpuTextureCreateView(texture_wgpu->texture, &view_desc);

            entry.binding = 0;
            entry.textureView = src;
            group_desc.layout = is_3d ? device_wgpu->null_resources.mip_layout_3d
                    : device_wgpu->null_resources.mip_layout;
            group_desc.entryCount = 1;
            group_desc.entries = &entry;
            group = wgpuDeviceCreateBindGroup(device_wgpu->device, &group_desc);

            /* A pass per slice of a 3D level. */
            for (slice = 0; slice < slice_count; ++slice)
            {
                attachment.view = dst;
                attachment.depthSlice = is_3d ? slice : WGPU_DEPTH_SLICE_UNDEFINED;
                attachment.loadOp = WGPULoadOp_Clear;
                attachment.storeOp = WGPUStoreOp_Store;
                pass_desc.colorAttachmentCount = 1;
                pass_desc.colorAttachments = &attachment;
                pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
                wgpuRenderPassEncoderSetPipeline(pass, pipeline);
                wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, NULL);
                wgpuRenderPassEncoderDraw(pass, 3, 1, 0, slice);
                wgpuRenderPassEncoderEnd(pass);
                wgpuRenderPassEncoderRelease(pass);
            }
            wgpuBindGroupRelease(group);
            wgpuTextureViewRelease(dst);
            wgpuTextureViewRelease(src);

            wined3d_texture_validate_location(texture, sub_resource_idx, WINED3D_LOCATION_TEXTURE_RGB);
            wined3d_texture_invalidate_location(texture, sub_resource_idx, ~WINED3D_LOCATION_TEXTURE_RGB);
        }
    }
}
