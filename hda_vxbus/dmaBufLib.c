/* vxbDmaBufLib.c - buffer and DMA system for VxBus drivers */

/*
 * Copyright (c) 2005-2007 Wind River Systems, Inc.
 *
 * The right to copy, distribute, modify or otherwise make use
 * of this software may be licensed only pursuant to the terms
 * of an applicable Wind River license agreement.
 */

/*
modification history
--------------------
01s,10sep09,yjw  Fix logic bug in vxbDmaBufMapIoVecLoad().(WIND00180060)
01r,22mar09,wap  Add boundary checks
01q,26sep08,h_k  added a test for invalid block number case. (CQ:132074)
01p,28may08,dlk  Handle MIPS K0_TO_PHYS/PHYS_TO_K0 address translation here.
01o,27mar08,dlk  Fix compiler warning, also small logic bug in
                 vxbDmaBufMapMblkLoad().
01n,31oct07,wap  Performance optimizations: avoid excessive argument
                 validation checks in frequently used routines, cache
                 pointer to parent bus controller device instead of always
                 calling vxbParentGet(), assume x86 arch is always snooped
                 (WIND00104927)
01m,04sep07,pdg  corrected apigen errors
01l,15aug07,h_k  removed multiple global data definitions.
01k,19jul07,wap  Remove intLock()/intUnlock()
01j,13jun07,tor  remove VIRT_ADDR
01i,30jan07,wap  Use method declaration macros
01h,30jan07,wap  Use the heap for non-critical structures instead of hwMem to
                 reduce hwMem usage
01g,24aug06,pdg  fixed invalid memory access errors (Fix id:WIND00062743)
01f,19jul06,wap  Refine bounce buffer support
01e,14jul06,wap  Create extra fraglist entry in DMA maps for bounce buffering,
                 move DMA tag structure definition to vxbDmaBufLib.h, correct
                 a couple of method anchor names
01d,29jun06,wap  Add support for address translation
01c,25may06,wap  Fix multi-fragment map loading, and allocation alignment
                 handling
01c,12may06,dgp  doc: fix RETURNS formatting
01b,30jan06,pdg  Updated review comments and fixed bugs identified during
                 testing
01a,02Dec05,tor  created from FreeBSD DMA system
*/

/*

DESCRIPTION:

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
The map contains the physical addresses representing the buffer
which the driver can use to fill in to descriptor fields.  Each map
usually represents a single pending transfer.  Scatter/gather is
supported both with the UIO structure or with mBlks.

Buffers may be cached, and that is handled by the use of flush and
invalidate operations, vxbDmaBufMapFlush() and vxbDmaBufMapInvalidate().
Before initiating a write operation, the driver must call the flush
function.  After a buffer has been filled by the DMA device, the
driver must call the invalidate function before passing the data
to the OS/middleware module (e.g. the network stack or filesystem).
The vxbDmaBufSync() operation, with the same semantics as the FreeBSD
version, is available to help ease porting of FreeBSD drivers
to VxWorks.

Device descriptors are special in several ways.  Memory for the
descriptors must be allocated somehow, but DMA restrictions are
still in place for the descriptors, since both the CPU and the
device access the data contained in the descriptors.  This module
provides a memory allocation function, vxbDmaBufMemAlloc(), intended
primarily for use as descriptors.  In addition, descriptors
require DMA maps in order for the driver to program the descriptor
location into device register(s).  However, flush and invalidate
operations should be performed on an individual descriptor, and
not on the entire descriptor table.  Drivers may choose to have
a map for each descriptor, or a single map for all descriptors.
If a single map is used, flush and invalidate operations can
be performed on a range within a DMA map with the vxbDmaBufFlush()
and vxbDmaBufInvalidate() functions, which accept offset and size
arguments.

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
	vxbDmaBufSync(..., VXB_DMABUFSYNC_PREWRITE)

    during receive interrupt handling
        vxbDmaBufSync(..., VXB_DMABUFSYNC_POSTREAD)
	/@ in rfa_addbuf() @/
	    vxbDmaBufMapLoad()
	    vxbDmaBufSync(..., VXB_DMABUFSYNC_PREREAD | VXB_DMABUFSYNC_PREWRITE)

Current implementation notes:

This library is intended to support bounce buffers, where
the device reads and writes to one area of memory, but the
OS/middleware module uses a different region.  However, this
functionality is not currently implemented.

In the normal case, allocation for vxbDmaBufMemAlloc() is
currently performed with malloc() from the system memory
pool.  If the VXB_DMABUF_NOCACHE flag is specified in the
vxbDmaBufTagCreate() call, then cacheDmaMalloc() is used.  This has
several implications.  First, vxbDmaBufMemAlloc() cannot be called
until the system memory pool has been initialized, which occurs
before the VxBus init2 routine.  So vxbDmaBufMemAlloc() cannot
be used in the driver's probe or init1 routines.  Second, some
hardware platforms cannot use memory from the system memory pool,
whether cached or not.  Therefore, this module allows BSP hooks
for the BSP to override the memory allocation and free routines.
The routines must take the VXB_DEVICE_ID and size as arguments
to the allocation routine, and the VXB_DEVICE_ID and address as
arguments to the free routine.  If provided, the BSP routines
must allocate buffers for all devices, though internally they
can default to malloc() and free() for everything except the
special-requirements devices.  Once installed, the BSP hooks must
not be changed.

INCLUDE FILES: vxbDmaBufLib.h
*/

/* includes */

#include <vxWorks.h>
#include <intLib.h>
#include <string.h>
#include <cacheLib.h>
#include <stdlib.h>
#include <logLib.h>
#include <net/uio.h>
#include <netBufLib.h>

#include "audio/dmaBufLib.h"

/* defines */

#define DMABUF_DEBUG_ON
#define MAX_REGBASE 10		/* should be in target/h/hwif/vxbus/vxBus */

#if _VX_CPU_FAMILY == _VX_MIPS
#define ADDR_TRANS_FROM_VIRT(x) ((void *)K0_TO_PHYS((x)))
#define ADDR_TRANS_TO_VIRT(x) ((void *)PHYS_TO_K0((x)))
#else  /* _VX_CPU_FAMILY != _VX_MIPS */
#define ADDR_TRANS_FROM_VIRT(x) (x)
#define ADDR_TRANS_TO_VIRT(x) ((void *)(x))
#endif  /* _VX_CPU_FAMILY != _VX_MIPS */

/* typedefs */

/* globals */

/* function pointers available to be filled in by BSP */

void * (*vxbDmaBufBspAlloc)
    (
    int			size
    ) = NULL;
STATUS (*vxbDmaBufBspFree)
    (
    void *		vaddr
    ) = NULL;

/* function pointers available to be filled in by arch code */

STATUS (*vxbDmaBufArchInvalidate)
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    ) = NULL;
STATUS (*vxbDmaBufArchFlush)
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    ) = NULL;
STATUS (*vxbDmaBufMapArchInvalidate)
    (
    VXB_DMA_TAG_ID	dmaTagID,
    VXB_DMA_MAP_ID	map
    ) = NULL;
STATUS (*vxbDmaBufMapArchFlush)
    (
    VXB_DMA_TAG_ID	dmaTagID,
    VXB_DMA_MAP_ID	map
    ) = NULL;

/* locals */

/* forward declarations */

#if 0
/*******************************************************************************
*
* vxbDmaBufInit - initialize buffer and DMA system
*
* This routine initializes the buffer and DMA system.
*
* RETURNS: N/A
*
* ERRNO: N/A
*/

void vxbDmaBufInit (void)
    {
    }

/*******************************************************************************
*
* vxbDmaBufDefaultLock - default tag locking function
*
* This routine currently is a stub.
*
* RETURNS: N/A
*
* ERRNO: N/A
*/

LOCAL void vxbDmaBufDefaultLock
    (
    void *arg,
    void * op
    )
    {
    }
#endif

/*******************************************************************************
*
* vxbDmaBufTagParentGet - retrieve parent DMA tag
*
* This function retrieves the DMA tag associated with the upstream bus
* controller closest to the device, which provides a DMA tag.
*
* RETURNS: NULL
*
* ERRNO: N/A
*/

VXB_DMA_TAG_ID vxbDmaBufTagParentGet
    (
    UINT32 		pRegBaseIndex
    )
    {
#if 0 /* UNSUPPORTED_FUTURE_ENHANCEMENT */
    VXB_DEVICE_ID	pParent;
    VXB_DMA_TAG_ID	pTag;
    char *		lowAddr;
    char *		highAddr;

    pParent = vxbParentGet(pInst);

    pTag = pParent->pDmaTag;

    lowAddr = (bus_addr_t)pInst->pRegBase[pRegBaseIndex];
    highAddr = pInst->pRegSize[pRegBaseIndex];
    if ( highAddr == 0 )
        highAddr = 0xffffffff;

	while ( pTag != NULL )
		{
		if ( ( (UINT32)pRegBaseIndex >= lowAddr ) &&
			 ( (UINT32)pRegBaseIndex < highAddr ) )
			{
			return(pTag);
			}

		pTag = pTag->pNext;
		}
#endif

    return(NULL);
    }

/*******************************************************************************
*
* vxbDmaBufTagCreate - create a DMA tag
*
* This function creates a device-specific DMA tag with the given
* characteristics (type of allocation (general heap, uncached,
* dual-ported RAM, specific partition), required alignment, max
* and min physical address boundaries (can the PCI bridge see all
* memory in a 64-bit system, or just a 4GB window?), DMAable memory
* allocation size, max number of allowed entries in a DMA fragment
* list, etc...)
*
* Some devices impose a restriction that an individual DMA
* data segment may not cross a certain alignment boundary.
* For such cases, the <boundary> member of the VXB_DMA_TAG
* is non-zero (and a power of two) and specifies the boundary.
* If the given buffer would cross such a boundary, or
* if it exceeds the (independent) length limit given by
* the <maxSegSz> member, it will be split into multiple segments.
*
* RETURNS: pointer to the created DMA tag
*
* ERRNO: N/A
*/

VXB_DMA_TAG_ID vxbDmaBufTagCreate
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
    )
    {
    VXB_DMA_TAG_ID	pTag;
#if 0
    int			i;

    /* check if the instance is valid */

    if ( (pInst == NULL ) || ( pInst->pDriver == NULL ) )
        return NULL;
#endif

    /* allocate tag */

    pTag = malloc (sizeof (*pTag));
    if (pTag == NULL)
        return (NULL);

    /* initialize the memory */

    bzero((char *)pTag, sizeof(*pTag));

    /* save information from function args */

    pTag->parent = parent;

    /* fill in fields specified by arguments */

    pTag->alignment = alignment;
    pTag->boundary = boundary;
    pTag->lowAddr = lowAddr;
    pTag->highAddr = highAddr;
    pTag->filter = filter;
    pTag->filterArg = filterArg;
    pTag->maxSize = maxSize;
    pTag->maxSegSz = maxSegSz;
    pTag->flags = flags;

    if (nSegments != 0)
        pTag->nSegments = nSegments;
    else
        pTag->nSegments = 1;

#if 0
    if (lockFunc == NULL)
        {
        pTag->lockFuncArg = NULL;
        pTag->lockFunc = vxbDmaBufDefaultLock;
        }
    else
        {
        pTag->lockFunc = lockFunc;
        pTag->lockFuncArg = lockFuncArg;
        }
#endif

    pTag->refCount = 1;	/* count ourself */
    pTag->mapCount = 0;	/* no initial maps */

    /* account for restrictions of parent tag */

#if 0 /* UNSUPPORTED_FUTURE_ENHANCEMENT */
    if (parent != NULL) {
	pTag->lowAddr = min(parent->lowAddr, pTag->lowAddr);
	pTag->highAddr = max(parent->highAddr, pTag->highAddr);
	if (pTag->boundary == 0)
	    pTag->boundary = parent->boundary;
	else if (parent->boundary != 0)
	    pTag->boundary = MIN(parent->boundary,
			           pTag->boundary);
	if (pTag->filter == NULL) {
	    /*
	     * Short circuit looking at our parent directly
	     * since we have encapsulated all of its information
	     */
	    pTag->filter = parent->filter;
	    pTag->filterarg = parent->filterarg;
	    pTag->parent = parent->parent;
	}
	if (pTag->parent != NULL)
	    {

	    parent->refCount++;
	    }
    }
#endif

    /* mark tag as valid */

    pTag->vxbTagValid = TRUE;

    /* save instance in tag */
#if 0
    pTag->pInst = pInst;
#endif
#if 0 /* UNSUPPORTED_FUTURE_ENHANCEMENT */
    /* save tag in instance */

    pTag->pNext = pInst->pDmaTag;
    pInst->pDmaTag = pTag;
#endif

    /* fill in where caller requested */

    if ( ppDmaTag != NULL )
        *ppDmaTag = pTag;

    return(pTag);
    }

/*******************************************************************************
*
* vxbDmaBufTagDestroy - destroy a DMA tag
*
* This routine destroys the tag when we're done with it (usually
* during driver unload)
*
* RETURNS: 
* OK if the tag was valid and the tag structure's memory was freed,
* ERROR otherwise.
*
* ERRNO: N/A
*/

STATUS vxbDmaBufTagDestroy
    (
    VXB_DMA_TAG_ID 	dmaTagID
    )
    {

    if ( (dmaTagID != NULL) && 
         ( dmaTagID->vxbTagValid == TRUE ) && 
         ( dmaTagID->mapCount == 0 ) )
        {
        bzero ((char *)dmaTagID, sizeof(*dmaTagID));
        free (dmaTagID);
        return(OK);
        }

    return(ERROR);
    }

/*******************************************************************************
*
* vxbDmaBufMapCreate - create/allocate a DMA map
*
* This routine creates/allocates a DMA map for a transfer (used
* as an argument for vxbDmaBufMapLoad()/vxbDmaBufMapUnload() later --
* a map holds state like pointers to allocated bounce buffers).
*
* RETURNS: pointer to the created map ID
*
* ERRNO: N/A
*
* NOTE: Allocate a handle for mapping from kva/uva/physical
* address space into bus device space.
*/

VXB_DMA_MAP_ID vxbDmaBufMapCreate
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    int 		flags,
    VXB_DMA_MAP_ID *	mapp
    )
    {
    VXB_DMA_MAP_ID	pMap;
    int			structSize;
#if 0
    VXB_DEVICE_ID	pParent;
    FUNCPTR		mapCreateFunc;

    pParent = vxbDevParent (pInst);

    /*
     * See if the parent bus has a mapping routine. If
     * it does, call it, otherwise just use a generic default.
     */

    mapCreateFunc = vxbDevMethodGet (pParent,
        (UINT32)&vxbDmaBufMapCreate_desc);

    if (mapCreateFunc != NULL)
        {
        pMap = (VXB_DMA_MAP_ID)(mapCreateFunc) (pParent, pInst,
            dmaTagID, flags, mapp);
        if (pMap != NULL)
            pMap->pParent = pParent;
        return (pMap);
        }
#endif

    /* check if the parameters are valid */

    if ( /*(pInst == NULL ) || ( pInst->pDriver == NULL ) || */
         ( dmaTagID == NULL ) )
        return NULL;

    /* allocate structure and verify */

    structSize = sizeof(*pMap) + (sizeof(VXB_DMA_FRAG) * 
                                  (dmaTagID->nSegments - 1));

    pMap = malloc (structSize);

    if ( pMap == NULL )
        return(NULL);

    /* zero structure */

    bzero((char *)pMap, structSize);

#if 0
    /* Save creator object */

    pMap->pDev = pInst;
#endif
    /* save tag in map */

    pMap->dmaTagID = dmaTagID;

    /* increment mapCount */

    dmaTagID->mapCount++;

    /* save where requested */

    if ( mapp != NULL )
        *mapp = pMap;

    return(pMap);
    }

/*******************************************************************************
*
* vxbDmaBufMapDestroy - release a DMA map
*
* This routine releases a DMA map and zeros the memory associated with it
*
* RETURNS 
* OK if the map was valid and the storage could be freed,
* ERROR otherwise
*
* ERRNO: N/A
*
* NOTE: Destroy a handle for mapping from kva/uva/physical
* address space into bus device space.
*/

STATUS vxbDmaBufMapDestroy
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map
    )
    {
    int			structSize;
#if 0
    VXB_DEVICE_ID	pParent;
    FUNCPTR		mapDestroyFunc;
#endif

    if ( ( map == NULL ) || (dmaTagID == NULL) ||
                     ( dmaTagID != map->dmaTagID ) )
        return(ERROR);

#if 0
    pParent = vxbDevParent (map->pDev);

    /*
     * See if the parent bus has a mapping routine. If
     * it does, call it, otherwise just use a generic default.
     */

    mapDestroyFunc = vxbDevMethodGet (pParent,
        (UINT32)&vxbDmaBufMapDestroy_desc);

    if (mapDestroyFunc != NULL)
        return ((mapDestroyFunc) (pParent, dmaTagID, map));
#endif

    structSize = sizeof(*map) + (sizeof(VXB_DMA_FRAG) * 
                                                  (dmaTagID->nSegments - 1));
    bzero((char *)map, structSize);

    free (map);

    dmaTagID->mapCount--;

    return(OK);
    }

/*******************************************************************************
*
* vxbDmaBufMemAlloc - allocate DMA-able memory
*
* This routine allocates DMA-able memory that satisfies requirements
* encoded in the supplied DMA tag -- note: in the BSD API, the DMA
* tag controls the size of the memory block returned by
* this function. So if you need, say, 1536 byte blocks, you would
* create a tag that lets you allocate 1536-byte chunks of memory
* from whatever pool you need to satisfy the DMA constraints of
* your device. Note also that this offers the possibility of using
* a slab allocator to speed up allocation time
*
* RETURNS: a buffer pointer
*
* ERRNO: N/A
*
* NOTE: Allocate a piece of memory that can be efficiently mapped into
* bus device space based on the constraints listed in the DMA tag.
* A dmamap to for use with dmamap_load is also allocated.
*/

void * vxbDmaBufMemAlloc
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    void ** 		vaddr,
    int 		flags,
    VXB_DMA_MAP_ID *	pMap
    )
    {
    VXB_DMA_MAP_ID 	map = NULL;
    char		* pMem;
    int			size;
#if 0
    VXB_DEVICE_ID pParent;
    FUNCPTR memAllocFunc;
#endif
    /* check if the instance is valid */

    if ( /*( pInst == NULL ) || ( pInst->pDriver == NULL ) || */
         ( pMap == NULL ) || ( dmaTagID == NULL ) )
        return NULL;

    /* take smaller size */

    if ( dmaTagID->maxSegSz < dmaTagID->maxSize )
        size = dmaTagID->maxSegSz;
    else
        size = dmaTagID->maxSize;

    map = vxbDmaBufMapCreate(dmaTagID, flags, pMap);
    if ( map == NULL )
        return(NULL);
#if 0
    /*
     * See if the parent bus has an allocation routine. If
     * it does, call it, otherwise just use a generic default.
     */

    pParent = vxbDevParent (pInst);
    memAllocFunc = vxbDevMethodGet (pParent,
        (UINT32)&vxbDmaBufMapMemAlloc_desc);
    if (memAllocFunc != NULL)
        {
        pMem = (void *)memAllocFunc (pParent, dmaTagID, vaddr, flags, map);
        return (pMem);
        }

    if ( ( vxbDmaBufBspAlloc != NULL ) && ( vxbDmaBufBspFree != NULL ) )
        pMem = (*vxbDmaBufBspAlloc)(pInst, size);
    else if ( ( dmaTagID->flags & VXB_DMABUF_NOCACHE ) == VXB_DMABUF_NOCACHE )
#endif
    if ( ( dmaTagID->flags & VXB_DMABUF_NOCACHE ) == VXB_DMABUF_NOCACHE )
        {
        /*
         * cacheDmaMalloc() is guaranteed to return memory that's
         * aligned on a cache line boundary, but some devices have
         * additional alignment constraints. To account for them,
         * we might need to perform our own alignment fixups, and
         * for that we need to temporarily stash the real pointer
         * returned by cacheDmaMalloc() and return an adjusted
         * one. The real pointer will be needed later when time
         * comes to free the memory.
         */
        pMem = cacheDmaMalloc (size + dmaTagID->alignment);
        if (pMem != NULL)
            {
            map->pMem = pMem;
            pMem = (char *)ROUND_UP(pMem, dmaTagID->alignment);
            }
        }
    else
        pMem = memalign (dmaTagID->alignment, size);

    if (pMem == NULL)
        return (NULL);

    if (vaddr != NULL)
        *vaddr = pMem;

    return(pMem);
    }

/*******************************************************************************
*
* vxbDmaBufMemFree - release DMA-able memory
*
* This routine releases DMA-able memory allocated with
* vxbDmaBufMemAlloc().
*
* RETURNS: OK, always
*
* ERRNO: N/A
*
* NOTE:  Free a piece of memory and it's allocated dmamap, that was allocated
* via bus_dmamem_alloc().  Make the same choice for free/contigfree.
*/

STATUS vxbDmaBufMemFree
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    void *		vaddr,
    VXB_DMA_MAP_ID 	map
    )
    {
    STATUS		retVal = OK;
#if 0
    VXB_DEVICE_ID	pInst;
    VXB_DEVICE_ID	pParent;
    FUNCPTR		memFreeFunc;
#endif

    if ( ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;

#if 0
    /*
     * See if the parent bus has a mapping routine. If
     * it does, call it, otherwise just use a generic default.
     */

    pParent = vxbDevParent (map->pDev);
    memFreeFunc = vxbDevMethodGet (pParent,
        (UINT32)&vxbDmaBufMapMemFree_desc);
    if (memFreeFunc != NULL)
        return (memFreeFunc (pParent, dmaTagID, vaddr, map));

    pInst = dmaTagID->pInst;
    if ( ( vxbDmaBufBspAlloc != NULL ) && ( vxbDmaBufBspFree != NULL ) )
        (*vxbDmaBufBspFree)(pInst, vaddr);

    else if ( ( dmaTagID->flags & VXB_DMABUF_NOCACHE ) == VXB_DMABUF_NOCACHE )
#endif
    if ( ( dmaTagID->flags & VXB_DMABUF_NOCACHE ) == VXB_DMABUF_NOCACHE )
        cacheDmaFree (map->pMem);
    else
        free (vaddr);

    vxbDmaBufMapDestroy(dmaTagID, map);

    return(retVal);
    }

/*******************************************************************************
*
* vxbDmaBufMapLoad - map a virtual buffer
*
* This routine maps a virtual buffer into a physical
* address and length, using info in DMA tag to decided what sort
* of address translation and possible bounce-buffering may need to
* be done. Consumes a DMA map allocated with vxbDmaBufMapCreate().
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*
* NOTE: Map the buffer buf into bus space using the dmamap map.
*/

STATUS vxbDmaBufMapLoad
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    void *		buf,
    bus_size_t 		bufLen,
    int 		flags
    )
    {
    bus_size_t bmask;
    bus_size_t segSize;
    bus_addr_t pCur;
    bus_size_t bCur;
    int i;

#if 1 /*def VXB_DMA_BUF_DEBUG_CHECKS*/
    /* check if the parameters are valid */

    if ( /*( pInst == NULL ) || ( pInst->pDriver == NULL ) ||*/
         ( dmaTagID == NULL ) || ( map == NULL ) || ( buf == NULL ) ) 
        return ERROR;
#endif

#if 0
    /*
     * See if the parent bus has a mapping routine. If
     * it does, call it, otherwise just use a generic default.
     */

    if (map->mapLoadFunc != NULL)
        return ((map->mapLoadFunc) (map->pParent, dmaTagID, map,
            buf, bufLen, flags));
#endif

    bmask = ~(dmaTagID->boundary - 1);
    pCur = (bus_addr_t)buf;

    for (i = 0; bufLen > 0; i++)
        {
        if (i >= dmaTagID->nSegments)
            return (ERROR);
        if (bufLen > dmaTagID->maxSegSz)
            segSize = dmaTagID->maxSegSz;
        else
            segSize = bufLen;
        if (dmaTagID->boundary)
            {
            bCur = (pCur + dmaTagID->boundary) & bmask;
            if (segSize > (bCur - pCur))
                segSize = (bCur - pCur);
            }
        map->fragList[i].frag = ADDR_TRANS_FROM_VIRT ((void *)pCur);
        map->fragList[i].fragLen = segSize;
        bufLen -= segSize;
        pCur += segSize;
        }

    map->nFrags = i;

    return(OK);
    }

/*******************************************************************************
*
* vxbDmaBufMapMblkLoad - map a virtual buffer with mBlk
*
* This routine maps a virtual buffer.  Its behavior is like
* vxbDmaBufMapLoad(), but uses an mBlk tuple.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS vxbDmaBufMapMblkLoad
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    M_BLK_ID		pMblk,
    int 		flags
    )
    {
    M_BLK_ID	pFrag;
    int		nSegs;
    bus_size_t	bmask;

#if 1 /*def VXB_DMA_BUF_DEBUG_CHECKS*/
    /* check if the parameters are valid */

    if (( dmaTagID == NULL ) || ( map == NULL ) || ( pMblk == NULL ) )
        return ERROR;
#endif

#if 0
    /*
     * See if the parent bus has a mapping routine. If
     * it does, call it, otherwise just use a generic default.
     */

    if (map->mapLoadMblkFunc != NULL)
        return ((map->mapLoadMblkFunc) (map->pParent, dmaTagID,
            map, pMblk, flags));
#endif

    bmask = ~(dmaTagID->boundary - 1);

    for (pFrag = pMblk, nSegs = 0 ;
#ifdef	VXB_DMA_BUF_DEBUG_CHECKS
	 (pFrag != NULL) && (nSegs < dmaTagID->nSegments) ;
#else	/* VXB_DMA_BUF_DEBUG_CHECKS */
	 pFrag != NULL ;
#endif	/* VXB_DMA_BUF_DEBUG_CHECKS */
        pFrag = pFrag->mBlkHdr.mNext)
        {
        bus_size_t segSize;
        bus_size_t bCur;
        bus_addr_t pCur;
	bus_size_t len = pFrag->m_len;

	if (len == 0)
	    continue;
	
        pCur = mtod(pFrag, bus_addr_t);
        while (len > 0)
            {
	    if (nSegs >= dmaTagID->nSegments
#ifdef VXB_DMA_BUF_DEBUG_CHECKS
                || (map->fragList[nSegs].frag != NULL)
                || (map->fragList[nSegs].fragLen != 0)
#endif
                )
            return (ERROR);

            if (len > dmaTagID->maxSegSz)
                segSize = dmaTagID->maxSegSz;
            else
                segSize = len;
            if (dmaTagID->boundary)
                {
                bCur = (pCur + dmaTagID->boundary) & bmask;
                if (segSize > (bCur - pCur))
                    segSize = (bCur - pCur);
                }
	    map->fragList[nSegs].frag = ADDR_TRANS_FROM_VIRT((void *)pCur);
	    map->fragList[nSegs].fragLen = segSize;
	    nSegs++;
            len -= segSize;
            pCur += segSize;
            }
	}

#ifdef	VXB_DMA_BUF_DEBUG_CHECKS
    /*
     * check if pFrag is not NULL. This is possible, when the number of blocks
     * is more than the number of segments
     */

    if (pFrag != NULL)
	return ERROR;
#endif	/* VXB_DMA_BUF_DEBUG_CHECKS */

    map->nFrags = nSegs;

    return(OK);
    }

/*******************************************************************************
*
* vxbDmaBufMapIoVecLoad - map a virtual buffer with scatter/gather
*
* This routine maps a virtual buffer.  Its behavior is like
* vxbDmaBufMapLoad(), but works on a scatter/gather array of virtual
* buffers (called a struct iovec in BSD) -- useful for disk transfers
* of multiple data blocks.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS vxbDmaBufMapIoVecLoad
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    struct uio *	uio,
    int 		flags
    )
    {
    int			i, nSegs = 0;
    struct iovec *	uiovec;
    STATUS		retVal = OK;
    bus_size_t		bmask;

#if 1 /*def VXB_DMA_BUF_DEBUG_CHECKS*/
    /* check if the parameters are valid */

    if ( ( dmaTagID == NULL ) || ( map == NULL ) || ( uio == NULL ) )
        return ERROR;
#endif

    if ( uio->uio_iovcnt > dmaTagID->nSegments )
        return(ERROR);

#if 0
    /*
     * See if the parent bus has a mapping routine. If
     * it does, call it, otherwise just use a generic default.
     */

    if (map->mapLoadIoVecFunc != NULL)
        return ((map->mapLoadIoVecFunc) (map->pParent, dmaTagID,
            map, uio, flags));
#endif

    uiovec = uio->uio_iov;
    bmask = ~(dmaTagID->boundary - 1);

    for ( i = 0 ; i < uio->uio_iovcnt; i++ )
        {
        bus_size_t segSize, len;
        bus_size_t bCur;
        bus_addr_t pCur;

        if ((uiovec->iov_len > dmaTagID->maxSegSz)
#ifdef VXB_DMA_BUF_DEBUG_CHECKS
	    || (map->fragList[i].frag != NULL)
	    || (map->fragList[i].fragLen != 0)
#endif
	   )
           return ERROR;

        len = uiovec->iov_len;
        pCur = (UINT32)uiovec->iov_base;

        while (len > 0)
            {
            if (nSegs >= dmaTagID->nSegments
#ifdef VXB_DMA_BUF_DEBUG_CHECKS
                || (map->fragList[nSegs].frag != NULL)
                || (map->fragList[nSegs].fragLen != 0)
#endif
                )
            return (ERROR);

            if (len > dmaTagID->maxSegSz)
                segSize = dmaTagID->maxSegSz;
            else
                segSize = len;
            if (dmaTagID->boundary)
                {
                bCur = (pCur + dmaTagID->boundary) & bmask;
                if (segSize > (bCur - pCur))
                    segSize = (bCur - pCur);
                }
            map->fragList[nSegs].frag = ADDR_TRANS_FROM_VIRT((void *)pCur);
            map->fragList[nSegs].fragLen = segSize;
            nSegs++;
            len -= segSize;
            pCur += segSize;
            }
        uiovec++;
        }

    map->nFrags = nSegs;

    return(retVal);
    }

/*******************************************************************************
*
* vxbDmaBufMapUnload - unmap/destroy a previous virtual buffer mapping
*
* This routine unmaps/destroys a previous virtual buffer mapping
* after a transfer completes, possibly releasing any bounce buffers
* or other system resources consumed by mapping the virtual buffer.
*
* RETURNS: OK, always
*
* ERRNO: N/A
*/

STATUS vxbDmaBufMapUnload
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map
    )
    {
    int	i = 0;

#if 1 /*def VXB_DMA_BUF_DEBUG_CHECKS*/
    /* check if the parameters are valid */

    if ( ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;
#endif

#if 0
    /*
     * See if the parent bus has a mapping routine. If
     * it does, call it, otherwise just use a generic default.
     */

    if (map->mapUnloadFunc != NULL)
        return ((map->mapUnloadFunc) (map->pParent, dmaTagID, map));
#endif

    for (i = 0; i < dmaTagID->nSegments; i++)
        {
        map->fragList[i].frag = NULL;
        map->fragList[i].fragLen = 0;
        }

    return(OK);
    }

/*******************************************************************************
*
* vxbDmaBufMapFlush - flush DMA Map cache
*
* This routine does cache flush for the specified DMA map.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS vxbDmaBufMapFlush
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map
    )
    {
    STATUS		retVal = OK;
    int			i;

#if 1 /*def VXB_DMA_BUF_DEBUG_CHECKS*/
    /* check if the parameters are valid */

    if ( /*( pInst == NULL ) || ( pInst->pDriver == NULL ) ||*/
         ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;
#endif

#if 0
    if ( vxbDmaBufMapArchFlush != NULL )
        return((*vxbDmaBufMapArchFlush)(pInst, dmaTagID, map));
#endif

    /* flush individual fragments */

    for ( i = 0 ; i < map->nFrags ; i++ )
        {
        retVal = cacheFlush(DATA_CACHE,
			    ADDR_TRANS_TO_VIRT (map->fragList[i].frag),
			    map->fragList[i].fragLen);
        if (retVal != OK)
            break;
        }

    return(retVal);
    }

/*******************************************************************************
*
* vxbDmaBufMapInvalidate - invalidate DMA Map cache
*
* This routine does cache invalidate for the specified DMA map.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS vxbDmaBufMapInvalidate
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map
    )
    {
    STATUS		retVal = OK;
    int			i;

#if 1 /*def VXB_DMA_BUF_DEBUG_CHECKS*/
    /* check if the parameters are valid */

    if ( /*(pInst == NULL ) || ( pInst->pDriver == NULL ) ||*/
         ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;
#endif

#if 0
    if ( vxbDmaBufMapArchInvalidate != NULL )
        return((*vxbDmaBufMapArchInvalidate)(pInst, dmaTagID, map));
#endif

    /* invalidate individual fragments */

    for ( i = 0 ; i < map->nFrags ; i++ )
        {
        retVal = cacheInvalidate(DATA_CACHE,
				 ADDR_TRANS_TO_VIRT (map->fragList[i].frag),
				 map->fragList[i].fragLen);
        if (retVal != OK)
            break;
        }

    return(retVal);
    }

/*******************************************************************************
*
* vxbDmaBufFlush - partial cache flush for DMA map
*
* This routine does cache flush for a portion of the specified DMA map within
* one fragment.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS vxbDmaBufFlush
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    int			index,
    int			offset,		/* within fragment */
    int			length		/* within fragment */
    )
    {
    unsigned addr;

#if 1 /*def VXB_DMA_BUF_DEBUG_CHECKS*/
    /* check if the parameters are valid */

    if ( /*( pInst == NULL ) || ( pInst->pDriver == NULL ) ||*/
         ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;
#endif

    if ( index >= dmaTagID->nSegments ) 
        return(ERROR);

#if 0
    if ( vxbDmaBufArchFlush != NULL )
        return((*vxbDmaBufArchFlush)(pInst, 
                                     dmaTagID,
                                     map,
                                     index,
                                     offset,
                                     length));
#endif

    addr = (unsigned)map->fragList[index].frag + offset;
    cacheFlush(DATA_CACHE, ADDR_TRANS_TO_VIRT (addr), length);

    return(OK);
    }

/*******************************************************************************
*
* vxbDmaBufInvalidate - partial cache invalidate for DMA map
*
* This routine does cache invalidate for a portion of the specified DMA map within
* one fragment.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS vxbDmaBufInvalidate
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    int			index,
    int			offset,		/* within fragment */
    int			length		/* within fragment */
    )
    {
    unsigned addr;

#if 1 /*def VXB_DMA_BUF_DEBUG_CHECKS*/
    /* check if the parameters are valid */

    if ( /*( pInst == NULL ) || ( pInst->pDriver == NULL ) ||*/
         ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;
#endif

    if ( index >= dmaTagID->nSegments )
        return(ERROR);

#if 0
    if ( vxbDmaBufArchInvalidate != NULL )
        return((*vxbDmaBufArchInvalidate)(pInst,
                                          dmaTagID,
                                          map,
                                          index,
                                          offset,
                                          length));
#endif

    addr = (unsigned)map->fragList[index].frag + offset;
    cacheInvalidate(DATA_CACHE,
		    ADDR_TRANS_TO_VIRT (addr),
                    length);

    return(OK);
    }

/*******************************************************************************
*
* vxbDmaBufSync - do cache flushes or invalidates
*
* This routine does cache flushes or invalidates before/after
* transfers.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS vxbDmaBufSync
    (
    VXB_DMA_TAG_ID 	dmaTagID,
    VXB_DMA_MAP_ID 	map,
    bus_dmasync_op_t 	op
    )
    {
#if (CPU_FAMILY != I80X86)
    STATUS		retVal = OK;
#endif

#if 1 /*def VXB_DMA_BUF_DEBUG_CHECKS*/
    /* check if the parameters are valid */

    if ( /*( pInst == NULL ) || ( pInst->pDriver == NULL ) ||*/
         ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;
#endif

#if 0
    /*
     * See if the parent bus has a mapping routine. If
     * it does, call it, otherwise just use a generic default.
     */

    if (map->mapSyncFunc != NULL)
        return ((map->mapSyncFunc) (map->pParent, dmaTagID, map, op));
#endif

    /*
     * As a performance optimization, we make the assumption that,
     * barring any special cases mandated by the parent bus controller,
     * the x86 arch is always cache coherent. At the moment, the only
     * devices that use DMA for which we have drivers that support
     * vxbDmaBufLib are all PCI, and on the x86 arch, PCI is always
     * snooped.
     */

#if (CPU_FAMILY == I80X86)
    return (OK);
#else
    if (dmaTagID->flags & VXB_DMABUF_NOCACHE)
        return (OK);

    if ((op & VXB_DMABUFSYNC_POSTWRITE) == VXB_DMABUFSYNC_POSTWRITE)
        retVal = vxbDmaBufMapFlush (pInst, dmaTagID, map);

    if ((op & VXB_DMABUFSYNC_PREREAD) == VXB_DMABUFSYNC_PREREAD)
        retVal = vxbDmaBufMapInvalidate (pInst, dmaTagID, map);

    return (retVal);
#endif
    }


