/*
 * winegstreamer's Unix side over FFmpeg (no GStreamer): shared definitions
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

#ifndef __WINE_WINEGSTREAMER_WG_FFMPEG_H
#define __WINE_WINEGSTREAMER_WG_FFMPEG_H

#include <stdbool.h>
#include <stdint.h>

#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "unixlib.h"

/* libavcodec's thread count for every decoder: 1 unless WINE_WG_FFMPEG_THREADS says
 * otherwise (0: FFmpeg's choice) */
extern int wg_ffmpeg_threads;

/* a codec for a compressed media type, its parameters filled in (extradata
 * allocated with av_malloc) */
extern bool wg_ffmpeg_codec_from_media_type(const struct wg_media_type *type, AVCodecParameters *par);
/* the pixel format of a raw video subtype, and whether its U and V planes are swapped (YV12) */
extern enum AVPixelFormat wg_ffmpeg_pix_fmt_from_subtype(const GUID *subtype, bool *swap_uv);
/* the MF subtype of a wg_video_format and back */
extern const GUID *wg_ffmpeg_subtype_from_video_format(wg_video_format format);
extern wg_video_format wg_ffmpeg_video_format_from_subtype(const GUID *subtype);

/* The layout of a raw video frame in a sample, as winegstreamer's
 * GStreamer side lays it out (its align_video_info_planes over
 * gst_video_info_align): planes, strides (negative: bottom up) and size. */
struct wg_ffmpeg_layout
{
    int planes;
    int64_t offset[4];
    int stride[4];
    int64_t size;
    int left, top; /* the picture's place in the padded frame */
};
extern bool wg_ffmpeg_video_layout(enum AVPixelFormat fmt, int width, int height, const MFVideoInfo *info,
        UINT32 plane_align, INT32 sample_stride, struct wg_ffmpeg_layout *layout);
/* the planes of a frame with that layout at base (U and V swapped for YV12) */
extern void wg_ffmpeg_layout_planes(enum AVPixelFormat fmt, bool swap_uv, const struct wg_ffmpeg_layout *layout,
        BYTE *base, uint8_t *planes[4], int strides[4]);
/* frame (any pixel format) into a sample's memory with that layout, through sws */
extern bool wg_ffmpeg_copy_frame(struct SwsContext **sws, const AVFrame *frame, enum AVPixelFormat fmt,
        bool swap_uv, const struct wg_ffmpeg_layout *layout, BYTE *dst);

/* the wg_format of a decoded or compressed stream (wg_parser's) */
extern void wg_ffmpeg_format_from_codec(const AVCodecParameters *par, AVRational fps, struct wg_format *format);
extern void wg_ffmpeg_format_from_frame(const AVFrame *frame, enum AVMediaType type, AVRational fps, struct wg_format *format);
extern enum AVSampleFormat wg_ffmpeg_sample_fmt(wg_audio_format format, int *bytes);
extern enum AVPixelFormat wg_ffmpeg_pix_fmt_from_video_format(wg_video_format format, bool *swap_uv);

extern NTSTATUS wg_init_gstreamer(void *args);

extern NTSTATUS wg_transform_create(void *args);
extern NTSTATUS wg_transform_destroy(void *args);
extern NTSTATUS wg_transform_get_output_type(void *args);
extern NTSTATUS wg_transform_set_output_type(void *args);
extern NTSTATUS wg_transform_push_data(void *args);
extern NTSTATUS wg_transform_read_data(void *args);
extern NTSTATUS wg_transform_get_status(void *args);
extern NTSTATUS wg_transform_drain(void *args);
extern NTSTATUS wg_transform_flush(void *args);
extern NTSTATUS wg_transform_notify_qos(void *args);

extern NTSTATUS wg_parser_create(void *args);
extern NTSTATUS wg_parser_destroy(void *args);
extern NTSTATUS wg_parser_connect(void *args);
extern NTSTATUS wg_parser_disconnect(void *args);
extern NTSTATUS wg_parser_get_next_read_offset(void *args);
extern NTSTATUS wg_parser_push_data(void *args);
extern NTSTATUS wg_parser_get_stream_count(void *args);
extern NTSTATUS wg_parser_get_stream(void *args);
extern NTSTATUS wg_parser_stream_get_current_format(void *args);
extern NTSTATUS wg_parser_stream_get_codec_format(void *args);
extern NTSTATUS wg_parser_stream_enable(void *args);
extern NTSTATUS wg_parser_stream_disable(void *args);
extern NTSTATUS wg_parser_stream_get_buffer(void *args);
extern NTSTATUS wg_parser_stream_copy_buffer(void *args);
extern NTSTATUS wg_parser_stream_release_buffer(void *args);
extern NTSTATUS wg_parser_stream_notify_qos(void *args);
extern NTSTATUS wg_parser_stream_get_duration(void *args);
extern NTSTATUS wg_parser_stream_get_tag(void *args);
extern NTSTATUS wg_parser_stream_seek(void *args);

extern NTSTATUS wg_muxer_create(void *args);
extern NTSTATUS wg_muxer_destroy(void *args);
extern NTSTATUS wg_muxer_add_stream(void *args);
extern NTSTATUS wg_muxer_start(void *args);
extern NTSTATUS wg_muxer_push_sample(void *args);
extern NTSTATUS wg_muxer_read_data(void *args);
extern NTSTATUS wg_muxer_finalize(void *args);

static inline BOOL is_mf_video_area_empty(const MFVideoArea *area)
{
    return !area->OffsetX.value && !area->OffsetY.value && !area->Area.cx && !area->Area.cy;
}

static inline BYTE *wg_sample_data(struct wg_sample *sample)
{
    return (BYTE *)(UINT_PTR)sample->data;
}

#endif /* __WINE_WINEGSTREAMER_WG_FFMPEG_H */
