/* dmaBufLib.h - buffer and DMA system for VxBus drivers */

/*
 * Copyright (c) 2005-2007 Wind River Systems, Inc.
 *
 * The right to copy, distribute or otherwise make use of this software
 * may be licensed only pursuant to the terms of an applicable Wind River
 * license agreement.
 */

/*
modification history
--------------------
01l,30may08,dlk  Access headers under wrn/coreip directly.
01k,27mar08,dlk  Add mapLoadIpcomFunc member to vxbDmaBufMap, and
                 vxbDmaBufMapIpcomLoad() prototype.
01j,31oct07,wap  Cache pointer to parent bus controller in DMA map
                 (WIND00104927)
01i,21feb07,mil  Fixed missing include file vxBus.h.
01h,30jan07,wap  Use method declaration macros
01g,19jul06,wap  Refine bounce buffer support
01f,18jul06,wap  Add some includes to fix project build
01e,14jul06,wap  Add bounce buffer memory pointer to DMA map structure, move
                 DMA tag structure definition here from vxbDmaBufLib.c
01d,29jun06,wap  Add support for address translation
01c,25may06,wap  Add fragment count to map structure
01b,30jan06,pdg  Updated review comments
01a,02Dec05,tor  created from FreeBSD DMA system
*/

/*
This module provides support for driver DMA.  The organization
of this follows the FreeBSD bus_dma to some extent.

DMA devices are supported by use of a DMA Tag.  Each DMA tag
corresponds to a set of restrictions on DMA.  These restrictions
include information such as alignment, transaction size, minimum
and maximum address ranges, and so on.  Tags are created with
the vxbDmaBufTagCreate() function.

Once a DMA tag is available, the driver should create DMA maps
to describe pending operations.  A map is created when a buffer
address (or a set of buffer addresses) is provided to this module.
The map contains the physical addresses representing the buffer,
which the driver can use to fill in to descriptor fields.  Each map
usually represents a single pending transfer.  Scatter/gather is
supported both with the UIO structure or with mBlks.

Buffers may be cached, and that is handled by the use of
flush and invalidate operations, vxbDmaBufMapFlush() and
vxbDmaBufMapInvalidate().  Before initiating a write operation,
the driver must call the flush function.  After a buffer has been
filled by the DMA device, the driver must call the invalidate
function before passing the data to the OS/middleware module
(e.g. the network stack or filesystem).  The vxbDmaBufSync()
operation, with the same semantics as the FreeBSD version, is
available to help ease porting of FreeBSD drivers to VxWorks.

Device descriptors are special in several ways.  Memory for the
descriptors must be allocated somehow, but DMA restrictions are
still in place for the descriptors, since both the CPU and the
device access the data contained in the descriptors.  This module
provides a memory allocation function, vxbDmaBufMemAlloc(),
intended primarily for use as descriptors.  In addition,
descriptors require DMA maps in order for the driver to program
the descriptor location into device register(s).  However, flush
and invalidate operations should be performed on an individual
descriptor, and not on the entire descriptor table.  Drivers may
choose to have a map for each descriptor, or a single map for
all descriptors.  If a single map is used, flush and invalidate
operations can be performed on a range within a DMA map with the
vxbDmaBufFlush() and vxbDmaBufInvalidate() functions, which accept
offset and size arguments.

Typical calling sequences include:

descriptors:

    vxbDmaBufTagCreate()
    vxbDmaBufMemAlloc()
    vxbDmaBufFlush() and vxbDmaBufInvalidate()

buffers:

    during initialization:
	vxbDmaBufTagCreate()
	vxbDmaBufMapCreate()

    during write setup
	vxbDmaBufMapLoad() / vxbDmaBufMapMblkLoad() / vxbDmaBufMapIoVecLoad()
	vxbDmaBufMapFlush() / vxbDmaBufMapInvalidate()

    during receive interrupt handling
        vxbDmaBufSync(..., VXB_DMABUFSYNC_POSTREAD)
	/@ in rfa_addbuf() @/
	    vxbDmaBufMapLoad()
	    vxbDmaBufSync(..., VXB_DMABUFSYNC_PREREAD | BUS_DMASYNC_PREWRITE)

Current implementation notes:

This library is intended to support bounce buffers, where
the device reads and writes to one area of memory, but the
OS/middleware module uses a different region.  However, this
functionality is not currently implemented.

Allocation for vxbDmaBufMemAlloc() is currently performed
with malloc() from the system memory pool.  This has several
implications.  First, vxbDmaBufMemAlloc() cannot be called
until the system memory pool has been initialized, which occurs
before the VxBus init2 routine.  So vxbDmaBufMemAlloc() cannot
be used in the driver's probe or init1 routines.  Second, some
hardware platforms cannot use memory from the system memory pool.
Therefore, this module allows BSP hooks for the BSP to override
the memory allocation and free routines.  The routines must
take the VXB_DEVICE_ID and size as arguments to the allocation
routine, and the VXB_DEVICE_ID and address as arguments to the
free routine.  If provided, the BSP routines must allocate buffers
for all devices, though internally they can default to malloc()
and free() for everything except the special-requirements devices.

*/

#ifndef __INCvxbDmaBufBufLibh
#define __INCvxbDmaBufBufLibh

#ifdef __cplusplus
extern "C" {
#endif

/* includes */

#include <net/uio.h>
#include <netBufLib.h>
#include <net/mbuf.h>

/* defines */

/* vxbDmaBufTagCreate flags */

#define VXB_DMABUF_ALLOCNOW		0x00000001
#define VXB_DMABUF_NOCACHE		0x00000002

#define	VXB_DMABUFSYNC_POSTREAD		0x00010000
#define	VXB_DMABUFSYNC_POSTWRITE	0x00020000
#define	VXB_DMABUFSYNC_PREREAD		0x00040000
#define	VXB_DMABUFSYNC_PREWRITE		0x00080000

/* typedefs */

typedef unsigned long		bus_size_t;
typedef unsigned long		bus_addr_t;
typedef unsigned long		bus_dmasync_op_t;
typedef void			(bus_dma_filter_t)();
typedef void			(bus_dma_lock_t)();

typedef struct vxbDmaBufTag
    {
    struct vxbDmaBufTag *       pNext;          /* pointer to next tag */
    struct vxbDmaBufTag *       parent;         /* parent tag */
    BOOL                        vxbTagValid;    /* tag validity flag */
    bus_size_t                  alignment;      /* alignment for segments */
    bus_size_t                  boundary;       /* boundary for segments */
    bus_addr_t                  lowAddr;        /* low restricted address */
    bus_addr_t                  highAddr;       /* high restricted address */
    bus_dma_filter_t *          filter;         /* optional filter function */
    void *                      filterArg;      /* filter function argument */
    bus_size_t                  maxSize;        /* maximum mapping size */
    u_int                       nSegments;      /* number of segments */
    bus_size_t                  maxSegSz;       /* maximum segment size */
    int                         flags;          /* flags */
    int                         refCount;       /* reference count */
    int                         mapCount;       /* map count */
    bus_dma_lock_t *            lockFunc;       /* lock Function */
    void *                      lockFuncArg;    /* lock function argument */
    } VXB_DMA_TAG;

typedef VXB_DMA_TAG * VXB_DMA_TAG_ID;

typedef struct vxbDmaBufFrag VXB_DMA_FRAG;

struct vxbDmaBufFrag
    {
    void *		frag;		/* fragment pointer */
    bus_size_t		fragLen;	/* fragment length */
    };

typedef struct vxbDmaBufMap VXB_DMA_MAP;
typedef struct vxbDmaBufMap * VXB_DMA_MAP_ID;

struct vxbDmaBufMap
    {
    VXB_DMA_TAG_ID   	dmaTagID;
    FUNCPTR		mapLoadFunc;	/* parent bus load func */
    FUNCPTR		mapLoadMblkFunc;/* parent bus mBlk load */
    FUNCPTR		mapLoadIoVecFunc;/* parent bus iovec load func */
    FUNCPTR		mapLoadIpcomFunc; /* parent bus Ipcom_pkt load */
    FUNCPTR		mapUnloadFunc;	/* parent bus unload func */
    FUNCPTR		mapSyncFunc;	/* parent bus sync func */
    void		*pMem;		/* for alignment fixups */
    void		*pMemBounce;	/* for bounce buffering */
    int			nFrags;		/* number of valid frags */
    VXB_DMA_FRAG	fragList[1]; /* list of buffer fragments */
    };


/* globals */


/* function pointers available to be filled in by BSP */

IMPORT void * (*vxbDmaBufBspAlloc)
    (
    int			size
    );
IMPORT STATUS (*vxbDmaBufBspFree)
    (
    void *		vaddr
    );

/* function pointers available to be filled in by arch code */

IMPORT STATUS (*vxbDmaBufArchInvalidate)
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    );
IMPORT STATUS (*vxbDmaBufArchFlush)
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    );
IMPORT STATUS (*vxbDmaBufMapArchInvalidate)
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map
    );
IMPORT STATUS (*vxbDmaBufMapArchFlush)
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map
    );

/* locals */

/* forward declarations */

IMPORT void vxbDmaBufInit();

IMPORT VXB_DMA_TAG_ID vxbDmaBufTagParentGet
    (
    UINT32 		pRegBaseIndex
    );

IMPORT VXB_DMA_TAG_ID vxbDmaBufTagCreate
    (
    VXB_DMA_TAG_ID	parent,
    bus_size_t		alignment,
    bus_size_t		boundary,
    bus_addr_t		lowAddr,
    bus_addr_t		highAddr,
    bus_dma_filter_t *	filter,
    void *		filterArg,
    bus_size_t 		maxSize,
    int 		nSegments,
    bus_size_t 		maxSegSz,
    int 		flags,
    bus_dma_lock_t *	lockFunc,
    void *		lockFuncArg,
    VXB_DMA_TAG_ID *	ppDmaTag
    );

IMPORT STATUS vxbDmaBufTagDestroy
    (
    VXB_DMA_TAG_ID 	dmaTagID
    );

IMPORT VXB_DMA_MAP_ID vxbDmaBufMapCreate
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    int 		flags,
    VXB_DMA_MAP_ID *	mapp
    );

IMPORT STATUS vxbDmaBufMapDestroy
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map
    );

IMPORT void * vxbDmaBufMemAlloc
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    void ** 		vaddr,
    int 		flags,
    VXB_DMA_MAP_ID *	pMap
    );

IMPORT STATUS vxbDmaBufMemFree
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    void *		vaddr,
    VXB_DMA_MAP_ID 	map
    );

IMPORT STATUS vxbDmaBufMapLoad
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    void *		buf,
    bus_size_t 		bufLen,
    int 		flags
    );

IMPORT STATUS vxbDmaBufMapMblkLoad
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    M_BLK_ID		pMblk,
    int 		flags
    );

IMPORT STATUS vxbDmaBufMapIoVecLoad
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    struct uio *	uio,
    int 		flags
    );

struct Ipcom_pkt_struct;

IMPORT STATUS vxbDmaBufMapIpcomLoad
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    struct Ipcom_pkt_struct * pkt,
    int 		flags
    );

IMPORT STATUS vxbDmaBufMapUnload
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map
    );

IMPORT STATUS vxbDmaBufMapFlush
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map
    );

IMPORT STATUS vxbDmaBufMapInvalidate
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map
    );

IMPORT STATUS vxbDmaBufFlush
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    );

IMPORT STATUS vxbDmaBufInvalidate
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    );

IMPORT STATUS vxbDmaBufSync
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    bus_dmasync_op_t 	op
    );

#ifdef __cplusplus
}
#endif

#endif /* __INCvxbDmaBufBufLibh */
