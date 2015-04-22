/* dmaBufLib.h - buffer and DMA system drivers */

#ifndef __INCdmaBufBufLibh
#define __INCdmaBufBufLibh

#ifdef __cplusplus
extern "C" {
#endif

/* includes */

#include <net/uio.h>
#include <netBufLib.h>
#include <net/mbuf.h>

/* defines */

/* dmaBufTagCreate flags */

#define DMABUF_ALLOCNOW		0x00000001
#define DMABUF_NOCACHE		0x00000002

#define	DMABUFSYNC_POSTREAD		0x00010000
#define	DMABUFSYNC_POSTWRITE	0x00020000
#define	DMABUFSYNC_PREREAD		0x00040000
#define	DMABUFSYNC_PREWRITE		0x00080000

/* typedefs */

typedef unsigned long		bus_size_t;
typedef unsigned long		bus_addr_t;
typedef unsigned long		bus_dmasync_op_t;
typedef void			(bus_dma_filter_t)();
typedef void			(bus_dma_lock_t)();

typedef struct dmaBufTag
    {
    struct dmaBufTag *       pNext;          /* pointer to next tag */
    struct dmaBufTag *       parent;         /* parent tag */
    BOOL                        tagValid;    /* tag validity flag */
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
    } DMA_TAG;

typedef DMA_TAG * DMA_TAG_ID;

typedef struct dmaBufFrag DMA_FRAG;

struct dmaBufFrag
    {
    void *		frag;		/* fragment pointer */
    bus_size_t		fragLen;	/* fragment length */
    };

typedef struct dmaBufMap DMA_MAP;
typedef struct dmaBufMap * DMA_MAP_ID;

struct dmaBufMap
    {
    DMA_TAG_ID   	dmaTagID;
    FUNCPTR		mapLoadFunc;	/* parent bus load func */
    FUNCPTR		mapLoadMblkFunc;/* parent bus mBlk load */
    FUNCPTR		mapLoadIoVecFunc;/* parent bus iovec load func */
    FUNCPTR		mapLoadIpcomFunc; /* parent bus Ipcom_pkt load */
    FUNCPTR		mapUnloadFunc;	/* parent bus unload func */
    FUNCPTR		mapSyncFunc;	/* parent bus sync func */
    void		*pMem;		/* for alignment fixups */
    void		*pMemBounce;	/* for bounce buffering */
    int			nFrags;		/* number of valid frags */
    DMA_FRAG	fragList[1]; /* list of buffer fragments */
    };


/* globals */


/* function pointers available to be filled in by BSP */

IMPORT void * (*dmaBufBspAlloc)
    (
    int			size
    );

IMPORT STATUS (*dmaBufBspFree)
    (
    void *		vaddr
    );

/* function pointers available to be filled in by arch code */

IMPORT STATUS (*dmaBufArchInvalidate)
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    );
IMPORT STATUS (*dmaBufArchFlush)
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    );
IMPORT STATUS (*dmaBufMapArchInvalidate)
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map
    );
IMPORT STATUS (*dmaBufMapArchFlush)
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map
    );

/* locals */

/* forward declarations */

IMPORT DMA_TAG_ID dmaBufTagParentGet
    (
    UINT32 		pRegBaseIndex
    );

IMPORT DMA_TAG_ID dmaBufTagCreate
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
    );

IMPORT STATUS dmaBufTagDestroy
    (
    DMA_TAG_ID 	dmaTagID
    );

IMPORT DMA_MAP_ID dmaBufMapCreate
    (
    DMA_TAG_ID 	dmaTagID,
    int 		flags,
    DMA_MAP_ID *	mapp
    );

IMPORT STATUS dmaBufMapDestroy
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map
    );

IMPORT void * dmaBufMemAlloc
    (
    DMA_TAG_ID 	dmaTagID,
    void ** 		vaddr,
    int 		flags,
    DMA_MAP_ID *	pMap
    );

IMPORT STATUS dmaBufMemFree
    (
    DMA_TAG_ID 	dmaTagID,
    void *		vaddr,
    DMA_MAP_ID 	map
    );

IMPORT STATUS dmaBufMapLoad
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    void *		buf,
    bus_size_t 		bufLen,
    int 		flags
    );

IMPORT STATUS dmaBufMapMblkLoad
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    M_BLK_ID		pMblk,
    int 		flags
    );

IMPORT STATUS dmaBufMapIoVecLoad
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    struct uio *	uio,
    int 		flags
    );

struct Ipcom_pkt_struct;

IMPORT STATUS dmaBufMapIpcomLoad
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    struct Ipcom_pkt_struct * pkt,
    int 		flags
    );

IMPORT STATUS dmaBufMapUnload
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map
    );

IMPORT STATUS dmaBufMapFlush
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map
    );

IMPORT STATUS dmaBufMapInvalidate
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map
    );

IMPORT STATUS dmaBufFlush
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    );

IMPORT STATUS dmaBufInvalidate
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    int			index,
    int			offset,
    int			length
    );

IMPORT STATUS dmaBufSync
    (
    DMA_TAG_ID 	dmaTagID,
    DMA_MAP_ID 	map,
    bus_dmasync_op_t 	op
    );

#ifdef __cplusplus
}
#endif

#endif /* __INCdmaBufBufLibh */
