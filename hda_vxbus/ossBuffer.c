/* ossBuffer.c - OSS Audio buffer management header */

/*
 * Copyright (c) 2012 Wind River Systems, Inc.
 *
 * The right to copy, distribute, modify or otherwise make use
 * of this software may be licensed only pursuant to the terms
 * of an applicable Wind River license agreement.
 */

/*
modification history
--------------------
01a,21dec12,jlj  write from ossAudio.c.
*/

/*
  DESCRIPTION

  This file support the management of OSS audio buffer.
*/

#include <ioLib.h>
#include <iosLib.h>
#include <fcntl.h>
#include <semLib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "audio/dmaBufLib.h"
#include "audio/ossAudio.h"

STATUS sndbuf_alloc (SND_BUF *b, VXB_DMA_TAG_ID dmatag, int dmaflags, unsigned int size)
    {
    STATUS status = ERROR;

	b->dma_tag = dmatag;
	b->maxsize = b->blksz * b->blkcnt;
    b->bufsize = b->blksz * b->blkcnt;
    b->head = 0;
    b->tail = 0;

    if ((b->buf_addr = vxbDmaBufMemAlloc (b->dma_tag,
                           NULL,
                           b->dma_flags,
                           &b->dma_map)) == NULL)
        return status;

    if (vxbDmaBufMapLoad(b->dma_tag,
                         b->dma_map,
                         b->buf_addr,
                         b->bufsize, 0) == ERROR)
        {
        vxbDmaBufMemFree (b->dma_tag, b->buf_addr, b->dma_map);
        return status;
        }

    /* Create shadow buffer for audio format convertion */
    
#define HDA_BUFSZ_DEFAULT       (65536*2)

    if ((b->shadow_buf_addr = (void*)malloc(HDA_BUFSZ_DEFAULT)) == NULL)
        return status;

#undef HDA_BUFSZ_DEFAULT

    status = sndbuf_resize (b, 2, b->maxsize / 2);
    return status;
    }
                    
SND_BUF * sndbuf_create(struct pcm_channel *channel)
    {
	struct snd_buf *b;
    
	b = calloc(1, sizeof(*b));
	b->channel = channel;
	return b;
    }

void sndbuf_reset(SND_BUF *b)
    {
    b->tail = 0;
    b->head = 0;
    }

void sndbuf_free(SND_BUF *b)
    {
	if (b->buf_addr)
        {
        if (b->dma_map)
            vxbDmaBufMapUnload (b->dma_tag, b->dma_map);

        if (b->dma_tag)
            vxbDmaBufMemFree (b->dma_tag, b->buf_addr, b->dma_map);
        }
    if(b->shadow_buf_addr)
        free(b->shadow_buf_addr);
    
    b->bufsize = 0;
	b->maxsize = 0;
	b->buf_addr = NULL;
	b->dma_tag = NULL;
	b->dma_map = NULL;
    b->head = b->tail = 0;
    }

void sndbuf_destroy(SND_BUF *b)
    {
    sndbuf_free(b);
    free(b);
    }

STATUS sndbuf_resize(SND_BUF *b, unsigned int blkcnt, unsigned int blksz)
    {
    STATUS status = OK;
    
	b->blksz = blksz;
    b->blkcnt = blkcnt;
    b->bufsize = b->blksz * b->blkcnt;
    b->head = 0;
    b->tail = 0;
    
    return status;
    }

size_t sndbuf_copy (const char *source, SND_BUF * b, size_t nbytes)
    {
    caddr_t bufaddr;
    signed int limit, delta, size, len, len2;
    signed int h,t;
    
    bufaddr = (caddr_t)(UINT32)sndbuf_getbufaddr(b);
    size = sndbuf_getsize (b);

    h = b->head;
    t = b->tail;
    
    delta = h - t;
    limit = 0;

    if (delta > 0)
        {
        len = size - h;
        len = min(len, nbytes);

        bcopy (source, bufaddr + h, len);
        limit += len;
        }
    else if (delta < 0)
        {
        len = t - h;
        len = min(len, nbytes);

        bcopy (source, bufaddr + h, len);
        limit += len;
        }
    else /* delta == 0 */
        {
        len = sndbuf_getsize(b) - h;
        len = min(len, nbytes);
        bcopy (source, bufaddr + h, len);
        limit += len;

        len2 = min(h, nbytes - len);
        if (len2 > 0)
            {
            bcopy (source + len, bufaddr, len2);
            limit += len2;
            }
        }

    h = h + limit;
    h = h % size;
    b->head = h;

    return limit;
    }

size_t sndbuf_read (char *dest, SND_BUF * b, size_t nbytes)
    {
    caddr_t bufaddr;
    signed int limit, bufsize, len;
    signed int h,t;
    
    bufaddr = (caddr_t)(UINT32)sndbuf_getbufaddr(b);
    bufsize = sndbuf_getsize (b);

    h = b->head;
    t = b->tail;

    len = min (nbytes, sndbuf_getblksz(b));
    limit = sndbuf_getblksz(b);

    bcopy (bufaddr + h, dest, len);

    h = h + limit;
    h = h % bufsize;
    b->head = h;

    return len;
    }

