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

/* Shaders for the WebGPU backend: vkd3d-shader's WGSL target. Like the
 * SPIR-V backend, every descriptor of the bound shaders gets a binding of
 * its own in one bind group, numbered in stage order; the WGSL target
 * reports what it declared for each, which makes the bind group layout. */

#include "wined3d_private.h"
#include "wined3d_wgpu.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d_shader);

static const struct wined3d_shader_backend_ops wgsl_shader_backend_wgpu;

#define MAX_SM1_INTER_STAGE_VARYINGS 12

struct shader_wgsl_resource_bindings
{
    struct vkd3d_shader_resource_binding *bindings;
    SIZE_T bindings_size, binding_count;

    size_t binding_base[WINED3D_SHADER_TYPE_COUNT];
    uint32_t next_binding;

    uint32_t ffp_ps_extra_binding, ffp_vs_extra_binding;
};

struct shader_wgsl_priv
{
    const struct wined3d_vertex_pipe_ops *vertex_pipe;
    const struct wined3d_fragment_pipe_ops *fragment_pipe;

    struct shader_wgsl_resource_bindings bindings;

    struct vkd3d_shader_compile_option compile_options[2];
};

struct shader_wgsl_compile_arguments
{
    union
    {
        struct
        {
            struct vkd3d_shader_varying_map varying_map[MAX_SM1_INTER_STAGE_VARYINGS];
            unsigned int varying_count;
            uint8_t clip_planes;
            uint8_t fog_source;
            /* The next stage's inputs, which the outputs match. */
            uint32_t flat_mask, noperspective_mask, centroid_mask, sample_mask;
            uint32_t location_mask, int_mask, uint_mask;
            /* Inputs read as float that integer attributes feed. */
            uint32_t uint_to_float_mask, sint_to_float_mask;
        } vs;
        struct
        {
            struct ps_compile_args args;
            unsigned int sample_count;
        } fs;
    } u;
};

struct shader_wgsl_variant
{
    struct shader_wgsl_compile_arguments compile_args;
    size_t binding_base;

    WGPUShaderModule module;
    struct vkd3d_shader_wgsl_binding_info binding_info;
    struct vkd3d_shader_varying_interpolation_info interpolation;
};

struct shader_wgsl_graphics_program
{
    struct shader_wgsl_variant *variants;
    SIZE_T variants_size, variant_count;

    struct vkd3d_shader_scan_descriptor_info descriptor_info;
    struct vkd3d_shader_scan_signature_info signature_info;
    struct vkd3d_shader_scan_combined_resource_sampler_info combined_info;
};

struct shader_wgsl_compute_program
{
    WGPUShaderModule module;
    bool failed;
    struct vkd3d_shader_wgsl_binding_info binding_info;

    struct vkd3d_shader_scan_descriptor_info descriptor_info;
    struct vkd3d_shader_scan_combined_resource_sampler_info combined_info;
};

struct shader_wgsl_compile_args
{
    struct vkd3d_shader_varying_map_info varying_map;
    struct vkd3d_shader_wgsl_target_info wgsl_target;
    struct vkd3d_shader_parameter_info parameter_info;
    struct vkd3d_shader_d3dbc_source_info d3dbc_info;
    struct vkd3d_shader_varying_interpolation_info interpolation;
    struct vkd3d_shader_wgsl_binding_info *binding_info;
    struct vkd3d_shader_parameter1 parameters[32];
};

static void shader_wgsl_handle_instruction(const struct wined3d_shader_instruction *ins)
{
}

/* The varying map between a Direct3D 9 vertex and pixel shader, with the
 * fog varying they will have: vkd3d adds a FOG output to the vertex shader
 * for some fog sources and a FOG input to the pixel shader for fragment fog,
 * after the signatures were scanned, and a varying in neither signature
 * would be left unlinked (the pixel shader reading zero). The elements go
 * where vkd3d puts them: last in the output signature, and in the input
 * signature at the register after the highest. */
static void shader_wgsl_build_varying_map(const struct wined3d_context *context, const struct wined3d_state *state,
        const struct wined3d_shader *ps, enum vkd3d_shader_fog_source fog_source,
        const struct vkd3d_shader_signature *vs_output, const struct vkd3d_shader_signature *ps_input,
        unsigned int *count, struct vkd3d_shader_varying_map *map)
{
    struct vkd3d_shader_signature_element out_elements[MAX_REG_OUTPUT + 2], in_elements[MAX_SM1_INTER_STAGE_VARYINGS + 1];
    struct vkd3d_shader_signature out = *vs_output, in = *ps_input;
    static const struct vkd3d_shader_signature_element fog =
    {
        .semantic_name = "FOG",
        .component_type = VKD3D_SHADER_COMPONENT_FLOAT,
        .mask = 0x1,
        .used_mask = 0x1,
    };
    struct ps_compile_args ps_args;
    unsigned int i, max_register = 0;
    bool has_fog = false, has_specular = false, ps_has_fog = false;

    for (i = 0; i < vs_output->element_count; ++i)
    {
        const struct vkd3d_shader_signature_element *e = &vs_output->elements[i];

        if (!_stricmp(e->semantic_name, "FOG") && !e->semantic_index)
            has_fog = true;
        if (!_stricmp(e->semantic_name, "COLOR") && e->semantic_index == 1)
            has_specular = true;
    }
    if (!has_fog && vs_output->element_count < ARRAY_SIZE(out_elements)
            && fog_source != VKD3D_SHADER_FOG_SOURCE_FOG
            && (fog_source != VKD3D_SHADER_FOG_SOURCE_FOG_OR_SPECULAR_W || has_specular))
    {
        memcpy(out_elements, vs_output->elements, vs_output->element_count * sizeof(*out_elements));
        out_elements[out.element_count++] = fog;
        out.elements = out_elements;
    }

    for (i = 0; i < ps_input->element_count; ++i)
    {
        const struct vkd3d_shader_signature_element *e = &ps_input->elements[i];

        if (!_stricmp(e->semantic_name, "FOG") && !e->semantic_index)
            ps_has_fog = true;
        max_register = max(max_register, e->register_index + 1);
    }
    find_ps_compile_args(state, ps, state->vertex_declaration && state->vertex_declaration->position_transformed,
            &ps_args, context);
    if (!ps_has_fog && ps_args.fog != WINED3D_FFP_PS_FOG_OFF && ps_input->element_count < ARRAY_SIZE(in_elements))
    {
        memcpy(in_elements, ps_input->elements, ps_input->element_count * sizeof(*in_elements));
        in_elements[in.element_count] = fog;
        in_elements[in.element_count++].register_index = max_register;
        in.elements = in_elements;
    }

    vkd3d_shader_build_varying_map(&out, &in, count, map);
    TRACE("fog source %#x, vs fog %u (+%u), ps fog %u (+%u, ps args fog %#x), %u varyings.\n", fog_source, has_fog,
            out.element_count - vs_output->element_count, ps_has_fog, in.element_count - ps_input->element_count,
            ps_args.fog, *count);
    for (i = 0; i < *count; ++i)
        TRACE("  output %u -> register %u mask %#x.\n", map[i].output_signature_index,
                map[i].input_register_index, map[i].input_mask);
}

static void shader_wgsl_compile_arguments_init(struct shader_wgsl_compile_arguments *args,
        const struct wined3d_context *context, const struct wined3d_shader *shader,
        const struct wined3d_state *state, unsigned int sample_count,
        const struct vkd3d_shader_varying_interpolation_info *ps_interpolation)
{
    memset(args, 0, sizeof(*args));

    switch (shader->reg_maps.shader_version.type)
    {
        case WINED3D_SHADER_TYPE_PIXEL:
            find_ps_compile_args(state, shader,
                    state->vertex_declaration && state->vertex_declaration->position_transformed,
                    &args->u.fs.args, context);
            args->u.fs.sample_count = sample_count;
            break;

        case WINED3D_SHADER_TYPE_VERTEX:
        {
            struct wined3d_shader *ps = state->shader[WINED3D_SHADER_TYPE_PIXEL];

            /* Table fog reads the depth, Z when orthographic; vertex fog the
             * fog output. */
            if (state->extra_vs_args.pixel_fog)
                args->u.vs.fog_source = state->extra_vs_args.ortho_fog
                        ? VKD3D_SHADER_FOG_SOURCE_Z : VKD3D_SHADER_FOG_SOURCE_W;
            else
                args->u.vs.fog_source = VKD3D_SHADER_FOG_SOURCE_FOG_OR_SPECULAR_W;

            if (shader->reg_maps.shader_version.major < 4 && ps && ps->backend_data)
            {
                struct shader_wgsl_graphics_program *vs_program = shader->backend_data;
                struct shader_wgsl_graphics_program *ps_program = ps->backend_data;

                if (ps_program->signature_info.input.element_count + 1 > ARRAY_SIZE(args->u.vs.varying_map))
                    ERR("Unexpected inter-stage varying count %u.\n", ps_program->signature_info.input.element_count);
                else
                    shader_wgsl_build_varying_map(context, state, ps, args->u.vs.fog_source,
                            &vs_program->signature_info.output, &ps_program->signature_info.input,
                            &args->u.vs.varying_count, args->u.vs.varying_map);
            }

            args->u.vs.clip_planes = state->extra_vs_args.clip_planes;
            if (ps_interpolation)
            {
                args->u.vs.flat_mask = ps_interpolation->flat_mask;
                args->u.vs.noperspective_mask = ps_interpolation->noperspective_mask;
                args->u.vs.centroid_mask = ps_interpolation->centroid_mask;
                args->u.vs.sample_mask = ps_interpolation->sample_mask;
                args->u.vs.location_mask = ps_interpolation->location_mask;
                args->u.vs.int_mask = ps_interpolation->int_mask;
                args->u.vs.uint_mask = ps_interpolation->uint_mask;
            }

            /* Direct3D 9 converts integer attributes (UBYTE4, SHORT2) to
             * float; WebGPU feeds them only to integer inputs. */
            if (shader->reg_maps.shader_version.major < 4 && state->vertex_declaration)
            {
                struct wined3d_stream_info stream_info;
                unsigned int i;

                wined3d_stream_info_from_declaration(&stream_info, state, context->d3d_info);
                for (i = 0; i < ARRAY_SIZE(stream_info.elements); ++i)
                {
                    const struct wined3d_format *format = stream_info.elements[i].format;

                    if (!(stream_info.use_map & (1u << i)) || !(format->attrs & WINED3D_FORMAT_ATTR_INTEGER)
                            || (format->attrs & WINED3D_FORMAT_ATTR_NORMALISED))
                        continue;
                    if (format->attrs & WINED3D_FORMAT_ATTR_UNSIGNED)
                        args->u.vs.uint_to_float_mask |= 1u << i;
                    else
                        args->u.vs.sint_to_float_mask |= 1u << i;
                }
            }
            break;
        }

        default:
            break;
    }
}

static void fill_buffer_parameter(struct vkd3d_shader_parameter1 *parameter, uint32_t binding,
        enum vkd3d_shader_parameter_name name, enum vkd3d_shader_parameter_data_type data_type, uint32_t offset)
{
    parameter->name = name;
    parameter->type = VKD3D_SHADER_PARAMETER_TYPE_BUFFER;
    parameter->data_type = data_type;
    parameter->u.buffer.set = 0;
    parameter->u.buffer.binding = binding;
    parameter->u.buffer.offset = offset;
}

static void fill_immediate_parameter(struct vkd3d_shader_parameter1 *parameter,
        enum vkd3d_shader_parameter_name name, uint32_t value)
{
    parameter->name = name;
    parameter->type = VKD3D_SHADER_PARAMETER_TYPE_IMMEDIATE_CONSTANT;
    parameter->data_type = VKD3D_SHADER_PARAMETER_DATA_TYPE_UINT32;
    parameter->u.immediate_constant.u.u32 = value;
}

static void fill_vs_parameters(struct shader_wgsl_compile_args *vkd3d_args,
        enum vkd3d_shader_source_type source_type,
        const struct shader_wgsl_compile_arguments *compile_args, uint32_t ffp_extra_binding)
{
    struct vkd3d_shader_parameter1 *parameters = vkd3d_args->parameters;
    unsigned int i;

    if (source_type != VKD3D_SHADER_SOURCE_D3D_BYTECODE)
        return;

    for (i = 0; i < WINED3D_MAX_CLIP_DISTANCES; ++i)
        fill_buffer_parameter(&parameters[i], ffp_extra_binding,
                VKD3D_SHADER_PARAMETER_NAME_CLIP_PLANE_0 + i, VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32_VEC4,
                offsetof(struct wined3d_ffp_vs_constants, clip_planes[i]));

    fill_immediate_parameter(&parameters[8], VKD3D_SHADER_PARAMETER_NAME_CLIP_PLANE_MASK,
            compile_args->u.vs.clip_planes);

    fill_buffer_parameter(&parameters[9], ffp_extra_binding, VKD3D_SHADER_PARAMETER_NAME_POINT_SIZE,
            VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32, offsetof(struct wined3d_ffp_vs_constants, point.size));
    fill_buffer_parameter(&parameters[10], ffp_extra_binding, VKD3D_SHADER_PARAMETER_NAME_POINT_SIZE_MIN,
            VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32, offsetof(struct wined3d_ffp_vs_constants, point_clamp.min));
    fill_buffer_parameter(&parameters[11], ffp_extra_binding, VKD3D_SHADER_PARAMETER_NAME_POINT_SIZE_MAX,
            VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32, offsetof(struct wined3d_ffp_vs_constants, point_clamp.max));
    fill_immediate_parameter(&parameters[12], VKD3D_SHADER_PARAMETER_NAME_FOG_SOURCE, compile_args->u.vs.fog_source);

    vkd3d_args->parameter_info.parameter_count = 13;
    vkd3d_args->parameter_info.parameters = vkd3d_args->parameters;
}

static void fill_ps_parameters(struct shader_wgsl_compile_args *vkd3d_args,
        enum vkd3d_shader_source_type source_type,
        const struct shader_wgsl_compile_arguments *compile_args, uint32_t ffp_extra_binding)
{
    struct vkd3d_shader_parameter1 *parameters = vkd3d_args->parameters;
    unsigned int i;

    vkd3d_args->parameter_info.parameters = vkd3d_args->parameters;

    if (source_type == VKD3D_SHADER_SOURCE_DXBC_TPF)
    {
        fill_immediate_parameter(&parameters[0], VKD3D_SHADER_PARAMETER_NAME_RASTERIZER_SAMPLE_COUNT,
                compile_args->u.fs.sample_count);
        vkd3d_args->parameter_info.parameter_count = 1;
    }
    else
    {
        fill_immediate_parameter(&parameters[0], VKD3D_SHADER_PARAMETER_NAME_FLAT_INTERPOLATION,
                compile_args->u.fs.args.flatshading);
        fill_immediate_parameter(&parameters[1], VKD3D_SHADER_PARAMETER_NAME_ALPHA_TEST_FUNC,
                compile_args->u.fs.args.alpha_test_func + 1);
        fill_buffer_parameter(&parameters[2], ffp_extra_binding, VKD3D_SHADER_PARAMETER_NAME_ALPHA_TEST_REF,
                VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32, offsetof(struct wined3d_ffp_ps_constants, alpha_test_ref));
        fill_immediate_parameter(&parameters[3], VKD3D_SHADER_PARAMETER_NAME_POINT_SPRITE,
                compile_args->u.fs.args.pointsprite);
        /* Shader model 1's bump mapping, from the fixed function pipeline's
         * constants. */
        for (i = 0; i < 6; ++i)
        {
            fill_buffer_parameter(&parameters[4 + i], ffp_extra_binding,
                    VKD3D_SHADER_PARAMETER_NAME_BUMP_MATRIX_0 + i, VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32_VEC4,
                    offsetof(struct wined3d_ffp_ps_constants, bumpenv.matrices[i]));
            fill_buffer_parameter(&parameters[10 + i], ffp_extra_binding,
                    VKD3D_SHADER_PARAMETER_NAME_BUMP_LUMINANCE_SCALE_0 + i, VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32,
                    offsetof(struct wined3d_ffp_ps_constants, bumpenv.lscale[i]));
            fill_buffer_parameter(&parameters[16 + i], ffp_extra_binding,
                    VKD3D_SHADER_PARAMETER_NAME_BUMP_LUMINANCE_OFFSET_0 + i, VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32,
                    offsetof(struct wined3d_ffp_ps_constants, bumpenv.loffset[i]));
        }
        /* Fog, which Direct3D 9 applies after the pixel shader. */
        switch (compile_args->u.fs.args.fog)
        {
            case WINED3D_FFP_PS_FOG_LINEAR:
                fill_immediate_parameter(&parameters[22], VKD3D_SHADER_PARAMETER_NAME_FOG_FRAGMENT_MODE,
                        VKD3D_SHADER_FOG_FRAGMENT_LINEAR);
                break;
            case WINED3D_FFP_PS_FOG_EXP:
                fill_immediate_parameter(&parameters[22], VKD3D_SHADER_PARAMETER_NAME_FOG_FRAGMENT_MODE,
                        VKD3D_SHADER_FOG_FRAGMENT_EXP);
                break;
            case WINED3D_FFP_PS_FOG_EXP2:
                fill_immediate_parameter(&parameters[22], VKD3D_SHADER_PARAMETER_NAME_FOG_FRAGMENT_MODE,
                        VKD3D_SHADER_FOG_FRAGMENT_EXP2);
                break;
            default:
                fill_immediate_parameter(&parameters[22], VKD3D_SHADER_PARAMETER_NAME_FOG_FRAGMENT_MODE,
                        VKD3D_SHADER_FOG_FRAGMENT_NONE);
                break;
        }
        fill_buffer_parameter(&parameters[23], ffp_extra_binding, VKD3D_SHADER_PARAMETER_NAME_FOG_COLOUR,
                VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32_VEC4, offsetof(struct wined3d_ffp_ps_constants, fog.colour));
        fill_buffer_parameter(&parameters[24], ffp_extra_binding, VKD3D_SHADER_PARAMETER_NAME_FOG_END,
                VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32, offsetof(struct wined3d_ffp_ps_constants, fog.end));
        fill_buffer_parameter(&parameters[25], ffp_extra_binding, VKD3D_SHADER_PARAMETER_NAME_FOG_SCALE,
                VKD3D_SHADER_PARAMETER_DATA_TYPE_FLOAT32, offsetof(struct wined3d_ffp_ps_constants, fog.scale));
        vkd3d_args->parameter_info.parameter_count = 26;
    }
}

static void shader_wgsl_init_compile_args(struct shader_wgsl_compile_args *args,
        struct vkd3d_shader_interface_info *vkd3d_interface, enum wined3d_shader_type shader_type,
        enum vkd3d_shader_source_type source_type, const struct shader_wgsl_compile_arguments *compile_args,
        const struct shader_wgsl_resource_bindings *bindings, struct vkd3d_shader_wgsl_binding_info *binding_info,
        struct vkd3d_shader_varying_interpolation_info *ps_interpolation)
{
    unsigned int i;

    memset(args, 0, sizeof(*args));
    args->wgsl_target.type = VKD3D_SHADER_STRUCTURE_TYPE_WGSL_TARGET_INFO;
    args->wgsl_target.next = &args->parameter_info;
    /* 1D textures are made 2D (texture_wgpu.c). */
    args->wgsl_target.textures_1d_as_2d = true;

    args->parameter_info.type = VKD3D_SHADER_STRUCTURE_TYPE_PARAMETER_INFO;
    args->parameter_info.next = &args->d3dbc_info;

    args->d3dbc_info.type = VKD3D_SHADER_STRUCTURE_TYPE_D3DBC_SOURCE_INFO;
    args->d3dbc_info.next = binding_info;

    binding_info->type = VKD3D_SHADER_STRUCTURE_TYPE_WGSL_BINDING_INFO;
    binding_info->next = vkd3d_interface;

    if (shader_type == WINED3D_SHADER_TYPE_PIXEL)
    {
        fill_ps_parameters(args, source_type, compile_args, bindings->ffp_ps_extra_binding);

        if (compile_args->u.fs.args.rt_alpha_swizzle)
            FIXME("Unhandled render target alpha swizzle %#x.\n", compile_args->u.fs.args.rt_alpha_swizzle);
        if (compile_args->u.fs.args.dual_source_blend)
            FIXME("Dual source blending is not implemented.\n");

        for (i = 0; i < ARRAY_SIZE(args->d3dbc_info.texture_dimensions); ++i)
        {
            enum wined3d_shader_tex_types type = (compile_args->u.fs.args.tex_types
                    >> (i * WINED3D_PSARGS_TEXTYPE_SHIFT)) & WINED3D_PSARGS_TEXTYPE_MASK;

            if (type == WINED3D_SHADER_TEX_3D)
                args->d3dbc_info.texture_dimensions[i] = VKD3D_SHADER_RESOURCE_TEXTURE_3D;
            else if (type == WINED3D_SHADER_TEX_CUBE)
                args->d3dbc_info.texture_dimensions[i] = VKD3D_SHADER_RESOURCE_TEXTURE_CUBE;
            else
                args->d3dbc_info.texture_dimensions[i] = VKD3D_SHADER_RESOURCE_TEXTURE_2D;
        }

        args->d3dbc_info.shadow_samplers = compile_args->u.fs.args.shadow;

        /* An output: the pixel shader's interpolation, for the vertex
         * shader to match. */
        ps_interpolation->type = VKD3D_SHADER_STRUCTURE_TYPE_VARYING_INTERPOLATION_INFO;
        ps_interpolation->next = args->wgsl_target.next;
        args->wgsl_target.next = ps_interpolation;
    }
    else if (shader_type == WINED3D_SHADER_TYPE_VERTEX)
    {
        fill_vs_parameters(args, source_type, compile_args, bindings->ffp_vs_extra_binding);

        if (source_type == VKD3D_SHADER_SOURCE_D3D_BYTECODE)
        {
            args->varying_map.type = VKD3D_SHADER_STRUCTURE_TYPE_VARYING_MAP_INFO;
            args->varying_map.next = args->wgsl_target.next;
            args->varying_map.varying_map = compile_args->u.vs.varying_map;
            args->varying_map.varying_count = compile_args->u.vs.varying_count;
            args->wgsl_target.next = &args->varying_map;
        }

        args->interpolation.type = VKD3D_SHADER_STRUCTURE_TYPE_VARYING_INTERPOLATION_INFO;
        args->interpolation.next = args->wgsl_target.next;
        args->interpolation.flat_mask = compile_args->u.vs.flat_mask;
        args->interpolation.noperspective_mask = compile_args->u.vs.noperspective_mask;
        args->interpolation.centroid_mask = compile_args->u.vs.centroid_mask;
        args->interpolation.sample_mask = compile_args->u.vs.sample_mask;
        args->interpolation.location_mask = compile_args->u.vs.location_mask;
        args->interpolation.int_mask = compile_args->u.vs.int_mask;
        args->interpolation.uint_mask = compile_args->u.vs.uint_mask;
        args->wgsl_target.next = &args->interpolation;

        args->wgsl_target.vertex_uint_to_float_mask = compile_args->u.vs.uint_to_float_mask;
        args->wgsl_target.vertex_sint_to_float_mask = compile_args->u.vs.sint_to_float_mask;
    }
}

static void shader_wgsl_log_messages(char *messages)
{
    const char *ptr, *end, *line;

    if (messages && *messages && FIXME_ON(d3d_shader))
    {
        FIXME("Shader log:\n");
        ptr = messages;
        end = ptr + strlen(ptr);
        while ((line = wined3d_get_line(&ptr, end)))
        {
            FIXME("    %.*s", (int)(ptr - line), line);
        }
        FIXME("\n");
    }
    vkd3d_shader_free_messages(messages);
}

/* WINE_WGSL_DUMP=dir writes each module to dir. */
static void shader_wgsl_dump(const struct vkd3d_shader_code *wgsl, enum wined3d_shader_type type)
{
    static LONG counter;
    const char *dir;
    char path[MAX_PATH];
    FILE *f;

    if (!(dir = getenv("WINE_WGSL_DUMP")) || !*dir)
        return;
    snprintf(path, sizeof(path), "%s\\%s-%04ld.wgsl", dir, debug_shader_type(type),
            (long)InterlockedIncrement(&counter));
    if ((f = fopen(path, "wb")))
    {
        fwrite(wgsl->code, 1, wgsl->size, f);
        fclose(f);
    }
}

static WGPUShaderModule shader_wgsl_compile_shader(struct wined3d_context_wgpu *context_wgpu,
        const struct wined3d_shader_desc *shader_desc, enum vkd3d_shader_source_type source_type,
        enum wined3d_shader_type shader_type, const struct shader_wgsl_compile_arguments *args,
        const struct shader_wgsl_resource_bindings *bindings, struct vkd3d_shader_wgsl_binding_info *binding_info,
        struct vkd3d_shader_varying_interpolation_info *ps_interpolation)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(context_wgpu->c.device);
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    WGPUShaderSourceWGSL wgsl_source = WGPU_SHADER_SOURCE_WGSL_INIT;
    const struct shader_wgsl_priv *priv = device_wgpu->d.shader_priv;
    struct vkd3d_shader_interface_info iface = {0};
    struct shader_wgsl_compile_args compile_args;
    struct vkd3d_shader_compile_info info;
    struct vkd3d_shader_code wgsl;
    WGPUShaderModule module;
    char *messages;
    int ret;

    iface.type = VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO;
    iface.bindings = bindings->bindings;
    iface.binding_count = bindings->binding_count;

    shader_wgsl_init_compile_args(&compile_args, &iface, shader_type, source_type, args, bindings,
            binding_info, ps_interpolation);

    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &compile_args.wgsl_target;
    info.source.code = shader_desc->byte_code;
    info.source.size = shader_desc->byte_code_size;
    info.source_type = source_type;
    info.target_type = VKD3D_SHADER_TARGET_WGSL;
    info.options = priv->compile_options;
    info.option_count = ARRAY_SIZE(priv->compile_options);
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    info.source_name = NULL;

    ret = vkd3d_shader_compile(&info, &wgsl, &messages);
    shader_wgsl_log_messages(messages);

    if (ret < 0)
    {
        ERR("Failed to compile shader, ret %d.\n", ret);
        return NULL;
    }

    shader_wgsl_dump(&wgsl, shader_type);

    wgsl_source.code.data = wgsl.code;
    wgsl_source.code.length = wgsl.size;
    module_desc.nextInChain = &wgsl_source.chain;
    wined3d_device_wgpu_push_error_scope(device_wgpu);
    module = wgpuDeviceCreateShaderModule(device_wgpu->device, &module_desc);
    if (!wined3d_device_wgpu_pop_error_scope(device_wgpu, "a shader module") && module)
    {
        if (ERR_ON(d3d_shader))
        {
            const char *ptr = wgsl.code, *end = ptr + wgsl.size, *line;

            ERR("WGSL:\n");
            while ((line = wined3d_get_line(&ptr, end)))
                ERR("    %.*s", (int)(ptr - line), line);
        }
        wgpuShaderModuleRelease(module);
        module = NULL;
    }

    vkd3d_shader_free_shader_code(&wgsl);

    return module;
}

static struct shader_wgsl_variant *shader_wgsl_find_graphics_variant(struct shader_wgsl_priv *priv,
        struct wined3d_context_wgpu *context_wgpu, struct wined3d_shader *shader,
        const struct wined3d_state *state, const struct shader_wgsl_resource_bindings *bindings,
        const struct vkd3d_shader_varying_interpolation_info *ps_interpolation)
{
    enum wined3d_shader_type shader_type = shader->reg_maps.shader_version.type;
    size_t binding_base = bindings->binding_base[shader_type];
    struct shader_wgsl_graphics_program *program;
    struct shader_wgsl_compile_arguments args;
    struct wined3d_shader_desc shader_desc;
    struct shader_wgsl_variant *variant;
    size_t variant_count, i;

    if (!(program = shader->backend_data))
        return NULL;

    shader_wgsl_compile_arguments_init(&args, &context_wgpu->c, shader, state,
            context_wgpu->sample_count, ps_interpolation);

    variant_count = program->variant_count;
    for (i = 0; i < variant_count; ++i)
    {
        variant = &program->variants[i];
        if (variant->binding_base == binding_base && !memcmp(&variant->compile_args, &args, sizeof(args)))
            return variant->module ? variant : NULL;
    }

    if (!wined3d_array_reserve((void **)&program->variants, &program->variants_size,
            variant_count + 1, sizeof(*program->variants)))
        return NULL;

    variant = &program->variants[variant_count];
    memset(variant, 0, sizeof(*variant));
    variant->compile_args = args;
    variant->binding_base = binding_base;

    if (shader->source_type == VKD3D_SHADER_SOURCE_D3D_BYTECODE)
    {
        shader_desc.byte_code = shader->function;
        shader_desc.byte_code_size = shader->functionLength;
    }
    else
    {
        shader_desc.byte_code = shader->byte_code;
        shader_desc.byte_code_size = shader->byte_code_size;
    }

    /* A variant that fails is kept, so as not to be compiled again. */
    variant->module = shader_wgsl_compile_shader(context_wgpu, &shader_desc, shader->source_type,
            shader_type, &args, bindings, &variant->binding_info, &variant->interpolation);
    ++program->variant_count;

    return variant->module ? variant : NULL;
}

static bool shader_wgsl_resource_bindings_add_binding(struct shader_wgsl_resource_bindings *bindings,
        enum vkd3d_shader_descriptor_type vkd3d_type, size_t register_idx,
        enum vkd3d_shader_visibility vkd3d_visibility, uint32_t flags, uint32_t *binding_idx)
{
    SIZE_T binding_count = bindings->binding_count;
    struct vkd3d_shader_resource_binding *binding;

    if (!wined3d_array_reserve((void **)&bindings->bindings, &bindings->bindings_size,
            binding_count + 1, sizeof(*bindings->bindings)))
        return false;

    *binding_idx = bindings->next_binding++;
    binding = &bindings->bindings[binding_count];
    binding->type = vkd3d_type;
    binding->register_space = 0;
    binding->register_index = register_idx;
    binding->shader_visibility = vkd3d_visibility;
    binding->flags = flags;
    binding->binding.set = 0;
    binding->binding.binding = *binding_idx;
    binding->binding.count = 1;
    ++bindings->binding_count;

    return true;
}

static bool shader_wgsl_resource_bindings_init(struct shader_wgsl_resource_bindings *bindings,
        const struct wined3d_state *state, uint32_t shader_mask)
{
    const struct vkd3d_shader_scan_descriptor_info *descriptor_info;
    enum vkd3d_shader_visibility shader_visibility;
    enum wined3d_shader_type shader_type;
    struct wined3d_shader *shader;
    uint32_t binding_idx;
    unsigned int i;

    bindings->binding_count = 0;
    bindings->next_binding = 0;

    for (shader_type = 0; shader_type < WINED3D_SHADER_TYPE_COUNT; ++shader_type)
    {
        bindings->binding_base[shader_type] = bindings->next_binding;

        if (!(shader_mask & (1u << shader_type)) || !(shader = state->shader[shader_type]))
            continue;

        if (shader_type == WINED3D_SHADER_TYPE_COMPUTE)
            descriptor_info = &((struct shader_wgsl_compute_program *)shader->backend_data)->descriptor_info;
        else
            descriptor_info = &((struct shader_wgsl_graphics_program *)shader->backend_data)->descriptor_info;

        shader_visibility = vkd3d_shader_visibility_from_wined3d(shader_type);

        for (i = 0; i < descriptor_info->descriptor_count; ++i)
        {
            const struct vkd3d_shader_descriptor_info *d = &descriptor_info->descriptors[i];
            uint32_t flags;

            if (d->register_space)
            {
                WARN("Unsupported register space %u.\n", d->register_space);
                return false;
            }

            if (d->type == VKD3D_SHADER_DESCRIPTOR_TYPE_CBV || d->resource_type == VKD3D_SHADER_RESOURCE_BUFFER)
                flags = VKD3D_SHADER_BINDING_FLAG_BUFFER;
            else
                flags = VKD3D_SHADER_BINDING_FLAG_IMAGE;

            if (!shader_wgsl_resource_bindings_add_binding(bindings, d->type,
                    d->register_index, shader_visibility, flags, &binding_idx))
                return false;
        }

        /* The uniform buffer the shader parameters are read from. */
        if (shader->source_type == VKD3D_SHADER_SOURCE_D3D_BYTECODE)
        {
            binding_idx = bindings->next_binding++;
            if (shader_type == WINED3D_SHADER_TYPE_PIXEL)
                bindings->ffp_ps_extra_binding = binding_idx;
            else
                bindings->ffp_vs_extra_binding = binding_idx;
        }
    }

    /* A compared SRV's depth view is 128 bindings past its own. */
    if (bindings->next_binding > 128)
    {
        FIXME("Too many bindings (%u).\n", bindings->next_binding);
        return false;
    }

    return true;
}

static WGPUTextureViewDimension wgpu_view_dimension_from_vkd3d(enum vkd3d_shader_resource_type t)
{
    switch (t)
    {
        case VKD3D_SHADER_RESOURCE_TEXTURE_1D:
        case VKD3D_SHADER_RESOURCE_TEXTURE_2D:
        case VKD3D_SHADER_RESOURCE_TEXTURE_2DMS:
            return WGPUTextureViewDimension_2D;
        case VKD3D_SHADER_RESOURCE_TEXTURE_3D:
            return WGPUTextureViewDimension_3D;
        case VKD3D_SHADER_RESOURCE_TEXTURE_CUBE:
            return WGPUTextureViewDimension_Cube;
        case VKD3D_SHADER_RESOURCE_TEXTURE_1DARRAY:
        case VKD3D_SHADER_RESOURCE_TEXTURE_2DARRAY:
        case VKD3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY:
            return WGPUTextureViewDimension_2DArray;
        case VKD3D_SHADER_RESOURCE_TEXTURE_CUBEARRAY:
            return WGPUTextureViewDimension_CubeArray;
        default:
            FIXME("Unhandled resource type %#x.\n", t);
            return WGPUTextureViewDimension_2D;
    }
}

static WGPUTextureFormat wgpu_format_from_wgsl(const char *format)
{
    static const struct
    {
        const char *name;
        WGPUTextureFormat format;
    }
    formats[] =
    {
        {"r32float", WGPUTextureFormat_R32Float},
        {"r32uint", WGPUTextureFormat_R32Uint},
        {"r32sint", WGPUTextureFormat_R32Sint},
        {"rg32float", WGPUTextureFormat_RG32Float},
        {"rg32uint", WGPUTextureFormat_RG32Uint},
        {"rg32sint", WGPUTextureFormat_RG32Sint},
        {"rgba32float", WGPUTextureFormat_RGBA32Float},
        {"rgba32uint", WGPUTextureFormat_RGBA32Uint},
        {"rgba32sint", WGPUTextureFormat_RGBA32Sint},
        {"rgba16float", WGPUTextureFormat_RGBA16Float},
        {"rgba16uint", WGPUTextureFormat_RGBA16Uint},
        {"rgba16sint", WGPUTextureFormat_RGBA16Sint},
        {"rgba8unorm", WGPUTextureFormat_RGBA8Unorm},
        {"rgba8snorm", WGPUTextureFormat_RGBA8Snorm},
        {"rgba8uint", WGPUTextureFormat_RGBA8Uint},
        {"rgba8sint", WGPUTextureFormat_RGBA8Sint},
        {"bgra8unorm", WGPUTextureFormat_BGRA8Unorm},
    };
    unsigned int i;

    for (i = 0; format && i < ARRAY_SIZE(formats); ++i)
    {
        if (!strcmp(formats[i].name, format))
            return formats[i].format;
    }
    FIXME("Unhandled storage texture format %s.\n", debugstr_a(format));
    return WGPUTextureFormat_R32Float;
}

static WGPUShaderStage wgpu_shader_stage_from_wined3d(enum wined3d_shader_type shader_type)
{
    switch (shader_type)
    {
        case WINED3D_SHADER_TYPE_VERTEX:
            return WGPUShaderStage_Vertex;
        case WINED3D_SHADER_TYPE_PIXEL:
            return WGPUShaderStage_Fragment;
        case WINED3D_SHADER_TYPE_COMPUTE:
            return WGPUShaderStage_Compute;
        default:
            return WGPUShaderStage_None;
    }
}

/* Adds a stage's bindings to a bind group layout and to the wined3d
 * bindings, which say what state each takes. */
static bool shader_wgsl_add_stage_bindings(struct wined3d_shader_bindings_wgpu *wined3d_bindings,
        enum wined3d_shader_type shader_type, const struct vkd3d_shader_wgsl_binding_info *info,
        const struct vkd3d_shader_scan_combined_resource_sampler_info *combined_info)
{
    struct wined3d_shader_binding_wgpu *b;
    WGPUBindGroupLayoutEntry *e;
    unsigned int i, j;

    for (i = 0; i < info->binding_count; ++i)
    {
        const struct vkd3d_shader_wgsl_binding *wgsl = &info->bindings[i];

        if (!wined3d_array_reserve((void **)&wined3d_bindings->entries, &wined3d_bindings->entries_size,
                wined3d_bindings->count + 1, sizeof(*wined3d_bindings->entries))
                || !wined3d_array_reserve((void **)&wined3d_bindings->bindings, &wined3d_bindings->size,
                wined3d_bindings->count + 1, sizeof(*wined3d_bindings->bindings)))
            return false;

        e = &wined3d_bindings->entries[wined3d_bindings->count];
        memset(e, 0, sizeof(*e));
        e->binding = wgsl->binding;
        e->visibility = wgpu_shader_stage_from_wined3d(shader_type);

        b = &wined3d_bindings->bindings[wined3d_bindings->count++];
        memset(b, 0, sizeof(*b));
        b->shader_type = shader_type;
        b->resource_idx = wgsl->register_index;
        b->resource_type = (enum wined3d_shader_resource_type)wgsl->resource_type;
        b->binding = wgsl->binding;
        b->data_type = wgsl->resource_data_type;

        switch (wgsl->type)
        {
            case VKD3D_SHADER_WGSL_BINDING_UNIFORM_BUFFER:
                e->buffer.type = WGPUBufferBindingType_Uniform;
                b->type = WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_CBV;
                if (wgsl->register_index == ~0u)
                    b->resource_idx = WINED3D_FFP_CONSTANTS_EXTRA_REGISTER;
                break;
            case VKD3D_SHADER_WGSL_BINDING_READ_ONLY_STORAGE_BUFFER:
                e->buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
                b->type = WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV;
                break;
            case VKD3D_SHADER_WGSL_BINDING_STORAGE_BUFFER:
                e->buffer.type = WGPUBufferBindingType_Storage;
                b->type = WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_UAV;
                break;
            case VKD3D_SHADER_WGSL_BINDING_SAMPLER:
                e->sampler.type = WGPUSamplerBindingType_Filtering;
                b->type = WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SAMPLER;
                b->data_type = 0;
                for (j = 0; combined_info && j < combined_info->combined_sampler_count; ++j)
                {
                    const struct vkd3d_shader_combined_resource_sampler_info *c = &combined_info->combined_samplers[j];

                    if (c->sampler_index == wgsl->register_index && c->resource_index < 128)
                        b->srv_mask[c->resource_index / 32] |= 1u << (c->resource_index % 32);
                }
                break;
            case VKD3D_SHADER_WGSL_BINDING_COMPARISON_SAMPLER:
                e->sampler.type = WGPUSamplerBindingType_Comparison;
                b->type = WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SAMPLER;
                /* For samplers, whether it compares. */
                b->data_type = 1;
                break;
            case VKD3D_SHADER_WGSL_BINDING_TEXTURE:
                e->texture.viewDimension = wgpu_view_dimension_from_vkd3d(wgsl->resource_type);
                e->texture.multisampled = wgsl->resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMS
                        || wgsl->resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY;
                if (wgsl->resource_data_type == VKD3D_SHADER_RESOURCE_DATA_INT)
                    e->texture.sampleType = WGPUTextureSampleType_Sint;
                else if (wgsl->resource_data_type == VKD3D_SHADER_RESOURCE_DATA_UINT)
                    e->texture.sampleType = WGPUTextureSampleType_Uint;
                else if (e->texture.multisampled)
                    e->texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
                else
                    e->texture.sampleType = WGPUTextureSampleType_Float;
                b->type = WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV;
                break;
            case VKD3D_SHADER_WGSL_BINDING_DEPTH_TEXTURE:
                e->texture.viewDimension = wgpu_view_dimension_from_vkd3d(wgsl->resource_type);
                e->texture.sampleType = WGPUTextureSampleType_Depth;
                e->texture.multisampled = wgsl->resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMS;
                b->type = WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_SRV_DEPTH;
                break;
            case VKD3D_SHADER_WGSL_BINDING_WRITE_ONLY_STORAGE_TEXTURE:
            case VKD3D_SHADER_WGSL_BINDING_READ_WRITE_STORAGE_TEXTURE:
                e->storageTexture.access = wgsl->type == VKD3D_SHADER_WGSL_BINDING_WRITE_ONLY_STORAGE_TEXTURE
                        ? WGPUStorageTextureAccess_WriteOnly : WGPUStorageTextureAccess_ReadWrite;
                e->storageTexture.format = wgpu_format_from_wgsl(wgsl->format);
                e->storageTexture.viewDimension = wgpu_view_dimension_from_vkd3d(wgsl->resource_type);
                b->type = WINED3D_SHADER_DESCRIPTOR_TYPE_WGPU_UAV;
                break;
            default:
                FIXME("Unhandled binding type %#x.\n", wgsl->type);
                return false;
        }
    }

    return true;
}

static void shader_wgsl_scan_shader(struct wined3d_shader *shader,
        struct vkd3d_shader_scan_descriptor_info *descriptor_info,
        struct vkd3d_shader_scan_signature_info *signature_info,
        struct vkd3d_shader_scan_combined_resource_sampler_info *combined_info)
{
    const struct shader_wgsl_priv *priv = shader->device->shader_priv;
    struct vkd3d_shader_compile_info info;
    char *messages;
    int ret;

    memset(descriptor_info, 0, sizeof(*descriptor_info));
    descriptor_info->type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_DESCRIPTOR_INFO;

    memset(combined_info, 0, sizeof(*combined_info));
    combined_info->type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_COMBINED_RESOURCE_SAMPLER_INFO;
    descriptor_info->next = combined_info;

    if (signature_info)
    {
        memset(signature_info, 0, sizeof(*signature_info));
        signature_info->type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
        combined_info->next = signature_info;
    }

    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = descriptor_info;
    if (shader->source_type == VKD3D_SHADER_SOURCE_D3D_BYTECODE)
    {
        info.source.code = shader->function;
        info.source.size = shader->functionLength;
    }
    else
    {
        info.source.code = shader->byte_code;
        info.source.size = shader->byte_code_size;
    }
    info.source_type = shader->source_type;
    info.target_type = VKD3D_SHADER_TARGET_WGSL;
    info.options = priv->compile_options;
    info.option_count = ARRAY_SIZE(priv->compile_options);
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    info.source_name = NULL;

    if ((ret = vkd3d_shader_scan(&info, &messages)) < 0)
        ERR("Failed to scan shader, ret %d.\n", ret);
    shader_wgsl_log_messages(messages);
}

static void shader_wgsl_precompile(void *shader_priv, struct wined3d_shader *shader)
{
    TRACE("shader_priv %p, shader %p.\n", shader_priv, shader);

    if (shader->reg_maps.shader_version.type == WINED3D_SHADER_TYPE_COMPUTE)
    {
        struct shader_wgsl_compute_program *program;

        if (!(program = shader->backend_data))
        {
            if (!(program = calloc(1, sizeof(*program))))
            {
                ERR("Failed to allocate program.\n");
                return;
            }
            shader->backend_data = program;
        }
        shader_wgsl_scan_shader(shader, &program->descriptor_info, NULL, &program->combined_info);
    }
    else
    {
        struct shader_wgsl_graphics_program *program;

        if (!(program = shader->backend_data))
        {
            if (!(program = calloc(1, sizeof(*program))))
            {
                ERR("Failed to allocate program.\n");
                return;
            }
            shader->backend_data = program;
        }
        shader_wgsl_scan_shader(shader, &program->descriptor_info, &program->signature_info,
                &program->combined_info);
    }
}

static void shader_wgsl_apply_draw_state(void *shader_priv, struct wined3d_context *context,
        const struct wined3d_state *state)
{
    struct wined3d_context_wgpu *context_wgpu = wined3d_context_wgpu(context);
    struct shader_wgsl_variant *vs_variant = NULL, *ps_variant = NULL;
    struct shader_wgsl_priv *priv = shader_priv;
    struct shader_wgsl_resource_bindings *bindings;
    struct wined3d_shader *vs, *ps;
    enum wined3d_shader_type type;

    priv->vertex_pipe->vp_apply_draw_state(context, state);
    priv->fragment_pipe->fp_apply_draw_state(context, state);

    context_wgpu->graphics.valid = false;
    context_wgpu->graphics.bindings.count = 0;
    context_wgpu->graphics.bindings.layout_valid = false;

    for (type = WINED3D_SHADER_TYPE_GEOMETRY; type < WINED3D_SHADER_TYPE_GRAPHICS_COUNT; ++type)
    {
        if (type != WINED3D_SHADER_TYPE_PIXEL && state->shader[type] && state->shader[type]->function)
        {
            FIXME("WebGPU has no %s shaders.\n", debug_shader_type(type));
            return;
        }
    }

    bindings = &priv->bindings;
    if (!shader_wgsl_resource_bindings_init(bindings, state,
            (1u << WINED3D_SHADER_TYPE_VERTEX) | (1u << WINED3D_SHADER_TYPE_PIXEL)))
    {
        ERR("Failed to initialise shader resource bindings.\n");
        return;
    }

    /* The pixel shader first: the vertex shader's outputs match its
     * interpolation. */
    if ((ps = state->shader[WINED3D_SHADER_TYPE_PIXEL]) && ps->function)
    {
        if (!(ps_variant = shader_wgsl_find_graphics_variant(priv, context_wgpu, ps, state, bindings, NULL)))
            return;
    }
    if (!(vs = state->shader[WINED3D_SHADER_TYPE_VERTEX]) || !vs->function)
    {
        FIXME("No vertex shader.\n");
        return;
    }
    if (!(vs_variant = shader_wgsl_find_graphics_variant(priv, context_wgpu, vs, state, bindings,
            ps_variant ? &ps_variant->interpolation : NULL)))
        return;

    if (!shader_wgsl_add_stage_bindings(&context_wgpu->graphics.bindings, WINED3D_SHADER_TYPE_VERTEX,
            &vs_variant->binding_info, &((struct shader_wgsl_graphics_program *)vs->backend_data)->combined_info)
            || (ps_variant && !shader_wgsl_add_stage_bindings(&context_wgpu->graphics.bindings,
            WINED3D_SHADER_TYPE_PIXEL, &ps_variant->binding_info,
            &((struct shader_wgsl_graphics_program *)ps->backend_data)->combined_info)))
        return;

    context_wgpu->graphics.modules[WINED3D_SHADER_TYPE_VERTEX] = vs_variant->module;
    context_wgpu->graphics.modules[WINED3D_SHADER_TYPE_PIXEL] = ps_variant ? ps_variant->module : NULL;
    context_wgpu->graphics.valid = true;
}

static void shader_wgsl_apply_compute_state(void *shader_priv,
        struct wined3d_context *context, const struct wined3d_state *state)
{
    struct wined3d_context_wgpu *context_wgpu = wined3d_context_wgpu(context);
    struct shader_wgsl_compute_program *program;
    struct shader_wgsl_priv *priv = shader_priv;
    struct wined3d_shader_desc shader_desc;
    struct wined3d_shader *shader;

    context_wgpu->compute.module = NULL;
    context_wgpu->compute.bindings.count = 0;
    context_wgpu->compute.bindings.layout_valid = false;

    if (!(shader = state->shader[WINED3D_SHADER_TYPE_COMPUTE]) || !(program = shader->backend_data))
        return;

    if (!program->module && !program->failed)
    {
        if (!shader_wgsl_resource_bindings_init(&priv->bindings, state, 1u << WINED3D_SHADER_TYPE_COMPUTE))
        {
            ERR("Failed to initialise shader resource bindings.\n");
            return;
        }
        shader_desc.byte_code = shader->byte_code;
        shader_desc.byte_code_size = shader->byte_code_size;
        if (!(program->module = shader_wgsl_compile_shader(context_wgpu, &shader_desc,
                shader->source_type, WINED3D_SHADER_TYPE_COMPUTE, NULL, &priv->bindings,
                &program->binding_info, NULL)))
            program->failed = true;
    }
    if (!program->module)
        return;

    if (!shader_wgsl_add_stage_bindings(&context_wgpu->compute.bindings, WINED3D_SHADER_TYPE_COMPUTE,
            &program->binding_info, &program->combined_info))
        return;
    context_wgpu->compute.module = program->module;
}

static void shader_wgsl_disable(void *shader_priv, struct wined3d_context *context)
{
    struct wined3d_context_wgpu *context_wgpu = wined3d_context_wgpu(context);
    struct shader_wgsl_priv *priv = shader_priv;

    priv->vertex_pipe->vp_disable(context);
    priv->fragment_pipe->fp_disable(context);

    context_wgpu->compute.module = NULL;
    context->shader_update_mask = (1u << WINED3D_SHADER_TYPE_PIXEL)
            | (1u << WINED3D_SHADER_TYPE_VERTEX)
            | (1u << WINED3D_SHADER_TYPE_GEOMETRY)
            | (1u << WINED3D_SHADER_TYPE_HULL)
            | (1u << WINED3D_SHADER_TYPE_DOMAIN)
            | (1u << WINED3D_SHADER_TYPE_COMPUTE);
}

static void shader_wgsl_update_float_vertex_constants(struct wined3d_device *device, UINT start, UINT count)
{
}

static void shader_wgsl_update_float_pixel_constants(struct wined3d_device *device, UINT start, UINT count)
{
}

static void shader_wgsl_destroy(struct wined3d_shader *shader)
{
    struct wined3d_device_wgpu *device_wgpu = wined3d_device_wgpu(shader->device);
    struct wined3d_context_wgpu *context_wgpu = &device_wgpu->context_wgpu;
    unsigned int i;
    size_t j;

    if (!shader->backend_data)
        return;

    if (shader->reg_maps.shader_version.type == WINED3D_SHADER_TYPE_COMPUTE)
    {
        struct shader_wgsl_compute_program *program = shader->backend_data;

        if (context_wgpu->compute.module == program->module)
        {
            context_wgpu->compute.module = NULL;
            context_wgpu->c.shader_update_mask |= 1u << WINED3D_SHADER_TYPE_COMPUTE;
        }
        if (program->module)
            wgpuShaderModuleRelease(program->module);
        vkd3d_shader_free_scan_combined_resource_sampler_info(&program->combined_info);
        vkd3d_shader_free_wgsl_binding_info(&program->binding_info);
        vkd3d_shader_free_scan_descriptor_info(&program->descriptor_info);
        free(program);
    }
    else
    {
        struct shader_wgsl_graphics_program *program = shader->backend_data;

        for (j = 0; j < program->variant_count; ++j)
        {
            struct shader_wgsl_variant *variant = &program->variants[j];

            for (i = 0; i < ARRAY_SIZE(context_wgpu->graphics.modules); ++i)
            {
                if (context_wgpu->graphics.modules[i] == variant->module)
                {
                    context_wgpu->graphics.modules[i] = NULL;
                    context_wgpu->c.shader_update_mask |= 1u << i;
                }
            }
            /* Pipelines made with the module keep it. */
            if (variant->module)
                wgpuShaderModuleRelease(variant->module);
            vkd3d_shader_free_wgsl_binding_info(&variant->binding_info);
        }
        free(program->variants);
        vkd3d_shader_free_scan_combined_resource_sampler_info(&program->combined_info);
        vkd3d_shader_free_scan_descriptor_info(&program->descriptor_info);
        vkd3d_shader_free_scan_signature_info(&program->signature_info);
        free(program);
    }
    shader->backend_data = NULL;
}

static HRESULT shader_wgsl_alloc(struct wined3d_device *device,
        const struct wined3d_vertex_pipe_ops *vertex_pipe, const struct wined3d_fragment_pipe_ops *fragment_pipe)
{
    void *vertex_priv, *fragment_priv;
    struct shader_wgsl_priv *priv;

    if (!(priv = calloc(1, sizeof(*priv))))
        return E_OUTOFMEMORY;

    if (!(vertex_priv = vertex_pipe->vp_alloc(&wgsl_shader_backend_wgpu, priv)))
    {
        ERR("Failed to initialise vertex pipe.\n");
        free(priv);
        return E_FAIL;
    }

    if (!(fragment_priv = fragment_pipe->alloc_private(&wgsl_shader_backend_wgpu, priv)))
    {
        ERR("Failed to initialise fragment pipe.\n");
        vertex_pipe->vp_free(device, NULL);
        free(priv);
        return E_FAIL;
    }

    priv->vertex_pipe = vertex_pipe;
    priv->fragment_pipe = fragment_pipe;

    device->vertex_priv = vertex_priv;
    device->fragment_priv = fragment_priv;
    device->shader_priv = priv;

    priv->compile_options[0].name = VKD3D_SHADER_COMPILE_OPTION_API_VERSION;
    priv->compile_options[0].value = VKD3D_SHADER_API_VERSION_1_7;
    priv->compile_options[1].name = VKD3D_SHADER_COMPILE_OPTION_WRITE_TESS_GEOM_POINT_SIZE;
    priv->compile_options[1].value = 0;

    return WINED3D_OK;
}

static void shader_wgsl_free(struct wined3d_device *device, struct wined3d_context *context)
{
    struct shader_wgsl_priv *priv = device->shader_priv;

    free(priv->bindings.bindings);
    priv->fragment_pipe->free_private(device, context);
    priv->vertex_pipe->vp_free(device, context);
    free(priv);
}

static BOOL shader_wgsl_allocate_context_data(struct wined3d_context *context)
{
    return TRUE;
}

static void shader_wgsl_free_context_data(struct wined3d_context *context)
{
}

static void shader_wgsl_init_context_state(struct wined3d_context *context)
{
}

static void shader_wgsl_get_caps(const struct wined3d_adapter *adapter, struct shader_caps *caps)
{
    memset(caps, 0, sizeof(*caps));

    caps->vs_version = min(wined3d_settings.max_sm_vs, 5);
    caps->ps_version = min(wined3d_settings.max_sm_ps, 5);
    caps->cs_version = min(wined3d_settings.max_sm_cs, 5);
    /* Geometry and tessellation shaders are only claimed, for a feature
     * level that needs them: they are created, but draws with them fail. */
    if (wined3d_settings.webgpu_feature_level >= WINED3D_FEATURE_LEVEL_10)
        caps->gs_version = min(wined3d_settings.max_sm_gs, 4);
    if (wined3d_settings.webgpu_feature_level >= WINED3D_FEATURE_LEVEL_11)
    {
        caps->gs_version = min(wined3d_settings.max_sm_gs, 5);
        caps->hs_version = min(wined3d_settings.max_sm_hs, 5);
        caps->ds_version = min(wined3d_settings.max_sm_ds, 5);
    }

    caps->vs_uniform_count = WINED3D_MAX_VS_CONSTS_F;
    caps->ps_uniform_count = WINED3D_MAX_PS_CONSTS_F;
    caps->ps_1x_max_value = FLT_MAX;
    caps->varying_count = wined3d_adapter_wgpu_const(adapter)->wgpu_info.limits.maxInterStageShaderVariables * 4;
    caps->wined3d_caps = WINED3D_SHADER_CAP_FULL_FFP_VARYINGS;
}

static BOOL shader_wgsl_color_fixup_supported(struct color_fixup_desc fixup)
{
    return is_identity_fixup(fixup);
}

static uint64_t shader_wgsl_compile(struct wined3d_context *context, const struct wined3d_shader_desc *shader_desc,
        enum wined3d_shader_type shader_type)
{
    FIXME("Not implemented.\n");
    return 0;
}

static const struct wined3d_shader_backend_ops wgsl_shader_backend_wgpu =
{
    .shader_handle_instruction = shader_wgsl_handle_instruction,
    .shader_precompile = shader_wgsl_precompile,
    .shader_apply_draw_state = shader_wgsl_apply_draw_state,
    .shader_apply_compute_state = shader_wgsl_apply_compute_state,
    .shader_disable = shader_wgsl_disable,
    .shader_update_float_vertex_constants = shader_wgsl_update_float_vertex_constants,
    .shader_update_float_pixel_constants = shader_wgsl_update_float_pixel_constants,
    .shader_destroy = shader_wgsl_destroy,
    .shader_alloc_private = shader_wgsl_alloc,
    .shader_free_private = shader_wgsl_free,
    .shader_allocate_context_data = shader_wgsl_allocate_context_data,
    .shader_free_context_data = shader_wgsl_free_context_data,
    .shader_init_context_state = shader_wgsl_init_context_state,
    .shader_get_caps = shader_wgsl_get_caps,
    .shader_color_fixup_supported = shader_wgsl_color_fixup_supported,
    .shader_compile = shader_wgsl_compile,
};

const struct wined3d_shader_backend_ops *wined3d_wgsl_shader_backend_init_wgpu(void)
{
    TRACE("Using %s.\n", vkd3d_shader_get_version(NULL, NULL));

    return &wgsl_shader_backend_wgpu;
}

static void wgsl_vertex_pipe_vp_apply_draw_state(struct wined3d_context *context, const struct wined3d_state *state)
{
}

static void wgsl_vertex_pipe_vp_disable(const struct wined3d_context *context)
{
}

static void wgsl_vertex_pipe_vp_get_caps(const struct wined3d_adapter *adapter, struct wined3d_vertex_caps *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->emulated_flatshading = true;
    caps->max_active_lights = WINED3D_MAX_ACTIVE_LIGHTS;
    caps->max_vertex_blend_matrices = MAX_VERTEX_BLENDS;
    caps->max_vertex_blend_matrix_index = 0;
    caps->vertex_processing_caps = WINED3DVTXPCAPS_TEXGEN
            | WINED3DVTXPCAPS_MATERIALSOURCE7
            | WINED3DVTXPCAPS_VERTEXFOG
            | WINED3DVTXPCAPS_DIRECTIONALLIGHTS
            | WINED3DVTXPCAPS_POSITIONALLIGHTS
            | WINED3DVTXPCAPS_LOCALVIEWER
            | WINED3DVTXPCAPS_TEXGEN_SPHEREMAP;
    caps->fvf_caps = WINED3DFVFCAPS_PSIZE | 8; /* 8 texture coordinates. */
    caps->max_user_clip_planes = wined3d_adapter_wgpu_const(adapter)->wgpu_info.clip_distances
            ? WINED3D_MAX_CLIP_DISTANCES : 0;
    caps->raster_caps = WINED3DPRASTERCAPS_FOGRANGE;
}

static unsigned int wgsl_vertex_pipe_vp_get_emul_mask(const struct wined3d_adapter *adapter)
{
    return 0;
}

static void *wgsl_vertex_pipe_vp_alloc(const struct wined3d_shader_backend_ops *shader_backend, void *shader_priv)
{
    if (shader_backend != &wgsl_shader_backend_wgpu)
    {
        FIXME("WGSL vertex pipe without WGSL shader backend not implemented.\n");
        return NULL;
    }

    return shader_priv;
}

static void wgsl_vertex_pipe_vp_free(struct wined3d_device *device, struct wined3d_context *context)
{
}

static const struct wined3d_state_entry_template wgsl_vertex_pipe_vp_states[] =
{
    {STATE_SHADER(WINED3D_SHADER_TYPE_VERTEX),          {STATE_SHADER(WINED3D_SHADER_TYPE_VERTEX),          state_nop}},
    {0}, /* Terminate */
};

static const struct wined3d_vertex_pipe_ops wgsl_vertex_pipe_wgpu =
{
    .vp_apply_draw_state = wgsl_vertex_pipe_vp_apply_draw_state,
    .vp_disable = wgsl_vertex_pipe_vp_disable,
    .vp_get_caps = wgsl_vertex_pipe_vp_get_caps,
    .vp_get_emul_mask = wgsl_vertex_pipe_vp_get_emul_mask,
    .vp_alloc = wgsl_vertex_pipe_vp_alloc,
    .vp_free = wgsl_vertex_pipe_vp_free,
    .vp_states = wgsl_vertex_pipe_vp_states,
};

const struct wined3d_vertex_pipe_ops *wined3d_wgsl_vertex_pipe_init_wgpu(void)
{
    return &wgsl_vertex_pipe_wgpu;
}

static void wgsl_fragment_pipe_fp_apply_draw_state(struct wined3d_context *context, const struct wined3d_state *state)
{
}

static void wgsl_fragment_pipe_fp_disable(const struct wined3d_context *context)
{
}

static void wgsl_fragment_pipe_fp_get_caps(const struct wined3d_adapter *adapter, struct fragment_caps *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->PrimitiveMiscCaps = WINED3DPMISCCAPS_TSSARGTEMP
            | WINED3DPMISCCAPS_PERSTAGECONSTANT;
    caps->TextureOpCaps = WINED3DTEXOPCAPS_DISABLE
            | WINED3DTEXOPCAPS_SELECTARG1
            | WINED3DTEXOPCAPS_SELECTARG2
            | WINED3DTEXOPCAPS_MODULATE4X
            | WINED3DTEXOPCAPS_MODULATE2X
            | WINED3DTEXOPCAPS_MODULATE
            | WINED3DTEXOPCAPS_ADDSIGNED2X
            | WINED3DTEXOPCAPS_ADDSIGNED
            | WINED3DTEXOPCAPS_ADD
            | WINED3DTEXOPCAPS_SUBTRACT
            | WINED3DTEXOPCAPS_ADDSMOOTH
            | WINED3DTEXOPCAPS_BLENDCURRENTALPHA
            | WINED3DTEXOPCAPS_BLENDFACTORALPHA
            | WINED3DTEXOPCAPS_BLENDTEXTUREALPHA
            | WINED3DTEXOPCAPS_BLENDDIFFUSEALPHA
            | WINED3DTEXOPCAPS_BLENDTEXTUREALPHAPM
            | WINED3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR
            | WINED3DTEXOPCAPS_MODULATECOLOR_ADDALPHA
            | WINED3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA
            | WINED3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR
            | WINED3DTEXOPCAPS_DOTPRODUCT3
            | WINED3DTEXOPCAPS_MULTIPLYADD
            | WINED3DTEXOPCAPS_LERP
            | WINED3DTEXOPCAPS_BUMPENVMAP
            | WINED3DTEXOPCAPS_BUMPENVMAPLUMINANCE;
    caps->max_blend_stages = WINED3D_MAX_FFP_TEXTURES;
    caps->max_textures = WINED3D_MAX_FFP_TEXTURES;
}

static unsigned int wgsl_fragment_pipe_fp_get_emul_mask(const struct wined3d_adapter *adapter)
{
    return 0;
}

static void *wgsl_fragment_pipe_fp_alloc(const struct wined3d_shader_backend_ops *shader_backend, void *shader_priv)
{
    if (shader_backend != &wgsl_shader_backend_wgpu)
    {
        FIXME("WGSL fragment pipe without WGSL shader backend not implemented.\n");
        return NULL;
    }

    return shader_priv;
}

static void wgsl_fragment_pipe_fp_free(struct wined3d_device *device, struct wined3d_context *context)
{
}

static BOOL wgsl_fragment_pipe_fp_alloc_context_data(struct wined3d_context *context)
{
    return TRUE;
}

static void wgsl_fragment_pipe_fp_free_context_data(struct wined3d_context *context)
{
}

static const struct wined3d_state_entry_template wgsl_fragment_pipe_fp_states[] =
{
    {STATE_RENDER(WINED3D_RS_FOGVERTEXMODE),     {STATE_RENDER(WINED3D_RS_FOGVERTEXMODE),     state_nop}},
    {0}, /* Terminate */
};

static const struct wined3d_fragment_pipe_ops wgsl_fragment_pipe_wgpu =
{
    .fp_apply_draw_state = wgsl_fragment_pipe_fp_apply_draw_state,
    .fp_disable = wgsl_fragment_pipe_fp_disable,
    .get_caps = wgsl_fragment_pipe_fp_get_caps,
    .get_emul_mask = wgsl_fragment_pipe_fp_get_emul_mask,
    .alloc_private = wgsl_fragment_pipe_fp_alloc,
    .free_private = wgsl_fragment_pipe_fp_free,
    .allocate_context_data = wgsl_fragment_pipe_fp_alloc_context_data,
    .free_context_data = wgsl_fragment_pipe_fp_free_context_data,
    .color_fixup_supported = shader_wgsl_color_fixup_supported,
    .states = wgsl_fragment_pipe_fp_states,
};

const struct wined3d_fragment_pipe_ops *wined3d_wgsl_fragment_pipe_init_wgpu(void)
{
    return &wgsl_fragment_pipe_wgpu;
}
