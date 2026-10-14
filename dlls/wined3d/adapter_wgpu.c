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

static const struct wined3d_state_entry_template misc_state_template_wgpu[] =
{
    {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_VERTEX),   {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_VERTEX),   state_nop}},
    {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_HULL),     {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_HULL),     state_nop}},
    {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_DOMAIN),   {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_DOMAIN),   state_nop}},
    {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_GEOMETRY), {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_GEOMETRY), state_nop}},
    {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_PIXEL),    {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_PIXEL),    state_nop}},
    {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_COMPUTE),  {STATE_CONSTANT_BUFFER(WINED3D_SHADER_TYPE_COMPUTE),  state_nop}},
    {STATE_GRAPHICS_SHADER_RESOURCE_BINDING,              {STATE_GRAPHICS_SHADER_RESOURCE_BINDING,              state_nop}},
    {STATE_GRAPHICS_UNORDERED_ACCESS_VIEW_BINDING,        {STATE_GRAPHICS_UNORDERED_ACCESS_VIEW_BINDING,        state_nop}},
    {STATE_COMPUTE_SHADER_RESOURCE_BINDING,               {STATE_COMPUTE_SHADER_RESOURCE_BINDING,               state_nop}},
    {STATE_COMPUTE_UNORDERED_ACCESS_VIEW_BINDING,         {STATE_COMPUTE_UNORDERED_ACCESS_VIEW_BINDING,         state_nop}},
    {STATE_STREAM_OUTPUT,                                 {STATE_STREAM_OUTPUT,                                 state_nop}},
    {STATE_BLEND,                                         {STATE_BLEND,                                         state_nop}},
    {STATE_BLEND_FACTOR,                                  {STATE_BLEND_FACTOR,                                  state_nop}},
    {STATE_SAMPLE_MASK,                                   {STATE_SAMPLE_MASK,                                   state_nop}},
    {STATE_STREAMSRC,                                     {STATE_STREAMSRC,                                     state_nop}},
    {STATE_VDECL,                                         {STATE_VDECL,                                         state_nop}},
    {STATE_DEPTH_STENCIL,                                 {STATE_DEPTH_STENCIL,                                 state_nop}},
    {STATE_STENCIL_REF,                                   {STATE_STENCIL_REF,                                   state_nop}},
    {STATE_DEPTH_BOUNDS,                                  {STATE_DEPTH_BOUNDS,                                  state_nop}},
    {STATE_RASTERIZER,                                    {STATE_RASTERIZER,                                    state_nop}},
    {STATE_SCISSORRECT,                                   {STATE_SCISSORRECT,                                   state_nop}},
    {STATE_VIEWPORT,                                      {STATE_VIEWPORT,                                      state_nop}},
    {STATE_INDEXBUFFER,                                   {STATE_INDEXBUFFER,                                   state_nop}},
    {STATE_RENDER(WINED3D_RS_LINEPATTERN),                {STATE_RENDER(WINED3D_RS_LINEPATTERN),                state_nop}},
    {STATE_RENDER(WINED3D_RS_DITHERENABLE),               {STATE_RENDER(WINED3D_RS_DITHERENABLE),               state_nop}},
    {STATE_RENDER(WINED3D_RS_MULTISAMPLEANTIALIAS),       {STATE_RENDER(WINED3D_RS_MULTISAMPLEANTIALIAS),       state_nop}},
    {STATE_BASEVERTEXINDEX,                               {STATE_STREAMSRC}},
    {STATE_FRAMEBUFFER,                                   {STATE_FRAMEBUFFER,                                   state_nop}},
    {STATE_SHADER(WINED3D_SHADER_TYPE_PIXEL),             {STATE_SHADER(WINED3D_SHADER_TYPE_PIXEL),             state_nop}},
    {STATE_SHADER(WINED3D_SHADER_TYPE_HULL),              {STATE_SHADER(WINED3D_SHADER_TYPE_HULL),              state_nop}},
    {STATE_SHADER(WINED3D_SHADER_TYPE_DOMAIN),            {STATE_SHADER(WINED3D_SHADER_TYPE_DOMAIN),            state_nop}},
    {STATE_SHADER(WINED3D_SHADER_TYPE_GEOMETRY),          {STATE_SHADER(WINED3D_SHADER_TYPE_GEOMETRY),          state_nop}},
    {STATE_SHADER(WINED3D_SHADER_TYPE_COMPUTE),           {STATE_SHADER(WINED3D_SHADER_TYPE_COMPUTE),           state_nop}},
    {0}, /* Terminate */
};

static bool wined3d_wgpu_wait(WGPUInstance instance, WGPUFuture future)
{
    WGPUFutureWaitInfo info = {future, FALSE};

    return wgpuInstanceWaitAny(instance, 1, &info, UINT64_MAX) == WGPUWaitStatus_Success && info.completed;
}

static void adapter_wgpu_destroy(struct wined3d_adapter *adapter)
{
    struct wined3d_adapter_wgpu *adapter_wgpu = wined3d_adapter_wgpu(adapter);
    struct wined3d_wgpu_info *wgpu_info = &adapter_wgpu->wgpu_info;

    wgpuAdapterInfoFreeMembers(wgpu_info->adapter_info);
    wgpuAdapterRelease(wgpu_info->adapter);
    wined3d_adapter_cleanup(&adapter_wgpu->a);
    free(adapter_wgpu);
}

static void wgpu_device_callback(WGPURequestDeviceStatus status, WGPUDevice device,
        WGPUStringView message, void *userdata1, void *userdata2)
{
    WGPUDevice *out = userdata1;

    if (status != WGPURequestDeviceStatus_Success)
        ERR("Failed to create a WebGPU device, status %#x: %s.\n", status,
                debugstr_an(message.data, message.data ? (int)message.length : 0));
    *out = status == WGPURequestDeviceStatus_Success ? device : NULL;
}

static void wgpu_device_lost_callback(WGPUDevice const *device, WGPUDeviceLostReason reason,
        WGPUStringView message, void *userdata1, void *userdata2)
{
    if (reason != WGPUDeviceLostReason_Destroyed)
        ERR("WebGPU device lost, reason %#x: %s.\n", reason,
                debugstr_an(message.data, message.data ? (int)message.length : 0));
}

static void wgpu_uncaptured_error_callback(WGPUDevice const *device, WGPUErrorType type,
        WGPUStringView message, void *userdata1, void *userdata2)
{
    ERR("WebGPU error %#x: %s\n", type, debugstr_an(message.data, message.data ? (int)message.length : 0));
}

/* The state templates use no extensions. */
static const BOOL supported_extensions[1] = {TRUE};

static HRESULT adapter_wgpu_create_device(struct wined3d *wined3d, const struct wined3d_adapter *adapter,
        enum wined3d_device_type device_type, HWND focus_window, unsigned int flags, BYTE surface_alignment,
        const enum wined3d_feature_level *levels, unsigned int level_count,
        struct wined3d_device_parent *device_parent, struct wined3d_device **device)
{
    const struct wined3d_adapter_wgpu *adapter_wgpu = wined3d_adapter_wgpu_const(adapter);
    const struct wined3d_wgpu_info *wgpu_info = &adapter_wgpu->wgpu_info;
    WGPURequestDeviceCallbackInfo callback_info = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    WGPUDeviceDescriptor device_desc = WGPU_DEVICE_DESCRIPTOR_INIT;
    WGPUFeatureName features[16];
    struct wined3d_device_wgpu *device_wgpu;
    WGPUDevice wgpu_device = NULL;
    unsigned int feature_count = 0;
    HRESULT hr;

    if (!(device_wgpu = calloc(1, sizeof(*device_wgpu))))
        return E_OUTOFMEMORY;

#define ADD_FEATURE(cond, name) if (wgpu_info->cond) features[feature_count++] = WGPUFeatureName_##name;
    ADD_FEATURE(texture_compression_bc, TextureCompressionBC)
    ADD_FEATURE(texture_compression_bc_3d, TextureCompressionBCSliced3D)
    ADD_FEATURE(depth32float_stencil8, Depth32FloatStencil8)
    ADD_FEATURE(depth_clip_control, DepthClipControl)
    ADD_FEATURE(float32_filterable, Float32Filterable)
    ADD_FEATURE(float32_blendable, Float32Blendable)
    ADD_FEATURE(rg11b10_renderable, RG11B10UfloatRenderable)
    ADD_FEATURE(dual_source_blending, DualSourceBlending)
    ADD_FEATURE(clip_distances, ClipDistances)
    ADD_FEATURE(texture_component_swizzle, TextureComponentSwizzle)
    ADD_FEATURE(texture_formats_tier1, TextureFormatsTier1)
    ADD_FEATURE(indirect_first_instance, IndirectFirstInstance)
#undef ADD_FEATURE

    device_desc.label = wgpu_string("wined3d");
    device_desc.requiredFeatureCount = feature_count;
    device_desc.requiredFeatures = features;
    device_desc.requiredLimits = &wgpu_info->limits;
    device_desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    device_desc.deviceLostCallbackInfo.callback = wgpu_device_lost_callback;
    device_desc.uncapturedErrorCallbackInfo.callback = wgpu_uncaptured_error_callback;

    callback_info.mode = WGPUCallbackMode_WaitAnyOnly;
    callback_info.callback = wgpu_device_callback;
    callback_info.userdata1 = &wgpu_device;
    if (!wined3d_wgpu_wait(wgpu_info->instance, wgpuAdapterRequestDevice(wgpu_info->adapter,
            &device_desc, callback_info)) || !wgpu_device)
    {
        WARN("Failed to create a WebGPU device.\n");
        free(device_wgpu);
        return E_FAIL;
    }

    device_wgpu->device = wgpu_device;
    device_wgpu->queue = wgpuDeviceGetQueue(wgpu_device);

    if (FAILED(hr = wined3d_device_init(&device_wgpu->d, wined3d, adapter->ordinal, device_type, focus_window,
            flags, surface_alignment, levels, level_count, supported_extensions, device_parent)))
    {
        WARN("Failed to initialize device, hr %#lx.\n", hr);
        wgpuQueueRelease(device_wgpu->queue);
        wgpuDeviceRelease(device_wgpu->device);
        free(device_wgpu);
        return hr;
    }

    *device = &device_wgpu->d;

    return WINED3D_OK;
}

/* Objects made rarely (shader modules, pipelines, textures) are made in an
 * error scope and checked, so that an invalid one is never used: an error
 * in an encoder discards everything recorded in it. */
void wined3d_device_wgpu_push_error_scope(struct wined3d_device_wgpu *device_wgpu)
{
    wgpuDevicePushErrorScope(device_wgpu->device, WGPUErrorFilter_Validation);
}

struct wgpu_error_scope_result
{
    WGPUErrorType type;
    const char *what;
};

static void wgpu_pop_error_scope_callback(WGPUPopErrorScopeStatus status, WGPUErrorType type,
        WGPUStringView message, void *userdata1, void *userdata2)
{
    struct wgpu_error_scope_result *result = userdata1;

    result->type = status == WGPUPopErrorScopeStatus_Success ? type : WGPUErrorType_Unknown;
    if (result->type != WGPUErrorType_NoError)
        ERR("Failed to create %s: %s\n", result->what,
                debugstr_an(message.data, message.data ? (int)message.length : 0));
}

bool wined3d_device_wgpu_pop_error_scope(struct wined3d_device_wgpu *device_wgpu, const char *what)
{
    WGPUPopErrorScopeCallbackInfo callback_info = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    struct wgpu_error_scope_result result = {WGPUErrorType_Unknown, what};

    callback_info.mode = WGPUCallbackMode_WaitAnyOnly;
    callback_info.callback = wgpu_pop_error_scope_callback;
    callback_info.userdata1 = &result;
    if (!wined3d_wgpu_wait(wined3d_adapter_wgpu(device_wgpu->d.adapter)->wgpu_info.instance,
            wgpuDevicePopErrorScope(device_wgpu->device, callback_info)))
        return false;
    return result.type == WGPUErrorType_NoError;
}

static void adapter_wgpu_destroy_device(struct wined3d_device *device)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(device);
    struct wined3d *wined3d = device->wined3d;

    /* The adapter holds the instance, which is needed until the device is
     * released. */
    wined3d_incref(wined3d);

    wined3d_device_cleanup(&device_wgpu->d);

    wgpuQueueRelease(device_wgpu->queue);
    wgpuDeviceDestroy(device_wgpu->device);
    wgpuDeviceRelease(device_wgpu->device);
    wined3d_decref(wined3d);
    free(device_wgpu);
}

static struct wined3d_context *adapter_wgpu_acquire_context(struct wined3d_device *device,
        struct wined3d_texture *texture, unsigned int sub_resource_idx)
{
    TRACE("device %p, texture %p, sub_resource_idx %u.\n", device, texture, sub_resource_idx);

    wined3d_from_cs(device->cs);

    if (!device->context_count)
        return NULL;

    return &wined3d_device_wgpu(device)->context_wgpu.c;
}

static void adapter_wgpu_release_context(struct wined3d_context *context)
{
    TRACE("context %p.\n", context);
}

static void adapter_wgpu_get_wined3d_caps(const struct wined3d_adapter *adapter, struct wined3d_caps *caps)
{
    const struct wined3d_adapter_wgpu *adapter_wgpu = wined3d_adapter_wgpu_const(adapter);
    const WGPULimits *limits = &adapter_wgpu->wgpu_info.limits;

    caps->ddraw_caps.dds_caps |= WINEDDSCAPS_BACKBUFFER
            | WINEDDSCAPS_COMPLEX
            | WINEDDSCAPS_FRONTBUFFER
            | WINEDDSCAPS_3DDEVICE
            | WINEDDSCAPS_VIDEOMEMORY
            | WINEDDSCAPS_OWNDC
            | WINEDDSCAPS_LOCALVIDMEM
            | WINEDDSCAPS_NONLOCALVIDMEM;
    caps->ddraw_caps.caps |= WINEDDCAPS_3D;

    caps->PrimitiveMiscCaps |= WINED3DPMISCCAPS_BLENDOP
            | WINED3DPMISCCAPS_INDEPENDENTWRITEMASKS
            | WINED3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS
            | WINED3DPMISCCAPS_POSTBLENDSRGBCONVERT
            | WINED3DPMISCCAPS_SEPARATEALPHABLEND;

    caps->RasterCaps |= WINED3DPRASTERCAPS_MIPMAPLODBIAS
            | WINED3DPRASTERCAPS_ANISOTROPY;
    caps->TextureFilterCaps |= WINED3DPTFILTERCAPS_MAGFANISOTROPIC
            | WINED3DPTFILTERCAPS_MINFANISOTROPIC;
    caps->MaxAnisotropy = 16;

    caps->SrcBlendCaps |= WINED3DPBLENDCAPS_BLENDFACTOR;
    caps->DestBlendCaps |= WINED3DPBLENDCAPS_BLENDFACTOR
            | WINED3DPBLENDCAPS_SRCALPHASAT;

    caps->TextureCaps |= WINED3DPTEXTURECAPS_VOLUMEMAP
            | WINED3DPTEXTURECAPS_MIPVOLUMEMAP
            | WINED3DPTEXTURECAPS_VOLUMEMAP_POW2;
    caps->VolumeTextureFilterCaps |= WINED3DPTFILTERCAPS_MAGFLINEAR
            | WINED3DPTFILTERCAPS_MAGFPOINT
            | WINED3DPTFILTERCAPS_MINFLINEAR
            | WINED3DPTFILTERCAPS_MINFPOINT
            | WINED3DPTFILTERCAPS_MIPFLINEAR
            | WINED3DPTFILTERCAPS_MIPFPOINT
            | WINED3DPTFILTERCAPS_LINEAR
            | WINED3DPTFILTERCAPS_LINEARMIPLINEAR
            | WINED3DPTFILTERCAPS_LINEARMIPNEAREST
            | WINED3DPTFILTERCAPS_MIPLINEAR
            | WINED3DPTFILTERCAPS_MIPNEAREST
            | WINED3DPTFILTERCAPS_NEAREST;
    caps->VolumeTextureAddressCaps |= WINED3DPTADDRESSCAPS_INDEPENDENTUV
            | WINED3DPTADDRESSCAPS_CLAMP
            | WINED3DPTADDRESSCAPS_WRAP
            | WINED3DPTADDRESSCAPS_MIRROR;
    caps->MaxVolumeExtent = limits->maxTextureDimension3D;

    caps->TextureCaps |= WINED3DPTEXTURECAPS_CUBEMAP
            | WINED3DPTEXTURECAPS_MIPCUBEMAP
            | WINED3DPTEXTURECAPS_CUBEMAP_POW2;
    caps->CubeTextureFilterCaps |= WINED3DPTFILTERCAPS_MAGFLINEAR
            | WINED3DPTFILTERCAPS_MAGFPOINT
            | WINED3DPTFILTERCAPS_MINFLINEAR
            | WINED3DPTFILTERCAPS_MINFPOINT
            | WINED3DPTFILTERCAPS_MIPFLINEAR
            | WINED3DPTFILTERCAPS_MIPFPOINT
            | WINED3DPTFILTERCAPS_LINEAR
            | WINED3DPTFILTERCAPS_LINEARMIPLINEAR
            | WINED3DPTFILTERCAPS_LINEARMIPNEAREST
            | WINED3DPTFILTERCAPS_MIPLINEAR
            | WINED3DPTFILTERCAPS_MIPNEAREST
            | WINED3DPTFILTERCAPS_NEAREST
            | WINED3DPTFILTERCAPS_MAGFANISOTROPIC
            | WINED3DPTFILTERCAPS_MINFANISOTROPIC;

    /* WebGPU has no border colour. */
    caps->TextureAddressCaps |= WINED3DPTADDRESSCAPS_MIRROR;

    caps->StencilCaps |= WINED3DSTENCILCAPS_DECR
            | WINED3DSTENCILCAPS_INCR
            | WINED3DSTENCILCAPS_TWOSIDED;

    caps->DeclTypes |= WINED3DDTCAPS_FLOAT16_2 | WINED3DDTCAPS_FLOAT16_4;

    caps->MaxPixelShader30InstructionSlots = WINED3DMAX30SHADERINSTRUCTIONS;
    caps->MaxVertexShader30InstructionSlots = WINED3DMAX30SHADERINSTRUCTIONS;
    caps->PS20Caps.temp_count = WINED3DPS20_MAX_NUMTEMPS;
    caps->VS20Caps.temp_count = WINED3DVS20_MAX_NUMTEMPS;
}

static BOOL adapter_wgpu_check_format(const struct wined3d_adapter *adapter,
        const struct wined3d_format *adapter_format, const struct wined3d_format *rt_format,
        const struct wined3d_format *ds_format)
{
    return TRUE;
}

static HRESULT adapter_wgpu_init_3d(struct wined3d_device *device)
{
    struct wined3d_context_wgpu *context_wgpu;
    struct wined3d_device_wgpu *device_wgpu;
    HRESULT hr;

    TRACE("device %p.\n", device);

    device_wgpu = wined3d_device_wgpu(device);
    context_wgpu = &device_wgpu->context_wgpu;
    if (FAILED(hr = wined3d_context_wgpu_init(context_wgpu, device->swapchains[0])))
    {
        WARN("Failed to initialise context.\n");
        return hr;
    }

    if (FAILED(hr = device->shader_backend->shader_alloc_private(device,
            device->adapter->vertex_pipe, device->adapter->fragment_pipe)))
    {
        ERR("Failed to allocate shader private data, hr %#lx.\n", hr);
        wined3d_context_wgpu_cleanup(context_wgpu);
        return hr;
    }

    if (!device_context_add(device, &context_wgpu->c))
    {
        ERR("Failed to add the newly created context to the context list.\n");
        device->shader_backend->shader_free_private(device, NULL);
        wined3d_context_wgpu_cleanup(context_wgpu);
        return E_FAIL;
    }

    TRACE("Initialised context %p.\n", context_wgpu);

    if (!(device_wgpu->d.blitter = wined3d_cpu_blitter_create()))
    {
        ERR("Failed to create CPU blitter.\n");
        device_context_remove(device, &context_wgpu->c);
        device->shader_backend->shader_free_private(device, NULL);
        wined3d_context_wgpu_cleanup(context_wgpu);
        return E_FAIL;
    }
    wined3d_wgpu_blitter_create(&device_wgpu->d.blitter);

    wined3d_device_create_default_samplers(device, &context_wgpu->c);
    if (!wined3d_device_wgpu_create_null_resources(device_wgpu))
        ERR("Failed to create null resources.\n");

    return WINED3D_OK;
}

static void adapter_wgpu_uninit_3d_cs(void *object)
{
    struct wined3d_device_wgpu *device_wgpu = object;
    struct wined3d_context_wgpu *context_wgpu;
    struct wined3d_device *device;
    struct wined3d_shader *shader;

    TRACE("device_wgpu %p.\n", device_wgpu);

    context_wgpu = &device_wgpu->context_wgpu;
    device = &device_wgpu->d;

    LIST_FOR_EACH_ENTRY(shader, &device->shaders, struct wined3d_shader, shader_list_entry)
    {
        device->shader_backend->shader_destroy(shader);
    }

    device->blitter->ops->blitter_destroy(device->blitter, NULL);
    device->shader_backend->shader_free_private(device, &context_wgpu->c);
    wined3d_device_wgpu_destroy_null_resources(device_wgpu);
}

static void adapter_wgpu_uninit_3d(struct wined3d_device *device)
{
    struct wined3d_context_wgpu *context_wgpu;
    struct wined3d_device_wgpu *device_wgpu;

    TRACE("device %p.\n", device);

    device_wgpu = wined3d_device_wgpu(device);
    context_wgpu = &device_wgpu->context_wgpu;

    wined3d_device_destroy_default_samplers(device);
    wined3d_cs_destroy_object(device->cs, adapter_wgpu_uninit_3d_cs, device_wgpu);
    wined3d_cs_finish(device->cs, WINED3D_CS_QUEUE_DEFAULT);

    device_context_remove(device, &context_wgpu->c);
    wined3d_context_wgpu_cleanup(context_wgpu);
}

static void wgpu_map_callback(WGPUMapAsyncStatus status, WGPUStringView message, void *userdata1, void *userdata2)
{
    WGPUMapAsyncStatus *out = userdata1;

    *out = status;
}

/* Reads the GPU's copy of a buffer object back into its shadow. */
static void wined3d_bo_wgpu_read_back(struct wined3d_bo_wgpu *bo, struct wined3d_context_wgpu *context_wgpu)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPUBufferMapCallbackInfo callback_info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    WGPUMapAsyncStatus status = 0;
    WGPUCommandEncoder encoder;
    uint64_t size;
    WGPUBuffer readback;
    const void *data;

    if (!bo->gpu_written || !bo->buffer)
        return;

    size = (bo->size + 3) & ~(uint64_t)3;
    desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    desc.size = size;
    if (!(readback = wgpuDeviceCreateBuffer(device_wgpu->device, &desc)))
    {
        ERR("Failed to create a readback buffer.\n");
        return;
    }

    encoder = wined3d_context_wgpu_get_encoder(context_wgpu);
    wined3d_context_wgpu_end_passes(context_wgpu);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, bo->buffer, 0, readback, 0, size);
    wined3d_context_wgpu_submit(context_wgpu);

    callback_info.mode = WGPUCallbackMode_WaitAnyOnly;
    callback_info.callback = wgpu_map_callback;
    callback_info.userdata1 = &status;
    if (wined3d_context_wgpu_wait_future(context_wgpu,
            wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, size, callback_info))
            && status == WGPUMapAsyncStatus_Success
            && (data = wgpuBufferGetConstMappedRange(readback, 0, size)))
    {
        memcpy(bo->shadow, data, bo->size);
        wgpuBufferUnmap(readback);
        bo->gpu_written = false;
    }
    else
    {
        ERR("Failed to map the readback buffer, status %#x.\n", status);
    }
    wgpuBufferRelease(readback);
}

/* Gives a buffer object a new WGPUBuffer, leaving the old one to the
 * commands that use it. */
static bool wined3d_bo_wgpu_rename(struct wined3d_bo_wgpu *bo, struct wined3d_context_wgpu *context_wgpu)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    struct wined3d_bo_user *bo_user;
    WGPUBuffer buffer;

    desc.usage = bo->usage;
    desc.size = (bo->size + 3) & ~(uint64_t)3;
    if (!(buffer = wgpuDeviceCreateBuffer(device_wgpu->device, &desc)))
        return false;

    LIST_FOR_EACH_ENTRY(bo_user, &bo->b.users, struct wined3d_bo_user, entry)
        bo_user->valid = false;
    list_init(&bo->b.users);

    if (bo->buffer)
        wgpuBufferRelease(bo->buffer);
    bo->buffer = buffer;
    bo->encoder_id = 0;
    bo->gpu_written = false;
    return true;
}

/* Writes ranges of a buffer object's shadow to its WGPUBuffer. */
static void wined3d_bo_wgpu_upload(struct wined3d_bo_wgpu *bo, struct wined3d_context_wgpu *context_wgpu,
        uint64_t offset, uint64_t size)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    uint64_t start, end;

    if (!bo->buffer || !size)
        return;

    /* wgpuQueueWriteBuffer() wants 4-byte multiples; the shadow covers the
     * whole (padded) buffer, so round outwards. */
    start = offset & ~(uint64_t)3;
    end = min((offset + size + 3) & ~(uint64_t)3, (bo->size + 3) & ~(uint64_t)3);
    wgpuQueueWriteBuffer(device_wgpu->queue, bo->buffer, start, bo->shadow + start, end - start);
}

static void *adapter_wgpu_map_bo_address(struct wined3d_context *context,
        const struct wined3d_bo_address *data, size_t size, uint32_t map_flags)
{
    struct wined3d_context_wgpu *context_wgpu = wined3d_context_wgpu(context);
    struct wined3d_bo_wgpu *bo;

    if (!data->buffer_object)
        return data->addr;
    bo = wined3d_bo_wgpu(data->buffer_object);

    if (!(map_flags & WINED3D_MAP_NOOVERWRITE))
    {
        if ((map_flags & WINED3D_MAP_DISCARD) && bo->encoder_id == context_wgpu->encoder_id)
        {
            if (!wined3d_bo_wgpu_rename(bo, context_wgpu))
                ERR("Failed to rename buffer object %p.\n", bo);
        }
        else if (map_flags & WINED3D_MAP_READ)
        {
            wined3d_bo_wgpu_read_back(bo, context_wgpu);
        }

        /* The ranges written are uploaded at unmap, before every command
         * not yet submitted; submit the commands that use the old
         * contents. */
        if ((map_flags & WINED3D_MAP_WRITE) && bo->encoder_id == context_wgpu->encoder_id)
            wined3d_context_wgpu_submit(context_wgpu);
    }

    return bo->shadow + (uintptr_t)data->addr;
}

static void adapter_wgpu_unmap_bo_address(struct wined3d_context *context,
        const struct wined3d_bo_address *data, unsigned int range_count, const struct wined3d_range *ranges)
{
    struct wined3d_context_wgpu *context_wgpu = wined3d_context_wgpu(context);
    struct wined3d_bo_wgpu *bo;
    unsigned int i;

    if (!data->buffer_object)
        return;
    bo = wined3d_bo_wgpu(data->buffer_object);

    for (i = 0; i < range_count; ++i)
        wined3d_bo_wgpu_upload(bo, context_wgpu, (uintptr_t)data->addr + ranges[i].offset, ranges[i].size);
}

void adapter_wgpu_copy_bo_address(struct wined3d_context *context,
        const struct wined3d_bo_address *dst, const struct wined3d_bo_address *src,
        unsigned int range_count, const struct wined3d_range *ranges, uint32_t map_flags)
{
    struct wined3d_context_wgpu *context_wgpu = wined3d_context_wgpu(context);
    struct wined3d_bo_wgpu *src_bo, *dst_bo;
    uint8_t *dst_ptr, *src_ptr;
    bool aligned = true;
    unsigned int i;

    src_bo = src->buffer_object ? wined3d_bo_wgpu(src->buffer_object) : NULL;
    dst_bo = dst->buffer_object ? wined3d_bo_wgpu(dst->buffer_object) : NULL;

    for (i = 0; i < range_count; ++i)
    {
        if ((((uintptr_t)src->addr + ranges[i].offset) | ((uintptr_t)dst->addr + ranges[i].offset)
                | ranges[i].size) & 3)
            aligned = false;
    }

    if (src_bo && dst_bo && src_bo->buffer && dst_bo->buffer && aligned)
    {
        WGPUCommandEncoder encoder = wined3d_context_wgpu_get_encoder(context_wgpu);

        if ((map_flags & WINED3D_MAP_DISCARD) && dst_bo->encoder_id == context_wgpu->encoder_id)
            wined3d_bo_wgpu_rename(dst_bo, context_wgpu);

        wined3d_context_wgpu_end_passes(context_wgpu);
        for (i = 0; i < range_count; ++i)
            wgpuCommandEncoderCopyBufferToBuffer(encoder,
                    src_bo->buffer, (uintptr_t)src->addr + ranges[i].offset,
                    dst_bo->buffer, (uintptr_t)dst->addr + ranges[i].offset, ranges[i].size);
        wined3d_context_wgpu_reference_bo(context_wgpu, src_bo);
        wined3d_context_wgpu_reference_bo(context_wgpu, dst_bo);
        dst_bo->gpu_written = true;
        return;
    }

    src_ptr = adapter_wgpu_map_bo_address(context, src, 0, WINED3D_MAP_READ);
    dst_ptr = adapter_wgpu_map_bo_address(context, dst, 0, map_flags);

    for (i = 0; i < range_count; ++i)
        memcpy(dst_ptr + ranges[i].offset, src_ptr + ranges[i].offset, ranges[i].size);

    adapter_wgpu_unmap_bo_address(context, dst, range_count, ranges);
    adapter_wgpu_unmap_bo_address(context, src, 0, NULL);
}

static void adapter_wgpu_flush_bo_address(struct wined3d_context *context,
        const struct wined3d_const_bo_address *data, size_t size)
{
    struct wined3d_bo *bo;

    if (!(bo = data->buffer_object))
        return;

    wined3d_bo_wgpu_upload(wined3d_bo_wgpu(bo), wined3d_context_wgpu(context), (uintptr_t)data->addr, size);
}

static bool adapter_wgpu_alloc_bo(struct wined3d_device *device, struct wined3d_resource *resource,
        unsigned int sub_resource_idx, struct wined3d_bo_address *addr)
{
    /* Mapping from the client thread is not done yet: it would make
     * webgpu.h calls from two threads. */
    return false;
}

static void adapter_wgpu_destroy_bo(struct wined3d_context *context, struct wined3d_bo *bo)
{
    wined3d_context_wgpu_destroy_bo(wined3d_context_wgpu(context), wined3d_bo_wgpu(bo));
}

static HRESULT adapter_wgpu_create_swapchain(struct wined3d_device *device,
        const struct wined3d_swapchain_desc *desc, struct wined3d_swapchain_state_parent *state_parent,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_swapchain **swapchain)
{
    struct wined3d_swapchain_wgpu *swapchain_wgpu;
    HRESULT hr;

    TRACE("device %p, desc %p, state_parent %p, parent %p, parent_ops %p, swapchain %p.\n",
            device, desc, state_parent, parent, parent_ops, swapchain);

    if (!(swapchain_wgpu = calloc(1, sizeof(*swapchain_wgpu))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = wined3d_swapchain_wgpu_init(swapchain_wgpu, device, desc, state_parent, parent,
            parent_ops)))
    {
        WARN("Failed to initialise swapchain, hr %#lx.\n", hr);
        free(swapchain_wgpu);
        return hr;
    }

    TRACE("Created swapchain %p.\n", swapchain_wgpu);
    *swapchain = &swapchain_wgpu->s;

    return hr;
}

static void adapter_wgpu_destroy_swapchain(struct wined3d_swapchain *swapchain)
{
    struct wined3d_swapchain_wgpu *swapchain_wgpu = wined3d_swapchain_wgpu(swapchain);

    wined3d_swapchain_wgpu_cleanup(swapchain_wgpu);
    free(swapchain_wgpu);
}

static HRESULT adapter_wgpu_create_buffer(struct wined3d_device *device,
        const struct wined3d_buffer_desc *desc, const struct wined3d_sub_resource_data *data,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_buffer **buffer)
{
    struct wined3d_buffer_wgpu *buffer_wgpu;
    HRESULT hr;

    TRACE("device %p, desc %p, data %p, parent %p, parent_ops %p, buffer %p.\n",
            device, desc, data, parent, parent_ops, buffer);

    if (!(buffer_wgpu = calloc(1, sizeof(*buffer_wgpu))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = wined3d_buffer_wgpu_init(buffer_wgpu, device, desc, data, parent, parent_ops)))
    {
        WARN("Failed to initialise buffer, hr %#lx.\n", hr);
        free(buffer_wgpu);
        return hr;
    }

    TRACE("Created buffer %p.\n", buffer_wgpu);
    *buffer = &buffer_wgpu->b;

    return hr;
}

static void adapter_wgpu_destroy_buffer(struct wined3d_buffer *buffer)
{
    struct wined3d_buffer_wgpu *buffer_wgpu = wined3d_buffer_wgpu(buffer);
    struct wined3d_device *device = buffer_wgpu->b.resource.device;
    unsigned int swapchain_count = device->swapchain_count;

    TRACE("buffer_wgpu %p.\n", buffer_wgpu);

    if (swapchain_count)
        wined3d_device_incref(device);
    wined3d_buffer_cleanup(&buffer_wgpu->b);
    wined3d_cs_destroy_object(device->cs, free, buffer_wgpu);
    if (swapchain_count)
        wined3d_device_decref(device);
}

static HRESULT adapter_wgpu_create_texture(struct wined3d_device *device,
        const struct wined3d_resource_desc *desc, unsigned int layer_count, unsigned int level_count,
        uint32_t flags, void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_texture **texture)
{
    struct wined3d_texture_wgpu *texture_wgpu;
    HRESULT hr;

    TRACE("device %p, desc %p, layer_count %u, level_count %u, flags %#x, parent %p, parent_ops %p, texture %p.\n",
            device, desc, layer_count, level_count, flags, parent, parent_ops, texture);

    if (!(texture_wgpu = wined3d_texture_allocate_object_memory(sizeof(*texture_wgpu), level_count, layer_count)))
        return E_OUTOFMEMORY;

    if (FAILED(hr = wined3d_texture_wgpu_init(texture_wgpu, device, desc,
            layer_count, level_count, flags, parent, parent_ops)))
    {
        WARN("Failed to initialise texture, hr %#lx.\n", hr);
        free(texture_wgpu);
        return hr;
    }

    TRACE("Created texture %p.\n", texture_wgpu);
    *texture = &texture_wgpu->t;

    return hr;
}

static void adapter_wgpu_destroy_texture(struct wined3d_texture *texture)
{
    struct wined3d_texture_wgpu *texture_wgpu = wined3d_texture_wgpu(texture);
    struct wined3d_device *device = texture_wgpu->t.resource.device;
    unsigned int swapchain_count = device->swapchain_count;

    TRACE("texture_wgpu %p.\n", texture_wgpu);

    if (swapchain_count)
        wined3d_device_incref(device);

    wined3d_texture_sub_resources_destroyed(texture);
    texture->resource.parent_ops->wined3d_object_destroyed(texture->resource.parent);

    wined3d_texture_cleanup(&texture_wgpu->t);
    wined3d_cs_destroy_object(device->cs, free, texture_wgpu);

    if (swapchain_count)
        wined3d_device_decref(device);
}

static HRESULT adapter_wgpu_create_rendertarget_view(const struct wined3d_view_desc *desc,
        struct wined3d_resource *resource, void *parent, const struct wined3d_parent_ops *parent_ops,
        struct wined3d_rendertarget_view **view)
{
    struct wined3d_rendertarget_view_wgpu *view_wgpu;
    HRESULT hr;

    TRACE("desc %s, resource %p, parent %p, parent_ops %p, view %p.\n",
            wined3d_debug_view_desc(desc, resource), resource, parent, parent_ops, view);

    if (!(view_wgpu = calloc(1, sizeof(*view_wgpu))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = wined3d_rendertarget_view_wgpu_init(view_wgpu, desc, resource, parent, parent_ops)))
    {
        WARN("Failed to initialise view, hr %#lx.\n", hr);
        free(view_wgpu);
        return hr;
    }

    TRACE("Created render target view %p.\n", view_wgpu);
    *view = &view_wgpu->v;

    return hr;
}

struct wined3d_view_wgpu_destroy_ctx
{
    struct wined3d_view_wgpu *view_wgpu;
    void *object;
};

static void wined3d_view_wgpu_destroy_object(void *object)
{
    struct wined3d_view_wgpu_destroy_ctx *ctx = object;

    wined3d_view_wgpu_cleanup(ctx->view_wgpu);
    free(ctx->object);
    free(ctx);
}

static void wined3d_view_wgpu_destroy(struct wined3d_device *device, struct wined3d_view_wgpu *view_wgpu,
        void *object)
{
    struct wined3d_view_wgpu_destroy_ctx *ctx;

    if (!(ctx = malloc(sizeof(*ctx))))
    {
        wined3d_cs_finish(device->cs, WINED3D_CS_QUEUE_DEFAULT);
        wined3d_view_wgpu_cleanup(view_wgpu);
        free(object);
        return;
    }
    ctx->view_wgpu = view_wgpu;
    ctx->object = object;
    wined3d_cs_destroy_object(device->cs, wined3d_view_wgpu_destroy_object, ctx);
}

static void adapter_wgpu_destroy_rendertarget_view(struct wined3d_rendertarget_view *view)
{
    struct wined3d_rendertarget_view_wgpu *view_wgpu = wined3d_rendertarget_view_wgpu(view);
    struct wined3d_device *device = view_wgpu->v.resource->device;

    TRACE("view_wgpu %p.\n", view_wgpu);

    wined3d_rendertarget_view_cleanup(&view_wgpu->v);
    wined3d_view_wgpu_destroy(device, &view_wgpu->view_wgpu, view_wgpu);
}

static HRESULT adapter_wgpu_create_shader_resource_view(const struct wined3d_view_desc *desc,
        struct wined3d_resource *resource, void *parent, const struct wined3d_parent_ops *parent_ops,
        struct wined3d_shader_resource_view **view)
{
    struct wined3d_shader_resource_view_wgpu *view_wgpu;
    HRESULT hr;

    TRACE("desc %s, resource %p, parent %p, parent_ops %p, view %p.\n",
            wined3d_debug_view_desc(desc, resource), resource, parent, parent_ops, view);

    if (!(view_wgpu = calloc(1, sizeof(*view_wgpu))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = wined3d_shader_resource_view_wgpu_init(view_wgpu, desc, resource, parent, parent_ops)))
    {
        WARN("Failed to initialise view, hr %#lx.\n", hr);
        free(view_wgpu);
        return hr;
    }

    TRACE("Created shader resource view %p.\n", view_wgpu);
    *view = &view_wgpu->v;

    return hr;
}

static void adapter_wgpu_destroy_shader_resource_view(struct wined3d_shader_resource_view *view)
{
    struct wined3d_shader_resource_view_wgpu *view_wgpu = wined3d_shader_resource_view_wgpu(view);
    struct wined3d_device *device = view_wgpu->v.resource->device;

    TRACE("view_wgpu %p.\n", view_wgpu);

    wined3d_shader_resource_view_cleanup(&view_wgpu->v);
    wined3d_view_wgpu_destroy(device, &view_wgpu->view_wgpu, view_wgpu);
}

static HRESULT adapter_wgpu_create_unordered_access_view(const struct wined3d_view_desc *desc,
        struct wined3d_resource *resource, void *parent, const struct wined3d_parent_ops *parent_ops,
        struct wined3d_unordered_access_view **view)
{
    struct wined3d_unordered_access_view_wgpu *view_wgpu;
    HRESULT hr;

    TRACE("desc %s, resource %p, parent %p, parent_ops %p, view %p.\n",
            wined3d_debug_view_desc(desc, resource), resource, parent, parent_ops, view);

    if (!(view_wgpu = calloc(1, sizeof(*view_wgpu))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = wined3d_unordered_access_view_wgpu_init(view_wgpu, desc, resource, parent, parent_ops)))
    {
        WARN("Failed to initialise view, hr %#lx.\n", hr);
        free(view_wgpu);
        return hr;
    }

    TRACE("Created unordered access view %p.\n", view_wgpu);
    *view = &view_wgpu->v;

    return hr;
}

static void adapter_wgpu_destroy_unordered_access_view(struct wined3d_unordered_access_view *view)
{
    struct wined3d_unordered_access_view_wgpu *view_wgpu = wined3d_unordered_access_view_wgpu(view);
    struct wined3d_device *device = view_wgpu->v.resource->device;

    TRACE("view_wgpu %p.\n", view_wgpu);

    wined3d_unordered_access_view_cleanup(&view_wgpu->v);
    wined3d_view_wgpu_destroy(device, &view_wgpu->view_wgpu, view_wgpu);
}

static HRESULT adapter_wgpu_create_video_decoder_output_view(const struct wined3d_view_desc *desc,
        struct wined3d_texture *texture, void *parent, const struct wined3d_parent_ops *parent_ops,
        struct wined3d_decoder_output_view **view)
{
    FIXME("Not implemented.\n");
    return E_NOTIMPL;
}

static HRESULT adapter_wgpu_create_sampler(struct wined3d_device *device, const struct wined3d_sampler_desc *desc,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_sampler **sampler)
{
    struct wined3d_sampler_wgpu *sampler_wgpu;

    TRACE("device %p, desc %p, parent %p, parent_ops %p, sampler %p.\n",
            device, desc, parent, parent_ops, sampler);

    if (!(sampler_wgpu = calloc(1, sizeof(*sampler_wgpu))))
        return E_OUTOFMEMORY;

    wined3d_sampler_wgpu_init(sampler_wgpu, device, desc, parent, parent_ops);

    TRACE("Created sampler %p.\n", sampler_wgpu);
    *sampler = &sampler_wgpu->s;

    return WINED3D_OK;
}

static void wined3d_sampler_wgpu_destroy_object(void *object)
{
    struct wined3d_sampler_wgpu *sampler_wgpu = object;

    TRACE("sampler_wgpu %p.\n", sampler_wgpu);

    if (sampler_wgpu->sampler)
        wgpuSamplerRelease(sampler_wgpu->sampler);
    free(sampler_wgpu);
}

static void adapter_wgpu_destroy_sampler(struct wined3d_sampler *sampler)
{
    struct wined3d_sampler_wgpu *sampler_wgpu = wined3d_sampler_wgpu(sampler);

    TRACE("sampler_wgpu %p.\n", sampler_wgpu);

    wined3d_cs_destroy_object(sampler->device->cs, wined3d_sampler_wgpu_destroy_object, sampler_wgpu);
}

static HRESULT adapter_wgpu_create_query(struct wined3d_device *device, enum wined3d_query_type type,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_query **query)
{
    TRACE("device %p, type %#x, parent %p, parent_ops %p, query %p.\n",
            device, type, parent, parent_ops, query);

    return wined3d_query_wgpu_create(device, type, parent, parent_ops, query);
}

static void wined3d_query_wgpu_destroy_object(void *object)
{
    struct wined3d_query *query = object;

    TRACE("query %p.\n", query);

    query->query_ops->query_destroy(query);
}

static void adapter_wgpu_destroy_query(struct wined3d_query *query)
{
    TRACE("query %p.\n", query);

    wined3d_cs_destroy_object(query->device->cs, wined3d_query_wgpu_destroy_object, query);
}

static void adapter_wgpu_flush_context(struct wined3d_context *context)
{
    TRACE("context %p.\n", context);

    wined3d_context_wgpu_submit(wined3d_context_wgpu(context));
}

static void adapter_wgpu_draw_primitive(struct wined3d_device *device,
        const struct wined3d_state *state, const struct wined3d_draw_parameters *parameters)
{
    struct wined3d_context_wgpu *context_wgpu;
    struct wined3d_buffer *indirect = NULL;
    uint32_t instance_count;

    TRACE("device %p, state %p, parameters %p.\n", device, state, parameters);

    context_wgpu = wined3d_context_wgpu(context_acquire(device, NULL, 0));

    if (parameters->indirect)
        indirect = parameters->u.indirect.buffer;

    if (!wined3d_context_wgpu_apply_draw_state(context_wgpu, state, indirect, parameters->indexed))
    {
        WARN("Failed to apply draw state.\n");
        context_release(&context_wgpu->c);
        return;
    }

    if (!wined3d_context_wgpu_check_draw(context_wgpu, state, parameters))
    {
        context_release(&context_wgpu->c);
        return;
    }

    if (parameters->indirect)
    {
        struct wined3d_bo_wgpu *bo = wined3d_bo_wgpu(indirect->buffer_object);

        wined3d_context_wgpu_reference_bo(context_wgpu, bo);
        if (parameters->indexed)
            wgpuRenderPassEncoderDrawIndexedIndirect(context_wgpu->render_pass, bo->buffer,
                    bo->b.buffer_offset + parameters->u.indirect.offset);
        else
            wgpuRenderPassEncoderDrawIndirect(context_wgpu->render_pass, bo->buffer,
                    bo->b.buffer_offset + parameters->u.indirect.offset);
    }
    else
    {
        if (!(instance_count = parameters->u.direct.instance_count))
            instance_count = 1;

        if (parameters->indexed)
            wgpuRenderPassEncoderDrawIndexed(context_wgpu->render_pass, parameters->u.direct.index_count,
                    instance_count, parameters->u.direct.start_idx, parameters->u.direct.base_vertex_idx,
                    parameters->u.direct.start_instance);
        else
            wgpuRenderPassEncoderDraw(context_wgpu->render_pass, parameters->u.direct.index_count,
                    instance_count, parameters->u.direct.start_idx, parameters->u.direct.start_instance);
    }

    ++context_wgpu->work_count;

    context_release(&context_wgpu->c);
}

static void adapter_wgpu_dispatch_compute(struct wined3d_device *device,
        const struct wined3d_state *state, const struct wined3d_dispatch_parameters *parameters)
{
    struct wined3d_context_wgpu *context_wgpu;
    struct wined3d_buffer *indirect = NULL;

    TRACE("device %p, state %p, parameters %p.\n", device, state, parameters);

    context_wgpu = wined3d_context_wgpu(context_acquire(device, NULL, 0));

    if (parameters->indirect)
        indirect = parameters->u.indirect.buffer;

    if (!wined3d_context_wgpu_apply_compute_state(context_wgpu, state, indirect))
    {
        WARN("Failed to apply compute state.\n");
        context_release(&context_wgpu->c);
        return;
    }

    if (parameters->indirect)
    {
        struct wined3d_bo_wgpu *bo = wined3d_bo_wgpu(indirect->buffer_object);

        wined3d_context_wgpu_reference_bo(context_wgpu, bo);
        wgpuComputePassEncoderDispatchWorkgroupsIndirect(context_wgpu->compute_pass, bo->buffer,
                bo->b.buffer_offset + parameters->u.indirect.offset);
    }
    else
    {
        const struct wined3d_direct_dispatch_parameters *direct = &parameters->u.direct;

        wgpuComputePassEncoderDispatchWorkgroups(context_wgpu->compute_pass,
                direct->group_count_x, direct->group_count_y, direct->group_count_z);
    }

    ++context_wgpu->work_count;

    context_release(&context_wgpu->c);
}

static void adapter_wgpu_clear_uav(struct wined3d_context *context,
        struct wined3d_unordered_access_view *view, const struct wined3d_uvec4 *clear_value, bool fp)
{
    FIXME("context %p, view %p, clear_value %s, fp %d, not implemented.\n",
            context, view, debug_uvec4(clear_value), fp);
}

static void adapter_wgpu_generate_mipmap(struct wined3d_context *context, struct wined3d_shader_resource_view *view)
{
    FIXME("context %p, view %p, not implemented.\n", context, view);
}

static const struct wined3d_adapter_ops wined3d_adapter_wgpu_ops =
{
    .adapter_destroy = adapter_wgpu_destroy,
    .adapter_create_device = adapter_wgpu_create_device,
    .adapter_destroy_device = adapter_wgpu_destroy_device,
    .adapter_acquire_context = adapter_wgpu_acquire_context,
    .adapter_release_context = adapter_wgpu_release_context,
    .adapter_get_wined3d_caps = adapter_wgpu_get_wined3d_caps,
    .adapter_check_format = adapter_wgpu_check_format,
    .adapter_init_3d = adapter_wgpu_init_3d,
    .adapter_uninit_3d = adapter_wgpu_uninit_3d,
    .adapter_map_bo_address = adapter_wgpu_map_bo_address,
    .adapter_unmap_bo_address = adapter_wgpu_unmap_bo_address,
    .adapter_copy_bo_address = adapter_wgpu_copy_bo_address,
    .adapter_flush_bo_address = adapter_wgpu_flush_bo_address,
    .adapter_alloc_bo = adapter_wgpu_alloc_bo,
    .adapter_destroy_bo = adapter_wgpu_destroy_bo,
    .adapter_create_swapchain = adapter_wgpu_create_swapchain,
    .adapter_destroy_swapchain = adapter_wgpu_destroy_swapchain,
    .adapter_create_buffer = adapter_wgpu_create_buffer,
    .adapter_destroy_buffer = adapter_wgpu_destroy_buffer,
    .adapter_create_texture = adapter_wgpu_create_texture,
    .adapter_destroy_texture = adapter_wgpu_destroy_texture,
    .adapter_create_rendertarget_view = adapter_wgpu_create_rendertarget_view,
    .adapter_destroy_rendertarget_view = adapter_wgpu_destroy_rendertarget_view,
    .adapter_create_shader_resource_view = adapter_wgpu_create_shader_resource_view,
    .adapter_destroy_shader_resource_view = adapter_wgpu_destroy_shader_resource_view,
    .adapter_create_unordered_access_view = adapter_wgpu_create_unordered_access_view,
    .adapter_destroy_unordered_access_view = adapter_wgpu_destroy_unordered_access_view,
    .adapter_create_video_decoder_output_view = adapter_wgpu_create_video_decoder_output_view,
    .adapter_destroy_video_decoder_output_view = wined3d_decoder_output_view_cleanup,
    .adapter_create_sampler = adapter_wgpu_create_sampler,
    .adapter_destroy_sampler = adapter_wgpu_destroy_sampler,
    .adapter_create_query = adapter_wgpu_create_query,
    .adapter_destroy_query = adapter_wgpu_destroy_query,
    .adapter_flush_context = adapter_wgpu_flush_context,
    .adapter_draw_primitive = adapter_wgpu_draw_primitive,
    .adapter_dispatch_compute = adapter_wgpu_dispatch_compute,
    .adapter_clear_uav = adapter_wgpu_clear_uav,
    .adapter_generate_mipmap = adapter_wgpu_generate_mipmap,
};

static void wgpu_adapter_callback(WGPURequestAdapterStatus status, WGPUAdapter adapter,
        WGPUStringView message, void *userdata1, void *userdata2)
{
    WGPUAdapter *out = userdata1;

    if (status != WGPURequestAdapterStatus_Success)
        WARN("Failed to get a WebGPU adapter, status %#x: %s.\n", status,
                debugstr_an(message.data, message.data ? (int)message.length : 0));
    *out = status == WGPURequestAdapterStatus_Success ? adapter : NULL;
}

static WGPUInstance wined3d_wgpu_instance;

static bool wined3d_init_webgpu(struct wined3d_wgpu_info *wgpu_info)
{
    static const WGPUInstanceFeatureName instance_features[] = {WGPUInstanceFeatureName_TimedWaitAny};
    WGPURequestAdapterCallbackInfo callback_info = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    WGPUInstanceDescriptor instance_desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    WGPULimits limits = WGPU_LIMITS_INIT;

    /* One instance for the process: Dawn does not take an instance
     * released and another made. */
    if (!wined3d_wgpu_instance)
    {
        instance_desc.requiredFeatureCount = ARRAY_SIZE(instance_features);
        instance_desc.requiredFeatures = instance_features;
        if (!(wined3d_wgpu_instance = wgpuCreateInstance(&instance_desc)))
        {
            WARN("Failed to create a WebGPU instance.\n");
            return false;
        }
    }
    wgpu_info->instance = wined3d_wgpu_instance;

    options.powerPreference = WGPUPowerPreference_HighPerformance;
    callback_info.mode = WGPUCallbackMode_WaitAnyOnly;
    callback_info.callback = wgpu_adapter_callback;
    callback_info.userdata1 = &wgpu_info->adapter;
    if (!wined3d_wgpu_wait(wgpu_info->instance, wgpuInstanceRequestAdapter(wgpu_info->instance,
            &options, callback_info)) || !wgpu_info->adapter)
    {
        WARN("No WebGPU adapter.\n");
        return false;
    }

    if (wgpuAdapterGetLimits(wgpu_info->adapter, &limits) != WGPUStatus_Success
            || wgpuAdapterGetInfo(wgpu_info->adapter, &info) != WGPUStatus_Success)
    {
        WARN("Failed to get the adapter's limits and information.\n");
        wgpuAdapterRelease(wgpu_info->adapter);
        return false;
    }
    limits.nextInChain = NULL;
    wgpu_info->limits = limits;
    wgpu_info->adapter_info = info;

#define HAS(name) wgpuAdapterHasFeature(wgpu_info->adapter, WGPUFeatureName_##name)
    wgpu_info->texture_compression_bc = HAS(TextureCompressionBC);
    wgpu_info->texture_compression_bc_3d = HAS(TextureCompressionBCSliced3D);
    wgpu_info->depth32float_stencil8 = HAS(Depth32FloatStencil8);
    wgpu_info->depth_clip_control = HAS(DepthClipControl);
    wgpu_info->float32_filterable = HAS(Float32Filterable);
    wgpu_info->float32_blendable = HAS(Float32Blendable);
    wgpu_info->rg11b10_renderable = HAS(RG11B10UfloatRenderable);
    wgpu_info->dual_source_blending = HAS(DualSourceBlending);
    wgpu_info->clip_distances = HAS(ClipDistances);
    wgpu_info->texture_component_swizzle = HAS(TextureComponentSwizzle);
    wgpu_info->texture_formats_tier1 = HAS(TextureFormatsTier1);
    wgpu_info->indirect_first_instance = HAS(IndirectFirstInstance);
#undef HAS

    TRACE("Adapter %s (%s), vendor %#x, device %#x, backend %#x.\n",
            debugstr_an(info.device.data, info.device.data ? (int)info.device.length : 0),
            debugstr_an(info.description.data, info.description.data ? (int)info.description.length : 0),
            info.vendorID, info.deviceID, info.backendType);

    return true;
}

static enum wined3d_display_driver guess_display_driver(enum wined3d_pci_vendor vendor)
{
    switch (vendor)
    {
        case HW_VENDOR_AMD:    return DRIVER_AMD_RX;
        case HW_VENDOR_INTEL:  return DRIVER_INTEL_HD4000;
        case HW_VENDOR_NVIDIA: return DRIVER_NVIDIA_GEFORCE8;
        default:               return DRIVER_WINE;
    }
}

static bool adapter_wgpu_init_driver_info(struct wined3d_adapter_wgpu *adapter_wgpu)
{
    const WGPUAdapterInfo *info = &adapter_wgpu->wgpu_info.adapter_info;
    const struct wined3d_gpu_description *gpu_description;
    struct wined3d_gpu_description description;
    char name[128];
    /* WebGPU says nothing of memory sizes. */
    UINT64 vram_bytes = (UINT64)1024 * 1024 * 1024, sysmem_bytes = vram_bytes;

    if (!(gpu_description = wined3d_get_user_override_gpu_description(info->vendorID, info->deviceID)))
        gpu_description = wined3d_get_gpu_description(info->vendorID, info->deviceID);

    if (!gpu_description)
    {
        snprintf(name, sizeof(name), "%.*s", info->device.data ? (int)min(info->device.length, 100) : 0,
                info->device.data ? info->device.data : "");
        if (!name[0])
            strcpy(name, "WebGPU");

        description.vendor = info->vendorID;
        description.device = info->deviceID;
        description.description = name;
        description.driver = guess_display_driver(info->vendorID);
        description.vidmem = vram_bytes;

        gpu_description = &description;
    }

    return wined3d_driver_info_init(&adapter_wgpu->a.driver_info, gpu_description,
            adapter_wgpu->a.d3d_info.feature_level, vram_bytes, sysmem_bytes);
}

static void wined3d_adapter_wgpu_init_d3d_info(struct wined3d_adapter_wgpu *adapter_wgpu,
        uint32_t wined3d_creation_flags)
{
    struct wined3d_d3d_info *d3d_info = &adapter_wgpu->a.d3d_info;
    const struct wined3d_wgpu_info *wgpu_info = &adapter_wgpu->wgpu_info;
    struct wined3d_vertex_caps vertex_caps;
    struct shader_caps shader_caps;
    unsigned int shader_model;

    adapter_wgpu->a.shader_backend->shader_get_caps(&adapter_wgpu->a, &shader_caps);
    adapter_wgpu->a.vertex_pipe->vp_get_caps(&adapter_wgpu->a, &vertex_caps);
    adapter_wgpu->a.fragment_pipe->get_caps(&adapter_wgpu->a, &d3d_info->ffp_fragment_caps);

    d3d_info->limits.vs_version = shader_caps.vs_version;
    d3d_info->limits.hs_version = shader_caps.hs_version;
    d3d_info->limits.ds_version = shader_caps.ds_version;
    d3d_info->limits.gs_version = shader_caps.gs_version;
    d3d_info->limits.ps_version = shader_caps.ps_version;
    d3d_info->limits.cs_version = shader_caps.cs_version;
    d3d_info->limits.vs_uniform_count = shader_caps.vs_uniform_count;
    d3d_info->limits.ps_uniform_count = shader_caps.ps_uniform_count;
    d3d_info->limits.varying_count = shader_caps.varying_count;
    d3d_info->limits.ffp_vertex_blend_matrices = vertex_caps.max_vertex_blend_matrices;
    d3d_info->limits.active_light_count = vertex_caps.max_active_lights;

    d3d_info->limits.max_rt_count = min(WINED3D_MAX_RENDER_TARGETS, wgpu_info->limits.maxColorAttachments);
    d3d_info->limits.max_clip_distances = wgpu_info->clip_distances ? WINED3D_MAX_CLIP_DISTANCES : 0;
    d3d_info->limits.texture_size = wgpu_info->limits.maxTextureDimension2D;
    d3d_info->limits.pointsize_max = 1.0f;
    /* WebGPU has 1 and 4 samples. */
    d3d_info->limits.sample_count = 4;

    d3d_info->wined3d_creation_flags = wined3d_creation_flags;

    d3d_info->emulated_flatshading = vertex_caps.emulated_flatshading;
    d3d_info->ffp_alpha_test = false;
    d3d_info->shader_double_precision = false;
    d3d_info->shader_output_interpolation = false;
    d3d_info->viewport_array_index_any_shader = false;
    d3d_info->stencil_export = false;
    d3d_info->simple_instancing = true;
    d3d_info->min_max_filtering = false;
    d3d_info->unconditional_npot = true;
    d3d_info->draw_base_vertex_offset = true;
    d3d_info->vertex_bgra = true;
    d3d_info->texture_swizzle = wgpu_info->texture_component_swizzle;
    d3d_info->srgb_read_control = false;
    d3d_info->srgb_write_control = false;
    d3d_info->clip_control = true;
    d3d_info->full_ffp_varyings = !!(shader_caps.wined3d_caps & WINED3D_SHADER_CAP_FULL_FFP_VARYINGS);
    d3d_info->scaled_resolve = false;
    d3d_info->pbo = false;
    d3d_info->subpixel_viewport = true;
    d3d_info->fences = true;
    d3d_info->persistent_map = false;
    d3d_info->gpu_push_constants = true;
    d3d_info->ffp_hlsl = true;
    d3d_info->filling_convention_offset = 0.0f;
    d3d_info->multisample_draw_location = WINED3D_LOCATION_TEXTURE_RGB;

    /* Vertex and pixel shaders, and compute; no geometry or tessellation
     * shaders, which Direct3D 10 requires. */
    shader_model = min(shader_caps.vs_version, shader_caps.ps_version);
    if (!shader_model)
        d3d_info->feature_level = WINED3D_FEATURE_LEVEL_7;
    else if (shader_model <= 1)
        d3d_info->feature_level = WINED3D_FEATURE_LEVEL_8;
    else
        d3d_info->feature_level = min(WINED3D_FEATURE_LEVEL_11_1, wined3d_settings.webgpu_feature_level);
}

static BOOL wined3d_adapter_wgpu_init(struct wined3d_adapter_wgpu *adapter_wgpu,
        unsigned int ordinal, unsigned int wined3d_creation_flags)
{
    struct wined3d_wgpu_info *wgpu_info = &adapter_wgpu->wgpu_info;
    struct wined3d_adapter *adapter = &adapter_wgpu->a;
    LUID primary_luid, *luid = NULL;

    TRACE("adapter_wgpu %p, ordinal %u, wined3d_creation_flags %#x.\n",
            adapter_wgpu, ordinal, wined3d_creation_flags);

    if (!wined3d_init_webgpu(wgpu_info))
    {
        WARN("Failed to initialise WebGPU.\n");
        return FALSE;
    }

    if (ordinal == 0 && wined3d_get_primary_adapter_luid(&primary_luid))
        luid = &primary_luid;

    if (!wined3d_adapter_init(adapter, ordinal, luid, &wined3d_adapter_wgpu_ops))
        goto fail_webgpu;

    adapter->vertex_pipe = wined3d_wgsl_vertex_pipe_init_wgpu();
    adapter->fragment_pipe = wined3d_wgsl_fragment_pipe_init_wgpu();
    adapter->misc_state_template = misc_state_template_wgpu;
    adapter->shader_backend = wined3d_wgsl_shader_backend_init_wgpu();

    wined3d_adapter_wgpu_init_d3d_info(adapter_wgpu, wined3d_creation_flags);

    if (!adapter_wgpu_init_driver_info(adapter_wgpu))
        goto fail;
    adapter->vram_bytes_used = 0;

    if (!wined3d_adapter_wgpu_init_format_info(adapter_wgpu))
        goto fail;

    return TRUE;

fail:
    wined3d_adapter_cleanup(adapter);
fail_webgpu:
    wgpuAdapterInfoFreeMembers(wgpu_info->adapter_info);
    wgpuAdapterRelease(wgpu_info->adapter);
    return FALSE;
}

struct wined3d_adapter *wined3d_adapter_wgpu_create(unsigned int ordinal,
        unsigned int wined3d_creation_flags)
{
    struct wined3d_adapter_wgpu *adapter_wgpu;

    if (!(adapter_wgpu = calloc(1, sizeof(*adapter_wgpu))))
        return NULL;

    if (!wined3d_adapter_wgpu_init(adapter_wgpu, ordinal, wined3d_creation_flags))
    {
        free(adapter_wgpu);
        return NULL;
    }

    TRACE("Created adapter %p.\n", adapter_wgpu);

    return &adapter_wgpu->a;
}

/* Formats. */

static const struct
{
    enum wined3d_format_id id;
    WGPUTextureFormat texture_format;
    WGPUVertexFormat vertex_format;
    const char *fixup;
}
wgpu_formats[] =
{
    {WINED3DFMT_R32G32B32A32_FLOAT,     WGPUTextureFormat_RGBA32Float,      WGPUVertexFormat_Float32x4},
    {WINED3DFMT_R32G32B32A32_UINT,      WGPUTextureFormat_RGBA32Uint,       WGPUVertexFormat_Uint32x4},
    {WINED3DFMT_R32G32B32A32_SINT,      WGPUTextureFormat_RGBA32Sint,       WGPUVertexFormat_Sint32x4},
    {WINED3DFMT_R32G32B32_FLOAT,        WGPUTextureFormat_Undefined,        WGPUVertexFormat_Float32x3},
    {WINED3DFMT_R32G32B32_UINT,         WGPUTextureFormat_Undefined,        WGPUVertexFormat_Uint32x3},
    {WINED3DFMT_R32G32B32_SINT,         WGPUTextureFormat_Undefined,        WGPUVertexFormat_Sint32x3},
    {WINED3DFMT_R16G16B16A16_FLOAT,     WGPUTextureFormat_RGBA16Float,      WGPUVertexFormat_Float16x4},
    {WINED3DFMT_R16G16B16A16_UNORM,     WGPUTextureFormat_RGBA16Unorm,      WGPUVertexFormat_Unorm16x4},
    {WINED3DFMT_R16G16B16A16_UINT,      WGPUTextureFormat_RGBA16Uint,       WGPUVertexFormat_Uint16x4},
    {WINED3DFMT_R16G16B16A16_SNORM,     WGPUTextureFormat_RGBA16Snorm,      WGPUVertexFormat_Snorm16x4},
    {WINED3DFMT_R16G16B16A16_SINT,      WGPUTextureFormat_RGBA16Sint,       WGPUVertexFormat_Sint16x4},
    {WINED3DFMT_R32G32_FLOAT,           WGPUTextureFormat_RG32Float,        WGPUVertexFormat_Float32x2},
    {WINED3DFMT_R32G32_UINT,            WGPUTextureFormat_RG32Uint,         WGPUVertexFormat_Uint32x2},
    {WINED3DFMT_R32G32_SINT,            WGPUTextureFormat_RG32Sint,         WGPUVertexFormat_Sint32x2},
    {WINED3DFMT_R10G10B10A2_UNORM,      WGPUTextureFormat_RGB10A2Unorm,     WGPUVertexFormat_Unorm10_10_10_2},
    {WINED3DFMT_R10G10B10A2_UINT,       WGPUTextureFormat_RGB10A2Uint,      WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_R11G11B10_FLOAT,        WGPUTextureFormat_RG11B10Ufloat,    WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_R8G8_UNORM,             WGPUTextureFormat_RG8Unorm,         WGPUVertexFormat_Unorm8x2},
    {WINED3DFMT_R8G8_UINT,              WGPUTextureFormat_RG8Uint,          WGPUVertexFormat_Uint8x2},
    {WINED3DFMT_R8G8_SNORM,             WGPUTextureFormat_RG8Snorm,         WGPUVertexFormat_Snorm8x2},
    {WINED3DFMT_R8G8_SINT,              WGPUTextureFormat_RG8Sint,          WGPUVertexFormat_Sint8x2},
    {WINED3DFMT_R8G8B8A8_UNORM,         WGPUTextureFormat_RGBA8Unorm,       WGPUVertexFormat_Unorm8x4},
    {WINED3DFMT_R8G8B8A8_UNORM_SRGB,    WGPUTextureFormat_RGBA8UnormSrgb,   WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_R8G8B8A8_UINT,          WGPUTextureFormat_RGBA8Uint,        WGPUVertexFormat_Uint8x4},
    {WINED3DFMT_R8G8B8A8_SNORM,         WGPUTextureFormat_RGBA8Snorm,       WGPUVertexFormat_Snorm8x4},
    {WINED3DFMT_R8G8B8A8_SINT,          WGPUTextureFormat_RGBA8Sint,        WGPUVertexFormat_Sint8x4},
    {WINED3DFMT_R16G16_FLOAT,           WGPUTextureFormat_RG16Float,        WGPUVertexFormat_Float16x2},
    {WINED3DFMT_R16G16_UNORM,           WGPUTextureFormat_RG16Unorm,        WGPUVertexFormat_Unorm16x2},
    {WINED3DFMT_R16G16_UINT,            WGPUTextureFormat_RG16Uint,         WGPUVertexFormat_Uint16x2},
    {WINED3DFMT_R16G16_SNORM,           WGPUTextureFormat_RG16Snorm,        WGPUVertexFormat_Snorm16x2},
    {WINED3DFMT_R16G16_SINT,            WGPUTextureFormat_RG16Sint,         WGPUVertexFormat_Sint16x2},
    {WINED3DFMT_D32_FLOAT,              WGPUTextureFormat_Depth32Float,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_R32_FLOAT,              WGPUTextureFormat_R32Float,         WGPUVertexFormat_Float32},
    {WINED3DFMT_R32_UINT,               WGPUTextureFormat_R32Uint,          WGPUVertexFormat_Uint32},
    {WINED3DFMT_R32_SINT,               WGPUTextureFormat_R32Sint,          WGPUVertexFormat_Sint32},
    {WINED3DFMT_R16_FLOAT,              WGPUTextureFormat_R16Float,         WGPUVertexFormat_Float16},
    {WINED3DFMT_D16_UNORM,              WGPUTextureFormat_Depth16Unorm,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_R16_UNORM,              WGPUTextureFormat_R16Unorm,         WGPUVertexFormat_Unorm16},
    {WINED3DFMT_R16_UINT,               WGPUTextureFormat_R16Uint,          WGPUVertexFormat_Uint16},
    {WINED3DFMT_R16_SNORM,              WGPUTextureFormat_R16Snorm,         WGPUVertexFormat_Snorm16},
    {WINED3DFMT_R16_SINT,               WGPUTextureFormat_R16Sint,          WGPUVertexFormat_Sint16},
    {WINED3DFMT_R8_UNORM,               WGPUTextureFormat_R8Unorm,          WGPUVertexFormat_Unorm8},
    {WINED3DFMT_R8_UINT,                WGPUTextureFormat_R8Uint,           WGPUVertexFormat_Uint8},
    {WINED3DFMT_R8_SNORM,               WGPUTextureFormat_R8Snorm,          WGPUVertexFormat_Snorm8},
    {WINED3DFMT_R8_SINT,                WGPUTextureFormat_R8Sint,           WGPUVertexFormat_Sint8},
    {WINED3DFMT_A8_UNORM,               WGPUTextureFormat_R8Unorm,          WGPU_VERTEX_FORMAT_NONE, "000X"},
    {WINED3DFMT_L8_UNORM,               WGPUTextureFormat_R8Unorm,          WGPU_VERTEX_FORMAT_NONE, "XXX1"},
    {WINED3DFMT_L8A8_UNORM,             WGPUTextureFormat_RG8Unorm,         WGPU_VERTEX_FORMAT_NONE, "XXXY"},
    {WINED3DFMT_L16_UNORM,              WGPUTextureFormat_R16Unorm,         WGPU_VERTEX_FORMAT_NONE, "XXX1"},
    {WINED3DFMT_B8G8R8A8_UNORM,         WGPUTextureFormat_BGRA8Unorm,       WGPUVertexFormat_Unorm8x4BGRA},
    {WINED3DFMT_B8G8R8A8_UNORM_SRGB,    WGPUTextureFormat_BGRA8UnormSrgb,   WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_B8G8R8X8_UNORM,         WGPUTextureFormat_BGRA8Unorm,       WGPU_VERTEX_FORMAT_NONE, "XYZ1"},
    {WINED3DFMT_B8G8R8X8_UNORM_SRGB,    WGPUTextureFormat_BGRA8UnormSrgb,   WGPU_VERTEX_FORMAT_NONE, "XYZ1"},
    {WINED3DFMT_R8G8B8X8_UNORM,         WGPUTextureFormat_RGBA8Unorm,       WGPU_VERTEX_FORMAT_NONE, "XYZ1"},
    {WINED3DFMT_DXT1,                   WGPUTextureFormat_BC1RGBAUnorm,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_DXT2,                   WGPUTextureFormat_BC2RGBAUnorm,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_DXT3,                   WGPUTextureFormat_BC2RGBAUnorm,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_DXT4,                   WGPUTextureFormat_BC3RGBAUnorm,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_DXT5,                   WGPUTextureFormat_BC3RGBAUnorm,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC1_UNORM,              WGPUTextureFormat_BC1RGBAUnorm,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC1_UNORM_SRGB,         WGPUTextureFormat_BC1RGBAUnormSrgb, WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC2_UNORM,              WGPUTextureFormat_BC2RGBAUnorm,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC2_UNORM_SRGB,         WGPUTextureFormat_BC2RGBAUnormSrgb, WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC3_UNORM,              WGPUTextureFormat_BC3RGBAUnorm,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC3_UNORM_SRGB,         WGPUTextureFormat_BC3RGBAUnormSrgb, WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC4_UNORM,              WGPUTextureFormat_BC4RUnorm,        WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC4_SNORM,              WGPUTextureFormat_BC4RSnorm,        WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC5_UNORM,              WGPUTextureFormat_BC5RGUnorm,       WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC5_SNORM,              WGPUTextureFormat_BC5RGSnorm,       WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC6H_UF16,              WGPUTextureFormat_BC6HRGBUfloat,    WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC6H_SF16,              WGPUTextureFormat_BC6HRGBFloat,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC7_UNORM,              WGPUTextureFormat_BC7RGBAUnorm,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_BC7_UNORM_SRGB,         WGPUTextureFormat_BC7RGBAUnormSrgb, WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_R9G9B9E5_SHAREDEXP,     WGPUTextureFormat_RGB9E5Ufloat,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_D32_FLOAT_S8X24_UINT,   WGPUTextureFormat_Depth32FloatStencil8, WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_R32_FLOAT_X8X24_TYPELESS, WGPUTextureFormat_Depth32FloatStencil8, WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_X32_TYPELESS_G8X24_UINT, WGPUTextureFormat_Depth32FloatStencil8, WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_D24_UNORM_S8_UINT,      WGPUTextureFormat_Depth24PlusStencil8, WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_X8D24_UNORM,            WGPUTextureFormat_Depth24Plus,      WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_S1_UINT_D15_UNORM,      WGPUTextureFormat_Depth24PlusStencil8, WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_S4X4_UINT_D24_UNORM,    WGPUTextureFormat_Depth24PlusStencil8, WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_S8_UINT_D24_FLOAT,      WGPUTextureFormat_Depth24PlusStencil8, WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_D16_LOCKABLE,           WGPUTextureFormat_Depth16Unorm,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_D32_UNORM,              WGPUTextureFormat_Depth32Float,     WGPU_VERTEX_FORMAT_NONE},
    {WINED3DFMT_INTZ,                   WGPUTextureFormat_Depth24PlusStencil8, WGPU_VERTEX_FORMAT_NONE},
};

/* Packed 16-bit and 24-bit formats, as BGRA8. */
static inline uint32_t expand_bits(uint32_t v, unsigned int bits)
{
    return (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1);
}

static inline uint32_t reduce_bits(uint32_t v, unsigned int bits)
{
    return (v * ((1u << bits) - 1) + 127) / 255;
}

static void convert_b5g6r5_upload(const uint8_t *src, uint32_t *dst, unsigned int width)
{
    const uint16_t *s = (const uint16_t *)src;
    unsigned int x;

    for (x = 0; x < width; ++x)
        dst[x] = 0xff000000 | expand_bits(s[x] >> 11, 5) << 16 | expand_bits((s[x] >> 5) & 0x3f, 6) << 8
                | expand_bits(s[x] & 0x1f, 5);
}

static void convert_b5g6r5_download(const uint32_t *src, uint8_t *dst, unsigned int width)
{
    uint16_t *d = (uint16_t *)dst;
    unsigned int x;

    for (x = 0; x < width; ++x)
        d[x] = reduce_bits((src[x] >> 16) & 0xff, 5) << 11 | reduce_bits((src[x] >> 8) & 0xff, 6) << 5
                | reduce_bits(src[x] & 0xff, 5);
}

static void convert_b5g5r5a1_upload(const uint8_t *src, uint32_t *dst, unsigned int width)
{
    const uint16_t *s = (const uint16_t *)src;
    unsigned int x;

    for (x = 0; x < width; ++x)
        dst[x] = (s[x] & 0x8000 ? 0xff000000 : 0) | expand_bits((s[x] >> 10) & 0x1f, 5) << 16
                | expand_bits((s[x] >> 5) & 0x1f, 5) << 8 | expand_bits(s[x] & 0x1f, 5);
}

static void convert_b5g5r5a1_download(const uint32_t *src, uint8_t *dst, unsigned int width)
{
    uint16_t *d = (uint16_t *)dst;
    unsigned int x;

    for (x = 0; x < width; ++x)
        d[x] = (src[x] >> 31) << 15 | reduce_bits((src[x] >> 16) & 0xff, 5) << 10
                | reduce_bits((src[x] >> 8) & 0xff, 5) << 5 | reduce_bits(src[x] & 0xff, 5);
}

static void convert_b4g4r4a4_upload(const uint8_t *src, uint32_t *dst, unsigned int width)
{
    const uint16_t *s = (const uint16_t *)src;
    unsigned int x;

    for (x = 0; x < width; ++x)
        dst[x] = expand_bits(s[x] >> 12, 4) << 24 | expand_bits((s[x] >> 8) & 0xf, 4) << 16
                | expand_bits((s[x] >> 4) & 0xf, 4) << 8 | expand_bits(s[x] & 0xf, 4);
}

static void convert_b4g4r4a4_download(const uint32_t *src, uint8_t *dst, unsigned int width)
{
    uint16_t *d = (uint16_t *)dst;
    unsigned int x;

    for (x = 0; x < width; ++x)
        d[x] = reduce_bits(src[x] >> 24, 4) << 12 | reduce_bits((src[x] >> 16) & 0xff, 4) << 8
                | reduce_bits((src[x] >> 8) & 0xff, 4) << 4 | reduce_bits(src[x] & 0xff, 4);
}

static void convert_b8g8r8_upload(const uint8_t *src, uint32_t *dst, unsigned int width)
{
    unsigned int x;

    for (x = 0; x < width; ++x)
        dst[x] = 0xff000000 | src[3 * x + 2] << 16 | src[3 * x + 1] << 8 | src[3 * x];
}

static void convert_b8g8r8_download(const uint32_t *src, uint8_t *dst, unsigned int width)
{
    unsigned int x;

    for (x = 0; x < width; ++x)
    {
        dst[3 * x] = src[x];
        dst[3 * x + 1] = src[x] >> 8;
        dst[3 * x + 2] = src[x] >> 16;
    }
}

static const struct
{
    enum wined3d_format_id id;
    void (*upload)(const uint8_t *src, uint32_t *dst, unsigned int width);
    void (*download)(const uint32_t *src, uint8_t *dst, unsigned int width);
    const char *fixup;
}
wgpu_converted_formats[] =
{
    {WINED3DFMT_B5G6R5_UNORM,   convert_b5g6r5_upload,   convert_b5g6r5_download},
    {WINED3DFMT_B5G5R5A1_UNORM, convert_b5g5r5a1_upload, convert_b5g5r5a1_download},
    {WINED3DFMT_B5G5R5X1_UNORM, convert_b5g5r5a1_upload, convert_b5g5r5a1_download, "XYZ1"},
    {WINED3DFMT_B4G4R4A4_UNORM, convert_b4g4r4a4_upload, convert_b4g4r4a4_download},
    {WINED3DFMT_B4G4R4X4_UNORM, convert_b4g4r4a4_upload, convert_b4g4r4a4_download, "XYZ1"},
    {WINED3DFMT_B8G8R8_UNORM,   convert_b8g8r8_upload,   convert_b8g8r8_download},
};

static WGPUTextureFormat wgpu_srgb_toggle_format(WGPUTextureFormat format)
{
    static const WGPUTextureFormat pairs[][2] =
    {
        {WGPUTextureFormat_RGBA8Unorm,   WGPUTextureFormat_RGBA8UnormSrgb},
        {WGPUTextureFormat_BGRA8Unorm,   WGPUTextureFormat_BGRA8UnormSrgb},
        {WGPUTextureFormat_BC1RGBAUnorm, WGPUTextureFormat_BC1RGBAUnormSrgb},
        {WGPUTextureFormat_BC2RGBAUnorm, WGPUTextureFormat_BC2RGBAUnormSrgb},
        {WGPUTextureFormat_BC3RGBAUnorm, WGPUTextureFormat_BC3RGBAUnormSrgb},
        {WGPUTextureFormat_BC7RGBAUnorm, WGPUTextureFormat_BC7RGBAUnormSrgb},
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pairs); ++i)
    {
        if (pairs[i][0] == format)
            return pairs[i][1];
        if (pairs[i][1] == format)
            return pairs[i][0];
    }
    return WGPUTextureFormat_Undefined;
}

/* What WebGPU does with a format: its capability table, plus the features
 * that extend it. */
static unsigned int wgpu_format_caps(const struct wined3d_wgpu_info *wgpu_info,
        WGPUTextureFormat format, uint32_t *multisample_types)
{
    enum
    {
        R = WINED3D_FORMAT_CAP_RENDERTARGET,
        B = WINED3D_FORMAT_CAP_POSTPIXELSHADER_BLENDING,
        F = WINED3D_FORMAT_CAP_FILTERING,
        D = WINED3D_FORMAT_CAP_DEPTH_STENCIL,
        U = WINED3D_FORMAT_CAP_UNORDERED_ACCESS,
    };
    unsigned int caps = WINED3D_FORMAT_CAP_TEXTURE | WINED3D_FORMAT_CAP_VTF | WINED3D_FORMAT_CAP_BLIT;
    bool ms = false;

    switch (format)
    {
        case WGPUTextureFormat_R8Unorm:
        case WGPUTextureFormat_RG8Unorm:
        case WGPUTextureFormat_RGBA8UnormSrgb:
        case WGPUTextureFormat_BGRA8UnormSrgb:
        case WGPUTextureFormat_RGB10A2Unorm:
            caps |= R | B | F;
            ms = true;
            break;
        case WGPUTextureFormat_RGBA8Unorm:
        case WGPUTextureFormat_R16Float:
        case WGPUTextureFormat_RG16Float:
        case WGPUTextureFormat_RGBA16Float:
            caps |= R | B | F | U;
            ms = true;
            break;
        case WGPUTextureFormat_BGRA8Unorm:
            caps |= R | B | F;
            ms = true;
            break;
        case WGPUTextureFormat_R32Float:
            caps |= R | U;
            if (wgpu_info->float32_filterable)
                caps |= F;
            if (wgpu_info->float32_blendable)
                caps |= B;
            ms = true;
            break;
        case WGPUTextureFormat_RG32Float:
        case WGPUTextureFormat_RGBA32Float:
            caps |= R | U;
            if (wgpu_info->float32_filterable)
                caps |= F;
            if (wgpu_info->float32_blendable)
                caps |= B;
            break;
        case WGPUTextureFormat_R8Uint:
        case WGPUTextureFormat_R8Sint:
        case WGPUTextureFormat_RG8Uint:
        case WGPUTextureFormat_RG8Sint:
        case WGPUTextureFormat_R16Uint:
        case WGPUTextureFormat_R16Sint:
        case WGPUTextureFormat_RG16Uint:
        case WGPUTextureFormat_RG16Sint:
        case WGPUTextureFormat_RGB10A2Uint:
            caps |= R;
            ms = true;
            break;
        case WGPUTextureFormat_RGBA8Uint:
        case WGPUTextureFormat_RGBA8Sint:
        case WGPUTextureFormat_RGBA16Uint:
        case WGPUTextureFormat_RGBA16Sint:
            caps |= R | U;
            ms = true;
            break;
        case WGPUTextureFormat_R32Uint:
        case WGPUTextureFormat_R32Sint:
        case WGPUTextureFormat_RG32Uint:
        case WGPUTextureFormat_RG32Sint:
        case WGPUTextureFormat_RGBA32Uint:
        case WGPUTextureFormat_RGBA32Sint:
            caps |= R | U;
            break;
        case WGPUTextureFormat_RGBA8Snorm:
            caps |= F | U;
            if (wgpu_info->texture_formats_tier1)
                caps |= R | B;
            break;
        case WGPUTextureFormat_R8Snorm:
        case WGPUTextureFormat_RG8Snorm:
            caps |= F;
            if (wgpu_info->texture_formats_tier1)
                caps |= R | B;
            break;
        case WGPUTextureFormat_R16Unorm:
        case WGPUTextureFormat_RG16Unorm:
        case WGPUTextureFormat_RGBA16Unorm:
        case WGPUTextureFormat_R16Snorm:
        case WGPUTextureFormat_RG16Snorm:
        case WGPUTextureFormat_RGBA16Snorm:
            /* Renderable with tier 1, never filterable. */
            if (!wgpu_info->texture_formats_tier1)
                return 0;
            caps |= R | B;
            ms = true;
            break;
        case WGPUTextureFormat_RG11B10Ufloat:
            caps |= F;
            if (wgpu_info->rg11b10_renderable)
            {
                caps |= R | B;
                ms = true;
            }
            break;
        case WGPUTextureFormat_RGB9E5Ufloat:
            caps |= F;
            break;
        case WGPUTextureFormat_Depth16Unorm:
        case WGPUTextureFormat_Depth32Float:
            /* Sampling a depth texture does not filter. */
            caps |= D;
            ms = true;
            break;
        case WGPUTextureFormat_Depth24Plus:
        case WGPUTextureFormat_Depth24PlusStencil8:
            /* Not copyable, so neither uploaded nor read back. */
            caps = WINED3D_FORMAT_CAP_TEXTURE | D;
            ms = true;
            break;
        case WGPUTextureFormat_Depth32FloatStencil8:
            if (!wgpu_info->depth32float_stencil8)
                return 0;
            caps = WINED3D_FORMAT_CAP_TEXTURE | D;
            ms = true;
            break;
        case WGPUTextureFormat_BC1RGBAUnorm:
        case WGPUTextureFormat_BC1RGBAUnormSrgb:
        case WGPUTextureFormat_BC2RGBAUnorm:
        case WGPUTextureFormat_BC2RGBAUnormSrgb:
        case WGPUTextureFormat_BC3RGBAUnorm:
        case WGPUTextureFormat_BC3RGBAUnormSrgb:
        case WGPUTextureFormat_BC4RUnorm:
        case WGPUTextureFormat_BC4RSnorm:
        case WGPUTextureFormat_BC5RGUnorm:
        case WGPUTextureFormat_BC5RGSnorm:
        case WGPUTextureFormat_BC6HRGBUfloat:
        case WGPUTextureFormat_BC6HRGBFloat:
        case WGPUTextureFormat_BC7RGBAUnorm:
        case WGPUTextureFormat_BC7RGBAUnormSrgb:
            if (!wgpu_info->texture_compression_bc)
                return 0;
            caps |= F;
            break;
        default:
            return 0;
    }

    if (!(~caps & (R | F)))
        caps |= WINED3D_FORMAT_CAP_GEN_MIPMAP;
    *multisample_types = ms ? 1u << (4 - 1) : 0;
    return caps;
}

static struct wined3d_format_wgpu *wined3d_format_wgpu_mutable(struct wined3d_format *format)
{
    return CONTAINING_RECORD(format, struct wined3d_format_wgpu, f);
}

static void init_wgpu_format_info(struct wined3d_adapter *adapter, struct wined3d_format_wgpu *format,
        const struct wined3d_wgpu_info *wgpu_info)
{
    const char *fixup = NULL;
    uint32_t multisample_types = 0;
    unsigned int caps, i;

    for (i = 0; i < ARRAY_SIZE(wgpu_converted_formats); ++i)
    {
        if (wgpu_converted_formats[i].id != format->f.id)
            continue;
        format->texture_format = WGPUTextureFormat_BGRA8Unorm;
        format->convert_upload = wgpu_converted_formats[i].upload;
        format->convert_download = wgpu_converted_formats[i].download;
        fixup = wgpu_converted_formats[i].fixup;
        break;
    }
    if (i == ARRAY_SIZE(wgpu_converted_formats))
    {
        for (i = 0; i < ARRAY_SIZE(wgpu_formats); ++i)
        {
            if (wgpu_formats[i].id == format->f.id)
                break;
        }
        if (i == ARRAY_SIZE(wgpu_formats))
        {
            WARN("Unsupported format %s.\n", debug_d3dformat(format->f.id));
            return;
        }

        format->texture_format = wgpu_formats[i].texture_format;
        format->vertex_format = wgpu_formats[i].vertex_format;
        fixup = wgpu_formats[i].fixup;
    }
    format->srgb_toggle_format = format->convert_upload ? WGPUTextureFormat_Undefined
            : wgpu_srgb_toggle_format(format->texture_format);
    if (fixup && !wgpu_info->texture_component_swizzle)
        WARN("No texture swizzle for format %s.\n", debug_d3dformat(format->f.id));
    format->f.color_fixup = fixup ? wined3d_color_fixup_from_string(fixup) : COLOR_FIXUP_IDENTITY;

    if (format->vertex_format != WGPU_VERTEX_FORMAT_NONE)
        format->f.caps[WINED3D_GL_RES_TYPE_BUFFER] |= WINED3D_FORMAT_CAP_VERTEX_ATTRIBUTE;

    if (format->texture_format == WGPUTextureFormat_Undefined)
        return;
    if (!(caps = wgpu_format_caps(wgpu_info, format->texture_format, &multisample_types)))
    {
        TRACE("Format %s is not supported.\n", debug_d3dformat(format->f.id));
        format->texture_format = WGPUTextureFormat_Undefined;
        return;
    }

    TRACE("Caps %#08x are supported for format %s.\n", caps, debug_d3dformat(format->f.id));

    format->f.caps[WINED3D_GL_RES_TYPE_TEX_1D] |= caps;
    format->f.caps[WINED3D_GL_RES_TYPE_TEX_2D] |= caps;
    if (!(format->f.attrs & WINED3D_FORMAT_ATTR_COMPRESSED) || wgpu_info->texture_compression_bc_3d)
        format->f.caps[WINED3D_GL_RES_TYPE_TEX_3D] |= caps & ~WINED3D_FORMAT_CAP_DEPTH_STENCIL;
    format->f.caps[WINED3D_GL_RES_TYPE_TEX_CUBE] |= caps;
    format->f.multisample_types |= multisample_types;
}

BOOL wined3d_adapter_wgpu_init_format_info(struct wined3d_adapter_wgpu *adapter_wgpu)
{
    struct wined3d_adapter *adapter = &adapter_wgpu->a;
    struct wined3d_format_wgpu *format;
    unsigned int i;

    if (!wined3d_adapter_init_formats(adapter, sizeof(*format)))
        return FALSE;

    for (i = 0; i < wined3d_format_count(); ++i)
    {
        format = wined3d_format_wgpu_mutable(wined3d_adapter_get_format_by_idx(adapter, i));

        if (format->f.id)
            init_wgpu_format_info(adapter, format, &adapter_wgpu->wgpu_info);
    }

    if (!wined3d_adapter_init_typeless_formats(adapter))
    {
        free(adapter->formats);
        adapter->formats = NULL;
        return FALSE;
    }

    return TRUE;
}

/* Helpers shared by the backend's files. */

WGPUCompareFunction wgpu_compare_function_from_wined3d(enum wined3d_cmp_func op)
{
    switch (op)
    {
        case WINED3D_CMP_NEVER:
            return WGPUCompareFunction_Never;
        case WINED3D_CMP_LESS:
            return WGPUCompareFunction_Less;
        case WINED3D_CMP_EQUAL:
            return WGPUCompareFunction_Equal;
        case WINED3D_CMP_LESSEQUAL:
            return WGPUCompareFunction_LessEqual;
        case WINED3D_CMP_GREATER:
            return WGPUCompareFunction_Greater;
        case WINED3D_CMP_NOTEQUAL:
            return WGPUCompareFunction_NotEqual;
        case WINED3D_CMP_GREATEREQUAL:
            return WGPUCompareFunction_GreaterEqual;
        case WINED3D_CMP_ALWAYS:
            return WGPUCompareFunction_Always;
        default:
            if (!op)
                WARN("Unhandled compare function %#x.\n", op);
            else
                FIXME("Unhandled compare function %#x.\n", op);
            return WGPUCompareFunction_Never;
    }
}

WGPUTextureViewDimension wgpu_view_dimension_from_wined3d(enum wined3d_resource_type type,
        uint32_t flags, unsigned int layer_count, bool cube)
{
    switch (type)
    {
        case WINED3D_RTYPE_TEXTURE_1D:
            /* 1D textures are made 2D. */
            return flags & WINED3D_VIEW_TEXTURE_ARRAY ? WGPUTextureViewDimension_2DArray
                    : WGPUTextureViewDimension_2D;
        case WINED3D_RTYPE_TEXTURE_2D:
            if (flags & WINED3D_VIEW_TEXTURE_CUBE)
                return layer_count > 6 ? WGPUTextureViewDimension_CubeArray : WGPUTextureViewDimension_Cube;
            if (cube)
                return WGPUTextureViewDimension_Cube;
            if (flags & WINED3D_VIEW_TEXTURE_ARRAY)
                return WGPUTextureViewDimension_2DArray;
            return WGPUTextureViewDimension_2D;
        case WINED3D_RTYPE_TEXTURE_3D:
            return WGPUTextureViewDimension_3D;
        default:
            ERR("Unhandled resource type %s.\n", debug_d3dresourcetype(type));
            return WGPUTextureViewDimension_2D;
    }
}

void wined3d_format_colour_to_wgpu(const struct wined3d_format *format, const struct wined3d_color *c,
        WGPUColor *retval)
{
    retval->r = c->r;
    retval->g = c->g;
    retval->b = c->b;
    retval->a = c->a;

    /* An integer target is cleared to the colour's bits, as Vulkan's
     * clear colour union reinterprets them. */
    if (format->attrs & WINED3D_FORMAT_ATTR_INTEGER)
    {
        union
        {
            float f;
            int32_t i;
            uint32_t u;
        } r = {c->r}, g = {c->g}, b = {c->b}, a = {c->a};

        if (format->attrs & WINED3D_FORMAT_ATTR_UNSIGNED)
        {
            retval->r = r.u;
            retval->g = g.u;
            retval->b = b.u;
            retval->a = a.u;
        }
        else
        {
            retval->r = r.i;
            retval->g = g.i;
            retval->b = b.i;
            retval->a = a.i;
        }
    }
}
