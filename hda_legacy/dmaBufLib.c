/* dmaBufLib.c - buffer and DMA system drivers */

/*

DESCRIPTION:

This module provides support for driver DMA.  The organization
of this follows the FreeBSD bus_dma to some extent.

DMA devices are supported by use of a DMA Tag.  Each DMA tag
corresponds to a set of restrictions on DMA.  These restrictions
include information such as alignment, transaction size, minimum
and maximum address ranges, and so on.  Tags are created with
the dmaBufTagCreate() function.

Once a DMA tag is available, the driver should create DMA maps
to describe pending operations.  A map is created when a buffer
address (or a set of buffer addresses) is provided to this module.
The map contains the physical addresses representing the buffer
which the driver can use to fill in to descriptor fields.  Each map
usually represents a single pending transfer.  Scatter/gather is
supported both with the UIO structure or with mBlks.

Buffers may be cached, and that is handled by the use of flush and
invalidate operations, dmaBufMapFlush() and dmaBufMapInvalidate().
Before initiating a write operation, the driver must call the flush
function.  After a buffer has been filled by the DMA device, the
driver must call the invalidate function before passing the data
to the OS/middleware module (e.g. the network stack or filesystem).
The dmaBufSync() operation, with the same semantics as the FreeBSD
version, is available to help ease porting of FreeBSD drivers
to VxWorks.

Device descriptors are special in several ways.  Memory for the
descriptors must be allocated somehow, but DMA restrictions are
still in place for the descriptors, since both the CPU and the
device access the data contained in the descriptors.  This module
provides a memory allocation function, dmaBufMemAlloc(), intended
primarily for use as descriptors.  In addition, descriptors
require DMA maps in order for the driver to program the descriptor
location into device register(s).  However, flush and invalidate
operations should be performed on an individual descriptor, and
not on the entire descriptor table.  Drivers may choose to have
a map for each descriptor, or a single map for all descriptors.
If a single map is used, flush and invalidate operations can
be performed on a range within a DMA map with the dmaBufFlush()
and dmaBufInvalidate() functions, which accept offset and size
arguments.

Typical calling sequences include:

descriptors:

    dmaBufTagCreate()
    dmaBufMemAlloc()
    dmaBufFlush() and dmaBufInvalidate()

buffers:

    during initialization:
	dmaBufTagCreate()
	dmaBufMapCreate()

    during write setup
	dmaBufMapLoad() / dmaBufMapMblkLoad() / dmaBufMapIoVecLoad()
	dmaBufSync()

    during receive interrupt handling
        dmaBufSync()
	/@ in rfa_addbuf() @/
	    dmaBufMapLoad()
	    dmaBufSync()

Current implementation notes:

This library is intended to support bounce buffers, where
the device reads and writes to one area of memory, but the
OS/middleware module uses a different region.  However, this
functionality is not currently implemented.

INCLUDE FILES: dmaBufLib.h
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

void * (*dmaBufBspAlloc)
    (
    int			size
    ) = NULL;

STATUS (*dmaBufBspFree)
    (
    void *		vaddr
    ) = NULL;

/* function pointers available to be filled in by arch code */

STATUS (*dmaBufArchInvalidate)
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    ) = NULL;
STATUS (*dmaBufArchFlush)
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    ) = NULL;
STATUS (*dmaBufMapArchInvalidate)
    (
    DMA_TAG_ID	dmaTagID,
    DMA_MAP_ID	map
    ) = NULL;
STATUS (*dmaBufMapArchFlush)
    (
    DMA_TAG_ID	dmaTagID,
    DMA_MAP_ID	map
    ) = NULL;

/* locals */

/* forward declarations */

/*******************************************************************************
*
* dmaBufTagParentGet - retrieve parent DMA tag
*
* This function retrieves the DMA tag associated with the upstream bus
* controller closest to the device, which provides a DMA tag.
*
* RETURNS: NULL
*
* ERRNO: N/A
*/

DMA_TAG_ID dmaBufTagParentGet
    (
    UINT32 		pRegBaseIndex
    )
    {

    return(NULL);
    }

/*******************************************************************************
*
* dmaBufTagCreate - create a DMA tag
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
* For such cases, the <boundary> member of the DMA_TAG
* is non-zero (and a power of two) and specifies the boundary.
* If the given buffer would cross such a boundary, or
* if it exceeds the (independent) length limit given by
* the <maxSegSz> member, it will be split into multiple segments.
*
* RETURNS: pointer to the created DMA tag
*
* ERRNO: N/A
*/

DMA_TAG_ID dmaBufTagCreate
    (
    DMA_TAG_ID	parent,
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
    DMA_TAG_ID *	ppDmaTag
    )
    {
    DMA_TAG_ID	pTag;

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

    pTag->refCount = 1;	/* count ourself */
    pTag->mapCount = 0;	/* no initial maps */

    /* mark tag as valid */

    pTag->tagValid = TRUE;

    /* fill in where caller requested */

    if ( ppDmaTag != NULL )
        *ppDmaTag = pTag;

    return(pTag);
    }

/*******************************************************************************
*
* dmaBufTagDestroy - destroy a DMA tag
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

STATUS dmaBufTagDestroy
    (
    DMA_TAG_ID 	dmaTagID
    )
    {

    if ( (dmaTagID != NULL) && 
         ( dmaTagID->tagValid == TRUE ) && 
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
* dmaBufMapCreate - create/allocate a DMA map
*
* This routine creates/allocates a DMA map for a transfer (used
* as an argument for dmaBufMapLoad()/dmaBufMapUnload() later --
* a map holds state like pointers to allocated bounce buffers).
*
* RETURNS: pointer to the created map ID
*
* ERRNO: N/A
*
* NOTE: Allocate a handle for mapping from kva/uva/physical
* address space into bus device space.
*/

DMA_MAP_ID dmaBufMapCreate
    (
    DMA_TAG_ID 	dmaTagID,
    int 		flags,
    DMA_MAP_ID *	mapp
    )
    {
    DMA_MAP_ID	pMap;
    int			structSize;

    /* check if the parameters are valid */

    if ( /*(pInst == NULL ) || ( pInst->pDriver == NULL ) || */
         ( dmaTagID == NULL ) )
        return NULL;

    /* allocate structure and verify */

    structSize = sizeof(*pMap) + (sizeof(DMA_FRAG) * 
                                  (dmaTagID->nSegments - 1));

    pMap = malloc (structSize);

    if ( pMap == NULL )
        return(NULL);

    /* zero structure */

    bzero((char *)pMap, structSize);

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
* dmaBufMapDestroy - release a DMA map
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

STATUS dmaBufMapDestroy
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map
    )
    {
    int			structSize;

    if ( ( map == NULL ) || (dmaTagID == NULL) ||
                     ( dmaTagID != map->dmaTagID ) )
        return(ERROR);

    structSize = sizeof(*map) + (sizeof(DMA_FRAG) * 
                                                  (dmaTagID->nSegments - 1));
    bzero((char *)map, structSize);

    free (map);

    dmaTagID->mapCount--;

    return(OK);
    }

/*******************************************************************************
*
* dmaBufMemAlloc - allocate DMA-able memory
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

void * dmaBufMemAlloc
    (
    DMA_TAG_ID 	dmaTagID,
    void ** 		vaddr,
    int 		flags,
    DMA_MAP_ID *	pMap
    )
    {
    DMA_MAP_ID 	map = NULL;
    char		* pMem;
    int			size;

    /* check if the instance is valid */

    if ( /*( pInst == NULL ) || ( pInst->pDriver == NULL ) || */
         ( pMap == NULL ) || ( dmaTagID == NULL ) )
        return NULL;

    /* take smaller size */

    if ( dmaTagID->maxSegSz < dmaTagID->maxSize )
        size = dmaTagID->maxSegSz;
    else
        size = dmaTagID->maxSize;

    map = dmaBufMapCreate(dmaTagID, flags, pMap);
    if ( map == NULL )
        return(NULL);

    if ( ( dmaTagID->flags & DMABUF_NOCACHE ) == DMABUF_NOCACHE )
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
* dmaBufMemFree - release DMA-able memory
*
* This routine releases DMA-able memory allocated with
* dmaBufMemAlloc().
*
* RETURNS: OK, always
*
* ERRNO: N/A
*
* NOTE:  Free a piece of memory and it's allocated dmamap, that was allocated
* via bus_dmamem_alloc().  Make the same choice for free/contigfree.
*/

STATUS dmaBufMemFree
    (
    DMA_TAG_ID 	dmaTagID,
    void *		vaddr,
    DMA_MAP_ID 	map
    )
    {
    STATUS		retVal = OK;

    if ( ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;

    if ( ( dmaTagID->flags & DMABUF_NOCACHE ) == DMABUF_NOCACHE )
        cacheDmaFree (map->pMem);
    else
        free (vaddr);

    dmaBufMapDestroy(dmaTagID, map);

    return(retVal);
    }

/*******************************************************************************
*
* dmaBufMapLoad - map a virtual buffer
*
* This routine maps a virtual buffer into a physical
* address and length, using info in DMA tag to decided what sort
* of address translation and possible bounce-buffering may need to
* be done. Consumes a DMA map allocated with dmaBufMapCreate().
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*
* NOTE: Map the buffer buf into bus space using the dmamap map.
*/

STATUS dmaBufMapLoad
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
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

    /* check if the parameters are valid */

    if ( ( dmaTagID == NULL ) || ( map == NULL ) || ( buf == NULL ) ) 
        return ERROR;

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
* dmaBufMapMblkLoad - map a virtual buffer with mBlk
*
* This routine maps a virtual buffer.  Its behavior is like
* dmaBufMapLoad(), but uses an mBlk tuple.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS dmaBufMapMblkLoad
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    M_BLK_ID		pMblk,
    int 		flags
    )
    {
    M_BLK_ID	pFrag;
    int		nSegs;
    bus_size_t	bmask;

    /* check if the parameters are valid */

    if (( dmaTagID == NULL ) || ( map == NULL ) || ( pMblk == NULL ) )
        return ERROR;

    bmask = ~(dmaTagID->boundary - 1);

    for (pFrag = pMblk, nSegs = 0 ;
#ifdef	DMA_BUF_DEBUG_CHECKS
	 (pFrag != NULL) && (nSegs < dmaTagID->nSegments) ;
#else	/* DMA_BUF_DEBUG_CHECKS */
	 pFrag != NULL ;
#endif	/* DMA_BUF_DEBUG_CHECKS */
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
#ifdef DMA_BUF_DEBUG_CHECKS
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

#ifdef	DMA_BUF_DEBUG_CHECKS
    /*
     * check if pFrag is not NULL. This is possible, when the number of blocks
     * is more than the number of segments
     */

    if (pFrag != NULL)
	return ERROR;
#endif	/* DMA_BUF_DEBUG_CHECKS */

    map->nFrags = nSegs;

    return(OK);
    }

/*******************************************************************************
*
* dmaBufMapIoVecLoad - map a virtual buffer with scatter/gather
*
* This routine maps a virtual buffer.  Its behavior is like
* dmaBufMapLoad(), but works on a scatter/gather array of virtual
* buffers (called a struct iovec in BSD) -- useful for disk transfers
* of multiple data blocks.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS dmaBufMapIoVecLoad
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    struct uio *	uio,
    int 		flags
    )
    {
    int			i, nSegs = 0;
    struct iovec *	uiovec;
    STATUS		retVal = OK;
    bus_size_t		bmask;

    /* check if the parameters are valid */

    if ( ( dmaTagID == NULL ) || ( map == NULL ) || ( uio == NULL ) )
        return ERROR;

    if ( uio->uio_iovcnt > dmaTagID->nSegments )
        return(ERROR);

    uiovec = uio->uio_iov;
    bmask = ~(dmaTagID->boundary - 1);

    for ( i = 0 ; i < uio->uio_iovcnt; i++ )
        {
        bus_size_t segSize, len;
        bus_size_t bCur;
        bus_addr_t pCur;

        if ((uiovec->iov_len > dmaTagID->maxSegSz)
#ifdef DMA_BUF_DEBUG_CHECKS
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
#ifdef DMA_BUF_DEBUG_CHECKS
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
* dmaBufMapUnload - unmap/destroy a previous virtual buffer mapping
*
* This routine unmaps/destroys a previous virtual buffer mapping
* after a transfer completes, possibly releasing any bounce buffers
* or other system resources consumed by mapping the virtual buffer.
*
* RETURNS: OK, always
*
* ERRNO: N/A
*/

STATUS dmaBufMapUnload
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map
    )
    {
    int	i = 0;

    /* check if the parameters are valid */

    if ( ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;

    for (i = 0; i < dmaTagID->nSegments; i++)
        {
        map->fragList[i].frag = NULL;
        map->fragList[i].fragLen = 0;
        }

    return(OK);
    }

/*******************************************************************************
*
* dmaBufMapFlush - flush DMA Map cache
*
* This routine does cache flush for the specified DMA map.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS dmaBufMapFlush
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map
    )
    {
    STATUS		retVal = OK;
    int			i;

    /* check if the parameters are valid */

    if ( ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;

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
* dmaBufMapInvalidate - invalidate DMA Map cache
*
* This routine does cache invalidate for the specified DMA map.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS dmaBufMapInvalidate
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map
    )
    {
    STATUS		retVal = OK;
    int			i;

    /* check if the parameters are valid */

    if ( ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;

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
* dmaBufFlush - partial cache flush for DMA map
*
* This routine does cache flush for a portion of the specified DMA map within
* one fragment.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS dmaBufFlush
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    int			index,
    int			offset,		/* within fragment */
    int			length		/* within fragment */
    )
    {
    unsigned addr;

    /* check if the parameters are valid */

    if ( ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;

    if ( index >= dmaTagID->nSegments ) 
        return(ERROR);

    addr = (unsigned)map->fragList[index].frag + offset;
    cacheFlush(DATA_CACHE, ADDR_TRANS_TO_VIRT (addr), length);

    return(OK);
    }

/*******************************************************************************
*
* dmaBufInvalidate - partial cache invalidate for DMA map
*
* This routine does cache invalidate for a portion of the specified DMA map within
* one fragment.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS dmaBufInvalidate
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    int			index,
    int			offset,		/* within fragment */
    int			length		/* within fragment */
    )
    {
    unsigned addr;

    /* check if the parameters are valid */

    if ( ( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;

    if ( index >= dmaTagID->nSegments )
        return(ERROR);

    addr = (unsigned)map->fragList[index].frag + offset;
    cacheInvalidate(DATA_CACHE,
		    ADDR_TRANS_TO_VIRT (addr),
                    length);

    return(OK);
    }

/*******************************************************************************
*
* dmaBufSync - do cache flushes or invalidates
*
* This routine does cache flushes or invalidates before/after
* transfers.
*
* RETURNS: OK if successful, else ERROR
*
* ERRNO: N/A
*/

STATUS dmaBufSync
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    bus_dmasync_op_t 	op
    )
    {
#if (CPU_FAMILY != I80X86)
    STATUS		retVal = OK;
#endif

    /* check if the parameters are valid */

    if (( dmaTagID == NULL ) || ( map == NULL ) )
        return ERROR;

    /*
     * As a performance optimization, we make the assumption that,
     * barring any special cases mandated by the parent bus controller,
     * the x86 arch is always cache coherent. At the moment, the only
     * devices that use DMA for which we have drivers that support
     * dmaBufLib are all PCI, and on the x86 arch, PCI is always
     * snooped.
     */

#if (CPU_FAMILY == I80X86)
    return (OK);
#else
    if (dmaTagID->flags & DMABUF_NOCACHE)
        return (OK);

    if ((op & DMABUFSYNC_POSTWRITE) == DMABUFSYNC_POSTWRITE)
        retVal = dmaBufMapFlush (pInst, dmaTagID, map);

    if ((op & DMABUFSYNC_PREREAD) == DMABUFSYNC_PREREAD)
        retVal = dmaBufMapInvalidate (pInst, dmaTagID, map);

    return (retVal);
#endif
    }


