/*
 * WGSL (WebGPU Shading Language) target.
 *
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

/* The shape of the generated module, and why:
 *
 * - WGSL has no unions, so every register (temporaries, inputs, outputs,
 *   indexable temporaries) is a vec4<u32> and a read is a bitcast<> to the
 *   type the instruction wants.
 *
 * - WGSL cannot assign to a multi-component swizzle, so a masked write is
 *   one assignment per written component, through a let of the value.
 *
 * - WGSL rejects a non-finite value in a const-expression (and an
 *   operation whose const-evaluated result overflows), so a float immediate
 *   that is not finite is produced at run time, from a private variable
 *   holding zero.
 *
 * - Registers are module-scope private variables and the shader body is a
 *   function of no arguments; the entry point copies the stage's inputs in
 *   and its outputs out, as the MSL target does through its arguments.
 *
 * - Derivatives and implicit-LOD sampling in non-uniform control flow are
 *   valid D3D; the module turns WGSL's uniformity diagnostic for them off.
 *
 * - WGSL's switch has no fallthrough. Consecutive case labels with no code
 *   between them share a clause; code that falls through into the next case
 *   is not supported. Every WGSL switch needs a default clause, so one is
 *   added when the shader has none. */

#include "vkd3d_shader_private.h"

#define MAX_IO_REG_COUNT 32

struct wgsl_src
{
    struct vkd3d_string_buffer *str;
};

struct wgsl_dst
{
    const struct vsir_dst_operand *vsir;
    struct vkd3d_string_buffer *register_name;
    /* The register is a vec4<u32> that the write mask selects from, rather
     * than a typed scalar such as frag_depth. */
    bool vec4_u32;
    enum vsir_data_type scalar_type;
};

struct wgsl_switch
{
    /* The clause header ("case 1u, 2u") still to be printed, when labels
     * have been seen and no code yet. */
    struct vkd3d_string_buffer *pending;
    bool in_clause;
    bool has_default;
    enum vsir_data_type selector_type;
};

struct wgsl_generator
{
    struct vsir_program *program;
    struct vkd3d_string_buffer_cache string_buffers;
    struct vkd3d_string_buffer *buffer;
    struct vkd3d_shader_location location;
    struct vkd3d_shader_message_context *message_context;
    unsigned int indent;
    const char *prefix;
    bool failed;

    bool read_vertex_id;
    bool read_instance_id;

    struct wgsl_switch *switches;
    size_t switch_count, switch_capacity;

    const struct vkd3d_shader_interface_info *interface_info;
    struct vkd3d_shader_varying_interpolation_info *interpolation_info;
    const struct vkd3d_shader_wgsl_target_info *target_info;
    struct vkd3d_shader_wgsl_binding_info *binding_info;
    size_t binding_capacity;
};

struct wgsl_resource_type_info
{
    /* The number of coordinates needed to address/sample the resource type. */
    unsigned int coord_size;
    /* Whether the resource type is an array type. */
    bool array;
    /* Whether the resource type supports texel sample offsets. */
    bool offset;
    /* The WGSL texture type, without "texture_" or the sample type. */
    const char *type_suffix;
    /* The WGSL depth texture type suffix, or NULL. */
    const char *depth_suffix;
};

static void wgsl_print_subscript(struct vkd3d_string_buffer *buffer, struct wgsl_generator *gen,
        const struct vsir_src_operand *rel_addr, unsigned int offset);

#define wgsl_compiler_error(gen, error, ...) \
        wgsl_compiler_error_(gen, error, __FUNCTION__, __VA_ARGS__)
static void VKD3D_PRINTF_FUNC(4, 5) wgsl_compiler_error_(struct wgsl_generator *gen,
        enum vkd3d_shader_error error, const char *function, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vkd3d_shader_verror(gen->message_context, &gen->location, error, function, fmt, args);
    va_end(args);
    gen->failed = true;
}

static const struct wgsl_resource_type_info *wgsl_get_resource_type_info(enum vkd3d_shader_resource_type t)
{
    static const struct wgsl_resource_type_info info[] =
    {
        [VKD3D_SHADER_RESOURCE_NONE]              = {0, 0, 0, "none", NULL},
        [VKD3D_SHADER_RESOURCE_BUFFER]            = {1, 0, 0, "buffer", NULL},
        [VKD3D_SHADER_RESOURCE_TEXTURE_1D]        = {1, 0, 0, "1d", NULL},
        [VKD3D_SHADER_RESOURCE_TEXTURE_2D]        = {2, 0, 1, "2d", "2d"},
        [VKD3D_SHADER_RESOURCE_TEXTURE_2DMS]      = {2, 0, 0, "multisampled_2d", "multisampled_2d"},
        [VKD3D_SHADER_RESOURCE_TEXTURE_3D]        = {3, 0, 1, "3d", NULL},
        [VKD3D_SHADER_RESOURCE_TEXTURE_CUBE]      = {3, 0, 0, "cube", "cube"},
        [VKD3D_SHADER_RESOURCE_TEXTURE_1DARRAY]   = {1, 1, 0, "1d", NULL},
        [VKD3D_SHADER_RESOURCE_TEXTURE_2DARRAY]   = {2, 1, 1, "2d_array", "2d_array"},
        [VKD3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY] = {2, 1, 0, "multisampled_2d_array", NULL},
        [VKD3D_SHADER_RESOURCE_TEXTURE_CUBEARRAY] = {3, 1, 0, "cube_array", "cube_array"},
    };

    if (!t || t >= ARRAY_SIZE(info))
        return NULL;

    return &info[t];
}

static const char *wgsl_get_prefix(enum vkd3d_shader_type type)
{
    switch (type)
    {
        case VKD3D_SHADER_TYPE_VERTEX:
            return "vs";
        case VKD3D_SHADER_TYPE_PIXEL:
            return "ps";
        case VKD3D_SHADER_TYPE_COMPUTE:
            return "cs";
        default:
            return NULL;
    }
}

static void wgsl_print_indent(struct vkd3d_string_buffer *buffer, unsigned int indent)
{
    vkd3d_string_buffer_printf(buffer, "%*s", 4 * indent, "");
}

static enum vsir_data_type wgsl_scalar_type(enum vsir_data_type data_type)
{
    switch (data_type)
    {
        case VSIR_DATA_F32:
        case VSIR_DATA_SNORM:
        case VSIR_DATA_UNORM:
            return VSIR_DATA_F32;
        case VSIR_DATA_I32:
            return VSIR_DATA_I32;
        case VSIR_DATA_BOOL:
        case VSIR_DATA_U32:
            return VSIR_DATA_U32;
        default:
            return VSIR_DATA_UNUSED;
    }
}

static const char *wgsl_scalar_name(struct wgsl_generator *gen, enum vsir_data_type data_type)
{
    switch (wgsl_scalar_type(data_type))
    {
        case VSIR_DATA_F32:
            return "f32";
        case VSIR_DATA_I32:
            return "i32";
        case VSIR_DATA_U32:
            return "u32";
        default:
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                    "Unsupported data type \"%s\" (%#x).", vsir_data_type_get_name(data_type, "<unknown>"), data_type);
            return "<unsupported data type>";
    }
}

/* "f32" or "vec3<f32>". */
static void wgsl_print_type(struct vkd3d_string_buffer *buffer, struct wgsl_generator *gen,
        enum vsir_data_type data_type, unsigned int component_count)
{
    if (component_count == 1)
        vkd3d_string_buffer_printf(buffer, "%s", wgsl_scalar_name(gen, data_type));
    else
        vkd3d_string_buffer_printf(buffer, "vec%u<%s>", component_count, wgsl_scalar_name(gen, data_type));
}

static bool wgsl_check_shader_visibility(const struct wgsl_generator *gen,
        enum vkd3d_shader_visibility visibility)
{
    enum vkd3d_shader_type t = gen->program->shader_version.type;

    switch (visibility)
    {
        case VKD3D_SHADER_VISIBILITY_ALL:
            return true;
        case VKD3D_SHADER_VISIBILITY_VERTEX:
            return t == VKD3D_SHADER_TYPE_VERTEX;
        case VKD3D_SHADER_VISIBILITY_PIXEL:
            return t == VKD3D_SHADER_TYPE_PIXEL;
        case VKD3D_SHADER_VISIBILITY_COMPUTE:
            return t == VKD3D_SHADER_TYPE_COMPUTE;
        default:
            return false;
    }
}

static bool wgsl_get_binding(const struct wgsl_generator *gen, const struct vsir_descriptor *descriptor,
        unsigned int register_idx, enum vkd3d_shader_binding_flag flags, unsigned int *group, unsigned int *idx)
{
    const struct vkd3d_shader_interface_info *interface_info = gen->interface_info;
    unsigned int i;

    /* With no interface given, D3D11's model: a bind group per descriptor
     * type (WebGPU guarantees four), the register index as the binding. */
    if (!interface_info)
    {
        if (descriptor->register_space)
            return false;
        switch (descriptor->type)
        {
            case VKD3D_SHADER_DESCRIPTOR_TYPE_CBV:
                *group = 0;
                break;
            case VKD3D_SHADER_DESCRIPTOR_TYPE_SRV:
                *group = 1;
                break;
            case VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER:
                *group = 2;
                break;
            case VKD3D_SHADER_DESCRIPTOR_TYPE_UAV:
                *group = 3;
                break;
            default:
                return false;
        }
        *idx = register_idx;
        return true;
    }

    for (i = 0; i < interface_info->binding_count; ++i)
    {
        const struct vkd3d_shader_resource_binding *binding = &interface_info->bindings[i];

        if (binding->type != descriptor->type)
            continue;
        if (binding->register_space != descriptor->register_space)
            continue;
        if (binding->register_index > register_idx)
            continue;
        if (binding->binding.count <= register_idx - binding->register_index)
            continue;
        if (!wgsl_check_shader_visibility(gen, binding->shader_visibility))
            continue;
        if ((binding->flags & flags) != flags)
            continue;

        *group = binding->binding.set;
        *idx = register_idx + binding->binding.binding - binding->register_index;
        return true;
    }

    return false;
}

static enum vkd3d_shader_binding_flag wgsl_descriptor_binding_flags(const struct vsir_descriptor *descriptor)
{
    switch (descriptor->type)
    {
        case VKD3D_SHADER_DESCRIPTOR_TYPE_CBV:
            return VKD3D_SHADER_BINDING_FLAG_BUFFER;
        case VKD3D_SHADER_DESCRIPTOR_TYPE_SRV:
        case VKD3D_SHADER_DESCRIPTOR_TYPE_UAV:
            return descriptor->resource_type == VKD3D_SHADER_RESOURCE_BUFFER
                    ? VKD3D_SHADER_BINDING_FLAG_BUFFER : VKD3D_SHADER_BINDING_FLAG_IMAGE;
        default:
            return 0;
    }
}

static const char *wgsl_descriptor_prefix(enum vkd3d_shader_descriptor_type type)
{
    switch (type)
    {
        case VKD3D_SHADER_DESCRIPTOR_TYPE_CBV:
            return "cb";
        case VKD3D_SHADER_DESCRIPTOR_TYPE_SRV:
            return "t";
        case VKD3D_SHADER_DESCRIPTOR_TYPE_UAV:
            return "u";
        case VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER:
            return "s";
        default:
            return "<unknown>";
    }
}

/* Descriptors are named by their register range and the index within it,
 * "t2_0", so that two ranges never collide whatever their bindings. */
static const struct vsir_descriptor *wgsl_find_descriptor(struct wgsl_generator *gen,
        enum vkd3d_shader_descriptor_type type, const struct vsir_src_operand *operand,
        unsigned int *id, unsigned int *idx)
{
    const struct vsir_descriptor *descriptor;

    if (operand->reg.idx[0].rel_addr || operand->reg.idx[1].rel_addr)
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                "Descriptor indexing is not supported.");

    *id = operand->reg.idx[0].offset;
    *idx = operand->reg.idx[1].offset;
    if (!(descriptor = vkd3d_shader_find_descriptor(&gen->program->descriptors, type, *id)))
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Undeclared %s descriptor %u.", wgsl_descriptor_prefix(type), *id);
    return descriptor;
}

static void wgsl_print_descriptor_name(struct vkd3d_string_buffer *buffer,
        enum vkd3d_shader_descriptor_type type, unsigned int id, unsigned int idx)
{
    vkd3d_string_buffer_printf(buffer, "%s%u_%u", wgsl_descriptor_prefix(type), id, idx);
}

static void wgsl_print_swizzle(struct vkd3d_string_buffer *buffer, uint32_t swizzle, uint32_t mask)
{
    const char swizzle_chars[] = "xyzw";
    unsigned int i;

    vkd3d_string_buffer_printf(buffer, ".");
    for (i = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        if (mask & (VKD3DSP_WRITEMASK_0 << i))
            vkd3d_string_buffer_printf(buffer, "%c", swizzle_chars[vsir_swizzle_get_component(swizzle, i)]);
    }
}

static void wgsl_print_write_mask(struct vkd3d_string_buffer *buffer, uint32_t write_mask)
{
    vkd3d_string_buffer_printf(buffer, ".");
    if (write_mask & VKD3DSP_WRITEMASK_0)
        vkd3d_string_buffer_printf(buffer, "x");
    if (write_mask & VKD3DSP_WRITEMASK_1)
        vkd3d_string_buffer_printf(buffer, "y");
    if (write_mask & VKD3DSP_WRITEMASK_2)
        vkd3d_string_buffer_printf(buffer, "z");
    if (write_mask & VKD3DSP_WRITEMASK_3)
        vkd3d_string_buffer_printf(buffer, "w");
}

/* One 32-bit immediate as a WGSL value of the given type. */
static void wgsl_print_immediate(struct vkd3d_string_buffer *buffer, struct wgsl_generator *gen,
        uint32_t value, enum vsir_data_type data_type)
{
    switch (wgsl_scalar_type(data_type))
    {
        case VSIR_DATA_F32:
            if ((value & 0x7f800000u) == 0x7f800000u)
                /* Inf and NaN may not be const-expressions. */
                vkd3d_string_buffer_printf(buffer, "bitcast<f32>(%#xu | vkd3d_zero)", value);
            else if (!(value & 0x7fffffffu))
                vkd3d_string_buffer_printf(buffer, "%sf", value ? "-0.0" : "0.0");
            else
            {
                union
                {
                    uint32_t u;
                    float f;
                } v = {.u = value};
                /* A hex float is exact, and WGSL takes C's "%a" form. */
                vkd3d_string_buffer_printf(buffer, "%af", v.f);
            }
            break;
        case VSIR_DATA_I32:
            vkd3d_string_buffer_printf(buffer, "bitcast<i32>(%#xu)", value);
            break;
        case VSIR_DATA_U32:
            vkd3d_string_buffer_printf(buffer, "%#xu", value);
            break;
        default:
            wgsl_scalar_name(gen, data_type);
            break;
    }
}

/* What a source register is, before any bitcast: a vec4<u32> (or one of
 * its components) to swizzle, or a typed scalar. */
enum wgsl_operand_kind
{
    WGSL_OPERAND_VEC4_U32,
    WGSL_OPERAND_SCALAR,
    WGSL_OPERAND_IMMEDIATE,
};

/* The rows of the I/O signature element "element". */
static unsigned int wgsl_io_element_rows(const struct wgsl_generator *gen,
        enum vsir_register_type type, unsigned int element)
{
    const struct vsir_signature *signature = type == VSIR_REGISTER_INPUT
            ? &gen->program->input_signature : &gen->program->output_signature;

    if (element >= signature->element_count)
        return 1;
    return max(signature->elements[element].register_count, 1);
}

static enum wgsl_operand_kind wgsl_print_operand_name(struct vkd3d_string_buffer *buffer,
        struct wgsl_generator *gen, const struct vsir_operand *reg, enum vsir_data_type *scalar_type)
{
    const struct vsir_descriptor *descriptor;
    unsigned int group, binding;

    *scalar_type = VSIR_DATA_U32;

    switch (reg->type)
    {
        case VSIR_REGISTER_TEMP:
            vkd3d_string_buffer_printf(buffer, "r[%u]", reg->idx[0].offset);
            return WGSL_OPERAND_VEC4_U32;

        case VSIR_REGISTER_INPUT:
        case VSIR_REGISTER_OUTPUT:
            /* Arrayed I/O is addressed (row, element). An element of
             * several rows (clip distances, say) is an array of its own. */
            if (reg->idx_count == 2 && !reg->idx[1].rel_addr)
            {
                const char *prefix = reg->type == VSIR_REGISTER_INPUT ? "v" : "o";

                if (wgsl_io_element_rows(gen, reg->type, reg->idx[1].offset) > 1)
                {
                    vkd3d_string_buffer_printf(buffer, "%s_e%u", prefix, reg->idx[1].offset);
                    wgsl_print_subscript(buffer, gen, reg->idx[0].rel_addr, reg->idx[0].offset);
                    return WGSL_OPERAND_VEC4_U32;
                }
                if (!reg->idx[0].rel_addr && !reg->idx[0].offset)
                {
                    vkd3d_string_buffer_printf(buffer, "%s[%u]", prefix, reg->idx[1].offset);
                    return WGSL_OPERAND_VEC4_U32;
                }
            }
            if (reg->idx_count != 1)
            {
                wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                        "Internal compiler error: Unhandled I/O register index count %u (%s, %u%s, %u%s).",
                        reg->idx_count, reg->type == VSIR_REGISTER_INPUT ? "input" : "output",
                        reg->idx[0].offset, reg->idx[0].rel_addr ? " relative" : "",
                        reg->idx_count > 1 ? reg->idx[1].offset : 0,
                        reg->idx_count > 1 && reg->idx[1].rel_addr ? " relative" : "");
                vkd3d_string_buffer_printf(buffer, "<unhandled register %#x>", reg->type);
                return WGSL_OPERAND_VEC4_U32;
            }
            vkd3d_string_buffer_printf(buffer, "%s", reg->type == VSIR_REGISTER_INPUT ? "v" : "o");
            wgsl_print_subscript(buffer, gen, reg->idx[0].rel_addr, reg->idx[0].offset);
            return WGSL_OPERAND_VEC4_U32;

        case VSIR_REGISTER_DEPTHOUT:
            vkd3d_string_buffer_printf(buffer, "o_depth");
            *scalar_type = VSIR_DATA_F32;
            return WGSL_OPERAND_SCALAR;

        case VSIR_REGISTER_SAMPLEMASK:
            vkd3d_string_buffer_printf(buffer, "o_mask");
            return WGSL_OPERAND_SCALAR;

        case VSIR_REGISTER_OUTSTENCILREF:
            vkd3d_string_buffer_printf(buffer, "o_stencil_ref");
            return WGSL_OPERAND_SCALAR;

        case VSIR_REGISTER_IMMCONST:
            return WGSL_OPERAND_IMMEDIATE;

        case VSIR_REGISTER_CONSTBUFFER:
            if (reg->idx_count != 3)
            {
                wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                        "Internal compiler error: Unhandled constant buffer register index count %u.",
                        reg->idx_count);
                vkd3d_string_buffer_printf(buffer, "<unhandled register %#x>", reg->type);
                return WGSL_OPERAND_VEC4_U32;
            }
            if (reg->idx[0].rel_addr || reg->idx[1].rel_addr)
                wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                        "Constant buffer descriptor indexing is not supported.");
            if (!(descriptor = vkd3d_shader_find_descriptor(&gen->program->descriptors,
                    VKD3D_SHADER_DESCRIPTOR_TYPE_CBV, reg->idx[0].offset)))
                wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                        "Internal compiler error: Undeclared CBV descriptor %u.", reg->idx[0].offset);
            else if (!wgsl_get_binding(gen, descriptor, reg->idx[1].offset,
                    VKD3D_SHADER_BINDING_FLAG_BUFFER, &group, &binding))
                wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_BINDING_NOT_FOUND,
                        "No descriptor binding specified for CBV %u.", reg->idx[0].offset);
            wgsl_print_descriptor_name(buffer, VKD3D_SHADER_DESCRIPTOR_TYPE_CBV,
                    reg->idx[0].offset, reg->idx[1].offset);
            vkd3d_string_buffer_printf(buffer, ".v");
            wgsl_print_subscript(buffer, gen, reg->idx[2].rel_addr, reg->idx[2].offset);
            return WGSL_OPERAND_VEC4_U32;

        case VSIR_REGISTER_IMMCONSTBUFFER:
            vkd3d_string_buffer_printf(buffer, "icb%u", reg->idx[0].offset);
            wgsl_print_subscript(buffer, gen, reg->idx[1].rel_addr, reg->idx[1].offset);
            return WGSL_OPERAND_VEC4_U32;

        case VSIR_REGISTER_IDXTEMP:
            vkd3d_string_buffer_printf(buffer, "x%u", reg->idx[0].offset);
            wgsl_print_subscript(buffer, gen, reg->idx[1].rel_addr, reg->idx[1].offset);
            return WGSL_OPERAND_VEC4_U32;

        case VSIR_REGISTER_THREADID:
            vkd3d_string_buffer_printf(buffer, "v_thread_id");
            return WGSL_OPERAND_VEC4_U32;

        case VSIR_REGISTER_THREADGROUPID:
            vkd3d_string_buffer_printf(buffer, "v_thread_group_id");
            return WGSL_OPERAND_VEC4_U32;

        case VSIR_REGISTER_LOCALTHREADID:
            vkd3d_string_buffer_printf(buffer, "v_local_thread_id");
            return WGSL_OPERAND_VEC4_U32;

        case VSIR_REGISTER_LOCALTHREADINDEX:
            vkd3d_string_buffer_printf(buffer, "v_local_thread_index");
            return WGSL_OPERAND_VEC4_U32;

        case VSIR_REGISTER_PARAMETER:
        {
            const struct vkd3d_shader_parameter1 *parameter;
            unsigned int word;

            if (!(parameter = vsir_program_get_parameter(gen->program, reg->idx[0].offset))
                    || parameter->type != VKD3D_SHADER_PARAMETER_TYPE_BUFFER)
                return WGSL_OPERAND_IMMEDIATE;
            /* A parameter in a buffer is read from it: a vector whole,
             * a scalar into x. */
            word = parameter->u.buffer.offset / 4;
            if (parameter->data_type == VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32_VEC4 && !(word % 4))
                vkd3d_string_buffer_printf(buffer, "vkd3d_param_%u_%u[%u]",
                        parameter->u.buffer.set, parameter->u.buffer.binding, word / 4);
            else
                vkd3d_string_buffer_printf(buffer, "vec4<u32>(vkd3d_param_%u_%u[%u].%c, 0u, 0u, 0u)",
                        parameter->u.buffer.set, parameter->u.buffer.binding, word / 4, "xyzw"[word % 4]);
            return WGSL_OPERAND_VEC4_U32;
        }

        case VSIR_REGISTER_UNDEF:
            return WGSL_OPERAND_IMMEDIATE;

        case VSIR_REGISTER_OUT_POINT_SIZE:
            /* WebGPU draws points one pixel wide; the size goes nowhere. */
            vkd3d_string_buffer_printf(buffer, "o_point_size");
            *scalar_type = VSIR_DATA_F32;
            return WGSL_OPERAND_SCALAR;

        default:
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                    "Internal compiler error: Unhandled register type %#x.", reg->type);
            vkd3d_string_buffer_printf(buffer, "<unrecognised register %#x>", reg->type);
            return WGSL_OPERAND_VEC4_U32;
    }
}

/* The four 32-bit words of an immediate operand: a constant, or a shader
 * parameter given as one. */
static void wgsl_get_immediate_words(struct wgsl_generator *gen, const struct vsir_operand *reg, uint32_t words[4])
{
    const struct vkd3d_shader_parameter1 *parameter;
    unsigned int i;

    memset(words, 0, 4 * sizeof(*words));
    if (reg->type == VSIR_REGISTER_IMMCONST)
    {
        for (i = 0; i < (reg->dimension == VSIR_DIMENSION_VEC4 ? 4 : 1); ++i)
            words[i] = reg->u.immconst_u32[i];
        return;
    }
    if (reg->type != VSIR_REGISTER_PARAMETER)
        return;

    /* A parameter register is indexed by the parameter's name. */
    if (!(parameter = vsir_program_get_parameter(gen->program, reg->idx[0].offset)))
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Undeclared shader parameter %#x.", reg->idx[0].offset);
        return;
    }
    if (parameter->type != VKD3D_SHADER_PARAMETER_TYPE_IMMEDIATE_CONSTANT)
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                "Shader parameter %#x of type %#x is not supported.", parameter->name, parameter->type);
        return;
    }
    switch (parameter->data_type)
    {
        case VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32_VEC4:
            memcpy(words, parameter->u.immediate_constant.u.f32_vec4, 4 * sizeof(*words));
            break;
        default:
            words[0] = parameter->u.immediate_constant.u.u32;
            break;
    }
}

/* A source operand, read as "data_type" with as many components as "mask"
 * has: f32, or vec3<f32>, and so on. */
static void wgsl_print_src_with_type(struct vkd3d_string_buffer *buffer, struct wgsl_generator *gen,
        const struct vsir_src_operand *vsir_src, uint32_t mask, enum vsir_data_type data_type)
{
    const struct vsir_operand *reg = &vsir_src->reg;
    unsigned int component_count, i, j;
    struct vkd3d_string_buffer *name;
    enum wgsl_operand_kind kind;
    enum vsir_data_type scalar_type;

    component_count = vsir_write_mask_component_count(mask);
    name = vkd3d_string_buffer_get(&gen->string_buffers);
    kind = wgsl_print_operand_name(name, gen, reg, &scalar_type);

    switch (kind)
    {
        case WGSL_OPERAND_IMMEDIATE:
        {
            uint32_t words[4];

            wgsl_get_immediate_words(gen, reg, words);
            if (component_count > 1)
            {
                wgsl_print_type(buffer, gen, data_type, component_count);
                vkd3d_string_buffer_printf(buffer, "(");
            }
            for (i = 0, j = 0; i < VKD3D_VEC4_SIZE; ++i)
            {
                uint32_t value;

                if (!(mask & (VKD3DSP_WRITEMASK_0 << i)))
                    continue;
                value = reg->dimension == VSIR_DIMENSION_VEC4
                        ? words[vsir_swizzle_get_component(vsir_src->swizzle, i)] : words[0];
                vkd3d_string_buffer_printf(buffer, "%s", j++ ? ", " : "");
                wgsl_print_immediate(buffer, gen, value, data_type);
            }
            if (component_count > 1)
                vkd3d_string_buffer_printf(buffer, ")");
            break;
        }

        case WGSL_OPERAND_SCALAR:
            if (component_count > 1)
            {
                wgsl_print_type(buffer, gen, data_type, component_count);
                vkd3d_string_buffer_printf(buffer, "(");
            }
            if (wgsl_scalar_type(data_type) == scalar_type)
                vkd3d_string_buffer_printf(buffer, "%s", name->buffer);
            else
                vkd3d_string_buffer_printf(buffer, "bitcast<%s>(%s)", wgsl_scalar_name(gen, data_type), name->buffer);
            if (component_count > 1)
                vkd3d_string_buffer_printf(buffer, ")");
            break;

        case WGSL_OPERAND_VEC4_U32:
            if (reg->dimension != VSIR_DIMENSION_VEC4)
            {
                /* A scalar register, splatted to as many components as
                 * the mask has. */
                if (component_count > 1)
                {
                    wgsl_print_type(buffer, gen, data_type, component_count);
                    vkd3d_string_buffer_printf(buffer, "(");
                }
                if (wgsl_scalar_type(data_type) == VSIR_DATA_U32)
                    vkd3d_string_buffer_printf(buffer, "%s.x", name->buffer);
                else
                    vkd3d_string_buffer_printf(buffer, "bitcast<%s>(%s.x)",
                            wgsl_scalar_name(gen, data_type), name->buffer);
                if (component_count > 1)
                    vkd3d_string_buffer_printf(buffer, ")");
                break;
            }
            if (wgsl_scalar_type(data_type) == VSIR_DATA_U32)
            {
                vkd3d_string_buffer_printf(buffer, "%s", name->buffer);
            }
            else
            {
                vkd3d_string_buffer_printf(buffer, "bitcast<");
                wgsl_print_type(buffer, gen, data_type, component_count);
                vkd3d_string_buffer_printf(buffer, ">(%s", name->buffer);
            }
            if (reg->dimension == VSIR_DIMENSION_VEC4)
                wgsl_print_swizzle(buffer, vsir_src->swizzle, mask);
            else
                vkd3d_string_buffer_printf(buffer, ".x");
            if (wgsl_scalar_type(data_type) != VSIR_DATA_U32)
                vkd3d_string_buffer_printf(buffer, ")");
            break;
    }

    vkd3d_string_buffer_release(&gen->string_buffers, name);
}

static void wgsl_src_init(struct wgsl_src *wgsl_src, struct wgsl_generator *gen,
        const struct vsir_src_operand *vsir_src, uint32_t mask, enum vsir_data_type data_type)
{
    wgsl_src->str = vkd3d_string_buffer_get(&gen->string_buffers);
    wgsl_print_src_with_type(wgsl_src->str, gen, vsir_src, mask, data_type);
}

static void wgsl_src_cleanup(struct wgsl_src *src, struct vkd3d_string_buffer_cache *cache)
{
    vkd3d_string_buffer_release(cache, src->str);
}

static void wgsl_dst_cleanup(struct wgsl_dst *dst, struct vkd3d_string_buffer_cache *cache)
{
    if (dst->register_name)
        vkd3d_string_buffer_release(cache, dst->register_name);
}

/* Returns the write mask; 0 for a null destination, which nothing should
 * be written to. */
static uint32_t wgsl_dst_init(struct wgsl_dst *wgsl_dst, struct wgsl_generator *gen,
        const struct vsir_instruction *ins, const struct vsir_dst_operand *vsir_dst)
{
    enum wgsl_operand_kind kind;

    memset(wgsl_dst, 0, sizeof(*wgsl_dst));
    wgsl_dst->vsir = vsir_dst;

    if (vsir_dst->reg.type == VSIR_REGISTER_NULL)
        return 0;

    if (vsir_dst->reg.non_uniform)
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Unhandled 'non-uniform' modifier.");

    wgsl_dst->register_name = vkd3d_string_buffer_get(&gen->string_buffers);
    kind = wgsl_print_operand_name(wgsl_dst->register_name, gen, &vsir_dst->reg, &wgsl_dst->scalar_type);
    wgsl_dst->vec4_u32 = kind == WGSL_OPERAND_VEC4_U32;
    if (kind == WGSL_OPERAND_SCALAR)
        return VKD3DSP_WRITEMASK_0;
    if (kind == WGSL_OPERAND_IMMEDIATE)
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Immediate destination register.");

    return vsir_dst->reg.dimension == VSIR_DIMENSION_VEC4 ? vsir_dst->write_mask : VKD3DSP_WRITEMASK_0;
}

/* Writes "value", of type data_type with the write mask's component count,
 * to the destination. */
static void VKD3D_PRINTF_FUNC(4, 5) wgsl_print_assignment_with_type(struct wgsl_generator *gen,
        struct wgsl_dst *dst, enum vsir_data_type data_type, const char *format, ...)
{
    const struct vsir_dst_operand *vsir_dst = dst->vsir;
    const char components[] = "xyzw";
    unsigned int component_count, i, j;
    uint32_t write_mask;
    va_list args;

    if (!dst->register_name)
        return;

    wgsl_print_indent(gen->buffer, gen->indent);

    if (!dst->vec4_u32)
    {
        vkd3d_string_buffer_printf(gen->buffer, "%s = ", dst->register_name->buffer);
        if (wgsl_scalar_type(data_type) != dst->scalar_type)
            vkd3d_string_buffer_printf(gen->buffer, "bitcast<%s>(", wgsl_scalar_name(gen, dst->scalar_type));
        va_start(args, format);
        vkd3d_string_buffer_vprintf(gen->buffer, format, args);
        va_end(args);
        if (wgsl_scalar_type(data_type) != dst->scalar_type)
            vkd3d_string_buffer_printf(gen->buffer, ")");
        vkd3d_string_buffer_printf(gen->buffer, ";\n");
        return;
    }

    write_mask = vsir_dst->reg.dimension == VSIR_DIMENSION_VEC4 ? vsir_dst->write_mask : VKD3DSP_WRITEMASK_0;
    component_count = vsir_write_mask_component_count(write_mask);

    if (write_mask == VKD3DSP_WRITEMASK_ALL || component_count == 1)
    {
        vkd3d_string_buffer_printf(gen->buffer, "%s", dst->register_name->buffer);
        if (write_mask != VKD3DSP_WRITEMASK_ALL)
            wgsl_print_write_mask(gen->buffer, write_mask);
        vkd3d_string_buffer_printf(gen->buffer, " = ");
        if (wgsl_scalar_type(data_type) != VSIR_DATA_U32)
        {
            vkd3d_string_buffer_printf(gen->buffer, "bitcast<");
            wgsl_print_type(gen->buffer, gen, VSIR_DATA_U32, component_count);
            vkd3d_string_buffer_printf(gen->buffer, ">(");
        }
        va_start(args, format);
        vkd3d_string_buffer_vprintf(gen->buffer, format, args);
        va_end(args);
        if (wgsl_scalar_type(data_type) != VSIR_DATA_U32)
            vkd3d_string_buffer_printf(gen->buffer, ")");
        vkd3d_string_buffer_printf(gen->buffer, ";\n");
        return;
    }

    vkd3d_string_buffer_printf(gen->buffer, "{\n");
    wgsl_print_indent(gen->buffer, gen->indent + 1);
    vkd3d_string_buffer_printf(gen->buffer, "let t = ");
    if (wgsl_scalar_type(data_type) != VSIR_DATA_U32)
    {
        vkd3d_string_buffer_printf(gen->buffer, "bitcast<");
        wgsl_print_type(gen->buffer, gen, VSIR_DATA_U32, component_count);
        vkd3d_string_buffer_printf(gen->buffer, ">(");
    }
    va_start(args, format);
    vkd3d_string_buffer_vprintf(gen->buffer, format, args);
    va_end(args);
    if (wgsl_scalar_type(data_type) != VSIR_DATA_U32)
        vkd3d_string_buffer_printf(gen->buffer, ")");
    vkd3d_string_buffer_printf(gen->buffer, ";\n");
    for (i = 0, j = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        if (!(write_mask & (VKD3DSP_WRITEMASK_0 << i)))
            continue;
        wgsl_print_indent(gen->buffer, gen->indent + 1);
        vkd3d_string_buffer_printf(gen->buffer, "%s.%c = t.%c;\n",
                dst->register_name->buffer, components[i], components[j++]);
    }
    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "}\n");
}

static void wgsl_print_subscript(struct vkd3d_string_buffer *buffer, struct wgsl_generator *gen,
        const struct vsir_src_operand *rel_addr, unsigned int offset)
{
    struct wgsl_src r;

    if (!rel_addr)
    {
        vkd3d_string_buffer_printf(buffer, "[%u]", offset);
        return;
    }

    wgsl_src_init(&r, gen, rel_addr, VKD3DSP_WRITEMASK_0, VSIR_DATA_U32);
    vkd3d_string_buffer_printf(buffer, "[%s", r.str->buffer);
    if (offset)
        vkd3d_string_buffer_printf(buffer, " + %uu", offset);
    vkd3d_string_buffer_printf(buffer, "]");
    wgsl_src_cleanup(&r, &gen->string_buffers);
}

static void wgsl_unhandled(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    const char *name = vsir_opcode_get_name(ins->opcode, "<unknown>");

    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "/* <unhandled instruction \"%s\" (%#x)> */\n", name, ins->opcode);
    wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
            "Internal compiler error: Unhandled instruction \"%s\" (%#x).", name, ins->opcode);
}

/* The type an instruction computes in: its destination's. */
static enum vsir_data_type wgsl_dst_type(const struct vsir_instruction *ins)
{
    enum vsir_data_type t = wgsl_scalar_type(ins->dst[0].reg.data_type);

    return t == VSIR_DATA_UNUSED ? ins->dst[0].reg.data_type : t;
}

static void wgsl_binop(struct wgsl_generator *gen, const struct vsir_instruction *ins, const char *op)
{
    enum vsir_data_type type = wgsl_dst_type(ins);
    struct wgsl_src src[2];
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src[0], gen, &ins->src[0], mask, type);
    wgsl_src_init(&src[1], gen, &ins->src[1], mask, type);

    wgsl_print_assignment_with_type(gen, &dst, type, "%s %s %s", src[0].str->buffer, op, src[1].str->buffer);

    wgsl_src_cleanup(&src[1], &gen->string_buffers);
    wgsl_src_cleanup(&src[0], &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* D3D masks a shift amount to five bits; WGSL requires the amount be u32. */
static void wgsl_shift(struct wgsl_generator *gen, const struct vsir_instruction *ins, const char *op)
{
    enum vsir_data_type type = ins->opcode == VSIR_OP_ISHR ? VSIR_DATA_I32 : VSIR_DATA_U32;
    unsigned int component_count;
    struct wgsl_src src[2];
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    component_count = vsir_write_mask_component_count(mask);
    wgsl_src_init(&src[0], gen, &ins->src[0], mask, type);
    wgsl_src_init(&src[1], gen, &ins->src[1], mask, VSIR_DATA_U32);

    if (component_count > 1)
        wgsl_print_assignment_with_type(gen, &dst, type, "%s %s (%s & vec%u<u32>(31u))",
                src[0].str->buffer, op, src[1].str->buffer, component_count);
    else
        wgsl_print_assignment_with_type(gen, &dst, type, "%s %s (%s & 31u)",
                src[0].str->buffer, op, src[1].str->buffer);

    wgsl_src_cleanup(&src[1], &gen->string_buffers);
    wgsl_src_cleanup(&src[0], &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* D3D's unsigned division and remainder give 0xffffffff for a zero divisor;
 * WGSL's give the dividend and 0. */
static void wgsl_udiv(struct wgsl_generator *gen, const struct vsir_instruction *ins, const char *op)
{
    unsigned int component_count;
    struct wgsl_src src[2];
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    component_count = vsir_write_mask_component_count(mask);
    wgsl_src_init(&src[0], gen, &ins->src[0], mask, VSIR_DATA_U32);
    wgsl_src_init(&src[1], gen, &ins->src[1], mask, VSIR_DATA_U32);

    if (component_count > 1)
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32,
                "select(%s %s %s, vec%u<u32>(0xffffffffu), %s == vec%u<u32>(0u))",
                src[0].str->buffer, op, src[1].str->buffer, component_count,
                src[1].str->buffer, component_count);
    else
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "select(%s %s %s, 0xffffffffu, %s == 0u)",
                src[0].str->buffer, op, src[1].str->buffer, src[1].str->buffer);

    wgsl_src_cleanup(&src[1], &gen->string_buffers);
    wgsl_src_cleanup(&src[0], &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_dot(struct wgsl_generator *gen, const struct vsir_instruction *ins, uint32_t src_mask)
{
    unsigned int component_count;
    struct wgsl_src src[2];
    struct wgsl_dst dst;
    uint32_t dst_mask;

    if (!(dst_mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src[0], gen, &ins->src[0], src_mask, VSIR_DATA_F32);
    wgsl_src_init(&src[1], gen, &ins->src[1], src_mask, VSIR_DATA_F32);

    if ((component_count = vsir_write_mask_component_count(dst_mask)) > 1)
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_F32, "vec%u<f32>(dot(%s, %s))",
                component_count, src[0].str->buffer, src[1].str->buffer);
    else
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_F32, "dot(%s, %s)",
                src[0].str->buffer, src[1].str->buffer);

    wgsl_src_cleanup(&src[1], &gen->string_buffers);
    wgsl_src_cleanup(&src[0], &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* An intrinsic whose arguments are all of the destination's type. */
static void wgsl_intrinsic_typed(struct wgsl_generator *gen, const struct vsir_instruction *ins,
        const char *op, enum vsir_data_type type)
{
    struct vkd3d_string_buffer *args;
    struct wgsl_src src;
    struct wgsl_dst dst;
    unsigned int i;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    args = vkd3d_string_buffer_get(&gen->string_buffers);

    for (i = 0; i < ins->src_count; ++i)
    {
        wgsl_src_init(&src, gen, &ins->src[i], mask, type);
        vkd3d_string_buffer_printf(args, "%s%s", i ? ", " : "", src.str->buffer);
        wgsl_src_cleanup(&src, &gen->string_buffers);
    }

    wgsl_print_assignment_with_type(gen, &dst, type, "%s(%s)", op, args->buffer);

    vkd3d_string_buffer_release(&gen->string_buffers, args);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_intrinsic(struct wgsl_generator *gen, const struct vsir_instruction *ins, const char *op)
{
    wgsl_intrinsic_typed(gen, ins, op, wgsl_dst_type(ins));
}

static void wgsl_rcp(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    unsigned int component_count;
    struct wgsl_src src;
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    component_count = vsir_write_mask_component_count(mask);
    wgsl_src_init(&src, gen, &ins->src[0], mask, VSIR_DATA_F32);
    if (component_count > 1)
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_F32, "vec%u<f32>(1.0f) / %s",
                component_count, src.str->buffer);
    else
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_F32, "1.0f / %s", src.str->buffer);
    wgsl_src_cleanup(&src, &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* A comparison, giving vsir's 0/~0 booleans. "op" is printed between the
 * operands; "unordered" also makes the result true when either is NaN. */
static void wgsl_relop(struct wgsl_generator *gen, const struct vsir_instruction *ins,
        const char *op, bool unordered)
{
    enum vsir_data_type type = wgsl_scalar_type(ins->src[0].reg.data_type);
    struct vkd3d_string_buffer *condition;
    unsigned int mask_size;
    struct wgsl_src src[2];
    struct wgsl_dst dst;
    uint32_t mask;

    if (type == VSIR_DATA_UNUSED)
        type = ins->src[0].reg.data_type;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src[0], gen, &ins->src[0], mask, type);
    wgsl_src_init(&src[1], gen, &ins->src[1], mask, type);
    mask_size = vsir_write_mask_component_count(mask);

    condition = vkd3d_string_buffer_get(&gen->string_buffers);
    if (unordered)
        vkd3d_string_buffer_printf(condition, "!(%s %s %s)", src[0].str->buffer, op, src[1].str->buffer);
    else
        vkd3d_string_buffer_printf(condition, "%s %s %s", src[0].str->buffer, op, src[1].str->buffer);

    if (mask_size > 1)
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32,
                "select(vec%u<u32>(0u), vec%u<u32>(0xffffffffu), %s)",
                mask_size, mask_size, condition->buffer);
    else
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "select(0u, 0xffffffffu, %s)", condition->buffer);

    vkd3d_string_buffer_release(&gen->string_buffers, condition);
    wgsl_src_cleanup(&src[1], &gen->string_buffers);
    wgsl_src_cleanup(&src[0], &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* A per-component float classification, from the bits; WGSL has no
 * isnan() or isinf(). */
static void wgsl_float_class(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    struct vkd3d_string_buffer *condition;
    unsigned int mask_size;
    struct wgsl_src src;
    struct wgsl_dst dst;
    const char *test;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    mask_size = vsir_write_mask_component_count(mask);
    wgsl_src_init(&src, gen, &ins->src[0], mask, VSIR_DATA_U32);

    if (ins->opcode == VSIR_OP_ISNAN)
        test = ">";
    else if (ins->opcode == VSIR_OP_ISINF)
        test = "==";
    else
        test = "<";

    condition = vkd3d_string_buffer_get(&gen->string_buffers);
    if (mask_size > 1)
        vkd3d_string_buffer_printf(condition, "(%s & vec%u<u32>(0x7fffffffu)) %s vec%u<u32>(0x7f800000u)",
                src.str->buffer, mask_size, test, mask_size);
    else
        vkd3d_string_buffer_printf(condition, "(%s & 0x7fffffffu) %s 0x7f800000u", src.str->buffer, test);

    if (mask_size > 1)
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32,
                "select(vec%u<u32>(0u), vec%u<u32>(0xffffffffu), %s)",
                mask_size, mask_size, condition->buffer);
    else
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "select(0u, 0xffffffffu, %s)", condition->buffer);

    vkd3d_string_buffer_release(&gen->string_buffers, condition);
    wgsl_src_cleanup(&src, &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_cast(struct wgsl_generator *gen, const struct vsir_instruction *ins,
        enum vsir_data_type to, enum vsir_data_type from)
{
    unsigned int component_count;
    struct wgsl_src src;
    struct wgsl_dst dst;
    const char *negate;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src, gen, &ins->src[0], mask, from);

    /* A bool converted to float is 0/~0 as an integer: -1 as a signed one. */
    negate = "";
    if (ins->opcode == VSIR_OP_UTOF && data_type_is_bool(ins->src[0].reg.data_type))
    {
        wgsl_src_cleanup(&src, &gen->string_buffers);
        wgsl_src_init(&src, gen, &ins->src[0], mask, VSIR_DATA_I32);
        negate = "-";
    }

    component_count = vsir_write_mask_component_count(mask);
    if (component_count > 1)
        wgsl_print_assignment_with_type(gen, &dst, to, "vec%u<%s>(%s%s)",
                component_count, wgsl_scalar_name(gen, to), negate, src.str->buffer);
    else
        wgsl_print_assignment_with_type(gen, &dst, to, "%s(%s%s)",
                wgsl_scalar_name(gen, to), negate, src.str->buffer);

    wgsl_src_cleanup(&src, &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* firstbit_hi counts from the most significant bit; both give ~0 for 0. */
static void wgsl_firstbit(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    const char *op = ins->opcode == VSIR_OP_FIRSTBIT_LO ? "countTrailingZeros" : "countLeadingZeros";
    unsigned int mask_size;
    struct wgsl_src src;
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src, gen, &ins->src[0], mask, VSIR_DATA_U32);

    if ((mask_size = vsir_write_mask_component_count(mask)) > 1)
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32,
                "select(vec%u<u32>(0xffffffffu), %s(%s), %s != vec%u<u32>(0u))",
                mask_size, op, src.str->buffer, src.str->buffer, mask_size);
    else
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "select(0xffffffffu, %s(%s), %s != 0u)",
                op, src.str->buffer, src.str->buffer);

    wgsl_src_cleanup(&src, &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* ubfe/ibfe dst, width, offset, src. WGSL's extractBits() clamps the width
 * to the bits left above the offset, as D3D does. */
static void wgsl_bitfield_extract(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    enum vsir_data_type type = ins->opcode == VSIR_OP_IBFE ? VSIR_DATA_I32 : VSIR_DATA_U32;
    const char components[] = "xyzw";
    struct vkd3d_string_buffer *value;
    struct wgsl_src src[3];
    struct wgsl_dst dst;
    unsigned int i, j;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    value = vkd3d_string_buffer_get(&gen->string_buffers);

    /* extractBits() takes scalar offsets and counts. */
    if (vsir_write_mask_component_count(mask) > 1)
    {
        wgsl_print_type(value, gen, type, vsir_write_mask_component_count(mask));
        vkd3d_string_buffer_printf(value, "(");
    }
    for (i = 0, j = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        uint32_t c = VKD3DSP_WRITEMASK_0 << i;

        if (!(mask & c))
            continue;
        wgsl_src_init(&src[0], gen, &ins->src[0], c, VSIR_DATA_U32);
        wgsl_src_init(&src[1], gen, &ins->src[1], c, VSIR_DATA_U32);
        wgsl_src_init(&src[2], gen, &ins->src[2], c, type);
        vkd3d_string_buffer_printf(value, "%sextractBits(%s, %s & 31u, %s & 31u)", j++ ? ", " : "",
                src[2].str->buffer, src[1].str->buffer, src[0].str->buffer);
        wgsl_src_cleanup(&src[2], &gen->string_buffers);
        wgsl_src_cleanup(&src[1], &gen->string_buffers);
        wgsl_src_cleanup(&src[0], &gen->string_buffers);
    }
    if (vsir_write_mask_component_count(mask) > 1)
        vkd3d_string_buffer_printf(value, ")");
    (void)components;

    wgsl_print_assignment_with_type(gen, &dst, type, "%s", value->buffer);

    vkd3d_string_buffer_release(&gen->string_buffers, value);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* bfi dst, width, offset, insert, base. */
static void wgsl_bitfield_insert(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    struct vkd3d_string_buffer *value;
    struct wgsl_src src[4];
    struct wgsl_dst dst;
    unsigned int i, j;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    value = vkd3d_string_buffer_get(&gen->string_buffers);

    if (vsir_write_mask_component_count(mask) > 1)
        vkd3d_string_buffer_printf(value, "vec%u<u32>(", vsir_write_mask_component_count(mask));
    for (i = 0, j = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        uint32_t c = VKD3DSP_WRITEMASK_0 << i;

        if (!(mask & c))
            continue;
        wgsl_src_init(&src[0], gen, &ins->src[0], c, VSIR_DATA_U32);
        wgsl_src_init(&src[1], gen, &ins->src[1], c, VSIR_DATA_U32);
        wgsl_src_init(&src[2], gen, &ins->src[2], c, VSIR_DATA_U32);
        wgsl_src_init(&src[3], gen, &ins->src[3], c, VSIR_DATA_U32);
        vkd3d_string_buffer_printf(value, "%sinsertBits(%s, %s, %s & 31u, %s & 31u)", j++ ? ", " : "",
                src[3].str->buffer, src[2].str->buffer, src[1].str->buffer, src[0].str->buffer);
        wgsl_src_cleanup(&src[3], &gen->string_buffers);
        wgsl_src_cleanup(&src[2], &gen->string_buffers);
        wgsl_src_cleanup(&src[1], &gen->string_buffers);
        wgsl_src_cleanup(&src[0], &gen->string_buffers);
    }
    if (vsir_write_mask_component_count(mask) > 1)
        vkd3d_string_buffer_printf(value, ")");

    wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "%s", value->buffer);

    vkd3d_string_buffer_release(&gen->string_buffers, value);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* f16tof32 and f32tof16, one component at a time through WGSL's packing
 * builtins. */
static void wgsl_half(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    bool to_half = ins->opcode == VSIR_OP_F32TOF16;
    struct vkd3d_string_buffer *value;
    unsigned int i, j, count;
    struct wgsl_src src;
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    value = vkd3d_string_buffer_get(&gen->string_buffers);
    count = vsir_write_mask_component_count(mask);

    if (count > 1)
        vkd3d_string_buffer_printf(value, "vec%u<%s>(", count, to_half ? "u32" : "f32");
    for (i = 0, j = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        uint32_t c = VKD3DSP_WRITEMASK_0 << i;

        if (!(mask & c))
            continue;
        if (to_half)
        {
            wgsl_src_init(&src, gen, &ins->src[0], c, VSIR_DATA_F32);
            vkd3d_string_buffer_printf(value, "%s(pack2x16float(vec2<f32>(%s, 0.0f)) & 0xffffu)",
                    j++ ? ", " : "", src.str->buffer);
        }
        else
        {
            wgsl_src_init(&src, gen, &ins->src[0], c, VSIR_DATA_U32);
            vkd3d_string_buffer_printf(value, "%sunpack2x16float(%s).x", j++ ? ", " : "", src.str->buffer);
        }
        wgsl_src_cleanup(&src, &gen->string_buffers);
    }
    if (count > 1)
        vkd3d_string_buffer_printf(value, ")");

    wgsl_print_assignment_with_type(gen, &dst, to_half ? VSIR_DATA_U32 : VSIR_DATA_F32, "%s", value->buffer);

    vkd3d_string_buffer_release(&gen->string_buffers, value);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_end_block(struct wgsl_generator *gen)
{
    --gen->indent;
    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "}\n");
}

static void wgsl_begin_block(struct wgsl_generator *gen)
{
    vkd3d_string_buffer_printf(gen->buffer, "{\n");
    ++gen->indent;
}

static void wgsl_print_condition(struct vkd3d_string_buffer *buffer, struct wgsl_generator *gen,
        enum vkd3d_shader_conditional_op op, const struct vsir_src_operand *arg)
{
    struct wgsl_src src;

    wgsl_src_init(&src, gen, arg, VKD3DSP_WRITEMASK_0, VSIR_DATA_U32);
    vkd3d_string_buffer_printf(buffer, "if (%s %s 0u) ",
            src.str->buffer, op == VKD3D_SHADER_CONDITIONAL_OP_NZ ? "!=" : "==");
    wgsl_src_cleanup(&src, &gen->string_buffers);
}

/* Code that is about to be printed inside a switch: open the clause the
 * labels before it named. */
static void wgsl_open_pending_clause(struct wgsl_generator *gen)
{
    struct wgsl_switch *s;

    if (!gen->switch_count)
        return;
    s = &gen->switches[gen->switch_count - 1];
    if (!s->pending->content_size)
        return;

    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "%s: ", s->pending->buffer);
    vkd3d_string_buffer_clear(s->pending);
    wgsl_begin_block(gen);
    s->in_clause = true;
}

static void wgsl_discard(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    /* WGSL's discard demotes the invocation to a helper, as D3D's does. */
    wgsl_print_indent(gen->buffer, gen->indent);
    wgsl_print_condition(gen->buffer, gen, ins->flags, &ins->src[0]);
    vkd3d_string_buffer_printf(gen->buffer, "{ discard; }\n");
}

static void wgsl_if(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    wgsl_print_indent(gen->buffer, gen->indent);
    wgsl_print_condition(gen->buffer, gen, ins->flags, &ins->src[0]);
    wgsl_begin_block(gen);
}

static void wgsl_else(struct wgsl_generator *gen)
{
    --gen->indent;
    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "} else ");
    wgsl_begin_block(gen);
}

static void wgsl_loop(struct wgsl_generator *gen)
{
    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "loop ");
    wgsl_begin_block(gen);
}

static void wgsl_switch(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    struct wgsl_switch *s;
    struct wgsl_src src;

    if (!vkd3d_array_reserve((void **)&gen->switches, &gen->switch_capacity,
            gen->switch_count + 1, sizeof(*gen->switches)))
    {
        gen->failed = true;
        return;
    }
    s = &gen->switches[gen->switch_count++];
    memset(s, 0, sizeof(*s));
    s->pending = vkd3d_string_buffer_get(&gen->string_buffers);
    s->selector_type = wgsl_scalar_type(ins->src[0].reg.data_type) == VSIR_DATA_I32
            ? VSIR_DATA_I32 : VSIR_DATA_U32;

    wgsl_src_init(&src, gen, &ins->src[0], VKD3DSP_WRITEMASK_0, s->selector_type);
    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "switch (%s) ", src.str->buffer);
    wgsl_begin_block(gen);
    wgsl_src_cleanup(&src, &gen->string_buffers);
}

static void wgsl_close_clause(struct wgsl_generator *gen, struct wgsl_switch *s)
{
    if (!s->in_clause)
        return;
    wgsl_end_block(gen);
    s->in_clause = false;
}

static void wgsl_case(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    struct wgsl_switch *s;
    uint32_t value;

    if (!gen->switch_count)
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL, "Internal compiler error: Case outside a switch.");
        return;
    }
    s = &gen->switches[gen->switch_count - 1];
    wgsl_close_clause(gen, s);

    if (ins->src[0].reg.type != VSIR_REGISTER_IMMCONST)
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Unhandled non-constant case label.");
        return;
    }
    value = ins->src[0].reg.u.immconst_u32[0];

    vkd3d_string_buffer_printf(s->pending, "%s", s->pending->content_size ? ", " : "case ");
    if (s->selector_type == VSIR_DATA_I32)
        vkd3d_string_buffer_printf(s->pending, "%di", (int32_t)value);
    else
        vkd3d_string_buffer_printf(s->pending, "%uu", value);
}

static void wgsl_default(struct wgsl_generator *gen)
{
    struct wgsl_switch *s;

    if (!gen->switch_count)
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Default outside a switch.");
        return;
    }
    s = &gen->switches[gen->switch_count - 1];
    wgsl_close_clause(gen, s);
    vkd3d_string_buffer_printf(s->pending, "%s", s->pending->content_size ? ", default" : "default");
    s->has_default = true;
}

static void wgsl_endswitch(struct wgsl_generator *gen)
{
    struct wgsl_switch *s;

    if (!gen->switch_count)
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Endswitch outside a switch.");
        return;
    }
    s = &gen->switches[gen->switch_count - 1];
    wgsl_close_clause(gen, s);
    /* Labels with no code after them: an empty clause. */
    if (s->pending->content_size)
    {
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "%s: {}\n", s->pending->buffer);
    }
    if (!s->has_default)
    {
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "default: {}\n");
    }
    vkd3d_string_buffer_release(&gen->string_buffers, s->pending);
    --gen->switch_count;
    wgsl_end_block(gen);
}

static void wgsl_print_texel_offset(struct vkd3d_string_buffer *buffer, struct wgsl_generator *gen,
        unsigned int offset_size, const struct vkd3d_shader_texel_offset *offset)
{
    switch (offset_size)
    {
        case 1:
            vkd3d_string_buffer_printf(buffer, "%di", offset->u);
            break;
        case 2:
            vkd3d_string_buffer_printf(buffer, "vec2<i32>(%d, %d)", offset->u, offset->v);
            break;
        default:
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                    "Internal compiler error: Invalid texel offset size %u.", offset_size);
            /* fall through */
        case 3:
            vkd3d_string_buffer_printf(buffer, "vec3<i32>(%d, %d, %d)", offset->u, offset->v, offset->w);
            break;
    }
}

static enum vkd3d_shader_resource_type wgsl_effective_resource_type(enum vkd3d_shader_resource_type type,
        unsigned int sample_count)
{
    /* Similar to the other targets, multi-sample textures with sample count
     * 1 are their single-sample equivalents. */
    if (sample_count == 1)
    {
        if (type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMS)
            return VKD3D_SHADER_RESOURCE_TEXTURE_2D;
        if (type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY)
            return VKD3D_SHADER_RESOURCE_TEXTURE_2DARRAY;
    }
    return type;
}

/* The texture's result is a vec4 of its sample type; swizzle and bitcast it
 * to what the destination wants. */
static void wgsl_print_texel_result(struct wgsl_generator *gen, struct wgsl_dst *dst,
        const struct vsir_instruction *ins, enum vsir_data_type resource_type,
        const char *value, uint32_t swizzle, bool scalar_value)
{
    struct vkd3d_string_buffer *result;
    unsigned int count;
    uint32_t mask;

    mask = ins->dst[0].reg.dimension == VSIR_DIMENSION_VEC4 ? ins->dst[0].write_mask : VKD3DSP_WRITEMASK_0;
    count = vsir_write_mask_component_count(mask);
    result = vkd3d_string_buffer_get(&gen->string_buffers);
    if (scalar_value)
    {
        if (count > 1)
        {
            wgsl_print_type(result, gen, resource_type, count);
            vkd3d_string_buffer_printf(result, "(%s)", value);
        }
        else
        {
            vkd3d_string_buffer_printf(result, "%s", value);
        }
    }
    else
    {
        vkd3d_string_buffer_printf(result, "(%s)", value);
        wgsl_print_swizzle(result, swizzle, mask);
    }
    wgsl_print_assignment_with_type(gen, dst, resource_type, "%s", result->buffer);
    vkd3d_string_buffer_release(&gen->string_buffers, result);
}

static void wgsl_ld(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    const struct wgsl_resource_type_info *resource_type_info;
    enum vkd3d_shader_resource_type resource_type;
    const struct vsir_descriptor *descriptor;
    unsigned int resource_id, resource_idx;
    struct vkd3d_string_buffer *read;
    unsigned int sample_count = 1;
    enum vsir_data_type data_type;
    uint32_t coord_mask;
    struct wgsl_dst dst;

    if (vsir_instruction_has_texel_offset(ins))
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                "Texel fetch offsets are not supported.");

    if ((descriptor = wgsl_find_descriptor(gen, VKD3D_SHADER_DESCRIPTOR_TYPE_SRV,
            &ins->src[1], &resource_id, &resource_idx)))
    {
        resource_type = descriptor->resource_type;
        sample_count = descriptor->sample_count;
        data_type = wgsl_scalar_type(descriptor->resource_data_type);
    }
    else
    {
        resource_type = VKD3D_SHADER_RESOURCE_TEXTURE_2D;
        data_type = VSIR_DATA_F32;
    }
    resource_type = wgsl_effective_resource_type(resource_type, sample_count);

    if (resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_CUBE
            || resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_CUBEARRAY
            || resource_type == VKD3D_SHADER_RESOURCE_BUFFER)
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                "Texel fetches from resource type %#x are not supported.", resource_type);

    if (!(resource_type_info = wgsl_get_resource_type_info(resource_type)))
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Unhandled resource type %#x.", resource_type);
        resource_type_info = wgsl_get_resource_type_info(VKD3D_SHADER_RESOURCE_TEXTURE_2D);
    }
    coord_mask = vkd3d_write_mask_from_component_count(resource_type_info->coord_size);

    if (!wgsl_dst_init(&dst, gen, ins, &ins->dst[0]))
        return;
    read = vkd3d_string_buffer_get(&gen->string_buffers);

    vkd3d_string_buffer_printf(read, "textureLoad(");
    wgsl_print_descriptor_name(read, VKD3D_SHADER_DESCRIPTOR_TYPE_SRV, resource_id, resource_idx);
    vkd3d_string_buffer_printf(read, ", ");
    wgsl_print_src_with_type(read, gen, &ins->src[0], coord_mask, VSIR_DATA_I32);
    if (resource_type_info->array)
    {
        vkd3d_string_buffer_printf(read, ", ");
        wgsl_print_src_with_type(read, gen, &ins->src[0],
                VKD3DSP_WRITEMASK_0 << resource_type_info->coord_size, VSIR_DATA_I32);
    }
    vkd3d_string_buffer_printf(read, ", ");
    if (ins->opcode != VSIR_OP_LD2DMS)
        wgsl_print_src_with_type(read, gen, &ins->src[0], VKD3DSP_WRITEMASK_3, VSIR_DATA_I32);
    else if (sample_count == 1)
        /* If the resource isn't a true multisample resource, this is the
         * "lod" parameter instead of the "sample" parameter. */
        vkd3d_string_buffer_printf(read, "0i");
    else
        wgsl_print_src_with_type(read, gen, &ins->src[2], VKD3DSP_WRITEMASK_0, VSIR_DATA_I32);
    vkd3d_string_buffer_printf(read, ")");

    wgsl_print_texel_result(gen, &dst, ins, data_type, read->buffer, ins->src[1].swizzle, false);

    vkd3d_string_buffer_release(&gen->string_buffers, read);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_sample(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    bool bias, compare, dynamic_offset, gather, grad, lod, lod_zero, offset;
    const struct wgsl_resource_type_info *resource_type_info;
    unsigned int resource_id, resource_idx, sampler_id, sampler_idx;
    const struct vsir_src_operand *resource, *sampler;
    enum vkd3d_shader_resource_type resource_type;
    struct vkd3d_string_buffer *sample;
    enum vsir_data_type data_type;
    const struct vsir_descriptor *d;
    unsigned int component_idx;
    uint32_t coord_mask;
    struct wgsl_dst dst;

    bias = ins->opcode == VSIR_OP_SAMPLE_B;
    compare = ins->opcode == VSIR_OP_GATHER4_C || ins->opcode == VSIR_OP_SAMPLE_C
            || ins->opcode == VSIR_OP_SAMPLE_C_LZ;
    dynamic_offset = ins->opcode == VSIR_OP_GATHER4_PO;
    gather = ins->opcode == VSIR_OP_GATHER4 || ins->opcode == VSIR_OP_GATHER4_C
            || ins->opcode == VSIR_OP_GATHER4_PO;
    grad = ins->opcode == VSIR_OP_SAMPLE_GRAD;
    lod = ins->opcode == VSIR_OP_SAMPLE_LOD;
    lod_zero = ins->opcode == VSIR_OP_SAMPLE_C_LZ;
    offset = dynamic_offset || vsir_instruction_has_texel_offset(ins);

    /* Implicit-LOD sampling exists only in fragment shaders; elsewhere D3D's
     * sample is level 0. */
    if (gen->program->shader_version.type != VKD3D_SHADER_TYPE_PIXEL && !gather && !grad && !lod)
    {
        if (bias || ins->opcode == VSIR_OP_SAMPLE)
            lod_zero = true, bias = false;
        else if (ins->opcode == VSIR_OP_SAMPLE_C)
            lod_zero = true;
    }

    resource = &ins->src[1 + dynamic_offset];
    sampler = &ins->src[2 + dynamic_offset];

    if ((d = wgsl_find_descriptor(gen, VKD3D_SHADER_DESCRIPTOR_TYPE_SRV, resource, &resource_id, &resource_idx)))
    {
        resource_type = d->resource_type;
        data_type = wgsl_scalar_type(d->resource_data_type);
    }
    else
    {
        resource_type = VKD3D_SHADER_RESOURCE_TEXTURE_2D;
        data_type = VSIR_DATA_F32;
    }

    if (resource_type == VKD3D_SHADER_RESOURCE_BUFFER
            || resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMS
            || resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY)
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                "Sampling resource type %#x is not supported.", resource_type);

    if ((resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_1D || resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_1DARRAY)
            && (bias || grad || lod || compare || gather))
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                "Resource type %#x supports only plain sampling.", resource_type);

    if (!(resource_type_info = wgsl_get_resource_type_info(resource_type)))
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Unhandled resource type %#x.", resource_type);
        resource_type_info = wgsl_get_resource_type_info(VKD3D_SHADER_RESOURCE_TEXTURE_2D);
    }
    coord_mask = vkd3d_write_mask_from_component_count(resource_type_info->coord_size);

    wgsl_find_descriptor(gen, VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER, sampler, &sampler_id, &sampler_idx);

    if (!wgsl_dst_init(&dst, gen, ins, &ins->dst[0]))
        return;
    sample = vkd3d_string_buffer_get(&gen->string_buffers);

    if (gather && compare)
        vkd3d_string_buffer_printf(sample, "textureGatherCompare(");
    else if (gather)
        vkd3d_string_buffer_printf(sample, "textureGather(%u, ",
                (component_idx = vsir_swizzle_get_component(sampler->swizzle, 0)));
    else if (compare && lod_zero)
        vkd3d_string_buffer_printf(sample, "textureSampleCompareLevel(");
    else if (compare)
        vkd3d_string_buffer_printf(sample, "textureSampleCompare(");
    else if (grad)
        vkd3d_string_buffer_printf(sample, "textureSampleGrad(");
    else if (lod || lod_zero)
        vkd3d_string_buffer_printf(sample, "textureSampleLevel(");
    else if (bias)
        vkd3d_string_buffer_printf(sample, "textureSampleBias(");
    else
        vkd3d_string_buffer_printf(sample, "textureSample(");

    wgsl_print_descriptor_name(sample, VKD3D_SHADER_DESCRIPTOR_TYPE_SRV, resource_id, resource_idx);
    if (compare)
        vkd3d_string_buffer_printf(sample, "_depth");
    vkd3d_string_buffer_printf(sample, ", ");
    wgsl_print_descriptor_name(sample, VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER, sampler_id, sampler_idx);
    vkd3d_string_buffer_printf(sample, ", ");
    wgsl_print_src_with_type(sample, gen, &ins->src[0], coord_mask, VSIR_DATA_F32);
    if (resource_type_info->array)
    {
        vkd3d_string_buffer_printf(sample, ", i32(");
        wgsl_print_src_with_type(sample, gen, &ins->src[0],
                VKD3DSP_WRITEMASK_0 << resource_type_info->coord_size, VSIR_DATA_F32);
        vkd3d_string_buffer_printf(sample, ")");
    }
    if (compare)
    {
        vkd3d_string_buffer_printf(sample, ", ");
        wgsl_print_src_with_type(sample, gen, &ins->src[3], VKD3DSP_WRITEMASK_0, VSIR_DATA_F32);
    }
    if (grad)
    {
        vkd3d_string_buffer_printf(sample, ", ");
        wgsl_print_src_with_type(sample, gen, &ins->src[3], coord_mask, VSIR_DATA_F32);
        vkd3d_string_buffer_printf(sample, ", ");
        wgsl_print_src_with_type(sample, gen, &ins->src[4], coord_mask, VSIR_DATA_F32);
    }
    if (lod_zero && !compare)
    {
        vkd3d_string_buffer_printf(sample, ", 0.0f");
    }
    else if (lod)
    {
        vkd3d_string_buffer_printf(sample, ", ");
        wgsl_print_src_with_type(sample, gen, &ins->src[3], VKD3DSP_WRITEMASK_0, VSIR_DATA_F32);
    }
    if (bias)
    {
        vkd3d_string_buffer_printf(sample, ", ");
        wgsl_print_src_with_type(sample, gen, &ins->src[3], VKD3DSP_WRITEMASK_0, VSIR_DATA_F32);
    }
    if (offset)
    {
        if (!resource_type_info->offset)
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                    "Texel sample offsets are not supported with resource type %#x.", resource_type);
        vkd3d_string_buffer_printf(sample, ", ");
        if (dynamic_offset)
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                    "Dynamic texel offsets are not supported.");
        else
            wgsl_print_texel_offset(sample, gen, resource_type_info->coord_size, &ins->texel_offset);
    }
    vkd3d_string_buffer_printf(sample, ")");

    /* A comparison sample is a scalar f32. */
    wgsl_print_texel_result(gen, &dst, ins, compare ? VSIR_DATA_F32 : data_type, sample->buffer,
            resource->swizzle, compare && !gather);

    vkd3d_string_buffer_release(&gen->string_buffers, sample);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_unary_op(struct wgsl_generator *gen, const struct vsir_instruction *ins, const char *op)
{
    enum vsir_data_type type = wgsl_dst_type(ins);
    struct wgsl_src src;
    struct wgsl_dst dst;
    uint32_t mask;

    /* WGSL negates only signed types; the bits are the same. */
    if (!strcmp(op, "-") && wgsl_scalar_type(type) == VSIR_DATA_U32)
        type = VSIR_DATA_I32;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src, gen, &ins->src[0], mask, type);

    wgsl_print_assignment_with_type(gen, &dst, type, "%s%s", op, src.str->buffer);

    wgsl_src_cleanup(&src, &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_mov(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    struct wgsl_src src;
    struct wgsl_dst dst;
    uint32_t mask;

    /* A move is bits: copy them as u32, so a NaN payload survives. */
    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src, gen, &ins->src[0], mask,
            dst.vec4_u32 ? VSIR_DATA_U32 : dst.scalar_type);

    wgsl_print_assignment_with_type(gen, &dst, dst.vec4_u32 ? VSIR_DATA_U32 : dst.scalar_type,
            "%s", src.str->buffer);

    wgsl_src_cleanup(&src, &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_movc(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    unsigned int component_count;
    struct wgsl_src src[3];
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src[0], gen, &ins->src[0], mask, VSIR_DATA_U32);
    wgsl_src_init(&src[1], gen, &ins->src[1], mask, VSIR_DATA_U32);
    wgsl_src_init(&src[2], gen, &ins->src[2], mask, VSIR_DATA_U32);

    if ((component_count = vsir_write_mask_component_count(mask)) > 1)
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "select(%s, %s, %s != vec%u<u32>(0u))",
                src[2].str->buffer, src[1].str->buffer, src[0].str->buffer, component_count);
    else
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "select(%s, %s, %s != 0u)",
                src[2].str->buffer, src[1].str->buffer, src[0].str->buffer);

    wgsl_src_cleanup(&src[2], &gen->string_buffers);
    wgsl_src_cleanup(&src[1], &gen->string_buffers);
    wgsl_src_cleanup(&src[0], &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* cmp: src0 >= 0 ? src1 : src2, per component, src0 compared as float. */
/* cmp: src1 where src0 >= 0, src2 elsewhere; cnd: where src0 > 0.5. */
static void wgsl_cmp(struct wgsl_generator *gen, const struct vsir_instruction *ins, const char *condition)
{
    unsigned int component_count;
    struct wgsl_src src[3];
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src[0], gen, &ins->src[0], mask, VSIR_DATA_F32);
    wgsl_src_init(&src[1], gen, &ins->src[1], mask, VSIR_DATA_U32);
    wgsl_src_init(&src[2], gen, &ins->src[2], mask, VSIR_DATA_U32);

    if ((component_count = vsir_write_mask_component_count(mask)) > 1)
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "select(%s, %s, %s %s vec%u<f32>(%s))",
                src[2].str->buffer, src[1].str->buffer, src[0].str->buffer,
                condition, component_count, condition[1] == '=' ? "0.0f" : "0.5f");
    else
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "select(%s, %s, %s %s %s)",
                src[2].str->buffer, src[1].str->buffer, src[0].str->buffer,
                condition, condition[1] == '=' ? "0.0f" : "0.5f");

    wgsl_src_cleanup(&src[2], &gen->string_buffers);
    wgsl_src_cleanup(&src[1], &gen->string_buffers);
    wgsl_src_cleanup(&src[0], &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* lit: the lighting coefficients (1, diffuse, specular, 1) of
 * src = (N.L, N.H, -, power). */
static void wgsl_lit(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    unsigned int i, j;
    struct wgsl_src src;
    char swizzle[5];
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src, gen, &ins->src[0], VKD3DSP_WRITEMASK_ALL, VSIR_DATA_F32);

    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "{\n");
    wgsl_print_indent(gen->buffer, gen->indent + 1);
    vkd3d_string_buffer_printf(gen->buffer, "let l = %s;\n", src.str->buffer);
    wgsl_print_indent(gen->buffer, gen->indent + 1);
    vkd3d_string_buffer_printf(gen->buffer, "let p = clamp(l.w, -127.9961f, 127.9961f);\n");
    for (i = 0, j = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        if (mask & (VKD3DSP_WRITEMASK_0 << i))
            swizzle[j++] = "xyzw"[i];
    }
    swizzle[j] = 0;
    ++gen->indent;
    wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_F32,
            "vec4<f32>(1.0f, max(l.x, 0.0f), select(0.0f, pow(max(l.y, 0.0f), p), l.x > 0.0f && l.y > 0.0f), 1.0f).%s",
            swizzle);
    --gen->indent;
    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "}\n");

    wgsl_src_cleanup(&src, &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_sgn(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    struct wgsl_src src;
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src, gen, &ins->src[0], mask, VSIR_DATA_F32);
    wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_F32, "sign(%s)", src.str->buffer);
    wgsl_src_cleanup(&src, &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* slt and sge: 1.0 or 0.0. */
static void wgsl_float_set(struct wgsl_generator *gen, const struct vsir_instruction *ins, const char *op)
{
    unsigned int component_count;
    struct wgsl_src src[2];
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src[0], gen, &ins->src[0], mask, VSIR_DATA_F32);
    wgsl_src_init(&src[1], gen, &ins->src[1], mask, VSIR_DATA_F32);

    if ((component_count = vsir_write_mask_component_count(mask)) > 1)
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_F32,
                "select(vec%u<f32>(0.0f), vec%u<f32>(1.0f), %s %s %s)",
                component_count, component_count, src[0].str->buffer, op, src[1].str->buffer);
    else
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_F32, "select(0.0f, 1.0f, %s %s %s)",
                src[0].str->buffer, op, src[1].str->buffer);

    wgsl_src_cleanup(&src[1], &gen->string_buffers);
    wgsl_src_cleanup(&src[0], &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* ord and uno: whether neither, or either, operand is NaN. */
static void wgsl_ordered(struct wgsl_generator *gen, const struct vsir_instruction *ins, bool ordered)
{
    struct vkd3d_string_buffer *condition;
    unsigned int mask_size;
    struct wgsl_src src[2];
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&src[0], gen, &ins->src[0], mask, VSIR_DATA_F32);
    wgsl_src_init(&src[1], gen, &ins->src[1], mask, VSIR_DATA_F32);
    mask_size = vsir_write_mask_component_count(mask);

    condition = vkd3d_string_buffer_get(&gen->string_buffers);
    if (mask_size > 1)
        vkd3d_string_buffer_printf(condition, "%s((%s == %s) & (%s == %s))", ordered ? "" : "!",
                src[0].str->buffer, src[0].str->buffer, src[1].str->buffer, src[1].str->buffer);
    else
        vkd3d_string_buffer_printf(condition, "%s((%s == %s) && (%s == %s))", ordered ? "" : "!",
                src[0].str->buffer, src[0].str->buffer, src[1].str->buffer, src[1].str->buffer);

    if (mask_size > 1)
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32,
                "select(vec%u<u32>(0u), vec%u<u32>(0xffffffffu), %s)", mask_size, mask_size, condition->buffer);
    else
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "select(0u, 0xffffffffu, %s)", condition->buffer);

    vkd3d_string_buffer_release(&gen->string_buffers, condition);
    wgsl_src_cleanup(&src[1], &gen->string_buffers);
    wgsl_src_cleanup(&src[0], &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_ret(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "return;\n");
}

static void wgsl_dcl_indexable_temp(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    const struct vkd3d_shader_indexable_temp *t = &ins->declaration.indexable_temp;
    const struct vkd3d_shader_immediate_constant_buffer *icb = t->initialiser;
    unsigned int i;

    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "var x%u: array<vec4<u32>, %u>", t->register_idx, t->register_size);
    if (icb && !icb->is_null && !data_type_is_64_bit(icb->data_type))
    {
        vkd3d_string_buffer_printf(gen->buffer, " = array<vec4<u32>, %u>(", t->register_size);
        for (i = 0; i < icb->element_count && i < t->register_size; ++i)
        {
            if (icb->component_count == 4)
                vkd3d_string_buffer_printf(gen->buffer, "%svec4<u32>(%#xu, %#xu, %#xu, %#xu)", i ? ", " : "",
                        icb->data[4 * i + 0], icb->data[4 * i + 1], icb->data[4 * i + 2], icb->data[4 * i + 3]);
            else
                vkd3d_string_buffer_printf(gen->buffer, "%svec4<u32>(%#xu, 0u, 0u, 0u)", i ? ", " : "",
                        icb->data[i]);
        }
        vkd3d_string_buffer_printf(gen->buffer, ")");
    }
    vkd3d_string_buffer_printf(gen->buffer, ";\n");
}

static void wgsl_barrier(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    uint32_t flags = ins->flags;

    /* WGSL's barriers are all execution barriers; there is no fence alone.
     * A memory-only sync is the nearest barrier, which is stronger. */
    if (flags & (VKD3DSSF_GLOBAL_UAV | VKD3DSSF_THREAD_GROUP_UAV))
    {
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "storageBarrier();\n");
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "textureBarrier();\n");
        flags &= ~(VKD3DSSF_GLOBAL_UAV | VKD3DSSF_THREAD_GROUP_UAV);
    }
    if (flags & VKD3DSSF_GROUP_SHARED_MEMORY)
    {
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "workgroupBarrier();\n");
        flags &= ~VKD3DSSF_GROUP_SHARED_MEMORY;
    }
    if (flags & VKD3DSSF_THREAD_GROUP)
    {
        if (!(ins->flags & VKD3DSSF_GROUP_SHARED_MEMORY))
        {
            wgsl_print_indent(gen->buffer, gen->indent);
            vkd3d_string_buffer_printf(gen->buffer, "workgroupBarrier();\n");
        }
        flags &= ~VKD3DSSF_THREAD_GROUP;
    }

    if (flags)
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Unhandled synchronisation flags %#x.", flags);
}

/* Raw and structured buffers, SRV or UAV, and groupshared memory, are all
 * arrays of u32 words; of atomic<u32> when the shader does atomics on
 * them, which then makes every access an atomic one. */
struct wgsl_word_array
{
    struct vkd3d_string_buffer *name;
    unsigned int stride; /* In words; 0 for raw. */
    bool atomic;
    bool valid;
};

static bool wgsl_is_atomic_target(struct wgsl_generator *gen, enum vsir_register_type type, unsigned int id)
{
    struct vsir_program_iterator it = vsir_program_iterator(&gen->program->instructions);
    const struct vsir_instruction *ins;
    const struct vsir_dst_operand *dst;

    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
    {
        if (ins->opcode >= VSIR_OP_ATOMIC_AND && ins->opcode <= VSIR_OP_ATOMIC_XOR)
            dst = &ins->dst[0];
        else if (vsir_opcode_is_imm_atomic(ins->opcode))
            dst = &ins->dst[1];
        else
            continue;
        if (dst->reg.type == type && dst->reg.idx[0].offset == id)
            return true;
    }
    return false;
}

static void wgsl_word_array_init(struct wgsl_word_array *a, struct wgsl_generator *gen,
        const struct vsir_operand *reg)
{
    const struct vsir_descriptor *d;
    struct vsir_tgsm *t;
    unsigned int id;

    memset(a, 0, sizeof(*a));
    a->name = vkd3d_string_buffer_get(&gen->string_buffers);

    if (reg->idx[0].rel_addr || (reg->idx_count > 1 && reg->idx[1].rel_addr))
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED, "Descriptor indexing is not supported.");
    id = reg->idx[0].offset;

    switch (reg->type)
    {
        case VSIR_REGISTER_GROUPSHAREDMEM:
            if (!(t = vsir_program_find_tgsm(gen->program, id)))
            {
                wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                        "Internal compiler error: Undeclared groupshared memory %u.", id);
                return;
            }
            vkd3d_string_buffer_printf(a->name, "g%u", id);
            a->stride = t->structure_stride / 4;
            a->atomic = wgsl_is_atomic_target(gen, reg->type, id);
            a->valid = true;
            return;

        case VSIR_REGISTER_RESOURCE:
        case VSIR_REGISTER_UAV:
            if (!(d = vkd3d_shader_find_descriptor(&gen->program->descriptors, reg->type == VSIR_REGISTER_UAV
                    ? VKD3D_SHADER_DESCRIPTOR_TYPE_UAV : VKD3D_SHADER_DESCRIPTOR_TYPE_SRV, id)))
            {
                wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                        "Internal compiler error: Undeclared descriptor %u.", id);
                return;
            }
            wgsl_print_descriptor_name(a->name, d->type, id, reg->idx[1].offset);
            a->stride = d->structure_stride / 4;
            a->atomic = reg->type == VSIR_REGISTER_UAV && wgsl_is_atomic_target(gen, reg->type, id);
            a->valid = true;
            return;

        default:
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                    "Internal compiler error: Unhandled buffer register type %#x.", reg->type);
            return;
    }
}

static void wgsl_word_array_cleanup(struct wgsl_word_array *a, struct wgsl_generator *gen)
{
    vkd3d_string_buffer_release(&gen->string_buffers, a->name);
}

/* The word index of a raw (byte offset) or structured (index, byte offset)
 * address, as a u32 expression. */
static void wgsl_print_word_address(struct vkd3d_string_buffer *buffer, struct wgsl_generator *gen,
        const struct wgsl_word_array *a, const struct vsir_src_operand *index, uint32_t index_mask,
        const struct vsir_src_operand *offset, uint32_t offset_mask)
{
    struct wgsl_src i, o;

    wgsl_src_init(&o, gen, offset, offset_mask, VSIR_DATA_U32);
    if (a->stride && index)
    {
        wgsl_src_init(&i, gen, index, index_mask, VSIR_DATA_U32);
        vkd3d_string_buffer_printf(buffer, "(%s * %uu + (%s >> 2u))", i.str->buffer, a->stride, o.str->buffer);
        wgsl_src_cleanup(&i, &gen->string_buffers);
    }
    else
    {
        vkd3d_string_buffer_printf(buffer, "(%s >> 2u)", o.str->buffer);
    }
    wgsl_src_cleanup(&o, &gen->string_buffers);
}

static void wgsl_ld_raw_structured(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    bool structured = ins->opcode == VSIR_OP_LD_STRUCTURED;
    const struct vsir_src_operand *resource = &ins->src[structured ? 2 : 1];
    struct vkd3d_string_buffer *address, *value;
    struct wgsl_word_array a;
    unsigned int i, j, count;
    struct wgsl_dst dst;
    uint32_t mask;

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_word_array_init(&a, gen, &resource->reg);
    address = vkd3d_string_buffer_get(&gen->string_buffers);
    value = vkd3d_string_buffer_get(&gen->string_buffers);

    if (structured)
        wgsl_print_word_address(address, gen, &a, &ins->src[0], VKD3DSP_WRITEMASK_0,
                &ins->src[1], VKD3DSP_WRITEMASK_0);
    else
        wgsl_print_word_address(address, gen, &a, NULL, 0, &ins->src[0], VKD3DSP_WRITEMASK_0);

    count = vsir_write_mask_component_count(mask);
    if (count > 1)
        vkd3d_string_buffer_printf(value, "vec%u<u32>(", count);
    for (i = 0, j = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        unsigned int word;

        if (!(mask & (VKD3DSP_WRITEMASK_0 << i)))
            continue;
        word = vsir_swizzle_get_component(resource->swizzle, i);
        vkd3d_string_buffer_printf(value, "%s%s%s[%s + %uu]%s", j++ ? ", " : "",
                a.atomic ? "atomicLoad(&" : "", a.name->buffer, address->buffer, word, a.atomic ? ")" : "");
    }
    if (count > 1)
        vkd3d_string_buffer_printf(value, ")");

    wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "%s", value->buffer);

    vkd3d_string_buffer_release(&gen->string_buffers, value);
    vkd3d_string_buffer_release(&gen->string_buffers, address);
    wgsl_word_array_cleanup(&a, gen);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_store_raw_structured(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    bool structured = ins->opcode == VSIR_OP_STORE_STRUCTURED;
    const struct vsir_src_operand *data = &ins->src[structured ? 2 : 1];
    struct vkd3d_string_buffer *address;
    struct wgsl_word_array a;
    struct wgsl_src value;
    unsigned int i, j;

    wgsl_word_array_init(&a, gen, &ins->dst[0].reg);
    address = vkd3d_string_buffer_get(&gen->string_buffers);
    if (structured)
        wgsl_print_word_address(address, gen, &a, &ins->src[0], VKD3DSP_WRITEMASK_0,
                &ins->src[1], VKD3DSP_WRITEMASK_0);
    else
        wgsl_print_word_address(address, gen, &a, NULL, 0, &ins->src[0], VKD3DSP_WRITEMASK_0);

    /* The written components go to consecutive words. */
    for (i = 0, j = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        if (!(ins->dst[0].write_mask & (VKD3DSP_WRITEMASK_0 << i)))
            continue;
        wgsl_src_init(&value, gen, data, VKD3DSP_WRITEMASK_0 << i, VSIR_DATA_U32);
        wgsl_print_indent(gen->buffer, gen->indent);
        if (a.atomic)
            vkd3d_string_buffer_printf(gen->buffer, "atomicStore(&%s[%s + %uu], %s);\n",
                    a.name->buffer, address->buffer, i, value.str->buffer);
        else
            vkd3d_string_buffer_printf(gen->buffer, "%s[%s + %uu] = %s;\n",
                    a.name->buffer, address->buffer, i, value.str->buffer);
        wgsl_src_cleanup(&value, &gen->string_buffers);
        ++j;
    }

    vkd3d_string_buffer_release(&gen->string_buffers, address);
    wgsl_word_array_cleanup(&a, gen);
}

static void wgsl_atomic(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    bool is_imm = vsir_opcode_is_imm_atomic(ins->opcode);
    const struct vsir_dst_operand *resource = &ins->dst[is_imm];
    struct vkd3d_string_buffer *address, *pointer;
    struct wgsl_src value, compare;
    const char *op = NULL;
    struct wgsl_word_array a;
    bool signed_minmax = false;
    struct wgsl_dst dst;

    switch (ins->opcode)
    {
        case VSIR_OP_ATOMIC_AND:
        case VSIR_OP_IMM_ATOMIC_AND:
            op = "atomicAnd";
            break;
        case VSIR_OP_ATOMIC_IADD:
        case VSIR_OP_IMM_ATOMIC_IADD:
            op = "atomicAdd";
            break;
        case VSIR_OP_ATOMIC_OR:
        case VSIR_OP_IMM_ATOMIC_OR:
            op = "atomicOr";
            break;
        case VSIR_OP_ATOMIC_XOR:
        case VSIR_OP_IMM_ATOMIC_XOR:
            op = "atomicXor";
            break;
        case VSIR_OP_ATOMIC_UMAX:
        case VSIR_OP_IMM_ATOMIC_UMAX:
            op = "atomicMax";
            break;
        case VSIR_OP_ATOMIC_UMIN:
        case VSIR_OP_IMM_ATOMIC_UMIN:
            op = "atomicMin";
            break;
        case VSIR_OP_IMM_ATOMIC_EXCH:
            op = "atomicExchange";
            break;
        case VSIR_OP_ATOMIC_IMAX:
        case VSIR_OP_IMM_ATOMIC_IMAX:
            op = "max";
            signed_minmax = true;
            break;
        case VSIR_OP_ATOMIC_IMIN:
        case VSIR_OP_IMM_ATOMIC_IMIN:
            op = "min";
            signed_minmax = true;
            break;
        case VSIR_OP_ATOMIC_CMP_STORE:
        case VSIR_OP_IMM_ATOMIC_CMP_EXCH:
            break;
        default:
            wgsl_unhandled(gen, ins);
            return;
    }

    if (resource->reg.type != VSIR_REGISTER_GROUPSHAREDMEM)
    {
        const struct vsir_descriptor *d = vkd3d_shader_find_descriptor(&gen->program->descriptors,
                VKD3D_SHADER_DESCRIPTOR_TYPE_UAV, resource->reg.idx[0].offset);

        if (d && d->resource_type != VKD3D_SHADER_RESOURCE_BUFFER)
        {
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                    "Atomic operations on textures are not supported.");
            return;
        }
    }

    wgsl_word_array_init(&a, gen, &resource->reg);
    address = vkd3d_string_buffer_get(&gen->string_buffers);
    pointer = vkd3d_string_buffer_get(&gen->string_buffers);
    /* The address register is (index, byte offset) for a structured
     * buffer, the byte offset alone otherwise. */
    wgsl_print_word_address(address, gen, &a, &ins->src[0], VKD3DSP_WRITEMASK_0,
            &ins->src[0], a.stride ? VKD3DSP_WRITEMASK_1 : VKD3DSP_WRITEMASK_0);
    vkd3d_string_buffer_printf(pointer, "&%s[%s]", a.name->buffer, address->buffer);

    if (is_imm && !wgsl_dst_init(&dst, gen, ins, &ins->dst[0]))
        is_imm = false;

    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "{\n");
    ++gen->indent;

    if (ins->opcode == VSIR_OP_ATOMIC_CMP_STORE || ins->opcode == VSIR_OP_IMM_ATOMIC_CMP_EXCH)
    {
        /* WGSL's compare-exchange is weak; D3D's is not. */
        wgsl_src_init(&compare, gen, &ins->src[1], VKD3DSP_WRITEMASK_0, VSIR_DATA_U32);
        wgsl_src_init(&value, gen, &ins->src[2], VKD3DSP_WRITEMASK_0, VSIR_DATA_U32);
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "var old: u32;\n");
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "loop {\n");
        wgsl_print_indent(gen->buffer, gen->indent + 1);
        vkd3d_string_buffer_printf(gen->buffer, "let r = atomicCompareExchangeWeak(%s, %s, %s);\n",
                pointer->buffer, compare.str->buffer, value.str->buffer);
        wgsl_print_indent(gen->buffer, gen->indent + 1);
        vkd3d_string_buffer_printf(gen->buffer, "if (r.exchanged || r.old_value != %s) { old = r.old_value; break; }\n",
                compare.str->buffer);
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "}\n");
        wgsl_src_cleanup(&value, &gen->string_buffers);
        wgsl_src_cleanup(&compare, &gen->string_buffers);
    }
    else if (signed_minmax)
    {
        /* There is no signed atomic min or max on atomic<u32>. */
        wgsl_src_init(&value, gen, &ins->src[1], VKD3DSP_WRITEMASK_0, VSIR_DATA_I32);
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "var old: u32 = atomicLoad(%s);\n", pointer->buffer);
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "loop {\n");
        wgsl_print_indent(gen->buffer, gen->indent + 1);
        vkd3d_string_buffer_printf(gen->buffer,
                "let r = atomicCompareExchangeWeak(%s, old, bitcast<u32>(%s(bitcast<i32>(old), %s)));\n",
                pointer->buffer, op, value.str->buffer);
        wgsl_print_indent(gen->buffer, gen->indent + 1);
        vkd3d_string_buffer_printf(gen->buffer, "if (r.exchanged) { break; }\n");
        wgsl_print_indent(gen->buffer, gen->indent + 1);
        vkd3d_string_buffer_printf(gen->buffer, "old = r.old_value;\n");
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "}\n");
        wgsl_src_cleanup(&value, &gen->string_buffers);
    }
    else
    {
        wgsl_src_init(&value, gen, &ins->src[1], VKD3DSP_WRITEMASK_0, VSIR_DATA_U32);
        wgsl_print_indent(gen->buffer, gen->indent);
        vkd3d_string_buffer_printf(gen->buffer, "let old = %s(%s, %s);\n", op, pointer->buffer, value.str->buffer);
        wgsl_src_cleanup(&value, &gen->string_buffers);
    }

    if (is_imm)
    {
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "old");
        wgsl_dst_cleanup(&dst, &gen->string_buffers);
    }

    --gen->indent;
    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "}\n");

    if (!a.atomic)
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Atomic operation on a non-atomic array.");

    vkd3d_string_buffer_release(&gen->string_buffers, pointer);
    vkd3d_string_buffer_release(&gen->string_buffers, address);
    wgsl_word_array_cleanup(&a, gen);
}

/* bufinfo: a raw buffer's size in bytes, a structured buffer's in
 * structures. */
static void wgsl_bufinfo(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    struct wgsl_word_array a;
    struct wgsl_dst dst;
    unsigned int count;
    uint32_t mask;
    char value[96];

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_word_array_init(&a, gen, &ins->src[0].reg);
    if (a.stride)
        snprintf(value, sizeof(value), "(arrayLength(&%s) / %uu)", a.name->buffer, a.stride);
    else
        snprintf(value, sizeof(value), "(arrayLength(&%s) * 4u)", a.name->buffer);
    if ((count = vsir_write_mask_component_count(mask)) > 1)
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "vec%u<u32>(%s)", count, value);
    else
        wgsl_print_assignment_with_type(gen, &dst, VSIR_DATA_U32, "%s", value);
    wgsl_word_array_cleanup(&a, gen);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* A storage texture's WGSL format: from the target info if given, else a
 * single 32-bit channel of the declared type, the only formats WebGPU
 * reads and writes without features. */
static const char *wgsl_uav_format(struct wgsl_generator *gen, const struct vsir_descriptor *d, unsigned int idx)
{
    const struct vkd3d_shader_wgsl_target_info *info = gen->target_info;
    unsigned int i;

    if (info)
    {
        for (i = 0; i < info->uav_format_count; ++i)
        {
            if (info->uav_formats[i].register_space == d->register_space
                    && info->uav_formats[i].register_index == idx)
                return info->uav_formats[i].format;
        }
    }

    switch (wgsl_scalar_type(d->resource_data_type))
    {
        case VSIR_DATA_U32:
            return "r32uint";
        case VSIR_DATA_I32:
            return "r32sint";
        default:
            return "r32float";
    }
}

static const char *wgsl_storage_texture_suffix(enum vkd3d_shader_resource_type type)
{
    switch (type)
    {
        case VKD3D_SHADER_RESOURCE_TEXTURE_1D:
            return "1d";
        case VKD3D_SHADER_RESOURCE_TEXTURE_2D:
            return "2d";
        case VKD3D_SHADER_RESOURCE_TEXTURE_2DARRAY:
            return "2d_array";
        case VKD3D_SHADER_RESOURCE_TEXTURE_3D:
            return "3d";
        default:
            return NULL;
    }
}

static void wgsl_ld_uav_typed(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    const struct wgsl_resource_type_info *type_info;
    const struct vsir_descriptor *d;
    struct vkd3d_string_buffer *load;
    unsigned int uav_id, uav_idx;
    struct wgsl_dst dst;

    if (!(d = wgsl_find_descriptor(gen, VKD3D_SHADER_DESCRIPTOR_TYPE_UAV, &ins->src[1], &uav_id, &uav_idx)))
        return;
    if (!(type_info = wgsl_get_resource_type_info(d->resource_type)) || d->resource_type == VKD3D_SHADER_RESOURCE_BUFFER)
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                "Typed loads from UAVs of type %#x are not supported.", d->resource_type);
        return;
    }
    if (!wgsl_dst_init(&dst, gen, ins, &ins->dst[0]))
        return;

    load = vkd3d_string_buffer_get(&gen->string_buffers);
    vkd3d_string_buffer_printf(load, "textureLoad(");
    wgsl_print_descriptor_name(load, VKD3D_SHADER_DESCRIPTOR_TYPE_UAV, uav_id, uav_idx);
    vkd3d_string_buffer_printf(load, ", ");
    wgsl_print_src_with_type(load, gen, &ins->src[0],
            vkd3d_write_mask_from_component_count(type_info->coord_size), VSIR_DATA_U32);
    if (type_info->array)
    {
        vkd3d_string_buffer_printf(load, ", ");
        wgsl_print_src_with_type(load, gen, &ins->src[0],
                VKD3DSP_WRITEMASK_0 << type_info->coord_size, VSIR_DATA_U32);
    }
    vkd3d_string_buffer_printf(load, ")");
    wgsl_print_texel_result(gen, &dst, ins, wgsl_scalar_type(d->resource_data_type), load->buffer,
            ins->src[1].swizzle, false);

    vkd3d_string_buffer_release(&gen->string_buffers, load);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_store_uav_typed(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    const struct wgsl_resource_type_info *type_info;
    const struct vsir_descriptor *d;
    unsigned int uav_id, uav_idx;
    enum vsir_data_type type;
    struct wgsl_src value;

    if (ins->dst[0].reg.idx[0].rel_addr || ins->dst[0].reg.idx[1].rel_addr)
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED, "Descriptor indexing is not supported.");
    uav_id = ins->dst[0].reg.idx[0].offset;
    uav_idx = ins->dst[0].reg.idx[1].offset;
    if (!(d = vkd3d_shader_find_descriptor(&gen->program->descriptors, VKD3D_SHADER_DESCRIPTOR_TYPE_UAV, uav_id)))
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Undeclared UAV descriptor %u.", uav_id);
        return;
    }
    if (!(type_info = wgsl_get_resource_type_info(d->resource_type)) || d->resource_type == VKD3D_SHADER_RESOURCE_BUFFER)
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                "Typed stores to UAVs of type %#x are not supported.", d->resource_type);
        return;
    }
    type = wgsl_scalar_type(d->resource_data_type);

    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "textureStore(");
    wgsl_print_descriptor_name(gen->buffer, VKD3D_SHADER_DESCRIPTOR_TYPE_UAV, uav_id, uav_idx);
    vkd3d_string_buffer_printf(gen->buffer, ", ");
    wgsl_print_src_with_type(gen->buffer, gen, &ins->src[0],
            vkd3d_write_mask_from_component_count(type_info->coord_size), VSIR_DATA_U32);
    if (type_info->array)
    {
        vkd3d_string_buffer_printf(gen->buffer, ", ");
        wgsl_print_src_with_type(gen->buffer, gen, &ins->src[0],
                VKD3DSP_WRITEMASK_0 << type_info->coord_size, VSIR_DATA_U32);
    }
    vkd3d_string_buffer_printf(gen->buffer, ", ");
    if (ins->src[1].reg.dimension == VSIR_DIMENSION_SCALAR)
    {
        wgsl_src_init(&value, gen, &ins->src[1], VKD3DSP_WRITEMASK_0, type);
        vkd3d_string_buffer_printf(gen->buffer, "vec4<%s>(%s)", wgsl_scalar_name(gen, type), value.str->buffer);
    }
    else
    {
        wgsl_src_init(&value, gen, &ins->src[1], VKD3DSP_WRITEMASK_ALL, type);
        vkd3d_string_buffer_printf(gen->buffer, "%s", value.str->buffer);
    }
    vkd3d_string_buffer_printf(gen->buffer, ");\n");
    wgsl_src_cleanup(&value, &gen->string_buffers);
}

/* resinfo dst, level, resource: width, height, depth or layers, levels;
 * zero but the level count for a level out of range, as D3D gives. */
static void wgsl_resinfo(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    enum vkd3d_shader_descriptor_type type;
    const struct vsir_descriptor *d;
    struct vkd3d_string_buffer *value;
    unsigned int id, idx, count;
    char name[32], dims[160];
    struct wgsl_src level;
    bool storage, uint_result;
    struct wgsl_dst dst;
    uint32_t mask;

    type = ins->src[1].reg.type == VSIR_REGISTER_UAV ? VKD3D_SHADER_DESCRIPTOR_TYPE_UAV
            : VKD3D_SHADER_DESCRIPTOR_TYPE_SRV;
    if (!(d = wgsl_find_descriptor(gen, type, &ins->src[1], &id, &idx)))
        return;
    storage = type == VKD3D_SHADER_DESCRIPTOR_TYPE_UAV;
    snprintf(name, sizeof(name), "%s%u_%u", storage ? "u" : "t", id, idx);

    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    wgsl_src_init(&level, gen, &ins->src[0], VKD3DSP_WRITEMASK_0, VSIR_DATA_U32);
    value = vkd3d_string_buffer_get(&gen->string_buffers);

    /* The four components, as u32. */
    switch (wgsl_effective_resource_type(d->resource_type, d->sample_count))
    {
        case VKD3D_SHADER_RESOURCE_TEXTURE_1D:
            snprintf(dims, sizeof(dims), "textureDimensions(%s%s), 0u, 0u", name, storage ? "" : ", l");
            break;
        case VKD3D_SHADER_RESOURCE_TEXTURE_1DARRAY:
            snprintf(dims, sizeof(dims), "textureDimensions(%s%s), textureNumLayers(%s), 0u",
                    name, storage ? "" : ", l", name);
            break;
        case VKD3D_SHADER_RESOURCE_TEXTURE_2DMS:
            snprintf(dims, sizeof(dims), "textureDimensions(%s), 0u", name);
            break;
        case VKD3D_SHADER_RESOURCE_TEXTURE_2D:
        case VKD3D_SHADER_RESOURCE_TEXTURE_CUBE:
            snprintf(dims, sizeof(dims), "textureDimensions(%s%s), 0u", name, storage ? "" : ", l");
            break;
        case VKD3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY:
            snprintf(dims, sizeof(dims), "textureDimensions(%s), textureNumLayers(%s)", name, name);
            break;
        case VKD3D_SHADER_RESOURCE_TEXTURE_2DARRAY:
        case VKD3D_SHADER_RESOURCE_TEXTURE_CUBEARRAY:
            snprintf(dims, sizeof(dims), "textureDimensions(%s%s), textureNumLayers(%s)",
                    name, storage ? "" : ", l", name);
            break;
        case VKD3D_SHADER_RESOURCE_TEXTURE_3D:
            snprintf(dims, sizeof(dims), "textureDimensions(%s%s)", name, storage ? "" : ", l");
            break;
        default:
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                    "Querying the dimensions of resource type %#x is not supported.", d->resource_type);
            snprintf(dims, sizeof(dims), "0u, 0u, 0u");
            break;
    }

    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "{\n");
    ++gen->indent;
    wgsl_print_indent(gen->buffer, gen->indent);
    if (storage || d->resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMS
            || d->resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY)
        vkd3d_string_buffer_printf(gen->buffer, "let levels = 1u;\n");
    else
        vkd3d_string_buffer_printf(gen->buffer, "let levels = textureNumLevels(%s);\n", name);
    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "let l = min(%s, levels - 1u);\n", level.str->buffer);
    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "let d = select(vec4<u32>(0u, 0u, 0u, levels), "
            "vec4<u32>(%s, levels), %s < levels);\n", dims, level.str->buffer);

    uint_result = ins->flags & VKD3DSI_RESINFO_UINT;
    count = vsir_write_mask_component_count(mask);
    if (uint_result)
        vkd3d_string_buffer_printf(value, "d");
    else if (ins->flags & VKD3DSI_RESINFO_RCP_FLOAT)
        vkd3d_string_buffer_printf(value, "vec4<f32>(1.0f / vec3<f32>(d.xyz), f32(d.w))");
    else
        vkd3d_string_buffer_printf(value, "vec4<f32>(d)");
    wgsl_print_swizzle(value, ins->src[1].swizzle, mask);
    wgsl_print_assignment_with_type(gen, &dst, uint_result ? VSIR_DATA_U32 : VSIR_DATA_F32, "%s", value->buffer);
    (void)count;

    --gen->indent;
    wgsl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "}\n");

    vkd3d_string_buffer_release(&gen->string_buffers, value);
    wgsl_src_cleanup(&level, &gen->string_buffers);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

/* sample_info: a texture's sample count in x, as uint or float. */
static void wgsl_sample_info(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    const struct vsir_descriptor *d;
    struct vkd3d_string_buffer *value;
    unsigned int id, idx;
    struct wgsl_dst dst;
    uint32_t mask;
    bool uint_result = ins->flags & VKD3DSI_SAMPLE_INFO_UINT;

    if (ins->src[0].reg.type != VSIR_REGISTER_RESOURCE)
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                "The rasterizer's sample count is not supported.");
        return;
    }
    if (!(d = wgsl_find_descriptor(gen, VKD3D_SHADER_DESCRIPTOR_TYPE_SRV, &ins->src[0], &id, &idx)))
        return;
    if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
        return;
    value = vkd3d_string_buffer_get(&gen->string_buffers);
    vkd3d_string_buffer_printf(value, "%s", uint_result ? "(" : "vec4<f32>(");
    if (wgsl_effective_resource_type(d->resource_type, d->sample_count) == VKD3D_SHADER_RESOURCE_TEXTURE_2DMS
            || wgsl_effective_resource_type(d->resource_type, d->sample_count)
            == VKD3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY)
        vkd3d_string_buffer_printf(value, "vec4<u32>(textureNumSamples(t%u_%u), 0u, 0u, 0u))", id, idx);
    else
        vkd3d_string_buffer_printf(value, "vec4<u32>(1u, 0u, 0u, 0u))");
    wgsl_print_swizzle(value, ins->src[0].swizzle, mask);
    wgsl_print_assignment_with_type(gen, &dst, uint_result ? VSIR_DATA_U32 : VSIR_DATA_F32, "%s", value->buffer);
    vkd3d_string_buffer_release(&gen->string_buffers, value);
    wgsl_dst_cleanup(&dst, &gen->string_buffers);
}

static void wgsl_handle_instruction(struct wgsl_generator *gen, const struct vsir_instruction *ins)
{
    gen->location = ins->location;

    switch (ins->opcode)
    {
        case VSIR_OP_CASE:
        case VSIR_OP_DEFAULT:
        case VSIR_OP_ENDSWITCH:
        case VSIR_OP_NOP:
            break;
        default:
            wgsl_open_pending_clause(gen);
            break;
    }

    switch (ins->opcode)
    {
        case VSIR_OP_ABS:
            wgsl_intrinsic(gen, ins, "abs");
            break;
        case VSIR_OP_ACOS:
            wgsl_intrinsic(gen, ins, "acos");
            break;
        case VSIR_OP_ADD:
        case VSIR_OP_IADD:
            wgsl_binop(gen, ins, "+");
            break;
        case VSIR_OP_AND:
            wgsl_binop(gen, ins, "&");
            break;
        case VSIR_OP_ASIN:
            wgsl_intrinsic(gen, ins, "asin");
            break;
        case VSIR_OP_ATAN:
            wgsl_intrinsic(gen, ins, "atan");
            break;
        case VSIR_OP_BFI:
            wgsl_bitfield_insert(gen, ins);
            break;
        case VSIR_OP_BFREV:
            wgsl_intrinsic_typed(gen, ins, "reverseBits", VSIR_DATA_U32);
            break;
        case VSIR_OP_BREAK:
            wgsl_print_indent(gen->buffer, gen->indent);
            vkd3d_string_buffer_printf(gen->buffer, "break;\n");
            break;
        case VSIR_OP_CASE:
            wgsl_case(gen, ins);
            break;
        case VSIR_OP_CONTINUE:
            wgsl_print_indent(gen->buffer, gen->indent);
            vkd3d_string_buffer_printf(gen->buffer, "continue;\n");
            break;
        case VSIR_OP_COS:
            wgsl_intrinsic(gen, ins, "cos");
            break;
        case VSIR_OP_COUNTBITS:
            wgsl_intrinsic_typed(gen, ins, "countOneBits", VSIR_DATA_U32);
            break;
        case VSIR_OP_DCL_INDEXABLE_TEMP:
            wgsl_dcl_indexable_temp(gen, ins);
            break;
        case VSIR_OP_NOP:
            break;
        case VSIR_OP_DEFAULT:
            wgsl_default(gen);
            break;
        case VSIR_OP_DISCARD:
            wgsl_discard(gen, ins);
            break;
        case VSIR_OP_DIV:
            wgsl_binop(gen, ins, "/");
            break;
        case VSIR_OP_DP2:
            wgsl_dot(gen, ins, vkd3d_write_mask_from_component_count(2));
            break;
        case VSIR_OP_DP3:
            wgsl_dot(gen, ins, vkd3d_write_mask_from_component_count(3));
            break;
        case VSIR_OP_DP4:
            wgsl_dot(gen, ins, VKD3DSP_WRITEMASK_ALL);
            break;
        case VSIR_OP_DSX:
            wgsl_intrinsic(gen, ins, "dpdx");
            break;
        case VSIR_OP_DSX_COARSE:
            wgsl_intrinsic(gen, ins, "dpdxCoarse");
            break;
        case VSIR_OP_DSX_FINE:
            wgsl_intrinsic(gen, ins, "dpdxFine");
            break;
        case VSIR_OP_DSY:
            wgsl_intrinsic(gen, ins, "dpdy");
            break;
        case VSIR_OP_DSY_COARSE:
            wgsl_intrinsic(gen, ins, "dpdyCoarse");
            break;
        case VSIR_OP_DSY_FINE:
            wgsl_intrinsic(gen, ins, "dpdyFine");
            break;
        case VSIR_OP_ELSE:
            wgsl_else(gen);
            break;
        case VSIR_OP_ENDIF:
        case VSIR_OP_ENDLOOP:
            wgsl_end_block(gen);
            break;
        case VSIR_OP_ENDSWITCH:
            wgsl_endswitch(gen);
            break;
        case VSIR_OP_EQO:
        case VSIR_OP_IEQ:
            wgsl_relop(gen, ins, "==", false);
            break;
        case VSIR_OP_EQU:
            /* Equal or unordered: not (less or greater). */
            wgsl_relop(gen, ins, "!=", true);
            break;
        case VSIR_OP_EXP:
            wgsl_intrinsic(gen, ins, "exp2");
            break;
        case VSIR_OP_F16TOF32:
        case VSIR_OP_F32TOF16:
            wgsl_half(gen, ins);
            break;
        case VSIR_OP_FIRSTBIT_HI:
        case VSIR_OP_FIRSTBIT_LO:
            wgsl_firstbit(gen, ins);
            break;
        case VSIR_OP_FRC:
            wgsl_intrinsic(gen, ins, "fract");
            break;
        case VSIR_OP_FREM:
            wgsl_binop(gen, ins, "%");
            break;
        case VSIR_OP_FTOI:
            wgsl_cast(gen, ins, VSIR_DATA_I32, VSIR_DATA_F32);
            break;
        case VSIR_OP_FTOU:
            wgsl_cast(gen, ins, VSIR_DATA_U32, VSIR_DATA_F32);
            break;
        case VSIR_OP_GATHER4:
        case VSIR_OP_GATHER4_C:
        case VSIR_OP_GATHER4_PO:
        case VSIR_OP_SAMPLE:
        case VSIR_OP_SAMPLE_B:
        case VSIR_OP_SAMPLE_C:
        case VSIR_OP_SAMPLE_C_LZ:
        case VSIR_OP_SAMPLE_GRAD:
        case VSIR_OP_SAMPLE_LOD:
            wgsl_sample(gen, ins);
            break;
        case VSIR_OP_GEO:
        case VSIR_OP_IGE:
        case VSIR_OP_UGE:
            wgsl_relop(gen, ins, ">=", false);
            break;
        case VSIR_OP_GEU:
            wgsl_relop(gen, ins, "<", true);
            break;
        case VSIR_OP_HCOS:
            wgsl_intrinsic(gen, ins, "cosh");
            break;
        case VSIR_OP_HSIN:
            wgsl_intrinsic(gen, ins, "sinh");
            break;
        case VSIR_OP_HTAN:
            wgsl_intrinsic(gen, ins, "tanh");
            break;
        case VSIR_OP_IBFE:
        case VSIR_OP_UBFE:
            wgsl_bitfield_extract(gen, ins);
            break;
        case VSIR_OP_IDIV:
        case VSIR_OP_IREM:
            wgsl_binop(gen, ins, ins->opcode == VSIR_OP_IDIV ? "/" : "%");
            break;
        case VSIR_OP_IF:
            wgsl_if(gen, ins);
            break;
        case VSIR_OP_ILT:
        case VSIR_OP_LTO:
        case VSIR_OP_ULT:
            wgsl_relop(gen, ins, "<", false);
            break;
        case VSIR_OP_LTU:
            wgsl_relop(gen, ins, ">=", true);
            break;
        case VSIR_OP_IMAD:
        {
            enum vsir_data_type type = wgsl_dst_type(ins);
            struct wgsl_src src[3];
            struct wgsl_dst dst;
            uint32_t mask;

            if (!(mask = wgsl_dst_init(&dst, gen, ins, &ins->dst[0])))
                break;
            wgsl_src_init(&src[0], gen, &ins->src[0], mask, type);
            wgsl_src_init(&src[1], gen, &ins->src[1], mask, type);
            wgsl_src_init(&src[2], gen, &ins->src[2], mask, type);
            wgsl_print_assignment_with_type(gen, &dst, type, "%s * %s + %s",
                    src[0].str->buffer, src[1].str->buffer, src[2].str->buffer);
            wgsl_src_cleanup(&src[2], &gen->string_buffers);
            wgsl_src_cleanup(&src[1], &gen->string_buffers);
            wgsl_src_cleanup(&src[0], &gen->string_buffers);
            wgsl_dst_cleanup(&dst, &gen->string_buffers);
            break;
        }
        case VSIR_OP_MAD:
            wgsl_intrinsic(gen, ins, "fma");
            break;
        case VSIR_OP_IMAX:
            wgsl_intrinsic_typed(gen, ins, "max", VSIR_DATA_I32);
            break;
        case VSIR_OP_IMIN:
            wgsl_intrinsic_typed(gen, ins, "min", VSIR_DATA_I32);
            break;
        case VSIR_OP_MAX:
            wgsl_intrinsic_typed(gen, ins, "max", VSIR_DATA_F32);
            break;
        case VSIR_OP_MIN:
            wgsl_intrinsic_typed(gen, ins, "min", VSIR_DATA_F32);
            break;
        case VSIR_OP_UMAX:
            wgsl_intrinsic_typed(gen, ins, "max", VSIR_DATA_U32);
            break;
        case VSIR_OP_UMIN:
            wgsl_intrinsic_typed(gen, ins, "min", VSIR_DATA_U32);
            break;
        case VSIR_OP_IMUL_LOW:
            wgsl_binop(gen, ins, "*");
            break;
        case VSIR_OP_INE:
        case VSIR_OP_NEU:
            wgsl_relop(gen, ins, "!=", false);
            break;
        case VSIR_OP_NEO:
            /* Not equal and ordered: not (equal or unordered). */
            wgsl_relop(gen, ins, "==", true);
            break;
        case VSIR_OP_INEG:
        case VSIR_OP_NEG:
            wgsl_unary_op(gen, ins, "-");
            break;
        case VSIR_OP_ISFINITE:
        case VSIR_OP_ISINF:
        case VSIR_OP_ISNAN:
            wgsl_float_class(gen, ins);
            break;
        case VSIR_OP_ISHL:
            wgsl_shift(gen, ins, "<<");
            break;
        case VSIR_OP_ISHR:
        case VSIR_OP_USHR:
            wgsl_shift(gen, ins, ">>");
            break;
        case VSIR_OP_ITOF:
            wgsl_cast(gen, ins, VSIR_DATA_F32, VSIR_DATA_I32);
            break;
        case VSIR_OP_UTOF:
            wgsl_cast(gen, ins, VSIR_DATA_F32, VSIR_DATA_U32);
            break;
        case VSIR_OP_LD:
        case VSIR_OP_LD2DMS:
            wgsl_ld(gen, ins);
            break;
        case VSIR_OP_LOG:
            wgsl_intrinsic(gen, ins, "log2");
            break;
        case VSIR_OP_LOOP:
            wgsl_loop(gen);
            break;
        case VSIR_OP_MOV:
            wgsl_mov(gen, ins);
            break;
        case VSIR_OP_MOVC:
            wgsl_movc(gen, ins);
            break;
        case VSIR_OP_MUL:
            wgsl_binop(gen, ins, "*");
            break;
        case VSIR_OP_NOT:
            wgsl_unary_op(gen, ins, "~");
            break;
        case VSIR_OP_OR:
            wgsl_binop(gen, ins, "|");
            break;
        case VSIR_OP_RCP:
            wgsl_rcp(gen, ins);
            break;
        case VSIR_OP_RET:
            wgsl_ret(gen, ins);
            break;
        case VSIR_OP_BREAKP:
            wgsl_print_indent(gen->buffer, gen->indent);
            wgsl_print_condition(gen->buffer, gen, ins->flags, &ins->src[0]);
            vkd3d_string_buffer_printf(gen->buffer, "{ break; }\n");
            break;
        case VSIR_OP_CONTINUEP:
            wgsl_print_indent(gen->buffer, gen->indent);
            wgsl_print_condition(gen->buffer, gen, ins->flags, &ins->src[0]);
            vkd3d_string_buffer_printf(gen->buffer, "{ continue; }\n");
            break;
        case VSIR_OP_RETP:
            wgsl_print_indent(gen->buffer, gen->indent);
            wgsl_print_condition(gen->buffer, gen, ins->flags, &ins->src[0]);
            vkd3d_string_buffer_printf(gen->buffer, "{ return; }\n");
            break;
        case VSIR_OP_CMP:
            wgsl_cmp(gen, ins, ">=");
            break;
        case VSIR_OP_CND:
            wgsl_cmp(gen, ins, ">");
            break;
        case VSIR_OP_LIT:
            wgsl_lit(gen, ins);
            break;
        case VSIR_OP_SGN:
            /* Shader model 2's sgn has two scratch operands besides. */
            wgsl_sgn(gen, ins);
            break;
        case VSIR_OP_SLT:
            wgsl_float_set(gen, ins, "<");
            break;
        case VSIR_OP_SGE:
            wgsl_float_set(gen, ins, ">=");
            break;
        case VSIR_OP_ORD:
            wgsl_ordered(gen, ins, true);
            break;
        case VSIR_OP_UNO:
            wgsl_ordered(gen, ins, false);
            break;
        case VSIR_OP_ITOI:
        case VSIR_OP_UTOU:
            wgsl_mov(gen, ins);
            break;
        case VSIR_OP_ROUND_NE:
            wgsl_intrinsic(gen, ins, "round");
            break;
        case VSIR_OP_ROUND_NI:
            wgsl_intrinsic(gen, ins, "floor");
            break;
        case VSIR_OP_ROUND_PI:
            wgsl_intrinsic(gen, ins, "ceil");
            break;
        case VSIR_OP_ROUND_Z:
            wgsl_intrinsic(gen, ins, "trunc");
            break;
        case VSIR_OP_RSQ:
            wgsl_intrinsic(gen, ins, "inverseSqrt");
            break;
        case VSIR_OP_SATURATE:
            wgsl_intrinsic(gen, ins, "saturate");
            break;
        case VSIR_OP_SIN:
            wgsl_intrinsic(gen, ins, "sin");
            break;
        case VSIR_OP_SQRT:
            wgsl_intrinsic(gen, ins, "sqrt");
            break;
        case VSIR_OP_SWITCH:
            wgsl_switch(gen, ins);
            break;
        case VSIR_OP_SYNC:
            wgsl_barrier(gen, ins);
            break;
        case VSIR_OP_TAN:
            wgsl_intrinsic(gen, ins, "tan");
            break;
        case VSIR_OP_UDIV_SIMPLE:
            wgsl_udiv(gen, ins, "/");
            break;
        case VSIR_OP_UREM:
            wgsl_udiv(gen, ins, "%");
            break;
        case VSIR_OP_XOR:
            wgsl_binop(gen, ins, "^");
            break;
        case VSIR_OP_LD_RAW:
        case VSIR_OP_LD_STRUCTURED:
            wgsl_ld_raw_structured(gen, ins);
            break;
        case VSIR_OP_STORE_RAW:
        case VSIR_OP_STORE_STRUCTURED:
            wgsl_store_raw_structured(gen, ins);
            break;
        case VSIR_OP_LD_UAV_TYPED:
            wgsl_ld_uav_typed(gen, ins);
            break;
        case VSIR_OP_STORE_UAV_TYPED:
            wgsl_store_uav_typed(gen, ins);
            break;
        case VSIR_OP_ATOMIC_AND:
        case VSIR_OP_ATOMIC_CMP_STORE:
        case VSIR_OP_ATOMIC_IADD:
        case VSIR_OP_ATOMIC_IMAX:
        case VSIR_OP_ATOMIC_IMIN:
        case VSIR_OP_ATOMIC_OR:
        case VSIR_OP_ATOMIC_UMAX:
        case VSIR_OP_ATOMIC_UMIN:
        case VSIR_OP_ATOMIC_XOR:
        case VSIR_OP_IMM_ATOMIC_AND:
        case VSIR_OP_IMM_ATOMIC_CMP_EXCH:
        case VSIR_OP_IMM_ATOMIC_EXCH:
        case VSIR_OP_IMM_ATOMIC_IADD:
        case VSIR_OP_IMM_ATOMIC_IMAX:
        case VSIR_OP_IMM_ATOMIC_IMIN:
        case VSIR_OP_IMM_ATOMIC_OR:
        case VSIR_OP_IMM_ATOMIC_UMAX:
        case VSIR_OP_IMM_ATOMIC_UMIN:
        case VSIR_OP_IMM_ATOMIC_XOR:
            wgsl_atomic(gen, ins);
            break;
        case VSIR_OP_BUFINFO:
            wgsl_bufinfo(gen, ins);
            break;
        case VSIR_OP_RESINFO:
            wgsl_resinfo(gen, ins);
            break;
        case VSIR_OP_SAMPLE_INFO:
            wgsl_sample_info(gen, ins);
            break;
        case VSIR_OP_DCL_TGSM_RAW:
        case VSIR_OP_DCL_TGSM_STRUCTURED:
            break;
        default:
            wgsl_unhandled(gen, ins);
            break;
    }
}

/* The WGSL type of a stage input or output element: always four
 * components, so a vertex shader's outputs and a pixel shader's inputs
 * agree whatever their masks. */
static void wgsl_print_io_type(struct vkd3d_string_buffer *buffer, struct wgsl_generator *gen,
        const struct vsir_signature_element *e)
{
    switch (e->component_type)
    {
        case VKD3D_SHADER_COMPONENT_FLOAT:
            vkd3d_string_buffer_printf(buffer, "vec4<f32>");
            break;
        case VKD3D_SHADER_COMPONENT_INT:
            vkd3d_string_buffer_printf(buffer, "vec4<i32>");
            break;
        case VKD3D_SHADER_COMPONENT_UINT:
        case VKD3D_SHADER_COMPONENT_BOOL:
            vkd3d_string_buffer_printf(buffer, "vec4<u32>");
            break;
        default:
            vkd3d_string_buffer_printf(buffer, "<unhandled component type %#x>", e->component_type);
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                    "Internal compiler error: Unhandled component type %#x.", e->component_type);
            break;
    }
}

static bool wgsl_check_io_element(struct wgsl_generator *gen, const struct vsir_signature_element *e,
        bool *locations)
{
    if (e->target_location == SIGNATURE_TARGET_LOCATION_UNUSED)
        return false;

    if (e->target_location >= MAX_IO_REG_COUNT)
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Unhandled target location %u.", e->target_location);
        return false;
    }

    if (e->min_precision != VKD3D_SHADER_MINIMUM_PRECISION_NONE)
        TRACE("Ignoring minimum precision %#x.\n", e->min_precision);

    if (e->register_count > 1)
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Unhandled register count %u.", e->register_count);
        return false;
    }

    return true;
}

/* The interpolation the pixel shader declares for the varying at "location",
 * from the masks it filled in. */
static enum vkd3d_shader_interpolation_mode wgsl_next_stage_interpolation(struct wgsl_generator *gen,
        unsigned int location)
{
    const struct vkd3d_shader_varying_interpolation_info *info = gen->interpolation_info;
    uint32_t bit = 1u << location;
    bool centroid, sample;

    if (!info)
        return VKD3DSIM_LINEAR;
    if (info->flat_mask & bit)
        return VKD3DSIM_CONSTANT;
    centroid = info->centroid_mask & bit;
    sample = info->sample_mask & bit;
    if (info->noperspective_mask & bit)
        return sample ? VKD3DSIM_LINEAR_NOPERSPECTIVE_SAMPLE
                : centroid ? VKD3DSIM_LINEAR_NOPERSPECTIVE_CENTROID : VKD3DSIM_LINEAR_NOPERSPECTIVE;
    return sample ? VKD3DSIM_LINEAR_SAMPLE : centroid ? VKD3DSIM_LINEAR_CENTROID : VKD3DSIM_LINEAR;
}

static void wgsl_record_interpolation(struct wgsl_generator *gen, const struct vsir_signature_element *e)
{
    struct vkd3d_shader_varying_interpolation_info *info = gen->interpolation_info;
    uint32_t bit = 1u << e->target_location;

    if (!info || gen->program->shader_version.type != VKD3D_SHADER_TYPE_PIXEL)
        return;

    info->location_mask |= bit;
    if (e->component_type == VKD3D_SHADER_COMPONENT_INT)
        info->int_mask |= bit;
    else if (e->component_type != VKD3D_SHADER_COMPONENT_FLOAT)
        info->uint_mask |= bit;

    switch (e->component_type != VKD3D_SHADER_COMPONENT_FLOAT ? VKD3DSIM_CONSTANT : e->interpolation_mode)
    {
        case VKD3DSIM_CONSTANT:
            info->flat_mask |= bit;
            break;
        case VKD3DSIM_LINEAR_CENTROID:
            info->centroid_mask |= bit;
            break;
        case VKD3DSIM_LINEAR_SAMPLE:
            info->sample_mask |= bit;
            break;
        case VKD3DSIM_LINEAR_NOPERSPECTIVE:
            info->noperspective_mask |= bit;
            break;
        case VKD3DSIM_LINEAR_NOPERSPECTIVE_CENTROID:
            info->noperspective_mask |= bit;
            info->centroid_mask |= bit;
            break;
        case VKD3DSIM_LINEAR_NOPERSPECTIVE_SAMPLE:
            info->noperspective_mask |= bit;
            info->sample_mask |= bit;
            break;
        default:
            break;
    }
}

static void wgsl_print_interpolation(struct wgsl_generator *gen, const struct vsir_signature_element *e)
{
    enum vkd3d_shader_interpolation_mode mode = e->interpolation_mode;

    /* WGSL requires integer stage variables to be flat. */
    if (e->component_type != VKD3D_SHADER_COMPONENT_FLOAT)
    {
        vkd3d_string_buffer_printf(gen->buffer, " @interpolate(flat)");
        return;
    }

    /* An output's interpolation is the next stage's, which must match. */
    if (gen->program->shader_version.type != VKD3D_SHADER_TYPE_PIXEL)
        mode = wgsl_next_stage_interpolation(gen, e->target_location);

    switch (mode)
    {
        case VKD3DSIM_LINEAR:
        case VKD3DSIM_NONE:
            break;
        case VKD3DSIM_CONSTANT:
            vkd3d_string_buffer_printf(gen->buffer, " @interpolate(flat)");
            break;
        case VKD3DSIM_LINEAR_CENTROID:
            vkd3d_string_buffer_printf(gen->buffer, " @interpolate(perspective, centroid)");
            break;
        case VKD3DSIM_LINEAR_SAMPLE:
            vkd3d_string_buffer_printf(gen->buffer, " @interpolate(perspective, sample)");
            break;
        case VKD3DSIM_LINEAR_NOPERSPECTIVE:
            vkd3d_string_buffer_printf(gen->buffer, " @interpolate(linear)");
            break;
        case VKD3DSIM_LINEAR_NOPERSPECTIVE_CENTROID:
            vkd3d_string_buffer_printf(gen->buffer, " @interpolate(linear, centroid)");
            break;
        case VKD3DSIM_LINEAR_NOPERSPECTIVE_SAMPLE:
            vkd3d_string_buffer_printf(gen->buffer, " @interpolate(linear, sample)");
            break;
        default:
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                    "Internal compiler error: Unhandled interpolation mode %#x.", e->interpolation_mode);
            break;
    }
}

/* A stage variable: the varyings at one location, which several signature
 * elements may share (TEXCOORD0.xy with TEXCOORD1.zw). */
static const struct vsir_signature_element *wgsl_location_element(const struct vsir_signature *signature,
        unsigned int location)
{
    const struct vsir_signature_element *e;
    unsigned int i;

    for (i = 0; i < signature->element_count; ++i)
    {
        e = &signature->elements[i];
        if (e->sysval_semantic != VKD3D_SHADER_SV_NONE && e->sysval_semantic != VKD3D_SHADER_SV_TARGET)
            continue;
        if (e->target_location == location && e->register_count == 1)
            return e;
    }
    return NULL;
}

static bool wgsl_is_depth_output(const struct vsir_signature_element *e)
{
    return e->sysval_semantic == VKD3D_SHADER_SV_DEPTH
            || e->sysval_semantic == VKD3D_SHADER_SV_DEPTH_GREATER_EQUAL
            || e->sysval_semantic == VKD3D_SHADER_SV_DEPTH_LESS_EQUAL;
}

/* The integer type a float vertex input is declared as, fed an integer
 * attribute, or NULL. */
static const char *wgsl_vertex_input_conversion(const struct wgsl_generator *gen,
        const struct vsir_signature_element *e)
{
    uint32_t bit = 1u << e->target_location;

    if (!gen->target_info || gen->program->shader_version.type != VKD3D_SHADER_TYPE_VERTEX
            || e->component_type != VKD3D_SHADER_COMPONENT_FLOAT || e->target_location >= 32)
        return NULL;
    if (gen->target_info->vertex_uint_to_float_mask & bit)
        return "u32";
    if (gen->target_info->vertex_sint_to_float_mask & bit)
        return "i32";
    return NULL;
}

static void wgsl_generate_input_struct_declarations(struct wgsl_generator *gen)
{
    const struct vsir_signature *signature = &gen->program->input_signature;
    enum vkd3d_shader_type type = gen->program->shader_version.type;
    struct vkd3d_string_buffer *buffer = gen->buffer;
    bool locations[MAX_IO_REG_COUNT] = {0};
    const struct vsir_signature_element *e;
    bool position = false;
    bool empty = true;
    unsigned int i;

    vkd3d_string_buffer_printf(buffer, "struct vkd3d_%s_in\n{\n", gen->prefix);

    for (i = 0; i < signature->element_count; ++i)
    {
        e = &signature->elements[i];

        if (!wgsl_check_io_element(gen, e, locations))
            continue;

        switch (e->sysval_semantic)
        {
            case VKD3D_SHADER_SV_NONE:
                break;

            case VKD3D_SHADER_SV_POSITION:
                if (type != VKD3D_SHADER_TYPE_PIXEL)
                {
                    /* A vertex shader's position input is an attribute. */
                    break;
                }
                if (!position)
                    vkd3d_string_buffer_printf(buffer, "    @builtin(position) position: vec4<f32>,\n");
                position = true;
                empty = false;
                continue;

            case VKD3D_SHADER_SV_VERTEX_ID:
                gen->read_vertex_id = true;
                vkd3d_string_buffer_printf(buffer, "    @builtin(vertex_index) vertex_id: u32,\n");
                empty = false;
                continue;

            case VKD3D_SHADER_SV_INSTANCE_ID:
                gen->read_instance_id = true;
                vkd3d_string_buffer_printf(buffer, "    @builtin(instance_index) instance_id: u32,\n");
                empty = false;
                continue;

            case VKD3D_SHADER_SV_IS_FRONT_FACE:
                vkd3d_string_buffer_printf(buffer, "    @builtin(front_facing) is_front_face: bool,\n");
                empty = false;
                continue;

            case VKD3D_SHADER_SV_SAMPLE_INDEX:
                vkd3d_string_buffer_printf(buffer, "    @builtin(sample_index) sample_index: u32,\n");
                empty = false;
                continue;

            default:
                wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                        "Unsupported system value %#x input.", e->sysval_semantic);
                continue;
        }

        /* WebGPU requires every fragment input be written by the vertex
         * stage; one the shader never reads is left out. */
        if (type == VKD3D_SHADER_TYPE_PIXEL && !e->used_mask)
            continue;

        /* One stage variable per location; the first element there gives
         * its type and interpolation. */
        if (locations[e->target_location])
            continue;
        locations[e->target_location] = true;

        vkd3d_string_buffer_printf(buffer, "    @location(%u)", e->target_location);
        if (type == VKD3D_SHADER_TYPE_PIXEL)
        {
            wgsl_print_interpolation(gen, e);
            wgsl_record_interpolation(gen, e);
        }
        vkd3d_string_buffer_printf(buffer, " in_%u: ", e->target_location);
        if (wgsl_vertex_input_conversion(gen, e))
            vkd3d_string_buffer_printf(buffer, "vec4<%s>", wgsl_vertex_input_conversion(gen, e));
        else
            wgsl_print_io_type(buffer, gen, e);
        vkd3d_string_buffer_printf(buffer, ",\n");
        empty = false;
    }

    /* WGSL structures may not be empty. */
    if (empty)
        vkd3d_string_buffer_printf(buffer, "    @builtin(%s) unused: %s,\n",
                type == VKD3D_SHADER_TYPE_PIXEL ? "position" : "vertex_index",
                type == VKD3D_SHADER_TYPE_PIXEL ? "vec4<f32>" : "u32");

    vkd3d_string_buffer_printf(buffer, "}\n\n");
}

/* The clip distances before signature element "end" (all of them, for
 * the element count): numbered in element order, then by row and
 * component. */
static unsigned int wgsl_clip_distance_base(const struct wgsl_generator *gen, unsigned int end)
{
    const struct vsir_signature *signature = &gen->program->output_signature;
    unsigned int i, count = 0;

    if (gen->program->shader_version.type != VKD3D_SHADER_TYPE_VERTEX)
        return 0;
    for (i = 0; i < min(end, signature->element_count); ++i)
    {
        const struct vsir_signature_element *e = &signature->elements[i];

        if (e->sysval_semantic == VKD3D_SHADER_SV_CLIP_DISTANCE)
            count += max(e->register_count, 1) * vkd3d_popcount(e->mask);
    }
    return count;
}

/* The size of the clip_distances array. */
static unsigned int wgsl_clip_distance_count(const struct wgsl_generator *gen)
{
    return min(wgsl_clip_distance_base(gen, ~0u), 8);
}

static void wgsl_generate_output_struct_declarations(struct wgsl_generator *gen)
{
    unsigned int clip_distance_count = wgsl_clip_distance_count(gen);
    const struct vsir_signature *signature = &gen->program->output_signature;
    enum vkd3d_shader_type type = gen->program->shader_version.type;
    struct vkd3d_string_buffer *buffer = gen->buffer;
    bool locations[MAX_IO_REG_COUNT] = {0};
    const struct vsir_signature_element *e;
    unsigned int i;

    vkd3d_string_buffer_printf(buffer, "struct vkd3d_%s_out\n{\n", gen->prefix);

    /* A vertex shader always has a position, whether or not the next
     * stage reads it. */
    if (type == VKD3D_SHADER_TYPE_VERTEX)
        vkd3d_string_buffer_printf(buffer, "    @builtin(position) position: vec4<f32>,\n");

    for (i = 0; i < signature->element_count; ++i)
    {
        e = &signature->elements[i];

        if (wgsl_is_depth_output(e) || e->sysval_semantic == VKD3D_SHADER_SV_POSITION)
            continue;
        /* WebGPU has clip distances (in an array, below), but not cull
         * distances. */
        if (e->sysval_semantic == VKD3D_SHADER_SV_CLIP_DISTANCE
                || e->sysval_semantic == VKD3D_SHADER_SV_CULL_DISTANCE)
            continue;
        if (!wgsl_check_io_element(gen, e, locations))
            continue;

        if (e->sysval_semantic != VKD3D_SHADER_SV_NONE && e->sysval_semantic != VKD3D_SHADER_SV_TARGET)
        {
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                    "Unsupported system value %#x output.", e->sysval_semantic);
            continue;
        }

        if (locations[e->target_location])
            continue;
        locations[e->target_location] = true;

        vkd3d_string_buffer_printf(buffer, "    @location(%u)", e->target_location);
        if (type == VKD3D_SHADER_TYPE_VERTEX)
            wgsl_print_interpolation(gen, e);
        vkd3d_string_buffer_printf(buffer, " out_%u: ", e->target_location);
        wgsl_print_io_type(buffer, gen, e);
        vkd3d_string_buffer_printf(buffer, ",\n");
    }

    /* The pixel shader's inputs that this shader does not write are zero. */
    if (type == VKD3D_SHADER_TYPE_VERTEX && gen->interpolation_info)
    {
        const struct vkd3d_shader_varying_interpolation_info *info = gen->interpolation_info;
        static const char *const interpolation[] =
        {
            [VKD3DSIM_CONSTANT] = " @interpolate(flat)",
            [VKD3DSIM_LINEAR_CENTROID] = " @interpolate(perspective, centroid)",
            [VKD3DSIM_LINEAR_SAMPLE] = " @interpolate(perspective, sample)",
            [VKD3DSIM_LINEAR_NOPERSPECTIVE] = " @interpolate(linear)",
            [VKD3DSIM_LINEAR_NOPERSPECTIVE_CENTROID] = " @interpolate(linear, centroid)",
            [VKD3DSIM_LINEAR_NOPERSPECTIVE_SAMPLE] = " @interpolate(linear, sample)",
        };
        enum vkd3d_shader_interpolation_mode mode;
        uint32_t bit;

        for (i = 0; i < MAX_IO_REG_COUNT; ++i)
        {
            bit = 1u << i;
            if (!(info->location_mask & bit) || locations[i])
                continue;
            mode = (info->int_mask | info->uint_mask) & bit ? VKD3DSIM_CONSTANT
                    : wgsl_next_stage_interpolation(gen, i);
            vkd3d_string_buffer_printf(buffer, "    @location(%u)%s out_%u: vec4<%s>,\n", i,
                    mode < ARRAY_SIZE(interpolation) && interpolation[mode] ? interpolation[mode] : "", i,
                    info->int_mask & bit ? "i32" : info->uint_mask & bit ? "u32" : "f32");
        }
    }

    if (clip_distance_count)
        vkd3d_string_buffer_printf(buffer, "    @builtin(clip_distances) clip_distances: array<f32, %u>,\n",
                clip_distance_count);

    if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_DEPTHOUT))
        vkd3d_string_buffer_printf(buffer, "    @builtin(frag_depth) depth: f32,\n");

    if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_SAMPLEMASK))
        vkd3d_string_buffer_printf(buffer, "    @builtin(sample_mask) mask: u32,\n");

    if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_OUTSTENCILREF))
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                "Stencil reference export is not supported.");

    vkd3d_string_buffer_printf(buffer, "}\n\n");
}

static void wgsl_generate_immediate_constant_buffers(struct wgsl_generator *gen)
{
    const struct vkd3d_shader_immediate_constant_buffer *icb;
    size_t i, j;

    for (i = 0; i < gen->program->icb_count; ++i)
    {
        icb = gen->program->icbs[i];

        if (data_type_is_64_bit(icb->data_type))
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                    "Immediate constant buffer %zu has unsupported data type \"%s\" (%#x).",
                    i, vsir_data_type_get_name(icb->data_type, "<unknown>"), icb->data_type);

        vkd3d_string_buffer_printf(gen->buffer, "var<private> icb%u: array<vec4<u32>, %u>",
                icb->register_idx, icb->element_count);
        if (icb->is_null || data_type_is_64_bit(icb->data_type))
        {
            vkd3d_string_buffer_printf(gen->buffer, ";\n\n");
            continue;
        }

        vkd3d_string_buffer_printf(gen->buffer, " = array<vec4<u32>, %u>(\n", icb->element_count);
        for (j = 0; j < icb->element_count; ++j)
        {
            if (icb->component_count == 4)
                vkd3d_string_buffer_printf(gen->buffer, "    vec4<u32>(%#xu, %#xu, %#xu, %#xu),\n",
                        icb->data[4 * j + 0], icb->data[4 * j + 1], icb->data[4 * j + 2], icb->data[4 * j + 3]);
            else
                vkd3d_string_buffer_printf(gen->buffer, "    vec4<u32>(%#xu, 0u, 0u, 0u),\n", icb->data[j]);
        }
        vkd3d_string_buffer_printf(gen->buffer, ");\n\n");
    }
}

static void wgsl_texture_type(struct vkd3d_string_buffer *buffer, struct wgsl_generator *gen,
        const struct wgsl_resource_type_info *info, enum vsir_data_type data_type, bool depth)
{
    if (depth)
    {
        if (!info->depth_suffix)
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                    "Comparison sampling of a \"%s\" texture is not supported.", info->type_suffix);
        vkd3d_string_buffer_printf(buffer, "texture_depth_%s", info->depth_suffix ? info->depth_suffix : "2d");
        return;
    }
    vkd3d_string_buffer_printf(buffer, "texture_%s<%s>", info->type_suffix, wgsl_scalar_name(gen, data_type));
}

/* Whether any instruction compares against SRV "id", and whether any
 * samples or loads it otherwise. */
static void wgsl_srv_usage(struct wgsl_generator *gen, unsigned int id, bool *compare, bool *plain)
{
    struct vsir_program_iterator it = vsir_program_iterator(&gen->program->instructions);
    const struct vsir_instruction *ins;
    const struct vsir_src_operand *resource;

    *compare = *plain = false;
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
    {
        bool c;

        switch (ins->opcode)
        {
            case VSIR_OP_GATHER4_C:
            case VSIR_OP_SAMPLE_C:
            case VSIR_OP_SAMPLE_C_LZ:
                c = true;
                resource = &ins->src[1];
                break;
            case VSIR_OP_GATHER4_PO:
                c = false;
                resource = &ins->src[2];
                break;
            case VSIR_OP_GATHER4:
            case VSIR_OP_SAMPLE:
            case VSIR_OP_SAMPLE_B:
            case VSIR_OP_SAMPLE_GRAD:
            case VSIR_OP_SAMPLE_LOD:
            case VSIR_OP_LD:
            case VSIR_OP_LD2DMS:
                c = false;
                resource = &ins->src[1];
                break;
            default:
                continue;
        }
        if (resource->reg.type != VSIR_REGISTER_RESOURCE || resource->reg.idx[0].offset != id)
            continue;
        if (c)
            *compare = true;
        else
            *plain = true;
    }
}

static void wgsl_add_binding(struct wgsl_generator *gen, unsigned int group, unsigned int binding,
        enum vkd3d_shader_wgsl_binding_type type, const struct vsir_descriptor *d, unsigned int idx,
        const char *format)
{
    struct vkd3d_shader_wgsl_binding_info *info = gen->binding_info;
    struct vkd3d_shader_wgsl_binding *b;

    if (!info)
        return;
    if (!vkd3d_array_reserve((void **)&info->bindings, &gen->binding_capacity,
            info->binding_count + 1, sizeof(*info->bindings)))
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL, "Out of memory.");
        return;
    }
    b = &info->bindings[info->binding_count++];
    memset(b, 0, sizeof(*b));
    b->group = group;
    b->binding = binding;
    b->type = type;
    b->format = format;
    if (!d)
    {
        b->descriptor_type = VKD3D_SHADER_DESCRIPTOR_TYPE_CBV;
        b->register_space = ~0u;
        b->register_index = ~0u;
        return;
    }
    b->descriptor_type = d->type;
    b->register_space = d->register_space;
    b->register_index = idx;
    b->resource_type = wgsl_effective_resource_type(d->resource_type, d->sample_count);
    switch (wgsl_scalar_type(d->resource_data_type))
    {
        case VSIR_DATA_I32:
            b->resource_data_type = VKD3D_SHADER_RESOURCE_DATA_INT;
            break;
        case VSIR_DATA_U32:
            b->resource_data_type = VKD3D_SHADER_RESOURCE_DATA_UINT;
            break;
        default:
            b->resource_data_type = VKD3D_SHADER_RESOURCE_DATA_FLOAT;
            break;
    }
}

/* Buffer shader parameters are read from a uniform array of vec4<u32> per
 * buffer, which is declared whether or not the shader reads it, since the
 * caller binds it either way. */
static void wgsl_generate_parameter_buffer_declarations(struct wgsl_generator *gen)
{
    const struct vkd3d_shader_parameter1 *p, *q;
    unsigned int i, j, end, size;
    bool seen;

    for (i = 0; i < gen->program->parameter_count; ++i)
    {
        p = &gen->program->parameters[i];
        if (p->type != VKD3D_SHADER_PARAMETER_TYPE_BUFFER)
            continue;

        for (j = 0, seen = false, end = 0; j < gen->program->parameter_count; ++j)
        {
            q = &gen->program->parameters[j];
            if (q->type != VKD3D_SHADER_PARAMETER_TYPE_BUFFER
                    || q->u.buffer.set != p->u.buffer.set || q->u.buffer.binding != p->u.buffer.binding)
                continue;
            if (j < i)
            {
                seen = true;
                break;
            }
            size = q->data_type == VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32_VEC4 ? 16 : 4;
            end = max(end, q->u.buffer.offset + size);
        }
        if (seen)
            continue;

        vkd3d_string_buffer_printf(gen->buffer,
                "@group(%u) @binding(%u) var<uniform> vkd3d_param_%u_%u: array<vec4<u32>, %u>;\n\n",
                p->u.buffer.set, p->u.buffer.binding, p->u.buffer.set, p->u.buffer.binding, (end + 15) / 16);
        wgsl_add_binding(gen, p->u.buffer.set, p->u.buffer.binding,
                VKD3D_SHADER_WGSL_BINDING_UNIFORM_BUFFER, NULL, 0, NULL);
    }
}

static void wgsl_generate_descriptor_declarations(struct wgsl_generator *gen)
{
    const struct vsir_descriptor_info *info = &gen->program->descriptors;
    const struct wgsl_resource_type_info *type_info;
    const struct vsir_descriptor *d;
    unsigned int group, binding, k, count;
    bool compare, plain;
    size_t i;

    for (i = 0; i < info->count; ++i)
    {
        d = &info->descriptors[i];
        count = d->count == ~0u ? 1 : d->count;

        for (k = 0; k < count; ++k)
        {
            unsigned int idx = d->register_index + k;

            if (!wgsl_get_binding(gen, d, idx, wgsl_descriptor_binding_flags(d), &group, &binding))
            {
                wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_BINDING_NOT_FOUND,
                        "No descriptor binding specified for %s %u (index %u, space %u).",
                        wgsl_descriptor_prefix(d->type), d->register_id, idx, d->register_space);
                continue;
            }

            switch (d->type)
            {
                case VKD3D_SHADER_DESCRIPTOR_TYPE_CBV:
                    /* A constant buffer is an array of vec4<u32>, which has
                     * the 16-byte stride a uniform array needs. */
                    vkd3d_string_buffer_printf(gen->buffer, "struct vkd3d_cb%u_%u\n{\n"
                            "    v: array<vec4<u32>, %u>,\n}\n", d->register_id, idx,
                            d->buffer_size ? (d->buffer_size + 15) / 16 : 4096);
                    vkd3d_string_buffer_printf(gen->buffer, "@group(%u) @binding(%u) var<uniform> cb%u_%u: vkd3d_cb%u_%u;\n\n",
                            group, binding, d->register_id, idx, d->register_id, idx);
                    wgsl_add_binding(gen, group, binding, VKD3D_SHADER_WGSL_BINDING_UNIFORM_BUFFER, d, idx, NULL);
                    break;

                case VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER:
                    vkd3d_string_buffer_printf(gen->buffer, "@group(%u) @binding(%u) var s%u_%u: %s;\n\n",
                            group, binding, d->register_id, idx,
                            d->flags & VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_SAMPLER_COMPARISON_MODE
                            ? "sampler_comparison" : "sampler");
                    wgsl_add_binding(gen, group, binding,
                            d->flags & VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_SAMPLER_COMPARISON_MODE
                            ? VKD3D_SHADER_WGSL_BINDING_COMPARISON_SAMPLER : VKD3D_SHADER_WGSL_BINDING_SAMPLER,
                            d, idx, NULL);
                    break;

                case VKD3D_SHADER_DESCRIPTOR_TYPE_SRV:
                    if (!(type_info = wgsl_get_resource_type_info(
                            wgsl_effective_resource_type(d->resource_type, d->sample_count))))
                    {
                        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                                "Internal compiler error: Unhandled resource type %#x.", d->resource_type);
                        break;
                    }
                    if (d->resource_type == VKD3D_SHADER_RESOURCE_BUFFER)
                    {
                        if (!(d->flags & VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_RAW_BUFFER) && !d->structure_stride)
                        {
                            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                                    "Typed buffer shader resource views are not supported.");
                            break;
                        }
                        vkd3d_string_buffer_printf(gen->buffer,
                                "@group(%u) @binding(%u) var<storage, read> t%u_%u: array<u32>;\n\n",
                                group, binding, d->register_id, idx);
                        wgsl_add_binding(gen, group, binding,
                                VKD3D_SHADER_WGSL_BINDING_READ_ONLY_STORAGE_BUFFER, d, idx, NULL);
                        break;
                    }
                    wgsl_srv_usage(gen, d->register_id, &compare, &plain);
                    /* Declared for any use that is not a comparison:
                     * resinfo, sample_info, or none at all. */
                    plain = plain || !compare;
                    /* WGSL compares only against a depth texture, which
                     * cannot also be sampled plainly; a texture compared
                     * against has a depth view 128 bindings past its own
                     * (beyond D3D11's 128 resource slots), whether or not
                     * it is also sampled plainly. */
                    if (plain)
                    {
                        vkd3d_string_buffer_printf(gen->buffer, "@group(%u) @binding(%u) var t%u_%u: ",
                                group, binding, d->register_id, idx);
                        wgsl_texture_type(gen->buffer, gen, type_info,
                                wgsl_scalar_type(d->resource_data_type), false);
                        vkd3d_string_buffer_printf(gen->buffer, ";\n\n");
                        wgsl_add_binding(gen, group, binding, VKD3D_SHADER_WGSL_BINDING_TEXTURE, d, idx, NULL);
                    }
                    if (compare)
                    {
                        vkd3d_string_buffer_printf(gen->buffer, "@group(%u) @binding(%u) var t%u_%u_depth: ",
                                group, binding + 128, d->register_id, idx);
                        wgsl_texture_type(gen->buffer, gen, type_info, VSIR_DATA_F32, true);
                        vkd3d_string_buffer_printf(gen->buffer, ";\n\n");
                        wgsl_add_binding(gen, group, binding + 128,
                                VKD3D_SHADER_WGSL_BINDING_DEPTH_TEXTURE, d, idx, NULL);
                    }
                    break;

                case VKD3D_SHADER_DESCRIPTOR_TYPE_UAV:
                    if (d->resource_type == VKD3D_SHADER_RESOURCE_BUFFER)
                    {
                        if (!(d->flags & VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_RAW_BUFFER) && !d->structure_stride)
                        {
                            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                                    "Typed buffer unordered access views are not supported.");
                            break;
                        }
                        vkd3d_string_buffer_printf(gen->buffer,
                                "@group(%u) @binding(%u) var<storage, read_write> u%u_%u: array<%s>;\n\n",
                                group, binding, d->register_id, idx,
                                wgsl_is_atomic_target(gen, VSIR_REGISTER_UAV, d->register_id) ? "atomic<u32>" : "u32");
                        wgsl_add_binding(gen, group, binding, VKD3D_SHADER_WGSL_BINDING_STORAGE_BUFFER, d, idx, NULL);
                        break;
                    }
                    if (!wgsl_storage_texture_suffix(d->resource_type))
                    {
                        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                                "Unordered access views of type %#x are not supported.", d->resource_type);
                        break;
                    }
                    vkd3d_string_buffer_printf(gen->buffer,
                            "@group(%u) @binding(%u) var u%u_%u: texture_storage_%s<%s, %s>;\n\n",
                            group, binding, d->register_id, idx, wgsl_storage_texture_suffix(d->resource_type),
                            wgsl_uav_format(gen, d, idx),
                            d->flags & VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_READ ? "read_write" : "write");
                    wgsl_add_binding(gen, group, binding, d->flags & VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_READ
                            ? VKD3D_SHADER_WGSL_BINDING_READ_WRITE_STORAGE_TEXTURE
                            : VKD3D_SHADER_WGSL_BINDING_WRITE_ONLY_STORAGE_TEXTURE,
                            d, idx, wgsl_uav_format(gen, d, idx));
                    break;

                default:
                    wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                            "Unsupported descriptor type %#x.", d->type);
                    break;
            }
        }
    }

    for (i = 0; i < gen->program->tgsm_count; ++i)
    {
        const struct vsir_tgsm *t = &gen->program->tgsms[i];

        vkd3d_string_buffer_printf(gen->buffer, "var<workgroup> g%u: array<%s, %zu>;\n\n", t->id,
                wgsl_is_atomic_target(gen, VSIR_REGISTER_GROUPSHAREDMEM, t->id) ? "atomic<u32>" : "u32",
                (t->byte_count + 3) / 4);
    }
}

/* The bitcast that makes a stage variable's components u32 register bits. */
static const char *wgsl_io_to_u32(const struct vsir_signature_element *e, unsigned int count)
{
    if (e->component_type == VKD3D_SHADER_COMPONENT_FLOAT || e->component_type == VKD3D_SHADER_COMPONENT_INT)
        return count == 1 ? "bitcast<u32>" : count == 2 ? "bitcast<vec2<u32>>"
                : count == 3 ? "bitcast<vec3<u32>>" : "bitcast<vec4<u32>>";
    return "";
}

static void wgsl_generate_entrypoint(struct wgsl_generator *gen)
{
    const struct vsir_signature *input = &gen->program->input_signature;
    const struct vsir_signature *output = &gen->program->output_signature;
    enum vkd3d_shader_type type = gen->program->shader_version.type;
    struct vkd3d_string_buffer *buffer = gen->buffer;
    const struct vsir_signature_element *e, *l;
    const char components[] = "xyzw";
    unsigned int i, c, j;

    switch (type)
    {
        case VKD3D_SHADER_TYPE_VERTEX:
            vkd3d_string_buffer_printf(buffer, "@vertex\n");
            break;
        case VKD3D_SHADER_TYPE_PIXEL:
            vkd3d_string_buffer_printf(buffer, "@fragment\n");
            break;
        case VKD3D_SHADER_TYPE_COMPUTE:
            vkd3d_string_buffer_printf(buffer, "@compute @workgroup_size(%u, %u, %u)\n",
                    gen->program->thread_group_size.x, gen->program->thread_group_size.y,
                    gen->program->thread_group_size.z);
            break;
        default:
            wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                    "Internal compiler error: Unhandled shader type %#x.", type);
            return;
    }

    vkd3d_string_buffer_printf(buffer, "fn shader_entry(");
    if (type == VKD3D_SHADER_TYPE_COMPUTE)
    {
        vkd3d_string_buffer_printf(buffer,
                "@builtin(global_invocation_id) thread_id: vec3<u32>,\n"
                "        @builtin(workgroup_id) thread_group_id: vec3<u32>,\n"
                "        @builtin(local_invocation_id) local_thread_id: vec3<u32>,\n"
                "        @builtin(local_invocation_index) local_thread_index: u32)\n{\n");
        if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_THREADID))
            vkd3d_string_buffer_printf(buffer, "    v_thread_id = vec4<u32>(thread_id, 0u);\n");
        if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_THREADGROUPID))
            vkd3d_string_buffer_printf(buffer, "    v_thread_group_id = vec4<u32>(thread_group_id, 0u);\n");
        if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_LOCALTHREADID))
            vkd3d_string_buffer_printf(buffer, "    v_local_thread_id = vec4<u32>(local_thread_id, 0u);\n");
        if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_LOCALTHREADINDEX))
            vkd3d_string_buffer_printf(buffer,
                    "    v_local_thread_index = vec4<u32>(local_thread_index, 0u, 0u, 0u);\n");
        vkd3d_string_buffer_printf(buffer, "    %s_main();\n}\n", gen->prefix);
        return;
    }

    vkd3d_string_buffer_printf(buffer, "input: vkd3d_%s_in) -> vkd3d_%s_out\n{\n", gen->prefix, gen->prefix);
    vkd3d_string_buffer_printf(buffer, "    var output: vkd3d_%s_out;\n\n", gen->prefix);

    /* Normalised I/O registers are indexed by signature element, and an
     * input's components start at x whatever its mask. */
    for (i = 0; i < input->element_count; ++i)
    {
        const char *value = NULL;
        char source[96];

        e = &input->elements[i];

        if (e->target_location == SIGNATURE_TARGET_LOCATION_UNUSED || e->register_count > 1
                || e->target_location >= MAX_IO_REG_COUNT)
            continue;

        switch (e->sysval_semantic)
        {
            case VKD3D_SHADER_SV_POSITION:
                if (type == VKD3D_SHADER_TYPE_PIXEL)
                {
                    /* WGSL's position.w, like Metal's and Vulkan's, is 1/w. */
                    value = "bitcast<vec4<u32>>(vec4<f32>(input.position.xyz, 1.0f / input.position.w))";
                    break;
                }
                /* fall through */
            case VKD3D_SHADER_SV_NONE:
                break;

            case VKD3D_SHADER_SV_VERTEX_ID:
                value = "vec4<u32>(input.vertex_id, 0u, 0u, 0u)";
                break;

            case VKD3D_SHADER_SV_INSTANCE_ID:
                value = "vec4<u32>(input.instance_id, 0u, 0u, 0u)";
                break;

            case VKD3D_SHADER_SV_IS_FRONT_FACE:
                value = "vec4<u32>(select(0u, 0xffffffffu, input.is_front_face), 0u, 0u, 0u)";
                break;

            case VKD3D_SHADER_SV_SAMPLE_INDEX:
                value = "vec4<u32>(input.sample_index, 0u, 0u, 0u)";
                break;

            default:
                continue;
        }

        if (value)
        {
            vkd3d_string_buffer_printf(buffer, "    v[%u] = %s;\n", i, value);
            continue;
        }

        if (type == VKD3D_SHADER_TYPE_PIXEL && !e->used_mask)
            continue;

        /* A varying: its components of the location's variable, from x. */
        l = wgsl_location_element(input, e->target_location);
        if (!l)
            l = e;
        snprintf(source, sizeof(source), "input.in_%u", e->target_location);
        for (c = 0, j = 0; c < VKD3D_VEC4_SIZE; ++c)
        {
            if (!(e->mask & (VKD3DSP_WRITEMASK_0 << c)))
                continue;
            if (wgsl_vertex_input_conversion(gen, l))
                vkd3d_string_buffer_printf(buffer, "    v[%u].%c = bitcast<u32>(f32(%s.%c));\n", i,
                        components[j++], source, components[c]);
            else
                vkd3d_string_buffer_printf(buffer, "    v[%u].%c = %s(%s.%c);\n", i, components[j++],
                        wgsl_io_to_u32(l, 1), source, components[c]);
        }
    }

    vkd3d_string_buffer_printf(buffer, "\n    %s_main();\n\n", gen->prefix);

    /* An output's components are where its mask puts them. */
    for (i = 0; i < output->element_count; ++i)
    {
        const char *to_type;

        e = &output->elements[i];

        if (e->sysval_semantic == VKD3D_SHADER_SV_CLIP_DISTANCE && type == VKD3D_SHADER_TYPE_VERTEX)
        {
            unsigned int row, rows = max(e->register_count, 1), d, k;

            for (row = 0; row < rows; ++row)
            {
                for (c = 0, k = 0; c < VKD3D_VEC4_SIZE; ++c)
                {
                    if (!(e->mask & (VKD3DSP_WRITEMASK_0 << c)))
                        continue;
                    d = wgsl_clip_distance_base(gen, i) + row * vkd3d_popcount(e->mask) + k++;
                    if (d >= 8)
                        continue;
                    if (rows > 1)
                        vkd3d_string_buffer_printf(buffer, "    output.clip_distances[%u] = bitcast<f32>(o_e%u[%u].%c);\n",
                                d, i, row, components[c]);
                    else
                        vkd3d_string_buffer_printf(buffer, "    output.clip_distances[%u] = bitcast<f32>(o[%u].%c);\n",
                                d, i, components[c]);
                }
            }
            continue;
        }

        if (e->register_count > 1)
            continue;

        if (e->sysval_semantic == VKD3D_SHADER_SV_POSITION && type == VKD3D_SHADER_TYPE_VERTEX)
        {
            vkd3d_string_buffer_printf(buffer, "    output.position = bitcast<vec4<f32>>(o[%u]);\n", i);
            continue;
        }

        if (e->sysval_semantic != VKD3D_SHADER_SV_NONE && e->sysval_semantic != VKD3D_SHADER_SV_TARGET)
            continue;
        if (e->target_location == SIGNATURE_TARGET_LOCATION_UNUSED || e->target_location >= MAX_IO_REG_COUNT)
            continue;

        if (!(l = wgsl_location_element(output, e->target_location)))
            l = e;
        switch (l->component_type)
        {
            case VKD3D_SHADER_COMPONENT_FLOAT:
                to_type = "bitcast<f32>";
                break;
            case VKD3D_SHADER_COMPONENT_INT:
                to_type = "bitcast<i32>";
                break;
            default:
                to_type = "";
                break;
        }
        for (c = 0; c < VKD3D_VEC4_SIZE; ++c)
        {
            if (!(e->mask & (VKD3DSP_WRITEMASK_0 << c)))
                continue;
            vkd3d_string_buffer_printf(buffer, "    output.out_%u.%c = %s(o[%u].%c);\n",
                    e->target_location, components[c], to_type, i, components[c]);
        }
    }

    if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_DEPTHOUT))
        vkd3d_string_buffer_printf(buffer, "    output.depth = o_depth;\n");
    if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_SAMPLEMASK))
        vkd3d_string_buffer_printf(buffer, "    output.mask = o_mask;\n");

    vkd3d_string_buffer_printf(buffer, "    return output;\n}\n");
}

static int wgsl_generator_generate(struct wgsl_generator *gen, struct vkd3d_shader_code *out)
{
    enum vsir_global_flags flags = gen->program->global_flags;
    enum vkd3d_shader_type type = gen->program->shader_version.type;
    struct vsir_program_iterator it;
    struct vsir_instruction *ins;

    static const uint64_t ignored_flags = VKD3DSGF_REFACTORING_ALLOWED
            | VKD3DSGF_FORCE_EARLY_DEPTH_STENCIL
            | VKD3DSGF_BIND_FOR_DURATION
            | VKD3DSGF_ENABLE_STENCIL_REF
            | VKD3DSGF_ENABLE_UP_TO_64_UAVS
            | VKD3DSGF_SKIP_OPTIMIZATION
            | VKD3DSGF_ENABLE_MINIMUM_PRECISION
            | VKD3DSGF_ENABLE_RAW_AND_STRUCTURED_BUFFERS;

    MESSAGE("Generating a WGSL shader. This is unsupported; you get to keep all the pieces if it breaks.\n");

    vkd3d_string_buffer_printf(gen->buffer, "/* Generated by %s. */\n\n", vkd3d_shader_get_version(NULL, NULL));
    if (wgsl_clip_distance_count(gen))
        vkd3d_string_buffer_printf(gen->buffer, "enable clip_distances;\n");
    vkd3d_string_buffer_printf(gen->buffer, "diagnostic(off, derivative_uniformity);\n\n");

    if (flags & ignored_flags)
    {
        TRACE("Ignoring global flags %#"PRIx64".\n", flags & ignored_flags);
        flags &= ~ignored_flags;
    }

    if (flags)
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_INTERNAL,
                "Internal compiler error: Unhandled global flags %#"PRIx64".", (uint64_t)flags);

    /* Read at run time, so an expression built on it is never const. */
    vkd3d_string_buffer_printf(gen->buffer, "var<private> vkd3d_zero: u32 = 0u;\n\n");

    if (type != VKD3D_SHADER_TYPE_COMPUTE)
    {
        wgsl_generate_input_struct_declarations(gen);
        wgsl_generate_output_struct_declarations(gen);
    }
    wgsl_generate_immediate_constant_buffers(gen);
    wgsl_generate_parameter_buffer_declarations(gen);
    wgsl_generate_descriptor_declarations(gen);

    if (gen->program->temp_count)
        vkd3d_string_buffer_printf(gen->buffer, "var<private> r: array<vec4<u32>, %u>;\n",
                gen->program->temp_count);
    if (type != VKD3D_SHADER_TYPE_COMPUTE)
    {
        unsigned int i;

        vkd3d_string_buffer_printf(gen->buffer, "var<private> v: array<vec4<u32>, %u>;\n", MAX_IO_REG_COUNT);
        vkd3d_string_buffer_printf(gen->buffer, "var<private> o: array<vec4<u32>, %u>;\n", MAX_IO_REG_COUNT);
        for (i = 0; i < gen->program->input_signature.element_count; ++i)
        {
            if (gen->program->input_signature.elements[i].register_count > 1)
                vkd3d_string_buffer_printf(gen->buffer, "var<private> v_e%u: array<vec4<u32>, %u>;\n",
                        i, gen->program->input_signature.elements[i].register_count);
        }
        for (i = 0; i < gen->program->output_signature.element_count; ++i)
        {
            if (gen->program->output_signature.elements[i].register_count > 1)
                vkd3d_string_buffer_printf(gen->buffer, "var<private> o_e%u: array<vec4<u32>, %u>;\n",
                        i, gen->program->output_signature.elements[i].register_count);
        }
    }
    if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_DEPTHOUT))
        vkd3d_string_buffer_printf(gen->buffer, "var<private> o_depth: f32;\n");
    if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_SAMPLEMASK))
        vkd3d_string_buffer_printf(gen->buffer, "var<private> o_mask: u32;\n");
    if (type == VKD3D_SHADER_TYPE_VERTEX)
        vkd3d_string_buffer_printf(gen->buffer, "var<private> o_point_size: f32;\n");
    if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_THREADID))
        vkd3d_string_buffer_printf(gen->buffer, "var<private> v_thread_id: vec4<u32>;\n");
    if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_THREADGROUPID))
        vkd3d_string_buffer_printf(gen->buffer, "var<private> v_thread_group_id: vec4<u32>;\n");
    if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_LOCALTHREADID))
        vkd3d_string_buffer_printf(gen->buffer, "var<private> v_local_thread_id: vec4<u32>;\n");
    if (bitmap_is_set(gen->program->io_dcls, VSIR_REGISTER_LOCALTHREADINDEX))
        vkd3d_string_buffer_printf(gen->buffer, "var<private> v_local_thread_index: vec4<u32>;\n");
    vkd3d_string_buffer_printf(gen->buffer, "\nfn %s_main()\n{\n", gen->prefix);

    ++gen->indent;
    it = vsir_program_iterator(&gen->program->instructions);
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
    {
        wgsl_handle_instruction(gen, ins);
    }
    --gen->indent;

    vkd3d_string_buffer_printf(gen->buffer, "}\n\n");

    wgsl_generate_entrypoint(gen);

    if (TRACE_ON())
        vkd3d_string_buffer_trace(gen->buffer);

    if (gen->failed)
        return VKD3D_ERROR_INVALID_SHADER;

    vkd3d_shader_code_from_string_buffer(out, gen->buffer);

    return VKD3D_OK;
}

static void wgsl_generator_cleanup(struct wgsl_generator *gen)
{
    vkd3d_free(gen->switches);
    vkd3d_string_buffer_release(&gen->string_buffers, gen->buffer);
    vkd3d_string_buffer_cache_cleanup(&gen->string_buffers);
}

static int wgsl_generator_init(struct wgsl_generator *gen, struct vsir_program *program,
        const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_message_context *message_context)
{
    enum vkd3d_shader_type type = program->shader_version.type;

    memset(gen, 0, sizeof(*gen));
    gen->program = program;
    vkd3d_string_buffer_cache_init(&gen->string_buffers);
    if (!(gen->buffer = vkd3d_string_buffer_get(&gen->string_buffers)))
    {
        vkd3d_string_buffer_cache_cleanup(&gen->string_buffers);
        return VKD3D_ERROR_OUT_OF_MEMORY;
    }
    gen->message_context = message_context;
    if (!(gen->prefix = wgsl_get_prefix(type)))
    {
        wgsl_compiler_error(gen, VKD3D_SHADER_ERROR_WGSL_UNSUPPORTED,
                "Shader type %#x is not supported by WGSL.", type);
        gen->prefix = "unknown";
    }
    gen->interface_info = vkd3d_find_struct(compile_info->next, INTERFACE_INFO);
    gen->target_info = vkd3d_find_struct(compile_info->next, WGSL_TARGET_INFO);
    if ((gen->binding_info = (struct vkd3d_shader_wgsl_binding_info *)vkd3d_find_struct(
            compile_info->next, WGSL_BINDING_INFO)))
    {
        gen->binding_info->bindings = NULL;
        gen->binding_info->binding_count = 0;
    }
    if ((gen->interpolation_info = (struct vkd3d_shader_varying_interpolation_info *)vkd3d_find_struct(
            compile_info->next, VARYING_INTERPOLATION_INFO)) && type == VKD3D_SHADER_TYPE_PIXEL)
    {
        gen->interpolation_info->flat_mask = 0;
        gen->interpolation_info->noperspective_mask = 0;
        gen->interpolation_info->centroid_mask = 0;
        gen->interpolation_info->sample_mask = 0;
        gen->interpolation_info->location_mask = 0;
        gen->interpolation_info->int_mask = 0;
        gen->interpolation_info->uint_mask = 0;
    }

    return VKD3D_OK;
}

int wgsl_compile(struct vsir_program *program, uint64_t config_flags,
        const struct vkd3d_shader_compile_info *compile_info, struct vkd3d_shader_code *out,
        struct vkd3d_shader_message_context *message_context)
{
    struct wgsl_generator generator;
    int ret;

    if ((ret = vsir_program_transform(program, config_flags, compile_info, message_context)) < 0)
        return ret;

    if ((ret = vsir_program_allocate_temp_registers(program, config_flags, compile_info, message_context)) < 0)
        return ret;

    VKD3D_ASSERT(program->normalisation_level == VSIR_NORMALISED_SM6);
    VKD3D_ASSERT(program->normalisation_flags.has_descriptor_info);
    VKD3D_ASSERT(program->normalisation_flags.has_no_modifiers);

    if ((ret = wgsl_generator_init(&generator, program, compile_info, message_context)) < 0)
        return ret;
    ret = wgsl_generator_generate(&generator, out);
    if (ret < 0 && generator.binding_info)
        vkd3d_shader_free_wgsl_binding_info(generator.binding_info);
    wgsl_generator_cleanup(&generator);

    return ret;
}

void vkd3d_shader_free_wgsl_binding_info(struct vkd3d_shader_wgsl_binding_info *info)
{
    TRACE("info %p.\n", info);

    vkd3d_free(info->bindings);
    info->bindings = NULL;
    info->binding_count = 0;
}
