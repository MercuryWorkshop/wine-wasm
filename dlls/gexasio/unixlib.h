/*
 * gex audio (ASIO): the calls between the driver and its Unix side
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

#include "wine/unixlib.h"

/* What plays the device: gex's /dev/dsp answers SNDCTL_DSP_GEX_INFO. */
struct gexasio_info
{
    UINT32 player;      /* 1: gex's device with a player (the page) */
    UINT32 rate;        /* the player's rate (48000 without one) */
    UINT32 latency_us;  /* the player's own output latency */
    UINT32 quantum;     /* its render quantum, frames */
    UINT32 capture;     /* 1: the device has input (capture ring) */
};

struct gexasio_probe_params
{
    struct gexasio_info info;
};

struct gexasio_open_params
{
    UINT32 rate;        /* frames a second */
    UINT32 frames;      /* the ASIO buffer: one half, frames */
    UINT32 inputs;      /* 1: open capture too */
    UINT32 fragments;   /* out: output buffers the device holds */
    UINT64 stream;      /* out */
    NTSTATUS result;
};

struct gexasio_io_params
{
    UINT64 stream;
    float *data;        /* stereo interleaved, frames * 2 floats */
    UINT32 frames;
    UINT32 done;        /* out: frames moved (read: the rest is zeroed) */
    NTSTATUS result;
};

struct gexasio_pos_params
{
    UINT64 stream;
    UINT64 written;     /* out: frames written since open */
    UINT32 queued;      /* out: frames written and not yet played */
    UINT32 latency_us;  /* out: the player's output latency */
    UINT32 quantum;     /* out: frames */
};

enum gexasio_funcs
{
    gexasio_probe,
    gexasio_open,
    gexasio_close,
    gexasio_write,
    gexasio_read,
    gexasio_pos,
    gexasio_funcs_count
};
