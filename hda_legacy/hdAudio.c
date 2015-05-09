/* hdAudio.c - HD Audio Driver */

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
  01a,20jan12,gkw  written
*/

/*
  DESCRIPTION

  This is the HD Audio driver which implements the
  functionality specified in the High Definition Audio Specification.

  The HD Audio controller provides an interface between a host CPU
  and one or more resident audio codecs.

  \tb "Intel ICH manual"
  \tb "High Definition Audio Specification"
*/

/* includes */

#include <vxWorks.h>
#include <stdio.h>
#include <semLib.h>
#include <sysLib.h>
#include <taskLib.h>
#include <cacheLib.h>
#include <rebootLib.h>
#include <ioLib.h>
#include <iosLib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <ffsLib.h>
#include <iv.h>

#include <drv/pci/pciConfigLib.h>
#include <drv/pci/pciIntLib.h>

#include "audio/ossAudio.h"
#include "audio/hdAudio.h"
#include "audio/hdWidget.h"
#include "audio/dmaBufLib.h"

#define HDA_DBG_ON
#undef  HDA_DBG_ON

#ifdef  HDA_DBG_ON

#include <logLib.h>

HDCODEC_ID global_codec;
HDA_DRV_CTRL* global_controller;
PCM_DEVINFO * global_pcmdev;
BOOL    global_poll = FALSE;
#ifdef  LOCAL
#undef  LOCAL
#define LOCAL
#endif

#define HDA_DBG_IRQ            0x00000001
#define HDA_DBG_RW             0x00000002
#define HDA_DBG_ERR            0x00000008
#define HDA_DBG_INFO           0x00000010
#define HDA_DBG_ALL            0xffffffff
#define HDA_DBG_OFF            0x00000000

LOCAL UINT32 hdaDbgMask = HDA_DBG_ALL;

void HDA_DBG(unsigned int mask, const char *string, ...)
    {
    va_list va;
    va_start (va, string);
    if ((hdaDbgMask & mask) || (mask == HDA_DBG_ALL))
        vprintf(string, va);
    va_end(va);
    }

#else

#define HDA_DBG(mask, string, ...)

#endif /* HDA_DBG_ON */

#define HDA_PCI_VENDOR_ID   0x8086
#define HDA_PCI_DEV_ID      0x1c20

#define INT_NUM_GET(irq)	(sysInumTbl[(int)irq])

/* global */

HDA_DRV_CTRL * gHdaDrvCtrl = NULL;

/* forward declarations */

void hdAudioInstInit (void);
LOCAL void hdAudioInstConnect (void);

LOCAL void hdAudioDevInit (void);
LOCAL void hdAudioIsr (HDA_DRV_CTRL *);
LOCAL void hdAudioMonTask (MSG_Q_ID msgQ);

LOCAL UINT32 hdAudioFormVerb (cad_t cad, nid_t nid, UINT32 cmd, UINT32 payload);
LOCAL UINT32 hdAudioCommand(HDA_DRV_CTRL* pDrvCtrl, cad_t cad, UINT32 verb);
LOCAL void hdAudioWidgetDiscovery(HDA_DRV_CTRL* pDrvCtrl, cad_t cad);
LOCAL WIDGET* hdAudioWidgetFind (HDCODEC_ID codec, cad_t cad, nid_t nid);
LOCAL WIDGET* hdAudioWidgetNum (HDCODEC_ID codec, int num);

void hdAudioSetPinCtrl (HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int val);

/* locals */

LOCAL void audio_parse(HDCODEC_ID codec);

LOCAL void stream_stop(int dir, int stream);
LOCAL int stream_start(int dir, int stream, bus_addr_t buf, int blksz, int blkcnt);

LOCAL void hdacc_unsol_intr(HDCODEC_ID codec, UINT32 resp);

LOCAL void stream_intr(HDCODEC_ID codec, int dir, int stream);
LOCAL void dmapos_init(HDA_DRV_CTRL *sc);

void channel_stop(PCM_CHANNEL* chan);
int channel_start(PCM_CHANNEL* chan);
void * channel_init(void *data, struct snd_buf *b, PCM_CHANNEL *chan, int dir);
UINT32 channel_setspeed(PCM_CHANNEL* chan, UINT32 speed);
int channel_setformat(PCM_CHANNEL *chan, UINT32 format);
int channel_setfragments(PCM_CHANNEL* chan, UINT32 blksz, UINT32 blkcnt);
int channel_trigger(PCM_CHANNEL* chan, int go);
UINT32 channel_getptr(PCM_CHANNEL* chan);
PCMCHAN_CAPS * channel_getcaps(PCM_CHANNEL* chan);

int audio_ctl_ossmixer_set(SND_MIXER *m, unsigned dev, unsigned left, unsigned right);
UINT32 audio_ctl_ossmixer_setrecsrc(SND_MIXER *m, UINT32 src);
int audio_ctl_ossmixer_init(SND_MIXER *m);
LOCAL void audio_ctl_dev_volume(PCM_DEVINFO *pdevinfo, unsigned dev);

LOCAL void powerup(HDCODEC_ID codec);

LOCAL void corb_init(HDA_DRV_CTRL *);
LOCAL void rirb_init(HDA_DRV_CTRL *);
LOCAL void corb_start(HDA_DRV_CTRL *);
LOCAL void rirb_start(HDA_DRV_CTRL *);
LOCAL void corb_stop(HDA_DRV_CTRL *);
LOCAL void rirb_stop(HDA_DRV_CTRL *);
LOCAL int  get_capabilities(HDA_DRV_CTRL *sc);
LOCAL int  dma_alloc(HDA_DRV_CTRL *, DMA_OBJECT *, bus_size_t);
LOCAL int  reset(HDA_DRV_CTRL *, BOOL);
LOCAL void dma_free(HDA_DRV_CTRL *sc, DMA_OBJECT *dma);
LOCAL int  unsolq_flush(HDA_DRV_CTRL *sc);
LOCAL int  rirb_flush(HDA_DRV_CTRL *sc);
LOCAL UINT32 send_command(HDA_DRV_CTRL *, int cad, UINT32);


LOCAL int audio_ctl_dest_amp(HDCODEC_ID codec, nid_t nid, int index, int ossdev, int depth, int *minamp, int *maxamp);
LOCAL int audio_ctl_source_amp(HDCODEC_ID codec, nid_t nid, int index, int ossdev, int ctlable, int depth, int *minamp, int *maxamp);
LOCAL void presence_handler(WIDGET *w);

LOCAL WIDGET* widget_get (HDCODEC* codec, nid_t nid);
LOCAL UINT32 hda_command (HDCODEC* codec, UINT32 verb);
LOCAL void widget_parse(WIDGET *w);
LOCAL UINT32 audio_ctl_recsel_comm(PCM_DEVINFO *pdevinfo, UINT32 src, nid_t nid, int depth);
LOCAL AUDIO_CTL* audio_ctl_each(HDCODEC_ID codec, int *index);

LOCAL void audio_as_parse(HDCODEC_ID codec);
LOCAL void audio_build_tree(HDCODEC_ID codec);
LOCAL void create_pcms(HDCODEC_ID codec);
LOCAL void delete_pcms(HDCODEC_ID codec);
LOCAL void audio_ctl_parse (HDCODEC_ID codec);
LOCAL void audio_disable_nonaudio(HDCODEC_ID codec);
LOCAL void audio_disable_useless(HDCODEC_ID codec);
LOCAL void audio_disable_unas(HDCODEC_ID codec);
LOCAL void audio_disable_notselected(HDCODEC_ID codec);
LOCAL void audio_disable_crossas(HDCODEC_ID codec);
LOCAL void audio_bind_as(HDCODEC_ID codec);
LOCAL void audio_assign_names(HDCODEC_ID codec);
LOCAL void prepare_pcms(HDCODEC_ID codec);
LOCAL void audio_assign_mixers(HDCODEC_ID codec);
LOCAL void audio_prepare_pin_ctrl(HDCODEC_ID codec);
LOCAL void audio_commit(HDCODEC_ID codec);
LOCAL void sense_init(HDCODEC_ID codec);
LOCAL void create_pcms(HDCODEC_ID codec);
LOCAL void audio_ctl_set_defaults(PCM_DEVINFO *pcm_dev_table);


LOCAL UINT32 hdAudioCommand (HDA_DRV_CTRL* pDrvCtrl, cad_t cad, UINT32 verb);
LOCAL WIDGET* hdAudioGetRootWidget (HDA_DRV_CTRL* pDrvCtrl, cad_t cad);
LOCAL WIDGET* hdAudioWidgetCreate (HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid);

LOCAL nid_t audio_trace_dac (HDCODEC_ID codec, int, int, int, int, int, int, int);
LOCAL int audio_trace_as_out(HDCODEC_ID codec, int as, int seq);
LOCAL void audio_undo_trace(HDCODEC_ID codec, int as, int seq);

LOCAL STATUS hdAudioUnlink (void * unused);

IMPORT void sysUsDelay(int uSec);
#define hdaUsDelay sysUsDelay

#define HDA_DMA_ALIGNMENT       128
#define HDA_BDL_MIN             2
#define HDA_BDL_MAX             256
#define HDA_BDL_DEFAULT         HDA_BDL_MIN
#define HDA_BLK_MIN             HDA_DMA_ALIGNMENT
#define HDA_BLK_ALIGN           (~(HDA_BLK_MIN - 1))
#define HDA_BUFSZ_MIN           (HDA_BDL_MIN * HDA_BLK_MIN)
#define HDA_BUFSZ_MAX           262144
#define HDA_BUFSZ_DEFAULT       65536
#define HDA_GPIO_MAX            8


#define HDA_WRITE_4(offset, value) \
        ((*(volatile UINT32 *)(pDrvCtrl->regBase + (offset))) = \
        (UINT32) (value))

#define HDA_READ_4(offset, result)                                \
    do  {                                                  \
        result = *(volatile UINT32 *)(pDrvCtrl->regBase + offset); \
        } while (FALSE)

#define HDA_WRITE_2(offset, value) \
        ((*(volatile UINT16 *)(pDrvCtrl->regBase + (offset))) = \
        (UINT16) (value))

#define HDA_READ_2(offset, result)                                \
    do  {                                                  \
        result = *(volatile UINT16 *)(pDrvCtrl->regBase + offset); \
        } while (FALSE)

#define HDA_WRITE_1(offset, value) \
        ((*(volatile UINT8 *)(pDrvCtrl->regBase + (offset))) = \
        (UINT8) (value))

#define HDA_READ_1(offset, result)                                \
    do  {                                                  \
        result = *(volatile UINT8 *)(pDrvCtrl->regBase + offset); \
        } while (FALSE)

#define ISDCTL(sc, n)   (_HDAC_ISDCTL((n),  (sc)->num_iss, (sc)->num_oss))
#define ISDSTS(sc, n)   (_HDAC_ISDSTS((n),  (sc)->num_iss, (sc)->num_oss))
#define ISDPICB(sc, n)  (_HDAC_ISDPICB((n), (sc)->num_iss, (sc)->num_oss))
#define ISDCBL(sc, n)   (_HDAC_ISDCBL((n),  (sc)->num_iss, (sc)->num_oss))
#define ISDLVI(sc, n)   (_HDAC_ISDLVI((n),  (sc)->num_iss, (sc)->num_oss))
#define ISDFIFOD(sc, n) (_HDAC_ISDFIFOD((n),(sc)->num_iss, (sc)->num_oss))
#define ISDFMT(sc, n)   (_HDAC_ISDFMT((n),  (sc)->num_iss, (sc)->num_oss))
#define ISDBDPL(sc, n)  (_HDAC_ISDBDPL((n), (sc)->num_iss, (sc)->num_oss))
#define ISDBDPU(sc, n)  (_HDAC_ISDBDPU((n), (sc)->num_iss, (sc)->num_oss))

#define OSDCTL(sc, n)   (_HDAC_OSDCTL((n),  (sc)->num_iss, (sc)->num_oss))
#define OSDSTS(sc, n)   (_HDAC_OSDSTS((n),  (sc)->num_iss, (sc)->num_oss))
#define OSDPICB(sc, n)  (_HDAC_OSDPICB((n), (sc)->num_iss, (sc)->num_oss))
#define OSDCBL(sc, n)   (_HDAC_OSDCBL((n),  (sc)->num_iss, (sc)->num_oss))
#define OSDLVI(sc, n)   (_HDAC_OSDLVI((n),  (sc)->num_iss, (sc)->num_oss))
#define OSDFIFOD(sc, n) (_HDAC_OSDFIFOD((n),(sc)->num_iss, (sc)->num_oss))
#define OSDBDPL(sc, n)  (_HDAC_OSDBDPL((n), (sc)->num_iss, (sc)->num_oss))
#define OSDBDPU(sc, n)  (_HDAC_OSDBDPU((n), (sc)->num_iss, (sc)->num_oss))

#define BSDCTL(sc, n)   (_HDAC_BSDCTL((n),  (sc)->num_iss, (sc)->num_oss))
#define BSDSTS(sc, n)   (_HDAC_BSDSTS((n),  (sc)->num_iss, (sc)->num_oss))
#define BSDPICB(sc, n)  (_HDAC_BSDPICB((n), (sc)->num_iss, (sc)->num_oss))
#define BSDCBL(sc, n)   (_HDAC_BSDCBL((n),  (sc)->num_iss, (sc)->num_oss))
#define BSDLVI(sc, n)   (_HDAC_BSDLVI((n),  (sc)->num_iss, (sc)->num_oss))
#define BSDFIFOD(sc, n) (_HDAC_BSDFIFOD((n),(sc)->num_iss, (sc)->num_oss))
#define BSDBDPL(sc, n)  (_HDAC_BSDBDPL((n), (sc)->num_iss, (sc)->num_oss))
#define BSDBDPU(sc, n)  (_HDAC_BSDBDPU((n), (sc)->num_iss, (sc)->num_oss))


#define RIRB_RESPONSE_EX_SDATA_IN_MASK 0x0000000f
#define RIRB_RESPONSE_EX_SDATA_IN_OFFSET   0
#define RIRB_RESPONSE_EX_UNSOLICITED   0x00000010

#define RIRB_RESPONSE_EX_SDATA_IN(response_ex)                     \
    (((response_ex) & RIRB_RESPONSE_EX_SDATA_IN_MASK) >>           \
     RIRB_RESPONSE_EX_SDATA_IN_OFFSET)

#define HDA_RATE_TAB_LEN (sizeof(hda_rate_tab) / sizeof(hda_rate_tab[0]))

/* Stuff from FreeBSD Driver */

#define CTL_OUT    1
#define CTL_IN     2

#define HDA_MAX_CONNS   32
#define HDA_MAX_NAMELEN 32

#define CHN_RUNNING    0x00000001
#define CHN_SUSPEND    0x00000002

/* Default */

#define AFMT_ENCODING_MASK      0xf00fffff
#define AFMT_CHANNEL_MASK       0x01f00000
#define AFMT_CHANNEL_SHIFT      20
#define AFMT_EXTCHANNEL_MASK    0x0e000000
#define AFMT_EXTCHANNEL_SHIFT   25
#define AFMT_ENCODING(v)        ((v) & AFMT_ENCODING_MASK)

#define AFMT_EXTCHANNEL(v)      (((v) & AFMT_EXTCHANNEL_MASK) >>        \
                                 AFMT_EXTCHANNEL_SHIFT)

#define SND_FORMAT(f, c, e)     (AFMT_ENCODING(f) |                     \
                                 (((c) << AFMT_CHANNEL_SHIFT) &         \
                                  AFMT_CHANNEL_MASK) |                  \
                                 (((e) << AFMT_EXTCHANNEL_SHIFT) &      \
                                  AFMT_EXTCHANNEL_MASK))


#define AFMT_CHANNEL(v)         (((v) & AFMT_CHANNEL_MASK) >>           \
                                 AFMT_CHANNEL_SHIFT)

/* defines */
#define AUTO_SELECT_RECORD_SOURCE_DISABLE       0
#define AUTO_SELECT_RECORD_SOURCE_ONCE          1
#define AUTO_SELECT_RECORD_SOURCE_ENABLED       2

typedef struct rirb_t
    {
    UINT32    response;
    UINT32    response_ex;
    } RIRB;

typedef struct bdle_t
    {
    volatile UINT32 addrl;
    volatile UINT32 addrh;
    volatile UINT32 len;
    volatile UINT32 ioc;
    } BDLE;

LOCAL UINT32 oss_fmt_list[] = {
    SND_FORMAT(AFMT_S16_LE, 2, 0),
    0
};

LOCAL struct pcmchan_caps caps = {48000, 48000, oss_fmt_list,
                                       DSP_CAP_DUPLEX | DSP_CAP_REALTIME | DSP_CAP_TRIGGER};

LOCAL const struct {
UINT32    rate;
int     valid;
UINT16    base;
UINT16    mul;
UINT16    div;
    } hda_rate_tab[] = {
    {   8000, 1, 0x0000, 0x0000, 0x0500 },  /* (48000 * 1) / 6 */
    {   9600, 0, 0x0000, 0x0000, 0x0400 },  /* (48000 * 1) / 5 */
    {  12000, 0, 0x0000, 0x0000, 0x0300 },  /* (48000 * 1) / 4 */
    {  16000, 1, 0x0000, 0x0000, 0x0200 },  /* (48000 * 1) / 3 */
    {  18000, 0, 0x0000, 0x1000, 0x0700 },  /* (48000 * 3) / 8 */
    {  19200, 0, 0x0000, 0x0800, 0x0400 },  /* (48000 * 2) / 5 */
    {  24000, 0, 0x0000, 0x0000, 0x0100 },  /* (48000 * 1) / 2 */
    {  28800, 0, 0x0000, 0x1000, 0x0400 },  /* (48000 * 3) / 5 */
    {  32000, 1, 0x0000, 0x0800, 0x0200 },  /* (48000 * 2) / 3 */
    {  36000, 0, 0x0000, 0x1000, 0x0300 },  /* (48000 * 3) / 4 */
    {  38400, 0, 0x0000, 0x1800, 0x0400 },  /* (48000 * 4) / 5 */
    {  48000, 1, 0x0000, 0x0000, 0x0000 },  /* (48000 * 1) / 1 */
    {  64000, 0, 0x0000, 0x1800, 0x0200 },  /* (48000 * 4) / 3 */
    {  72000, 0, 0x0000, 0x1000, 0x0100 },  /* (48000 * 3) / 2 */
    {  96000, 1, 0x0000, 0x0800, 0x0000 },  /* (48000 * 2) / 1 */
    { 144000, 0, 0x0000, 0x1000, 0x0000 },  /* (48000 * 3) / 1 */
    { 192000, 1, 0x0000, 0x1800, 0x0000 },  /* (48000 * 4) / 1 */
    {   8820, 0, 0x4000, 0x0000, 0x0400 },  /* (44100 * 1) / 5 */
    {  11025, 1, 0x4000, 0x0000, 0x0300 },  /* (44100 * 1) / 4 */
    {  12600, 0, 0x4000, 0x0800, 0x0600 },  /* (44100 * 2) / 7 */
    {  14700, 0, 0x4000, 0x0000, 0x0200 },  /* (44100 * 1) / 3 */
    {  17640, 0, 0x4000, 0x0800, 0x0400 },  /* (44100 * 2) / 5 */
    {  18900, 0, 0x4000, 0x1000, 0x0600 },  /* (44100 * 3) / 7 */
    {  22050, 1, 0x4000, 0x0000, 0x0100 },  /* (44100 * 1) / 2 */
    {  25200, 0, 0x4000, 0x1800, 0x0600 },  /* (44100 * 4) / 7 */
    {  26460, 0, 0x4000, 0x1000, 0x0400 },  /* (44100 * 3) / 5 */
    {  29400, 0, 0x4000, 0x0800, 0x0200 },  /* (44100 * 2) / 3 */
    {  33075, 0, 0x4000, 0x1000, 0x0300 },  /* (44100 * 3) / 4 */
    {  35280, 0, 0x4000, 0x1800, 0x0400 },  /* (44100 * 4) / 5 */
    {  44100, 1, 0x4000, 0x0000, 0x0000 },  /* (44100 * 1) / 1 */
    {  58800, 0, 0x4000, 0x1800, 0x0200 },  /* (44100 * 4) / 3 */
    {  66150, 0, 0x4000, 0x1000, 0x0100 },  /* (44100 * 3) / 2 */
    {  88200, 1, 0x4000, 0x0800, 0x0000 },  /* (44100 * 2) / 1 */
    { 132300, 0, 0x4000, 0x1000, 0x0000 },  /* (44100 * 3) / 1 */
    { 176400, 1, 0x4000, 0x1800, 0x0000 },  /* (44100 * 4) / 1 */
};


/* externs */

IMPORT UINT8	*sysInumTbl;			/* IRQ vs intNum table */
IMPORT STATUS sysIntEnablePIC(int irqNo);
IMPORT STATUS sysIntDisablePIC(int irqNo);

/*******************************************************************************
 *
 * hdAudioInstInit - first initialization routine of HD Audio device
 *
 * This routine performs the first initialization of the HDA device.
 *
 * RETURNS: N/A
 *
 * ERRNO: N/A
 */

void hdAudioInstInit(void)
    {
    HDA_DRV_CTRL * pDrvCtrl;
    int stat, i;
    UINT16 temp16;
    int   busNo;          /* PCI bus number              */
    int   devNo;          /* PCI device number           */
    int   funcNo;         /* PCI function number         */
    int   index = 0;      /* desired instance of device  */
    UINT32 hdaRegBase;
    UINT8  hdaIntLvl;
    
    pDrvCtrl = (HDA_DRV_CTRL *)calloc (1, sizeof(HDA_DRV_CTRL));
    if (pDrvCtrl == NULL)
        return;

    gHdaDrvCtrl = pDrvCtrl;
    bzero ((char *)pDrvCtrl, sizeof(HDA_DRV_CTRL));
    
#ifdef  HDA_DBG_ON
    global_controller = pDrvCtrl;
#endif

    if ((pciFindDevice (HDA_PCI_VENDOR_ID, HDA_PCI_DEV_ID,
			index, &busNo, &devNo, &funcNo)) != OK)
        {
        logMsg("HDA controller not found!\n", 1,2,3,4,5,6); 
        return;
        }

    pciConfigInLong(busNo, devNo, funcNo, PCI_CFG_BASE_ADDRESS_0, 
                    &hdaRegBase);
    hdaRegBase &= PCI_MEMBASE_MASK;
    pDrvCtrl->regBase = (void *)hdaRegBase;

#ifdef  HDA_DBG_ON
    HDA_DBG(HDA_DBG_ERR, "regBase= 0x%8.8x\n", pDrvCtrl->regBase);
#endif

    pciConfigInByte(busNo, devNo, funcNo, PCI_CFG_DEV_INT_LINE, 
                    &hdaIntLvl);
    pDrvCtrl->intLvl = hdaIntLvl;
    pDrvCtrl->intVector = (VOIDFUNCPTR *)(INUM_TO_IVEC (INT_NUM_GET (pDrvCtrl->intLvl)));

#ifdef  HDA_DBG_ON
    HDA_DBG(HDA_DBG_ERR, "interrupt line = 0x%x\n", pDrvCtrl->intLvl);
    HDA_DBG(HDA_DBG_ERR, "interrupt vector = 0x%x\n", (int)pDrvCtrl->intVector);
#endif

    /* Clear bit: No Snoop Enable(NSNPEN) (offset: 78h) */

#   define PCI_CFG_DEVC 0x78

    pciConfigInWord(busNo, devNo, funcNo, PCI_CFG_DEVC, &temp16);
    temp16 &= ~0x0800;
    pciConfigOutWord(busNo, devNo, funcNo, PCI_CFG_DEVC, temp16);

    /* Get HD controller capabilities */
    stat = get_capabilities(pDrvCtrl);

    pDrvCtrl->parentTag = dmaBufTagParentGet(0);
#ifdef  HDA_DBG_ON
    HDA_DBG(HDA_DBG_INFO, "parentTag= 0x%8.8x\n", pDrvCtrl->parentTag);
#endif

    /* Allocate CORB, RIRB, POS and BDLs dma memory */
    stat = dma_alloc(pDrvCtrl, &pDrvCtrl->corb_dma,
                          pDrvCtrl->corb_size * sizeof(UINT32));
    
    stat = dma_alloc(pDrvCtrl, &pDrvCtrl->rirb_dma, pDrvCtrl->rirb_size * sizeof(RIRB));
    
    pDrvCtrl->streams = calloc(pDrvCtrl->num_ss, sizeof(STREAM));
    for (i = 0; i < pDrvCtrl->num_ss; i++)
        {
        stat = dma_alloc(pDrvCtrl, &pDrvCtrl->streams[i].bdl,
                              sizeof(BDLE) * HDA_BDL_MAX);
        }

    if (dma_alloc(pDrvCtrl, &pDrvCtrl->pos_dma, sizeof(BDLE) * 0x16 * (pDrvCtrl->num_ss) * 8) != 0)
        {
        HDA_DBG(HDA_DBG_ERR, "Failed to "
                "allocate DMA pos buffer "
                "(non-fatal)\n");
        }
    else
        {
        uint64_t addr = pDrvCtrl->pos_dma.dma_paddr;
        bzero((void*)(UINT32)addr, pDrvCtrl->num_ss * 8);
        HDA_DBG(HDA_DBG_ERR, "dma_pos_addr = %8.8x\n",addr);
        }

    pDrvCtrl->sndbuf_dma_tag = dmaBufTagCreate(pDrvCtrl->parentTag,
                       HDA_DMA_ALIGNMENT,
                       0, 
                       (pDrvCtrl->support_64bit) ? BUS_SPACE_MAXADDR : BUS_SPACE_MAXADDR_32BIT,
                       BUS_SPACE_MAXADDR,
                       NULL,
                       NULL,
                       HDA_BUFSZ_DEFAULT,
                       HDA_BDL_MIN,
                       HDA_BUFSZ_DEFAULT,
                       DMABUF_ALLOCNOW|DMABUF_NOCACHE,
                       NULL,
                       NULL,
                       NULL);

    reset(pDrvCtrl, TRUE);
    corb_stop(pDrvCtrl);
    rirb_stop(pDrvCtrl);

    /* Initialize the CORB and RIRB */
    corb_init(pDrvCtrl);
    rirb_init(pDrvCtrl);

    dmapos_init(pDrvCtrl);

    /* second level initialization */
    hdAudioInstConnect();
    
    return;
    }

/*******************************************************************************
 *
 * hdAudioInstConnect - second level initialization routine of HDA controller
 *
 * This routine performs the second level initialization of the HDA controller.
 *
 * RETURNS: N/A
 *
 * ERRNO: N/A
 */

 LOCAL void hdAudioInstConnect (void)
    {
    HDA_DRV_CTRL * pDrvCtrl = device_get_softc();

    /*
     * The device semaphore is used for mutual exclusion
     */

    pDrvCtrl->mutex = semMCreate (SEM_Q_PRIORITY|SEM_DELETE_SAFE|SEM_INVERSION_SAFE);
    if (pDrvCtrl->mutex == NULL)
        {
        HDA_DBG (HDA_DBG_ERR, "semMCreate failed for mutex\n",
                 0, 0, 0, 0, 0, 0);
        return;
        }

    pDrvCtrl->unsolq_msgQ = msgQCreate(10, sizeof(HDA_MSG), MSG_Q_FIFO);

    /* create a monitor task that handles state change and msgs */


    taskSpawn (HDA_MON_TASK_NAME, HDA_MON_TASK_PRI, 0,
               HDA_MON_TASK_STACK, (void*)hdAudioMonTask, (int)pDrvCtrl->unsolq_msgQ,
               0, 0, 0, 0, 0, 0, 0, 0, 0);
    
    pciIntConnect (pDrvCtrl->intVector, hdAudioIsr, (int)pDrvCtrl);
    sysIntEnablePIC (pDrvCtrl->intLvl);
    
    /* per-device init */

    hdAudioDevInit ();

    return;
    }

/*******************************************************************************
 *
 * hdAudioDevInit - HDA per device specific initialization
 *
 * This routine performs per device specific initialization of the HD Audio 
 * controller.
 *
 * RETURNS: N/A
 *
 * ERRNO: N/A
 */

LOCAL void hdAudioDevInit (void)
    {
    int cad;
    UINT8 corbsts, rirbsts,rirbctl,corbctl;
    UINT16 corbrp, corbwp,rintcnt,rirbwp;
    UINT16 statests;
    UINT32 gctl, intcl;
    UINT32 vendorid, revisionid;
    HDA_DRV_CTRL * pDrvCtrl = device_get_softc();
    UINT32 tmp32;

    HDA_DBG(HDA_DBG_INFO, "Starting CORB Engine...\n");

    HDA_READ_2(HDAC_CORBWP, corbwp);
    HDA_READ_2(HDAC_CORBRP, corbrp);
    HDA_READ_2(HDAC_RINTCNT, rintcnt);
    HDA_READ_2(HDAC_RIRBWP, rirbwp);
    HDA_READ_1(HDAC_CORBSTS, corbsts);
    HDA_READ_1(HDAC_RIRBSTS, rirbsts);
    HDA_READ_1(HDAC_CORBCTL, corbctl);
    HDA_READ_1(HDAC_RIRBCTL, rirbctl);

    corb_start(pDrvCtrl);
    HDA_DBG(HDA_DBG_INFO, "Starting RIRB Engine...\n");

    rirb_start(pDrvCtrl);
    HDA_DBG(HDA_DBG_INFO, "Enabling controller interrupt...\n");

    HDA_READ_4(HDAC_GCTL, tmp32);
    HDA_WRITE_4(HDAC_GCTL, tmp32 | HDAC_GCTL_UNSOL);

    HDA_WRITE_4(HDAC_INTCTL, HDAC_INTCTL_CIE | HDAC_INTCTL_GIE);
    
    hdaUsDelay(100);

    HDA_READ_4(HDAC_GCTL, gctl);
    HDA_READ_4(HDAC_INTCTL, intcl);

    HDA_DBG(HDA_DBG_INFO, "Scanning HDA codecs ...\n");
    
    HDA_READ_2(HDAC_WAKEEN, statests);
    HDA_READ_2(HDAC_STATESTS, statests);

    for (cad = 0; cad < HDAC_CODEC_NUM_MAX; cad++)
        {
        if (HDAC_STATESTS_SDIWAKE(statests, cad))
            pDrvCtrl->num_codec++;
        }

    for (cad = 0; cad < HDAC_CODEC_NUM_MAX; cad++)
        {
        UINT32 verb;
        HDCODEC_ID codec;

        if (HDAC_STATESTS_SDIWAKE(statests, cad))
            {

            pDrvCtrl->codec_table[cad] = calloc(1, sizeof(HDCODEC));

            codec = pDrvCtrl->codec_table[cad];

#ifdef  HDA_DBG_ON
            global_codec = codec;
#endif
            verb = HDA_CMD_GET_PARAMETER(cad, 0, HDA_PARAM_VENDOR_ID);
            vendorid = hdAudioCommand(pDrvCtrl, cad, verb);

            verb = HDA_CMD_GET_PARAMETER(cad, 0, HDA_PARAM_REVISION_ID);
            revisionid = hdAudioCommand(pDrvCtrl, cad, verb);

            if (vendorid == HDA_INVALID && revisionid == HDA_INVALID)
                {
                HDA_DBG(HDA_DBG_ERR, "CODEC is not responding!\n");
                continue;
                }

            codec->vendor_id = HDA_PARAM_VENDOR_ID_VENDOR_ID(vendorid);
            codec->device_id = HDA_PARAM_VENDOR_ID_DEVICE_ID(vendorid);
            codec->revision_id = HDA_PARAM_REVISION_ID_REVISION_ID(revisionid);
            codec->stepping_id = HDA_PARAM_REVISION_ID_STEPPING_ID(revisionid);

            hdAudioWidgetDiscovery (pDrvCtrl, cad);

            powerup(codec);

            audio_parse(codec);

            audio_ctl_parse(codec);
            
            audio_disable_nonaudio(codec);

            audio_disable_useless(codec);

            audio_as_parse(codec);

            audio_build_tree(codec);

            audio_disable_unas(codec);

            audio_disable_notselected(codec);

            audio_disable_useless(codec);

            audio_disable_crossas(codec);

            audio_disable_useless(codec);

            audio_bind_as(codec);

            audio_assign_names(codec);

            prepare_pcms(codec);

            audio_assign_mixers(codec);

            audio_prepare_pin_ctrl(codec);

            audio_commit(codec);

            create_pcms(codec);

            sense_init(codec);
            }
        }
    }

/*******************************************************************************
 *
 * hdAudioIsr - interrupt service routine
 *
 * This routine handles interrupts of the HD Audio controller
 *
 * RETURNS: N/A
 *
 * ERRNO: N/A
 */

LOCAL void hdAudioIsr
    (
    HDA_DRV_CTRL * pDrvCtrl
    )
    {
    UINT8 rirbsts;
    UINT32 intsts;

    HDA_READ_4(HDAC_INTSTS, intsts);
    if (intsts & HDAC_INTSTS_GIS)
        {
        if (intsts & HDAC_INTSTS_CIS)
            {
            HDA_READ_1(HDAC_RIRBSTS, rirbsts);

            while (rirbsts & HDAC_RIRBSTS_RINTFL)
                {
                HDA_WRITE_1(HDAC_RIRBSTS, rirbsts);
                rirb_flush(pDrvCtrl);
                HDA_READ_1(HDAC_RIRBSTS, rirbsts);
                }

            if (pDrvCtrl->unsolq_rp != pDrvCtrl->unsolq_wp)
                {
                HDA_MSG msg;
                msg.type = HDA_MSG_TYPE_UNSOLQ;
                msgQSend(pDrvCtrl->unsolq_msgQ, (char*)&msg,
                         HDA_MSG_SIZE, NO_WAIT, MSG_PRI_NORMAL);
                }
            }

        if (intsts & HDAC_INTSTS_SIS_MASK)
            {
            int i;
            HDCODEC* codec;
            UINT8 tmp8;
       
            for (i = 0; i < pDrvCtrl->num_ss; i++)
                {
                if ((intsts & (1 << i)) == 0)
                    continue;

                HDA_READ_1((i << 5) + HDAC_SDSTS, tmp8);
                HDA_WRITE_1((i << 5) + HDAC_SDSTS, tmp8);

                if ((codec = pDrvCtrl->streams[i].codec) != NULL)
                    {
                    stream_intr(pDrvCtrl->streams[i].codec, pDrvCtrl->streams[i].dir, pDrvCtrl->streams[i].stream);
                    }
                }
            }
        HDA_WRITE_4(HDAC_INTSTS, intsts);
        }
    }

LOCAL void stream_intr
    (
    HDCODEC_ID codec,
    int dir,
    int stream
    )
    {
    int i;
    CHAN *ch;

    for (i = 0; i < codec->num_hdaa_chans; i++)
        {
        ch = &codec->hdaa_chan_table[i];
        if (!(ch->flags & CHN_RUNNING))
            continue;

        if (((ch->sid == stream) && (dir == 1) && (ch->dir == CTL_OUT)) ||
            ((ch->sid == stream) && (dir == 0) && (ch->dir == CTL_IN)))
            {
            osschannel_intr(ch->c);
            }
        }
    }

/*******************************************************************************
 *
 * hdAudioMonTask - status monitor task
 *
 * This routine is the task loop to handle jack insertion/removal
 *
 * RETURN: N/A
 *
 * ERRNO: N/A
 */

LOCAL void hdAudioMonTask
    (
    MSG_Q_ID    msgQ
    )
    {
    HDA_DRV_CTRL * pDrvCtrl = device_get_softc();

    FOREVER
        {
        HDA_MSG msg;
        
        if (msgQReceive (msgQ, (char*)&msg, sizeof(msg), NO_WAIT) != ERROR)
            {
            switch (msg.type)
                {
                /* delete my queue and end execution */
                case HDA_MSG_TYPE_UNLINK:
                    msgQDelete(msgQ);
                    return;
                    
                case HDA_MSG_TYPE_UNSOLQ:
                default:
                    unsolq_flush(pDrvCtrl);
                    break;
                }
            }

        taskDelay (HDA_MON_DELAY_SECS * sysClkRateGet ());
        }
    }




/**********************************************************************
 *
 * find_stream - locate index number stream based on logical number
 *
 * RETURNS: index number of stream in the streams array
 *
 * NOMANUAL
 */

LOCAL int find_stream(HDA_DRV_CTRL *pDrvCtrl, int dir, int stream)
    {
    int i, ss;
    
    if (dir == 0)
        {
        ss = 0;
        for (i = 0; i < pDrvCtrl->num_iss; i++, ss++)
            {
            if (pDrvCtrl->streams[ss].stream == stream)
                return ss;
            }
        }
    else
        {
        ss = pDrvCtrl->num_iss;
        for (i = 0; i < pDrvCtrl->num_oss; i++, ss++)
            {
            if (pDrvCtrl->streams[ss].stream == stream)
                return ss;
            }
        }

    /* fall back to bi-directional steam */
    ss = pDrvCtrl->num_iss + pDrvCtrl->num_oss;
    for (i = 0; i < pDrvCtrl->num_bss; i++, ss++)
        {
        if (pDrvCtrl->streams[ss].stream == stream)
            return ss;
        }
    
    return -1;
    }

LOCAL int stream_alloc
    (
    HDCODEC_ID codec,
    int dir,
    int format,
    int stripe,
    UINT32 **dmapos
    )
    {
    int stream, ss;
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();

    /* Look for empty stream. */
    ss = find_stream(pDrvCtrl, dir, 0);

    /* Return if found nothing. */
    if (ss < 0)
        return (0);

    /* Allocate stream number */
    if (ss >= pDrvCtrl->num_iss + pDrvCtrl->num_oss)
        stream = 15 - (ss - pDrvCtrl->num_iss + pDrvCtrl->num_oss);
    else if (ss >= pDrvCtrl->num_iss)
        stream = ss - pDrvCtrl->num_iss + 1;
    else
        stream = ss + 1;

    pDrvCtrl->streams[ss].codec = codec;
    pDrvCtrl->streams[ss].dir = dir;
    pDrvCtrl->streams[ss].stream = stream;
    pDrvCtrl->streams[ss].format = format;
    pDrvCtrl->streams[ss].stripe = stripe;

    if (dmapos != NULL)
        {
        if (pDrvCtrl->pos_dma.dma_vaddr != NULL)
            *dmapos = (UINT32 *)(pDrvCtrl->pos_dma.dma_vaddr + ss * 8);
        else
            *dmapos = NULL;
        }

    return (stream);
    }

LOCAL void stream_free
    (
    int dir,
    int stream
    )
    {
    int ss;
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();

    ss = find_stream(pDrvCtrl, dir, stream);

    pDrvCtrl->streams[ss].stream = 0;
    pDrvCtrl->streams[ss].codec = NULL;
    }

LOCAL void
    stream_stop(int dir, int stream)
    {
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    int ss, off;
    UINT32 ctl;

    ss = find_stream(pDrvCtrl, dir, stream);
    
    off = ss << 5;
    HDA_READ_1(off + HDAC_SDCTL0, ctl);
    ctl &= ~(HDAC_SDCTL_IOCE | HDAC_SDCTL_FEIE | HDAC_SDCTL_DEIE |
             HDAC_SDCTL_RUN);
    HDA_WRITE_1(off + HDAC_SDCTL0, ctl);

    HDA_READ_4(HDAC_INTCTL, ctl);
    ctl &= ~(1 << ss);
    HDA_WRITE_4(HDAC_INTCTL, ctl);

    pDrvCtrl->streams[ss].running = FALSE;
    }

LOCAL int stream_start
    (
    int dir,
    int stream,
    bus_addr_t buf,
    int blksz,
    int blkcnt
    )
    {
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    BDLE *bdle;
    uint64_t addr;
    int i, ss, off;
    UINT32 ctl;

    ss = find_stream(pDrvCtrl, dir, stream);

    addr = (uint64_t)buf;
    bdle = (BDLE *)pDrvCtrl->streams[ss].bdl.dma_vaddr;

    for (i = 0; i < blkcnt; i++, bdle++)
        {
        bdle->addrl = (UINT32)addr;
        bdle->addrh = (UINT32)(addr >> 32);
        bdle->len = blksz;
        bdle->ioc = 1;

        if ((i + 1) == blkcnt)
            bdle->ioc = 1;
        addr += blksz;
        }

    off = ss << 5;
    HDA_WRITE_4(off + HDAC_SDCBL, blksz * blkcnt);
    HDA_WRITE_2(off + HDAC_SDLVI, blkcnt - 1);
    addr = pDrvCtrl->streams[ss].bdl.dma_paddr;
    HDA_WRITE_4(off + HDAC_SDBDPL, (UINT32)addr);
    HDA_WRITE_4(off + HDAC_SDBDPU, (UINT32)(addr >> 32));

    HDA_READ_1(off + HDAC_SDCTL2, ctl);
    if (dir)
        ctl |= HDAC_SDCTL2_DIR;
    else
        ctl &= ~HDAC_SDCTL2_DIR;
    ctl &= ~HDAC_SDCTL2_STRM_MASK;
    ctl |= stream << HDAC_SDCTL2_STRM_SHIFT;
    ctl &= ~HDAC_SDCTL2_STRIPE_MASK;
    ctl |= pDrvCtrl->streams[ss].stripe << HDAC_SDCTL2_STRIPE_SHIFT;
    HDA_WRITE_1(off + HDAC_SDCTL2, ctl);
    
    HDA_WRITE_2(off + HDAC_SDFMT, pDrvCtrl->streams[ss].format);

    HDA_READ_4(HDAC_INTCTL, ctl);
    ctl |= 1 << ss;
    HDA_WRITE_4(HDAC_INTCTL, ctl);

    HDA_READ_1(off + HDAC_SDCTL0, ctl);
    ctl |= HDAC_SDCTL_IOCE | HDAC_SDCTL_FEIE | HDAC_SDCTL_DEIE |
        HDAC_SDCTL_RUN;

    HDA_WRITE_1(off + HDAC_SDCTL0, ctl);

    pDrvCtrl->streams[ss].blksz = blksz;
    pDrvCtrl->streams[ss].running = TRUE;

    return (0);
    }



LOCAL void stream_reset
    (
    int dir,
    int stream
    )
    {
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    int timeout = 1000;
    int to = timeout;
    int ss, off;
    UINT32 ctl;

    ss = find_stream(pDrvCtrl, dir, stream);

    off = ss << 5;
    HDA_READ_1(off + HDAC_SDCTL0, ctl);
    ctl |= HDAC_SDCTL_SRST;
    HDA_WRITE_1(off + HDAC_SDCTL0, ctl);
    do {
    HDA_READ_1(off + HDAC_SDCTL0, ctl);
    if (ctl & HDAC_SDCTL_SRST)
        break;
    hdaUsDelay(10);
    } while (--to);
    if (!(ctl & HDAC_SDCTL_SRST))
        {
        HDA_DBG(HDA_DBG_ERR, "Reset setting timeout\n");
        }
    ctl &= ~HDAC_SDCTL_SRST;
    HDA_WRITE_1(off + HDAC_SDCTL0, ctl);
    to = timeout;
    do {
    HDA_READ_1(off + HDAC_SDCTL0, ctl);
    if (!(ctl & HDAC_SDCTL_SRST))
        break;
    hdaUsDelay(10);
    } while (--to);
    if (ctl & HDAC_SDCTL_SRST)
        HDA_DBG(HDA_DBG_ERR, "Reset timeout!\n");
    }

LOCAL UINT32 stream_getptr
    (
    int dir,
    int stream
    )
    {
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    int ss, off;
    UINT32 retVal;

    ss = find_stream(pDrvCtrl, dir, stream);

    off = ss << 5;
    HDA_READ_4(off + HDAC_SDLPIB, retVal);
    
    return (retVal);
    }


LOCAL int rirb_flush
    (
    HDA_DRV_CTRL *pDrvCtrl
    )
    {
    RIRB *rirb_base, *rirb;
    cad_t cad;
    UINT32 resp;
    UINT8 rirbwp;
    int ret;

    rirb_base = (RIRB *)pDrvCtrl->rirb_dma.dma_vaddr;
    HDA_READ_1(HDAC_RIRBWP, rirbwp);

    dmaBufSync( pDrvCtrl->rirb_dma.dma_tag,
                   pDrvCtrl->rirb_dma.dma_map, DMABUFSYNC_POSTREAD );

    ret = 0;

    while (pDrvCtrl->rirb_rp != rirbwp)
        {
        pDrvCtrl->rirb_rp++;
        pDrvCtrl->rirb_rp %= pDrvCtrl->rirb_size;
        rirb = &rirb_base[pDrvCtrl->rirb_rp];
        cad = RIRB_RESPONSE_EX_SDATA_IN(rirb->response_ex);
        resp = rirb->response;

        if ((pDrvCtrl->codec_table[cad] != NULL) && (pDrvCtrl->codec_table[cad]->pending > 0))
            {
            pDrvCtrl->codec_table[cad]->response = resp;
            pDrvCtrl->codec_table[cad]->pending--;
            }
        else if (rirb->response_ex & RIRB_RESPONSE_EX_UNSOLICITED)
            {
            pDrvCtrl->unsolq[pDrvCtrl->unsolq_wp++] = resp;
            pDrvCtrl->unsolq_wp %= HDAC_UNSOLQ_MAX;
            pDrvCtrl->unsolq[pDrvCtrl->unsolq_wp++] = cad;
            pDrvCtrl->unsolq_wp %= HDAC_UNSOLQ_MAX;
            }
        else
            {
            HDA_DBG(HDA_DBG_ERR, "Unexpected unsolicited response from address %d: %x %08x\n", cad, rirb->response_ex, resp);
            }
        
        ret++;
        }

    return (ret);
    }

LOCAL int unsolq_flush
    (
    HDA_DRV_CTRL *pDrvCtrl
    )
    {
    cad_t cad;
    UINT32 resp;
    int ret = 0;

    if (pDrvCtrl->unsolq_st == HDAC_UNSOLQ_READY)
        {
        pDrvCtrl->unsolq_st = HDAC_UNSOLQ_BUSY;

        while (pDrvCtrl->unsolq_rp != pDrvCtrl->unsolq_wp)
            {
            resp = pDrvCtrl->unsolq[pDrvCtrl->unsolq_rp++];
            pDrvCtrl->unsolq_rp %= HDAC_UNSOLQ_MAX;

            cad = pDrvCtrl->unsolq[pDrvCtrl->unsolq_rp++];
            pDrvCtrl->unsolq_rp %= HDAC_UNSOLQ_MAX;

            if (pDrvCtrl->codec_table[cad] != NULL)
                {
                hdacc_unsol_intr(pDrvCtrl->codec_table[cad], resp);
                }
            
            ret++;
            }
        pDrvCtrl->unsolq_st = HDAC_UNSOLQ_READY;
        }
    
    return (ret);
    }

/****************************************************************************
 * UINT32 command_sendone_internal
 *
 * Wrapper function that sends only one command to a given codec
 ****************************************************************************/

LOCAL UINT32 send_command
    (
    HDA_DRV_CTRL *pDrvCtrl,
    int cad,
    UINT32 verb
    )
    {
    int timeout;
    UINT32 *corb;
    UINT16 corbrp, corbwp,rintcnt,rirbwp;
    UINT8 corbsts, rirbsts,rirbctl,corbctl;

    verb &= ~HDA_CMD_CAD_MASK;
    verb |= ((UINT32)cad) << HDA_CMD_CAD_SHIFT;

    pDrvCtrl->codec_table[cad]->response = HDA_INVALID;

    pDrvCtrl->codec_table[cad]->pending++;
    pDrvCtrl->corb_wp++;
    pDrvCtrl->corb_wp %= pDrvCtrl->corb_size;
    corb = (UINT32 *)pDrvCtrl->corb_dma.dma_vaddr;

    dmaBufSync( pDrvCtrl->corb_dma.dma_tag,
                   pDrvCtrl->corb_dma.dma_map, DMABUFSYNC_PREWRITE );

    corb[pDrvCtrl->corb_wp] = verb;

    dmaBufSync( pDrvCtrl->corb_dma.dma_tag,
                   pDrvCtrl->corb_dma.dma_map, DMABUFSYNC_POSTWRITE );

    HDA_WRITE_2(HDAC_CORBWP, pDrvCtrl->corb_wp);

    timeout = 10000;
    do {
    if (rirb_flush(pDrvCtrl) == 0)
        hdaUsDelay(1000);
    } while (pDrvCtrl->codec_table[cad]->pending != 0 && --timeout);

    if (pDrvCtrl->codec_table[cad]->pending != 0)
        {
        HDA_READ_2(HDAC_CORBWP, corbwp);
        HDA_READ_2(HDAC_CORBRP, corbrp);
        HDA_READ_2(HDAC_RINTCNT, rintcnt);
        HDA_READ_2(HDAC_RIRBWP, rirbwp);
        HDA_READ_1(HDAC_CORBSTS, corbsts);
        HDA_READ_1(HDAC_RIRBSTS, rirbsts);
        HDA_READ_1(HDAC_CORBCTL, corbctl);
        HDA_READ_1(HDAC_RIRBCTL, rirbctl);
        HDA_DBG(HDA_DBG_ERR, "Command timeout on address %d, HDAC_CORBWP=%4.4x, HDAC_CORBRP=%4.4x, HDAC_RINTCNT=%4.4x, HDAC_RIRBWP=%4.4x\n",
                cad, corbwp, corbrp, rintcnt, rirbwp );
        HDA_DBG(HDA_DBG_ERR, "Command timeout on address %d, HDAC_CORBSTS=%2.2x, HDAC_RIRBSTS=%2.2x, RIRBCTL=%2.2x, HDAC_CORBCTL=%2.2x\n",
                cad, corbsts, rirbsts, rirbctl, corbctl );
        pDrvCtrl->codec_table[cad]->pending = 0;
        }

    if (pDrvCtrl->unsolq_rp != pDrvCtrl->unsolq_wp)
        {
        unsolq_flush(pDrvCtrl);
        }
    return (pDrvCtrl->codec_table[cad]->response);
    }


/****************************************************************************
 * int reset(softc *, int)
 *
 * Reset the hdac to a quiescent and known state.
 ****************************************************************************/
LOCAL int reset
    (
    HDA_DRV_CTRL *pDrvCtrl,
    int wakeup
    )
    {
    UINT32 gctl;
    int count, i;

    HDA_WRITE_2(HDAC_STATESTS, 0x3);
    
    /*
     * Stop all Streams DMA engine
     */
    for (i = 0; i < pDrvCtrl->num_iss; i++)
        HDA_WRITE_4(ISDCTL(pDrvCtrl, i), 0x0);
    for (i = 0; i < pDrvCtrl->num_oss; i++)
        HDA_WRITE_4(OSDCTL(pDrvCtrl, i), 0x0);
    for (i = 0; i < pDrvCtrl->num_bss; i++)
        HDA_WRITE_4(BSDCTL(pDrvCtrl, i), 0x0);

    /*
     * Stop Control DMA engines.
     */
    HDA_WRITE_1(HDAC_CORBCTL, 0x0);
    HDA_WRITE_1(HDAC_RIRBCTL, 0x0);

    /*
     * Reset DMA position buffer.
     */
    HDA_WRITE_4(HDAC_DPIBLBASE, 0x0);
    HDA_WRITE_4(HDAC_DPIBUBASE, 0x0);

    /*
     * Reset the controller. The reset must remain asserted for
     * a minimum of 100us.
     */
    HDA_READ_4(HDAC_GCTL, gctl);
    HDA_DBG(HDA_DBG_ERR, "HDAC_GCTL=%8.8x\n", gctl);
    HDA_WRITE_4(HDAC_GCTL, gctl & ~HDAC_GCTL_CRST);
    count = 10000;
    do
        {
        hdaUsDelay(10);
        HDA_READ_4(HDAC_GCTL, gctl);
        if (!(gctl & HDAC_GCTL_CRST))
            break;
        }
    while   (--count);

    HDA_DBG(HDA_DBG_INFO, "HDAC_GCTL=%8.8x, count=%d\n", gctl, count);
    if (gctl & HDAC_GCTL_CRST)
        {
        HDA_DBG(HDA_DBG_INFO, "Unable to put hdac in reset\n");
        return (ENXIO);
        }

    /* If wakeup is not requested - leave the controller in reset state. */
    if (!wakeup)
        return (0);

    hdaUsDelay(2000);
    HDA_READ_4(HDAC_GCTL, gctl);
    HDA_WRITE_4(HDAC_GCTL, gctl | HDAC_GCTL_CRST);
    count = 10000;

    do
        {
        hdaUsDelay(10); 
        HDA_READ_4(HDAC_GCTL, gctl);
        if (gctl & HDAC_GCTL_CRST)
            break;
        }
    while (--count);
    if (!(gctl & HDAC_GCTL_CRST))
        {
        HDA_DBG(HDA_DBG_ERR, "Device stuck in reset\n");
        return (ENXIO);
        }


    /*
     * Wait for codecs to finish their own reset sequence. The delay here
     * should be of 250us but for some reasons, on it's not enough in certain
     * scenario. Let's use twice as much as necessary to make sure that
     * it's reset properly.
     */
    hdaUsDelay(10000); /*2000*/

    return (0);
    }

/****************************************************************************
 * int dma_alloc
 *
 * This function allocate and setup a dma region (DMA_OBJECT).
 * It must be freed by a corresponding dma_free.
 ****************************************************************************/

LOCAL int dma_alloc
    (
    HDA_DRV_CTRL *pDrvCtrl,
    DMA_OBJECT *dma,
    bus_size_t size
    )
    {
    bus_size_t roundsz;
    int result;
    STATUS status;

    roundsz = ROUND_UP(size, HDA_DMA_ALIGNMENT);
    bzero((void *)dma, sizeof(DMA_OBJECT));

    /*
     * Create a DMA tag
     */
    dma->dma_tag = dmaBufTagCreate(pDrvCtrl->parentTag,
                       HDA_DMA_ALIGNMENT,
                       0, 
                       (pDrvCtrl->support_64bit) ? BUS_SPACE_MAXADDR : BUS_SPACE_MAXADDR_32BIT, 
                       BUS_SPACE_MAXADDR,
                       NULL,
                       NULL,
                       roundsz,
                       1,
                       roundsz,
                       DMABUF_ALLOCNOW|DMABUF_NOCACHE,
                       NULL,
                       NULL,
                       NULL/*&dma->dma_tag*/ );

#ifdef  HDA_DBG_ON
    HDA_DBG(HDA_DBG_INFO, "dma_tag= 0x%8.8x\n", dma->dma_tag);
#endif
    if (dma->dma_tag == (DMA_TAG_ID)NULL)
        {
        HDA_DBG(HDA_DBG_ERR, 
                "could not create dma tag\n");
        return (ENOMEM);
        }

    /* Allocate DMA'able memory for ring. */
    dma->dma_vaddr = dmaBufMemAlloc( dma->dma_tag, 
                                        NULL/*(void **)(bus_size_t)&dma->dma_vaddr*/,
                                        0, &dma->dma_map );

    if (dma->dma_vaddr == NULL)
        {
        result = ENOMEM;
        HDA_DBG(HDA_DBG_ERR, "%s: bus_dmamem_alloc failed (%x)\n", __func__, result);
        goto dma_alloc_fail;
        }
    dma->dma_size = roundsz;

    /*
     * Map the memory
     */
    status = dmaBufMapLoad(dma->dma_tag, dma->dma_map, 
                              (void *)dma->dma_vaddr, roundsz,  0 );

    if (status != OK)
        {
        HDA_DBG(HDA_DBG_ERR, "could not load DMA'able memory for\n");
        result = ENOMEM;
        goto dma_alloc_fail;
        }

    dma->dma_paddr = (bus_addr_t)dma->dma_map->fragList[0].frag;    
    HDA_DBG(HDA_DBG_INFO, "DMA ALLOC: %8.8x(v), %8.8x(p)\n", dma->dma_vaddr, dma->dma_paddr);
    HDA_DBG(HDA_DBG_INFO, "%s: size=%u -> roundsz=%u\n", __func__, (UINT32)size, (UINT32)roundsz);

    memset(dma->dma_vaddr, 0, roundsz);
    return (0);

    dma_alloc_fail:
    dma_free(pDrvCtrl, dma);

    return (result);
    }


/****************************************************************************
 * void dma_free(HDA_DRV_CTRL *, DMA_OBJECT *)
 *
 * Free a struct dhac_dma that has been previously allocated via the
 * dma_alloc function.
 ****************************************************************************/
LOCAL void dma_free
    (
    HDA_DRV_CTRL *pDrvCtrl,
    DMA_OBJECT *dma
    )
    {
    if (dma->dma_map != NULL)
        dmaBufMapUnload(dma->dma_tag, dma->dma_map);

    if (dma->dma_vaddr != NULL)
        {
        dmaBufMemFree(dma->dma_tag, dma->dma_vaddr, dma->dma_map);
        dma->dma_vaddr = NULL;
        }

    dma->dma_map = NULL;

    if (dma->dma_tag != NULL)
        {
        dmaBufTagDestroy(dma->dma_tag);
        dma->dma_tag = NULL;
        }

    dma->dma_size = 0;
    }

/****************************************************************************
 * int get_capabilities(HDA_DRV_CTRL *);
 *
 * Retreive the general capabilities of the hdac;
 *  Number of Input Streams
 *  Number of Output Streams
 *  Number of bidirectional Streams
 *  64bit ready
 *  CORB and RIRB sizes
 ****************************************************************************/

LOCAL int get_capabilities
    (
    HDA_DRV_CTRL *pDrvCtrl
    )
    {
    UINT16 gcap,gstatus;
    UINT8  min_vers, maj_vers;
    UINT8 corbsize, rirbsize;

    HDA_READ_2(HDAC_GCAP, gcap);
    HDA_READ_2(HDAC_GSTS, gstatus);
    HDA_DBG(HDA_DBG_ERR, "gcap=%4.4x, gstatus=%4.4x\n", gcap, gstatus);
    HDA_READ_1(HDAC_VMIN, min_vers);
    HDA_READ_1(HDAC_VMAJ, maj_vers);
    HDA_DBG(HDA_DBG_ERR, "min_vers=%2.2x, maj_vers=%2.2x\n", min_vers, maj_vers);
    pDrvCtrl->num_iss = HDAC_GCAP_ISS(gcap);
    pDrvCtrl->num_oss = HDAC_GCAP_OSS(gcap);
    pDrvCtrl->num_bss = HDAC_GCAP_BSS(gcap);
    pDrvCtrl->num_ss = pDrvCtrl->num_iss + pDrvCtrl->num_oss + pDrvCtrl->num_bss;
    pDrvCtrl->num_sdo = HDAC_GCAP_NSDO(gcap);
    pDrvCtrl->support_64bit = (gcap & HDAC_GCAP_64OK) != 0;

    HDA_READ_1(HDAC_CORBSIZE, corbsize);
    if ((corbsize & HDAC_CORBSIZE_CORBSZCAP_256) ==
        HDAC_CORBSIZE_CORBSZCAP_256)
        pDrvCtrl->corb_size = 256;
    else if ((corbsize & HDAC_CORBSIZE_CORBSZCAP_16) ==
             HDAC_CORBSIZE_CORBSZCAP_16)
        pDrvCtrl->corb_size = 16;
    else if ((corbsize & HDAC_CORBSIZE_CORBSZCAP_2) ==
             HDAC_CORBSIZE_CORBSZCAP_2)
        pDrvCtrl->corb_size = 2;
    else {
    HDA_DBG(HDA_DBG_ERR, "%s: Invalid corb size (%x)\n",
            __func__, corbsize);
    return (ENXIO);
    }

    HDA_READ_1(HDAC_RIRBSIZE, rirbsize);
    if ((rirbsize & HDAC_RIRBSIZE_RIRBSZCAP_256) ==
        HDAC_RIRBSIZE_RIRBSZCAP_256)
        pDrvCtrl->rirb_size = 256;
    else if ((rirbsize & HDAC_RIRBSIZE_RIRBSZCAP_16) ==
             HDAC_RIRBSIZE_RIRBSZCAP_16)
        pDrvCtrl->rirb_size = 16;
    else if ((rirbsize & HDAC_RIRBSIZE_RIRBSZCAP_2) ==
             HDAC_RIRBSIZE_RIRBSZCAP_2)
        pDrvCtrl->rirb_size = 2;
    else {
    HDA_DBG(HDA_DBG_ERR, "%s: Invalid rirb size (%x)\n",
            __func__, rirbsize);
    return (ENXIO);
    }

    if (pDrvCtrl->support_64bit)
        {
        HDA_DBG(HDA_DBG_ERR, "Caps: OSS %d, ISS %d, BSS %d, "
                "NSDO %d, 64bit, CORB %d, RIRB %d\n",
                pDrvCtrl->num_oss, pDrvCtrl->num_iss, pDrvCtrl->num_bss, 1 << pDrvCtrl->num_sdo,
                pDrvCtrl->corb_size, pDrvCtrl->rirb_size);
        }
    else
        {
        HDA_DBG(HDA_DBG_ERR, "Caps: OSS %d, ISS %d, BSS %d, "
                "NSDO %d, CORB %d, RIRB %d\n",
                pDrvCtrl->num_oss, pDrvCtrl->num_iss, pDrvCtrl->num_bss, 1 << pDrvCtrl->num_sdo,
                pDrvCtrl->corb_size, pDrvCtrl->rirb_size);
        }

    return (0);
    }


/****************************************************************************
 * void dmapos_init(HDA_DRV_CTRL *)
 *
 ****************************************************************************/
LOCAL void dmapos_init
    (
    HDA_DRV_CTRL *pDrvCtrl
    )
    {
    uint64_t addr = pDrvCtrl->pos_dma.dma_paddr;
    HDA_WRITE_4(HDAC_DPIBUBASE, addr >> 32);
    HDA_WRITE_4(HDAC_DPIBLBASE,
                 (addr & HDAC_DPLBASE_DPLBASE_MASK) |
                 HDAC_DPLBASE_DPLBASE_DMAPBE);
    }

/****************************************************************************
 * void corb_init(HDA_DRV_CTRL *)
 *
 * Initialize the corb registers for operations but do not start it up yet.
 * The CORB engine must not be running when this function is called.
 ****************************************************************************/
LOCAL void corb_init
    (
    HDA_DRV_CTRL *pDrvCtrl
    )
    {
    UINT8 corbsize = 0;
    uint64_t corbpaddr;

    /* Setup the CORB size. */
    switch (pDrvCtrl->corb_size)
        {
        case 256:
            corbsize = HDAC_CORBSIZE_CORBSIZE(HDAC_CORBSIZE_CORBSIZE_256);
            break;
        case 16:
            corbsize = HDAC_CORBSIZE_CORBSIZE(HDAC_CORBSIZE_CORBSIZE_16);
            break;
        case 2:
            corbsize = HDAC_CORBSIZE_CORBSIZE(HDAC_CORBSIZE_CORBSIZE_2);
            break;
        default:
            HDA_DBG(HDA_DBG_ERR, "%s: Invalid CORB size (%x)\n", __func__, pDrvCtrl->corb_size);
        }
    HDA_WRITE_1(HDAC_CORBSIZE, corbsize);

    /* Setup the CORB Address in the hdac */
    corbpaddr = (uint64_t)pDrvCtrl->corb_dma.dma_paddr;
    HDA_WRITE_4(HDAC_CORBLBASE, (UINT32)corbpaddr);
    HDA_WRITE_4(HDAC_CORBUBASE, (UINT32)(corbpaddr >> 32));

    /* Set the WP and RP */
    pDrvCtrl->corb_wp = 0;
    HDA_WRITE_2(HDAC_CORBWP, pDrvCtrl->corb_wp);
    HDA_WRITE_2(HDAC_CORBRP, HDAC_CORBRP_CORBRPRST);
    /*
     * The HDA specification indicates that the HDAC_CORBRPRST bit will always
     * read as zero. Unfortunately, it seems that at least the 82801G
     * doesn't reset the bit to zero, which stalls the corb engine.
     * manually reset the bit to zero before continuing.
     */
    HDA_WRITE_2(HDAC_CORBRP, 0x0);

    /* Enable CORB error reporting */

    HDA_WRITE_1(HDAC_CORBCTL, HDAC_CORBCTL_CMEIE);

    }

/****************************************************************************
 * void rirb_init(HDA_DRV_CTRL *)
 *
 * Initialize the rirb registers for operations but do not start it up yet.
 * The RIRB engine must not be running when this function is called.
 ****************************************************************************/

LOCAL void rirb_init
    (
    HDA_DRV_CTRL *pDrvCtrl
    )
    {
    UINT8 rirbsize = 0;
    uint64_t rirbpaddr;

    /* Setup the RIRB size. */
    switch (pDrvCtrl->rirb_size) {
    case 256:
        rirbsize = HDAC_RIRBSIZE_RIRBSIZE(HDAC_RIRBSIZE_RIRBSIZE_256);
        break;
    case 16:
        rirbsize = HDAC_RIRBSIZE_RIRBSIZE(HDAC_RIRBSIZE_RIRBSIZE_16);
        break;
    case 2:
        rirbsize = HDAC_RIRBSIZE_RIRBSIZE(HDAC_RIRBSIZE_RIRBSIZE_2);
        break;
    default:
        HDA_DBG(HDA_DBG_ERR, "%s: Invalid RIRB size (%x)\n", (int)__func__, (int)pDrvCtrl->rirb_size);
    }
    HDA_WRITE_1(HDAC_RIRBSIZE, rirbsize);

    /* Setup the RIRB Address in the hdac */
    rirbpaddr = (uint64_t)pDrvCtrl->rirb_dma.dma_paddr;
    HDA_WRITE_4(HDAC_RIRBLBASE, (UINT32)rirbpaddr);
    HDA_WRITE_4(HDAC_RIRBUBASE, (UINT32)(rirbpaddr >> 32));

    /* Setup the WP and RP */
    pDrvCtrl->rirb_rp = 0;
    HDA_WRITE_2(HDAC_RIRBWP, HDAC_RIRBWP_RIRBWPRST);

    /* setup the interrupt threshold */

    HDA_WRITE_2(HDAC_RINTCNT, pDrvCtrl->rirb_size / 2);

    HDA_WRITE_1(HDAC_RIRBCTL, HDAC_RIRBCTL_RINTCTL);

    dmaBufSync( pDrvCtrl->rirb_dma.dma_tag,
                   pDrvCtrl->rirb_dma.dma_map, DMABUFSYNC_PREREAD );
    }

/****************************************************************************
 * void corb_start(softc *)
 *
 * Startup the corb DMA engine
 ****************************************************************************/
LOCAL void corb_start
    (
    HDA_DRV_CTRL *pDrvCtrl
    )
    {
    UINT32 corbctl;

    HDA_READ_1(HDAC_CORBCTL, corbctl);
    corbctl |= (HDAC_CORBCTL_CORBRUN | HDAC_CORBCTL_CMEIE);
    HDA_WRITE_1(HDAC_CORBCTL, corbctl);
    }

/****************************************************************************
 * void rirb_start(softc *)
 *
 * Startup the rirb DMA engine
 ****************************************************************************/
LOCAL void rirb_start
    (
    HDA_DRV_CTRL *pDrvCtrl
    )
    {
    UINT32 rirbctl;

    HDA_READ_1(HDAC_RIRBCTL, rirbctl);
    rirbctl |= (HDAC_RIRBCTL_RIRBDMAEN | HDAC_RIRBCTL_RINTCTL);
    HDA_WRITE_1(HDAC_RIRBCTL, rirbctl);
    }

/****************************************************************************
 * void corb_stop(softc *)
 *
 * Stop the corb DMA engine
 ****************************************************************************/

LOCAL void corb_stop
    (
    HDA_DRV_CTRL *pDrvCtrl
    )
    {
    UINT32 corbctl;

    HDA_READ_1(HDAC_CORBCTL, corbctl);
    corbctl &= ~(HDAC_CORBCTL_CORBRUN | HDAC_CORBCTL_CMEIE);
    HDA_WRITE_1(HDAC_CORBCTL, corbctl);
    }

/****************************************************************************
 * void rirb_stop(softc *)
 *
 * Stop the rirb DMA engine
 ****************************************************************************/
LOCAL void rirb_stop
    (
    HDA_DRV_CTRL *pDrvCtrl
    )
    {
    UINT32 rirbctl;

    HDA_READ_1(HDAC_RIRBCTL, rirbctl);
    rirbctl &= ~(HDAC_RIRBCTL_RIRBDMAEN | HDAC_RIRBCTL_RINTCTL);
    HDA_WRITE_1(HDAC_RIRBCTL, rirbctl);
    }

LOCAL UINT32 hdAudioCommand (HDA_DRV_CTRL* pDrvCtrl, cad_t cad, UINT32 verb)
    {
    return send_command(pDrvCtrl, cad, verb);
    }

LOCAL void hdAudioWidgetDelete (WIDGET* w)
    {
    free (w);
    }

LOCAL WIDGET* hdAudioWidgetCreate (HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid)
    {
    WIDGET* w;

    w = calloc(1, sizeof(WIDGET));
    memset(w, 0, sizeof(WIDGET));

    lstInit(&w->widgetList);

    w->nid = nid;
    w->enable = TRUE;
    w->bindas = -1;
    w->codec = pDrvCtrl->codec_table[cad];

#if 0
    widget_parse (w);
#endif
    return w;
    }

LOCAL WIDGET* hdAudioGetRootWidget (HDA_DRV_CTRL* pDrvCtrl, cad_t cad)
    {
    return &pDrvCtrl->codec_table[cad]->root;
    }

#if 0
LOCAL void hdAudioWidgetTraverse(HDA_DRV_CTRL* pDrvCtrl, cad_t cad)
    {
    WIDGET *parent, *child;

    /* retrieve the root Widget */

    parent = hdAudioGetRootWidget(pDrvCtrl, cad);

    child = (WIDGET*)lstFirst(&parent->widgetList);
    
    while (child)
        {
        HDA_DBG(HDA_DBG_INFO, "Widget nid = %d child of %d\n", child->nid, parent->nid);

        if (lstCount(&child->widgetList) > 0)
            {
            parent = child;
            child = (WIDGET*)lstFirst((LIST*)&parent->widgetList);
            }
        else
            child = (WIDGET*)lstNext((NODE*)child);
        }
    }
#endif

LOCAL void hdAudioWidgetDeleteAll(HDA_DRV_CTRL* pDrvCtrl, HDCODEC_ID codec)
    {
    WIDGET *parent, *child, *root, *afg;

    /* retrieve the root Widget */

    root = parent = hdAudioGetRootWidget(pDrvCtrl, codec->cad);

    afg = child = (WIDGET*)lstFirst(&parent->widgetList);
    
    while (child)
        {
        HDA_DBG(HDA_DBG_INFO, "Widget nid = %d child of %d\n", child->nid, parent->nid);

        if (lstCount(&child->widgetList) > 0)
            {
            parent = child;
            child = (WIDGET*)lstFirst((LIST*)&parent->widgetList);
            }
        else
            child = (WIDGET*)lstNext((NODE*)child);

        hdAudioWidgetDelete (child);
        }

    hdAudioWidgetDelete (afg);
    }


LOCAL WIDGET* hdAudioWidgetNext (WIDGET *node)
    {
    /* retrieve the root Widget */

    if (NULL == node)
        {
        return NULL;
        }
    
    if (lstCount(&node->widgetList) > 0)
        {
        node = (WIDGET*)lstFirst((LIST*)&node->widgetList);
        }
    else
        node = (WIDGET*)lstNext((NODE*)node);

    return node;
    }


LOCAL WIDGET* hdAudioWidgetFind (HDCODEC_ID codec, cad_t cad, nid_t nid)
    {
    WIDGET *parent, *child;

    /* retrieve the root Widget */

    parent = hdAudioGetRootWidget(device_get_softc(), cad);

    if (parent->nid == nid)
        return parent;
    
    child = (WIDGET*)lstFirst(&parent->widgetList);
    
    while (child)
        {
        if (child->nid == nid)
            break;

        if (lstCount(&child->widgetList) > 0)
            {
            parent = child;
            child = (WIDGET*)lstFirst(&parent->widgetList);
            }
        else
            child = (WIDGET*)lstNext((NODE*)child);
        }
    
    return child;
    }

void hdAudioWidgetDiscovery (HDA_DRV_CTRL* pDrvCtrl, cad_t cad)
    {
    WIDGET *parent, *child;
    UINT32 verb;
    UINT32 val;
    int subnode = 0;
    int limit, size = 1;
    HDCODEC_ID codec;

    codec = device_get_codec(pDrvCtrl, cad);
    
    /* establish a root Widget */

    codec->cad = cad;

    parent = hdAudioGetRootWidget(device_get_softc(), cad);
    lstInit (&parent->widgetList);
    parent->nid = 0; /* root nid */

    /* check root for an AFG */
    verb = hdAudioFormVerb(cad, parent->nid,
                              HDA_CMD_VERB_GET_PARAMETER,
                              HDA_PARAM_SUB_NODE_COUNT);
    val = hdAudioCommand(pDrvCtrl, cad, verb);

    size = HDA_PARAM_SUB_NODE_COUNT_TOTAL(val); /* n children of parent */
    subnode = HDA_PARAM_SUB_NODE_COUNT_START(val); /* children of the AFG begins at node */
    limit = subnode + size;
    HDA_DBG(HDA_DBG_INFO, "Root node at nid=0: %d subnodes %d-%d\n",
        size, subnode, limit-1);

    /* store nid of AFG */
    codec->nid = subnode;
    
    while (subnode < limit)
        {
        child = hdAudioWidgetCreate(pDrvCtrl, cad, subnode);
        lstAdd (&parent->widgetList, (NODE*)child);

        HDA_DBG(HDA_DBG_INFO, "Widget nid = %d\n", child->nid);
        
        child->type = HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE(hdAudioCommand(pDrvCtrl,
            cad, HDA_CMD_GET_PARAMETER(0, subnode, HDA_PARAM_FCT_GRP_TYPE)));
        if (child->type == HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO)
            HDA_DBG(HDA_DBG_INFO, "Func Group Type = Audio\n");

        /* any children */
        verb = hdAudioFormVerb(cad, child->nid,
                                  HDA_CMD_VERB_GET_PARAMETER,
                                  HDA_PARAM_SUB_NODE_COUNT);
        val = hdAudioCommand(pDrvCtrl, cad, verb);

        size = HDA_PARAM_SUB_NODE_COUNT_TOTAL(val); /* n children of parent */
        if (size > 0)
            {
            parent = child;
            subnode = HDA_PARAM_SUB_NODE_COUNT_START(val); /* children of the AFG begins at node */
            limit = subnode + size;
            HDA_DBG(HDA_DBG_INFO, "AFG at nid= %d: %d subnodes %d-%d\n", child->nid,
                size, subnode, limit-1);

            codec->nodecnt = HDA_PARAM_SUB_NODE_COUNT_TOTAL(val);
            codec->startnode = subnode;
            codec->endnode = limit - 1;
            codec->outamp_cap = 0xffffffff;
            codec->inamp_cap =  0xffffffff;
            codec->supp_stream_formats = 0xffffffff;
            codec->supp_pcm_size_rate = 0xffffffff;
            }
        else
            {
            subnode++;
#if 0
            widget_parse (child);
#endif
            }
        }
    }


LOCAL WIDGET* hdAudioWidgetNum (HDCODEC_ID codec, int num)
    {
    WIDGET *node;

    node = hdAudioWidgetNext (hdAudioGetRootWidget(device_get_softc(), codec->cad));

    return (WIDGET*)lstNth(&node->widgetList, num + 1);
    }

LOCAL UINT32 hdAudioFormVerb (cad_t cad, nid_t nid, UINT32 cmd, UINT32 payload)
    {
    if (cmd & 0x700)
        {
        /* get/set 12 bit cmd 8 bit data */
        return HDA_CMD_12BIT((cad), (nid), cmd, payload);
        }
    else
        {
        /* get/set 4 bit cmd 12 bit data */
        return HDA_CMD_4BIT((cad), (nid), cmd, payload);
        }
    }

/****************************************************************************
 * Device Methods
 ****************************************************************************/

/**********************************************************************
 *
 * channel_init - prepare a PCM_CHANNEL for use by driver
 *
 * Implementation of method pcm_channel_init
 *
 * RETURNS: an implementation specific opaque pointer as a handle
 *
 * NOMANUAL
 */

void * channel_init
    (
    void *data, struct snd_buf *b, PCM_CHANNEL *chan, int dir
    )
    {
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    CHAN *ch = chan->stream;
    PCM_DEVINFO *pdevinfo  = ch->pcm_dev;


    semTake (pDrvCtrl->mutex, WAIT_FOREVER);

    ch->b = b;
    ch->c = chan;
    chan->sndbuf = b;

    ch->pcm_dev = pdevinfo;
    ch->blksz = pdevinfo->chan_size / pdevinfo->chan_blkcnt;
    ch->blkcnt = pdevinfo->chan_blkcnt;

    b->blksz = ch->blksz;
    b->blkcnt = ch->blkcnt;
    b->dma_flags = 0;

    if (sndbuf_alloc(ch->b, pDrvCtrl->sndbuf_dma_tag, 0, ch->pcm_dev->chan_size) == OK)
        ch = data;
    else
        HDA_DBG(HDA_DBG_ERR, "sndbuf_alloc() failed!\n");

    semGive (pDrvCtrl->mutex);
    
    return (ch);
    }

/**********************************************************************
 *
 * channl_setformat - method set audio stream format
 *
 * Implementation of method pcm_channel_setformat
 *
 * RETURNS: 0 if requested format is support
 *
 * NOMANUAL
 */

int channel_setformat(PCM_CHANNEL *chan, UINT32 format)
    {
    int i, val = EINVAL;
    CHAN *ch = chan->stream;
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();

    semTake (pDrvCtrl->mutex, WAIT_FOREVER);

    /*
      this driver exploits unused bits in afmt
      to indicate the number of channels used in afmt
    */
    /*icelee, workaround for 8bit sample  */
    if (chan->afmt == AFMT_U8 )
        format = AFMT_S16_LE;
    else
        format = chan->afmt;
        
    if  (chan->channels == 1 )
        format |= (2 << 20);
    else
    	format |= (chan->channels << 20);
#if 0
    HDA_DBG(HDA_DBG_INFO, "channel = %d, format = 0x%x\n", chan->channels, format);

    for (i = 0; ch->caps.fmtlist[i] != 0; i++)
        {
        printf("%s: fmtlist= x%08x\n", (int)__func__,ch->caps.fmtlist[i]);
        }
#endif

    /* locate supported format from fmtlist */

    for (i = 0; ch->caps.fmtlist[i] != 0; i++)
        {
        if (format == ch->caps.fmtlist[i])
            {
            ch->fmt = format;
            val = 0;
            break;
            }
        }

    semGive (pDrvCtrl->mutex);
    return (val);
    }

UINT32 channel_setspeed(PCM_CHANNEL *chan, UINT32 speed)
    {
    int i;
    UINT32 spd = 0;
    CHAN *ch = chan->stream;
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();

    semTake (pDrvCtrl->mutex, WAIT_FOREVER);

    for (i = 0; ch->pcmrates[i] != 0; i++)
        {
        spd = ch->pcmrates[i];
        if (speed != 0 && spd / speed * speed == spd)
            {
            ch->spd = spd;
            break;
            }
        }

    chan->rate = ch->spd = spd;
    
    semGive (pDrvCtrl->mutex);

    return (spd);
    }

LOCAL UINT16 stream_format(CHAN *ch)
    {
    int i;
    UINT16 fmt;

    fmt = 0;
    if (ch->fmt & AFMT_S16_LE)
        fmt |= ch->bit16 << 4;
    else if (ch->fmt & AFMT_S32_LE)
        fmt |= ch->bit32 << 4;
    else
        fmt |= 1 << 4;
    for (i = 0; i < HDA_RATE_TAB_LEN; i++) {
    if (hda_rate_tab[i].valid && ch->spd == hda_rate_tab[i].rate) {
    fmt |= hda_rate_tab[i].base;
    fmt |= hda_rate_tab[i].mul;
    fmt |= hda_rate_tab[i].div;
    break;
    }
    }
    fmt |= (AFMT_CHANNEL(ch->fmt) - 1);

    return (fmt);
    }

LOCAL int allowed_stripes(UINT16 fmt)
    {
    static const int bits[8] = { 8, 16, 20, 24, 32, 32, 32, 32 };
    int size;

    size = bits[(fmt >> 4) & 0x03];
    size *= (fmt & 0x0f) + 1;
    size *= ((fmt >> 11) & 0x07) + 1;
    return (0xffffffffU >> (32 - ffsMsb(size / 8)));
    }

LOCAL void audio_setup(CHAN *ch)
    {
    HDCODEC_ID codec = ch->codec;
    ASSOC *as = &codec->assoc_table[ch->as];
    WIDGET *w, *wp;
    int i, j, chn, cchn, totalchn, totalextchn, c;
    UINT16 fmt, dfmt;
    /* Mapping channel pairs to codec pins/converters. */
    const static UINT16 convmap[2][5] =
        {{ 0x0010, 0x0001, 0x0201, 0x0231, 0x0231 }, /* 5.1 */
         { 0x0010, 0x0001, 0x2001, 0x2031, 0x2431 }};/* 7.1 */

    int convmapid = -1;
    nid_t nid;

    totalchn = AFMT_CHANNEL(ch->fmt);
    totalextchn = AFMT_EXTCHANNEL(ch->fmt);

    HDA_DBG(HDA_DBG_INFO, 
            "PCMDIR_%s: Stream setup fmt=%08x (%d.%d) speed=%d\n",
            (ch->dir == CTL_OUT) ? "PLAY" : "REC",
            ch->fmt, totalchn - totalextchn, totalextchn, ch->spd);

    fmt = stream_format(ch);

    /* Set channels to I/O converters mapping for known speaker setups. */
    if ((as->pinset == 0x0007 || as->pinset == 0x0013)) /* Standard 5.1 */
        convmapid = 0;
    else if (as->pinset == 0x0017) /* Standard 7.1 */
        convmapid = 1;

    dfmt = HDA_CMD_SET_DIGITAL_CONV_FMT1_DIGEN;

    chn = 0;
    for (i = 0; ch->io[i] != -1; i++) {
    w = widget_get(ch->codec, ch->io[i]);
    if (w == NULL)
        continue;

    /* If HP redirection is enabled, but failed to use same
       DAC, make last DAC to duplicate first one. */
    if (as->fakeredir && i == (as->pincnt - 1)) {
    c = (ch->sid << 4);
    } else {
    /* Map channels to I/O converters, if set. */
    if (convmapid >= 0)
        chn = (((convmap[convmapid][totalchn / 2]
                 >> i * 4) & 0xf) - 1) * 2;
    if (chn < 0 || chn >= totalchn) {
    c = 0;
    } else {
    c = (ch->sid << 4) | chn;
    }
    }
    hda_command(ch->codec, HDA_CMD_SET_CONV_FMT(0, ch->io[i], fmt));
    if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(w->param.widget_cap))
        {
        hda_command(ch->codec,
                    HDA_CMD_SET_DIGITAL_CONV_FMT1(0, ch->io[i], dfmt));
        }
    hda_command(ch->codec, HDA_CMD_SET_CONV_STREAM_CHAN(0, ch->io[i], c));
    if (HDA_PARAM_AUDIO_WIDGET_CAP_STRIPE(w->param.widget_cap))
        {
        hda_command(ch->codec, HDA_CMD_SET_STRIPE_CONTROL(0, w->nid, ch->stripectl));
        }
    cchn = HDA_PARAM_AUDIO_WIDGET_CAP_CC(w->param.widget_cap);
    if (cchn > 1 && chn < totalchn) {
    cchn = min(cchn, totalchn - chn - 1);
    hda_command(ch->codec,
                HDA_CMD_SET_CONV_CHAN_COUNT(0, ch->io[i], cchn));
    }
    HDA_DBG(HDA_DBG_INFO,
            "PCMDIR_%s: Stream setup nid=%d: "
            "fmt=0x%04x, dfmt=0x%04x, chan=0x%04x, "
            "chan_count=0x%02x, stripe=%d\n",
            (ch->dir == CTL_OUT) ? "PLAY" : "REC",
            ch->io[i], fmt, dfmt, c, cchn, ch->stripectl);

    for (j = 0; j < 16; j++) {
    if (as->dacs[ch->asindex][j] != ch->io[i])
        continue;
    nid = as->pins[j];
#if 0
    logMsg("%s: nid= %d\n", (int)__func__,nid,3,4,5,6);
#endif
    wp = widget_get(ch->codec, nid);
    if (wp == NULL)
        continue;
    if (!HDA_PARAM_PIN_CAP_DP(wp->wclass.pin.cap) &&
        !HDA_PARAM_PIN_CAP_HDMI(wp->wclass.pin.cap))
        continue;

    }
    chn += cchn + 1;
    }
    }

/*
 * Greatest Common Divisor.
 */
LOCAL unsigned gcd(unsigned a, unsigned b)
    {
    u_int c;

    while (b != 0) {
    c = a;
    a = b;
    b = (c % b);
    }
    return (a);
    }

/*
 * Least Common Multiple.
 */
LOCAL unsigned lcm(unsigned a, unsigned b)
    {
    return ((a * b) / gcd(a, b));
    }


int channel_setfragments(PCM_CHANNEL* chan, UINT32 blksz, UINT32 blkcnt)
    {
    CHAN *ch = chan->stream;
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();

    semTake (pDrvCtrl->mutex, WAIT_FOREVER);

    blksz -= blksz % lcm(HDA_DMA_ALIGNMENT, sndbuf_getalign(ch->b));

    if (blksz > (sndbuf_getsize(ch->b) / HDA_BDL_MIN))
        blksz = sndbuf_getsize(ch->b) / HDA_BDL_MIN;
    if (blksz < HDA_BLK_MIN)
        blksz = HDA_BLK_MIN;
    if (blkcnt > HDA_BDL_MAX)
        blkcnt = HDA_BDL_MAX;
    if (blkcnt < HDA_BDL_MIN)
        blkcnt = HDA_BDL_MIN;

    while ((blksz * blkcnt) > sndbuf_getsize(ch->b))
        {
        if ((blkcnt >> 1) >= HDA_BDL_MIN)
            blkcnt >>= 1;
        else if ((blksz >> 1) >= HDA_BLK_MIN)
            blksz >>= 1;
        else
            break;
        }
    
    if ((sndbuf_getblksz(ch->b) != blksz) || (sndbuf_getblkcnt(ch->b) != blkcnt))
        {
        if (sndbuf_resize(ch->b, blkcnt, blksz) != OK)
            HDA_DBG(HDA_DBG_ERR, "%s: failed blksz=%u blkcnt=%u\n", __func__, blksz, blkcnt);

        }
                      

    ch->blksz = sndbuf_getblksz(ch->b);
    ch->blkcnt = sndbuf_getblkcnt(ch->b);

    semGive (pDrvCtrl->mutex);

    return (0);
    }

void channel_stop(PCM_CHANNEL *chan)
    {
    CHAN *ch = chan->stream;
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    WIDGET *w;
    int i;

    if ((ch->flags & CHN_RUNNING) == 0)
        return;

    semTake (pDrvCtrl->mutex, WAIT_FOREVER);

    ch->flags &= ~CHN_RUNNING;

    stream_stop(ch->dir == CTL_OUT ? 1 : 0, ch->sid);

    for (i = 0; ch->io[i] != -1; i++) {
    w = widget_get(ch->codec, ch->io[i]);
    if (w == NULL)
        continue;
    if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(w->param.widget_cap))
        {
        hda_command(ch->codec, HDA_CMD_SET_DIGITAL_CONV_FMT1(0, ch->io[i], 0));
        }
    hda_command(ch->codec, HDA_CMD_SET_CONV_STREAM_CHAN(0, ch->io[i], 0));
    }

    stream_free(ch->dir == CTL_OUT ? 1 : 0, ch->sid);

    semGive (pDrvCtrl->mutex);
    
    }

int channel_start(PCM_CHANNEL* chan)
    {
    CHAN *ch = chan->stream;
    UINT32 fmt;

    fmt = stream_format(ch);
    ch->stripectl = ffsMsb(ch->stripecap & allowed_stripes(fmt)) - 1;

    ch->sid = stream_alloc(ch->codec,
                                ch->dir == CTL_OUT ? 1 : 0,
                                fmt,
                                ch->stripectl,
                                &ch->dmapos);

    if (ch->sid <= 0)
        return (EBUSY);
    audio_setup(ch);

    stream_reset(ch->dir == CTL_OUT ? 1 : 0, ch->sid);

    stream_start(ch->dir == CTL_OUT ? 1 : 0, ch->sid,
                      sndbuf_getbufaddr(ch->b), ch->blksz, ch->blkcnt);

    ch->flags |= CHN_RUNNING;
    return (0);
    }

int channel_trigger(PCM_CHANNEL* chan, int go)
    {
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    int error = 0;

    if (!PCMTRIG_COMMON(go))
        return (0);

    semTake (pDrvCtrl->mutex, WAIT_FOREVER);
    
    switch (go)
        {
        case PCMTRIG_START:
            error = channel_start(chan);
            break;
            
        case PCMTRIG_STOP:
        case PCMTRIG_ABORT:
            channel_stop(chan);
            break;
        default:
            break;
        }

    semGive (pDrvCtrl->mutex);

    return (error);
    }

UINT32 channel_getptr(PCM_CHANNEL *chan)
    {
    CHAN *ch = chan->stream;
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    UINT32 ptr;

    semTake (pDrvCtrl->mutex, WAIT_FOREVER);

    if (ch->dmapos != NULL)
        {
        ptr = *(ch->dmapos);
        }
    else
        {
        ptr = stream_getptr (ch->dir == CTL_OUT ? 1 : 0, ch->sid);
        }

    /*
     * Round to available space and force 128 bytes aligment.
     */
    ptr %= ch->blksz * ch->blkcnt;
    ptr &= HDA_BLK_ALIGN;

    semGive (pDrvCtrl->mutex);

    return (ptr);
    }

PCMCHAN_CAPS * channel_getcaps(PCM_CHANNEL* chan)
    {
    CHAN *ch = chan->stream;
    return (&(ch->caps));
    }

/*
 * OSS Mixer set method.
 */
UINT32 audio_ctl_ossmixer_setrecsrc(SND_MIXER *m, UINT32 src)
    {
    PCM_DEVINFO *pdevinfo = ossmix_getdevinfo(m);
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    HDCODEC_ID codec = pdevinfo->codec;
    WIDGET *w;
    ASSOC *as;
    AUDIO_CTL *ctl;
    CHAN *ch;
    int i, j;
    UINT32 ret = 0xffffffff;

    semTake (pDrvCtrl->mutex, WAIT_FOREVER);

    if (pdevinfo->recas < 0)
        {
        semGive (pDrvCtrl->mutex);
        return (0);
        }

    as = &codec->assoc_table[pdevinfo->recas];

    /* For non-mixed associations we always recording everything. */
    if (!as->mixed)
        {
        semGive (pDrvCtrl->mutex);
        return (ossmix_getrecdevs(m));
        }

    /* Commutate requested recsrc for each ADC. */
    for (j = 0; j < as->num_chans; j++)
        {
        ch = &codec->hdaa_chan_table[as->chans[j]];
        for (i = 0; ch->io[i] >= 0; i++)
            {
            w = widget_get(codec, ch->io[i]);
            if (w == NULL || w->enable == 0)
                continue;
            ret &= audio_ctl_recsel_comm(pdevinfo, src,
                                              ch->io[i], 0);
            }
        }
    if (ret == 0xffffffff)
        ret = 0;

    /*
     * Some controls could be shared. Reset volumes for controls
     * related to previously chosen devices, as they may no longer
     * affect the signal.
     */
    i = 0;
    while ((ctl = audio_ctl_each(codec, &i)) != NULL)
        {
        if (ctl->enable == 0 ||
            !(ctl->ossmask & pdevinfo->recsrc))
            continue;
        if (!((pdevinfo->playas >= 0 &&
               ctl->widget->bindas == pdevinfo->playas) ||
              (pdevinfo->recas >= 0 &&
               ctl->widget->bindas == pdevinfo->recas) ||
              (pdevinfo->index == 0 &&
               ctl->widget->bindas == -2)))
            continue;
        for (j = 0; j < SOUND_MIXER_NRDEVICES; j++)
            {
            if (pdevinfo->recsrc & (1 << j))
                {
                ctl->devleft[j] = 0;
                ctl->devright[j] = 0;
                ctl->devmute[j] = 0;
                }
            }
        }

    /*
     * Some controls could be shared. Set volumes for controls
     * related to devices selected both previously and now.
     */
    for (j = 0; j < SOUND_MIXER_NRDEVICES; j++)
        {
        if ((ret | pdevinfo->recsrc) & (1 << j))
            audio_ctl_dev_volume(pdevinfo, j);
        }

    pdevinfo->recsrc = ret;

    semGive (pDrvCtrl->mutex);
    return (ret);
    }

int audio_ctl_ossmixer_init(SND_MIXER *m)
    {
    PCM_DEVINFO *pdevinfo = ossmix_getdevinfo(m);
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    HDCODEC_ID codec = pdevinfo->codec;
    WIDGET *w, *cw;
    UINT32 mask = 0, recmask = 0;
    int i, j;

    semTake (pDrvCtrl->mutex, WAIT_FOREVER);

    pdevinfo->mixer = m;
    mask = pdevinfo->ossmask;

    for (i = 0; i < SOUND_MIXER_NRDEVICES; i++)
        {
        pdevinfo->left[i] = 100;
        pdevinfo->right[i] = 100;
        }

    if (pdevinfo->playas >= 0)
        {
        for (i = codec->startnode; i < codec->endnode; i++)
            {
            w = widget_get(codec, i);
            if (w == NULL || w->enable == 0)
                continue;

            if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX ||
                w->param.eapdbtl == HDA_INVALID ||
                w->bindas != pdevinfo->playas)
                continue;

            mask |= SOUND_MASK_OGAIN;
            break;
            }

        if ((mask & SOUND_MASK_VOLUME) == 0)
            {
            mask |= SOUND_MASK_VOLUME;
#if 0
            mix_setparentchild(m, SOUND_MIXER_VOLUME, SOUND_MASK_PCM);
            mix_setrealdev(m, SOUND_MIXER_VOLUME, SOUND_MIXER_NONE);
#endif
            }
        }

    if (pdevinfo->recas >= 0)
        {
        for (i = 0; i < 16; i++)
            {
            if (codec->assoc_table[pdevinfo->recas].dacs[0][i] < 0)
                continue;
            w = widget_get(codec,
                                codec->assoc_table[pdevinfo->recas].dacs[0][i]);
            if (w == NULL || w->enable == 0)
                continue;
            for (j = 0; j < w->nconns; j++)
                {
                if (w->connsenable[j] == 0)
                    continue;
                cw = widget_get(codec, w->conns[j]);
                if (cw == NULL || cw->enable == 0)
                    continue;
                if (cw->bindas != pdevinfo->recas &&
                    cw->bindas != -2)
                    continue;
                recmask |= cw->ossmask;
                }
            }
        }

    recmask &= (1 << SOUND_MIXER_NRDEVICES) - 1;
    mask &= (1 << SOUND_MIXER_NRDEVICES) - 1;
    pdevinfo->ossmask = mask;

    ossmix_setrecdevs(m, recmask);
    ossmix_setdevs(m, mask);
    
    /* default to microphone as recording source */
    
    if (pdevinfo->autorecsrc == AUTO_SELECT_RECORD_SOURCE_DISABLE)
        ossmix_setrecsrc(pdevinfo->mixer, SOUND_MASK_MIC);

    semGive (pDrvCtrl->mutex);
    return (0);
    }

/*
 * OSS Mixer set method.
 */
int audio_ctl_ossmixer_set
    (
    SND_MIXER *m,
    unsigned dev,
    unsigned left,
    unsigned right
    )
    {
    PCM_DEVINFO *pdevinfo = ossmix_getdevinfo(m);
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    HDCODEC_ID codec = pdevinfo->codec;
    WIDGET *w;
    int i;

    semTake (pDrvCtrl->mutex, WAIT_FOREVER);

    /* Save new values. */
    pdevinfo->left[dev] = left;
    pdevinfo->right[dev] = right;

    /* 'ogain' is the special case implemented with EAPD. */
    if (dev == SOUND_MIXER_OGAIN)
        {
        UINT32 orig;
        w = NULL;
        for (i = codec->startnode; i < codec->endnode; i++)
            {
            w = widget_get(codec, i);
            if (w == NULL || w->enable == 0)
                continue;
            if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX ||
                w->param.eapdbtl == HDA_INVALID)
                continue;
            break;
            }

        if (i >= codec->endnode)
            {
            semGive (pDrvCtrl->mutex);
            return (-1);
            }

        orig = w->param.eapdbtl;
        if (left == 0)
            w->param.eapdbtl &= ~HDA_CMD_SET_EAPD_BTL_ENABLE_EAPD;
        else
            w->param.eapdbtl |= HDA_CMD_SET_EAPD_BTL_ENABLE_EAPD;

        if (orig != w->param.eapdbtl)
            {
            UINT32 val;

            val = w->param.eapdbtl;

            hda_command(codec, HDA_CMD_SET_EAPD_BTL_ENABLE(0, w->nid, val));
            }

        semGive (pDrvCtrl->mutex);
        return (left | (left << 8));
        }

    /* Recalculate all controls related to this OSS device. */
    audio_ctl_dev_volume(pdevinfo, dev);

    semGive (pDrvCtrl->mutex);
    return (left | (right << 8));
    }


/*****************************************************************************
*
* hdAudioUnlink -  Unlink handler
*
* RETURNS: OK if device was successfully destroyed, otherwise ERROR
*
* ERRNO: N/A
*/

LOCAL STATUS hdCodecUnlink
    (
    HDCODEC_ID codec,
    void * unused
    )
    { 
    int i;
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    
	for (i = 0; i < codec->num_hdaa_chans; i++)
        {
		if (codec->hdaa_chan_table[i].flags & CHN_RUNNING)
            {
            /* Use CHN_SUSPEND to indicate channel may not be started */
			codec->hdaa_chan_table[i].flags |= CHN_SUSPEND;
			channel_stop(codec->hdaa_chan_table[i].c);
            }
        }
    delete_pcms(codec);

	hda_command(codec, HDA_CMD_SET_POWER_STATE(0, codec->nid, HDA_CMD_POWER_STATE_D3));

    hdAudioWidgetDeleteAll(pDrvCtrl, codec);

    free (codec->assoc_table);
    free (codec->ctl);
    free (codec->pcm_dev_table);
    free (codec->hdaa_chan_table);
    free (codec);
    return OK;
    }


/*****************************************************************************
*
* hdAudioUnlink - unlink handler
*
* RETURNS: OK if device was successfully destroyed, otherwise ERROR
*
* ERRNO: N/A
*/

LOCAL STATUS hdAudioUnlink
    (
    void * unused
    )
    { 
    int i;
    HDA_DRV_CTRL * pDrvCtrl;
    HDA_MSG msg;
    
    pDrvCtrl = device_get_softc();

    semTake(pDrvCtrl->mutex, NO_WAIT);
    
    msg.type = HDA_MSG_TYPE_UNLINK;
    msgQSend(pDrvCtrl->unsolq_msgQ, (char*)&msg,
             HDA_MSG_SIZE, NO_WAIT, MSG_PRI_NORMAL);

    for (i = 0; i < pDrvCtrl->num_codec; i++)
        hdCodecUnlink(pDrvCtrl->codec_table[i], 0);

    for (i = 0; i < pDrvCtrl->num_ss; i++)
        dma_free (pDrvCtrl, &pDrvCtrl->streams[i].bdl);
    free (pDrvCtrl->streams);


    dma_free (pDrvCtrl, &pDrvCtrl->rirb_dma);
    dma_free (pDrvCtrl, &pDrvCtrl->corb_dma);
    dma_free (pDrvCtrl, &pDrvCtrl->pos_dma);

    dmaBufTagDestroy (pDrvCtrl->sndbuf_dma_tag);

    pciIntDisconnect (pDrvCtrl->intVector, hdAudioIsr);
    sysIntDisablePIC (pDrvCtrl->intLvl);

    semDelete (pDrvCtrl->mutex);

    free (pDrvCtrl);

    return OK;
    }

LOCAL WIDGET* widget_get (HDCODEC* codec, nid_t nid)
    {
    return hdAudioWidgetFind (codec, codec->cad, nid);
    }

LOCAL UINT32 hda_command (HDCODEC* codec, UINT32 verb)
    {
    HDA_DRV_CTRL *pDrvCtrl = device_get_softc();
    return send_command(pDrvCtrl, codec->cad, verb);
    }

LOCAL int hdacc_unsol_alloc(HDCODEC_ID  codec, int wanted)
    {
    int tag;

    wanted &= 0x3f;
    tag = wanted;
    do
        {
        if (codec->tags[tag] == NULL)
            {
            codec->tags[tag] = codec;
            return (tag);
            }
        tag++;
        tag &= 0x3f;
        } while (tag != wanted);
    
    return (-1);
    }

LOCAL void hdacc_unsol_intr(HDCODEC_ID codec, UINT32 resp)
    {
    HDCODEC_ID child;
    int tag;

    tag = resp >> 26;
    if ((child = codec->tags[tag]) != NULL)
        {
        WIDGET *w;
        int i, tag, flags;

        HDA_DBG(HDA_DBG_INFO, "Unsolicited response %08x\n", resp);

        tag = resp >> 26;
        for (i = codec->startnode; i < codec->endnode; i++)
            {
            w = widget_get(codec, i);
            if (w == NULL || w->enable == 0 || w->type !=
                HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
                continue;
            if (w->unsol != tag)
                continue;
            if (HDA_PARAM_PIN_CAP_DP(w->wclass.pin.cap) ||
                HDA_PARAM_PIN_CAP_HDMI(w->wclass.pin.cap))
                flags = resp & 0x03;
            else
                flags = 0x01;
            if (flags & 0x01)
                {
                presence_handler(w);
                }
            }
        }
    else
        HDA_DBG(HDA_DBG_ERR, "Unexpected unsolicited "
                "response with tag %d: %08x\n", tag, resp);
    }


LOCAL void audio_ctl_amp_set_internal
    (
    HDCODEC_ID codec,
    nid_t nid,
    int index, int lmute, int rmute,
    int left, int right, int dir
    )
    {
    UINT16 v = 0;

    HDA_DBG(HDA_DBG_INFO, "Setting amplifier nid=%d index=%d %s mute=%d/%d vol=%d/%d\n",
            nid,index,dir ? "in" : "out",lmute,rmute,left,right);

    if (left != right || lmute != rmute)
        {
        v = (1 << (15 - dir)) | (1 << 13) | (index << 8) |
            (lmute << 7) | left;
        hda_command(codec, HDA_CMD_SET_AMP_GAIN_MUTE(0, nid, v));
        v = (1 << (15 - dir)) | (1 << 12) | (index << 8) |
            (rmute << 7) | right;
        }
    else
        {
        v = (1 << (15 - dir)) | (3 << 12) | (index << 8) |
            (lmute << 7) | left;
        }

    hda_command(codec, HDA_CMD_SET_AMP_GAIN_MUTE(0, nid, v));
    }

LOCAL void audio_ctl_amp_set
    (
    AUDIO_CTL *ctl,
    UINT32 mute,
    int left,
    int right
    )
    {
    nid_t nid;
    int lmute, rmute;

    nid = ctl->widget->nid;

    /* Save new values if valid. */
    if (mute != HDAA_AMP_MUTE_DEFAULT)
        ctl->muted = mute;
    if (left != HDAA_AMP_VOL_DEFAULT)
        ctl->left = left;
    if (right != HDAA_AMP_VOL_DEFAULT)
        ctl->right = right;

    /* Prepare effective values */
    if (ctl->forcemute)
        {
        lmute = 1;
        rmute = 1;
        left = 0;
        right = 0;
        }
    else
        {
        lmute = HDAA_AMP_LEFT_MUTED(ctl->muted);
        rmute = HDAA_AMP_RIGHT_MUTED(ctl->muted);
        left = ctl->left;
        right = ctl->right;
        }
    /* Apply effective values */
    if (ctl->dir & CTL_OUT)
        audio_ctl_amp_set_internal(ctl->widget->codec, nid, ctl->index,
                                        lmute, rmute, left, right, 0);
    if (ctl->dir & CTL_IN)
        audio_ctl_amp_set_internal(ctl->widget->codec, nid, ctl->index,
                                        lmute, rmute, left, right, 1);
    }

LOCAL void widget_connection_select(WIDGET *w, UINT8 index)
    {
    if (w == NULL || w->nconns < 1 || index > (w->nconns - 1))
        return;

    HDA_DBG(HDA_DBG_INFO, "Setting selector nid=%d index=%d\n", w->nid, index);

    hda_command(w->codec,
                HDA_CMD_SET_CONNECTION_SELECT_CONTROL(0, w->nid, index));
    w->selconn = index;
    }

LOCAL void widget_connection_parse(WIDGET *w)
    {
    UINT32 res;
    int i, j, max, ents, entnum;
    nid_t nid = w->nid;
    nid_t cnid, addcnid, prevcnid;

    w->nconns = 0;

    res = hda_command(w->codec,
                      HDA_CMD_GET_PARAMETER(0, nid, HDA_PARAM_CONN_LIST_LENGTH));

    ents = HDA_PARAM_CONN_LIST_LENGTH_LIST_LENGTH(res);

    if (ents < 1)
        return;

    entnum = HDA_PARAM_CONN_LIST_LENGTH_LONG_FORM(res) ? 2 : 4;
    max = (sizeof(w->conns) / sizeof(w->conns[0])) - 1;
    prevcnid = 0;

#define CONN_RMASK(e)       (1 << ((32 / (e)) - 1))
#define CONN_NMASK(e)       (CONN_RMASK(e) - 1)
#define CONN_RESVAL(r, e, n)    ((r) >> ((32 / (e)) * (n)))
#define CONN_RANGE(r, e, n) (CONN_RESVAL(r, e, n) & CONN_RMASK(e))
#define CONN_CNID(r, e, n)  (CONN_RESVAL(r, e, n) & CONN_NMASK(e))

    for (i = 0; i < ents; i += entnum) {
    res = hda_command(w->codec,
                      HDA_CMD_GET_CONN_LIST_ENTRY(0, nid, i));
    for (j = 0; j < entnum; j++) {
    cnid = CONN_CNID(res, entnum, j);
    if (cnid == 0) {
    if (w->nconns < ents)
        {
        HDA_DBG(HDA_DBG_INFO, 
                "WARNING: nid=%d has zero cnid "
                "entnum=%d j=%d index=%d "
                "entries=%d found=%d res=0x%08x\n",
                nid, entnum, j, i,
                ents, w->nconns, res);
        }
    else
        {
        goto getconns_out;
        }
    }
    if (cnid < w->codec->startnode ||
        cnid > w->codec->endnode) {
    HDA_DBG(HDA_DBG_INFO, 
            "WARNING: nid=%d has cnid %d outside "
            "of the AFG range j=%d "
            "entnum=%d index=%d res=0x%08x\n",
            nid, cnid, j, entnum, i, res);
    }
    if (CONN_RANGE(res, entnum, j) == 0)
        addcnid = cnid;
    else if (prevcnid == 0 || prevcnid >= cnid) {
    HDA_DBG(HDA_DBG_INFO, 
            "WARNING: Invalid child range "
            "nid=%d index=%d j=%d entnum=%d "
            "prevcnid=%d cnid=%d res=0x%08x\n",
            nid, i, j, entnum, prevcnid,
            cnid, res);
    addcnid = cnid;
    } else
        addcnid = prevcnid + 1;
    while (addcnid <= cnid) {
    if (w->nconns > max) {
    HDA_DBG(HDA_DBG_INFO, 
            "Adding %d (nid=%d): "
            "Max connection reached! max=%d\n",
            addcnid, nid, max + 1);
    goto getconns_out;
    }
    w->connsenable[w->nconns] = 1;
    w->conns[w->nconns++] = addcnid++;
    }
    prevcnid = cnid;
    }
    }

    getconns_out:
    return;
    }


LOCAL void widget_parse
    (
    WIDGET *w
    )
    {
    UINT32 wcap, cap;
    nid_t nid = w->nid;
    
    wcap = hda_command(w->codec, HDA_CMD_GET_PARAMETER(0, nid, HDA_PARAM_AUDIO_WIDGET_CAP));
    w->param.widget_cap = wcap;

    w->type = HDA_PARAM_AUDIO_WIDGET_CAP_TYPE(wcap);
    
    widget_connection_parse(w);

    if (HDA_PARAM_AUDIO_WIDGET_CAP_OUT_AMP(wcap))
        {
        if (HDA_PARAM_AUDIO_WIDGET_CAP_AMP_OVR(wcap))
            w->param.outamp_cap =  hda_command(w->codec, HDA_CMD_GET_PARAMETER(0, nid, HDA_PARAM_OUTPUT_AMP_CAP));
        else
            w->param.outamp_cap = w->codec->outamp_cap;
        }
    else
        w->param.outamp_cap = 0;

    if (HDA_PARAM_AUDIO_WIDGET_CAP_IN_AMP(wcap))
        {
        if (HDA_PARAM_AUDIO_WIDGET_CAP_AMP_OVR(wcap))
            w->param.inamp_cap = hda_command(w->codec, HDA_CMD_GET_PARAMETER(0, nid, HDA_PARAM_INPUT_AMP_CAP));
        else
            w->param.inamp_cap = w->codec->inamp_cap;
        }
    else
        w->param.inamp_cap = 0;

    if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_OUTPUT ||
        w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT)
        {
        if (HDA_PARAM_AUDIO_WIDGET_CAP_FORMAT_OVR(wcap))
            {
            cap = hda_command(w->codec, HDA_CMD_GET_PARAMETER(0, nid, HDA_PARAM_SUPP_STREAM_FORMATS));
            w->param.supp_stream_formats = (cap != 0) ? cap : w->codec->supp_stream_formats;
            cap = hda_command(w->codec, HDA_CMD_GET_PARAMETER(0, nid, HDA_PARAM_SUPP_PCM_SIZE_RATE));
            w->param.supp_pcm_size_rate = (cap != 0) ? cap :  w->codec->supp_pcm_size_rate;
            }
        else
            {
            w->param.supp_stream_formats = w->codec->supp_stream_formats;
            w->param.supp_pcm_size_rate = w->codec->supp_pcm_size_rate;
            }
        if (HDA_PARAM_AUDIO_WIDGET_CAP_STRIPE(w->param.widget_cap))
            {
            w->wclass.conv.stripecap = hda_command(w->codec,
                                                   HDA_CMD_GET_STRIPE_CONTROL(0, w->nid)) >> 20;
            }
        else
            w->wclass.conv.stripecap = 1;
        }
    else
        {
        w->param.supp_stream_formats = 0;
        w->param.supp_pcm_size_rate = 0;
        }

    if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
        {
        w->wclass.pin.config = hda_command(w->codec, HDA_CMD_GET_CONFIGURATION_DEFAULT(0, w->nid));
        w->wclass.pin.original = w->wclass.pin.config;
        w->wclass.pin.newconf = w->wclass.pin.config;
        w->wclass.pin.cap = hda_command(w->codec, HDA_CMD_GET_PARAMETER(0, w->nid, HDA_PARAM_PIN_CAP));;
        w->wclass.pin.ctrl = hda_command(w->codec, HDA_CMD_GET_PIN_WIDGET_CTRL(0, nid));

        w->param.eapdbtl = HDA_INVALID;
        if (HDA_PARAM_PIN_CAP_EAPD_CAP(w->wclass.pin.cap))
            {
            w->param.eapdbtl = hda_command(w->codec,
                                           HDA_CMD_GET_EAPD_BTL_ENABLE(0, nid));
            w->param.eapdbtl &= 0x7;
            w->param.eapdbtl |= HDA_CMD_SET_EAPD_BTL_ENABLE_EAPD;
            }
        }
    
    w->unsol = -1;
    }



LOCAL AUDIO_CTL* audio_ctl_each
    (
    HDCODEC_ID codec,
    int *index
    )
    {
    if (codec == NULL ||
        index == NULL || codec->ctl == NULL ||
        codec->ctlcnt < 1 ||
        *index < 0 || *index >= codec->ctlcnt)
        return (NULL);
    return (&codec->ctl[(*index)++]);
    }

LOCAL AUDIO_CTL* audio_ctl_amp_get
    (
    HDCODEC_ID codec,
    nid_t nid, int dir,
    int index, int cnt
    )
    {
    AUDIO_CTL *ctl;
    int i, found = 0;

    if (codec == NULL || codec->ctl == NULL)
        return (NULL);

    i = 0;
    while ((ctl = audio_ctl_each(codec, &i)) != NULL) {
    if (ctl->enable == 0)
        continue;
    if (ctl->widget->nid != nid)
        continue;
    if (dir && ctl->ndir != dir)
        continue;
    if (index >= 0 && ctl->ndir == CTL_IN &&
        ctl->dir == ctl->ndir && ctl->index != index)
        continue;
    found++;
    if (found == cnt || cnt <= 0)
        return (ctl);
    }

    return (NULL);
    }

LOCAL void audio_ctl_parse (HDCODEC_ID codec)
    {
    AUDIO_CTL *ctls;
    WIDGET *w, *cw;
    int i, j, cnt, max, ocap, icap;
    int mute, offset, step, size;

    /* XXX This is redundant */
    max = 0;
    for (i = codec->startnode; i < codec->endnode; i++)
        {
        w = widget_get(codec, i);
        if (w == NULL || w->enable == 0)
            continue;
        if (w->param.outamp_cap != 0)
            max++;
        if (w->param.inamp_cap != 0)
            {
            switch (w->type)
                {
                case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_SELECTOR:
                case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER:
                    for (j = 0; j < w->nconns; j++)
                        {
                        cw = widget_get(codec,
                                             w->conns[j]);
                        if (cw == NULL || cw->enable == 0)
                            continue;
                        max++;
                        }
                    break;
                default:
                    max++;
                    break;
                }
            }
        }
    codec->ctlcnt = max;

    if (max < 1)
        return;

    ctls = (AUDIO_CTL *)calloc(max, sizeof(*ctls));

    if (ctls == NULL)
        {
        /* Blekh! */
        HDA_DBG(HDA_DBG_ERR, "unable to allocate ctls!\n");
        codec->ctlcnt = 0;
        return;
        }

    cnt = 0;
    for (i = codec->startnode; cnt < max && i < codec->endnode; i++)
        {
        if (cnt >= max)
            {
            HDA_DBG(HDA_DBG_ERR, "%s: Ctl overflow!\n", __func__);
            break;
            }
        w = widget_get(codec, i);
        if (w == NULL || w->enable == 0)
            continue;
        ocap = w->param.outamp_cap;
        icap = w->param.inamp_cap;
        if (ocap != 0)
            {
            mute = HDA_PARAM_OUTPUT_AMP_CAP_MUTE_CAP(ocap);
            step = HDA_PARAM_OUTPUT_AMP_CAP_NUMSTEPS(ocap);
            size = HDA_PARAM_OUTPUT_AMP_CAP_STEPSIZE(ocap);
            offset = HDA_PARAM_OUTPUT_AMP_CAP_OFFSET(ocap);
            /*if (offset > step)
              {
              HDA_DBG(HDA_DBG_ERR, 
              "BUGGY outamp: nid=%d "
              "[offset=%d > step=%d]\n",
              w->nid, offset, step);
              offset = step;
              }*/
            ctls[cnt].enable = 1;
            ctls[cnt].widget = w;
            ctls[cnt].mute = mute;
            ctls[cnt].step = step;
            ctls[cnt].size = size;
            ctls[cnt].offset = offset;
            ctls[cnt].left = offset;
            ctls[cnt].right = offset;
            if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX ||
                w->waspin)
                ctls[cnt].ndir = CTL_IN;
            else 
                ctls[cnt].ndir = CTL_OUT;
            ctls[cnt++].dir = CTL_OUT;
            }

        if (icap != 0)
            {
            mute = HDA_PARAM_OUTPUT_AMP_CAP_MUTE_CAP(icap);
            step = HDA_PARAM_OUTPUT_AMP_CAP_NUMSTEPS(icap);
            size = HDA_PARAM_OUTPUT_AMP_CAP_STEPSIZE(icap);
            offset = HDA_PARAM_OUTPUT_AMP_CAP_OFFSET(icap);
            /*if (offset > step)
              {
              HDA_DBG(HDA_DBG_ERR, 
              "BUGGY inamp: nid=%d "
              "[offset=%d > step=%d]\n",
              w->nid, offset, step);
              offset = step;
              }*/
            switch (w->type)
                {
                case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_SELECTOR:
                case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER:
                    for (j = 0; j < w->nconns; j++)
                        {
                        if (cnt >= max)
                            {
                            HDA_DBG(HDA_DBG_ERR, "%s: Ctl overflow!\n", __func__);
                            break;
                            }
                        cw = widget_get(codec,
                                             w->conns[j]);
                        if (cw == NULL || cw->enable == 0)
                            continue;
                        ctls[cnt].enable = 1;
                        ctls[cnt].widget = w;
                        ctls[cnt].childwidget = cw;
                        ctls[cnt].index = j;
                        ctls[cnt].mute = mute;
                        ctls[cnt].step = step;
                        ctls[cnt].size = size;
                        ctls[cnt].offset = offset;
                        ctls[cnt].left = offset;
                        ctls[cnt].right = offset;
                        ctls[cnt].ndir = CTL_IN;
                        ctls[cnt++].dir = CTL_IN;
                        }
                    break;
                default:
                    if (cnt >= max)
                        {
                        HDA_DBG(HDA_DBG_ERR, "%s: Ctl overflow!\n", __func__);
                        break;
                        }
                    ctls[cnt].enable = 1;
                    ctls[cnt].widget = w;
                    ctls[cnt].mute = mute;
                    ctls[cnt].step = step;
                    ctls[cnt].size = size;
                    ctls[cnt].offset = offset;
                    ctls[cnt].left = offset;
                    ctls[cnt].right = offset;
                    if (w->type ==
                        HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
                        ctls[cnt].ndir = CTL_OUT;
                    else 
                        ctls[cnt].ndir = CTL_IN;
                    ctls[cnt++].dir = CTL_IN;
                    break;
                }
            }
        }

    codec->ctl = ctls;
    }

LOCAL void audio_disable_nonaudio(HDCODEC_ID codec)
    {
    WIDGET *w;
    int i;

    /* Disable power and volume widgets. */
    for (i = codec->startnode; i < codec->endnode; i++)
        {
        w = widget_get(codec, i);
        if (w == NULL || w->enable == 0)
            continue;
        if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_POWER_WIDGET ||
            w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_VOLUME_WIDGET)
            {
            w->enable = 0;
            HDA_DBG(HDA_DBG_INFO, 
                    " Disabling nid %d due to it's"
                    " non-audio type.\n",
                    w->nid);
            }
        }
    }

LOCAL void audio_disable_useless(HDCODEC_ID codec)
    {
    WIDGET *w, *cw;
    AUDIO_CTL *ctl;
    int done, found, i, j, k;

    /* Disable useless pins. */
    for (i = codec->startnode; i < codec->endnode; i++)
        {
        w = widget_get(codec, i);
        if (w == NULL || w->enable == 0)
            continue;
        if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
            {
            if ((w->wclass.pin.config &
                 HDA_CONFIG_DEFAULTCONF_CONNECTIVITY_MASK) ==
                HDA_CONFIG_DEFAULTCONF_CONNECTIVITY_NONE)
                {
                w->enable = 0;
                HDA_DBG(HDA_DBG_INFO, 
                        " Disabling pin nid %d due"
                        " to None connectivity.\n",
                        w->nid);
                } else if ((w->wclass.pin.config &
                            HDA_CONFIG_DEFAULTCONF_ASSOCIATION_MASK) == 0)
                {
                w->enable = 0;
                HDA_DBG(HDA_DBG_INFO, 
                        " Disabling unassociated"
                        " pin nid %d.\n",
                        w->nid);
                }
            }
        }
    do
        {
        done = 1;
        /* Disable and mute controls for disabled widgets. */
        i = 0;
        while ((ctl = audio_ctl_each(codec, &i)) != NULL)
            {
            if (ctl->enable == 0)
                continue;
            if (ctl->widget->enable == 0 ||
                (ctl->childwidget != NULL &&
                 ctl->childwidget->enable == 0))
                {
                ctl->forcemute = 1;
                ctl->muted = HDAA_AMP_MUTE_ALL;
                ctl->left = 0;
                ctl->right = 0;
                ctl->enable = 0;
                if (ctl->ndir == CTL_IN)
                    ctl->widget->connsenable[ctl->index] = 0;
                done = 0;
                HDA_DBG(HDA_DBG_INFO, 
                        " Disabling ctl %d nid %d cnid %d due"
                        " to disabled widget.\n", i,
                        ctl->widget->nid,
                        (ctl->childwidget != NULL)?
                        ctl->childwidget->nid:-1);
                }
            }
        /* Disable useless widgets. */
        for (i = codec->startnode; i < codec->endnode; i++)
            {
            w = widget_get(codec, i);
            if (w == NULL || w->enable == 0)
                continue;
            /* Disable inputs with disabled child widgets. */
            for (j = 0; j < w->nconns; j++)
                {
                if (w->connsenable[j])
                    {
                    cw = widget_get(codec, w->conns[j]);
                    if (cw == NULL || cw->enable == 0)
                        {
                        w->connsenable[j] = 0;
                        HDA_DBG(HDA_DBG_INFO, 
                                " Disabling nid %d connection %d due"
                                " to disabled child widget.\n",
                                i, j);
                        }
                    }
                }
            if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_SELECTOR &&
                w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER)
                continue;
            /* Disable mixers and selectors without inputs. */
            found = 0;
            for (j = 0; j < w->nconns; j++)
                {
                if (w->connsenable[j])
                    {
                    found = 1;
                    break;
                    }
                }
            if (found == 0)
                {
                w->enable = 0;
                done = 0;
                HDA_DBG(HDA_DBG_INFO, 
                        " Disabling nid %d due to all it's"
                        " inputs disabled.\n", w->nid);
                }
            /* Disable nodes without consumers. */
            if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_SELECTOR &&
                w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER)
                continue;
            found = 0;
            for (k = codec->startnode; k < codec->endnode; k++)
                {
                cw = widget_get(codec, k);
                if (cw == NULL || cw->enable == 0)
                    continue;
                for (j = 0; j < cw->nconns; j++)
                    {
                    if (cw->connsenable[j] && cw->conns[j] == i)
                        {
                        found = 1;
                        break;
                        }
                    }
                }
            if (found == 0) {
            w->enable = 0;
            done = 0;
            HDA_DBG(HDA_DBG_INFO, 
                    " Disabling nid %d due to all it's"
                    " consumers disabled.\n", w->nid);
            }
            }
        } while (done == 0);

    }

LOCAL void audio_disable_unas(HDCODEC_ID codec)
    {
    ASSOC *as = codec->assoc_table;
    WIDGET *w, *cw;
    AUDIO_CTL *ctl;
    int i, j, k;

    /* Disable unassosiated widgets. */
    for (i = codec->startnode; i < codec->endnode; i++) {
    w = widget_get(codec, i);
    if (w == NULL || w->enable == 0)
        continue;
    if (w->bindas == -1) {
    w->enable = 0;
    HDA_DBG(HDA_DBG_INFO, 
            " Disabling unassociated nid %d.\n",
            w->nid);
    }
    }
    /* Disable input connections on input pin and
     * output on output. */
    for (i = codec->startnode; i < codec->endnode; i++) {
    w = widget_get(codec, i);
    if (w == NULL || w->enable == 0)
        continue;
    if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
        continue;
    if (w->bindas < 0)
        continue;
    if (as[w->bindas].dir == CTL_IN) {
    for (j = 0; j < w->nconns; j++) {
    if (w->connsenable[j] == 0)
        continue;
    w->connsenable[j] = 0;
    HDA_DBG(HDA_DBG_INFO, 
            " Disabling connection to input pin "
            "nid %d conn %d.\n",
            i, j);
    }
    ctl = audio_ctl_amp_get(codec, w->nid,
                                 CTL_IN, -1, 1);
    if (ctl && ctl->enable) {
    ctl->forcemute = 1;
    ctl->muted = HDAA_AMP_MUTE_ALL;
    ctl->left = 0;
    ctl->right = 0;
    ctl->enable = 0;
    }
    } else {
    ctl = audio_ctl_amp_get(codec, w->nid,
                                 CTL_OUT, -1, 1);
    if (ctl && ctl->enable) {
    ctl->forcemute = 1;
    ctl->muted = HDAA_AMP_MUTE_ALL;
    ctl->left = 0;
    ctl->right = 0;
    ctl->enable = 0;
    }
    for (k = codec->startnode; k < codec->endnode; k++) {
    cw = widget_get(codec, k);
    if (cw == NULL || cw->enable == 0)
        continue;
    for (j = 0; j < cw->nconns; j++) {
    if (cw->connsenable[j] && cw->conns[j] == i) {
    cw->connsenable[j] = 0;
    HDA_DBG(HDA_DBG_INFO, 
            " Disabling connection from output pin "
            "nid %d conn %d cnid %d.\n",
            k, j, i);
    if (cw->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX &&
        cw->nconns > 1)
        continue;
    ctl = audio_ctl_amp_get(codec, k,
                                 CTL_IN, j, 1);
    if (ctl && ctl->enable) {
    ctl->forcemute = 1;
    ctl->muted = HDAA_AMP_MUTE_ALL;
    ctl->left = 0;
    ctl->right = 0;
    ctl->enable = 0;
    }
    }
    }
    }
    }
    }
    }

LOCAL void audio_disable_notselected(HDCODEC_ID codec)
    {
    ASSOC *as = codec->assoc_table;
    WIDGET *w;
    int i, j;

    /* On playback path we can safely disable all unseleted inputs. */
    for (i = codec->startnode; i < codec->endnode; i++) {
    w = widget_get(codec, i);
    if (w == NULL || w->enable == 0)
        continue;
    if (w->nconns <= 1)
        continue;
    if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER)
        continue;
    if (w->bindas < 0 || as[w->bindas].dir == CTL_IN)
        continue;
    for (j = 0; j < w->nconns; j++) {
    if (w->connsenable[j] == 0)
        continue;
    if (w->selconn < 0 || w->selconn == j)
        continue;
    w->connsenable[j] = 0;
    HDA_DBG(HDA_DBG_INFO, 
            " Disabling unselected connection "
            "nid %d conn %d.\n",
            i, j);
    }
    }
    }

LOCAL void audio_disable_crossas(HDCODEC_ID codec)
    {
    ASSOC *ases = codec->assoc_table;
    WIDGET *w, *cw;
    AUDIO_CTL *ctl;
    int i, j;

    /* Disable crossassociatement and unwanted crosschannel connections. */
    /* ... using selectors */
    for (i = codec->startnode; i < codec->endnode; i++) {
    w = widget_get(codec, i);
    if (w == NULL || w->enable == 0)
        continue;
    if (w->nconns <= 1)
        continue;
    if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER)
        continue;
    /* Allow any -> mix */
    if (w->bindas == -2)
        continue;
    for (j = 0; j < w->nconns; j++) {
    if (w->connsenable[j] == 0)
        continue;
    cw = widget_get(codec, w->conns[j]);
    if (cw == NULL || w->enable == 0)
        continue;
    /* Allow mix -> out. */
    if (cw->bindas == -2 && w->bindas >= 0 &&
        ases[w->bindas].dir == CTL_OUT)
        continue;
    /* Allow mix -> mixed-in. */
    if (cw->bindas == -2 && w->bindas >= 0 &&
        ases[w->bindas].mixed)
        continue;
    /* Allow in -> mix. */
    if ((w->pflags & HDAA_ADC_MONITOR) &&
        cw->bindas >= 0 &&
        ases[cw->bindas].dir == CTL_IN)
        continue;
    /* Allow if have common as/seqs. */
    if (w->bindas == cw->bindas &&
        (w->bindseqmask & cw->bindseqmask) != 0)
        continue;
    w->connsenable[j] = 0;
    HDA_DBG(HDA_DBG_INFO, 
            " Disabling crossassociatement connection "
            "nid %d conn %d cnid %d.\n",
            i, j, cw->nid);
    }
    }
    /* ... using controls */
    i = 0;
    while ((ctl = audio_ctl_each(codec, &i)) != NULL) {
    if (ctl->enable == 0 || ctl->childwidget == NULL)
        continue;
    /* Allow any -> mix */
    if (ctl->widget->bindas == -2)
        continue;
    /* Allow mix -> out. */
    if (ctl->childwidget->bindas == -2 &&
        ctl->widget->bindas >= 0 &&
        ases[ctl->widget->bindas].dir == CTL_OUT)
        continue;
    /* Allow mix -> mixed-in. */
    if (ctl->childwidget->bindas == -2 &&
        ctl->widget->bindas >= 0 &&
        ases[ctl->widget->bindas].mixed)
        continue;
    /* Allow in -> mix. */
    if ((ctl->widget->pflags & HDAA_ADC_MONITOR) &&
        ctl->childwidget->bindas >= 0 &&
        ases[ctl->childwidget->bindas].dir == CTL_IN)
        continue;
    /* Allow if have common as/seqs. */
    if (ctl->widget->bindas == ctl->childwidget->bindas &&
        (ctl->widget->bindseqmask & ctl->childwidget->bindseqmask) != 0)
        continue;
    ctl->forcemute = 1;
    ctl->muted = HDAA_AMP_MUTE_ALL;
    ctl->left = 0;
    ctl->right = 0;
    ctl->enable = 0;
    if (ctl->ndir == CTL_IN)
        ctl->widget->connsenable[ctl->index] = 0;
    HDA_DBG(HDA_DBG_INFO, 
            " Disabling crossassociatement connection "
            "ctl %d nid %d cnid %d.\n", i,
            ctl->widget->nid,
            ctl->childwidget->nid);
    }

    }

void audio_as_parse(HDCODEC_ID codec)
    {
    ASSOC *as;
    WIDGET *w;
    int i, j, cnt, max, type, dir, assoc, seq, first, hpredir;

    /* Count present associations */
    max = 0;
    for (j = 1; j < 16; j++) {
    for (i = codec->startnode; i < codec->endnode; i++) {
    w = widget_get(codec, i);
    if (w == NULL || w->enable == 0)
        continue;
    if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
        continue;
    if (HDA_CONFIG_DEFAULTCONF_ASSOCIATION(w->wclass.pin.config)
        != j)
        continue;
    max++;
    if (j != 15)  /* There could be many 1-pin assocs #15 */
        break;
    }
    }


    codec->ascnt = max;

    if (max < 1)
        return;

    as = (ASSOC*)calloc(max, sizeof(ASSOC));

    if (as == NULL) {
    /* Blekh! */
    HDA_DBG(HDA_DBG_ERR, "unable to allocate assocs!\n");
    codec->ascnt = 0;
    return;
    }
    else
        {
        /* clear as contents */
        memset(as, 0, sizeof(ASSOC));
        }

    for (i = 0; i < max; i++) {
    as[i].hpredir = -1;
    as[i].digital = 0;
    as[i].num_chans = 1;
    as[i].location = -1;
    }

    /* Scan associations skipping as=0. */
    cnt = 0;
    for (j = 1; j < 16; j++) {
    first = 16;
    hpredir = 0;
    for (i = codec->startnode; i < codec->endnode; i++) {
    w = widget_get(codec, i);
    if (w == NULL || w->enable == 0)
        continue;
    if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
        continue;
    assoc = HDA_CONFIG_DEFAULTCONF_ASSOCIATION(w->wclass.pin.config);
    seq = HDA_CONFIG_DEFAULTCONF_SEQUENCE(w->wclass.pin.config);
    if (assoc != j) {
    continue;
    }

    type = w->wclass.pin.config &
        HDA_CONFIG_DEFAULTCONF_DEVICE_MASK;
    /* Get pin direction. */
    if (type == HDA_CONFIG_DEFAULTCONF_DEVICE_LINE_OUT ||
        type == HDA_CONFIG_DEFAULTCONF_DEVICE_SPEAKER ||
        type == HDA_CONFIG_DEFAULTCONF_DEVICE_HP_OUT ||
        type == HDA_CONFIG_DEFAULTCONF_DEVICE_SPDIF_OUT ||
        type == HDA_CONFIG_DEFAULTCONF_DEVICE_DIGITAL_OTHER_OUT)
        dir = CTL_OUT;
    else
        dir = CTL_IN;
    /* If this is a first pin - create new association. */
    if (as[cnt].pincnt == 0) {
    as[cnt].enable = 1;
    as[cnt].index = j;
    as[cnt].dir = dir;
    }
    if (seq < first)
        first = seq;
    /* Check association correctness. */
    if (as[cnt].pins[seq] != 0) {
    HDA_DBG(HDA_DBG_INFO, "%s: Duplicate pin %d (%d) "
            "in association %d! Disabling association.\n",
            __func__, seq, w->nid, j);
    as[cnt].enable = 0;
    }
    if (dir != as[cnt].dir) {
    HDA_DBG(HDA_DBG_INFO, "%s: Pin %d has wrong "
            "direction for association %d! Disabling "
            "association.\n",
            __func__, w->nid, j);
    as[cnt].enable = 0;
    }
    if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(w->param.widget_cap)) {
    as[cnt].digital |= 0x1;
    if (HDA_PARAM_PIN_CAP_HDMI(w->wclass.pin.cap))
        as[cnt].digital |= 0x2;
    if (HDA_PARAM_PIN_CAP_DP(w->wclass.pin.cap))
        as[cnt].digital |= 0x4;
    }
    if (as[cnt].location == -1) {
    as[cnt].location =
        HDA_CONFIG_DEFAULTCONF_LOCATION(w->wclass.pin.config);
    } else if (as[cnt].location !=
               HDA_CONFIG_DEFAULTCONF_LOCATION(w->wclass.pin.config)) {
    as[cnt].location = -2;
    }
    /* Headphones with seq=15 may mean redirection. */
    if (type == HDA_CONFIG_DEFAULTCONF_DEVICE_HP_OUT &&
        seq == 15)
        hpredir = 1;
    as[cnt].pins[seq] = w->nid;
    as[cnt].pincnt++;
    /* Association 15 is a multiple unassociated pins. */
    if (j == 15)
        cnt++;
    }
    if (j != 15 && as[cnt].pincnt > 0) {
    if (hpredir && as[cnt].pincnt > 1)
        as[cnt].hpredir = first;
    cnt++;
    }
    }
    for (i = 0; i < max; i++) {
    if (as[i].dir == CTL_IN && (as[i].pincnt == 1 ||
                                     as[i].pins[14] > 0 || as[i].pins[15] > 0))
        as[i].mixed = 1;
    }

    HDA_DBG(HDA_DBG_INFO, "%d associations found:\n", max);
    for (i = 0; i < max; i++)
        {

        HDA_DBG(HDA_DBG_INFO, "Association %d (%d) %s%s:\n", i, as[i].index,
                (as[i].dir == CTL_IN) ? "in":"out", as[i].enable ? "" : " (disabled)");

        for (j = 0; j < 16; j++)
            {
            if (as[i].pins[j] == 0)
                continue;
            HDA_DBG(HDA_DBG_INFO, " Pin nid=%d seq=%d\n", as[i].pins[j], j);
            }
        }

    codec->assoc_table = as;

    }


LOCAL int pcmchannel_setup(CHAN *ch)
    {
    HDCODEC_ID codec = ch->codec;
    ASSOC *as = codec->assoc_table;
    WIDGET *w;
    UINT32 cap, fmtcap, pcmcap;
    int i, j, ret, channels, onlystereo;
    UINT16 pinset;

    ch->caps = caps;
    ch->caps.fmtlist = ch->fmtlist;
    ch->bit16 = 1;
    ch->bit32 = 0;
    ch->pcmrates[0] = 48000;
    ch->pcmrates[1] = 0;
    ch->stripecap = 0xff;

    ret = 0;
    channels = 0;
    onlystereo = 1;
    pinset = 0;
    fmtcap = codec->supp_stream_formats;
    pcmcap = codec->supp_pcm_size_rate;

    for (i = 0; i < 16; i++) {
    
    /* Check as is correct */
    if (ch->as < 0)
        break;
        
    /* Cound only present DACs */
    if (as[ch->as].dacs[ch->asindex][i] <= 0)
        continue;
        
    /* Ignore duplicates */
    for (j = 0; j < ret; j++) {
    if (ch->io[j] == as[ch->as].dacs[ch->asindex][i])
        break;
    }
    if (j < ret)
        continue;

    w = widget_get(codec, as[ch->as].dacs[ch->asindex][i]);
    if (w == NULL || w->enable == 0)
        continue;
    cap = w->param.supp_stream_formats;

    if (ret == 0) {
    fmtcap = cap;
    pcmcap = w->param.supp_pcm_size_rate;
    } else {
    fmtcap &= cap;
    pcmcap &= w->param.supp_pcm_size_rate;
    }

    ch->io[ret++] = as[ch->as].dacs[ch->asindex][i];
    ch->stripecap &= w->wclass.conv.stripecap;
    /* Do not count redirection pin/dac channels. */
    if (i == 15 && as[ch->as].hpredir >= 0)
        continue;
    channels += HDA_PARAM_AUDIO_WIDGET_CAP_CC(w->param.widget_cap) + 1;
    if (HDA_PARAM_AUDIO_WIDGET_CAP_CC(w->param.widget_cap) != 1)
        onlystereo = 0;
    pinset |= (1 << i);
    }
    ch->io[ret] = -1;
    ch->channels = channels;

    if (as[ch->as].fakeredir)
        ret--;
    /* Standard speaks only about stereo pins and playback, ... */
    if ((!onlystereo) || as[ch->as].mixed)
        pinset = 0;
    /* ..., but there it gives us info about speakers layout. */
    as[ch->as].pinset = pinset;

    ch->supp_stream_formats = fmtcap;
    ch->supp_pcm_size_rate = pcmcap;

    /*
     *  8bit = 0
     * 16bit = 1
     * 20bit = 2
     * 24bit = 3
     * 32bit = 4
     */
    if (ret > 0) {
    i = 0;
    if (HDA_PARAM_SUPP_STREAM_FORMATS_PCM(fmtcap)) {
    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_16BIT(pcmcap))
        ch->bit16 = 1;
    else if (HDA_PARAM_SUPP_PCM_SIZE_RATE_8BIT(pcmcap))
        ch->bit16 = 0;
    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_24BIT(pcmcap))
        ch->bit32 = 3;
    else if (HDA_PARAM_SUPP_PCM_SIZE_RATE_20BIT(pcmcap))
        ch->bit32 = 2;
    else if (HDA_PARAM_SUPP_PCM_SIZE_RATE_32BIT(pcmcap))
        ch->bit32 = 4;

    /* 8bit */
    ch->fmtlist[i++] = SND_FORMAT(AFMT_U8, 1, 0);
    ch->fmtlist[i++] = SND_FORMAT(AFMT_U8, 2, 0);

    /* mono */
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 1, 0);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 1, 0);

    if (channels >= 2) {
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 2, 0);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 2, 0);
    }
    if (channels >= 3 && !onlystereo) {
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 3, 0);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 3, 0);
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 3, 1);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 3, 1);
    }
    if (channels >= 4) {
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 4, 0);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 4, 0);
    if (!onlystereo) {
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 4, 1);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 4, 1);
    }
    }
    if (channels >= 5 && !onlystereo) {
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 5, 0);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 5, 0);
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 5, 1);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 5, 1);
    }
    if (channels >= 6) {
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 6, 1);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 6, 1);
    if (!onlystereo) {
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 6, 0);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 6, 0);
    }
    }
    if (channels >= 7 && !onlystereo) {
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 7, 0);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 7, 0);
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 7, 1);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 7, 1);
    }
    if (channels >= 8) {
    ch->fmtlist[i++] = SND_FORMAT(AFMT_S16_LE, 8, 1);
    if (ch->bit32)
        ch->fmtlist[i++] = SND_FORMAT(AFMT_S32_LE, 8, 1);
    }
    }

    ch->fmtlist[i] = 0;
    i = 0;
        
    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_8KHZ(pcmcap))
        ch->pcmrates[i++] = 8000;
    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_11KHZ(pcmcap))
        ch->pcmrates[i++] = 11025;
    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_16KHZ(pcmcap))
        ch->pcmrates[i++] = 16000;
    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_22KHZ(pcmcap))
        ch->pcmrates[i++] = 22050;
    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_32KHZ(pcmcap))
        ch->pcmrates[i++] = 32000;
    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_44KHZ(pcmcap))
        ch->pcmrates[i++] = 44100;

    ch->pcmrates[i++] = 48000;

    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_88KHZ(pcmcap))
        ch->pcmrates[i++] = 88200;
    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_96KHZ(pcmcap))
        ch->pcmrates[i++] = 96000;
    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_176KHZ(pcmcap))
        ch->pcmrates[i++] = 176400;
    if (HDA_PARAM_SUPP_PCM_SIZE_RATE_192KHZ(pcmcap))
        ch->pcmrates[i++] = 192000;

    ch->pcmrates[i] = 0;
    if (i > 0) {
    ch->caps.minspeed = ch->pcmrates[0];
    ch->caps.maxspeed = ch->pcmrates[i - 1];
    }
    }

    return (ret);
    }

LOCAL void prepare_pcms(HDCODEC_ID codec)
    {
    ASSOC *as = codec->assoc_table;
    int i, j, k, apdev = 0, ardev = 0, dpdev = 0, drdev = 0;

    for (i = 0; i < codec->ascnt; i++)
        {
        if (as[i].enable == 0)
            continue;
        if (as[i].dir == CTL_IN)
            {
            if (as[i].digital)
                drdev++;
            else
                ardev++;
            }
        else
            {
            if (as[i].digital)
                dpdev++;
            else
                apdev++;
            }
        }

    codec->num_devs = max(ardev, apdev) + max(drdev, dpdev);

    codec->pcm_dev_table = (PCM_DEVINFO *)calloc(codec->num_devs, sizeof(PCM_DEVINFO));
#ifdef  HDA_DBG_ON
    global_pcmdev = codec->pcm_dev_table;
#endif
    if (codec->pcm_dev_table == NULL)
        {
        HDA_DBG(HDA_DBG_ERR, 
                "Unable to allocate memory for devices\n");
        return;
        }

    /* pcm_dev_table entry default values */
    for (i = 0; i < codec->num_devs; i++)
        {
        codec->pcm_dev_table[i].index = i;
        codec->pcm_dev_table[i].codec = codec;
        codec->pcm_dev_table[i].playas = -1;
        codec->pcm_dev_table[i].recas = -1;
        codec->pcm_dev_table[i].digital = 255;
        codec->pcm_dev_table[i].chan_size = HDA_BUFSZ_DEFAULT;
        codec->pcm_dev_table[i].chan_blkcnt = HDA_BDL_MAX;
        }

    for (i = 0; i < codec->ascnt; i++)
        {
        if (as[i].enable == 0)
            continue;
        for (j = 0; j < codec->num_devs; j++)
            {
            if (codec->pcm_dev_table[j].digital != 255 &&
                (!codec->pcm_dev_table[j].digital) !=
                (!as[i].digital))
                continue;
            if (as[i].dir == CTL_IN)
                {
                if (codec->pcm_dev_table[j].recas < 0)
                    codec->pcm_dev_table[j].recas = i;
                else
                    continue;
                }
            else
                {
                if (codec->pcm_dev_table[j].playas < 0)
                    codec->pcm_dev_table[j].playas = i;
                else
                    continue;
                }
            as[i].pcm_dev = &codec->pcm_dev_table[j];
            for (k = 0; k < as[i].num_chans; k++)
                {
                int l = as[i].chans[k];
                HDA_DBG(HDA_DBG_INFO, "select hdaa_chan_table index l: as[%d].chans[%d] = hdaa_chan_table[%d].pcm_dev : pcm_dev_table[%d]\n", i, k, l, j);

                codec->hdaa_chan_table[l].pcm_dev = &codec->pcm_dev_table[j];
                }
            codec->pcm_dev_table[j].digital = as[i].digital;
            break;
            }
        }
    }

LOCAL void hdAudioOssDevDelete
    (
    HDCODEC_ID codec,
    PCM_DEVINFO *pdevinfo
    )
    {
    ossDeleteDsp(pdevinfo->pDspDev);
    ossDeleteMixer(pdevinfo->pMixerDev);
    }

LOCAL void hdAudioOssDevCreate
    (
    HDCODEC_ID codec,
    PCM_DEVINFO *pdevinfo
    )
    {
    ASSOC *as;
    CHAN *ch;
    int i;

    DSP_DEV * pDspDev;
    MIXER_DEV * pMixerDev;

    pDspDev = ossCreateDsp ();
    pdevinfo->pDspDev = pDspDev;
        
    if (pdevinfo->playas >= 0)
        {
        as = &codec->assoc_table[pdevinfo->playas];
        for (i = 0; i < as->num_chans; i++)
            {
            ch = &codec->hdaa_chan_table[as->chans[i]];
            osschannel_init (pDspDev, PCM_DIR_PLAY, ch);
            }
        }

    if (pdevinfo->recas >= 0)
        {
        as = &codec->assoc_table[pdevinfo->recas];
        for (i = 0; i < as->num_chans; i++)
            {
            ch = &codec->hdaa_chan_table[as->chans[i]];
            osschannel_init (pDspDev, PCM_DIR_REC, ch);
            }
        pdevinfo->autorecsrc = AUTO_SELECT_RECORD_SOURCE_ENABLED;
        }

    pMixerDev = ossCreateMixer ();
    pdevinfo->pMixerDev = pMixerDev;
    
    ossmixer_init (pMixerDev, pdevinfo);

    audio_ctl_set_defaults (pdevinfo);
    }
    

LOCAL void create_pcms(HDCODEC_ID codec)
    {
    int i;
    PCM_DEVINFO *pdevinfo;

    for (i = 0; i < codec->num_devs; i++)
        {
        pdevinfo = &codec->pcm_dev_table[i];
        hdAudioOssDevCreate (codec, pdevinfo);
        }
    }

LOCAL void delete_pcms(HDCODEC_ID codec)
    {
    int i;
    PCM_DEVINFO *pdevinfo;

    for (i = 0; i < codec->num_devs; i++)
        {
        pdevinfo = &codec->pcm_dev_table[i];
        hdAudioOssDevDelete (codec, pdevinfo);
        }
    }

/*
 * Bind associations to PCM channels
 */
LOCAL void audio_bind_as(HDCODEC_ID codec)
    {
    ASSOC *as = codec->assoc_table;
    int i, j, cnt = 0, free;

    for (j = 0; j < codec->ascnt; j++) {
    if (as[j].enable)
        cnt += as[j].num_chans;
    }
    if (codec->num_hdaa_chans == 0) {
    codec->hdaa_chan_table = (CHAN *)calloc(cnt, sizeof(CHAN));
    if (codec->hdaa_chan_table == NULL) {
    HDA_DBG(HDA_DBG_ERR, 
            "Channels memory allocation failed!\n");
    return;
    }
    } else {
    codec->hdaa_chan_table = (CHAN *)realloc(codec->hdaa_chan_table,
                                                         sizeof(CHAN) * (codec->num_hdaa_chans + cnt));
    if (codec->hdaa_chan_table == NULL) {
    codec->num_hdaa_chans = 0;
    HDA_DBG(HDA_DBG_ERR, 
            "Channels memory allocation failed!\n");
    return;
    }
    /* Fixup relative pointers after realloc */
    for (j = 0; j < codec->num_hdaa_chans; j++)
        codec->hdaa_chan_table[j].caps.fmtlist = codec->hdaa_chan_table[j].fmtlist;
    }
    free = codec->num_hdaa_chans;
    codec->num_hdaa_chans += cnt;

    for (j = free; j < free + cnt; j++) {
    codec->hdaa_chan_table[j].codec = codec;
    codec->hdaa_chan_table[j].as = -1;
    }

    /* Assign associations in order of their numbers, */
    for (j = 0; j < codec->ascnt; j++) {
    if (as[j].enable == 0)
        continue;
    for (i = 0; i < as[j].num_chans; i++) {
    codec->hdaa_chan_table[free].as = j;
    codec->hdaa_chan_table[free].asindex = i;
    codec->hdaa_chan_table[free].dir = as[j].dir;
    pcmchannel_setup(&codec->hdaa_chan_table[free]);
    as[j].chans[i] = free;
    free++;
    }
    }
    }

/*
 * Trace path from widget to ADC.
 */
LOCAL nid_t audio_trace_adc
    (
    HDCODEC_ID codec,
    int as, int seq, nid_t nid,
    int mixed, int min, int only,
    int depth, int *length, int onlylength
    )
    {
    WIDGET *w, *wc;
    int i, j, im, lm = HDA_PARSE_MAXDEPTH;
    nid_t m = 0, ret;

    if (depth > HDA_PARSE_MAXDEPTH)
        return (0);
    w = widget_get(codec, nid);
    if (w == NULL || w->enable == 0)
        return (0);
    HDA_DBG(HDA_DBG_INFO, 
            " %*stracing via nid %d\n",
            depth + 1, "", w->nid);

    /* Use only unused widgets */
    if (w->bindas >= 0 && w->bindas != as) {
    HDA_DBG(HDA_DBG_INFO, 
            " %*snid %d busy by association %d\n",
            depth + 1, "", w->nid, w->bindas);
    return (0);
    }
    if (!mixed && w->bindseqmask != 0) {
    HDA_DBG(HDA_DBG_INFO, 
            " %*snid %d busy by seqmask %x\n",
            depth + 1, "", w->nid, w->bindseqmask);
    return (0);
    }
    switch (w->type) {
    case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT:
        if ((only == 0 || only == w->nid) && (w->nid >= min) &&
            (onlylength == 0 || onlylength == depth)) {
        m = w->nid;
        if (length != NULL)
            *length = depth;
        }
        break;
    case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX:
        if (depth > 0)
            break;
        /* Fall */
    default:
        /* Try to find reachable ADCs with specified nid. */
        for (j = codec->startnode; j < codec->endnode; j++) {
        wc = widget_get(codec, j);
        if (wc == NULL || wc->enable == 0)
            continue;
        im = -1;
        for (i = 0; i < wc->nconns; i++) {
        if (wc->connsenable[i] == 0)
            continue;
        if (wc->conns[i] != nid)
            continue;
        if ((ret = audio_trace_adc(codec, as, seq,
                                        j, mixed, min, only, depth + 1,
                                        length, onlylength)) != 0) {
        if (m == 0 || ret < m ||
            (ret == m && length != NULL &&
             *length < lm)) {
        m = ret;
        im = i;
        lm = *length;
        }
        if (only)
            break;
        }
        }
        if (im >= 0 && only && ((wc->nconns > 1 &&
                                 wc->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER) ||
                                wc->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_SELECTOR))
            wc->selconn = im;
        }
        break;
    }
    if (m && only) {
    w->bindas = as;
    w->bindseqmask |= (1 << seq);
    }
    HDA_DBG(HDA_DBG_INFO, 
            " %*snid %d returned %d\n",
            depth + 1, "", w->nid, m);
    return (m);
    }

/*
 * Trace path from DAC to pin.
 */
LOCAL nid_t audio_trace_dac
    (
    HDCODEC_ID codec,
    int as,
    int seq,
    nid_t nid,
    int dupseq,
    int min,
    int only,
    int depth
    )
    {
    WIDGET *w;
    int i, im = -1;
    nid_t m = 0, ret;

    if (depth > HDA_PARSE_MAXDEPTH)
        return (0);
    w = widget_get(codec, nid);
    if (w == NULL || w->enable == 0)
        return (0);
    if (!only) {
    HDA_DBG(HDA_DBG_INFO, 
            " %*stracing via nid %d\n",
            depth + 1, "", w->nid);
    }
    /* Use only unused widgets */
    if (w->bindas >= 0 && w->bindas != as) {
    if (!only) {
    HDA_DBG(HDA_DBG_INFO, 
            " %*snid %d busy by association %d\n",
            depth + 1, "", w->nid, w->bindas);
    }
    return (0);
    }
    if (dupseq < 0) {
    if (w->bindseqmask != 0) {
    if (!only) {
    HDA_DBG(HDA_DBG_INFO, 
            " %*snid %d busy by seqmask %x\n",
            depth + 1, "", w->nid, w->bindseqmask);
    }
    return (0);
    }
    } else {
    /* If this is headphones - allow duplicate first pin. */
    if (w->bindseqmask != 0 &&
        (w->bindseqmask & (1 << dupseq)) == 0) {
    HDA_DBG(HDA_DBG_INFO, 
            " %*snid %d busy by seqmask %x\n",
            depth + 1, "", w->nid, w->bindseqmask);

    return (0);
    }
    }

    switch (w->type) {
    case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT:
        /* Do not traverse input. AD1988 has digital monitor
           for which we are not ready. */
        break;
    case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_OUTPUT:
        /* If we are tracing HP take only dac of first pin. */
        if ((only == 0 || only == w->nid) &&
            (w->nid >= min) && (dupseq < 0 || w->nid ==
                                codec->assoc_table[as].dacs[0][dupseq]))
            m = w->nid;
        break;
    case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX:
        if (depth > 0)
            break;
        /* Fall */
    default:
        /* Find reachable DACs with smallest nid respecting constraints. */
        for (i = 0; i < w->nconns; i++) {
        if (w->connsenable[i] == 0)
            continue;
        if (w->selconn != -1 && w->selconn != i)
            continue;
        if ((ret = audio_trace_dac(codec, as, seq,
                                        w->conns[i], dupseq, min, only, depth + 1)) != 0) {
        if (m == 0 || ret < m) {
        m = ret;
        im = i;
        }
        if (only || dupseq >= 0)
            break;
        }
        }
        if (im >= 0 && only && ((w->nconns > 1 &&
                                 w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER) ||
                                w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_SELECTOR))
            w->selconn = im;
        break;
    }
    if (m && only) {
    w->bindas = as;
    w->bindseqmask |= (1 << seq);
    }
    if (!only) {
    HDA_DBG(HDA_DBG_INFO, 
            " %*snid %d returned %d\n",
            depth + 1, "", w->nid, m);
    }
    return (m);
    }



/*
 * Erase trace path of the specified association.
 */
void audio_undo_trace(HDCODEC_ID codec, int as, int seq)
    {
    WIDGET *w;
    int i;

    for (i = codec->startnode; i < codec->endnode; i++) {
    w = widget_get(codec, i);
    if (w == NULL || w->enable == 0)
        continue;
    if (w->bindas == as) {
    if (seq >= 0) {
    w->bindseqmask &= ~(1 << seq);
    if (w->bindseqmask == 0) {
    w->bindas = -1;
    w->selconn = -1;
    }
    } else {
    w->bindas = -1;
    w->bindseqmask = 0;
    w->selconn = -1;
    }
    }
    }
    }

/*
 * Check equivalency of two DACs.
 */
LOCAL int audio_dacs_equal(WIDGET *w1, WIDGET *w2)
    {
    HDCODEC_ID codec = w1->codec;
    WIDGET *w3;
    int i, j, c1, c2;

    if (memcmp(&w1->param, &w2->param, sizeof(w1->param)))
        return (0);
    for (i = codec->startnode; i < codec->endnode; i++) {
    w3 = widget_get(codec, i);
    if (w3 == NULL || w3->enable == 0)
        continue;
    if (w3->bindas != w1->bindas)
        continue;
    if (w3->nconns == 0)
        continue;
    c1 = c2 = -1;
    for (j = 0; j < w3->nconns; j++) {
    if (w3->connsenable[j] == 0)
        continue;
    if (w3->conns[j] == w1->nid)
        c1 = j;
    if (w3->conns[j] == w2->nid)
        c2 = j;
    }
    if (c1 < 0)
        continue;
    if (c2 < 0)
        return (0);
    if (w3->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER)
        return (0);
    }
    return (1);
    }

/*
 * Check equivalency of two ADCs.
 */
LOCAL int audio_adcs_equal(WIDGET *w1, WIDGET *w2)
    {
    HDCODEC_ID codec = w1->codec;
    WIDGET *w3, *w4;
    int i;

    if (memcmp(&w1->param, &w2->param, sizeof(w1->param)))
        return (0);
    if (w1->nconns != 1 || w2->nconns != 1)
        return (0);
    if (w1->conns[0] == w2->conns[0])
        return (1);
    w3 = widget_get(codec, w1->conns[0]);
    if (w3 == NULL || w3->enable == 0)
        return (0);
    w4 = widget_get(codec, w2->conns[0]);
    if (w4 == NULL || w4->enable == 0)
        return (0);
    if (w3->bindas == w4->bindas && w3->bindseqmask == w4->bindseqmask)
        return (1);
    if (w4->bindas >= 0)
        return (0);
    if (w3->type != w4->type)
        return (0);
    if (memcmp(&w3->param, &w4->param, sizeof(w3->param)))
        return (0);
    if (w3->nconns != w4->nconns)
        return (0);
    for (i = 0; i < w3->nconns; i++)
        {
        if (w3->conns[i] != w4->conns[i])
            return (0);
        }
    return (1);
    }

/*
 * Look for equivalent DAC/ADC to implement second channel.
 */
LOCAL void audio_adddac(HDCODEC_ID codec, int asid)
    {
    ASSOC *as = &codec->assoc_table[asid];
    WIDGET *w1, *w2;
    int i, pos;
    nid_t nid1, nid2;

    /* Find the exisitng DAC position and return if found more the one. */
    pos = -1;
    for (i = 0; i < 16; i++)
        {
        if (as->dacs[0][i] <= 0)
            continue;
        if (pos >= 0 && as->dacs[0][i] != as->dacs[0][pos])
            return;
        pos = i;
        }

    nid1 = as->dacs[0][pos];
    w1 = widget_get(codec, nid1);
    w2 = NULL;
    for (nid2 = codec->startnode; nid2 < codec->endnode; nid2++)
        {
        w2 = widget_get(codec, nid2);
        if (w2 == NULL || w2->enable == 0)
            continue;
        if (w2->bindas >= 0)
            continue;
        if (w1->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_OUTPUT)
            {
            if (w2->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_OUTPUT)
                continue;
            if (audio_dacs_equal(w1, w2))
                break;
            }
        else
            {
            if (w2->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT)
                continue;
            if (audio_adcs_equal(w1, w2))
                break;
            }
        }
    if (nid2 >= codec->endnode)
        return;
    w2->bindas = w1->bindas;
    w2->bindseqmask = w1->bindseqmask;
    if (w1->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT)
        {
        HDA_DBG(HDA_DBG_INFO, 
                " ADC %d considered equal to ADC %d\n", nid2, nid1);

        w1 = widget_get(codec, w1->conns[0]);
        w2 = widget_get(codec, w2->conns[0]);
        w2->bindas = w1->bindas;
        w2->bindseqmask = w1->bindseqmask;
        }
    else
        {
        HDA_DBG(HDA_DBG_INFO, 
                " DAC %d considered equal to DAC %d\n", nid2, nid1);
        }
    for (i = 0; i < 16; i++)
        {
        if (as->dacs[0][i] <= 0)
            continue;
        as->dacs[as->num_chans][i] = nid2;
        }
    as->num_chans++;
    }

/*
 * Trace association path from DAC to output
 */
LOCAL int audio_trace_as_out(HDCODEC_ID codec, int as, int seq)
    {
    ASSOC *ases = codec->assoc_table;
    int i, hpredir;
    nid_t min, res;

    /* Find next pin */
    for (i = seq; i < 16 && ases[as].pins[i] == 0; i++)
        ;
    /* Check if there is no any left. If so - we succeeded. */
    if (i == 16)
        return (1);

    hpredir = (i == 15 && ases[as].fakeredir == 0)?ases[as].hpredir:-1;
    min = 0;
    do {
    HDA_DBG(HDA_DBG_INFO, " Tracing pin %d with min nid %d", ases[as].pins[i], min);
    if (hpredir >= 0)
        HDA_DBG(HDA_DBG_INFO, " and hpredir %d", hpredir);
    HDA_DBG(HDA_DBG_INFO, "\n");

    /* Trace this pin taking min nid into account. */
    res = audio_trace_dac(codec, as, i,
                               ases[as].pins[i], hpredir, min, 0, 0);
    if (res == 0)
        {
        /* If we failed - return to previous and redo it. */
        HDA_DBG(HDA_DBG_INFO, " Unable to trace pin %d seq %d with min nid %d", ases[as].pins[i], i, min);
        if (hpredir >= 0)
            HDA_DBG(HDA_DBG_INFO, " and hpredir %d", hpredir);
        HDA_DBG(HDA_DBG_INFO, "\n");

        return (0);
        }
    HDA_DBG(HDA_DBG_INFO, " Pin %d traced to DAC %d", ases[as].pins[i], res);
    if (hpredir >= 0)
        HDA_DBG(HDA_DBG_INFO, " and hpredir %d", hpredir);
    if (ases[as].fakeredir)
        HDA_DBG(HDA_DBG_INFO, " with fake redirection");
    HDA_DBG(HDA_DBG_INFO, "\n");

    /* Trace again to mark the path */
    audio_trace_dac(codec, as, i,
                         ases[as].pins[i], hpredir, min, res, 0);
    ases[as].dacs[0][i] = res;
    /* We succeeded, so call next. */
    if (audio_trace_as_out(codec, as, i + 1))
        return (1);
    /* If next failed, we should retry with next min */
    audio_undo_trace(codec, as, i);
    ases[as].dacs[0][i] = 0;
    min = res + 1;
    } while (1);
    }

LOCAL int audio_trace_as_in(HDCODEC_ID codec, int as)
    {
    ASSOC *ases = codec->assoc_table;
    WIDGET *w;
    int i, j, k, length;

    for (j = codec->startnode; j < codec->endnode; j++)
        {
        w = widget_get(codec, j);
        if (w == NULL || w->enable == 0)
            continue;
        if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT)
            continue;
        if (w->bindas >= 0 && w->bindas != as)
            continue;

        /* Find next pin */
        for (i = 0; i < 16; i++)
            {
            if (ases[as].pins[i] == 0)
                continue;

            HDA_DBG(HDA_DBG_INFO, 
                    " Tracing pin %d to ADC %d\n",
                    ases[as].pins[i], j);

            /* Trace this pin taking goal into account. */
            if (audio_trace_adc(codec, as, i,
                                     ases[as].pins[i], 1, 0, j, 0, &length, 0) == 0)
                {
                /* If we failed - return to previous and redo it. */
                HDA_DBG(HDA_DBG_INFO, 
                        " Unable to trace pin %d to ADC %d, undo traces\n",
                        ases[as].pins[i], j);

                audio_undo_trace(codec, as, -1);
                for (k = 0; k < 16; k++)
                    ases[as].dacs[0][k] = 0;
                break;
                }
            HDA_DBG(HDA_DBG_INFO, 
                    " Pin %d traced to ADC %d\n",
                    ases[as].pins[i], j);

            ases[as].dacs[0][i] = j;
            }
        if (i == 16)
            return (1);
        }
    return (0);
    }

LOCAL int audio_trace_as_in_mch(HDCODEC_ID codec, int as, int seq)
    {
    ASSOC *ases = codec->assoc_table;
    int i, length;
    nid_t min, res;

    /* Find next pin */
    for (i = seq; i < 16 && ases[as].pins[i] == 0; i++)
        ;
    /* Check if there is no any left. If so - we succeeded. */
    if (i == 16)
        return (1);

    min = 0;
    do {
    HDA_DBG(HDA_DBG_INFO, 
            " Tracing pin %d with min nid %d",
            ases[as].pins[i], min);
    HDA_DBG(HDA_DBG_INFO, "\n");

    /* Trace this pin taking min nid into account. */
    res = audio_trace_adc(codec, as, i,
                               ases[as].pins[i], 0, min, 0, 0, &length, 0);
    if (res == 0)
        {
        /* If we failed - return to previous and redo it. */
        HDA_DBG(HDA_DBG_INFO, 
                " Unable to trace pin %d seq %d with min "
                "nid %d",
                ases[as].pins[i], i, min);
        HDA_DBG(HDA_DBG_INFO, "\n");

        return (0);
        }
    HDA_DBG(HDA_DBG_INFO, 
            " Pin %d traced to ADC %d\n",
            ases[as].pins[i], res);

    /* Trace again to mark the path */
    audio_trace_adc(codec, as, i,
                         ases[as].pins[i], 0, min, res, 0, &length, length);
    ases[as].dacs[0][i] = res;
    /* We succeeded, so call next. */
    if (audio_trace_as_in_mch(codec, as, i + 1))
        return (1);
    /* If next failed, we should retry with next min */
    audio_undo_trace(codec, as, i);
    ases[as].dacs[0][i] = 0;
    min = res + 1;
    } while (1);
    }

/*
 * Assign OSS names to sound sources
 */
LOCAL void audio_assign_names(HDCODEC_ID codec)
    {
    ASSOC *as = codec->assoc_table;
    WIDGET *w;
    int i, j;
    int type = -1, use, used = 0;
    static const int types[7][13] = {
        { SOUND_MIXER_LINE, SOUND_MIXER_LINE1, SOUND_MIXER_LINE2, 
          SOUND_MIXER_LINE3, -1 },  /* line */
        { SOUND_MIXER_MONITOR, SOUND_MIXER_MIC, -1 }, /* int mic */
        { SOUND_MIXER_MIC, SOUND_MIXER_MONITOR, -1 }, /* ext mic */
        { SOUND_MIXER_CD, -1 }, /* cd */
        { SOUND_MIXER_SPEAKER, -1 },    /* speaker */
        { SOUND_MIXER_DIGITAL1, SOUND_MIXER_DIGITAL2, SOUND_MIXER_DIGITAL3,
          -1 }, /* digital */
        { SOUND_MIXER_LINE, SOUND_MIXER_LINE1, SOUND_MIXER_LINE2,
          SOUND_MIXER_LINE3, SOUND_MIXER_PHONEIN, SOUND_MIXER_PHONEOUT,
          SOUND_MIXER_VIDEO, SOUND_MIXER_RADIO, SOUND_MIXER_DIGITAL1,
          SOUND_MIXER_DIGITAL2, SOUND_MIXER_DIGITAL3, SOUND_MIXER_MONITOR,
          -1 }  /* others */
    };

    /* Surely known names */
    for (i = codec->startnode; i < codec->endnode; i++)
        {
        w = widget_get(codec, i);
        if (w == NULL || w->enable == 0)
            continue;
        if (w->bindas == -1)
            continue;
        use = -1;
        switch (w->type)
            {
            case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX:
                if (as[w->bindas].dir == CTL_OUT)
                    break;
                type = -1;
                switch (w->wclass.pin.config & HDA_CONFIG_DEFAULTCONF_DEVICE_MASK)
                    {
                    case HDA_CONFIG_DEFAULTCONF_DEVICE_LINE_IN:
                        type = 0;
                        break;
                    case HDA_CONFIG_DEFAULTCONF_DEVICE_MIC_IN:
                        if ((w->wclass.pin.config & HDA_CONFIG_DEFAULTCONF_CONNECTIVITY_MASK)
                            == HDA_CONFIG_DEFAULTCONF_CONNECTIVITY_JACK)
                            break;
                        type = 1;
                        break;
                    case HDA_CONFIG_DEFAULTCONF_DEVICE_CD:
                        type = 3;
                        break;
                    case HDA_CONFIG_DEFAULTCONF_DEVICE_SPEAKER:
                        type = 4;
                        break;
                    case HDA_CONFIG_DEFAULTCONF_DEVICE_SPDIF_IN:
                    case HDA_CONFIG_DEFAULTCONF_DEVICE_DIGITAL_OTHER_IN:
                        type = 5;
                        break;
                    }
                if (type == -1)
                    break;
                j = 0;
                while (types[type][j] >= 0 &&
                       (used & (1 << types[type][j])) != 0)
                    {
                    j++;
                    }
                if (types[type][j] >= 0)
                    use = types[type][j];
                break;
            case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_OUTPUT:
                use = SOUND_MIXER_PCM;
                break;
            case HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_BEEP_WIDGET:
                use = SOUND_MIXER_SPEAKER;
                break;
            default:
                break;
            }
        if (use >= 0)
            {
            w->ossdev = use;
            used |= (1 << use);
            }
        }
    /* Semi-known names */
    for (i = codec->startnode; i < codec->endnode; i++)
        {
        w = widget_get(codec, i);
        if (w == NULL || w->enable == 0)
            continue;
        if (w->ossdev >= 0)
            continue;
        if (w->bindas == -1)
            continue;
        if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
            continue;
        if (as[w->bindas].dir == CTL_OUT)
            continue;
        type = -1;
        switch (w->wclass.pin.config & HDA_CONFIG_DEFAULTCONF_DEVICE_MASK)
            {
            case HDA_CONFIG_DEFAULTCONF_DEVICE_LINE_OUT:
            case HDA_CONFIG_DEFAULTCONF_DEVICE_SPEAKER:
            case HDA_CONFIG_DEFAULTCONF_DEVICE_HP_OUT:
            case HDA_CONFIG_DEFAULTCONF_DEVICE_AUX:
                type = 0;
                break;
            case HDA_CONFIG_DEFAULTCONF_DEVICE_MIC_IN:
                type = 2;
                break;
            case HDA_CONFIG_DEFAULTCONF_DEVICE_SPDIF_OUT:
            case HDA_CONFIG_DEFAULTCONF_DEVICE_DIGITAL_OTHER_OUT:
                type = 5;
                break;
            }
        if (type == -1)
            break;
        j = 0;
        while (types[type][j] >= 0 &&
               (used & (1 << types[type][j])) != 0)
            {
            j++;
            }
        if (types[type][j] >= 0)
            {
            w->ossdev = types[type][j];
            used |= (1 << types[type][j]);
            }
        }
    /* Others */
    for (i = codec->startnode; i < codec->endnode; i++)
        {
        w = widget_get(codec, i);
        if (w == NULL || w->enable == 0)
            continue;
        if (w->ossdev >= 0)
            continue;
        if (w->bindas == -1)
            continue;
        if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
            continue;
        if (as[w->bindas].dir == CTL_OUT)
            continue;
        j = 0;
        while (types[6][j] >= 0 &&
               (used & (1 << types[6][j])) != 0)
            {
            j++;
            }
        if (types[6][j] >= 0)
            {
            w->ossdev = types[6][j];
            used |= (1 << types[6][j]);
            }
        }
    }

void audio_build_tree(HDCODEC_ID codec)
    {
    ASSOC *as = codec->assoc_table;
    int j, res;

    /* Trace all associations in order of their numbers. */
    for (j = 0; j < codec->ascnt; j++)
        {
        if (as[j].enable == 0)
            continue;
        HDA_DBG(HDA_DBG_INFO, 
                "Tracing association %d (%d)\n", j, as[j].index);

        if (as[j].dir == CTL_OUT)
            {
            retry:
            res = audio_trace_as_out(codec, j, 0);
            if (res == 0 && as[j].hpredir >= 0 &&
                as[j].fakeredir == 0)
                {
                /* If CODEC can't do analog HP redirection
                   try to make it using one more DAC. */
                as[j].fakeredir = 1;
                goto retry;
                }
            } else if (as[j].mixed)
            res = audio_trace_as_in(codec, j);
        else
            res = audio_trace_as_in_mch(codec, j, 0);
        if (res)
            {
            HDA_DBG(HDA_DBG_INFO, 
                    "Association %d (%d) trace succeeded\n",
                    j, as[j].index);

            } else
            {
            HDA_DBG(HDA_DBG_INFO, 
                    "Association %d (%d) trace failed\n",
                    j, as[j].index);

            as[j].enable = 0;
            }
        }

    /* Look for additional DACs/ADCs. */
    for (j = 0; j < codec->ascnt; j++)
        {
        if (as[j].enable == 0)
            continue;
        audio_adddac(codec, j);
        }
    }


/*
 * Store in pcm_dev_table new data about whether and how we can control signal
 * for OSS device to/from specified widget.
 */
LOCAL void
    adjust_amp(WIDGET *w, int ossdev,
                    int found, int minamp, int maxamp)
    {
    HDCODEC_ID codec = w->codec;
    PCM_DEVINFO *pcm_dev_table;

    if (w->bindas >= 0)
        pcm_dev_table = codec->assoc_table[w->bindas].pcm_dev;
    else
        pcm_dev_table = &codec->pcm_dev_table[0];
    if (found)
        pcm_dev_table->ossmask |= (1 << ossdev);
    if (minamp == 0 && maxamp == 0)
        return;
    if (pcm_dev_table->minamp[ossdev] == 0 && pcm_dev_table->maxamp[ossdev] == 0)
        {
        pcm_dev_table->minamp[ossdev] = minamp;
        pcm_dev_table->maxamp[ossdev] = maxamp;
        }
    else
        {
        pcm_dev_table->minamp[ossdev] = MAX(pcm_dev_table->minamp[ossdev], minamp);
        pcm_dev_table->maxamp[ossdev] = MIN(pcm_dev_table->maxamp[ossdev], maxamp);
        }
    }

/*
 * Trace signals from/to all possible sources/destionstions to find possible
 * recording sources, OSS device control ranges and to assign controls.
 */
LOCAL void audio_assign_mixers(HDCODEC_ID codec)
    {
    ASSOC *as = codec->assoc_table;
    WIDGET *w, *cw;
    int i, j, minamp, maxamp, found;

    /* Assign mixers to the tree. */
    for (i = codec->startnode; i < codec->endnode; i++)
        {
        w = widget_get(codec, i);
        if (w == NULL || w->enable == 0)
            continue;
        minamp = maxamp = 0;
        if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_OUTPUT ||
            w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_BEEP_WIDGET ||
            (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX &&
             as[w->bindas].dir == CTL_IN))
            {
            if (w->ossdev < 0)
                continue;
            found = audio_ctl_source_amp(codec, w->nid, -1,
                                              w->ossdev, 1, 0, &minamp, &maxamp);
            adjust_amp(w, w->ossdev, found, minamp, maxamp);
            } else if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT)
            {
            found = audio_ctl_dest_amp(codec, w->nid, -1,
                                            SOUND_MIXER_RECLEV, 0, &minamp, &maxamp);
            adjust_amp(w, SOUND_MIXER_RECLEV, found, minamp, maxamp);
            } else if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX &&
                       as[w->bindas].dir == CTL_OUT)
            {
            found = audio_ctl_dest_amp(codec, w->nid, -1,
                                            SOUND_MIXER_VOLUME, 0, &minamp, &maxamp);
            adjust_amp(w, SOUND_MIXER_VOLUME, found, minamp, maxamp);
            }
        if (w->ossdev == SOUND_MIXER_IMIX)
            {
            minamp = maxamp = 0;
            found = audio_ctl_source_amp(codec, w->nid, -1,
                                              w->ossdev, 1, 0, &minamp, &maxamp);
            if (minamp == maxamp)
                {
                /* If we are unable to control input monitor
                   as source - try to control it as destination. */
                found += audio_ctl_dest_amp(codec, w->nid, -1,
                                                 w->ossdev, 0, &minamp, &maxamp);
                w->pflags |= HDAA_IMIX_AS_DST;
                }
            adjust_amp(w, w->ossdev, found, minamp, maxamp);
            }
        if (w->pflags & HDAA_ADC_MONITOR)
            {
            for (j = 0; j < w->nconns; j++)
                {
                if (!w->connsenable[j])
                    continue;
                cw = widget_get(codec, w->conns[j]);
                if (cw == NULL || cw->enable == 0)
                    continue;
                if (cw->bindas == -1)
                    continue;
                if (cw->bindas >= 0 &&
                    as[cw->bindas].dir != CTL_IN)
                    continue;
                minamp = maxamp = 0;
                found = audio_ctl_dest_amp(codec,
                                                w->nid, j, SOUND_MIXER_IGAIN, 0,
                                                &minamp, &maxamp);
                adjust_amp(w, SOUND_MIXER_IGAIN,
                                found, minamp, maxamp);
                }
            }
        }
    }
/*
 * Find controls to control amplification for source and calculate possible
 * amplification range.
 */
LOCAL int audio_ctl_source_amp(HDCODEC_ID codec, nid_t nid, int index,
                                    int ossdev, int ctlable, int depth, int *minamp, int *maxamp)
    {
    WIDGET *w, *wc;
    AUDIO_CTL *ctl;
    int i, j, conns = 0, tminamp, tmaxamp, cminamp, cmaxamp, found = 0;

    if (depth > HDA_PARSE_MAXDEPTH)
        return (found);

    w = widget_get(codec, nid);
    if (w == NULL || w->enable == 0)
        return (found);

    /* Count number of active inputs. */
    if (depth > 0)
        {
        for (j = 0; j < w->nconns; j++)
            {
            if (!w->connsenable[j])
                continue;
            conns++;
            }
        }

    /* If this is not a first step - use input mixer.
       Pins have common input ctl so care must be taken. */
    if (depth > 0 && ctlable && (conns == 1 ||
                                 w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX))
        {
        ctl = audio_ctl_amp_get(codec, w->nid, CTL_IN,
                                     index, 1);
        if (ctl)
            {
            ctl->ossmask |= (1 << ossdev);
            found++;
            if (*minamp == *maxamp)
                {
                *minamp += MINQDB(ctl);
                *maxamp += MAXQDB(ctl);
                }
            }
        }

    /* If widget has own ossdev - not traverse it.
       It will be traversed on it's own. */
    if (w->ossdev >= 0 && depth > 0)
        return (found);

    /* We must not traverse pin */
    if ((w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT ||
         w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX) &&
        depth > 0)
        return (found);

    /* record that this widget exports such signal, */
    w->ossmask |= (1 << ossdev);

    /*
     * If signals mixed, we can't assign controls farther.
     * Ignore this on depth zero. Caller must knows why.
     */
    if (conns > 1 &&
        w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER)
        ctlable = 0;

    if (ctlable)
        {
        ctl = audio_ctl_amp_get(codec, w->nid, CTL_OUT, -1, 1);
        if (ctl)
            {
            ctl->ossmask |= (1 << ossdev);
            found++;
            if (*minamp == *maxamp)
                {
                *minamp += MINQDB(ctl);
                *maxamp += MAXQDB(ctl);
                }
            }
        }

    cminamp = cmaxamp = 0;
    for (i = codec->startnode; i < codec->endnode; i++)
        {
        wc = widget_get(codec, i);
        if (wc == NULL || wc->enable == 0)
            continue;
        for (j = 0; j < wc->nconns; j++)
            {
            if (wc->connsenable[j] && wc->conns[j] == nid)
                {
                tminamp = tmaxamp = 0;
                found += audio_ctl_source_amp(codec,
                                                   wc->nid, j, ossdev, ctlable, depth + 1,
                                                   &tminamp, &tmaxamp);
                if (cminamp == 0 && cmaxamp == 0)
                    {
                    cminamp = tminamp;
                    cmaxamp = tmaxamp;
                    } else if (tminamp != tmaxamp)
                    {
                    cminamp = MAX(cminamp, tminamp);
                    cmaxamp = MIN(cmaxamp, tmaxamp);
                    }
                }
            }
        }
    if (*minamp == *maxamp && cminamp < cmaxamp)
        {
        *minamp += cminamp;
        *maxamp += cmaxamp;
        }
    return (found);
    }

/*
 * Find controls to control amplification for destination and calculate
 * possible amplification range.
 */
LOCAL int audio_ctl_dest_amp(HDCODEC_ID codec, nid_t nid, int index,
                                  int ossdev, int depth, int *minamp, int *maxamp)
    {
    ASSOC *as = codec->assoc_table;
    WIDGET *w, *wc;
    AUDIO_CTL *ctl;
    int i, j, consumers, tminamp, tmaxamp, cminamp, cmaxamp, found = 0;

    if (depth > HDA_PARSE_MAXDEPTH)
        return (found);

    w = widget_get(codec, nid);
    if (w == NULL || w->enable == 0)
        return (found);

    if (depth > 0)
        {
        /* If this node produce output for several consumers,
           we can't touch it. */
        consumers = 0;
        for (i = codec->startnode; i < codec->endnode; i++)
            {
            wc = widget_get(codec, i);
            if (wc == NULL || wc->enable == 0)
                continue;
            for (j = 0; j < wc->nconns; j++)
                {
                if (wc->connsenable[j] && wc->conns[j] == nid)
                    consumers++;
                }
            }
        /* The only exception is if real HP redirection is configured
           and this is a duplication point.
           XXX: Actually exception is not completely correct.
           XXX: Duplication point check is not perfect. */
        if ((consumers == 2 && (w->bindas < 0 ||
                                as[w->bindas].hpredir < 0 || as[w->bindas].fakeredir ||
                                (w->bindseqmask & (1 << 15)) == 0)) ||
            consumers > 2)
            return (found);

        /* Else use it's output mixer. */
        ctl = audio_ctl_amp_get(codec, w->nid,
                                     CTL_OUT, -1, 1);
        if (ctl)
            {
            ctl->ossmask |= (1 << ossdev);
            found++;
            if (*minamp == *maxamp)
                {
                *minamp += MINQDB(ctl);
                *maxamp += MAXQDB(ctl);
                }
            }
        }

    /* We must not traverse pin */
    if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX &&
        depth > 0)
        return (found);

    cminamp = cmaxamp = 0;
    for (i = 0; i < w->nconns; i++)
        {
        if (w->connsenable[i] == 0)
            continue;
        if (index >= 0 && i != index)
            continue;
        tminamp = tmaxamp = 0;
        ctl = audio_ctl_amp_get(codec, w->nid,
                                     CTL_IN, i, 1);
        if (ctl)
            {
            ctl->ossmask |= (1 << ossdev);
            found++;
            if (*minamp == *maxamp)
                {
                tminamp += MINQDB(ctl);
                tmaxamp += MAXQDB(ctl);
                }
            }
        found += audio_ctl_dest_amp(codec, w->conns[i], -1, ossdev,
                                         depth + 1, &tminamp, &tmaxamp);
        if (cminamp == 0 && cmaxamp == 0)
            {
            cminamp = tminamp;
            cmaxamp = tmaxamp;
            } else if (tminamp != tmaxamp)
            {
            cminamp = MAX(cminamp, tminamp);
            cmaxamp = MIN(cmaxamp, tmaxamp);
            }
        }
    if (*minamp == *maxamp && cminamp < cmaxamp)
        {
        *minamp += cminamp;
        *maxamp += cmaxamp;
        }
    return (found);
    }


/*
 * Headphones redirection change handler.
 */
LOCAL void hpredir_handler(WIDGET *w)
    {
    HDCODEC_ID codec = w->codec;
    ASSOC *as = &codec->assoc_table[w->bindas];
    WIDGET *w1;
    AUDIO_CTL *ctl;
    UINT32 val;
    int j, connected = w->wclass.pin.connected;

    HDA_DBG(HDA_DBG_INFO, "Redirect output to: %s\n", connected ? "headphones": "main");

    /* (Un)Mute headphone pin. */
    ctl = audio_ctl_amp_get(codec,
                                 w->nid, CTL_IN, -1, 1);
    if (ctl != NULL && ctl->mute)
        {
        /* If pin has muter - use it. */
        val = connected ? 0 : 1;
        if (val != ctl->forcemute)
            {
            ctl->forcemute = val;
            audio_ctl_amp_set(ctl,
                                   HDAA_AMP_MUTE_DEFAULT,
                                   HDAA_AMP_VOL_DEFAULT, HDAA_AMP_VOL_DEFAULT);
            }
        }
    else
        {
        /* If there is no muter - disable pin output. */
        if (connected)
            val = w->wclass.pin.ctrl |
                HDA_CMD_SET_PIN_WIDGET_CTRL_OUT_ENABLE;
        else
            val = w->wclass.pin.ctrl &
                ~HDA_CMD_SET_PIN_WIDGET_CTRL_OUT_ENABLE;
        if (val != w->wclass.pin.ctrl)
            {
            w->wclass.pin.ctrl = val;
            hda_command(w->codec,
                        HDA_CMD_SET_PIN_WIDGET_CTRL(0,
                                                    w->nid, w->wclass.pin.ctrl));
            }
        }
    /* (Un)Mute other pins. */
    for (j = 0; j < 15; j++)
        {
        if (as->pins[j] <= 0)
            continue;
        ctl = audio_ctl_amp_get(codec,
                                     as->pins[j], CTL_IN, -1, 1);
        if (ctl != NULL && ctl->mute)
            {
            /* If pin has muter - use it. */
            val = connected ? 1 : 0;
            if (val == ctl->forcemute)
                continue;
            ctl->forcemute = val;
            audio_ctl_amp_set(ctl,
                                   HDAA_AMP_MUTE_DEFAULT,
                                   HDAA_AMP_VOL_DEFAULT, HDAA_AMP_VOL_DEFAULT);
            continue;
            }
        /* If there is no muter - disable pin output. */
        w1 = widget_get(codec, as->pins[j]);
        if (w1 != NULL)
            {
            if (connected)
                val = w1->wclass.pin.ctrl &
                    ~HDA_CMD_SET_PIN_WIDGET_CTRL_OUT_ENABLE;
            else
                val = w1->wclass.pin.ctrl |
                    HDA_CMD_SET_PIN_WIDGET_CTRL_OUT_ENABLE;
            if (val != w1->wclass.pin.ctrl)
                {
                w1->wclass.pin.ctrl = val;
                hda_command(w->codec,
                            HDA_CMD_SET_PIN_WIDGET_CTRL(0,
                                                        w1->nid, w1->wclass.pin.ctrl));
                }
            }
        }
    }

/*
 * Recording source change handler.
 */
LOCAL void autorecsrc_handler(ASSOC *as, WIDGET *w)
    {
    PCM_DEVINFO *pdevinfo = as->pcm_dev;
    HDCODEC_ID codec;
    WIDGET *w1;
    int i, mask, fullmask, prio, bestprio;

    if (!as->mixed || pdevinfo == NULL || pdevinfo->mixer == NULL)
        return;
    /* Don't touch anything if we asked not to. */
    if (pdevinfo->autorecsrc == AUTO_SELECT_RECORD_SOURCE_DISABLE ||
        (pdevinfo->autorecsrc == AUTO_SELECT_RECORD_SOURCE_ONCE && w != NULL))
        return;
    /* Don't touch anything if "mix" or "speaker" selected. */
    if (pdevinfo->recsrc & (SOUND_MASK_IMIX | SOUND_MASK_SPEAKER))
        return;
    /* Don't touch anything if several selected. */
    if (ffsMsb(pdevinfo->recsrc) != ffsLsb(pdevinfo->recsrc))
        return;
    codec = pdevinfo->codec;
    mask = fullmask = 0;
    bestprio = 0;
    for (i = 0; i < 16; i++)
        {
        if (as->pins[i] <= 0)
            continue;
        w1 = widget_get(codec, as->pins[i]);
        if (w1 == NULL || w1->enable == 0)
            continue;
        if (w1->wclass.pin.connected == 0)
            continue;
        prio = (w1->wclass.pin.connected == 1) ? 2 : 1;
        if (prio < bestprio)
            continue;
        if (prio > bestprio)
            {
            mask = 0;
            bestprio = prio;
            }
        mask |= (1 << w1->ossdev);
        fullmask |= (1 << w1->ossdev);
        }
    if (mask == 0)
        return;
    /* Prefer newly connected input. */
    if (w != NULL && (mask & (1 << w->ossdev)))
        mask = (1 << w->ossdev);
    /* Prefer previously selected input */
    if (mask & pdevinfo->recsrc)
        mask &= pdevinfo->recsrc;
    /* Prefer mic. */
    if (mask & SOUND_MASK_MIC)
        mask = SOUND_MASK_MIC;
    /* Prefer monitor (2nd mic). */
    if (mask & SOUND_MASK_MONITOR)
        mask = SOUND_MASK_MONITOR;
    /* Just take first one. */
    mask = (1 << (ffsLsb(mask) - 1));

    ossmix_setrecsrc(pdevinfo->mixer, mask);
    }

/*
 * Jack presence detection event handler.
 */
LOCAL void presence_handler(WIDGET *w)
    {
    HDCODEC_ID codec = w->codec;
    ASSOC *as;
    UINT32 res;
    int connected;

    if (w->enable == 0 || w->type !=
        HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
        return;

    if (HDA_PARAM_PIN_CAP_PRESENCE_DETECT_CAP(w->wclass.pin.cap) == 0 ||
        (HDA_CONFIG_DEFAULTCONF_MISC(w->wclass.pin.config) & 1) != 0)
        return;

    res = hda_command(w->codec, HDA_CMD_GET_PIN_SENSE(0, w->nid));
    connected = (res & HDA_CMD_GET_PIN_SENSE_PRESENCE_DETECT) != 0;

    if (connected == w->wclass.pin.connected)
        return;
    w->wclass.pin.connected = connected;

    HDA_DBG(HDA_DBG_INFO, "Pin sense: nid=%d sence=0x%08x (%sconnected)\n",
            w->nid, res, !w->wclass.pin.connected ? "dis" : "");


    as = &codec->assoc_table[w->bindas];
    if (as->hpredir >= 0 && as->pins[15] == w->nid)
        hpredir_handler(w);
    if (as->dir == CTL_IN)
        autorecsrc_handler(as, w);
    }

/*
 * Pin sense initializer.
 */
LOCAL void sense_init(HDCODEC_ID codec)
    {
    ASSOC *as = codec->assoc_table;
    WIDGET *w;
    int i, poll = 0;

    for (i = codec->startnode; i < codec->endnode; i++)
        {
        w = widget_get(codec, i);
        if (w == NULL || w->enable == 0 || w->type !=
            HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
            continue;
        if (HDA_PARAM_AUDIO_WIDGET_CAP_UNSOL_CAP(w->param.widget_cap) &&
            w->unsol < 0)
            {
            w->unsol = hdacc_unsol_alloc(codec, w->nid);
            hda_command(codec,
                        HDA_CMD_SET_UNSOLICITED_RESPONSE(0, w->nid,
                                                         HDA_CMD_SET_UNSOLICITED_RESPONSE_ENABLE | w->unsol));
            }
        as = &codec->assoc_table[w->bindas];
        if (as->hpredir >= 0 && as->pins[15] == w->nid)
            {
            if (HDA_PARAM_PIN_CAP_PRESENCE_DETECT_CAP(w->wclass.pin.cap) == 0 ||
                (HDA_CONFIG_DEFAULTCONF_MISC(w->wclass.pin.config) & 1) != 0)
                {
                HDA_DBG(HDA_DBG_INFO, 
                        "No presence detection support at nid %d\n",
                        as[i].pins[15]);
                }
            else
                {
                if (w->unsol < 0)
                    {
                    poll = 1;
#ifdef  HDA_DBG_ON
                    global_poll = TRUE;
#endif
                    }

                HDA_DBG(HDA_DBG_INFO, 
                        "Headphones redirection for "
                        "association %d nid=%d using %s.\n",
                        w->bindas, w->nid,
                        (poll != 0) ? "polling" :
                        "unsolicited responses");

                };
            }

        presence_handler(w);

        if (!HDA_PARAM_PIN_CAP_DP(w->wclass.pin.cap) &&
            !HDA_PARAM_PIN_CAP_HDMI(w->wclass.pin.cap))
            continue;
        }
#if 0
    if (poll)
        {
        callout_reset(&codec->poll_jack, 1,
                      jack_poll_callback, codec);
        }
#endif
    }

LOCAL void audio_prepare_pin_ctrl(HDCODEC_ID codec)
    {
    ASSOC *as = codec->assoc_table;
    WIDGET *w;
    UINT32 pincap;
    int i;

    for (i = 0; i < codec->nodecnt; i++)
        {
        w = hdAudioWidgetNum (codec, i);
        if (w == NULL)
            continue;
        if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX &&
            w->waspin == 0)
            continue;

        pincap = w->wclass.pin.cap;

        /* Disable everything. */
        w->wclass.pin.ctrl &= ~(
        HDA_CMD_SET_PIN_WIDGET_CTRL_HPHN_ENABLE |
        HDA_CMD_SET_PIN_WIDGET_CTRL_OUT_ENABLE |
        HDA_CMD_SET_PIN_WIDGET_CTRL_IN_ENABLE |
        HDA_CMD_SET_PIN_WIDGET_CTRL_VREF_ENABLE_MASK);

        if (w->enable == 0)
            {
            /* Pin is unused so left it disabled. */
            continue;
            }
        else if (w->waspin)
            {
            /* Enable input for beeper input. */
            w->wclass.pin.ctrl |=
                HDA_CMD_SET_PIN_WIDGET_CTRL_IN_ENABLE;
            }
        else if (w->bindas < 0 || as[w->bindas].enable == 0)
            {
            /* Pin is unused so left it disabled. */
            continue;
            }
        else if (as[w->bindas].dir == CTL_IN)
            {
            /* Input pin, configure for input. */
            if (HDA_PARAM_PIN_CAP_INPUT_CAP(pincap))
                w->wclass.pin.ctrl |=
                    HDA_CMD_SET_PIN_WIDGET_CTRL_IN_ENABLE;

            /* external input voltage reference */
 
            if (HDA_PARAM_PIN_CAP_VREF_CTRL_100(pincap))
				w->wclass.pin.ctrl |=
				    HDA_CMD_SET_PIN_WIDGET_CTRL_VREF_ENABLE(HDA_CMD_PIN_WIDGET_CTRL_VREF_ENABLE_100);

            else if (HDA_PARAM_PIN_CAP_VREF_CTRL_80(pincap))
				w->wclass.pin.ctrl |=
				    HDA_CMD_SET_PIN_WIDGET_CTRL_VREF_ENABLE(HDA_CMD_PIN_WIDGET_CTRL_VREF_ENABLE_80);
            else if (HDA_PARAM_PIN_CAP_VREF_CTRL_50(pincap))
				w->wclass.pin.ctrl |=
				    HDA_CMD_SET_PIN_WIDGET_CTRL_VREF_ENABLE(HDA_CMD_PIN_WIDGET_CTRL_VREF_ENABLE_50);
            }
        else
            {
            /* Output pin, configure for output. */
            if (HDA_PARAM_PIN_CAP_OUTPUT_CAP(pincap))
                w->wclass.pin.ctrl |=
                    HDA_CMD_SET_PIN_WIDGET_CTRL_OUT_ENABLE;

            if (HDA_PARAM_PIN_CAP_HEADPHONE_CAP(pincap) &&
                (w->wclass.pin.config &
                 HDA_CONFIG_DEFAULTCONF_DEVICE_MASK) ==
                HDA_CONFIG_DEFAULTCONF_DEVICE_HP_OUT)
                w->wclass.pin.ctrl |=
                    HDA_CMD_SET_PIN_WIDGET_CTRL_HPHN_ENABLE;

            /* external output voltage reference */
            if (HDA_PARAM_PIN_CAP_VREF_CTRL_100(pincap))
				w->wclass.pin.ctrl |=
				    HDA_CMD_SET_PIN_WIDGET_CTRL_VREF_ENABLE(HDA_CMD_PIN_WIDGET_CTRL_VREF_ENABLE_100);
            else if (HDA_PARAM_PIN_CAP_VREF_CTRL_80(pincap))
				w->wclass.pin.ctrl |=
				    HDA_CMD_SET_PIN_WIDGET_CTRL_VREF_ENABLE(HDA_CMD_PIN_WIDGET_CTRL_VREF_ENABLE_80);
            else if (HDA_PARAM_PIN_CAP_VREF_CTRL_50(pincap))
				w->wclass.pin.ctrl |=
				    HDA_CMD_SET_PIN_WIDGET_CTRL_VREF_ENABLE(HDA_CMD_PIN_WIDGET_CTRL_VREF_ENABLE_50);
            
            }
        }
    }

LOCAL void audio_ctl_commit(HDCODEC_ID codec)
    {
    AUDIO_CTL *ctl;
    int i, z;

    i = 0;
    while ((ctl = audio_ctl_each(codec, &i)) != NULL)
        {
        if (ctl->enable == 0 || ctl->ossmask != 0)
            {
            /* Mute disabled and mixer controllable controls.
             * Last will be initialized by mixer_init().
             * This expected to reduce click on startup. */
            audio_ctl_amp_set(ctl, HDAA_AMP_MUTE_ALL, 0, 0);
            continue;
            }
        /* Init fixed controls to 0dB amplification. */
        z = ctl->offset;
        if (z > ctl->step)
            z = ctl->step;
        audio_ctl_amp_set(ctl, HDAA_AMP_MUTE_NONE, z, z);
        }
    }


LOCAL void audio_commit(HDCODEC_ID codec)
    {
    WIDGET *w;
    int i;

    /* Commit controls. */
    audio_ctl_commit(codec);

    /* Commit selectors, pins and EAPD. */
    for (i = 0; i < codec->nodecnt; i++)
        {
        w = hdAudioWidgetNum (codec, i);
        if (w == NULL)
            continue;
        if (w->selconn == -1)
            w->selconn = 0;
        if (w->nconns > 0)
            widget_connection_select(w, w->selconn);
        if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX ||
            w->waspin)
            {
            hda_command(codec,
                        HDA_CMD_SET_PIN_WIDGET_CTRL(0, w->nid,
                                                    w->wclass.pin.ctrl));
            }
        if (w->param.eapdbtl != HDA_INVALID)
            {
            UINT32 val;

            val = w->param.eapdbtl;

            hda_command(codec,
                        HDA_CMD_SET_EAPD_BTL_ENABLE(0, w->nid,
                                                    val));
            }
        }
    }

LOCAL void powerup(HDCODEC_ID codec)
    {
    int i;
    UINT32 val = 0;
    val = hda_command(codec,
                      HDA_CMD_GET_POWER_STATE(0, codec->nid));
    hdaUsDelay(100);

    hda_command(codec,
                HDA_CMD_SET_POWER_STATE(0,
                                        codec->nid, HDA_CMD_POWER_STATE_D0));
    hdaUsDelay(100);

    for (i = codec->startnode; i < codec->endnode; i++)
        {
        hda_command(codec,
                    HDA_CMD_SET_POWER_STATE(0,
                                            i, HDA_CMD_POWER_STATE_D0));
        }
    hdaUsDelay(100);
    }

#if 0
LOCAL UINT32 power_state(HDCODEC_ID codec, nid_t nid)
    {
    UINT32 val = 0;
    val = hda_command(codec,
                      HDA_CMD_GET_POWER_STATE(0, nid));
    return val;
    }
#endif

/*
 * Set mixer settings to our own default values:
 * +20dB for mics, -10dB for analog vol, mute for igain, 0dB for others.
 */
LOCAL void audio_ctl_set_defaults(PCM_DEVINFO *pcm_dev_table)
    {
    int amp, vol, dev;

    for (dev = 0; dev < SOUND_MIXER_NRDEVICES; dev++)
        {
        if ((pcm_dev_table->ossmask & (1 << dev)) == 0)
            continue;

        vol = -1;
        if (dev == SOUND_MIXER_OGAIN)
            vol = 100;
        else if (dev == SOUND_MIXER_IGAIN)
            vol = 0;
        else if (dev == SOUND_MIXER_MIC ||
                 dev == SOUND_MIXER_MONITOR)
            amp = 20 * 4;   /* +20dB */
        else if (dev == SOUND_MIXER_VOLUME && !pcm_dev_table->digital)
            amp = -10 * 4;  /* -10dB */
        else
            amp = 0;
        if (vol < 0 &&
            (pcm_dev_table->maxamp[dev] - pcm_dev_table->minamp[dev]) <= 0)
            {
            vol = 100;
            }
        else if (vol < 0)
            {
            vol = ((amp - pcm_dev_table->minamp[dev]) * 100 +
                   (pcm_dev_table->maxamp[dev] - pcm_dev_table->minamp[dev]) / 2) /
                (pcm_dev_table->maxamp[dev] - pcm_dev_table->minamp[dev]);
            vol = MIN(MAX(vol, 1), 100);
            }

        ossmix_set(pcm_dev_table->mixer, dev, vol, vol);

        }
    }



LOCAL void audio_parse(HDCODEC_ID codec)
    {
    WIDGET *w;
    UINT32 res;
    int i;
    nid_t nid;

    nid = codec->nid;

#if 0
    res = hda_command(HDA_CMD_GET_PARAMETER(0, nid, HDA_PARAM_GPIO_COUNT));
    codec->gpio_cap = res;
#endif    

    res = hda_command(codec,
                      HDA_CMD_GET_PARAMETER(0, nid, HDA_PARAM_SUPP_STREAM_FORMATS));
    codec->supp_stream_formats = res;

    res = hda_command(codec,
                      HDA_CMD_GET_PARAMETER(0, nid, HDA_PARAM_SUPP_PCM_SIZE_RATE));
    codec->supp_pcm_size_rate = res;

    res = hda_command(codec,
                      HDA_CMD_GET_PARAMETER(0, nid, HDA_PARAM_OUTPUT_AMP_CAP));
    codec->outamp_cap = res;

    res = hda_command(codec,
                      HDA_CMD_GET_PARAMETER(0, nid, HDA_PARAM_INPUT_AMP_CAP));
    codec->inamp_cap = res;

    for (i = codec->startnode; i < codec->endnode; i++)
        {
        w = widget_get(codec, i);
        if (w == NULL)
            {
            HDA_DBG(HDA_DBG_INFO, "Ghost widget! nid=%d!\n", i);
            }
        else
            {
            w->codec = codec;
            w->nid = i;
            w->enable = 1;
            w->selconn = -1;
            w->pflags = 0;
            w->ossdev = -1;
            w->bindas = -1;
            w->param.eapdbtl = HDA_INVALID;
            widget_parse(w);
            }
        }
    }

/*
 * Recursively commutate specified record source.
 */
LOCAL UINT32 audio_ctl_recsel_comm(PCM_DEVINFO *pdevinfo, UINT32 src, nid_t nid, int depth)
    {
    HDCODEC_ID codec = pdevinfo->codec;
    WIDGET *w, *cw;
    AUDIO_CTL *ctl;
    int i, muted;
    UINT32 res = 0;

    if (depth > HDA_PARSE_MAXDEPTH)
        return (0);

    w = widget_get(codec, nid);
    if (w == NULL || w->enable == 0)
        return (0);

    for (i = 0; i < w->nconns; i++)
        {
        if (w->connsenable[i] == 0)
            continue;
        cw = widget_get(codec, w->conns[i]);
        if (cw == NULL || cw->enable == 0 || cw->bindas == -1)
            continue;
        /* Call recursively to trace signal to it's source if needed. */
        if ((src & cw->ossmask) != 0)
            {
            if (cw->ossdev < 0)
                {
                res |= audio_ctl_recsel_comm(pdevinfo, src,
                                                  w->conns[i], depth + 1);
                } else
                {
                res |= cw->ossmask;
                }
            }
        /* We have two special cases: mixers and others (selectors). */
        if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER)
            {
            ctl = audio_ctl_amp_get(codec,
                                         w->nid, CTL_IN, i, 1);
            if (ctl == NULL) 
                continue;

            /* If we have input control on this node mute them
             * according to requested sources. */
            muted = (src & cw->ossmask) ? 0 : 1;
            if (muted != ctl->forcemute)
                {
                ctl->forcemute = muted;
                audio_ctl_amp_set(ctl,
                                       HDAA_AMP_MUTE_DEFAULT,
                                       HDAA_AMP_VOL_DEFAULT, HDAA_AMP_VOL_DEFAULT);
                }

            }
        else
            {
            if (w->nconns == 1)
                break;
            if ((src & cw->ossmask) == 0)
                continue;
            /* If we found requested source - select it and exit. */
            widget_connection_select(w, i);

            break;
            }
        }
    return (res);
    }


/*
 * Update amplification per pdevinfo per ossdev, calculate summary coefficient
 * and write it to codec, update *left and *right to reflect remaining error.
 */
LOCAL void audio_ctl_dev_set(AUDIO_CTL *ctl, int ossdev,
                                  int mute, int *left, int *right)
    {
    int i, zleft, zright, sleft, sright, smute, lval, rval;

    ctl->devleft[ossdev] = *left;
    ctl->devright[ossdev] = *right;
    ctl->devmute[ossdev] = mute;
    smute = sleft = sright = zleft = zright = 0;
    for (i = 0; i < SOUND_MIXER_NRDEVICES; i++)
        {
        sleft += ctl->devleft[i];
        sright += ctl->devright[i];
        smute |= ctl->devmute[i];
        if (i == ossdev)
            continue;
        zleft += ctl->devleft[i];
        zright += ctl->devright[i];
        }
    lval = QDB2VAL(ctl, sleft);
    rval = QDB2VAL(ctl, sright);
    audio_ctl_amp_set(ctl, smute, lval, rval);
    *left -= VAL2QDB(ctl, lval) - VAL2QDB(ctl, QDB2VAL(ctl, zleft));
    *right -= VAL2QDB(ctl, rval) - VAL2QDB(ctl, QDB2VAL(ctl, zright));
    }

/*
 * Trace signal from source, setting volumes on the way.
 */
LOCAL void audio_ctl_source_volume
    (
    PCM_DEVINFO *pdevinfo,
    int ossdev, nid_t nid, int index,
    int mute, int left, int right, int depth
    )
    {
    HDCODEC_ID codec = pdevinfo->codec;
    WIDGET *w, *wc;
    AUDIO_CTL *ctl;
    int i, j, conns = 0;

    if (depth > HDA_PARSE_MAXDEPTH)
        return;

    w = widget_get(codec, nid);
    if (w == NULL || w->enable == 0)
        return;

    /* Count number of active inputs. */
    if (depth > 0)
        {
        for (j = 0; j < w->nconns; j++)
            {
            if (!w->connsenable[j])
                continue;
            conns++;
            }
        }

    /* If this is not a first step - use input mixer.
       Pins have common input ctl so care must be taken. */
    if (depth > 0 && (conns == 1 ||
                      w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX))
        {
        ctl = audio_ctl_amp_get(codec, w->nid, CTL_IN,
                                     index, 1);
        if (ctl)
            audio_ctl_dev_set(ctl, ossdev, mute, &left, &right);
        }

    /* If widget has own ossdev - not traverse it.
       It will be traversed on it's own. */
    if (w->ossdev >= 0 && depth > 0)
        return;

    /* We must not traverse pin */
    if ((w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT ||
         w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX) &&
        depth > 0)
        return;

    /*
     * If signals mixed, we can't assign controls farther.
     * Ignore this on depth zero. Caller must knows why.
     */
    if (conns > 1 &&
        (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER ||
         w->selconn != index))
        return;

    ctl = audio_ctl_amp_get(codec, w->nid, CTL_OUT, -1, 1);
    if (ctl)
        audio_ctl_dev_set(ctl, ossdev, mute, &left, &right);

    for (i = codec->startnode; i < codec->endnode; i++)
        {
        wc = widget_get(codec, i);
        if (wc == NULL || wc->enable == 0)
            continue;
        for (j = 0; j < wc->nconns; j++)
            {
            if (wc->connsenable[j] && wc->conns[j] == nid)
                {
                audio_ctl_source_volume(pdevinfo, ossdev,
                                             wc->nid, j, mute, left, right, depth + 1);
                }
            }
        }
    return;
    }

/*
 * Trace signal from destination, setting volumes on the way.
 */
LOCAL void audio_ctl_dest_volume
    (
    PCM_DEVINFO *pdevinfo,
    int ossdev,
    nid_t nid,
    int index,
    int mute,
    int left,
    int right,
    int depth
    )
    {
    HDCODEC_ID codec = pdevinfo->codec;
    ASSOC *as = codec->assoc_table;
    WIDGET *w, *wc;
    AUDIO_CTL *ctl;
    int i, j, consumers, cleft, cright;

    if (depth > HDA_PARSE_MAXDEPTH)
        return;

    w = widget_get(codec, nid);
    if (w == NULL || w->enable == 0)
        return;

    if (depth > 0)
        {
        /* If this node produce output for several consumers,
           we can't touch it. */
        consumers = 0;
        for (i = codec->startnode; i < codec->endnode; i++)
            {
            wc = widget_get(codec, i);
            if (wc == NULL || wc->enable == 0)
                continue;
            for (j = 0; j < wc->nconns; j++)
                {
                if (wc->connsenable[j] && wc->conns[j] == nid)
                    consumers++;
                }
            }
        /* The only exception is if real HP redirection is configured
           and this is a duplication point.
           XXX: Actually exception is not completely correct.
           XXX: Duplication point check is not perfect. */
        if ((consumers == 2 && (w->bindas < 0 ||
                                as[w->bindas].hpredir < 0 || as[w->bindas].fakeredir ||
                                (w->bindseqmask & (1 << 15)) == 0)) ||
            consumers > 2)
            return;

        /* Else use it's output mixer. */
        ctl = audio_ctl_amp_get(codec, w->nid,
                                     CTL_OUT, -1, 1);
        if (ctl)
            audio_ctl_dev_set(ctl, ossdev, mute, &left, &right);
        }

    /* We must not traverse pin */
    if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX &&
        depth > 0)
        return;

    for (i = 0; i < w->nconns; i++)
        {
        if (w->connsenable[i] == 0)
            continue;
        if (index >= 0 && i != index)
            continue;
        cleft = left;
        cright = right;
        ctl = audio_ctl_amp_get(codec, w->nid,
                                     CTL_IN, i, 1);
        if (ctl)
            audio_ctl_dev_set(ctl, ossdev, mute, &cleft, &cright);
        audio_ctl_dest_volume(pdevinfo, ossdev, w->conns[i], -1,
                                   mute, cleft, cright, depth + 1);
        }
    }

/*
 * Set volumes for the specified pdevinfo and ossdev.
 */
LOCAL void audio_ctl_dev_volume(PCM_DEVINFO *pdevinfo, unsigned dev)
    {
    HDCODEC_ID codec = pdevinfo->codec;
    WIDGET *w, *cw;
    UINT32 mute;
    int lvol, rvol;
    int i, j;

    mute = 0;
    if (pdevinfo->left[dev] == 0)
        {
        mute |= HDAA_AMP_MUTE_LEFT;
        lvol = -4000;
        } else
        lvol = ((pdevinfo->maxamp[dev] - pdevinfo->minamp[dev]) *
                pdevinfo->left[dev] + 50) / 100 + pdevinfo->minamp[dev];
    if (pdevinfo->right[dev] == 0)
        {
        mute |= HDAA_AMP_MUTE_RIGHT;
        rvol = -4000;
        } else
        rvol = ((pdevinfo->maxamp[dev] - pdevinfo->minamp[dev]) *
                pdevinfo->right[dev] + 50) / 100 + pdevinfo->minamp[dev];
#ifdef HDA_DBG_ON
    HDA_DBG(HDA_DBG_INFO, "audio_ctl_dev_volume(): lvol= x%x, rvol= x%x\n", lvol, rvol);
#endif
    for (i = codec->startnode; i < codec->endnode; i++)
        {
        w = widget_get(codec, i);
        if (w == NULL || w->enable == 0)
            continue;
        if (w->bindas < 0 && pdevinfo->index != 0)
            continue;
        if (w->bindas != pdevinfo->playas &&
            w->bindas != pdevinfo->recas)
            continue;
        if (dev == SOUND_MIXER_RECLEV &&
            w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT)
            {
            audio_ctl_dest_volume(pdevinfo, dev,
                                       w->nid, -1, mute, lvol, rvol, 0);
            continue;
            }
        if (dev == SOUND_MIXER_VOLUME &&
            w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX &&
            codec->assoc_table[w->bindas].dir == CTL_OUT)
            {
            audio_ctl_dest_volume(pdevinfo, dev,
                                       w->nid, -1, mute, lvol, rvol, 0);
            continue;
            }
        if (dev == SOUND_MIXER_IGAIN &&
            w->pflags & HDAA_ADC_MONITOR)
            {
            for (j = 0; j < w->nconns; j++)
                {
                if (!w->connsenable[j])
                    continue;
                cw = widget_get(codec, w->conns[j]);
                if (cw == NULL || cw->enable == 0)
                    continue;
                if (cw->bindas == -1)
                    continue;
                if (cw->bindas >= 0 &&
                    codec->assoc_table[cw->bindas].dir != CTL_IN)
                    continue;
                audio_ctl_dest_volume(pdevinfo, dev,
                                           w->nid, j, mute, lvol, rvol, 0);
                }
            continue;
            }
        if (w->ossdev != dev)
            continue;
        audio_ctl_source_volume(pdevinfo, dev,
                                     w->nid, -1, mute, lvol, rvol, 0);
        if (dev == SOUND_MIXER_IMIX && (w->pflags & HDAA_IMIX_AS_DST))
            audio_ctl_dest_volume(pdevinfo, dev,
                                       w->nid, -1, mute, lvol, rvol, 0);
        }
    }

void hdAudioSetPinCtrl (HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int val)
    {
    UINT32 verb;

    verb = hdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_SET_PIN_WIDGET_CTRL,
                              val);
    hdAudioCommand(pDrvCtrl, cad, verb);
    }    

#ifdef  HDA_DBG_ON
#include "hdAudioShow.inc"
#include "hdAudioDebug.inc"
#endif

