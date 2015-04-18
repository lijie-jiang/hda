/* ossAudio.h - OSS Audio driver header */

/*
modification history
--------------------
01b,28may28,dmh  fix B0003
01a,06mar12,dmh  written.
*/

#ifndef __INCossAudioh
#define __INCossAudioh

#include <ioLib.h>
#include <iosLib.h>
#include <fcntl.h>
#include <semLib.h>
#include <lstLib.h>
#include <selectLib.h>
#include <drv/sound/soundcard.h>

#include <hwif/util/vxbDmaBufLib.h>

/* device driver prototype */

#ifdef _WRS_CONFIG_LP64
    typedef long  _Vx_ioctl_arg_t;
#else
    typedef int   _Vx_ioctl_arg_t;
#endif  /*_WRS_CONFIG_LP64*/


#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

struct snd_mixer;

typedef struct snd_mixer
    {
    VXB_DEVICE_ID       pDev;
    void *              pdevinfo;
    UINT32              devs;
    UINT32              recdevs;
    UINT32              recsrc;
    UINT32              realdev[32];
    UINT16              level[32];
    } SND_MIXER;

struct pcm_channel;
struct snd_buf;

typedef struct snd_buf
    {
    VXB_DEVICE_ID       dev;
    UINT32              bufsize;
    UINT32              maxsize;
    UINT32              blksz;
    UINT32              blkcnt;
    caddr_t             buf;
    int                 head;
    int                 tail;
    VXB_DMA_TAG_ID      dma_tag;
    VXB_DMA_MAP_ID      dma_map;
    int                 dma_flags;
    void               *buf_addr;
    void               *shadow_buf_addr;
    struct pcm_channel *channel;
    } SND_BUF;


typedef struct pcmchan_caps
    {
    UINT32      minspeed;
    UINT32      maxspeed;
    UINT32      *fmtlist;
    UINT32      caps;
    } PCMCHAN_CAPS;


/* PCM_CHANNEL flags */

/* direction */
#define PCM_DIR_NONE            0x00000000
#define PCM_DIR_PLAY            0x00000001
#define PCM_DIR_REC             0x00000002

/* states */

#define CHAN_STATE_STOPPED       0x00000000
#define CHAN_STATE_RUNNING       0x00000001
#define CHAN_FLAG_ENABLE         0x00000010
#define CHAN_FLAG_TRIGGER        0x00000020

#define CHANNEL_ONE 0
#define CHANNEL_TWO 1

typedef struct pcm_channel
    {
    VXB_DEVICE_ID       pDev;
    int                 refcount;
    int                 dir;
    int                 channels; /* selected number of audio channels */
    int                 rate;   /* selected sample rate */
    int                 afmt;   /* selected format */
    int                 afmts;  /* supported formats */
    audio_buf_info      abinfo; /* frag and buffer information */
    struct snd_buf *    sndbuf; /* circular sample buffer */
    void *              stream; /* opaque pointer used by drivers */
    SEM_ID              sem;    /* counting semaphore */
    SEM_ID              msem;   /* mutex semaphore */
    int                 semcnt; /* initial count for CSem */
    UINT32              flags;  /* flags and options */
    } PCM_CHANNEL;

/* commands */
#define PCMTRIG_START    1
#define PCMTRIG_EMLDMAWR 2
#define PCMTRIG_EMLDMARD 3
#define PCMTRIG_STOP     0
#define PCMTRIG_ABORT   -1


#define PCMTRIG_COMMON(x)	((x) == PCMTRIG_START ||		\
				 (x) == PCMTRIG_STOP ||			\
				 (x) == PCMTRIG_ABORT)

typedef struct mixer_t
    {
    DEV_HDR             devHdr;
    SEM_ID              mutex;
    LIST                fdList;
    VXB_DEVICE_ID       pDev;
    int                 num_chan;
    SND_MIXER *         mixer;
    } MIXER_DEV;

typedef struct dsp_t
    {
    DEV_HDR             devHdr;
    SEM_ID              mutex;
    LIST                fdList;
    VXB_DEVICE_ID       pDev;
    int                 num_chan;
    PCM_CHANNEL *       channel;
    SEL_WAKEUP_LIST     selWakeupList;	/* list of tasks pended in select */
    } DSP_DEV;

typedef struct dsp_fd
    {
    NODE                fdNode;
    DSP_DEV *           pDspDev;
    PCM_CHANNEL *       play;
    PCM_CHANNEL *       record;
    UINT32              flags; /* flags and options */
    } DSP_FD;

typedef struct mixer_fd
    {
    NODE                fdNode;
    MIXER_DEV *         pMixerDev;
    UINT32              flags; /* flags and options */
    } MIXER_FD;


METHOD_DECL(pcm_channel_init);
METHOD_DECL(pcm_channel_setformat);
METHOD_DECL(pcm_channel_setspeed);
METHOD_DECL(pcm_channel_setfragments);
METHOD_DECL(pcm_channel_stop);
METHOD_DECL(pcm_channel_trigger);
METHOD_DECL(pcm_channel_getptr);
METHOD_DECL(pcm_channel_getcaps);

METHOD_DECL(mixer_init);
METHOD_DECL(mixer_set);
METHOD_DECL(mixer_setrecsrc);

#define METHOD_CALL(dev, name, ...)                                \
    vxbDevMethodGet((VXB_DEVICE_ID)dev, VXB_DRIVER_METHOD(name))(__VA_ARGS__)

extern MIXER_DEV* ossCreateMixer (VXB_DEVICE_ID pDev);
extern DSP_DEV* ossCreateDsp (VXB_DEVICE_ID pDev);
extern void ossDeleteMixer (MIXER_DEV*);
extern void ossDeleteDsp (DSP_DEV* pDspDev);

extern STATUS osschannel_init (VXB_DEVICE_ID pDev, DSP_DEV * pDspDev, int dir, void * devinfo);
extern void osschannel_intr (PCM_CHANNEL* pChan);
extern STATUS ossmixer_init (VXB_DEVICE_ID pDev, MIXER_DEV * pMixerDev, void *devinfo);
extern int ossmixer_delete (struct snd_mixer *m);
extern int ossmixer_setrecsrc (SND_MIXER *m, unsigned int src);
extern int ossmixer_set (SND_MIXER *m, unsigned int dev, unsigned int level);
extern int ossmixer_get (SND_MIXER *m, unsigned int dev);

extern STATUS sndbuf_alloc (SND_BUF *b, VXB_DMA_TAG_ID dmatag, int dmaflags, unsigned int size);
extern STATUS sndbuf_resize(SND_BUF *b, unsigned int blkcnt, unsigned int blksz);
extern SND_BUF* sndbuf_create(VXB_DEVICE_ID dev, struct pcm_channel *channel);
extern void sndbuf_destroy(SND_BUF *b);
extern size_t sndbuf_copy (const char *source, SND_BUF * b, size_t nbytes);
extern size_t sndbuf_read (char *dest, SND_BUF * b, size_t nbytes);
extern void sndbuf_reset(SND_BUF *b);

#define sndbuf_getsize(b) (b->bufsize)
#define sndbuf_getblksz(b) (b->blksz)
#define sndbuf_getblkcnt(b) (b->blkcnt)
#define sndbuf_getmaxsize(b) (b->maxsize)
#define sndbuf_getalign(b) (32)
#define sndbuf_getbufaddr(b) ((bus_addr_t)(VIRT_ADDR)b->buf_addr)

static __inline__ int ossmix_setdevs(SND_MIXER *m, unsigned int v)
     {
     m->devs = v;
     return 0;
     }
#if 0
static __inline__ int ossmix_setrealdev(SND_MIXER *m, unsigned int dev, unsigned int realdev)
    {
    m->realdev[dev] = realdev;
    return 0;
    }
#endif
static __inline__ int ossmix_setrecsrc(SND_MIXER *m, unsigned int src)
    {
    return ossmixer_setrecsrc (m, src);
    }

static __inline__ int ossmix_setrecdevs(SND_MIXER *m, unsigned int v)
    {
    m->recdevs = v;
    return 0;
    }

static __inline__ int ossmix_getdevs(SND_MIXER *m)
    {
    return m->devs;
    }

static __inline__ int ossmix_getrecsrc(SND_MIXER *m)
    {
    return m->recsrc;
    }

static __inline__ int ossmix_getrecdevs(SND_MIXER *m)
    {
    return m->recdevs;
    }

static __inline__ void* ossmix_getdevinfo(SND_MIXER *m)
    {
    return (m->pdevinfo);
    }

static __inline__ int ossmix_get(SND_MIXER *m, unsigned int dev)
    {
    return ossmixer_get (m, dev);
    }

static __inline__ int ossmix_set(SND_MIXER *m, unsigned int dev, unsigned int left, unsigned int right)
    {
	if ((dev < SOUND_MIXER_NRDEVICES) && (m->devs & (1 << dev)))
        {
        unsigned int level;
        level = ((right & 0xff) << 8);
        level |= (left & 0xff);
        return ossmixer_set (m, dev, level);
        }
	else
		return -1;
    }

#endif /* __INCossAudioh */

