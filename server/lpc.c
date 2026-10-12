/*
 * Server-side local procedure call (LPC) ports
 *
 * The legacy NT port interface: NtCreatePort, NtConnectPort,
 * NtListenPort/NtReplyWaitReceivePort, NtAcceptConnectPort,
 * NtCompleteConnectPort, NtRequestWaitReplyPort, NtRequestPort and
 * NtReplyPort. Windows services such as the Windows 7 Themes service
 * (themeservice.dll, \ThemeApiPort) talk to their clients this way.
 *
 * A server creates a named connection port. A client connects by name:
 * its connection request is queued on the connection port and the client
 * waits until the server accepts (NtAcceptConnectPort, which makes the
 * server's end of the connection, then NtCompleteConnectPort) or refuses.
 * The client then holds a client communication port. Its requests and
 * datagrams, and the LPC_PORT_CLOSED message sent when it goes away, are
 * all queued on the server's connection port, which is what the server
 * receives on. A request's sender waits on an lpc_wait object until the
 * server replies by message id.
 *
 * Copyright 2026 the gex project
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

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "handle.h"
#include "thread.h"
#include "process.h"
#include "request.h"
#include "security.h"

/* message types (PORT_MESSAGE u2.s2.Type) */
#define LPC_REQUEST             1
#define LPC_REPLY               2
#define LPC_DATAGRAM            3
#define LPC_LOST_REPLY          4
#define LPC_PORT_CLOSED         5
#define LPC_CLIENT_DIED         6
#define LPC_CONNECTION_REQUEST  10

#define LPC_MAX_DATA            0x1000  /* bound on one message's data */

#define PORT_CONNECT    0x0001
#define PORT_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | PORT_CONNECT)

enum lpc_kind
{
    LPC_CONNECTION_PORT,    /* the server's named port, where it receives */
    LPC_SERVER_PORT,        /* the server's end of one connection */
    LPC_CLIENT_PORT,        /* the client's end of one connection */
};

struct lpc_port;
struct lpc_wait;

struct lpc_message
{
    struct list         entry;      /* in a port's queue, or in the pending list */
    unsigned int        type;
    unsigned int        id;         /* message id */
    process_id_t        pid;        /* sender's client id */
    thread_id_t         tid;
    struct lpc_port    *via;        /* server port the message is for (weak: cleared when it goes) */
    struct lpc_port    *conn;       /* connection port it was queued on (weak, for pending messages) */
    struct lpc_wait    *wait;       /* sender's wait for the reply, or NULL */
    data_size_t         size;
    char               *data;
};

struct lpc_port
{
    struct object       obj;
    enum lpc_kind       kind;
    struct object      *sync;       /* connection port: signaled while its queue is not empty */
    struct list         queue;      /* connection port: messages to receive */
    struct lpc_port    *connection; /* comm ports: the connection port (strong) */
    struct lpc_port    *peer;       /* comm ports: the other end (weak) */
    client_ptr_t        context;    /* server port: the server's context for this connection */
    data_size_t         max_info;   /* connection port: max connection info */
    data_size_t         max_msg;    /* max message length */
    int                 connected;  /* comm ports: connection completed */
};

/* what a connecting or requesting thread waits on */
struct lpc_wait
{
    struct object       obj;
    struct object      *sync;
    unsigned int        status;     /* STATUS_PENDING until answered */
    unsigned int        id;         /* message id of the reply */
    process_id_t        pid;        /* the replier */
    thread_id_t         tid;
    data_size_t         size;
    char               *data;
    struct lpc_port    *port;       /* connect: the client's port, handed over when accepted */
    struct lpc_message *msg;        /* the message being answered, while pending (weak) */
};

static unsigned int lpc_last_id;
static struct list lpc_pending = LIST_INIT( lpc_pending );   /* sent, waiting for an answer */

static void lpc_port_dump( struct object *obj, int verbose );
static struct object *lpc_port_get_sync( struct object *obj );
static void lpc_port_destroy( struct object *obj );

static const struct object_ops lpc_port_ops =
{
    .size         = sizeof(struct lpc_port),
    .type         = &alpc_port_type,   /* on Windows 7 an LPC port is an ALPC Port */
    .dump         = lpc_port_dump,
    .get_sync     = lpc_port_get_sync,
    .destroy      = lpc_port_destroy,
};

static void lpc_wait_dump( struct object *obj, int verbose );
static struct object *lpc_wait_get_sync( struct object *obj );
static void lpc_wait_destroy( struct object *obj );

static const struct object_ops lpc_wait_ops =
{
    .size         = sizeof(struct lpc_wait),
    .type         = &no_type,
    .dump         = lpc_wait_dump,
    .get_sync     = lpc_wait_get_sync,
    .destroy      = lpc_wait_destroy,
};

static void free_message( struct lpc_message *msg )
{
    if (msg->wait) msg->wait->msg = NULL;
    free( msg->data );
    free( msg );
}

static struct lpc_message *alloc_message( unsigned int type, const void *data, data_size_t size )
{
    struct lpc_message *msg;

    if (size > LPC_MAX_DATA)
    {
        set_error( STATUS_PORT_MESSAGE_TOO_LONG );
        return NULL;
    }
    if (!(msg = mem_alloc( sizeof(*msg) ))) return NULL;
    memset( msg, 0, sizeof(*msg) );
    if (size && !(msg->data = memdup( data, size )))
    {
        free( msg );
        return NULL;
    }
    msg->type = type;
    msg->size = size;
    if (!++lpc_last_id) ++lpc_last_id;
    msg->id   = lpc_last_id;
    msg->pid  = current ? current->process->id : 0;
    msg->tid  = current ? current->id : 0;
    return msg;
}

/* answer a wait: wakes the connecting or requesting thread */
static void lpc_wait_answer( struct lpc_wait *wait, unsigned int status, const void *data, data_size_t size )
{
    if (wait->status != STATUS_PENDING) return;
    wait->status = status;
    wait->msg = NULL;
    free( wait->data );
    wait->data = NULL;
    wait->size = 0;
    if (size && (wait->data = memdup( data, size ))) wait->size = size;
    wait->pid = current ? current->process->id : 0;
    wait->tid = current ? current->id : 0;
    signal_sync( wait->sync );
}

/* drop a message that will never be received or answered */
static void fail_message( struct lpc_message *msg, unsigned int status )
{
    list_remove( &msg->entry );
    if (msg->wait)
    {
        lpc_wait_answer( msg->wait, status, NULL, 0 );
        release_object( &msg->wait->obj );
        msg->wait = NULL;
    }
    free_message( msg );
}

static void queue_message( struct lpc_port *conn, struct lpc_message *msg )
{
    assert( conn->kind == LPC_CONNECTION_PORT );
    msg->conn = conn;
    list_add_tail( &conn->queue, &msg->entry );
    signal_sync( conn->sync );
}

static void lpc_port_dump( struct object *obj, int verbose )
{
    struct lpc_port *port = (struct lpc_port *)obj;
    assert( obj->ops == &lpc_port_ops );
    fprintf( stderr, "LPC port kind=%d connected=%d\n", port->kind, port->connected );
}

static struct object *lpc_port_get_sync( struct object *obj )
{
    struct lpc_port *port = (struct lpc_port *)obj;
    assert( obj->ops == &lpc_port_ops );
    if (port->kind == LPC_CONNECTION_PORT) return grab_object( port->sync );
    if (port->connection) return grab_object( port->connection->sync );
    return grab_object( port->sync );
}

static void lpc_port_destroy( struct object *obj )
{
    struct lpc_port *port = (struct lpc_port *)obj;
    struct lpc_message *msg, *next;

    assert( obj->ops == &lpc_port_ops );

    switch (port->kind)
    {
    case LPC_CONNECTION_PORT:
        /* nobody will receive these, nor answer the requests already received */
        LIST_FOR_EACH_ENTRY_SAFE( msg, next, &port->queue, struct lpc_message, entry )
            fail_message( msg, msg->type == LPC_CONNECTION_REQUEST ? STATUS_PORT_CONNECTION_REFUSED
                                                                   : STATUS_PORT_DISCONNECTED );
        LIST_FOR_EACH_ENTRY_SAFE( msg, next, &lpc_pending, struct lpc_message, entry )
            if (msg->conn == port) fail_message( msg, STATUS_PORT_DISCONNECTED );
        break;

    case LPC_SERVER_PORT:
        /* accepted but never completed */
        LIST_FOR_EACH_ENTRY_SAFE( msg, next, &port->queue, struct lpc_message, entry )
            fail_message( msg, STATUS_PORT_CONNECTION_REFUSED );
        if (port->peer) port->peer->peer = NULL;
        if (port->connection)
        {
            /* the client's requests through this connection get no answer, and
             * its datagrams and its closing are of no interest any more */
            LIST_FOR_EACH_ENTRY_SAFE( msg, next, &port->connection->queue, struct lpc_message, entry )
                if (msg->via == port) fail_message( msg, STATUS_PORT_DISCONNECTED );
            if (list_empty( &port->connection->queue )) reset_sync( port->connection->sync );
            LIST_FOR_EACH_ENTRY_SAFE( msg, next, &lpc_pending, struct lpc_message, entry )
                if (msg->via == port) fail_message( msg, STATUS_PORT_DISCONNECTED );
        }
        break;

    case LPC_CLIENT_PORT:
        if (port->peer)
        {
            struct lpc_port *server = port->peer;
            server->peer = NULL;
            /* tell the server the connection is closed */
            if (server->connected && server->connection &&
                (msg = alloc_message( LPC_PORT_CLOSED, NULL, 0 )))
            {
                msg->via = server;
                queue_message( server->connection, msg );
            }
        }
        break;
    }
    if (port->connection) release_object( port->connection );
    if (port->sync) release_object( port->sync );
}

static void lpc_wait_dump( struct object *obj, int verbose )
{
    struct lpc_wait *wait = (struct lpc_wait *)obj;
    assert( obj->ops == &lpc_wait_ops );
    fprintf( stderr, "LPC wait status=%08x\n", wait->status );
}

static struct object *lpc_wait_get_sync( struct object *obj )
{
    struct lpc_wait *wait = (struct lpc_wait *)obj;
    assert( obj->ops == &lpc_wait_ops );
    return grab_object( wait->sync );
}

static void lpc_wait_destroy( struct object *obj )
{
    struct lpc_wait *wait = (struct lpc_wait *)obj;
    assert( obj->ops == &lpc_wait_ops );
    if (wait->msg) wait->msg->wait = NULL;  /* a pending message outlived its sender: no answer to give */
    if (wait->port) release_object( wait->port );
    if (wait->sync) release_object( wait->sync );
    free( wait->data );
}

static struct lpc_port *create_port( enum lpc_kind kind )
{
    struct lpc_port *port;

    if (!(port = alloc_object( &lpc_port_ops ))) return NULL;
    port->kind       = kind;
    port->sync       = NULL;
    port->connection = NULL;
    port->peer       = NULL;
    port->context    = 0;
    port->max_info   = 0;
    port->max_msg    = 0;
    port->connected  = 0;
    list_init( &port->queue );
    if (!(port->sync = create_internal_sync( 1, 0 )))
    {
        release_object( port );
        return NULL;
    }
    return port;
}

static struct lpc_wait *create_wait( void )
{
    struct lpc_wait *wait;

    if (!(wait = alloc_object( &lpc_wait_ops ))) return NULL;
    wait->status = STATUS_PENDING;
    wait->id     = 0;
    wait->pid    = 0;
    wait->tid    = 0;
    wait->size   = 0;
    wait->data   = NULL;
    wait->port   = NULL;
    wait->msg    = NULL;
    if (!(wait->sync = create_internal_sync( 1, 0 )))
    {
        release_object( wait );
        return NULL;
    }
    return wait;
}

static struct lpc_port *get_port( obj_handle_t handle, unsigned int access )
{
    return (struct lpc_port *)get_handle_obj( current->process, handle, access, &lpc_port_ops );
}

/* create a named connection port */
DECL_HANDLER(create_lpc_port)
{
    struct object_params params = { .ops = &lpc_port_ops, .access = PORT_ALL_ACCESS };
    struct lpc_port *port;

    if (!get_req_object_attributes( &params )) return;

    if (params.name.len)
    {
        /* a named object, made the usual way and initialised below */
        if ((port = create_named_object( &params )))
        {
            if (get_error() == STATUS_OBJECT_NAME_EXISTS)
            {
                set_error( STATUS_OBJECT_NAME_COLLISION );
                release_object( port );
                port = NULL;
            }
        }
    }
    else port = alloc_object( &lpc_port_ops );

    if (port)
    {
        port->kind       = LPC_CONNECTION_PORT;
        port->connection = NULL;
        port->peer       = NULL;
        port->context    = 0;
        port->connected  = 0;
        port->max_info   = req->max_info;
        port->max_msg    = req->max_msg;
        list_init( &port->queue );
        if ((port->sync = create_internal_sync( 1, 0 )))
            reply->handle = alloc_handle( current->process, port, PORT_ALL_ACCESS, params.attr );
        release_object( port );
    }
    if (params.root) release_object( params.root );
}

/* connect to a named connection port: queues the request; the caller waits on the returned wait */
DECL_HANDLER(connect_lpc_port)
{
    struct object_params params = { .ops = &lpc_port_ops };
    struct lpc_port *conn, *client;
    struct lpc_message *msg;
    struct lpc_wait *wait;
    const void *info;
    data_size_t info_len;

    if (!get_req_object_attributes( &params )) return;
    info = get_req_data_after_objattr( &params, &info_len );

    if (!(conn = open_named_object( &params ))) goto done;
    if (conn->kind != LPC_CONNECTION_PORT)
    {
        set_error( STATUS_OBJECT_TYPE_MISMATCH );
        goto done_conn;
    }
    if (conn->max_info && info_len > conn->max_info)
    {
        set_error( STATUS_INVALID_PARAMETER );
        goto done_conn;
    }
    if (!(client = create_port( LPC_CLIENT_PORT ))) goto done_conn;
    client->max_msg = conn->max_msg;
    if ((wait = create_wait()))
    {
        wait->port = (struct lpc_port *)grab_object( client );
        if ((msg = alloc_message( LPC_CONNECTION_REQUEST, info, info_len )))
        {
            msg->wait = (struct lpc_wait *)grab_object( wait );
            wait->msg = msg;
            wait->id  = msg->id;
            queue_message( conn, msg );
            reply->wait    = alloc_handle( current->process, wait, SYNCHRONIZE, 0 );
            reply->max_msg = conn->max_msg;
        }
        release_object( wait );
    }
    release_object( client );
done_conn:
    release_object( conn );
done:
    if (params.root) release_object( params.root );
}

/* receive the next message on a port (STATUS_PENDING if there is none: wait on the port) */
DECL_HANDLER(listen_lpc_port)
{
    struct lpc_port *port, *conn;
    struct lpc_message *msg;
    struct list *ptr;

    if (!(port = get_port( req->handle, 0 ))) return;

    conn = port->kind == LPC_CONNECTION_PORT ? port : port->connection;
    if (!conn)
    {
        /* a client port: receiving server-to-client requests is not supported */
        set_error( STATUS_NOT_IMPLEMENTED );
        release_object( port );
        return;
    }

    for (;;)
    {
        if (!(ptr = list_head( &conn->queue )))
        {
            reset_sync( conn->sync );
            set_error( STATUS_PENDING );
            break;
        }
        msg = LIST_ENTRY( ptr, struct lpc_message, entry );
        if (msg->size > get_reply_max_size())
        {
            set_error( STATUS_BUFFER_TOO_SMALL );
            break;
        }
        list_remove( &msg->entry );
        if (list_empty( &conn->queue )) reset_sync( conn->sync );

        reply->type    = msg->type;
        reply->id      = msg->id;
        reply->pid     = msg->pid;
        reply->tid     = msg->tid;
        reply->context = msg->via ? msg->via->context : 0;
        set_reply_data( msg->data, msg->size );

        if (msg->wait)
        {
            /* now the server's to answer, by message id */
            list_add_tail( &lpc_pending, &msg->entry );
        }
        else free_message( msg );
        break;
    }
    release_object( port );
}

static struct lpc_message *find_pending( unsigned int id, unsigned int type )
{
    struct lpc_message *msg;

    LIST_FOR_EACH_ENTRY( msg, &lpc_pending, struct lpc_message, entry )
        if (msg->id == id && (type == LPC_REQUEST ? msg->type != LPC_CONNECTION_REQUEST : msg->type == type))
            return msg;
    return NULL;
}

/* accept or refuse a received connection request */
DECL_HANDLER(accept_lpc_port)
{
    struct lpc_message *msg = find_pending( req->id, LPC_CONNECTION_REQUEST );
    struct lpc_wait *wait;
    struct lpc_port *server, *client;

    if (!msg || !msg->wait || !msg->wait->port)
    {
        set_error( STATUS_INVALID_CID );
        return;
    }
    wait = msg->wait;
    client = wait->port;

    if (!req->accept)
    {
        fail_message( msg, STATUS_PORT_CONNECTION_REFUSED );
        return;
    }
    if (!(server = create_port( LPC_SERVER_PORT ))) return;
    server->connection = (struct lpc_port *)grab_object( msg->conn );
    server->max_msg    = client->max_msg;
    server->context    = req->context;
    server->peer       = client;
    client->peer       = server;
    client->connection = NULL;

    /* the server may have changed the connection info: it goes back to the client */
    free( wait->data );
    wait->data = NULL;
    wait->size = 0;
    if (get_req_data_size() && (wait->data = memdup( get_req_data(), get_req_data_size() )))
        wait->size = get_req_data_size();

    /* the client is woken by complete_lpc_port: until then the server port
     * keeps the request (and its reference on the client's wait) */
    list_remove( &msg->entry );
    wait->msg = NULL;
    msg->via  = server;
    list_add_tail( &server->queue, &msg->entry );
    reply->handle = alloc_handle( current->process, server, PORT_ALL_ACCESS, 0 );
    release_object( server );
}

/* finish an accepted connection: the client's NtConnectPort returns */
DECL_HANDLER(complete_lpc_port)
{
    struct lpc_port *server;
    struct lpc_message *done;
    struct list *ptr;

    if (!(server = get_port( req->handle, 0 ))) return;
    if (server->kind != LPC_SERVER_PORT || !(ptr = list_head( &server->queue )))
    {
        set_error( STATUS_INVALID_PARAMETER );
        release_object( server );
        return;
    }
    done = LIST_ENTRY( ptr, struct lpc_message, entry );
    list_remove( &done->entry );
    server->connected = 1;
    if (server->peer) server->peer->connected = 1;
    if (done->wait)
    {
        struct lpc_wait *wait = done->wait;
        if (wait->status == STATUS_PENDING)
        {
            /* keep the accepted connection info */
            wait->status = STATUS_SUCCESS;
            wait->pid = current->process->id;
            wait->tid = current->id;
            signal_sync( wait->sync );
        }
        release_object( &wait->obj );
        done->wait = NULL;
    }
    free_message( done );
    release_object( server );
}

/* send a request (answered with reply_lpc_message) or a datagram */
DECL_HANDLER(send_lpc_message)
{
    struct lpc_port *port, *server;
    struct lpc_message *msg;
    struct lpc_wait *wait = NULL;

    if (!(port = get_port( req->handle, 0 ))) return;

    if (port->kind != LPC_CLIENT_PORT)
    {
        /* server-to-client requests are not supported */
        set_error( STATUS_NOT_IMPLEMENTED );
        goto done;
    }
    if (!(server = port->peer) || !server->connection)
    {
        set_error( STATUS_PORT_DISCONNECTED );
        goto done;
    }
    if (req->type != LPC_REQUEST && req->type != LPC_DATAGRAM)
    {
        set_error( STATUS_INVALID_PARAMETER );
        goto done;
    }
    if (!(msg = alloc_message( req->type, get_req_data(), get_req_data_size() ))) goto done;
    msg->via = server;
    if (req->type == LPC_REQUEST)
    {
        if (!(wait = create_wait()))
        {
            free_message( msg );
            goto done;
        }
        if (!(reply->wait = alloc_handle( current->process, wait, SYNCHRONIZE, 0 )))
        {
            release_object( wait );
            free_message( msg );
            goto done;
        }
        msg->wait = wait;   /* the message holds the reference */
        wait->msg = msg;
        wait->id  = msg->id;
    }
    reply->id = msg->id;
    queue_message( server->connection, msg );
done:
    release_object( port );
}

/* answer a received request (or a datagram's sender, which gets nothing) */
DECL_HANDLER(reply_lpc_message)
{
    struct lpc_port *port;
    struct lpc_message *msg;

    if (!(port = get_port( req->handle, 0 ))) return;
    if (!(msg = find_pending( req->id, LPC_REQUEST )))
        set_error( STATUS_REPLY_MESSAGE_MISMATCH );
    else if (get_req_data_size() > LPC_MAX_DATA)
        set_error( STATUS_PORT_MESSAGE_TOO_LONG );
    else
    {
        list_remove( &msg->entry );
        if (msg->wait)
        {
            lpc_wait_answer( msg->wait, STATUS_SUCCESS, get_req_data(), get_req_data_size() );
            msg->wait->id = msg->id;
            release_object( &msg->wait->obj );
            msg->wait = NULL;
        }
        free_message( msg );
    }
    release_object( port );
}

/* the answer a wait got: a connected port, or a reply's data */
DECL_HANDLER(get_lpc_reply)
{
    struct lpc_wait *wait;

    if (!(wait = (struct lpc_wait *)get_handle_obj( current->process, req->wait, 0, &lpc_wait_ops ))) return;

    if (wait->status == STATUS_PENDING) set_error( STATUS_PENDING );
    else if (wait->status) set_error( wait->status );
    else
    {
        if (wait->port)
        {
            if (wait->port->peer && wait->port->connected)
                reply->port = alloc_handle( current->process, wait->port, PORT_ALL_ACCESS, 0 );
            else set_error( STATUS_PORT_DISCONNECTED );
            release_object( wait->port );
            wait->port = NULL;
        }
        reply->id  = wait->id;
        reply->pid = wait->pid;
        reply->tid = wait->tid;
        set_reply_data( wait->data, min( wait->size, get_reply_max_size() ));
    }
    release_object( wait );
}
