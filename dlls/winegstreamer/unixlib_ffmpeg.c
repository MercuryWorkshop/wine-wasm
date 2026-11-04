/*
 * winegstreamer's Unix side over FFmpeg: the Unix call tables
 *
 * Built in place of the GStreamer side (wg_parser.c, wg_transform.c and
 * the rest) where FFmpeg is present and GStreamer is not; its PE side is
 * unchanged. Decoding and demuxing only: wg_muxer is not implemented.
 * The tables and WoW64 thunks are wg_parser.c's.
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"
#include "windef.h"
#include "winbase.h"

#include "wg_ffmpeg.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

int wg_ffmpeg_threads = 1;

static void log_callback(void *avcl, int level, const char *fmt, va_list va)
{
    enum __wine_debug_class dbcl = level <= AV_LOG_ERROR ? __WINE_DBCL_ERR
            : level <= AV_LOG_WARNING ? __WINE_DBCL_WARN : __WINE_DBCL_TRACE;
    if (level > AV_LOG_WARNING && !TRACE_ON(mfplat)) return;
    wine_dbg_vlog(dbcl, __wine_dbch___default, __func__, fmt, va);
}

NTSTATUS wg_init_gstreamer(void *args)
{
    av_log_set_callback(log_callback);
    TRACE("FFmpeg avcodec %u, avformat %u.\n", avcodec_version(), avformat_version());
    return STATUS_SUCCESS;
}

NTSTATUS wg_muxer_create(void *args) { FIXME("Not implemented.\n"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS wg_muxer_destroy(void *args) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS wg_muxer_add_stream(void *args) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS wg_muxer_start(void *args) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS wg_muxer_push_sample(void *args) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS wg_muxer_read_data(void *args) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS wg_muxer_finalize(void *args) { return STATUS_NOT_IMPLEMENTED; }

const unixlib_entry_t __wine_unix_call_funcs[] =
{
#define X(name) [unix_ ## name] = name
    X(wg_init_gstreamer),

    X(wg_parser_create),
    X(wg_parser_destroy),

    X(wg_parser_connect),
    X(wg_parser_disconnect),

    X(wg_parser_get_next_read_offset),
    X(wg_parser_push_data),

    X(wg_parser_get_stream_count),
    X(wg_parser_get_stream),

    X(wg_parser_stream_get_current_format),
    X(wg_parser_stream_get_codec_format),
    X(wg_parser_stream_enable),
    X(wg_parser_stream_disable),

    X(wg_parser_stream_get_buffer),
    X(wg_parser_stream_copy_buffer),
    X(wg_parser_stream_release_buffer),
    X(wg_parser_stream_notify_qos),

    X(wg_parser_stream_get_duration),
    X(wg_parser_stream_get_tag),
    X(wg_parser_stream_seek),

    X(wg_transform_create),
    X(wg_transform_destroy),
    X(wg_transform_get_output_type),
    X(wg_transform_set_output_type),

    X(wg_transform_push_data),
    X(wg_transform_read_data),
    X(wg_transform_get_status),
    X(wg_transform_drain),
    X(wg_transform_flush),
    X(wg_transform_notify_qos),

    X(wg_muxer_create),
    X(wg_muxer_destroy),
    X(wg_muxer_add_stream),
    X(wg_muxer_start),
    X(wg_muxer_push_sample),
    X(wg_muxer_read_data),
    X(wg_muxer_finalize),
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_wg_funcs_count);

#ifdef _WIN64

typedef ULONG PTR32;

struct wg_media_type32
{
    GUID major;
    UINT32 format_size;
    PTR32 format;
};

static NTSTATUS wow64_wg_parser_connect(void *args)
{
    struct
    {
        wg_parser_t parser;
        PTR32 uri;
        UINT64 file_size;
    } *params32 = args;
    struct wg_parser_connect_params params =
    {
        .parser = params32->parser,
        .uri = ULongToPtr(params32->uri),
        .file_size = params32->file_size,
    };

    return wg_parser_connect(&params);
}

static NTSTATUS wow64_wg_parser_push_data(void *args) {
    struct
    {
        wg_parser_t parser;
        PTR32 data;
        UINT32 size;
    } *params32 = args;
    struct wg_parser_push_data_params params =
    {
        .parser = params32->parser,
        .data = ULongToPtr(params32->data),
        .size = params32->size,
    };

    return wg_parser_push_data(&params);
}

static NTSTATUS wow64_wg_parser_stream_get_current_format(void *args)
{
    struct
    {
        wg_parser_stream_t stream;
        PTR32 format;
    } *params32 = args;
    struct wg_parser_stream_get_current_format_params params =
    {
        .stream = params32->stream,
        .format = ULongToPtr(params32->format),
    };

    return wg_parser_stream_get_current_format(&params);
}

static NTSTATUS wow64_wg_parser_stream_get_codec_format(void *args)
{
    struct
    {
        wg_parser_stream_t stream;
        PTR32 format;
    } *params32 = args;
    struct wg_parser_stream_get_codec_format_params params =
    {
        .stream = params32->stream,
        .format = ULongToPtr(params32->format),
    };

    return wg_parser_stream_get_codec_format(&params);
}

static NTSTATUS wow64_wg_parser_stream_enable(void *args)
{
    struct
    {
        wg_parser_stream_t stream;
        PTR32 format;
    } *params32 = args;
    struct wg_parser_stream_enable_params params =
    {
        .stream = params32->stream,
        .format = ULongToPtr(params32->format),
    };

    return wg_parser_stream_enable(&params);
}

static NTSTATUS wow64_wg_parser_stream_get_buffer(void *args)
{
    struct
    {
        wg_parser_t parser;
        wg_parser_stream_t stream;
        PTR32 buffer;
    } *params32 = args;
    struct wg_parser_stream_get_buffer_params params =
    {
        .parser = params32->parser,
        .stream = params32->stream,
        .buffer = ULongToPtr(params32->buffer),
    };
    return wg_parser_stream_get_buffer(&params);
}

static NTSTATUS wow64_wg_parser_stream_copy_buffer(void *args)
{
    struct
    {
        wg_parser_stream_t stream;
        PTR32 data;
        UINT32 offset;
        UINT32 size;
    } *params32 = args;
    struct wg_parser_stream_copy_buffer_params params =
    {
        .stream = params32->stream,
        .data = ULongToPtr(params32->data),
        .offset = params32->offset,
        .size = params32->size,
    };
    return wg_parser_stream_copy_buffer(&params);
}

static NTSTATUS wow64_wg_parser_stream_get_tag(void *args)
{
    struct
    {
        wg_parser_stream_t stream;
        wg_parser_tag tag;
        PTR32 buffer;
        PTR32 size;
    } *params32 = args;
    struct wg_parser_stream_get_tag_params params =
    {
        .stream = params32->stream,
        .tag = params32->tag,
        .buffer = ULongToPtr(params32->buffer),
        .size = ULongToPtr(params32->size),
    };

    return wg_parser_stream_get_tag(&params);
}

NTSTATUS wow64_wg_transform_create(void *args)
{
    struct
    {
        wg_transform_t transform;
        struct wg_media_type32 input_type;
        struct wg_media_type32 output_type;
        struct wg_transform_attrs attrs;
    } *params32 = args;
    struct wg_transform_create_params params =
    {
        .input_type =
        {
            .major = params32->input_type.major,
            .format_size = params32->input_type.format_size,
            .u.format = ULongToPtr(params32->input_type.format),
        },
        .output_type =
        {
            .major = params32->output_type.major,
            .format_size = params32->output_type.format_size,
            .u.format = ULongToPtr(params32->output_type.format),
        },
        .attrs = params32->attrs,
    };
    NTSTATUS ret;

    ret = wg_transform_create(&params);
    params32->transform = params.transform;
    return ret;
}

NTSTATUS wow64_wg_transform_get_output_type(void *args)
{
    struct
    {
        wg_transform_t transform;
        struct wg_media_type32 media_type;
    } *params32 = args;
    struct wg_transform_get_output_type_params params =
    {
        .transform = params32->transform,
        .media_type =
        {
            .major = params32->media_type.major,
            .format_size = params32->media_type.format_size,
            .u.format = ULongToPtr(params32->media_type.format),
        },
    };
    NTSTATUS status;

    status = wg_transform_get_output_type(&params);
    params32->media_type.major = params.media_type.major;
    params32->media_type.format_size = params.media_type.format_size;
    return status;
}

NTSTATUS wow64_wg_transform_set_output_type(void *args)
{
    struct
    {
        wg_transform_t transform;
        struct wg_media_type32 media_type;
    } *params32 = args;
    struct wg_transform_set_output_type_params params =
    {
        .transform = params32->transform,
        .media_type =
        {
            .major = params32->media_type.major,
            .format_size = params32->media_type.format_size,
            .u.format = ULongToPtr(params32->media_type.format),
        },
    };
    return wg_transform_set_output_type(&params);
}

NTSTATUS wow64_wg_transform_push_data(void *args)
{
    struct
    {
        wg_transform_t transform;
        PTR32 sample;
        HRESULT result;
    } *params32 = args;
    struct wg_transform_push_data_params params =
    {
        .transform = params32->transform,
        .sample = ULongToPtr(params32->sample),
    };
    NTSTATUS ret;

    ret = wg_transform_push_data(&params);
    params32->result = params.result;
    return ret;
}

NTSTATUS wow64_wg_transform_read_data(void *args)
{
    struct
    {
        wg_transform_t transform;
        PTR32 sample;
        HRESULT result;
    } *params32 = args;
    struct wg_transform_read_data_params params =
    {
        .transform = params32->transform,
        .sample = ULongToPtr(params32->sample),
    };
    NTSTATUS ret;

    ret = wg_transform_read_data(&params);
    params32->result = params.result;
    return ret;
}

NTSTATUS wow64_wg_muxer_create(void *args)
{
    struct
    {
        wg_muxer_t muxer;
        PTR32 format;
    } *params32 = args;
    struct wg_muxer_create_params params =
    {
        .format = ULongToPtr(params32->format),
    };
    NTSTATUS ret;

    ret = wg_muxer_create(&params);
    params32->muxer = params.muxer;
    return ret;
}

NTSTATUS wow64_wg_muxer_add_stream(void *args)
{
    struct
    {
        wg_muxer_t muxer;
        UINT32 stream_id;
        PTR32 format;
    } *params32 = args;
    struct wg_muxer_add_stream_params params =
    {
        .muxer = params32->muxer,
        .stream_id = params32->stream_id,
        .format = ULongToPtr(params32->format),
    };
    return wg_muxer_add_stream(&params);
}

NTSTATUS wow64_wg_muxer_push_sample(void *args)
{
    struct
    {
        wg_muxer_t muxer;
        PTR32 sample;
        UINT32 stream_id;
    } *params32 = args;
    struct wg_muxer_push_sample_params params =
    {
        .muxer = params32->muxer,
        .sample = ULongToPtr(params32->sample),
        .stream_id = params32->stream_id,
    };
    return wg_muxer_push_sample(&params);
}

NTSTATUS wow64_wg_muxer_read_data(void *args)
{
    struct
    {
        wg_muxer_t muxer;
        PTR32 buffer;
        UINT32 size;
        UINT64 offset;
    } *params32 = args;
    struct wg_muxer_read_data_params params =
    {
        .muxer = params32->muxer,
        .buffer = ULongToPtr(params32->buffer),
        .size = params32->size,
        .offset = params32->offset,
    };
    NTSTATUS ret;

    ret = wg_muxer_read_data(&params);
    params32->size = params.size;
    params32->offset = params.offset;
    return ret;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
#define X64(name) [unix_ ## name] = wow64_ ## name
    X(wg_init_gstreamer),

    X(wg_parser_create),
    X(wg_parser_destroy),

    X64(wg_parser_connect),
    X(wg_parser_disconnect),

    X(wg_parser_get_next_read_offset),
    X64(wg_parser_push_data),

    X(wg_parser_get_stream_count),
    X(wg_parser_get_stream),

    X64(wg_parser_stream_get_current_format),
    X64(wg_parser_stream_get_codec_format),
    X64(wg_parser_stream_enable),
    X(wg_parser_stream_disable),

    X64(wg_parser_stream_get_buffer),
    X64(wg_parser_stream_copy_buffer),
    X(wg_parser_stream_release_buffer),
    X(wg_parser_stream_notify_qos),

    X(wg_parser_stream_get_duration),
    X64(wg_parser_stream_get_tag),
    X(wg_parser_stream_seek),

    X64(wg_transform_create),
    X(wg_transform_destroy),
    X64(wg_transform_get_output_type),
    X64(wg_transform_set_output_type),

    X64(wg_transform_push_data),
    X64(wg_transform_read_data),
    X(wg_transform_get_status),
    X(wg_transform_drain),
    X(wg_transform_flush),
    X(wg_transform_notify_qos),

    X64(wg_muxer_create),
    X(wg_muxer_destroy),
    X64(wg_muxer_add_stream),
    X(wg_muxer_start),
    X64(wg_muxer_push_sample),
    X64(wg_muxer_read_data),
    X(wg_muxer_finalize),
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_wg_funcs_count);

#endif  /* _WIN64 */
