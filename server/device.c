/*
 * Server-side device management
 *
 * Copyright (C) 1999 Alexandre Julliard
 */

/*
 * FIXME:
 * all this stuff is a simple hack to avoid breaking
 * client-side device support.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "winbase.h"

#include "handle.h"
#include "thread.h"
#include "request.h"

struct device
{
    struct object  obj;             /* object header */
    int            id;              /* client identifier */
};

static void device_dump( struct object *obj, int verbose );
static int device_get_info( struct object *obj, struct get_file_info_reply *reply, int *flags );

static const struct object_ops device_ops =
{
    sizeof(struct device),    /* size */
    device_dump,              /* dump */
    no_add_queue,             /* add_queue */
    NULL,                     /* remove_queue */
    NULL,                     /* signaled */
    NULL,                     /* satisfied */
    NULL,                     /* get_poll_events */
    NULL,                     /* poll_event */
    no_get_fd,                /* get_fd */
    no_flush,                 /* flush */
    device_get_info,          /* get_file_info */
    NULL,                     /* queue_async */
    no_destroy                /* destroy */
};

static struct device *create_device( int id )
{
    struct device *dev;
    if ((dev = alloc_object( &device_ops, -1 )))
    {
        dev->id = id;
    }
    return dev;
}

static void device_dump( struct object *obj, int verbose )
{
    struct device *dev = (struct device *)obj;
    assert( obj->ops == &device_ops );
    fprintf( stderr, "Device id=%08x\n", dev->id );
}

static int device_get_info( struct object *obj, struct get_file_info_reply *reply, int *flags )
{
    struct device *dev = (struct device *)obj;
    assert( obj->ops == &device_ops );

    if (reply)
    {
        reply->type        = FILE_TYPE_UNKNOWN;
        reply->attr        = dev->id;  /* hack! */
        reply->access_time = 0;
        reply->write_time  = 0;
        reply->size_high   = 0;
        reply->size_low    = 0;
        reply->links       = 0;
        reply->index_high  = 0;
        reply->index_low   = 0;
        reply->serial      = 0;
    }
    *flags = 0;
    return FD_TYPE_DEFAULT;
}

/* create a device */
DECL_HANDLER(create_device)
{
    struct device *dev;

    reply->handle = 0;
    if ((dev = create_device( req->id )))
    {
        reply->handle = alloc_handle( current->process, dev, req->access, req->inherit );
        release_object( dev );
    }
}
