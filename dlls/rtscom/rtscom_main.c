/*
* Copyright 2026 Alistair Leslie-Hughes
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
#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include <initguid.h>
#include "oleidl.h"
#include "rpcproxy.h"
#include "rtscom.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(rtscom);


/* No tablet reaches a Wine program, so no packet ever does: the stylus keeps what
 * it is given (its window, its plugins, the packet description) and says yes,
 * which is all a program setting up for ink needs (Sticky Notes threw on the
 * failures and died on its first click). */
#define MAX_PLUGINS 8

struct rtstylus
{
    IRealTimeStylus IRealTimeStylus_iface;
    LONG ref;
    BOOL enabled;
    HANDLE_PTR hwnd;
    RECT rect;
    IStylusSyncPlugin *sync[MAX_PLUGINS];
    ULONG sync_count;
    IStylusAsyncPlugin *async[MAX_PLUGINS];
    ULONG async_count;
    IRealTimeStylus *child;
    GUID *props;
    ULONG props_count;
};

static struct rtstylus *impl_from_IRealTimeStylus( IRealTimeStylus *iface )
{
    return CONTAINING_RECORD( iface, struct rtstylus, IRealTimeStylus_iface );
}

static HRESULT WINAPI rtstylus_QueryInterface(IRealTimeStylus *iface, REFIID iid, void **obj)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);

    TRACE( "(%p)->(%s %p)\n", This, debugstr_guid( iid ), obj );

    if (IsEqualIID( iid, &IID_IUnknown ) ||
        IsEqualIID( iid, &IID_IRealTimeStylus ))
    {
        IRealTimeStylus_AddRef( iface );
        *obj = &This->IRealTimeStylus_iface;
        return S_OK;
    }

    FIXME( "interface %s not implemented\n", debugstr_guid( iid ) );
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI rtstylus_AddRef(IRealTimeStylus *iface)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref = %lu\n", This, ref );
    return ref;
}

static ULONG WINAPI rtstylus_Release(IRealTimeStylus *iface)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE( "(%p) ref = %lu\n", This, ref );

    if (!ref)
    {
        while (This->sync_count) IStylusSyncPlugin_Release( This->sync[--This->sync_count] );
        while (This->async_count) IStylusAsyncPlugin_Release( This->async[--This->async_count] );
        if (This->child) IRealTimeStylus_Release( This->child );
        CoTaskMemFree( This->props );
        free( This );
    }

    return ref;
}

static HRESULT WINAPI rtstylus_get_Enabled(IRealTimeStylus *iface, BOOL *pfEnable)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %p\n", This, pfEnable);
    if (!pfEnable) return E_POINTER;
    *pfEnable = This->enabled;
    return S_OK;
}

static HRESULT WINAPI rtstylus_put_Enabled(IRealTimeStylus *iface, BOOL fEnable)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %d\n", This, fEnable);
    This->enabled = fEnable;
    return S_OK;
}

static HRESULT WINAPI rtstylus_get_HWND(IRealTimeStylus *iface, HANDLE_PTR *phwnd)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %p\n", This, phwnd);
    if (!phwnd) return E_POINTER;
    *phwnd = This->hwnd;
    return S_OK;
}

static HRESULT WINAPI rtstylus_put_HWND(IRealTimeStylus *iface, HANDLE_PTR hwnd)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %Ix\n", This, hwnd);
    This->hwnd = hwnd;
    return S_OK;
}

static HRESULT WINAPI rtstylus_get_WindowInputRectangle(IRealTimeStylus *iface, RECT *prcWndInputRect)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %p\n", This, prcWndInputRect);
    if (!prcWndInputRect) return E_POINTER;
    *prcWndInputRect = This->rect;
    return S_OK;
}

static HRESULT WINAPI rtstylus_put_WindowInputRectangle(IRealTimeStylus *iface, const RECT *prcWndInputRect)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %p\n", This, prcWndInputRect);
    if (!prcWndInputRect) return E_POINTER;
    This->rect = *prcWndInputRect;
    return S_OK;
}

static HRESULT WINAPI rtstylus_AddStylusSyncPlugin(IRealTimeStylus *iface, ULONG iIndex, IStylusSyncPlugin *piPlugin)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %lu, %p\n", This, iIndex, piPlugin);
    if (!piPlugin) return E_POINTER;
    if (This->sync_count == MAX_PLUGINS) return E_OUTOFMEMORY;
    if (iIndex > This->sync_count) iIndex = This->sync_count;
    memmove( This->sync + iIndex + 1, This->sync + iIndex, (This->sync_count - iIndex) * sizeof(*This->sync) );
    This->sync[iIndex] = piPlugin;
    IStylusSyncPlugin_AddRef( piPlugin );
    This->sync_count++;
    return S_OK;
}

static HRESULT WINAPI rtstylus_RemoveStylusSyncPlugin(IRealTimeStylus *iface, ULONG iIndex, IStylusSyncPlugin **ppiPlugin)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %lu, %p\n", This, iIndex, ppiPlugin);
    if (iIndex >= This->sync_count) return E_INVALIDARG;
    if (ppiPlugin) *ppiPlugin = This->sync[iIndex];
    else IStylusSyncPlugin_Release( This->sync[iIndex] );
    This->sync_count--;
    memmove( This->sync + iIndex, This->sync + iIndex + 1, (This->sync_count - iIndex) * sizeof(*This->sync) );
    return S_OK;
}

static HRESULT WINAPI rtstylus_RemoveAllStylusSyncPlugins(IRealTimeStylus *iface)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p\n", This);
    while (This->sync_count) IStylusSyncPlugin_Release( This->sync[--This->sync_count] );
    return S_OK;
}

static HRESULT WINAPI rtstylus_GetStylusSyncPlugin(IRealTimeStylus *iface, ULONG iIndex, IStylusSyncPlugin **ppiPlugin)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %lu, %p\n", This, iIndex, ppiPlugin);
    if (!ppiPlugin) return E_POINTER;
    if (iIndex >= This->sync_count) return E_INVALIDARG;
    *ppiPlugin = This->sync[iIndex];
    IStylusSyncPlugin_AddRef( *ppiPlugin );
    return S_OK;
}

static HRESULT WINAPI rtstylus_GetStylusSyncPluginCount(IRealTimeStylus *iface, ULONG *pcPlugins)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %p\n", This, pcPlugins);
    if (!pcPlugins) return E_POINTER;
    *pcPlugins = This->sync_count;
    return S_OK;
}

static HRESULT WINAPI rtstylus_AddStylusAsyncPlugin(IRealTimeStylus *iface, ULONG iIndex, IStylusAsyncPlugin *piPlugin)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %lu, %p\n", This, iIndex, piPlugin);
    if (!piPlugin) return E_POINTER;
    if (This->async_count == MAX_PLUGINS) return E_OUTOFMEMORY;
    if (iIndex > This->async_count) iIndex = This->async_count;
    memmove( This->async + iIndex + 1, This->async + iIndex, (This->async_count - iIndex) * sizeof(*This->async) );
    This->async[iIndex] = piPlugin;
    IStylusAsyncPlugin_AddRef( piPlugin );
    This->async_count++;
    return S_OK;
}

static HRESULT WINAPI rtstylus_RemoveStylusAsyncPlugin(IRealTimeStylus *iface, ULONG iIndex, IStylusAsyncPlugin **ppiPlugin)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %lu, %p\n", This, iIndex, ppiPlugin);
    if (iIndex >= This->async_count) return E_INVALIDARG;
    if (ppiPlugin) *ppiPlugin = This->async[iIndex];
    else IStylusAsyncPlugin_Release( This->async[iIndex] );
    This->async_count--;
    memmove( This->async + iIndex, This->async + iIndex + 1, (This->async_count - iIndex) * sizeof(*This->async) );
    return S_OK;
}

static HRESULT WINAPI rtstylus_RemoveAllStylusAsyncPlugins(IRealTimeStylus *iface)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p\n", This);
    while (This->async_count) IStylusAsyncPlugin_Release( This->async[--This->async_count] );
    return S_OK;
}

static HRESULT WINAPI rtstylus_GetStylusAsyncPlugin(IRealTimeStylus *iface, ULONG iIndex, IStylusAsyncPlugin **ppiPlugin)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %lu, %p\n", This, iIndex, ppiPlugin);
    if (!ppiPlugin) return E_POINTER;
    if (iIndex >= This->async_count) return E_INVALIDARG;
    *ppiPlugin = This->async[iIndex];
    IStylusAsyncPlugin_AddRef( *ppiPlugin );
    return S_OK;
}

static HRESULT WINAPI rtstylus_GetStylusAsyncPluginCount(IRealTimeStylus *iface, ULONG *pcPlugins)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %p\n", This, pcPlugins);
    if (!pcPlugins) return E_POINTER;
    *pcPlugins = This->async_count;
    return S_OK;
}

static HRESULT WINAPI rtstylus_get_ChildRealTimeStylusPlugin(IRealTimeStylus *iface, IRealTimeStylus **ppiRTS)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %p\n", This, ppiRTS);
    if (!ppiRTS) return E_POINTER;
    if ((*ppiRTS = This->child)) IRealTimeStylus_AddRef( This->child );
    return S_OK;
}

static HRESULT WINAPI rtstylus_putref_ChildRealTimeStylusPlugin(IRealTimeStylus *iface, IRealTimeStylus *piRTS)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %p\n", This, piRTS);
    if (piRTS) IRealTimeStylus_AddRef( piRTS );
    if (This->child) IRealTimeStylus_Release( This->child );
    This->child = piRTS;
    return S_OK;
}

static HRESULT WINAPI rtstylus_AddCustomStylusDataToQueue(IRealTimeStylus *iface, StylusQueue sq, const GUID *pGuidId,
                ULONG cbData, BYTE *pbData)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %d, %s, %lu, %p\n", This, sq, debugstr_guid(pGuidId), cbData, pbData);
    return S_OK;
}

static HRESULT WINAPI rtstylus_ClearStylusQueues(IRealTimeStylus *iface)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p\n", This);
    return S_OK;
}

static HRESULT WINAPI rtstylus_SetAllTabletsMode(IRealTimeStylus *iface, BOOL fUseMouseForInput)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %d\n", This, fUseMouseForInput);
    return S_OK;
}

static HRESULT WINAPI rtstylus_SetSingleTabletMode(IRealTimeStylus *iface, IInkTablet *piTablet)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %p\n", This, piTablet);
    return S_OK;
}

static HRESULT WINAPI rtstylus_GetTablet(IRealTimeStylus *iface, IInkTablet **ppiSingleTablet)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    FIXME("%p, %p\n", This, ppiSingleTablet);
    return E_NOTIMPL;
}

static HRESULT WINAPI rtstylus_GetTabletContextIdFromTablet(IRealTimeStylus *iface, IInkTablet *piTablet, TABLET_CONTEXT_ID *ptcid)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    FIXME("%p, %p, %p\n", This, piTablet, ptcid);
    return E_NOTIMPL;
}

static HRESULT WINAPI rtstylus_GetTabletFromTabletContextId(IRealTimeStylus *iface, TABLET_CONTEXT_ID tcid, IInkTablet **ppiTablet)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    FIXME("%p, %ld, %p\n", This, tcid, ppiTablet);
    return E_NOTIMPL;
}

static HRESULT WINAPI rtstylus_GetAllTabletContextIds(IRealTimeStylus *iface, ULONG *pcTcidCount, TABLET_CONTEXT_ID **ppTcids)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %p, %p\n", This, pcTcidCount, ppTcids);
    if (!pcTcidCount || !ppTcids) return E_POINTER;
    *pcTcidCount = 0;
    *ppTcids = NULL;
    return S_OK;
}

static HRESULT WINAPI rtstylus_GetStyluses(IRealTimeStylus *iface, IInkCursors **ppiInkCursors)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    FIXME("%p, %p\n", This, ppiInkCursors);
    return E_NOTIMPL;
}

static HRESULT WINAPI rtstylus_GetStylusForId(IRealTimeStylus *iface, STYLUS_ID sid, IInkCursor **ppiInkCursor)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    FIXME("%p, %ld, %p\n", This, sid, ppiInkCursor);
    return E_NOTIMPL;
}

static HRESULT WINAPI rtstylus_SetDesiredPacketDescription(IRealTimeStylus *iface, ULONG cProperties, const GUID *pPropertyGuids)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    GUID *props = NULL;

    TRACE("%p, %lu, %p\n", This, cProperties, pPropertyGuids);
    if (cProperties && !pPropertyGuids) return E_POINTER;
    if (cProperties && !(props = CoTaskMemAlloc( cProperties * sizeof(*props) ))) return E_OUTOFMEMORY;
    if (cProperties) memcpy( props, pPropertyGuids, cProperties * sizeof(*props) );
    CoTaskMemFree( This->props );
    This->props = props;
    This->props_count = cProperties;
    return S_OK;
}

static HRESULT WINAPI rtstylus_GetDesiredPacketDescription(IRealTimeStylus *iface, ULONG *pcProperties, GUID **ppPropertyGuids)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    TRACE("%p, %p, %p\n", This, pcProperties, ppPropertyGuids);
    if (!pcProperties) return E_POINTER;
    *pcProperties = This->props_count;
    if (!ppPropertyGuids) return S_OK;
    *ppPropertyGuids = NULL;
    if (!This->props_count) return S_OK;
    if (!(*ppPropertyGuids = CoTaskMemAlloc( This->props_count * sizeof(GUID) ))) return E_OUTOFMEMORY;
    memcpy( *ppPropertyGuids, This->props, This->props_count * sizeof(GUID) );
    return S_OK;
}

static HRESULT WINAPI rtstylus_GetPacketDescriptionData(IRealTimeStylus *iface, TABLET_CONTEXT_ID tcid, FLOAT *pfInkToDeviceScaleX,
    FLOAT *pfInkToDeviceScaleY, ULONG *pcPacketProperties, PACKET_PROPERTY **ppPacketProperties)
{
    struct rtstylus *This = impl_from_IRealTimeStylus(iface);
    FIXME("%p, %lu, %p, %p, %p, %p\n", This, tcid, pfInkToDeviceScaleX, pfInkToDeviceScaleY, pcPacketProperties, ppPacketProperties);
    return E_NOTIMPL;
}

static const struct IRealTimeStylusVtbl realtime_stylus_vtbl =
{
    rtstylus_QueryInterface,
    rtstylus_AddRef,
    rtstylus_Release,
    rtstylus_get_Enabled,
    rtstylus_put_Enabled,
    rtstylus_get_HWND,
    rtstylus_put_HWND,
    rtstylus_get_WindowInputRectangle,
    rtstylus_put_WindowInputRectangle,
    rtstylus_AddStylusSyncPlugin,
    rtstylus_RemoveStylusSyncPlugin,
    rtstylus_RemoveAllStylusSyncPlugins,
    rtstylus_GetStylusSyncPlugin,
    rtstylus_GetStylusSyncPluginCount,
    rtstylus_AddStylusAsyncPlugin,
    rtstylus_RemoveStylusAsyncPlugin,
    rtstylus_RemoveAllStylusAsyncPlugins,
    rtstylus_GetStylusAsyncPlugin,
    rtstylus_GetStylusAsyncPluginCount,
    rtstylus_get_ChildRealTimeStylusPlugin,
    rtstylus_putref_ChildRealTimeStylusPlugin,
    rtstylus_AddCustomStylusDataToQueue,
    rtstylus_ClearStylusQueues,
    rtstylus_SetAllTabletsMode,
    rtstylus_SetSingleTabletMode,
    rtstylus_GetTablet,
    rtstylus_GetTabletContextIdFromTablet,
    rtstylus_GetTabletFromTabletContextId,
    rtstylus_GetAllTabletContextIds,
    rtstylus_GetStyluses,
    rtstylus_GetStylusForId,
    rtstylus_SetDesiredPacketDescription,
    rtstylus_GetDesiredPacketDescription,
    rtstylus_GetPacketDescriptionData
};

static HRESULT realtime_stylus_create( IUnknown *outer, REFIID iid, void **obj )
{
    struct rtstylus *This = calloc( 1, sizeof(*This) );
    HRESULT hr = E_FAIL;

    if (!This) return E_OUTOFMEMORY;
    This->IRealTimeStylus_iface.lpVtbl = &realtime_stylus_vtbl;
    This->ref = 1;

    hr = IRealTimeStylus_QueryInterface( &This->IRealTimeStylus_iface, iid, obj );
    IRealTimeStylus_Release( &This->IRealTimeStylus_iface );
    return hr;
}

/* Drawing attributes for the dynamic renderer. Windows hands out a default set
 * (black, 53 HIMETRIC wide, ball tip); callers read and change them. */
struct drawing_attrs
{
    IInkDrawingAttributes IInkDrawingAttributes_iface;
    LONG ref;
    LONG color, transparency;
    float width, height;
    VARIANT_BOOL fit, ignore_pressure, antialiased;
    InkRasterOperation rop;
    InkPenTip tip;
};

static struct drawing_attrs *impl_from_IInkDrawingAttributes( IInkDrawingAttributes *iface )
{
    return CONTAINING_RECORD( iface, struct drawing_attrs, IInkDrawingAttributes_iface );
}

static HRESULT drawing_attrs_create( const struct drawing_attrs *from, IInkDrawingAttributes **out );

static HRESULT WINAPI attrs_QueryInterface( IInkDrawingAttributes *iface, REFIID iid, void **obj )
{
    TRACE( "(%p)->(%s %p)\n", iface, debugstr_guid( iid ), obj );
    if (IsEqualIID( iid, &IID_IUnknown ) || IsEqualIID( iid, &IID_IDispatch ) ||
        IsEqualIID( iid, &IID_IInkDrawingAttributes ))
    {
        *obj = iface;
        IInkDrawingAttributes_AddRef( iface );
        return S_OK;
    }
    FIXME( "interface %s not implemented\n", debugstr_guid( iid ) );
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI attrs_AddRef( IInkDrawingAttributes *iface )
{
    struct drawing_attrs *This = impl_from_IInkDrawingAttributes( iface );
    return InterlockedIncrement( &This->ref );
}

static ULONG WINAPI attrs_Release( IInkDrawingAttributes *iface )
{
    struct drawing_attrs *This = impl_from_IInkDrawingAttributes( iface );
    ULONG ref = InterlockedDecrement( &This->ref );
    if (!ref) free( This );
    return ref;
}

static HRESULT WINAPI attrs_GetTypeInfoCount( IInkDrawingAttributes *iface, UINT *count )
{
    *count = 0;
    return S_OK;
}

static HRESULT WINAPI attrs_GetTypeInfo( IInkDrawingAttributes *iface, UINT index, LCID lcid, ITypeInfo **info )
{
    FIXME( "%p, %u\n", iface, index );
    *info = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI attrs_GetIDsOfNames( IInkDrawingAttributes *iface, REFIID iid, LPOLESTR *names,
                                           UINT count, LCID lcid, DISPID *ids )
{
    FIXME( "%p, %u\n", iface, count );
    return E_NOTIMPL;
}

static HRESULT WINAPI attrs_Invoke( IInkDrawingAttributes *iface, DISPID id, REFIID iid, LCID lcid, WORD flags,
                                    DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg )
{
    FIXME( "%p, %ld\n", iface, id );
    return E_NOTIMPL;
}

#define ATTR_GET_PUT( name, type, field ) \
static HRESULT WINAPI attrs_get_##name( IInkDrawingAttributes *iface, type *value ) \
{ \
    struct drawing_attrs *This = impl_from_IInkDrawingAttributes( iface ); \
    TRACE( "%p, %p\n", This, value ); \
    if (!value) return E_POINTER; \
    *value = This->field; \
    return S_OK; \
} \
static HRESULT WINAPI attrs_put_##name( IInkDrawingAttributes *iface, type value ) \
{ \
    struct drawing_attrs *This = impl_from_IInkDrawingAttributes( iface ); \
    TRACE( "%p\n", This ); \
    This->field = value; \
    return S_OK; \
}

ATTR_GET_PUT( Color, LONG, color )
ATTR_GET_PUT( Width, float, width )
ATTR_GET_PUT( Height, float, height )
ATTR_GET_PUT( FitToCurve, VARIANT_BOOL, fit )
ATTR_GET_PUT( IgnorePressure, VARIANT_BOOL, ignore_pressure )
ATTR_GET_PUT( AntiAliased, VARIANT_BOOL, antialiased )
ATTR_GET_PUT( Transparency, LONG, transparency )
ATTR_GET_PUT( RasterOperation, InkRasterOperation, rop )
ATTR_GET_PUT( PenTip, InkPenTip, tip )

#undef ATTR_GET_PUT

static HRESULT WINAPI attrs_get_ExtendedProperties( IInkDrawingAttributes *iface, IInkExtendedProperties **props )
{
    FIXME( "%p, %p\n", iface, props );
    if (!props) return E_POINTER;
    *props = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI attrs_Clone( IInkDrawingAttributes *iface, IInkDrawingAttributes **clone )
{
    struct drawing_attrs *This = impl_from_IInkDrawingAttributes( iface );
    TRACE( "%p, %p\n", This, clone );
    if (!clone) return E_POINTER;
    return drawing_attrs_create( This, clone );
}

static const IInkDrawingAttributesVtbl drawing_attrs_vtbl =
{
    attrs_QueryInterface,
    attrs_AddRef,
    attrs_Release,
    attrs_GetTypeInfoCount,
    attrs_GetTypeInfo,
    attrs_GetIDsOfNames,
    attrs_Invoke,
    attrs_get_Color,
    attrs_put_Color,
    attrs_get_Width,
    attrs_put_Width,
    attrs_get_Height,
    attrs_put_Height,
    attrs_get_FitToCurve,
    attrs_put_FitToCurve,
    attrs_get_IgnorePressure,
    attrs_put_IgnorePressure,
    attrs_get_AntiAliased,
    attrs_put_AntiAliased,
    attrs_get_Transparency,
    attrs_put_Transparency,
    attrs_get_RasterOperation,
    attrs_put_RasterOperation,
    attrs_get_PenTip,
    attrs_put_PenTip,
    attrs_get_ExtendedProperties,
    attrs_Clone
};

static HRESULT drawing_attrs_create( const struct drawing_attrs *from, IInkDrawingAttributes **out )
{
    struct drawing_attrs *This = calloc( 1, sizeof(*This) );

    if (!This) return E_OUTOFMEMORY;
    if (from) *This = *from;
    else
    {
        This->width = 53.0f;
        This->height = 1.0f;
        This->ignore_pressure = VARIANT_FALSE;
        This->antialiased = VARIANT_TRUE;
        This->rop = IRO_CopyPen;
        This->tip = IPT_Ball;
    }
    This->IInkDrawingAttributes_iface.lpVtbl = &drawing_attrs_vtbl;
    This->ref = 1;
    *out = &This->IInkDrawingAttributes_iface;
    return S_OK;
}

/* The dynamic renderer draws ink while the pen moves. With no packets there is
 * nothing to draw; it keeps its settings. */
struct renderer
{
    IDynamicRenderer IDynamicRenderer_iface;
    IStylusSyncPlugin IStylusSyncPlugin_iface;
    LONG ref;
    BOOL enabled, cache;
    HANDLE_PTR hwnd, region;
    RECT clip;
    IInkDrawingAttributes *attrs;
};

static struct renderer *impl_from_IDynamicRenderer( IDynamicRenderer *iface )
{
    return CONTAINING_RECORD( iface, struct renderer, IDynamicRenderer_iface );
}

static struct renderer *impl_from_IStylusSyncPlugin( IStylusSyncPlugin *iface )
{
    return CONTAINING_RECORD( iface, struct renderer, IStylusSyncPlugin_iface );
}

static HRESULT WINAPI renderer_QueryInterface( IDynamicRenderer *iface, REFIID iid, void **obj )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );

    TRACE( "(%p)->(%s %p)\n", This, debugstr_guid( iid ), obj );

    if (IsEqualIID( iid, &IID_IUnknown ) || IsEqualIID( iid, &IID_IDynamicRenderer ))
        *obj = &This->IDynamicRenderer_iface;
    else if (IsEqualIID( iid, &IID_IStylusSyncPlugin ) || IsEqualIID( iid, &IID_IStylusPlugin ))
        *obj = &This->IStylusSyncPlugin_iface;
    else
    {
        FIXME( "interface %s not implemented\n", debugstr_guid( iid ) );
        *obj = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*obj );
    return S_OK;
}

static ULONG WINAPI renderer_AddRef( IDynamicRenderer *iface )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    ULONG ref = InterlockedIncrement( &This->ref );
    TRACE( "(%p) ref = %lu\n", This, ref );
    return ref;
}

static ULONG WINAPI renderer_Release( IDynamicRenderer *iface )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    ULONG ref = InterlockedDecrement( &This->ref );

    TRACE( "(%p) ref = %lu\n", This, ref );
    if (!ref)
    {
        if (This->attrs) IInkDrawingAttributes_Release( This->attrs );
        free( This );
    }
    return ref;
}

static HRESULT WINAPI renderer_get_Enabled( IDynamicRenderer *iface, BOOL *enabled )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %p\n", This, enabled );
    if (!enabled) return E_POINTER;
    *enabled = This->enabled;
    return S_OK;
}

static HRESULT WINAPI renderer_put_Enabled( IDynamicRenderer *iface, BOOL enabled )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %d\n", This, enabled );
    This->enabled = enabled;
    return S_OK;
}

static HRESULT WINAPI renderer_get_HWND( IDynamicRenderer *iface, HANDLE_PTR *hwnd )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %p\n", This, hwnd );
    if (!hwnd) return E_POINTER;
    *hwnd = This->hwnd;
    return S_OK;
}

static HRESULT WINAPI renderer_put_HWND( IDynamicRenderer *iface, HANDLE_PTR hwnd )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %Ix\n", This, hwnd );
    This->hwnd = hwnd;
    return S_OK;
}

static HRESULT WINAPI renderer_get_ClipRectangle( IDynamicRenderer *iface, RECT *rect )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %p\n", This, rect );
    if (!rect) return E_POINTER;
    *rect = This->clip;
    return S_OK;
}

static HRESULT WINAPI renderer_put_ClipRectangle( IDynamicRenderer *iface, const RECT *rect )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %p\n", This, rect );
    if (!rect) return E_POINTER;
    This->clip = *rect;
    return S_OK;
}

static HRESULT WINAPI renderer_get_ClipRegion( IDynamicRenderer *iface, HANDLE_PTR *region )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %p\n", This, region );
    if (!region) return E_POINTER;
    *region = This->region;
    return S_OK;
}

static HRESULT WINAPI renderer_put_ClipRegion( IDynamicRenderer *iface, HANDLE_PTR region )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %Ix\n", This, region );
    This->region = region;
    return S_OK;
}

static HRESULT WINAPI renderer_get_DrawingAttributes( IDynamicRenderer *iface, IInkDrawingAttributes **attrs )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %p\n", This, attrs );
    if (!attrs) return E_POINTER;
    if ((*attrs = This->attrs)) IInkDrawingAttributes_AddRef( This->attrs );
    return S_OK;
}

static HRESULT WINAPI renderer_putref_DrawingAttributes( IDynamicRenderer *iface, IInkDrawingAttributes *attrs )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %p\n", This, attrs );
    if (attrs) IInkDrawingAttributes_AddRef( attrs );
    if (This->attrs) IInkDrawingAttributes_Release( This->attrs );
    This->attrs = attrs;
    return S_OK;
}

static HRESULT WINAPI renderer_get_DataCacheEnabled( IDynamicRenderer *iface, BOOL *cache )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %p\n", This, cache );
    if (!cache) return E_POINTER;
    *cache = This->cache;
    return S_OK;
}

static HRESULT WINAPI renderer_put_DataCacheEnabled( IDynamicRenderer *iface, BOOL cache )
{
    struct renderer *This = impl_from_IDynamicRenderer( iface );
    TRACE( "%p, %d\n", This, cache );
    This->cache = cache;
    return S_OK;
}

static HRESULT WINAPI renderer_ReleaseCachedData( IDynamicRenderer *iface, DWORD stroke )
{
    TRACE( "%p, %lu\n", iface, stroke );
    return S_OK;
}

static HRESULT WINAPI renderer_Refresh( IDynamicRenderer *iface )
{
    TRACE( "%p\n", iface );
    return S_OK;
}

static HRESULT WINAPI renderer_Draw( IDynamicRenderer *iface, HANDLE_PTR hdc )
{
    TRACE( "%p, %Ix\n", iface, hdc );
    return S_OK;
}

static const struct IDynamicRendererVtbl renderer_vtbl =
{
    renderer_QueryInterface,
    renderer_AddRef,
    renderer_Release,
    renderer_get_Enabled,
    renderer_put_Enabled,
    renderer_get_HWND,
    renderer_put_HWND,
    renderer_get_ClipRectangle,
    renderer_put_ClipRectangle,
    renderer_get_ClipRegion,
    renderer_put_ClipRegion,
    renderer_get_DrawingAttributes,
    renderer_putref_DrawingAttributes,
    renderer_get_DataCacheEnabled,
    renderer_put_DataCacheEnabled,
    renderer_ReleaseCachedData,
    renderer_Refresh,
    renderer_Draw
};

static HRESULT WINAPI plugin_QueryInterface( IStylusSyncPlugin *iface, REFIID iid, void **obj )
{
    return renderer_QueryInterface( &impl_from_IStylusSyncPlugin( iface )->IDynamicRenderer_iface, iid, obj );
}

static ULONG WINAPI plugin_AddRef( IStylusSyncPlugin *iface )
{
    return renderer_AddRef( &impl_from_IStylusSyncPlugin( iface )->IDynamicRenderer_iface );
}

static ULONG WINAPI plugin_Release( IStylusSyncPlugin *iface )
{
    return renderer_Release( &impl_from_IStylusSyncPlugin( iface )->IDynamicRenderer_iface );
}

static HRESULT WINAPI plugin_RealTimeStylusEnabled( IStylusSyncPlugin *iface, IRealTimeStylus *rts, ULONG count, const TABLET_CONTEXT_ID *ids )
{
    return S_OK;
}

static HRESULT WINAPI plugin_RealTimeStylusDisabled( IStylusSyncPlugin *iface, IRealTimeStylus *rts, ULONG count, const TABLET_CONTEXT_ID *ids )
{
    return S_OK;
}

static HRESULT WINAPI plugin_StylusInRange( IStylusSyncPlugin *iface, IRealTimeStylus *rts, TABLET_CONTEXT_ID tcid, STYLUS_ID sid )
{
    return S_OK;
}

static HRESULT WINAPI plugin_StylusOutOfRange( IStylusSyncPlugin *iface, IRealTimeStylus *rts, TABLET_CONTEXT_ID tcid, STYLUS_ID sid )
{
    return S_OK;
}

static HRESULT WINAPI plugin_StylusDown( IStylusSyncPlugin *iface, IRealTimeStylus *rts, const StylusInfo *info,
                                         ULONG count, LONG *packet, LONG **out )
{
    return S_OK;
}

static HRESULT WINAPI plugin_StylusUp( IStylusSyncPlugin *iface, IRealTimeStylus *rts, const StylusInfo *info,
                                       ULONG count, LONG *packet, LONG **out )
{
    return S_OK;
}

static HRESULT WINAPI plugin_StylusButtonDown( IStylusSyncPlugin *iface, IRealTimeStylus *rts, STYLUS_ID sid,
                                               const GUID *button, POINT *pos )
{
    return S_OK;
}

static HRESULT WINAPI plugin_StylusButtonUp( IStylusSyncPlugin *iface, IRealTimeStylus *rts, STYLUS_ID sid,
                                             const GUID *button, POINT *pos )
{
    return S_OK;
}

static HRESULT WINAPI plugin_InAirPackets( IStylusSyncPlugin *iface, IRealTimeStylus *rts, const StylusInfo *info,
                                           ULONG count, ULONG len, LONG *packets, ULONG *out_count, LONG **out )
{
    return S_OK;
}

static HRESULT WINAPI plugin_Packets( IStylusSyncPlugin *iface, IRealTimeStylus *rts, const StylusInfo *info,
                                      ULONG count, ULONG len, LONG *packets, ULONG *out_count, LONG **out )
{
    return S_OK;
}

static HRESULT WINAPI plugin_CustomStylusDataAdded( IStylusSyncPlugin *iface, IRealTimeStylus *rts, const GUID *id,
                                                    ULONG size, const BYTE *data )
{
    return S_OK;
}

static HRESULT WINAPI plugin_SystemEvent( IStylusSyncPlugin *iface, IRealTimeStylus *rts, TABLET_CONTEXT_ID tcid,
                                          STYLUS_ID sid, SYSTEM_EVENT event, SYSTEM_EVENT_DATA data )
{
    return S_OK;
}

static HRESULT WINAPI plugin_TabletAdded( IStylusSyncPlugin *iface, IRealTimeStylus *rts, IInkTablet *tablet )
{
    return S_OK;
}

static HRESULT WINAPI plugin_TabletRemoved( IStylusSyncPlugin *iface, IRealTimeStylus *rts, LONG index )
{
    return S_OK;
}

static HRESULT WINAPI plugin_Error( IStylusSyncPlugin *iface, IRealTimeStylus *rts, IStylusPlugin *plugin,
                                    RealTimeStylusDataInterest interest, HRESULT hr, LONG_PTR *key )
{
    return S_OK;
}

static HRESULT WINAPI plugin_UpdateMapping( IStylusSyncPlugin *iface, IRealTimeStylus *rts )
{
    return S_OK;
}

static HRESULT WINAPI plugin_DataInterest( IStylusSyncPlugin *iface, RealTimeStylusDataInterest *interest )
{
    if (!interest) return E_POINTER;
    *interest = RTSDI_StylusDown | RTSDI_Packets | RTSDI_StylusUp;
    return S_OK;
}

static const struct IStylusSyncPluginVtbl plugin_vtbl =
{
    plugin_QueryInterface,
    plugin_AddRef,
    plugin_Release,
    plugin_RealTimeStylusEnabled,
    plugin_RealTimeStylusDisabled,
    plugin_StylusInRange,
    plugin_StylusOutOfRange,
    plugin_StylusDown,
    plugin_StylusUp,
    plugin_StylusButtonDown,
    plugin_StylusButtonUp,
    plugin_InAirPackets,
    plugin_Packets,
    plugin_CustomStylusDataAdded,
    plugin_SystemEvent,
    plugin_TabletAdded,
    plugin_TabletRemoved,
    plugin_Error,
    plugin_UpdateMapping,
    plugin_DataInterest
};

static HRESULT dynamic_renderer_create( IUnknown *outer, REFIID iid, void **obj )
{
    struct renderer *This;
    HRESULT hr;

    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(This = calloc( 1, sizeof(*This) ))) return E_OUTOFMEMORY;
    This->IDynamicRenderer_iface.lpVtbl = &renderer_vtbl;
    This->IStylusSyncPlugin_iface.lpVtbl = &plugin_vtbl;
    This->ref = 1;
    This->enabled = TRUE;
    if (FAILED(hr = drawing_attrs_create( NULL, &This->attrs )))
    {
        free( This );
        return hr;
    }

    hr = IDynamicRenderer_QueryInterface( &This->IDynamicRenderer_iface, iid, obj );
    IDynamicRenderer_Release( &This->IDynamicRenderer_iface );
    return hr;
}

struct class_factory
{
    IClassFactory IClassFactory_iface;
    HRESULT (*create_instance)(IUnknown *, REFIID, void **);
};

static inline struct class_factory *impl_from_IClassFactory( IClassFactory *iface )
{
    return CONTAINING_RECORD( iface, struct class_factory, IClassFactory_iface );
}

static HRESULT WINAPI class_factory_QueryInterface( IClassFactory *iface,
                                                    REFIID iid, void **obj )
{
    if (IsEqualIID( iid, &IID_IUnknown ) ||
        IsEqualIID( iid, &IID_IClassFactory ))
    {
        IClassFactory_AddRef( iface );
        *obj = iface;
        return S_OK;
    }

    FIXME( "interface %s not implemented\n", debugstr_guid( iid ) );
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI class_factory_AddRef( IClassFactory *iface )
{
    return 2;
}

static ULONG WINAPI class_factory_Release( IClassFactory *iface )
{
    return 1;
}

static HRESULT WINAPI class_factory_CreateInstance( IClassFactory *iface,
                                                    IUnknown *outer, REFIID iid,
                                                    void **obj )
{
    struct class_factory *This = impl_from_IClassFactory( iface );

    TRACE( "%p %s %p\n", outer, debugstr_guid( iid ), obj );

    *obj = NULL;
    return This->create_instance( outer, iid, obj );
}

static HRESULT WINAPI class_factory_LockServer( IClassFactory *iface, BOOL lock )
{
    FIXME( "%d: stub!\n", lock );
    return S_OK;
}

static const struct IClassFactoryVtbl class_factory_vtbl =
{
    class_factory_QueryInterface,
    class_factory_AddRef,
    class_factory_Release,
    class_factory_CreateInstance,
    class_factory_LockServer
};

static struct class_factory runtime_styus_cf = { { &class_factory_vtbl }, realtime_stylus_create };
static struct class_factory dynamic_renderer_cf = { { &class_factory_vtbl }, dynamic_renderer_create };

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID *obj)
{
    IClassFactory *cf = NULL;

    TRACE( "(%s %s %p)\n", debugstr_guid( clsid ), debugstr_guid( iid ), obj );

    if (IsEqualCLSID( clsid, &CLSID_RealTimeStylus ))
        cf = &runtime_styus_cf.IClassFactory_iface;
    else if (IsEqualCLSID( clsid, &CLSID_DynamicRenderer ))
        cf = &dynamic_renderer_cf.IClassFactory_iface;
    else
        FIXME( "(%s %s %p) unsupported interface\n", debugstr_guid( clsid ), debugstr_guid( iid ), obj );

    if (!cf) return CLASS_E_CLASSNOTAVAILABLE;

    return IClassFactory_QueryInterface( cf, iid, obj );
}
