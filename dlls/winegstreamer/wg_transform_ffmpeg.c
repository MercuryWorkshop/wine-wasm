/*
 * winegstreamer's Unix side over FFmpeg: wg_transform (decoders) over libavcodec
 *
 * The same calls, media types and sample flags as wg_transform.c's
 * GStreamer pipeline, so that the PE side (aac_decoder.c, video_decoder.c,
 * wma_decoder.c, quartz_transform.c) sees no difference. A raw input type
 * (video_processor.c, color_convert.c, resampler.c) is converted alone,
 * through libswscale or libswresample.
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
#include "mferror.h"
#include "mfapi.h"
#include "mmreg.h"

#include "wg_ffmpeg.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

#define MAX_QUEUE 64

struct wg_transform
{
    struct wg_transform_attrs attrs;
    bool video;

    AVCodecContext *codec;  /* NULL: raw input, converted alone */
    /* raw input: its pixel format (and size and layout) or sample format */
    enum AVPixelFormat in_pix_fmt;
    bool in_swap_uv;
    int in_width, in_height;
    MFVideoInfo in_info;
    enum AVSampleFormat in_sample_fmt;
    int in_sample_bytes, in_channels, in_rate;
    AVPacket *queue[MAX_QUEUE];
    unsigned int queue_len;
    bool draining, flushing_decoder;
    INT64 ts_offset;

    /* the output type: an audio format or MFVIDEOFORMAT (no codec data), as
     * the caller set it, kept up to date with the decoded stream */
    BYTE *output;
    UINT32 output_size;

    /* video: the requested pixel format; audio: the requested sample format */
    enum AVPixelFormat pix_fmt;
    bool swap_uv;
    struct SwsContext *sws;
    enum AVSampleFormat sample_fmt;
    int sample_bytes, channels, rate;
    SwrContext *swr;

    AVFrame *frame;
    bool frame_changed;
    /* audio: the pending frame converted, served across reads */
    BYTE *pcm;
    size_t pcm_size, pcm_pos;
    INT64 pcm_pts;
};

static struct wg_transform *get_transform(wg_transform_t trans)
{
    return (struct wg_transform *)(ULONG_PTR)trans;
}

static WAVEFORMATEX *output_audio(struct wg_transform *t)
{
    return (WAVEFORMATEX *)t->output;
}

static MFVIDEOFORMAT *output_video(struct wg_transform *t)
{
    return (MFVIDEOFORMAT *)t->output;
}

/* the audio output format, as caps_to_media_type makes it from the decoder's
 * output caps: WAVEFORMATEXTENSIBLE when there is a channel mask (more than
 * one channel), else WAVEFORMATEX */
static bool build_wave_format(struct wg_transform *t, WORD tag, const GUID *subtype, int depth, int channels, int rate)
{
    UINT64 mask = 0;
    UINT32 size;
    WAVEFORMATEX *wfx;

    if (channels > 1)
    {
        AVChannelLayout layout;
        av_channel_layout_default(&layout, channels);
        if (layout.order == AV_CHANNEL_ORDER_NATIVE) mask = layout.u.mask;
        av_channel_layout_uninit(&layout);
    }
    size = mask ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX);
    if (!(wfx = calloc(1, size))) return false;
    wfx->wFormatTag = mask ? WAVE_FORMAT_EXTENSIBLE : tag;
    wfx->nChannels = channels;
    wfx->nSamplesPerSec = rate;
    wfx->wBitsPerSample = depth;
    wfx->nBlockAlign = depth * channels / 8;
    wfx->nAvgBytesPerSec = rate * wfx->nBlockAlign;
    wfx->cbSize = size - sizeof(WAVEFORMATEX);
    if (mask)
    {
        WAVEFORMATEXTENSIBLE *ext = (WAVEFORMATEXTENSIBLE *)wfx;
        ext->Samples.wValidBitsPerSample = 0;
        ext->dwChannelMask = mask;
        ext->SubFormat = *subtype;
    }
    free(t->output);
    t->output = (BYTE *)wfx;
    t->output_size = size;
    return true;
}

/* the video output format, as caps_to_media_type makes it: dimensions
 * rounded up to the plane alignment, the picture as the aperture */
static bool build_video_format(struct wg_transform *t, const GUID *subtype, int width, int height,
        MFRatio par, MFRatio fps, UINT32 flags)
{
    UINT32 align = t->attrs.output_plane_align;
    MFVIDEOFORMAT *format;

    if (!(format = calloc(1, sizeof(*format)))) return false;
    format->dwSize = sizeof(*format);
    format->guidFormat = *subtype;
    format->videoInfo.dwWidth = (width + align) & ~align;
    format->videoInfo.dwHeight = (height + align) & ~align;
    if (format->videoInfo.dwWidth != width || format->videoInfo.dwHeight != height)
    {
        format->videoInfo.MinimumDisplayAperture.Area.cx = width;
        format->videoInfo.MinimumDisplayAperture.Area.cy = height;
    }
    format->videoInfo.GeometricAperture = format->videoInfo.MinimumDisplayAperture;
    format->videoInfo.PanScanAperture = format->videoInfo.MinimumDisplayAperture;
    format->videoInfo.PixelAspectRatio = par;
    format->videoInfo.FramesPerSecond = fps;
    format->videoInfo.VideoFlags = flags;
    free(t->output);
    t->output = (BYTE *)format;
    t->output_size = sizeof(*format);
    return true;
}

static void video_size_of(const MFVIDEOFORMAT *format, int *width, int *height)
{
    *width = format->videoInfo.dwWidth;
    *height = format->videoInfo.dwHeight;
    if (!is_mf_video_area_empty(&format->videoInfo.MinimumDisplayAperture))
    {
        *width = format->videoInfo.MinimumDisplayAperture.Area.cx;
        *height = format->videoInfo.MinimumDisplayAperture.Area.cy;
    }
}

/* the requested output type: raw audio or video; false if it is not one */
static bool set_output(struct wg_transform *t, const struct wg_media_type *type)
{
    if (IsEqualGUID(&type->major, &MFMediaType_Audio) && type->format_size >= sizeof(WAVEFORMATEX))
    {
        const WAVEFORMATEX *wfx = type->u.audio;
        GUID subtype = MFAudioFormat_Base;
        wg_audio_format format = WG_AUDIO_FORMAT_UNKNOWN;
        WORD tag;

        subtype.Data1 = wfx->wFormatTag;
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && type->format_size >= sizeof(WAVEFORMATEXTENSIBLE))
            subtype = ((const WAVEFORMATEXTENSIBLE *)wfx)->SubFormat;
        if (IsEqualGUID(&subtype, &MFAudioFormat_PCM))
            format = wfx->wBitsPerSample == 8 ? WG_AUDIO_FORMAT_U8 : wfx->wBitsPerSample == 16 ? WG_AUDIO_FORMAT_S16LE
                    : wfx->wBitsPerSample == 32 ? WG_AUDIO_FORMAT_S32LE : WG_AUDIO_FORMAT_UNKNOWN;
        else if (IsEqualGUID(&subtype, &MFAudioFormat_Float))
            format = wfx->wBitsPerSample == 64 ? WG_AUDIO_FORMAT_F64LE : wfx->wBitsPerSample == 32 ? WG_AUDIO_FORMAT_F32LE
                    : WG_AUDIO_FORMAT_UNKNOWN;
        if ((t->sample_fmt = wg_ffmpeg_sample_fmt(format, &t->sample_bytes)) == AV_SAMPLE_FMT_NONE)
            return false;
        t->channels = wfx->nChannels;
        t->rate = wfx->nSamplesPerSec;
        tag = IsEqualGUID(&subtype, &MFAudioFormat_Float) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        swr_free(&t->swr);
        return build_wave_format(t, tag, &subtype, t->sample_bytes * 8, t->channels, t->rate);
    }
    if (IsEqualGUID(&type->major, &MFMediaType_Video) && type->format_size >= sizeof(MFVIDEOFORMAT))
    {
        const MFVIDEOFORMAT *format = type->u.video;
        int width, height;

        if ((t->pix_fmt = wg_ffmpeg_pix_fmt_from_subtype(&format->guidFormat, &t->swap_uv)) == AV_PIX_FMT_NONE)
            return false;
        video_size_of(format, &width, &height);
        return build_video_format(t, &format->guidFormat, width, height, format->videoInfo.PixelAspectRatio,
                format->videoInfo.FramesPerSecond, format->videoInfo.VideoFlags);
    }
    return false;
}

static void clear_queue(struct wg_transform *t)
{
    while (t->queue_len) { --t->queue_len; if (t->queue[t->queue_len]) av_packet_free(&t->queue[t->queue_len]); }
}

static void clear_output(struct wg_transform *t)
{
    av_frame_unref(t->frame);
    t->frame_changed = false;
    free(t->pcm);
    t->pcm = NULL;
    t->pcm_size = t->pcm_pos = 0;
}

/* a raw input type: true, with its format kept, if it is one */
static bool set_raw_input(struct wg_transform *t, const struct wg_media_type *type)
{
    if (IsEqualGUID(&type->major, &MFMediaType_Audio) && type->format_size >= sizeof(WAVEFORMATEX))
    {
        const WAVEFORMATEX *wfx = type->u.audio;
        GUID subtype = MFAudioFormat_Base;
        wg_audio_format format = WG_AUDIO_FORMAT_UNKNOWN;

        subtype.Data1 = wfx->wFormatTag;
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && type->format_size >= sizeof(WAVEFORMATEXTENSIBLE))
            subtype = ((const WAVEFORMATEXTENSIBLE *)wfx)->SubFormat;
        if (IsEqualGUID(&subtype, &MFAudioFormat_PCM))
            format = wfx->wBitsPerSample == 8 ? WG_AUDIO_FORMAT_U8 : wfx->wBitsPerSample == 16 ? WG_AUDIO_FORMAT_S16LE
                    : wfx->wBitsPerSample == 32 ? WG_AUDIO_FORMAT_S32LE : WG_AUDIO_FORMAT_UNKNOWN;
        else if (IsEqualGUID(&subtype, &MFAudioFormat_Float))
            format = wfx->wBitsPerSample == 64 ? WG_AUDIO_FORMAT_F64LE : wfx->wBitsPerSample == 32 ? WG_AUDIO_FORMAT_F32LE
                    : WG_AUDIO_FORMAT_UNKNOWN;
        else return false;
        if ((t->in_sample_fmt = wg_ffmpeg_sample_fmt(format, &t->in_sample_bytes)) == AV_SAMPLE_FMT_NONE
                || !wfx->nChannels || !wfx->nSamplesPerSec)
            return false;
        t->in_channels = wfx->nChannels;
        t->in_rate = wfx->nSamplesPerSec;
        return true;
    }
    if (IsEqualGUID(&type->major, &MFMediaType_Video) && type->format_size >= sizeof(MFVIDEOFORMAT))
    {
        const MFVIDEOFORMAT *format = type->u.video;

        if ((t->in_pix_fmt = wg_ffmpeg_pix_fmt_from_subtype(&format->guidFormat, &t->in_swap_uv)) == AV_PIX_FMT_NONE)
            return false;
        video_size_of(format, &t->in_width, &t->in_height);
        t->in_info = format->videoInfo;
        return true;
    }
    return false;
}

/* a raw input sample as a frame (over the packet's memory) */
static bool frame_from_raw(struct wg_transform *t, AVPacket *pkt)
{
    AVFrame *f = t->frame;

    if (t->video)
    {
        struct wg_ffmpeg_layout layout;

        if (!wg_ffmpeg_video_layout(t->in_pix_fmt, t->in_width, t->in_height, &t->in_info, 0, 0, &layout)
                || pkt->size < layout.size)
        {
            WARN("Input sample of %d bytes is too small.\n", pkt->size);
            return false;
        }
        wg_ffmpeg_layout_planes(t->in_pix_fmt, t->in_swap_uv, &layout, pkt->data, f->data, f->linesize);
        f->format = t->in_pix_fmt;
        f->width = t->in_width;
        f->height = t->in_height;
    }
    else
    {
        int block = t->in_sample_bytes * t->in_channels;

        if (!(f->nb_samples = pkt->size / block)) return false;
        f->data[0] = pkt->data;
        f->linesize[0] = f->nb_samples * block;
        f->extended_data = f->data;
        f->format = t->in_sample_fmt;
        f->sample_rate = t->in_rate;
        av_channel_layout_default(&f->ch_layout, t->in_channels);
    }
    if (!(f->buf[0] = av_buffer_ref(pkt->buf))) return false;
    f->pts = f->best_effort_timestamp = pkt->pts;
    f->duration = pkt->duration;
    f->flags = AV_FRAME_FLAG_KEY;
    return true;
}

NTSTATUS wg_transform_create(void *args)
{
    struct wg_transform_create_params *params = args;
    AVCodecParameters *par = NULL;
    const AVCodec *codec;
    struct wg_transform *t;
    char *threads;

    if (!(t = calloc(1, sizeof(*t)))) return STATUS_NO_MEMORY;
    t->attrs = params->attrs;
    t->video = IsEqualGUID(&params->output_type.major, &MFMediaType_Video);

    if (!(par = avcodec_parameters_alloc()) || !(t->frame = av_frame_alloc()))
        goto fail;
    if (set_raw_input(t, &params->input_type))
    {
        if (!set_output(t, &params->output_type))
        {
            FIXME("Unsupported output type for a raw input.\n");
            goto fail;
        }
        avcodec_parameters_free(&par);
        TRACE("Created transform %p, raw input.\n", t);
        params->transform = (wg_transform_t)(ULONG_PTR)t;
        return STATUS_SUCCESS;
    }
    if (!wg_ffmpeg_codec_from_media_type(&params->input_type, par))
    {
        FIXME("Unsupported input type %s.\n", debugstr_guid(&params->input_type.major));
        goto fail;
    }
    if (!set_output(t, &params->output_type))
    {
        FIXME("Unsupported output type (an encoder or converter).\n");
        goto fail;
    }
    if (!(codec = avcodec_find_decoder(par->codec_id)))
    {
        WARN("No decoder for %s.\n", avcodec_get_name(par->codec_id));
        goto fail;
    }
    if (!(t->codec = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(t->codec, par) < 0)
        goto fail;
    t->codec->pkt_timebase = (AVRational){1, 10000000};
    t->codec->thread_count = wg_ffmpeg_threads;
    if ((threads = getenv("WINE_WG_FFMPEG_THREADS"))) t->codec->thread_count = atoi(threads);
    if (t->attrs.low_latency) t->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(t->codec, codec, NULL) < 0)
    {
        WARN("Failed to open %s.\n", codec->name);
        goto fail;
    }
    avcodec_parameters_free(&par);
    TRACE("Created transform %p, %s.\n", t, codec->name);
    params->transform = (wg_transform_t)(ULONG_PTR)t;
    return STATUS_SUCCESS;

fail:
    avcodec_parameters_free(&par);
    avcodec_free_context(&t->codec);
    av_frame_free(&t->frame);
    free(t->output);
    free(t);
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS wg_transform_destroy(void *args)
{
    struct wg_transform *t = get_transform(*(wg_transform_t *)args);

    clear_queue(t);
    clear_output(t);
    av_frame_free(&t->frame);
    avcodec_free_context(&t->codec);
    sws_freeContext(t->sws);
    swr_free(&t->swr);
    free(t->output);
    free(t);
    return STATUS_SUCCESS;
}

NTSTATUS wg_transform_get_output_type(void *args)
{
    struct wg_transform_get_output_type_params *params = args;
    struct wg_transform *t = get_transform(params->transform);
    UINT32 capacity = params->media_type.format_size;

    params->media_type.major = t->video ? MFMediaType_Video : MFMediaType_Audio;
    params->media_type.format_size = t->output_size;
    if (capacity < t->output_size) return STATUS_BUFFER_TOO_SMALL;
    memcpy(params->media_type.u.format, t->output, t->output_size);
    return STATUS_SUCCESS;
}

NTSTATUS wg_transform_set_output_type(void *args)
{
    struct wg_transform_set_output_type_params *params = args;
    struct wg_transform *t = get_transform(params->transform);

    if (!set_output(t, &params->media_type))
    {
        ERR("Failed to set the output type.\n");
        return STATUS_UNSUCCESSFUL;
    }
    /* (the GStreamer side drops queued output on a reconfigure too) */
    clear_output(t);
    return STATUS_SUCCESS;
}

NTSTATUS wg_transform_push_data(void *args)
{
    struct wg_transform_push_data_params *params = args;
    struct wg_transform *t = get_transform(params->transform);
    struct wg_sample *sample = params->sample;
    AVPacket *pkt;

    if (t->draining || t->queue_len >= min(t->attrs.input_queue_length + 1, MAX_QUEUE - 1))
    {
        params->result = MF_E_NOTACCEPTING;
        return STATUS_SUCCESS;
    }
    if (!(pkt = av_packet_alloc()) || av_new_packet(pkt, sample->size) < 0)
    {
        av_packet_free(&pkt);
        return STATUS_NO_MEMORY;
    }
    memcpy(pkt->data, wg_sample_data(sample), sample->size);
    if (sample->flags & WG_SAMPLE_FLAG_HAS_PTS)
    {
        if (sample->pts < t->ts_offset) t->ts_offset = sample->pts;
        pkt->pts = sample->pts - t->ts_offset;
    }
    if (sample->flags & WG_SAMPLE_FLAG_HAS_DURATION) pkt->duration = sample->duration;
    if (sample->flags & WG_SAMPLE_FLAG_SYNC_POINT) pkt->flags |= AV_PKT_FLAG_KEY;
    t->queue[t->queue_len++] = pkt;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* the next decoded frame into t->frame: true if there is one. A NULL in the
 * queue is a drain: the decoder is flushed there (its last frames come out),
 * then takes the packets after it as a new stream, as the GStreamer side's
 * EOS and stream-start. */
static bool decode_next(struct wg_transform *t)
{
    int ret;

    while (!t->codec)
    {
        AVPacket *pkt;
        bool ok;

        if (!t->queue_len) return false;
        pkt = t->queue[0];
        memmove(t->queue, t->queue + 1, --t->queue_len * sizeof(*t->queue));
        if (!pkt) { t->draining = false; continue; }
        ok = frame_from_raw(t, pkt);
        av_packet_free(&pkt);
        if (ok) return true;
        av_frame_unref(t->frame);
    }
    for (;;)
    {
        if (!(ret = avcodec_receive_frame(t->codec, t->frame))) return true;
        if (ret == AVERROR_EOF)
        {
            avcodec_flush_buffers(t->codec);
            t->flushing_decoder = false;
        }
        else if (ret != AVERROR(EAGAIN))
            WARN("Decoding failed, %d.\n", ret);
        else if (t->flushing_decoder)
            continue;
        if (!t->queue_len) return false;
        {
            AVPacket *pkt = t->queue[0];
            memmove(t->queue, t->queue + 1, --t->queue_len * sizeof(*t->queue));
            if (!pkt)
            {
                t->draining = false;
                t->flushing_decoder = true;
                avcodec_send_packet(t->codec, NULL);
                continue;
            }
            if ((ret = avcodec_send_packet(t->codec, pkt)) < 0 && ret != AVERROR(EAGAIN))
                WARN("Failed to send a packet, %d.\n", ret);
            av_packet_free(&pkt);
        }
    }
}

/* whether the frame changes the output type (and if so, the new one) */
static bool update_output_for_frame(struct wg_transform *t)
{
    const AVFrame *f = t->frame;

    if (t->video)
    {
        MFVIDEOFORMAT *format = output_video(t);
        MFRatio par = format->videoInfo.PixelAspectRatio, fps = format->videoInfo.FramesPerSecond;
        int width, height;

        video_size_of(format, &width, &height);
        if (f->width == width && f->height == height) return false;
        if (f->sample_aspect_ratio.num > 0) par = (MFRatio){f->sample_aspect_ratio.num, f->sample_aspect_ratio.den};
        if (t->codec && t->codec->framerate.num > 0) fps = (MFRatio){t->codec->framerate.num, t->codec->framerate.den};
        build_video_format(t, &format->guidFormat, f->width, f->height, par, fps, format->videoInfo.VideoFlags);
        return true;
    }
    else
    {
        WAVEFORMATEX *wfx = output_audio(t);
        GUID subtype = MFAudioFormat_PCM;

        if (f->sample_rate == t->rate && f->ch_layout.nb_channels == t->channels) return false;
        if (!t->attrs.allow_format_change) return false;  /* resampled to the type set */
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) subtype = ((WAVEFORMATEXTENSIBLE *)wfx)->SubFormat;
        else if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) subtype = MFAudioFormat_Float;
        t->rate = f->sample_rate;
        t->channels = f->ch_layout.nb_channels;
        swr_free(&t->swr);
        build_wave_format(t, IsEqualGUID(&subtype, &MFAudioFormat_Float) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM,
                &subtype, t->sample_bytes * 8, t->channels, t->rate);
        return true;
    }
}

static bool convert_audio(struct wg_transform *t)
{
    const AVFrame *f = t->frame;
    AVChannelLayout out_layout;
    uint8_t *out[1];
    int n, max_out;

    av_channel_layout_default(&out_layout, t->channels);
    if (!t->swr)
    {
        if (swr_alloc_set_opts2(&t->swr, &out_layout, t->sample_fmt, t->rate,
                &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0 || swr_init(t->swr) < 0)
        {
            av_channel_layout_uninit(&out_layout);
            swr_free(&t->swr);
            return false;
        }
    }
    av_channel_layout_uninit(&out_layout);
    max_out = swr_get_out_samples(t->swr, f->nb_samples);
    free(t->pcm);
    if (!(t->pcm = malloc((size_t)max_out * t->sample_bytes * t->channels + 1))) return false;
    out[0] = t->pcm;
    if ((n = swr_convert(t->swr, out, max_out, (const uint8_t **)f->extended_data, f->nb_samples)) < 0)
        return false;
    t->pcm_size = (size_t)n * t->sample_bytes * t->channels;
    t->pcm_pos = 0;
    t->pcm_pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    return true;
}

static NTSTATUS read_audio(struct wg_transform *t, struct wg_sample *sample)
{
    size_t left = t->pcm_size - t->pcm_pos, n = min(left, (size_t)sample->max_size);
    UINT32 block = t->sample_bytes * t->channels;
    UINT64 done = t->pcm_pos / block;

    memcpy(wg_sample_data(sample), t->pcm + t->pcm_pos, n);
    sample->size = n;
    sample->flags |= WG_SAMPLE_FLAG_SYNC_POINT;
    if (t->pcm_pts != AV_NOPTS_VALUE)
    {
        sample->flags |= WG_SAMPLE_FLAG_HAS_PTS;
        sample->pts = t->pcm_pts + done * 10000000 / t->rate;
    }
    sample->flags |= WG_SAMPLE_FLAG_HAS_DURATION;
    sample->duration = (UINT64)(n / block) * 10000000 / t->rate;
    t->pcm_pos += n;
    if (t->pcm_pos < t->pcm_size) sample->flags |= WG_SAMPLE_FLAG_INCOMPLETE;
    return STATUS_SUCCESS;
}

static NTSTATUS read_video(struct wg_transform *t, struct wg_sample *sample)
{
    const AVFrame *f = t->frame;
    struct wg_ffmpeg_layout layout;
    INT64 pts;

    if (!wg_ffmpeg_video_layout(t->pix_fmt, f->width, f->height, &output_video(t)->videoInfo,
            t->attrs.output_plane_align, sample->stride, &layout))
        return STATUS_UNSUCCESSFUL;
    if (sample->max_size < layout.size)
    {
        ERR("Output buffer is too small.\n");
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (!wg_ffmpeg_copy_frame(&t->sws, f, t->pix_fmt, t->swap_uv, &layout, wg_sample_data(sample)))
    {
        ERR("Failed to convert the frame.\n");
        return STATUS_UNSUCCESSFUL;
    }
    sample->size = layout.size;
    pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    if (pts != AV_NOPTS_VALUE)
    {
        sample->flags |= WG_SAMPLE_FLAG_HAS_PTS;
        sample->pts = pts;
    }
    if (f->duration > 0)
    {
        sample->flags |= WG_SAMPLE_FLAG_HAS_DURATION;
        sample->duration = f->duration;
    }
    else if (t->codec && t->codec->framerate.num > 0)
    {
        sample->flags |= WG_SAMPLE_FLAG_HAS_DURATION;
        sample->duration = (UINT64)10000000 * t->codec->framerate.den / t->codec->framerate.num;
    }
    if (f->flags & AV_FRAME_FLAG_KEY) sample->flags |= WG_SAMPLE_FLAG_SYNC_POINT;
    return STATUS_SUCCESS;
}

NTSTATUS wg_transform_read_data(void *args)
{
    struct wg_transform_read_data_params *params = args;
    struct wg_transform *t = get_transform(params->transform);
    struct wg_sample *sample = params->sample;
    bool have = t->video ? !!t->frame->buf[0] : t->pcm_pos < t->pcm_size;
    NTSTATUS status;

    sample->flags &= ~(WG_SAMPLE_FLAG_INCOMPLETE | WG_SAMPLE_FLAG_HAS_PTS | WG_SAMPLE_FLAG_HAS_DURATION
            | WG_SAMPLE_FLAG_SYNC_POINT | WG_SAMPLE_FLAG_DISCONTINUITY);
    while (!have)
    {
        av_frame_unref(t->frame);
        if (!decode_next(t))
        {
            sample->size = 0;
            params->result = MF_E_TRANSFORM_NEED_MORE_INPUT;
            return STATUS_SUCCESS;
        }
        t->frame_changed = update_output_for_frame(t);
        if (t->video) have = true;
        else if (!convert_audio(t)) return STATUS_UNSUCCESSFUL;
        else have = t->pcm_size > 0;
    }

    if (t->frame_changed)
    {
        t->frame_changed = false;
        sample->size = 0;
        params->result = MF_E_TRANSFORM_STREAM_CHANGE;
        return STATUS_SUCCESS;
    }

    if (t->video)
    {
        if ((status = read_video(t, sample))) { sample->size = 0; return status; }
        av_frame_unref(t->frame);
    }
    else if ((status = read_audio(t, sample))) return status;

    if (t->attrs.preserve_timestamps && (sample->flags & WG_SAMPLE_FLAG_HAS_PTS))
    {
        sample->flags |= WG_SAMPLE_FLAG_PRESERVE_TIMESTAMPS;
        sample->pts += t->ts_offset;
    }
    params->result = S_OK;
    return STATUS_SUCCESS;
}

NTSTATUS wg_transform_get_status(void *args)
{
    struct wg_transform_get_status_params *params = args;
    struct wg_transform *t = get_transform(params->transform);

    params->accepts_input = t->queue_len < t->attrs.input_queue_length + 1;
    return STATUS_SUCCESS;
}

NTSTATUS wg_transform_drain(void *args)
{
    struct wg_transform *t = get_transform(*(wg_transform_t *)args);

    /* (pushes are refused while queued input is ahead of the drain) */
    if (t->queue_len >= MAX_QUEUE) return STATUS_UNSUCCESSFUL;
    t->queue[t->queue_len++] = NULL;
    t->draining = t->queue_len > 1;
    return STATUS_SUCCESS;
}

NTSTATUS wg_transform_flush(void *args)
{
    struct wg_transform *t = get_transform(*(wg_transform_t *)args);

    clear_queue(t);
    clear_output(t);
    if (t->codec) avcodec_flush_buffers(t->codec);
    if (t->swr) swr_init(t->swr);
    t->draining = t->flushing_decoder = false;
    return STATUS_SUCCESS;
}

NTSTATUS wg_transform_notify_qos(void *args)
{
    return STATUS_SUCCESS;
}
