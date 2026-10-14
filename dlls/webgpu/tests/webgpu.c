/*
 * webgpu.dll: webgpu.h through the gex WebGPU wire.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "webgpu.h"
#include "wine/test.h"

static WGPUInstance instance;
static WGPUAdapter adapter;
static WGPUDevice device;
static WGPUQueue queue;

static WGPUStringView string_view(const char *s)
{
    WGPUStringView view = {s, strlen(s)};

    return view;
}

static void wait(WGPUFuture future)
{
    WGPUFutureWaitInfo info = {future, FALSE};
    WGPUWaitStatus status;

    status = wgpuInstanceWaitAny(instance, 1, &info, UINT64_MAX);
    ok(status == WGPUWaitStatus_Success, "Got wait status %#x.\n", status);
    ok(info.completed, "Future not completed.\n");
}

static void adapter_callback(WGPURequestAdapterStatus status, WGPUAdapter result,
        WGPUStringView message, void *userdata1, void *userdata2)
{
    ok(status == WGPURequestAdapterStatus_Success, "Got status %#x: %.*s.\n", status,
            (int)message.length, message.data);
    ok(userdata1 == (void *)0x1234, "Got userdata %p.\n", userdata1);
    adapter = result;
}

static void device_callback(WGPURequestDeviceStatus status, WGPUDevice result,
        WGPUStringView message, void *userdata1, void *userdata2)
{
    ok(status == WGPURequestDeviceStatus_Success, "Got status %#x.\n", status);
    device = result;
}

static unsigned int uncaptured_errors;

static void uncaptured_error_callback(WGPUDevice const *d, WGPUErrorType type,
        WGPUStringView message, void *userdata1, void *userdata2)
{
    trace("Uncaptured error %#x: %.*s\n", type, (int)message.length, message.data);
    ++uncaptured_errors;
}

static void init(void)
{
    static const WGPUInstanceFeatureName features[] = {WGPUInstanceFeatureName_TimedWaitAny};
    WGPURequestAdapterCallbackInfo adapter_info = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    WGPURequestDeviceCallbackInfo device_info = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    WGPUInstanceDescriptor instance_desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    WGPUDeviceDescriptor device_desc = WGPU_DEVICE_DESCRIPTOR_INIT;
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    WGPULimits limits = WGPU_LIMITS_INIT;
    WGPUStatus status;

    instance_desc.requiredFeatureCount = 1;
    instance_desc.requiredFeatures = features;
    instance = wgpuCreateInstance(&instance_desc);
    ok(!!instance, "Failed to create an instance.\n");

    adapter_info.mode = WGPUCallbackMode_WaitAnyOnly;
    adapter_info.callback = adapter_callback;
    adapter_info.userdata1 = (void *)0x1234;
    wait(wgpuInstanceRequestAdapter(instance, NULL, adapter_info));
    ok(!!adapter, "No adapter.\n");

    status = wgpuAdapterGetInfo(adapter, &info);
    ok(status == WGPUStatus_Success, "Got status %#x.\n", status);
    trace("Adapter: %.*s (%.*s)\n", (int)info.device.length, info.device.data,
            (int)info.description.length, info.description.data);
    ok(info.backendType != WGPUBackendType_Undefined, "Got backend %#x.\n", info.backendType);
    wgpuAdapterInfoFreeMembers(info);

    status = wgpuAdapterGetLimits(adapter, &limits);
    ok(status == WGPUStatus_Success, "Got status %#x.\n", status);
    ok(limits.maxTextureDimension2D >= 8192, "Got max 2D texture dimension %u.\n", limits.maxTextureDimension2D);

    device_desc.uncapturedErrorCallbackInfo.callback = uncaptured_error_callback;
    device_info.mode = WGPUCallbackMode_AllowProcessEvents;
    device_info.callback = device_callback;
    wait(wgpuAdapterRequestDevice(adapter, &device_desc, device_info));
    ok(!!device, "No device.\n");
    queue = wgpuDeviceGetQueue(device);
}

static WGPUMapAsyncStatus map_status;

static void map_callback(WGPUMapAsyncStatus status, WGPUStringView message, void *userdata1, void *userdata2)
{
    map_status = status;
}

static const void *map_read(WGPUBuffer buffer, size_t size)
{
    WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;

    info.mode = WGPUCallbackMode_WaitAnyOnly;
    info.callback = map_callback;
    map_status = 0;
    wait(wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, size, info));
    ok(map_status == WGPUMapAsyncStatus_Success, "Got map status %#x.\n", map_status);
    ok(wgpuBufferGetMapState(buffer) == WGPUBufferMapState_Mapped, "Not mapped.\n");
    return wgpuBufferGetConstMappedRange(buffer, 0, size);
}

static void test_compute(void)
{
    static const char shader[] =
            "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
            "@compute @workgroup_size(64)\n"
            "fn main(@builtin(global_invocation_id) id: vec3<u32>) { data[id.x] = data[id.x] * 2u + 1u; }\n";
    WGPUComputePipelineDescriptor pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
    WGPUBuffer storage, readback;
    WGPUComputePipeline pipeline;
    WGPUComputePassEncoder pass;
    WGPUCommandEncoder encoder;
    WGPUCommandBuffer commands;
    WGPUShaderModule module;
    WGPUBindGroup group;
    uint32_t data[256];
    const uint32_t *result;
    unsigned int i, bad = 0;

    for (i = 0; i < ARRAY_SIZE(data); ++i)
        data[i] = i;

    wgsl.code = string_view(shader);
    module_desc.nextInChain = &wgsl.chain;
    module = wgpuDeviceCreateShaderModule(device, &module_desc);

    pipeline_desc.compute.module = module;
    pipeline_desc.compute.entryPoint = string_view("main");
    pipeline = wgpuDeviceCreateComputePipeline(device, &pipeline_desc);

    buffer_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
    buffer_desc.size = sizeof(data);
    storage = wgpuDeviceCreateBuffer(device, &buffer_desc);
    wgpuQueueWriteBuffer(queue, storage, 0, data, sizeof(data));
    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    readback = wgpuDeviceCreateBuffer(device, &buffer_desc);

    entry.binding = 0;
    entry.buffer = storage;
    entry.size = sizeof(data);
    group_desc.layout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
    group_desc.entryCount = 1;
    group_desc.entries = &entry;
    group = wgpuDeviceCreateBindGroup(device, &group_desc);

    encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);
    wgpuComputePassEncoderSetPipeline(pass, pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(pass, ARRAY_SIZE(data) / 64, 1, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, storage, 0, readback, 0, sizeof(data));
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(queue, 1, &commands);

    result = map_read(readback, sizeof(data));
    ok(!!result, "No mapped range.\n");
    for (i = 0; result && i < ARRAY_SIZE(data); ++i)
    {
        if (result[i] != i * 2 + 1)
            ++bad;
    }
    ok(!bad, "%u values wrong; e.g. %u for 1.\n", bad, result ? result[1] : 0);
    wgpuBufferUnmap(readback);

    wgpuCommandBufferRelease(commands);
    wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupRelease(group);
    wgpuBindGroupLayoutRelease(group_desc.layout);
    wgpuBufferRelease(readback);
    wgpuBufferRelease(storage);
    wgpuComputePipelineRelease(pipeline);
    wgpuShaderModuleRelease(module);
}

static void test_mapped_at_creation(void)
{
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    WGPUCommandEncoder encoder;
    WGPUCommandBuffer commands;
    WGPUBuffer upload, readback;
    const uint32_t *result;
    uint32_t *p;

    buffer_desc.usage = WGPUBufferUsage_CopySrc;
    buffer_desc.size = 16;
    buffer_desc.mappedAtCreation = TRUE;
    upload = wgpuDeviceCreateBuffer(device, &buffer_desc);
    ok(wgpuBufferGetMapState(upload) == WGPUBufferMapState_Mapped, "Not mapped at creation.\n");
    ok(wgpuBufferGetSize(upload) == 16, "Got size %I64u.\n", wgpuBufferGetSize(upload));
    p = wgpuBufferGetMappedRange(upload, 0, 16);
    ok(!!p, "No mapped range.\n");
    p[0] = 0xdeadbeef;
    p[3] = 0xcafef00d;
    wgpuBufferUnmap(upload);

    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    buffer_desc.mappedAtCreation = FALSE;
    readback = wgpuDeviceCreateBuffer(device, &buffer_desc);
    encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, upload, 0, readback, 0, 16);
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(queue, 1, &commands);

    result = map_read(readback, 16);
    ok(result && result[0] == 0xdeadbeef && result[3] == 0xcafef00d, "Got %#x, %#x.\n",
            result ? result[0] : 0, result ? result[3] : 0);
    wgpuBufferUnmap(readback);

    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    wgpuBufferRelease(readback);
    wgpuBufferRelease(upload);
}

static void test_texture(void)
{
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    WGPUTextureDescriptor texture_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    WGPUExtent3D size = {4, 4, 1};
    WGPUCommandEncoder encoder;
    WGPUCommandBuffer commands;
    const uint32_t *result;
    WGPUTexture texture;
    WGPUBuffer readback;
    uint32_t pixels[16];
    unsigned int i;

    for (i = 0; i < 16; ++i)
        pixels[i] = 0xff000000 | i * 0x010203;

    texture_desc.usage = WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    texture_desc.size = size;
    texture_desc.format = WGPUTextureFormat_RGBA8Unorm;
    texture = wgpuDeviceCreateTexture(device, &texture_desc);
    ok(wgpuTextureGetWidth(texture) == 4, "Got width %u.\n", wgpuTextureGetWidth(texture));
    ok(wgpuTextureGetFormat(texture) == WGPUTextureFormat_RGBA8Unorm, "Got format %#x.\n",
            wgpuTextureGetFormat(texture));

    src.texture = texture;
    layout.bytesPerRow = 16;
    layout.rowsPerImage = 4;
    wgpuQueueWriteTexture(queue, &src, pixels, sizeof(pixels), &layout, &size);

    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    buffer_desc.size = 256 * 4;
    readback = wgpuDeviceCreateBuffer(device, &buffer_desc);
    dst.buffer = readback;
    dst.layout.bytesPerRow = 256;
    dst.layout.rowsPerImage = 4;
    encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &size);
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(queue, 1, &commands);

    result = map_read(readback, 256 * 4);
    ok(result && result[64 * 3 + 2] == pixels[3 * 4 + 2], "Got %#x, expected %#x.\n",
            result ? result[64 * 3 + 2] : 0, pixels[3 * 4 + 2]);
    wgpuBufferUnmap(readback);

    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    wgpuBufferRelease(readback);
    wgpuTextureRelease(texture);
}

static WGPUErrorType scope_error;

static void pop_error_scope_callback(WGPUPopErrorScopeStatus status, WGPUErrorType type,
        WGPUStringView message, void *userdata1, void *userdata2)
{
    ok(status == WGPUPopErrorScopeStatus_Success, "Got status %#x.\n", status);
    scope_error = type;
}

static void test_error_scope(void)
{
    WGPUPopErrorScopeCallbackInfo info = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    WGPUBuffer buffer;

    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    /* Map-read buffers may only also be copy destinations. */
    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_Storage;
    buffer_desc.size = 16;
    buffer = wgpuDeviceCreateBuffer(device, &buffer_desc);
    info.mode = WGPUCallbackMode_WaitAnyOnly;
    info.callback = pop_error_scope_callback;
    scope_error = WGPUErrorType_NoError;
    wait(wgpuDevicePopErrorScope(device, info));
    ok(scope_error == WGPUErrorType_Validation, "Got error type %#x.\n", scope_error);
    wgpuBufferRelease(buffer);
    ok(!uncaptured_errors, "Got %u uncaptured errors.\n", uncaptured_errors);
}

START_TEST(webgpu)
{
    init();
    if (!device)
        return;
    test_compute();
    test_mapped_at_creation();
    test_texture();
    test_error_scope();
    wgpuQueueRelease(queue);
    wgpuDeviceRelease(device);
    wgpuAdapterRelease(adapter);
    wgpuInstanceRelease(instance);
}
