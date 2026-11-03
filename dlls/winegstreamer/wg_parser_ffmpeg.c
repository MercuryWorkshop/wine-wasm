/*
 * winegstreamer's Unix side over FFmpeg: wg_parser (demux and decode) over libavformat and libavcodec
 *
 * The same pull protocol as wg_parser.c's GStreamer decodebin: the PE side
 * answers read requests (get_next_read_offset, push_data) from a thread of
 * its own, while a thread here demuxes and decodes, holding at most one
 * buffer per stream until the PE side releases it.
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

#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"
#include "windef.h"
#include "winbase.h"
#include "dshow.h"
#include "vfwmsgs.h"

#include "wg_ffmpeg.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

#define IO_SIZE (64 << 10)
#define TIME_100NS ((AVRational){1, 10000000})

struct queued_buffer
{
    struct queued_buffer *next;
    BYTE *data;
    struct wg_parser_buffer info;
};

/* buffers a stream holds before the demuxer waits for it to be read, unless
 * another enabled stream has none (a demuxer reading on for one stream while
 * another is not read, as GStreamer's multiqueue lets it) */
#define QUEUE_MAX 4

struct wg_parser_stream
{
    struct wg_parser *parser;
    uint32_t number;
    AVStream *av;

    struct wg_format codec_format, current_format, desired;
    bool enabled, compressed_out, eos, get_buffer_called;

    AVCodecContext *codec;
    struct SwsContext *sws;
    SwrContext *swr;
    AVFrame *frame;

    /* the buffer being made */
    BYTE *data;
    UINT32 size, capacity;
    bool discont;
    /* the buffers made and not yet released, oldest first */
    struct queued_buffer *head, **tail;
    unsigned int queued;

    uint64_t duration;
    char *tags[WG_PARSER_TAG_COUNT];
};

struct wg_parser
{
    struct wg_parser_stream **streams;
    unsigned int stream_count;
    bool output_compressed;

    pthread_t thread;
    bool thread_started;
    pthread_mutex_t mutex;
    pthread_cond_t init_cond, read_cond, read_done_cond, buffer_cond, empty_cond;

    uint64_t file_size, position;
    bool sink_connected, init_done, init_failed, running, quit;

    struct
    {
        uint64_t offset;
        uint32_t size;
        void *dst;
        bool done;
        int ret;
    } read_request;

    bool seek_pending;
    uint64_t seek_start, stop_pos;
    bool accurate_seek;
    int64_t drop_before;

    AVFormatContext *fmt;
    AVIOContext *io;
};

static struct wg_parser *get_parser(wg_parser_t parser)
{
    return (struct wg_parser *)(ULONG_PTR)parser;
}

static struct wg_parser_stream *get_stream(wg_parser_stream_t stream)
{
    return (struct wg_parser_stream *)(ULONG_PTR)stream;
}

/* avio read: a read request the PE side answers (mutex not held) */
static void flush_queue(struct wg_parser_stream *stream)
{
    struct queued_buffer *b, *next;

    for (b = stream->head; b; b = next)
    {
        next = b->next;
        free(b->data);
        free(b);
    }
    stream->head = NULL;
    stream->tail = &stream->head;
    stream->queued = 0;
    stream->get_buffer_called = false;
}

static int io_read(void *opaque, uint8_t *buf, int size)
{
    struct wg_parser *parser = opaque;
    int ret;

    pthread_mutex_lock(&parser->mutex);
    if (parser->quit || !parser->sink_connected)
    {
        pthread_mutex_unlock(&parser->mutex);
        return AVERROR_EXIT;
    }
    if (parser->position >= parser->file_size)
    {
        pthread_mutex_unlock(&parser->mutex);
        return AVERROR_EOF;
    }
    if (size > parser->file_size - parser->position) size = parser->file_size - parser->position;
    parser->read_request.offset = parser->position;
    parser->read_request.size = size;
    parser->read_request.dst = buf;
    parser->read_request.done = false;
    pthread_cond_signal(&parser->read_cond);
    while (!parser->read_request.done && !parser->quit)
        pthread_cond_wait(&parser->read_done_cond, &parser->mutex);
    ret = parser->quit ? AVERROR_EXIT : parser->read_request.ret;
    if (ret > 0) parser->position += ret;
    parser->read_request.size = 0;
    pthread_mutex_unlock(&parser->mutex);
    return ret;
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
    struct wg_parser *parser = opaque;
    int64_t pos;

    pthread_mutex_lock(&parser->mutex);
    switch (whence & ~AVSEEK_FORCE)
    {
    case AVSEEK_SIZE: pos = parser->file_size; pthread_mutex_unlock(&parser->mutex); return pos;
    case SEEK_SET: pos = offset; break;
    case SEEK_CUR: pos = parser->position + offset; break;
    case SEEK_END: pos = parser->file_size + offset; break;
    default: pthread_mutex_unlock(&parser->mutex); return AVERROR(EINVAL);
    }
    if (pos < 0 || pos > parser->file_size) { pthread_mutex_unlock(&parser->mutex); return AVERROR(EINVAL); }
    parser->position = pos;
    pthread_mutex_unlock(&parser->mutex);
    return pos;
}

NTSTATUS wg_parser_get_next_read_offset(void *args)
{
    struct wg_parser_get_next_read_offset_params *params = args;
    struct wg_parser *parser = get_parser(params->parser);

    pthread_mutex_lock(&parser->mutex);
    while (parser->sink_connected && !parser->read_request.size)
        pthread_cond_wait(&parser->read_cond, &parser->mutex);
    if (!parser->sink_connected)
    {
        pthread_mutex_unlock(&parser->mutex);
        return VFW_E_WRONG_STATE;
    }
    params->offset = parser->read_request.offset;
    params->size = parser->read_request.size;
    pthread_mutex_unlock(&parser->mutex);
    return S_OK;
}

NTSTATUS wg_parser_push_data(void *args)
{
    const struct wg_parser_push_data_params *params = args;
    struct wg_parser *parser = get_parser(params->parser);

    pthread_mutex_lock(&parser->mutex);
    if (!params->data) parser->read_request.ret = AVERROR(EIO);
    else if (!params->size) parser->read_request.ret = AVERROR_EOF;
    else
    {
        uint32_t size = min(params->size, parser->read_request.size);
        memcpy(parser->read_request.dst, params->data, size);
        parser->read_request.ret = size;
    }
    parser->read_request.done = true;
    parser->read_request.size = 0;
    pthread_mutex_unlock(&parser->mutex);
    pthread_cond_signal(&parser->read_done_cond);
    return S_OK;
}

NTSTATUS wg_parser_get_stream_count(void *args)
{
    struct wg_parser_get_stream_count_params *params = args;
    params->count = get_parser(params->parser)->stream_count;
    return S_OK;
}

NTSTATUS wg_parser_get_stream(void *args)
{
    struct wg_parser_get_stream_params *params = args;
    params->stream = (wg_parser_stream_t)(ULONG_PTR)get_parser(params->parser)->streams[params->index];
    return S_OK;
}

NTSTATUS wg_parser_stream_get_current_format(void *args)
{
    const struct wg_parser_stream_get_current_format_params *params = args;
    *params->format = get_stream(params->stream)->current_format;
    return S_OK;
}

NTSTATUS wg_parser_stream_get_codec_format(void *args)
{
    struct wg_parser_stream_get_codec_format_params *params = args;
    struct wg_parser_stream *stream = get_stream(params->stream);

    *params->format = stream->codec_format.major_type != WG_MAJOR_TYPE_UNKNOWN ? stream->codec_format : stream->current_format;
    return S_OK;
}

static bool is_compressed(const struct wg_format *format)
{
    return format->major_type != WG_MAJOR_TYPE_AUDIO && format->major_type != WG_MAJOR_TYPE_VIDEO
            && format->major_type != WG_MAJOR_TYPE_UNKNOWN;
}

static bool open_decoder(struct wg_parser_stream *stream)
{
    const AVCodec *codec;
    char *threads;

    if (stream->codec) return true;
    if (!(codec = avcodec_find_decoder(stream->av->codecpar->codec_id))) return false;
    if (!(stream->codec = avcodec_alloc_context3(codec))) return false;
    avcodec_parameters_to_context(stream->codec, stream->av->codecpar);
    stream->codec->pkt_timebase = stream->av->time_base;
    stream->codec->thread_count = wg_ffmpeg_threads;
    if ((threads = getenv("WINE_WG_FFMPEG_THREADS"))) stream->codec->thread_count = atoi(threads);
    if (avcodec_open2(stream->codec, codec, NULL) < 0)
    {
        avcodec_free_context(&stream->codec);
        return false;
    }
    return !!(stream->frame = av_frame_alloc());
}

NTSTATUS wg_parser_stream_enable(void *args)
{
    const struct wg_parser_stream_enable_params *params = args;
    struct wg_parser_stream *stream = get_stream(params->stream);
    struct wg_parser *parser = stream->parser;

    pthread_mutex_lock(&parser->mutex);
    stream->desired = *params->format;
    stream->compressed_out = is_compressed(params->format);
    if (!stream->compressed_out && !open_decoder(stream))
        ERR("No decoder for stream %u.\n", stream->number);
    sws_freeContext(stream->sws);
    stream->sws = NULL;
    swr_free(&stream->swr);
    stream->enabled = true;
    pthread_mutex_unlock(&parser->mutex);
    return S_OK;
}

NTSTATUS wg_parser_stream_disable(void *args)
{
    struct wg_parser_stream *stream = get_stream(*(wg_parser_stream_t *)args);
    struct wg_parser *parser = stream->parser;

    pthread_mutex_lock(&parser->mutex);
    stream->enabled = false;
    flush_queue(stream);
    pthread_mutex_unlock(&parser->mutex);
    pthread_cond_broadcast(&parser->empty_cond);
    pthread_cond_broadcast(&parser->buffer_cond);
    return S_OK;
}

static bool stream_waiting(struct wg_parser_stream *stream)
{
    return stream->enabled && !stream->head && !stream->eos;
}

NTSTATUS wg_parser_stream_get_buffer(void *args)
{
    const struct wg_parser_stream_get_buffer_params *params = args;
    struct wg_parser_stream *stream = get_stream(params->stream);
    struct wg_parser *parser = get_parser(params->parser);
    unsigned int i;

    pthread_mutex_lock(&parser->mutex);
    if (!parser->running)
    {
        parser->running = true;
        pthread_cond_broadcast(&parser->empty_cond);
    }
    if (stream)
    {
        /* (the demuxer reads on past a full stream for one that waits) */
        if (stream_waiting(stream)) pthread_cond_broadcast(&parser->empty_cond);
        while (stream_waiting(stream)) pthread_cond_wait(&parser->buffer_cond, &parser->mutex);
        if (!stream->head) stream = NULL;
    }
    else
    {
        /* the earliest buffer by PTS of all enabled streams, as the GStreamer side */
        struct wg_parser_stream *earliest = NULL;
        for (;;)
        {
            bool wait = false;
            for (i = 0; i < parser->stream_count; ++i) wait |= stream_waiting(parser->streams[i]);
            if (!wait) break;
            pthread_cond_broadcast(&parser->empty_cond);
            pthread_cond_wait(&parser->buffer_cond, &parser->mutex);
        }
        for (i = 0; i < parser->stream_count; ++i)
        {
            struct wg_parser_stream *s = parser->streams[i];
            if (!s->enabled || !s->head) continue;
            if (!earliest || (s->head->info.has_pts && (!earliest->head->info.has_pts || s->head->info.pts < earliest->head->info.pts)))
                earliest = s;
        }
        stream = earliest;
    }
    if (!stream)
    {
        pthread_mutex_unlock(&parser->mutex);
        return S_FALSE;
    }
    *params->buffer = stream->head->info;
    stream->get_buffer_called = true;
    pthread_mutex_unlock(&parser->mutex);
    return S_OK;
}

NTSTATUS wg_parser_stream_copy_buffer(void *args)
{
    const struct wg_parser_stream_copy_buffer_params *params = args;
    struct wg_parser_stream *stream = get_stream(params->stream);
    struct wg_parser *parser = stream->parser;

    pthread_mutex_lock(&parser->mutex);
    if (!stream->head || !stream->get_buffer_called)
    {
        pthread_mutex_unlock(&parser->mutex);
        return VFW_E_WRONG_STATE;
    }
    assert(params->offset + params->size <= stream->head->info.size);
    memcpy(params->data, stream->head->data + params->offset, params->size);
    pthread_mutex_unlock(&parser->mutex);
    return S_OK;
}

NTSTATUS wg_parser_stream_release_buffer(void *args)
{
    struct wg_parser_stream *stream = get_stream(*(wg_parser_stream_t *)args);
    struct wg_parser *parser = stream->parser;

    pthread_mutex_lock(&parser->mutex);
    if (stream->head && stream->get_buffer_called)
    {
        struct queued_buffer *b = stream->head;
        if (!(stream->head = b->next)) stream->tail = &stream->head;
        stream->queued--;
        free(b->data);
        free(b);
    }
    stream->get_buffer_called = false;
    pthread_mutex_unlock(&parser->mutex);
    pthread_cond_broadcast(&parser->empty_cond);
    return S_OK;
}

NTSTATUS wg_parser_stream_get_duration(void *args)
{
    struct wg_parser_stream_get_duration_params *params = args;
    params->duration = get_stream(params->stream)->duration;
    return S_OK;
}

NTSTATUS wg_parser_stream_get_tag(void *args)
{
    struct wg_parser_stream_get_tag_params *params = args;
    struct wg_parser_stream *stream = get_stream(params->stream);
    uint32_t len;

    if (params->tag >= WG_PARSER_TAG_COUNT) return STATUS_INVALID_PARAMETER;
    if (!stream->tags[params->tag]) return STATUS_NOT_FOUND;
    if ((len = strlen(stream->tags[params->tag]) + 1) > *params->size)
    {
        *params->size = len;
        return STATUS_BUFFER_TOO_SMALL;
    }
    memcpy(params->buffer, stream->tags[params->tag], len);
    return STATUS_SUCCESS;
}

NTSTATUS wg_parser_stream_seek(void *args)
{
    const struct wg_parser_stream_seek_params *params = args;
    struct wg_parser_stream *stream = get_stream(params->stream);
    struct wg_parser *parser = stream->parser;
    unsigned int i;

    pthread_mutex_lock(&parser->mutex);
    if ((params->stop_flags & AM_SEEKING_PositioningBitsMask) != AM_SEEKING_NoPositioning)
        parser->stop_pos = params->stop_pos == stream->duration ? 0 : params->stop_pos;
    if ((params->start_flags & AM_SEEKING_PositioningBitsMask) != AM_SEEKING_NoPositioning)
    {
        parser->seek_pending = true;
        parser->seek_start = params->start_pos;
        parser->accurate_seek = !(params->start_flags & AM_SEEKING_SeekToKeyFrame);
        if (!(params->start_flags & AM_SEEKING_NoFlush))
            for (i = 0; i < parser->stream_count; ++i)
            {
                flush_queue(parser->streams[i]);
                parser->streams[i]->eos = false;
            }
    }
    parser->running = true;
    pthread_mutex_unlock(&parser->mutex);
    pthread_cond_broadcast(&parser->empty_cond);
    return S_OK;
}

NTSTATUS wg_parser_stream_notify_qos(void *args)
{
    return S_OK;
}

static bool ensure_capacity(struct wg_parser_stream *stream, size_t size)
{
    BYTE *data;
    if (size <= stream->capacity) return true;
    if (!(data = realloc(stream->data, size))) return false;
    stream->data = data;
    stream->capacity = size;
    return true;
}

static int64_t to_100ns(struct wg_parser_stream *stream, int64_t ts)
{
    return av_rescale_q(ts, stream->av->time_base, TIME_100NS);
}

/* another enabled stream waits for a buffer (mutex held) */
static bool other_waiting(struct wg_parser *parser, struct wg_parser_stream *stream)
{
    unsigned int i;

    for (i = 0; i < parser->stream_count; ++i)
        if (parser->streams[i] != stream && stream_waiting(parser->streams[i])) return true;
    return false;
}

/* waits (mutex held) until the stream has room for a buffer; false on a
 * flush (seek), a disable or a disconnect */
static bool wait_slot(struct wg_parser *parser, struct wg_parser_stream *stream)
{
    while (stream->queued >= QUEUE_MAX && !other_waiting(parser, stream)
            && stream->enabled && !parser->quit && !parser->seek_pending)
        pthread_cond_wait(&parser->empty_cond, &parser->mutex);
    return stream->enabled && !parser->quit && !parser->seek_pending;
}

static void post_buffer(struct wg_parser *parser, struct wg_parser_stream *stream,
        int64_t pts, int64_t duration, bool delta)
{
    struct queued_buffer *b;

    /* the queue takes the buffer made; the next is made in a new one */
    if (!(b = calloc(1, sizeof(*b))))
        return;
    b->data = stream->data;
    stream->data = NULL;
    stream->capacity = 0;
    if ((b->info.has_pts = pts != AV_NOPTS_VALUE)) b->info.pts = to_100ns(stream, pts);
    if ((b->info.has_duration = duration > 0)) b->info.duration = to_100ns(stream, duration);
    b->info.delta = delta;
    b->info.discontinuity = stream->discont;
    b->info.size = stream->size;
    b->info.stream = stream->number;
    stream->discont = false;
    *stream->tail = b;
    stream->tail = &b->next;
    stream->queued++;
    pthread_cond_broadcast(&parser->buffer_cond);
}

/* a decoded frame into the stream's buffer in its desired format (mutex held) */
static bool frame_to_buffer(struct wg_parser *parser, struct wg_parser_stream *stream)
{
    AVFrame *f = stream->frame;
    int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    const struct wg_format *d = &stream->desired;

    if (parser->drop_before != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && to_100ns(stream, pts) + to_100ns(stream, f->duration) <= parser->drop_before)
        return true;
    if (parser->stop_pos && pts != AV_NOPTS_VALUE && to_100ns(stream, pts) >= parser->stop_pos)
    {
        stream->eos = true;
        pthread_cond_broadcast(&parser->buffer_cond);
        return true;
    }
    if (!wait_slot(parser, stream)) return true;

    if (d->major_type == WG_MAJOR_TYPE_VIDEO)
    {
        struct wg_ffmpeg_layout layout;
        MFVideoInfo info = {0};
        enum AVPixelFormat fmt;
        bool swap_uv;

        if ((fmt = wg_ffmpeg_pix_fmt_from_video_format(d->u.video.format, &swap_uv)) == AV_PIX_FMT_NONE)
            return false;
        /* a negative height is bottom-up (the GStreamer side's videoflip) */
        if (d->u.video.height < 0) info.VideoFlags = MFVideoFlag_BottomUpLinearRep;
        if (!wg_ffmpeg_video_layout(fmt, f->width, f->height, &info, 0, 0, &layout)
                || !ensure_capacity(stream, layout.size)
                || !wg_ffmpeg_copy_frame(&stream->sws, f, fmt, swap_uv, &layout, stream->data))
            return false;
        stream->size = layout.size;
        post_buffer(parser, stream, pts, f->duration, !(f->flags & AV_FRAME_FLAG_KEY));
    }
    else if (d->major_type == WG_MAJOR_TYPE_AUDIO)
    {
        int bytes, channels = d->u.audio.channels ? d->u.audio.channels : f->ch_layout.nb_channels;
        int rate = d->u.audio.rate ? d->u.audio.rate : f->sample_rate, n, max;
        enum AVSampleFormat fmt = wg_ffmpeg_sample_fmt(d->u.audio.format, &bytes);
        AVChannelLayout layout;
        uint8_t *out[1];

        if (fmt == AV_SAMPLE_FMT_NONE) return false;
        av_channel_layout_default(&layout, channels);
        if (!stream->swr && (swr_alloc_set_opts2(&stream->swr, &layout, fmt, rate, &f->ch_layout, f->format,
                f->sample_rate, 0, NULL) < 0 || swr_init(stream->swr) < 0))
        {
            av_channel_layout_uninit(&layout);
            swr_free(&stream->swr);
            return false;
        }
        av_channel_layout_uninit(&layout);
        max = swr_get_out_samples(stream->swr, f->nb_samples);
        if (!ensure_capacity(stream, (size_t)max * bytes * channels + 1)) return false;
        out[0] = stream->data;
        if ((n = swr_convert(stream->swr, out, max, (const uint8_t **)f->extended_data, f->nb_samples)) <= 0)
            return true;
        stream->size = n * bytes * channels;
        post_buffer(parser, stream, pts, f->duration > 0 ? f->duration
                : av_rescale_q(n, (AVRational){1, rate}, stream->av->time_base), false);
    }
    return true;
}

static void decode_packet(struct wg_parser *parser, struct wg_parser_stream *stream, const AVPacket *pkt)
{
    if (!stream->codec) return;
    pthread_mutex_unlock(&parser->mutex);
    avcodec_send_packet(stream->codec, pkt);
    pthread_mutex_lock(&parser->mutex);
    for (;;)
    {
        int ret;
        pthread_mutex_unlock(&parser->mutex);
        ret = avcodec_receive_frame(stream->codec, stream->frame);
        pthread_mutex_lock(&parser->mutex);
        if (ret) break;
        if (stream->enabled && !frame_to_buffer(parser, stream))
            ERR("Failed to convert a frame of stream %u.\n", stream->number);
        av_frame_unref(stream->frame);
        if (parser->quit || parser->seek_pending) break;
    }
}

static void do_seek(struct wg_parser *parser)
{
    int64_t ts = av_rescale_q(parser->seek_start, TIME_100NS, AV_TIME_BASE_Q);
    unsigned int i;

    parser->seek_pending = false;
    parser->drop_before = parser->accurate_seek ? (int64_t)parser->seek_start : AV_NOPTS_VALUE;
    pthread_mutex_unlock(&parser->mutex);
    if (avformat_seek_file(parser->fmt, -1, INT64_MIN, ts, ts, 0) < 0)
        WARN("Failed to seek to %s.\n", wine_dbgstr_longlong(parser->seek_start));
    pthread_mutex_lock(&parser->mutex);
    for (i = 0; i < parser->stream_count; ++i)
    {
        struct wg_parser_stream *s = parser->streams[i];
        if (s->codec) avcodec_flush_buffers(s->codec);
        s->discont = true;
        s->eos = false;
    }
}

static void run_streams(struct wg_parser *parser)
{
    AVPacket *pkt = av_packet_alloc();
    unsigned int i;

    pthread_mutex_lock(&parser->mutex);
    parser->drop_before = AV_NOPTS_VALUE;
    while (!parser->quit)
    {
        struct wg_parser_stream *stream = NULL;
        int ret;

        while (!parser->quit && !parser->seek_pending && !parser->running)
            pthread_cond_wait(&parser->empty_cond, &parser->mutex);
        if (parser->quit) break;
        if (parser->seek_pending) do_seek(parser);

        pthread_mutex_unlock(&parser->mutex);
        ret = av_read_frame(parser->fmt, pkt);
        pthread_mutex_lock(&parser->mutex);
        if (parser->seek_pending) { av_packet_unref(pkt); continue; }

        if (ret < 0)
        {
            /* the end: the decoders' last frames, then EOS on every stream */
            for (i = 0; i < parser->stream_count && !parser->quit && !parser->seek_pending; ++i)
                if (parser->streams[i]->enabled && !parser->streams[i]->compressed_out)
                    decode_packet(parser, parser->streams[i], NULL);
            if (parser->seek_pending) continue;
            for (i = 0; i < parser->stream_count; ++i) parser->streams[i]->eos = true;
            pthread_cond_broadcast(&parser->buffer_cond);
            parser->running = false;
            while (!parser->quit && !parser->seek_pending)
                pthread_cond_wait(&parser->empty_cond, &parser->mutex);
            continue;
        }

        for (i = 0; i < parser->stream_count; ++i)
            if (parser->streams[i]->av->index == pkt->stream_index) stream = parser->streams[i];
        if (stream && stream->enabled)
        {
            if (stream->compressed_out)
            {
                if (wait_slot(parser, stream) && ensure_capacity(stream, pkt->size))
                {
                    memcpy(stream->data, pkt->data, pkt->size);
                    stream->size = pkt->size;
                    post_buffer(parser, stream, pkt->pts, pkt->duration, !(pkt->flags & AV_PKT_FLAG_KEY));
                }
            }
            else decode_packet(parser, stream, pkt);
        }
        av_packet_unref(pkt);
    }
    pthread_mutex_unlock(&parser->mutex);
    av_packet_free(&pkt);
}

static char *dup_tag(AVDictionary *dict, const char *key)
{
    AVDictionaryEntry *e = av_dict_get(dict, key, NULL, 0);
    return e && e->value ? strdup(e->value) : NULL;
}

static void *parser_thread(void *arg)
{
    struct wg_parser *parser = arg;
    unsigned int i;
    uint8_t *buf;

    if (!(buf = av_malloc(IO_SIZE)) || !(parser->io = avio_alloc_context(buf, IO_SIZE, 0, parser, io_read, NULL, io_seek))
            || !(parser->fmt = avformat_alloc_context()))
        goto fail;
    parser->fmt->pb = parser->io;
    parser->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&parser->fmt, NULL, NULL, NULL) < 0)
    {
        WARN("No demuxer took the stream.\n");
        goto fail;
    }
    if (avformat_find_stream_info(parser->fmt, NULL) < 0)
        WARN("Failed to find the stream info.\n");

    if (!(parser->streams = calloc(parser->fmt->nb_streams, sizeof(*parser->streams)))) goto fail;
    for (i = 0; i < parser->fmt->nb_streams; ++i)
    {
        AVStream *av = parser->fmt->streams[i];
        AVCodecParameters *par = av->codecpar;
        struct wg_parser_stream *stream;
        AVRational fps = av->avg_frame_rate.num ? av->avg_frame_rate : av->r_frame_rate;

        if (par->codec_type != AVMEDIA_TYPE_AUDIO && par->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if (!(stream = calloc(1, sizeof(*stream)))) goto fail;
        stream->parser = parser;
        stream->number = parser->stream_count;
        stream->tail = &stream->head;
        stream->av = av;
        wg_ffmpeg_format_from_codec(par, fps, &stream->codec_format);
        /* the decoded format: what the decoder makes (its pixel or sample format) */
        if (par->format >= 0 || par->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            AVFrame f = {0};
            f.format = par->format >= 0 ? par->format : (par->codec_type == AVMEDIA_TYPE_AUDIO ? AV_SAMPLE_FMT_FLT : AV_PIX_FMT_YUV420P);
            f.width = par->width;
            f.height = par->height;
            f.sample_rate = par->sample_rate;
            av_channel_layout_copy(&f.ch_layout, &par->ch_layout);
            wg_ffmpeg_format_from_frame(&f, par->codec_type, fps, &stream->current_format);
            av_channel_layout_uninit(&f.ch_layout);
        }
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && !stream->current_format.major_type)
        {
            AVFrame f = {0};
            f.format = AV_PIX_FMT_YUV420P;
            f.width = par->width;
            f.height = par->height;
            wg_ffmpeg_format_from_frame(&f, par->codec_type, fps, &stream->current_format);
        }
        if (av->duration != AV_NOPTS_VALUE) stream->duration = av_rescale_q(av->duration, av->time_base, TIME_100NS);
        else if (parser->fmt->duration != AV_NOPTS_VALUE) stream->duration = av_rescale_q(parser->fmt->duration, AV_TIME_BASE_Q, TIME_100NS);
        stream->tags[WG_PARSER_TAG_LANGUAGE] = dup_tag(av->metadata, "language");
        stream->tags[WG_PARSER_TAG_NAME] = dup_tag(av->metadata, "title");
        parser->streams[parser->stream_count++] = stream;
    }

    pthread_mutex_lock(&parser->mutex);
    parser->init_done = true;
    pthread_mutex_unlock(&parser->mutex);
    pthread_cond_broadcast(&parser->init_cond);
    run_streams(parser);
    return NULL;

fail:
    pthread_mutex_lock(&parser->mutex);
    parser->init_done = parser->init_failed = true;
    pthread_mutex_unlock(&parser->mutex);
    pthread_cond_broadcast(&parser->init_cond);
    return NULL;
}

static void free_streams(struct wg_parser *parser)
{
    unsigned int i, j;

    for (i = 0; i < parser->stream_count; ++i)
    {
        struct wg_parser_stream *s = parser->streams[i];
        avcodec_free_context(&s->codec);
        av_frame_free(&s->frame);
        sws_freeContext(s->sws);
        swr_free(&s->swr);
        flush_queue(s);
        free(s->data);
        for (j = 0; j < WG_PARSER_TAG_COUNT; ++j) free(s->tags[j]);
        free(s);
    }
    free(parser->streams);
    parser->streams = NULL;
    parser->stream_count = 0;
}

static void stop(struct wg_parser *parser)
{
    pthread_mutex_lock(&parser->mutex);
    parser->quit = true;
    parser->sink_connected = false;
    pthread_mutex_unlock(&parser->mutex);
    pthread_cond_broadcast(&parser->read_cond);
    pthread_cond_broadcast(&parser->read_done_cond);
    pthread_cond_broadcast(&parser->empty_cond);
    pthread_cond_broadcast(&parser->buffer_cond);
    if (parser->thread_started) pthread_join(parser->thread, NULL);
    parser->thread_started = false;
    free_streams(parser);
    avformat_close_input(&parser->fmt);
    if (parser->io) av_freep(&parser->io->buffer);
    avio_context_free(&parser->io);
}

NTSTATUS wg_parser_connect(void *args)
{
    const struct wg_parser_connect_params *params = args;
    struct wg_parser *parser = get_parser(params->parser);

    /* A URI (the media source's and the WM reader's) only names the
     * stream: its bytes still come through read requests, as without. */
    if (params->uri) TRACE("uri %s\n", debugstr_w(params->uri));
    parser->file_size = params->file_size;
    parser->position = 0;
    parser->quit = parser->init_done = parser->init_failed = parser->running = parser->seek_pending = false;
    parser->stop_pos = 0;
    parser->sink_connected = true;
    if (pthread_create(&parser->thread, NULL, parser_thread, parser))
    {
        parser->sink_connected = false;
        return E_FAIL;
    }
    parser->thread_started = true;

    pthread_mutex_lock(&parser->mutex);
    while (!parser->init_done) pthread_cond_wait(&parser->init_cond, &parser->mutex);
    pthread_mutex_unlock(&parser->mutex);
    if (parser->init_failed || !parser->stream_count)
    {
        stop(parser);
        return E_FAIL;
    }
    return S_OK;
}

NTSTATUS wg_parser_disconnect(void *args)
{
    stop(get_parser(*(wg_parser_t *)args));
    return S_OK;
}

NTSTATUS wg_parser_create(void *args)
{
    struct wg_parser_create_params *params = args;
    struct wg_parser *parser;

    if (!(parser = calloc(1, sizeof(*parser)))) return E_OUTOFMEMORY;
    pthread_mutex_init(&parser->mutex, NULL);
    pthread_cond_init(&parser->init_cond, NULL);
    pthread_cond_init(&parser->read_cond, NULL);
    pthread_cond_init(&parser->read_done_cond, NULL);
    pthread_cond_init(&parser->buffer_cond, NULL);
    pthread_cond_init(&parser->empty_cond, NULL);
    parser->output_compressed = params->output_compressed;
    params->parser = (wg_parser_t)(ULONG_PTR)parser;
    return S_OK;
}

NTSTATUS wg_parser_destroy(void *args)
{
    struct wg_parser *parser = get_parser(*(wg_parser_t *)args);

    pthread_mutex_destroy(&parser->mutex);
    pthread_cond_destroy(&parser->init_cond);
    pthread_cond_destroy(&parser->read_cond);
    pthread_cond_destroy(&parser->read_done_cond);
    pthread_cond_destroy(&parser->buffer_cond);
    pthread_cond_destroy(&parser->empty_cond);
    free(parser);
    return S_OK;
}
