/*
 * winegstreamer's Unix side over FFmpeg: media types, wg_format and frame layout
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
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "objbase.h"
#include "vfw.h"

/* The headers' GUIDs, defined weak: a build that links this library into
 * one module with others (each defining the GUIDs its headers name) keeps
 * one copy of each. */
#ifdef __clang__
#pragma clang attribute push (__attribute__((weak)), apply_to = variable(is_global))
#endif
#include "initguid.h"
#include "d3d9types.h"
#include "mfapi.h"
#include "wmcodecdsp.h"
#include "mmreg.h"
#include "wg_ffmpeg.h"
#ifdef __clang__
#pragma clang attribute pop
#endif
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

#ifdef __clang__
#pragma clang attribute push (__attribute__((weak)), apply_to = variable(is_global))
#endif
DEFINE_MEDIATYPE_GUID(MFVideoFormat_CVID, MAKEFOURCC('c','v','i','d'));
DEFINE_MEDIATYPE_GUID(MFVideoFormat_IV50, MAKEFOURCC('I','V','5','0'));
DEFINE_MEDIATYPE_GUID(MFVideoFormat_ABGR32, D3DFMT_A8B8G8R8);
DEFINE_MEDIATYPE_GUID(MFAudioFormat_RAW_AAC, WAVE_FORMAT_RAW_AAC1);
DEFINE_MEDIATYPE_GUID(MFAudioFormat_MSAudio1, WAVE_FORMAT_MSAUDIO1);
#ifdef __clang__
#pragma clang attribute pop
#endif

static bool set_extradata(AVCodecParameters *par, const void *data, size_t size)
{
    if (!size) return true;
    if (!(par->extradata = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE))) return false;
    memcpy(par->extradata, data, size);
    par->extradata_size = size;
    return true;
}

static bool codec_from_wave_format(const WAVEFORMATEX *wfx, UINT32 size, AVCodecParameters *par)
{
    GUID subtype = MFAudioFormat_Base;
    const BYTE *extra = (const BYTE *)(wfx + 1);
    UINT32 extra_size = wfx->cbSize;

    subtype.Data1 = wfx->wFormatTag;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && size >= sizeof(WAVEFORMATEXTENSIBLE))
    {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wfx;
        subtype = ext->SubFormat;
        extra = (const BYTE *)(ext + 1);
        extra_size = wfx->cbSize + sizeof(WAVEFORMATEX) - sizeof(WAVEFORMATEXTENSIBLE);
    }
    if (extra_size > size - (extra - (const BYTE *)wfx)) extra_size = size - (extra - (const BYTE *)wfx);

    par->codec_type = AVMEDIA_TYPE_AUDIO;
    par->sample_rate = wfx->nSamplesPerSec;
    av_channel_layout_default(&par->ch_layout, wfx->nChannels);
    par->block_align = wfx->nBlockAlign;
    par->bit_rate = (int64_t)wfx->nAvgBytesPerSec * 8;
    par->bits_per_coded_sample = wfx->wBitsPerSample;

    if (IsEqualGUID(&subtype, &MFAudioFormat_MPEG))
    {
        const MPEG1WAVEFORMAT *mpeg = (const MPEG1WAVEFORMAT *)wfx;
        par->codec_id = size >= sizeof(*mpeg) && mpeg->fwHeadLayer == ACM_MPEG_LAYER1 ? AV_CODEC_ID_MP1
                : size >= sizeof(*mpeg) && mpeg->fwHeadLayer == ACM_MPEG_LAYER3 ? AV_CODEC_ID_MP3 : AV_CODEC_ID_MP2;
        return true;
    }
    if (IsEqualGUID(&subtype, &MFAudioFormat_MP3))
    {
        par->codec_id = AV_CODEC_ID_MP3;
        return true;
    }
    if (IsEqualGUID(&subtype, &MFAudioFormat_AAC))
    {
        const HEAACWAVEFORMAT *aac = (const HEAACWAVEFORMAT *)wfx;
        par->codec_id = AV_CODEC_ID_AAC;
        /* raw (0) carries the AudioSpecificConfig; ADTS (1) and LOAS (3) frames carry their own */
        if (size > sizeof(aac->wfInfo) && aac->wfInfo.wPayloadType == 0)
            return set_extradata(par, aac->pbAudioSpecificConfig, size - sizeof(aac->wfInfo));
        if (aac->wfInfo.wPayloadType == 3) par->codec_id = AV_CODEC_ID_AAC_LATM;
        return true;
    }
    if (IsEqualGUID(&subtype, &MFAudioFormat_RAW_AAC))
    {
        par->codec_id = AV_CODEC_ID_AAC;
        return set_extradata(par, extra, extra_size);
    }
    if (IsEqualGUID(&subtype, &MFAudioFormat_MSAudio1)) par->codec_id = AV_CODEC_ID_WMAV1;
    else if (IsEqualGUID(&subtype, &MFAudioFormat_WMAudioV8)) par->codec_id = AV_CODEC_ID_WMAV2;
    else if (IsEqualGUID(&subtype, &MFAudioFormat_WMAudioV9)) par->codec_id = AV_CODEC_ID_WMAPRO;
    else if (IsEqualGUID(&subtype, &MFAudioFormat_WMAudio_Lossless)) par->codec_id = AV_CODEC_ID_WMALOSSLESS;
    else return false;
    return set_extradata(par, extra, extra_size);
}

static bool codec_from_video_format(const MFVIDEOFORMAT *format, UINT32 size, AVCodecParameters *par)
{
    const GUID *subtype = &format->guidFormat;
    const void *extra = format + 1;
    UINT32 extra_size = size > sizeof(*format) ? size - sizeof(*format) : 0;

    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->width = format->videoInfo.dwWidth;
    par->height = format->videoInfo.dwHeight;
    if (!is_mf_video_area_empty(&format->videoInfo.MinimumDisplayAperture))
    {
        par->width = format->videoInfo.MinimumDisplayAperture.Area.cx;
        par->height = format->videoInfo.MinimumDisplayAperture.Area.cy;
    }

    if (IsEqualGUID(subtype, &MFVideoFormat_H264)) par->codec_id = AV_CODEC_ID_H264;
    else if (IsEqualGUID(subtype, &MFVideoFormat_WMV1)) par->codec_id = AV_CODEC_ID_WMV1;
    else if (IsEqualGUID(subtype, &MFVideoFormat_WMV2)) par->codec_id = AV_CODEC_ID_WMV2;
    else if (IsEqualGUID(subtype, &MFVideoFormat_WMV3)) par->codec_id = AV_CODEC_ID_WMV3;
    else if (IsEqualGUID(subtype, &MEDIASUBTYPE_WMVA) || IsEqualGUID(subtype, &MFVideoFormat_WVC1)) par->codec_id = AV_CODEC_ID_VC1;
    else if (IsEqualGUID(subtype, &MFVideoFormat_CVID)) par->codec_id = AV_CODEC_ID_CINEPAK;
    else if (IsEqualGUID(subtype, &MFVideoFormat_IV50)) par->codec_id = AV_CODEC_ID_INDEO5;
    else if (IsEqualGUID(subtype, &MEDIASUBTYPE_MPEG1Payload))
    {
        const struct mpeg_video_format *mpeg = (const struct mpeg_video_format *)format;
        par->codec_id = AV_CODEC_ID_MPEG1VIDEO;
        if (size >= sizeof(*mpeg) && mpeg->sequence_header_count <= size - sizeof(*mpeg))
            return set_extradata(par, mpeg->sequence_header, mpeg->sequence_header_count);
        return true;
    }
    else return false;
    return set_extradata(par, extra, extra_size);
}

bool wg_ffmpeg_codec_from_media_type(const struct wg_media_type *type, AVCodecParameters *par)
{
    if (IsEqualGUID(&type->major, &MFMediaType_Audio) && type->format_size >= sizeof(WAVEFORMATEX))
        return codec_from_wave_format(type->u.audio, type->format_size, par);
    if (IsEqualGUID(&type->major, &MFMediaType_Video) && type->format_size >= sizeof(MFVIDEOFORMAT))
        return codec_from_video_format(type->u.video, type->format_size, par);
    return false;
}

static const struct
{
    wg_video_format format;
    const GUID *subtype;
    enum AVPixelFormat pix_fmt;
    bool swap_uv;
}
video_formats[] =
{
    {WG_VIDEO_FORMAT_BGRA, &MFVideoFormat_ARGB32, AV_PIX_FMT_BGRA},
    {WG_VIDEO_FORMAT_BGRx, &MFVideoFormat_RGB32, AV_PIX_FMT_BGR0},
    {WG_VIDEO_FORMAT_BGR, &MFVideoFormat_RGB24, AV_PIX_FMT_BGR24},
    {WG_VIDEO_FORMAT_RGBA, &MFVideoFormat_ABGR32, AV_PIX_FMT_RGBA},
    {WG_VIDEO_FORMAT_RGB15, &MFVideoFormat_RGB555, AV_PIX_FMT_RGB555LE},
    {WG_VIDEO_FORMAT_RGB16, &MFVideoFormat_RGB565, AV_PIX_FMT_RGB565LE},
    {WG_VIDEO_FORMAT_AYUV, &MFVideoFormat_AYUV, AV_PIX_FMT_VUYA},
    {WG_VIDEO_FORMAT_I420, &MFVideoFormat_I420, AV_PIX_FMT_YUV420P},
    {WG_VIDEO_FORMAT_I420, &MFVideoFormat_IYUV, AV_PIX_FMT_YUV420P},
    {WG_VIDEO_FORMAT_NV12, &MFVideoFormat_NV12, AV_PIX_FMT_NV12},
    {WG_VIDEO_FORMAT_P010_10LE, &MFVideoFormat_P010, AV_PIX_FMT_P010LE},
    {WG_VIDEO_FORMAT_UYVY, &MFVideoFormat_UYVY, AV_PIX_FMT_UYVY422},
    {WG_VIDEO_FORMAT_YUY2, &MFVideoFormat_YUY2, AV_PIX_FMT_YUYV422},
    {WG_VIDEO_FORMAT_YV12, &MFVideoFormat_YV12, AV_PIX_FMT_YUV420P, true},
    {WG_VIDEO_FORMAT_YVYU, &MFVideoFormat_YVYU, AV_PIX_FMT_YVYU422},
};

enum AVPixelFormat wg_ffmpeg_pix_fmt_from_subtype(const GUID *subtype, bool *swap_uv)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(video_formats); ++i)
        if (IsEqualGUID(subtype, video_formats[i].subtype))
        {
            *swap_uv = video_formats[i].swap_uv;
            return video_formats[i].pix_fmt;
        }
    return AV_PIX_FMT_NONE;
}

enum AVPixelFormat wg_ffmpeg_pix_fmt_from_video_format(wg_video_format format, bool *swap_uv)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(video_formats); ++i)
        if (video_formats[i].format == format)
        {
            *swap_uv = video_formats[i].swap_uv;
            return video_formats[i].pix_fmt;
        }
    return AV_PIX_FMT_NONE;
}

const GUID *wg_ffmpeg_subtype_from_video_format(wg_video_format format)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(video_formats); ++i)
        if (video_formats[i].format == format) return video_formats[i].subtype;
    return NULL;
}

wg_video_format wg_ffmpeg_video_format_from_subtype(const GUID *subtype)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(video_formats); ++i)
        if (IsEqualGUID(subtype, video_formats[i].subtype)) return video_formats[i].format;
    return WG_VIDEO_FORMAT_UNKNOWN;
}

#define ROUND_UP(x, n) (((x) + (n) - 1) & ~((n) - 1))

/* gst_video_info_align's default strides (rows rounded up to 4 bytes) and
 * plane heights, for a padded frame of width x height */
static int plane_geometry(enum AVPixelFormat fmt, int width, int height, int stride[4], int rows[4])
{
    switch (fmt)
    {
    case AV_PIX_FMT_YUV420P:
        stride[0] = ROUND_UP(width, 4); rows[0] = height;
        stride[1] = stride[2] = ROUND_UP(ROUND_UP(width, 2) / 2, 4); rows[1] = rows[2] = ROUND_UP(height, 2) / 2;
        return 3;
    case AV_PIX_FMT_NV12:
        stride[0] = stride[1] = ROUND_UP(width, 4); rows[0] = height; rows[1] = ROUND_UP(height, 2) / 2;
        return 2;
    case AV_PIX_FMT_P010LE:
        stride[0] = stride[1] = ROUND_UP(width * 2, 4); rows[0] = height; rows[1] = ROUND_UP(height, 2) / 2;
        return 2;
    case AV_PIX_FMT_YUYV422: case AV_PIX_FMT_UYVY422: case AV_PIX_FMT_YVYU422:
        stride[0] = ROUND_UP(ROUND_UP(width, 2) * 2, 4); rows[0] = height;
        return 1;
    case AV_PIX_FMT_BGRA: case AV_PIX_FMT_BGR0: case AV_PIX_FMT_RGBA: case AV_PIX_FMT_VUYA:
        stride[0] = width * 4; rows[0] = height;
        return 1;
    case AV_PIX_FMT_BGR24:
        stride[0] = ROUND_UP(width * 3, 4); rows[0] = height;
        return 1;
    case AV_PIX_FMT_RGB555LE: case AV_PIX_FMT_RGB565LE:
        stride[0] = ROUND_UP(width * 2, 4); rows[0] = height;
        return 1;
    default:
        return 0;
    }
}

static int pixel_stride(enum AVPixelFormat fmt)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    return desc ? desc->comp[0].step : 1;
}

bool wg_ffmpeg_video_layout(enum AVPixelFormat fmt, int width, int height, const MFVideoInfo *info,
        UINT32 plane_align, INT32 sample_stride, struct wg_ffmpeg_layout *layout)
{
    bool bottom_up = info && (info->VideoFlags & MFVideoFlag_BottomUpLinearRep);
    int pad_right, pad_bottom, pad_left = 0, pad_top = 0, w, h, rows[4], i;
    int64_t offset = 0;

    pad_right = ((plane_align + 1) - (width & plane_align)) & plane_align;
    pad_bottom = ((plane_align + 1) - (height & plane_align)) & plane_align;
    if (info && !is_mf_video_area_empty(&info->MinimumDisplayAperture))
    {
        const MFVideoArea *aperture = &info->MinimumDisplayAperture;
        pad_right = max(pad_right, (int)info->dwWidth - aperture->OffsetX.value - aperture->Area.cx);
        pad_bottom = max(pad_bottom, (int)info->dwHeight - aperture->OffsetY.value - aperture->Area.cy);
        /* (as the GStreamer side, which swaps them) */
        pad_top = aperture->OffsetX.value;
        pad_left = aperture->OffsetY.value;
    }
    if (sample_stride)
    {
        int ps = pixel_stride(fmt), s;
        bottom_up = sample_stride < 0;
        s = abs(sample_stride) / ps;
        if (s >= pad_left + width + pad_right) pad_right += s - (pad_left + width + pad_right);
        else ERR("Invalid stride %d\n", sample_stride);
    }
    if (bottom_up)
    {
        int t = pad_top; pad_top = pad_bottom; pad_bottom = t;
    }

    w = pad_left + width + pad_right;
    h = pad_top + height + pad_bottom;
    memset(layout, 0, sizeof(*layout));
    if (!(layout->planes = plane_geometry(fmt, w, h, layout->stride, rows))) return false;

    if (fmt == AV_PIX_FMT_NV12 && !plane_align && (width & 3) && (width & 3) != 3
            && !pad_left && !pad_top && !pad_right && !pad_bottom)
    {
        /* NV12's stride is 2-aligned on Windows (the GStreamer side's fix_nv12) */
        layout->stride[0] = layout->stride[1] = ROUND_UP(width, 2);
        rows[0] = ROUND_UP(height, 2);
        rows[1] = rows[0] / 2;
    }
    else if (plane_align)
    {
        for (i = 0; i < 1; ++i) layout->stride[i] = (layout->stride[i] + plane_align) & ~plane_align;
    }

    for (i = 0; i < layout->planes; ++i)
    {
        layout->offset[i] = offset;
        offset += (int64_t)layout->stride[i] * rows[i];
    }
    layout->size = offset;
    layout->left = pad_left;
    layout->top = pad_top;

    if (bottom_up)
    {
        for (i = 0; i < layout->planes; ++i)
        {
            layout->offset[i] += (int64_t)layout->stride[i] * (rows[i] - 1);
            layout->stride[i] = -layout->stride[i];
        }
    }
    return true;
}

void wg_ffmpeg_layout_planes(enum AVPixelFormat fmt, bool swap_uv, const struct wg_ffmpeg_layout *layout,
        BYTE *base, uint8_t *planes[4], int strides[4])
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    int i;

    for (i = 0; i < 4; ++i) { planes[i] = NULL; strides[i] = 0; }
    for (i = 0; i < layout->planes; ++i)
    {
        int64_t x = layout->left, y = layout->top;
        if (i && desc) { x >>= desc->log2_chroma_w; y >>= desc->log2_chroma_h; }
        planes[i] = base + layout->offset[i] + y * layout->stride[i] + x * pixel_stride(fmt);
        strides[i] = layout->stride[i];
    }
    if (swap_uv)
    {
        uint8_t *p = planes[1]; int s = strides[1];
        planes[1] = planes[2]; strides[1] = strides[2];
        planes[2] = p; strides[2] = s;
    }
}

bool wg_ffmpeg_copy_frame(struct SwsContext **sws, const AVFrame *frame, enum AVPixelFormat fmt,
        bool swap_uv, const struct wg_ffmpeg_layout *layout, BYTE *dst)
{
    uint8_t *planes[4];
    int strides[4];

    wg_ffmpeg_layout_planes(fmt, swap_uv, layout, dst, planes, strides);
    if (!(*sws = sws_getCachedContext(*sws, frame->width, frame->height, frame->format,
            frame->width, frame->height, fmt, SWS_BILINEAR, NULL, NULL, NULL)))
        return false;
    return sws_scale(*sws, (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height,
            planes, strides) > 0;
}

/* WinMM's channel mask of a layout (FFmpeg's native order follows WinMM's
 * bits); an unspecified one's is the default for its channel count, as
 * GStreamer's fallback mask (ASF's WMA streams name none) */
static uint32_t channel_mask(const AVChannelLayout *layout)
{
    AVChannelLayout def;

    if (layout->order == AV_CHANNEL_ORDER_NATIVE) return (uint32_t)layout->u.mask;
    if (layout->order != AV_CHANNEL_ORDER_UNSPEC || layout->nb_channels <= 0) return 0;
    av_channel_layout_default(&def, layout->nb_channels);
    return def.order == AV_CHANNEL_ORDER_NATIVE ? (uint32_t)def.u.mask : 0;
}

enum AVSampleFormat wg_ffmpeg_sample_fmt(wg_audio_format format, int *bytes)
{
    switch (format)
    {
    case WG_AUDIO_FORMAT_U8: *bytes = 1; return AV_SAMPLE_FMT_U8;
    case WG_AUDIO_FORMAT_S16LE: *bytes = 2; return AV_SAMPLE_FMT_S16;
    case WG_AUDIO_FORMAT_S32LE: *bytes = 4; return AV_SAMPLE_FMT_S32;
    case WG_AUDIO_FORMAT_F32LE: *bytes = 4; return AV_SAMPLE_FMT_FLT;
    case WG_AUDIO_FORMAT_F64LE: *bytes = 8; return AV_SAMPLE_FMT_DBL;
    default: *bytes = 0; return AV_SAMPLE_FMT_NONE;
    }
}

static wg_audio_format audio_format_from_sample_fmt(enum AVSampleFormat fmt)
{
    switch (av_get_packed_sample_fmt(fmt))
    {
    case AV_SAMPLE_FMT_U8: return WG_AUDIO_FORMAT_U8;
    case AV_SAMPLE_FMT_S16: return WG_AUDIO_FORMAT_S16LE;
    case AV_SAMPLE_FMT_S32: return WG_AUDIO_FORMAT_S32LE;
    case AV_SAMPLE_FMT_DBL: return WG_AUDIO_FORMAT_F64LE;
    default: return WG_AUDIO_FORMAT_F32LE;
    }
}

static wg_video_format video_format_from_pix_fmt(enum AVPixelFormat fmt)
{
    switch (fmt)
    {
    case AV_PIX_FMT_NV12: return WG_VIDEO_FORMAT_NV12;
    case AV_PIX_FMT_YUYV422: return WG_VIDEO_FORMAT_YUY2;
    case AV_PIX_FMT_UYVY422: return WG_VIDEO_FORMAT_UYVY;
    case AV_PIX_FMT_BGRA: return WG_VIDEO_FORMAT_BGRA;
    case AV_PIX_FMT_BGR0: return WG_VIDEO_FORMAT_BGRx;
    case AV_PIX_FMT_BGR24: return WG_VIDEO_FORMAT_BGR;
    case AV_PIX_FMT_RGB555LE: return WG_VIDEO_FORMAT_RGB15;
    case AV_PIX_FMT_RGB565LE: return WG_VIDEO_FORMAT_RGB16;
    default: return WG_VIDEO_FORMAT_I420;
    }
}

static void set_fps(struct wg_format *format, AVRational fps)
{
    if (fps.num > 0 && fps.den > 0)
    {
        format->u.video.fps_n = fps.num;
        format->u.video.fps_d = fps.den;
    }
    else
    {
        format->u.video.fps_n = 0;
        format->u.video.fps_d = 1;
    }
}

void wg_ffmpeg_format_from_frame(const AVFrame *frame, enum AVMediaType type, AVRational fps, struct wg_format *format)
{
    memset(format, 0, sizeof(*format));
    if (type == AVMEDIA_TYPE_AUDIO)
    {
        format->major_type = WG_MAJOR_TYPE_AUDIO;
        format->u.audio.format = audio_format_from_sample_fmt(frame->format);
        format->u.audio.channels = frame->ch_layout.nb_channels;
        format->u.audio.channel_mask = channel_mask(&frame->ch_layout);
        format->u.audio.rate = frame->sample_rate;
    }
    else if (type == AVMEDIA_TYPE_VIDEO)
    {
        format->major_type = WG_MAJOR_TYPE_VIDEO;
        format->u.video.format = video_format_from_pix_fmt(frame->format);
        format->u.video.width = frame->width;
        format->u.video.height = frame->height;
        set_fps(format, fps);
    }
}

void wg_ffmpeg_format_from_codec(const AVCodecParameters *par, AVRational fps, struct wg_format *format)
{
    memset(format, 0, sizeof(*format));
    if (par->codec_type == AVMEDIA_TYPE_AUDIO)
    {
        format->u.audio.channels = par->ch_layout.nb_channels;
        format->u.audio.channel_mask = channel_mask(&par->ch_layout);
        format->u.audio.rate = par->sample_rate;
        format->u.audio.bitrate = par->bit_rate;
        format->u.audio.block_align = par->block_align;
        format->u.audio.depth = par->bits_per_coded_sample;
        switch (par->codec_id)
        {
        case AV_CODEC_ID_MP1: case AV_CODEC_ID_MP2: case AV_CODEC_ID_MP3:
            format->major_type = WG_MAJOR_TYPE_AUDIO_MPEG1;
            format->u.audio.layer = par->codec_id == AV_CODEC_ID_MP1 ? 1 : par->codec_id == AV_CODEC_ID_MP2 ? 2 : 3;
            break;
        case AV_CODEC_ID_AAC:
            format->major_type = WG_MAJOR_TYPE_AUDIO_MPEG4;
            format->u.audio.payload_type = par->extradata_size ? 0 : 1;
            break;
        case AV_CODEC_ID_WMAV1: case AV_CODEC_ID_WMAV2: case AV_CODEC_ID_WMAPRO: case AV_CODEC_ID_WMALOSSLESS:
            format->major_type = WG_MAJOR_TYPE_AUDIO_WMA;
            format->u.audio.version = par->codec_id == AV_CODEC_ID_WMAV1 ? 1 : par->codec_id == AV_CODEC_ID_WMAV2 ? 2
                    : par->codec_id == AV_CODEC_ID_WMAPRO ? 3 : 4;
            break;
        default:
            format->major_type = WG_MAJOR_TYPE_UNKNOWN;
            return;
        }
        if (par->extradata_size <= sizeof(format->u.audio.codec_data))
        {
            format->u.audio.codec_data_len = par->extradata_size;
            if (par->extradata_size) memcpy(format->u.audio.codec_data, par->extradata, par->extradata_size);
        }
    }
    else if (par->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        format->u.video.width = par->width;
        format->u.video.height = par->height;
        set_fps(format, fps);
        switch (par->codec_id)
        {
        case AV_CODEC_ID_H264:
            format->major_type = WG_MAJOR_TYPE_VIDEO_H264;
            format->u.video.profile = par->profile;
            format->u.video.level = par->level;
            break;
        case AV_CODEC_ID_WMV1: case AV_CODEC_ID_WMV2: case AV_CODEC_ID_WMV3: case AV_CODEC_ID_VC1:
            format->major_type = WG_MAJOR_TYPE_VIDEO_WMV;
            format->u.video.format = par->codec_id == AV_CODEC_ID_WMV1 ? WG_VIDEO_FORMAT_WMV1
                    : par->codec_id == AV_CODEC_ID_WMV2 ? WG_VIDEO_FORMAT_WMV2
                    : par->codec_id == AV_CODEC_ID_WMV3 ? WG_VIDEO_FORMAT_WMV3
                    : par->codec_tag == MKTAG('W','M','V','A') ? WG_VIDEO_FORMAT_WMVA : WG_VIDEO_FORMAT_WVC1;
            break;
        case AV_CODEC_ID_CINEPAK:
            format->major_type = WG_MAJOR_TYPE_VIDEO_CINEPAK;
            break;
        case AV_CODEC_ID_INDEO5:
            format->major_type = WG_MAJOR_TYPE_VIDEO_INDEO;
            format->u.video.version = 5;
            break;
        case AV_CODEC_ID_MPEG1VIDEO:
            format->major_type = WG_MAJOR_TYPE_VIDEO_MPEG1;
            break;
        default:
            format->major_type = WG_MAJOR_TYPE_UNKNOWN;
            return;
        }
        if (par->extradata_size <= sizeof(format->u.video.codec_data))
        {
            format->u.video.codec_data_len = par->extradata_size;
            if (par->extradata_size) memcpy(format->u.video.codec_data, par->extradata, par->extradata_size);
        }
    }
}
