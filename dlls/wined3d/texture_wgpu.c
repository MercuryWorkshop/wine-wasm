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

            if (view->resource->type != WINED3D_RTYPE_BUFFER
                    && wined3d_rendertarget_view_is_full_clear(view, draw_rect, clear_rects)
                    && !wined3d_rendertarget_view_use_cpu_clear(view))
            {
                struct wined3d_texture *texture = texture_from_resource(view->resource);

                wined3d_rendertarget_view_validate_location(view, WINED3D_LOCATION_CLEARED);
                wined3d_rendertarget_view_invalidate_location(view, ~WINED3D_LOCATION_CLEARED);
                texture->sub_resources[view->sub_resource_idx].clear_value.colour = *colour;
                continue;
            }

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
        else if (!wined3d_rendertarget_view_is_full_clear(view, draw_rect, clear_rects)
                || !wgpu_clear_ds_aspect(device, view, flags, depth, stencil))
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
