/*
 * gex audio (ASIO): the Unix side, over gex's /dev/dsp
 *
 * The device (gex-kernel's vfs/oss.rs) mixes what each open writes and, in
 * the page, the browser's audio thread plays it; SNDCTL_DSP_GEX_INFO says
 * what plays it. A stream is float stereo at the host's rate, with two
 * fragments of one ASIO buffer each: a write of the next buffer returns once
 * the one before it starts to play, which paces the driver's thread.
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

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "unixlib.h"

/* OSSv4's numbers (sys/soundcard.h), and gex's own. */
#define SNDCTL_DSP_SPEED       0xc0045002
#define SNDCTL_DSP_SETFMT      0xc0045005
#define SNDCTL_DSP_CHANNELS    0xc0045006
#define SNDCTL_DSP_SETFRAGMENT 0xc004500a
#define SNDCTL_DSP_GETCAPS     0x8004500f
#define SNDCTL_DSP_GETODELAY   0x80045017
#define SNDCTL_DSP_GEX_INFO    0x801450e0
#define AFMT_FLOAT             0x10000000
#define PCM_CAP_INPUT          0x00010000

struct stream
{
    int out, in;
    UINT32 rate, frames;
    UINT64 written;
};

static void fill_info(int fd, struct gexasio_info *info)
{
    unsigned int v[5] = {0};
    int caps = 0;

    memset(info, 0, sizeof(*info));
    info->rate = 48000;
    if (ioctl(fd, SNDCTL_DSP_GEX_INFO, v) < 0) return;
    info->player = 1;
    if (v[1]) info->rate = v[1];
    info->latency_us = v[2];
    info->quantum = v[3];
    if (ioctl(fd, SNDCTL_DSP_GETCAPS, &caps) == 0 && (caps & PCM_CAP_INPUT))
        info->capture = 1;
}

static NTSTATUS probe(void *args)
{
    struct gexasio_probe_params *params = args;
    int fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);

    if (fd < 0)
    {
        memset(&params->info, 0, sizeof(params->info));
        return STATUS_SUCCESS;
    }
    fill_info(fd, &params->info);
    close(fd);
    return STATUS_SUCCESS;
}

static int setup(int fd, UINT32 rate, int frag)
{
    int v = AFMT_FLOAT;
    if (frag && ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag) < 0) return -1;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &v) < 0 || v != AFMT_FLOAT) return -1;
    v = 2;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &v) < 0 || v != 2) return -1;
    v = rate;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &v) < 0) return -1;
    return 0;
}

static NTSTATUS do_open(void *args)
{
    struct gexasio_open_params *params = args;
    struct stream *s;
    UINT32 bytes = params->frames * 2 * sizeof(float);
    int shift = 4;

    params->stream = 0;
    if (!(s = calloc(1, sizeof(*s))))
    {
        params->result = STATUS_NO_MEMORY;
        return STATUS_SUCCESS;
    }
    s->rate = params->rate;
    s->frames = params->frames;
    s->in = -1;
    while (shift < 14 && (1u << shift) < bytes) shift++;
    if ((s->out = open("/dev/dsp", O_WRONLY)) < 0 || setup(s->out, s->rate, (2 << 16) | shift) < 0)
        goto fail;
    if (params->inputs)
    {
        char drain[4096];
        if ((s->in = open("/dev/dsp", O_RDONLY | O_NONBLOCK)) < 0 || setup(s->in, s->rate, 0) < 0)
            goto fail;
        /* from now on: what capture held before is not this stream's */
        while (read(s->in, drain, sizeof(drain)) > 0) ;
    }
    params->stream = (UINT_PTR)s;
    params->result = STATUS_SUCCESS;
    return STATUS_SUCCESS;
fail:
    if (s->out >= 0) close(s->out);
    if (s->in >= 0) close(s->in);
    free(s);
    params->result = STATUS_DEVICE_NOT_READY;
    return STATUS_SUCCESS;
}

static NTSTATUS do_close(void *args)
{
    struct gexasio_io_params *params = args;
    struct stream *s = (struct stream *)(UINT_PTR)params->stream;

    if (s)
    {
        close(s->out);
        if (s->in >= 0) close(s->in);
        free(s);
    }
    params->result = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

/* The whole buffer, blocking until the device takes it. */
static NTSTATUS do_write(void *args)
{
    struct gexasio_io_params *params = args;
    struct stream *s = (struct stream *)(UINT_PTR)params->stream;
    const char *p = (const char *)params->data;
    size_t left = params->frames * 2 * sizeof(float);

    while (left)
    {
        ssize_t n = write(s->out, p, left);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            params->done = (p - (const char *)params->data) / (2 * sizeof(float));
            params->result = STATUS_UNSUCCESSFUL;
            return STATUS_SUCCESS;
        }
        p += n;
        left -= n;
    }
    s->written += params->frames;
    params->done = params->frames;
    params->result = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

/* What capture has, up to the buffer, without waiting; the rest silence. */
static NTSTATUS do_read(void *args)
{
    struct gexasio_io_params *params = args;
    struct stream *s = (struct stream *)(UINT_PTR)params->stream;
    size_t want = params->frames * 2 * sizeof(float), got = 0;

    while (s->in >= 0 && got < want)
    {
        ssize_t n = read(s->in, (char *)params->data + got, want - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += n;
    }
    got -= got % (2 * sizeof(float));
    memset((char *)params->data + got, 0, want - got);
    params->done = got / (2 * sizeof(float));
    params->result = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

static NTSTATUS do_pos(void *args)
{
    struct gexasio_pos_params *params = args;
    struct stream *s = (struct stream *)(UINT_PTR)params->stream;
    struct gexasio_info info;
    int delay = 0;

    params->written = s->written;
    if (ioctl(s->out, SNDCTL_DSP_GETODELAY, &delay) < 0) delay = 0;
    params->queued = delay / (2 * sizeof(float));
    fill_info(s->out, &info);
    params->latency_us = info.latency_us;
    params->quantum = info.quantum;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    probe,
    do_open,
    do_close,
    do_write,
    do_read,
    do_pos,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == gexasio_funcs_count);
