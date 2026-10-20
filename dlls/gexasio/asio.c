/*
 * gex audio (ASIO): an ASIO driver over gex's audio device
 *
 * Derived from WineASIO (asio.c, main.c and regsvr.c of
 * https://github.com/wineasio/wineasio), its JACK backend replaced by gex's
 * /dev/dsp through this driver's Unix side (unixlib.c). It uses no
 * Steinberg SDK: the interface is WineASIO's own declaration of it.
 *
 * Copyright (C) 2006 Robert Reif
 * Portions copyright (C) 2007 Ralf Beck
 * Portions copyright (C) 2007 Johnny Petrantoni
 * Portions copyright (C) 2007 Stephane Letz
 * Portions copyright (C) 2008 William Steidtmann
 * Portions copyright (C) 2010 Peter L Jones
 * Portions copyright (C) 2010 Torben Hohn
 * Portions copyright (C) 2010 Nedko Arnaudov
 * Portions copyright (C) 2013 Joakim Hernberg
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

/* How it runs: CreateBuffers opens a float stereo stream on the device at
 * the host's rate, two fragments of one buffer each; Start starts a thread
 * that, for each buffer, takes what capture has (when the device has input),
 * calls the host's bufferSwitch on one half, and writes that half to the
 * device, which blocks until the buffer before it plays. Output latency is
 * then two buffers, the player's render quantum and its output latency. */

#include <stdarg.h>
#include <string.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "objbase.h"
#include "unknwn.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(gexasio);

#define DRIVER_NAME        "gex audio"
#define DRIVER_DESCRIPTION L"gex audio (ASIO)"
#define DRIVER_KEY         L"Software\\ASIO\\gex audio"
#define CLSID_STRING       L"{F2D1C87F-1EC3-41DB-A5F3-9C335307145D}"
#define MIN_BUFFER         64
#define MAX_BUFFER         2048
#define PREFERRED_BUFFER   256
#define OUTPUTS            2
#define MAX_INPUTS         2

/* {F2D1C87F-1EC3-41DB-A5F3-9C335307145D} */
static const GUID CLSID_GexAsio =
    {0xf2d1c87f, 0x1ec3, 0x41db, {0xa5, 0xf3, 0x9c, 0x33, 0x53, 0x07, 0x14, 0x5d}};

/* The ASIO result codes. */
enum
{
    ASE_OK = 0,
    ASE_SUCCESS = 0x3f4847a0,
    ASE_NotPresent = -1000,
    ASE_HWMalfunction = -999,
    ASE_InvalidParameter = -998,
    ASE_InvalidMode = -997,
    ASE_NoClock = -995,
    ASE_NoMemory = -994,
};

#define ASIOSTFloat32LSB 19

typedef struct w_int64_t
{
    ULONG hi;
    ULONG lo;
} w_int64_t;

typedef struct BufferInformation
{
    LONG isInputType;
    LONG channelNumber;
    void *audioBufferStart;
    void *audioBufferEnd;
} BufferInformation;

typedef struct TimeInformation
{
    LONG      _1[4];
    double    speed;
    w_int64_t timeStamp;
    w_int64_t numSamples;
    double    sampleRate;
    ULONG     flags;
    char      _3[12];
    double    speedForTimeCode;
    w_int64_t timeStampForTimeCode;
    ULONG     flagsForTimeCode;
    char      _4[64];
} TimeInformation;

typedef struct Callbacks
{
    void (CALLBACK *swapBuffers)(LONG, LONG);
    void (CALLBACK *sampleRateChanged)(double);
    LONG (CALLBACK *sendNotification)(LONG, LONG, void *, double *);
    void *(CALLBACK *swapBuffersWithTimeInfo)(TimeInformation *, LONG, LONG);
} Callbacks;

#define INTERFACE IGexAsio
DECLARE_INTERFACE_(IGexAsio, IUnknown)
{
    STDMETHOD_(HRESULT, QueryInterface)(THIS_ REFIID riid, void **ppvObject) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD_(LONG, Init)(THIS_ void *sysRef) PURE;
    STDMETHOD_(void, GetDriverName)(THIS_ char *name) PURE;
    STDMETHOD_(LONG, GetDriverVersion)(THIS) PURE;
    STDMETHOD_(void, GetErrorMessage)(THIS_ char *string) PURE;
    STDMETHOD_(LONG, Start)(THIS) PURE;
    STDMETHOD_(LONG, Stop)(THIS) PURE;
    STDMETHOD_(LONG, GetChannels)(THIS_ LONG *numInputChannels, LONG *numOutputChannels) PURE;
    STDMETHOD_(LONG, GetLatencies)(THIS_ LONG *inputLatency, LONG *outputLatency) PURE;
    STDMETHOD_(LONG, GetBufferSize)(THIS_ LONG *minSize, LONG *maxSize, LONG *preferredSize, LONG *granularity) PURE;
    STDMETHOD_(LONG, CanSampleRate)(THIS_ double sampleRate) PURE;
    STDMETHOD_(LONG, GetSampleRate)(THIS_ double *sampleRate) PURE;
    STDMETHOD_(LONG, SetSampleRate)(THIS_ double sampleRate) PURE;
    STDMETHOD_(LONG, GetClockSources)(THIS_ void *clocks, LONG *numSources) PURE;
    STDMETHOD_(LONG, SetClockSource)(THIS_ LONG index) PURE;
    STDMETHOD_(LONG, GetSamplePosition)(THIS_ w_int64_t *sPos, w_int64_t *tStamp) PURE;
    STDMETHOD_(LONG, GetChannelInfo)(THIS_ void *info) PURE;
    STDMETHOD_(LONG, CreateBuffers)(THIS_ BufferInformation *bufferInfo, LONG numChannels, LONG bufferSize, Callbacks *callbacks) PURE;
    STDMETHOD_(LONG, DisposeBuffers)(THIS) PURE;
    STDMETHOD_(LONG, ControlPanel)(THIS) PURE;
    STDMETHOD_(LONG, Future)(THIS_ LONG selector, void *opt) PURE;
    STDMETHOD_(LONG, OutputReady)(THIS) PURE;
};
#undef INTERFACE

enum { Loaded, Initialized, Prepared, Running };

typedef struct
{
    IGexAsio IGexAsio_iface;
    LONG ref;
    int state;
    HWND sys_ref;

    struct gexasio_info info;
    int inputs;
    double rate;
    LONG frames;

    Callbacks *callbacks;
    BOOL time_info, time_code;
    TimeInformation time;
    UINT64 num_samples;
    w_int64_t time_stamp;

    UINT64 stream;
    float *buffers;   /* per channel two halves: inputs, then outputs */
    float *io;        /* one buffer, interleaved stereo */
    BOOL in_active[MAX_INPUTS], out_active[OUTPUTS];
    HANDLE thread;
    volatile LONG stop;
} GexAsio;

static inline GexAsio *impl_from_IGexAsio(IGexAsio *iface)
{
    return CONTAINING_RECORD(iface, GexAsio, IGexAsio_iface);
}

static w_int64_t to_w64(UINT64 v)
{
    w_int64_t r = { (ULONG)(v >> 32), (ULONG)v };
    return r;
}

/* timeGetTime's clock (GetTickCount's), in ns, as ASIO time stamps are */
static w_int64_t now_ns(void)
{
    return to_w64(GetTickCount64() * 1000000);
}

static float *in_buffer(GexAsio *This, int ch)
{
    return This->buffers + ch * 2 * This->frames;
}

static float *out_buffer(GexAsio *This, int ch)
{
    return This->buffers + (This->inputs + ch) * 2 * This->frames;
}

static HRESULT WINAPI asio_QueryInterface(IGexAsio *iface, REFIID riid, void **ppv)
{
    GexAsio *This = impl_from_IGexAsio(iface);

    if (!ppv) return E_INVALIDARG;
    /* ASIO hosts ask for the class id as the interface */
    if (IsEqualIID(riid, &CLSID_GexAsio) || IsEqualIID(riid, &IID_IUnknown))
    {
        InterlockedIncrement(&This->ref);
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI asio_AddRef(IGexAsio *iface)
{
    GexAsio *This = impl_from_IGexAsio(iface);
    return InterlockedIncrement(&This->ref);
}

static LONG WINAPI asio_Stop(IGexAsio *iface);
static LONG WINAPI asio_DisposeBuffers(IGexAsio *iface);

static ULONG WINAPI asio_Release(IGexAsio *iface)
{
    GexAsio *This = impl_from_IGexAsio(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    if (!ref)
    {
        if (This->state == Running) asio_Stop(iface);
        if (This->state == Prepared) asio_DisposeBuffers(iface);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return ref;
}

static LONG WINAPI asio_Init(IGexAsio *iface, void *sysRef)
{
    GexAsio *This = impl_from_IGexAsio(iface);
    struct gexasio_probe_params params;

    if (This->state != Loaded) return 1;
    This->sys_ref = sysRef;
    if (WINE_UNIX_CALL(gexasio_probe, &params) || !params.info.rate)
    {
        WARN("no audio device\n");
        return 0;
    }
    This->info = params.info;
    This->inputs = params.info.capture ? MAX_INPUTS : 0;
    This->rate = params.info.rate;
    This->frames = PREFERRED_BUFFER;
    This->state = Initialized;
    TRACE("rate %u, player %u, latency %u us, quantum %u, inputs %d\n", params.info.rate,
          params.info.player, params.info.latency_us, params.info.quantum, This->inputs);
    return 1;
}

static void WINAPI asio_GetDriverName(IGexAsio *iface, char *name)
{
    strcpy(name, DRIVER_NAME);
}

static LONG WINAPI asio_GetDriverVersion(IGexAsio *iface)
{
    return 1;
}

static void WINAPI asio_GetErrorMessage(IGexAsio *iface, char *string)
{
    GexAsio *This = impl_from_IGexAsio(iface);
    strcpy(string, This->state == Loaded ? "no audio device" : "no error");
}

static DWORD WINAPI callback_thread(void *arg)
{
    GexAsio *This = arg;
    struct gexasio_io_params io = { This->stream, This->io, This->frames };
    LONG index = 0, n = This->frames, i, c;

    while (!This->stop)
    {
        if (This->inputs)
        {
            WINE_UNIX_CALL(gexasio_read, &io);
            for (c = 0; c < This->inputs; c++)
            {
                float *b = in_buffer(This, c) + index * n;
                for (i = 0; i < n; i++) b[i] = This->io[2 * i + c];
            }
        }
        This->time_stamp = now_ns();
        if (This->time_info)
        {
            This->time.speed = 1.0;
            This->time.numSamples = to_w64(This->num_samples);
            This->time.timeStamp = This->time_stamp;
            This->time.sampleRate = This->rate;
            This->time.flags = 0x7; /* system time, sample position, rate valid */
            This->callbacks->swapBuffersWithTimeInfo(&This->time, index, 1);
        }
        else
            This->callbacks->swapBuffers(index, 1);
        for (c = 0; c < OUTPUTS; c++)
        {
            const float *b = out_buffer(This, c) + index * n;
            if (This->out_active[c])
                for (i = 0; i < n; i++) This->io[2 * i + c] = b[i];
            else
                for (i = 0; i < n; i++) This->io[2 * i + c] = 0;
        }
        io.frames = n;
        if (WINE_UNIX_CALL(gexasio_write, &io) || io.result)
        {
            ERR("the device stopped taking audio\n");
            break;
        }
        This->num_samples += n;
        index ^= 1;
    }
    return 0;
}

static LONG WINAPI asio_Start(IGexAsio *iface)
{
    GexAsio *This = impl_from_IGexAsio(iface);

    TRACE("state %d, %d frames\n", This->state, This->frames);
    if (This->state != Prepared) return ASE_NotPresent;
    memset(This->buffers, 0, (This->inputs + OUTPUTS) * 2 * This->frames * sizeof(float));
    This->num_samples = 0;
    This->time_stamp = now_ns();
    This->stop = 0;
    if (!(This->thread = CreateThread(NULL, 0, callback_thread, This, 0, NULL)))
        return ASE_HWMalfunction;
    SetThreadPriority(This->thread, THREAD_PRIORITY_TIME_CRITICAL);
    This->state = Running;
    return ASE_OK;
}

static LONG WINAPI asio_Stop(IGexAsio *iface)
{
    GexAsio *This = impl_from_IGexAsio(iface);

    if (This->state != Running) return ASE_NotPresent;
    This->stop = 1;
    WaitForSingleObject(This->thread, INFINITE);
    CloseHandle(This->thread);
    This->thread = NULL;
    This->state = Prepared;
    return ASE_OK;
}

static LONG WINAPI asio_GetChannels(IGexAsio *iface, LONG *numInputChannels, LONG *numOutputChannels)
{
    GexAsio *This = impl_from_IGexAsio(iface);

    if (!numInputChannels || !numOutputChannels) return ASE_InvalidParameter;
    if (This->state == Loaded) return ASE_NotPresent;
    *numInputChannels = This->inputs;
    *numOutputChannels = OUTPUTS;
    return ASE_OK;
}

/* In frames: output is the two buffers the device holds, the player's
 * quantum and its output latency; input one buffer and the quantum. */
static LONG WINAPI asio_GetLatencies(IGexAsio *iface, LONG *inputLatency, LONG *outputLatency)
{
    GexAsio *This = impl_from_IGexAsio(iface);
    struct gexasio_info info = This->info;

    if (!inputLatency || !outputLatency) return ASE_InvalidParameter;
    if (This->state == Loaded) return ASE_NotPresent;
    if (This->stream)
    {
        struct gexasio_pos_params pos = { This->stream };
        if (!WINE_UNIX_CALL(gexasio_pos, &pos))
        {
            info.latency_us = pos.latency_us;
            info.quantum = pos.quantum;
        }
    }
    *outputLatency = 2 * This->frames + info.quantum + (LONG)(info.latency_us * This->rate / 1e6 + 0.5);
    *inputLatency = This->frames + info.quantum;
    return ASE_OK;
}

static LONG WINAPI asio_GetBufferSize(IGexAsio *iface, LONG *minSize, LONG *maxSize, LONG *preferredSize, LONG *granularity)
{
    if (!minSize || !maxSize || !preferredSize || !granularity) return ASE_InvalidParameter;
    *minSize = MIN_BUFFER;
    *maxSize = MAX_BUFFER;
    *preferredSize = PREFERRED_BUFFER;
    *granularity = -1; /* powers of two */
    return ASE_OK;
}

/* The device resamples each stream to the player's rate: any common one. */
static BOOL rate_ok(double rate)
{
    static const double rates[] = { 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(rates); i++) if (rate == rates[i]) return TRUE;
    return FALSE;
}

static LONG WINAPI asio_CanSampleRate(IGexAsio *iface, double sampleRate)
{
    return rate_ok(sampleRate) ? ASE_OK : ASE_NoClock;
}

static LONG WINAPI asio_GetSampleRate(IGexAsio *iface, double *sampleRate)
{
    GexAsio *This = impl_from_IGexAsio(iface);

    if (!sampleRate) return ASE_InvalidParameter;
    *sampleRate = This->rate;
    return ASE_OK;
}

static LONG open_stream(GexAsio *This);
static void close_stream(GexAsio *This);

static LONG WINAPI asio_SetSampleRate(IGexAsio *iface, double sampleRate)
{
    GexAsio *This = impl_from_IGexAsio(iface);
    LONG r = ASE_OK;

    if (sampleRate == 0) return ASE_NoClock; /* no external clock */
    if (!rate_ok(sampleRate)) return ASE_NoClock;
    if (sampleRate == This->rate) return ASE_OK;
    if (This->state == Running) return ASE_InvalidMode;
    This->rate = sampleRate;
    if (This->state == Prepared)
    {
        close_stream(This);
        r = open_stream(This);
    }
    return r;
}

static LONG WINAPI asio_GetClockSources(IGexAsio *iface, void *clocks, LONG *numSources)
{
    LONG *l = clocks;

    if (!clocks || !numSources) return ASE_InvalidParameter;
    *l++ = 0;  /* index */
    *l++ = -1; /* associated channel */
    *l++ = -1; /* associated group */
    *l++ = 1;  /* the current source */
    strcpy((char *)l, "Internal");
    *numSources = 1;
    return ASE_OK;
}

static LONG WINAPI asio_SetClockSource(IGexAsio *iface, LONG index)
{
    return index ? ASE_NotPresent : ASE_OK;
}

/* The frame playing now: what was written less what the device holds. */
static LONG WINAPI asio_GetSamplePosition(IGexAsio *iface, w_int64_t *sPos, w_int64_t *tStamp)
{
    GexAsio *This = impl_from_IGexAsio(iface);
    struct gexasio_pos_params pos = { This->stream };

    if (!sPos || !tStamp) return ASE_InvalidParameter;
    if (This->state != Running) return ASE_NotPresent;
    if (WINE_UNIX_CALL(gexasio_pos, &pos)) return ASE_HWMalfunction;
    *tStamp = now_ns();
    *sPos = to_w64(pos.written > pos.queued ? pos.written - pos.queued : 0);
    return ASE_OK;
}

static LONG WINAPI asio_GetChannelInfo(IGexAsio *iface, void *info)
{
    GexAsio *This = impl_from_IGexAsio(iface);
    LONG *l = info;
    LONG channel = l[0], input = l[1];

    if (channel < 0 || channel >= (input ? This->inputs : OUTPUTS)) return ASE_InvalidParameter;
    l[2] = input ? This->in_active[channel] : This->out_active[channel];
    l[3] = 0;                /* group */
    l[4] = ASIOSTFloat32LSB; /* sample type */
    memset(l + 5, 0, 32);
    sprintf((char *)(l + 5), "%s %d", input ? "in" : "out", (int)channel + 1);
    return ASE_OK;
}

static LONG open_stream(GexAsio *This)
{
    struct gexasio_open_params params = { (UINT32)This->rate, This->frames, This->inputs > 0 };

    if (WINE_UNIX_CALL(gexasio_open, &params) || params.result)
    {
        WARN("the device did not open at %u Hz, %u frames\n", params.rate, params.frames);
        return ASE_HWMalfunction;
    }
    This->stream = params.stream;
    return ASE_OK;
}

static void close_stream(GexAsio *This)
{
    struct gexasio_io_params params = { This->stream };

    if (!This->stream) return;
    WINE_UNIX_CALL(gexasio_close, &params);
    This->stream = 0;
}

static LONG WINAPI asio_CreateBuffers(IGexAsio *iface, BufferInformation *bufferInfo, LONG numChannels,
                                      LONG bufferSize, Callbacks *callbacks)
{
    GexAsio *This = impl_from_IGexAsio(iface);
    BufferInformation *b;
    LONG i, r;

    TRACE("%d channels, %d frames, state %d\n", numChannels, bufferSize, This->state);
    if (This->state != Initialized) return ASE_NotPresent;
    if (!bufferInfo || !callbacks) return ASE_InvalidMode;
    for (i = 0, b = bufferInfo; i < numChannels; i++, b++)
        if (b->channelNumber < 0 || b->channelNumber >= (b->isInputType ? This->inputs : OUTPUTS))
            return ASE_InvalidMode;
    if (bufferSize < MIN_BUFFER || bufferSize > MAX_BUFFER || (bufferSize & (bufferSize - 1)))
        return ASE_InvalidMode;

    This->frames = bufferSize;
    This->callbacks = callbacks;
    This->time_info = This->time_code = FALSE;
    if (callbacks->sendNotification(1, 7, NULL, NULL) && callbacks->sendNotification(7, 0, NULL, NULL))
        This->time_info = TRUE;

    This->buffers = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                              (This->inputs + OUTPUTS) * 2 * bufferSize * sizeof(float));
    This->io = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 2 * bufferSize * sizeof(float));
    if (!This->buffers || !This->io)
    {
        HeapFree(GetProcessHeap(), 0, This->buffers);
        HeapFree(GetProcessHeap(), 0, This->io);
        This->buffers = This->io = NULL;
        return ASE_NoMemory;
    }
    memset(This->in_active, 0, sizeof(This->in_active));
    memset(This->out_active, 0, sizeof(This->out_active));
    for (i = 0, b = bufferInfo; i < numChannels; i++, b++)
    {
        float *p = b->isInputType ? in_buffer(This, b->channelNumber) : out_buffer(This, b->channelNumber);
        b->audioBufferStart = p;
        b->audioBufferEnd = p + bufferSize;
        if (b->isInputType) This->in_active[b->channelNumber] = TRUE;
        else This->out_active[b->channelNumber] = TRUE;
    }
    if ((r = open_stream(This)) != ASE_OK)
    {
        HeapFree(GetProcessHeap(), 0, This->buffers);
        HeapFree(GetProcessHeap(), 0, This->io);
        This->buffers = This->io = NULL;
        return r;
    }
    This->state = Prepared;
    return ASE_OK;
}

static LONG WINAPI asio_DisposeBuffers(IGexAsio *iface)
{
    GexAsio *This = impl_from_IGexAsio(iface);

    if (This->state == Running) asio_Stop(iface);
    if (This->state != Prepared) return ASE_InvalidMode;
    close_stream(This);
    HeapFree(GetProcessHeap(), 0, This->buffers);
    HeapFree(GetProcessHeap(), 0, This->io);
    This->buffers = This->io = NULL;
    This->callbacks = NULL;
    This->state = Initialized;
    return ASE_OK;
}

static LONG WINAPI asio_ControlPanel(IGexAsio *iface)
{
    return ASE_NotPresent; /* nothing to set */
}

static LONG WINAPI asio_Future(IGexAsio *iface, LONG selector, void *opt)
{
    GexAsio *This = impl_from_IGexAsio(iface);

    switch (selector)
    {
    case 1: /* kAsioEnableTimeCodeRead */
        This->time_code = TRUE;
        return ASE_SUCCESS;
    case 2: /* kAsioDisableTimeCodeRead */
        This->time_code = FALSE;
        return ASE_SUCCESS;
    case 10: /* kAsioCanTimeInfo */
    case 11: /* kAsioCanTimeCode */
        return ASE_SUCCESS;
    default:
        return ASE_InvalidParameter;
    }
}

static LONG WINAPI asio_OutputReady(IGexAsio *iface)
{
    return ASE_NotPresent;
}

static const IGexAsioVtbl asio_vtbl =
{
    asio_QueryInterface,
    asio_AddRef,
    asio_Release,
    asio_Init,
    asio_GetDriverName,
    asio_GetDriverVersion,
    asio_GetErrorMessage,
    asio_Start,
    asio_Stop,
    asio_GetChannels,
    asio_GetLatencies,
    asio_GetBufferSize,
    asio_CanSampleRate,
    asio_GetSampleRate,
    asio_SetSampleRate,
    asio_GetClockSources,
    asio_SetClockSource,
    asio_GetSamplePosition,
    asio_GetChannelInfo,
    asio_CreateBuffers,
    asio_DisposeBuffers,
    asio_ControlPanel,
    asio_Future,
    asio_OutputReady,
};

/* The class factory. */

static HRESULT WINAPI cf_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory))
    {
        *ppv = iface;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI cf_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI cf_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI cf_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    GexAsio *This;
    HRESULT hr;

    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This)))) return E_OUTOFMEMORY;
    This->IGexAsio_iface.lpVtbl = &asio_vtbl;
    This->ref = 1;
    This->state = Loaded;
    hr = asio_QueryInterface(&This->IGexAsio_iface, riid, ppv);
    asio_Release(&This->IGexAsio_iface);
    return hr;
}

static HRESULT WINAPI cf_LockServer(IClassFactory *iface, BOOL dolock)
{
    return S_OK;
}

static const IClassFactoryVtbl cf_vtbl =
{
    cf_QueryInterface,
    cf_AddRef,
    cf_Release,
    cf_CreateInstance,
    cf_LockServer,
};

static IClassFactory factory = { &cf_vtbl };

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    if (!IsEqualGUID(rclsid, &CLSID_GexAsio)) return CLASS_E_CLASSNOTAVAILABLE;
    return cf_QueryInterface(&factory, riid, ppv);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

/* Registration: the class (HKCR\CLSID) and the driver list ASIO hosts read
 * (HKLM\Software\ASIO). */

static LONG set_value(HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *value)
{
    HKEY key;
    LONG r = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
    if (r) return r;
    r = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
    RegCloseKey(key);
    return r;
}

static HINSTANCE instance;

HRESULT WINAPI DllRegisterServer(void)
{
    static const WCHAR clsid_key[] = L"CLSID\\" CLSID_STRING;
    static const WCHAR inproc_key[] = L"CLSID\\" CLSID_STRING L"\\InprocServer32";
    WCHAR path[MAX_PATH];

    if (!GetModuleFileNameW(instance, path, ARRAY_SIZE(path))) return E_FAIL;
    if (set_value(HKEY_CLASSES_ROOT, clsid_key, NULL, DRIVER_DESCRIPTION) ||
        set_value(HKEY_CLASSES_ROOT, inproc_key, NULL, path) ||
        set_value(HKEY_CLASSES_ROOT, inproc_key, L"ThreadingModel", L"Apartment") ||
        set_value(HKEY_LOCAL_MACHINE, DRIVER_KEY, L"CLSID", CLSID_STRING) ||
        set_value(HKEY_LOCAL_MACHINE, DRIVER_KEY, L"Description", DRIVER_DESCRIPTION))
        return E_FAIL;
    return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\" CLSID_STRING);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, DRIVER_KEY);
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        instance = inst;
        DisableThreadLibraryCalls(inst);
        if (__wine_init_unix_call()) return FALSE;
    }
    return TRUE;
}
