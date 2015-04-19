/* hdAudio.h - HD Audio driver header */

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
01a,20jan12,gkw  written.
*/

#ifndef __INChdAudioh
#define __INChdAudioh

extern void vxbHdAudioRegister (void);


#define VIA_VX900_DEVICE_ID 0x3288
#define VIA_VENDOR_ID   0x1106

#define WIDGET_TYPE_ROOT        -1


#define HDAC_CODEC_NUM_MAX      16
#define ASSOC_NUM_MAX           16
#define SEQ_NUM_MAX             16

#define HDAA_AMP_VOL_DEFAULT    (-1)
#define HDAA_AMP_MUTE_DEFAULT   (0xffffffff)
#define HDAA_AMP_MUTE_NONE  (0)
#define HDAA_AMP_MUTE_LEFT  (1 << 0)
#define HDAA_AMP_MUTE_RIGHT (1 << 1)
#define HDAA_AMP_MUTE_ALL   (HDAA_AMP_MUTE_LEFT | HDAA_AMP_MUTE_RIGHT)

#define HDAA_AMP_LEFT_MUTED(v)  ((v) & (HDAA_AMP_MUTE_LEFT))
#define HDAA_AMP_RIGHT_MUTED(v) (((v) & HDAA_AMP_MUTE_RIGHT) >> 1)


/* Widget in playback receiving signal from recording. */
#define HDAA_ADC_MONITOR        (1 << 0)
/* Input mixer widget needs volume control as destination. */
#define HDAA_IMIX_AS_DST        (2 << 0)

#define HDAA_CTL_OUT    1
#define HDAA_CTL_IN     2

#define HDA_MAX_CONNS   32
#define HDA_MAX_NAMELEN 32

#define HDA_INVALID     0xffffffff
#define HDA_PARSE_MAXDEPTH 10


/* HD Audio control structures */

typedef int nid_t;
typedef int cad_t;
typedef struct hdcodec_t* HDCODEC_ID;


#define VXB_HDA_MSG_TYPE_UNLINK 1
#define VXB_HDA_MSG_TYPE_UNSOLQ 2

typedef struct vxbHdaMsg
    {
    int                 type;
    } VXB_HDA_MSG;

#define VXB_HDA_MSG_SIZE sizeof(VXB_HDA_MSG)

typedef struct dma_object_t {
        VXB_DMA_TAG_ID      dma_tag;
        VXB_DMA_MAP_ID      dma_map;
        bus_addr_t          dma_paddr;
        bus_size_t          dma_size;
        caddr_t             dma_vaddr;
    } DMA_OBJECT;

#define HDAA_CHN_RUNNING    0x00000001
#define HDAA_CHN_SUSPEND    0x00000002


typedef struct pcm_devinfo_t
    {
    HDCODEC_ID          codec;
    DSP_DEV             *pDspDev;
    MIXER_DEV           *pMixerDev;
    SND_MIXER           *mixer;
    int                 index;
    int                 registered;
    int                 playas, recas;
    u_char              left[SOUND_MIXER_NRDEVICES];
    u_char              right[SOUND_MIXER_NRDEVICES];
    int                 minamp[SOUND_MIXER_NRDEVICES]; /* Minimal amps in 1/4dB. */
    int                 maxamp[SOUND_MIXER_NRDEVICES]; /* Maximal amps in 1/4dB. */
    int                 chan_size;
    int                 chan_blkcnt;
    u_char              digital;
    UINT32              ossmask;  /* Mask of supported OSS devices. */
    UINT32              recsrc;       /* Mask of supported OSS sources. */
    int                 autorecsrc;
    } PCM_DEVINFO;

typedef struct chan_t {
    HDCODEC_ID          codec;
    struct snd_buf      *b;
    struct pcm_channel  *c;
    struct pcmchan_caps caps;
    PCM_DEVINFO*        pcm_dev;
    UINT32              spd, fmt, fmtlist[32], pcmrates[16];
    UINT32              supp_stream_formats, supp_pcm_size_rate;
    UINT32              blkcnt, blksz;
    UINT32*             dmapos;
    UINT32              flags;
    int                 dir;
    int                 off;
    int                 sid;
    int                 bit16, bit32;
    int                 channels;       /* Number of audio channels. */
    int                 as;             /* Number of association. */
    int                 asindex;        /* Index within association. */
    nid_t               io[16];
    UINT8               stripecap;      /* AND of stripecap of all ios. */
    UINT8               stripectl;      /* stripe to use to all ios. */
} CHAN;

#define MINQDB(ctl)                         \
    ((0 - (ctl)->offset) * ((ctl)->size + 1))

#define MAXQDB(ctl)                         \
    (((ctl)->step - (ctl)->offset) * ((ctl)->size + 1))

#define RANGEQDB(ctl)                           \
    ((ctl)->step * ((ctl)->size + 1))

#define VAL2QDB(ctl, val)                       \
    (((ctl)->size + 1) * ((int)(val) - (ctl)->offset))

#define QDB2VAL(ctl, qdb)                       \
    MAX(MIN((((qdb) + (ctl)->size / 2 * ((qdb) > 0 ? 1 : -1)) / \
     ((ctl)->size + 1) + (ctl)->offset), (ctl)->step), 0)


typedef struct audio_ctl_t
    {
    struct widget_t *   widget;
    struct widget_t *   childwidget;
    int                 enable;
    int                 index, dir, ndir;
    int                 mute, step, size, offset;
    int                 left, right, forcemute;
    UINT32              muted;
    UINT32              ossmask;
    int                 devleft[SOUND_MIXER_NRDEVICES];
    int                 devright[SOUND_MIXER_NRDEVICES];
    int                 devmute[SOUND_MIXER_NRDEVICES];
    } AUDIO_CTL;

typedef struct assoc_t
    {
    UINT8               numPin;
    nid_t               pins[16];
    nid_t               dacs[2][16];
    int                 index;
    u_char              enable;
    u_char              dir;            /* record or play indicator */
    u_char              pincnt;
    u_char              fakeredir;
    u_char              digital;
    UINT16              pinset;
    nid_t               hpredir;
    int                 num_chans;
    int                 chans[2];
    int                 location;
    int                 mixed;
    PCM_DEVINFO         *pcm_dev;
    } ASSOC;

struct codec_t;

typedef struct widget_t
    {
    NODE                node;
    LIST                widgetList;
    nid_t               nid;
    int                 type;

    HDCODEC_ID          codec;
    int                 enable;
    int                 bindas;
    int                 bindseqmask;
    int                 nconns, selconn;
    nid_t               conns[32];
    UINT8               connsenable[32];

    int unsol;

    struct {
        UINT32 widget_cap;
        UINT32 outamp_cap;
        UINT32 inamp_cap;
        UINT32 supp_stream_formats;
        UINT32 supp_pcm_size_rate;
        UINT32 eapdbtl;
    } param;

    union {
        struct {
            UINT32 config;
            UINT32 original;
            UINT32 newconf;
            UINT32 cap;
            UINT32 ctrl;
            int connected;
        } pin;
        struct {
            UINT8   stripecap;
        } conv;
    } wclass;
    
    
    int                 waspin;
    UINT32              pflags;
    int                 ossdev;
    UINT32              ossmask;
    } WIDGET;

typedef struct hdcodec_t
    {
    UINT16              vendor_id;
    UINT16              device_id;
    UINT8               revision_id;
    UINT8               stepping_id;
    int                 pending;
    UINT32              response;
    WIDGET              root;
    
    int                 ascnt;
    ASSOC*              assoc_table;

    void *              tags[64];

    UINT32              outamp_cap;
    UINT32              inamp_cap;
    UINT32              supp_stream_formats;
    UINT32              supp_pcm_size_rate;
    int                 nodecnt;
    int                 startnode;
    int                 endnode;
    nid_t               nid;
    int                 cad;

    int                 ctlcnt;
    AUDIO_CTL           *ctl;

    int                 num_devs;
    PCM_DEVINFO         *pcm_dev_table;

    int                 num_hdaa_chans;
    CHAN                *hdaa_chan_table;
    } HDCODEC;

typedef struct stream_t
    {
    HDCODEC_ID          codec;
    int                 dir;
    int                 stripe;
    int                 blksz;
    
    int                 running;
    int                 stream;
    UINT16              format;
    UINT8*              buf;
    DMA_OBJECT          bdl;
    } STREAM;


typedef struct hdaDrvCtrl
    {
    void *              regBase;
    int                 intLvl;
    VOIDFUNCPTR *       intVector;
    SEM_ID              mutex;

/*
* Items pulled from the FreeBSD hda_softc structure
*/

    int                 num_iss;
    int                 num_oss;
    int                 num_bss;
    int                 num_ss;
    int                 num_sdo;
    int                 support_64bit;

    int                 corb_size;
    DMA_OBJECT          corb_dma;
    int                 corb_wp;

    int                 rirb_size;
    DMA_OBJECT          rirb_dma;
    int                 rirb_rp;

    DMA_OBJECT          pos_dma;

    /* Polling */
    int                 poll_ival;

    int                 unsol_registered;

#define HDAC_UNSOLQ_MAX     64
#define HDAC_UNSOLQ_READY   0
#define HDAC_UNSOLQ_BUSY    1
    int                 unsolq_rp;
    int                 unsolq_wp;
    int                 unsolq_st;
    UINT32              unsolq[HDAC_UNSOLQ_MAX];

    MSG_Q_ID            unsolq_msgQ;
    
    STREAM              *streams;

    int                 num_codec;
    HDCODEC_ID          codec_table[HDAC_CODEC_NUM_MAX];
    VXB_DMA_TAG_ID      sndbuf_dma_tag;
    VXB_DMA_TAG_ID      parentTag;
    } HDA_DRV_CTRL;

#define device_get_softc()   gHdaDrvCtrl
#define device_get_codec(pDrvCtrl, cad) (pDrvCtrl->codec_table[(int)cad])


/* HDA driver name */

#define HDA_NAME              "hdaudio"

/* HD Audio  monitor task name */

#define HDA_MON_TASK_NAME     "hdaMon"

/* HD Audio  monitor task priority */

#define HDA_MON_TASK_PRI      100

/* HD Audio  monitor task stack size */

#define HDA_MON_TASK_STACK    8192

/* HD Audio  monitor poll task delay */
#define HDA_MON_DELAY_SECS    2

#define HDA_BAR(p)         ((HDA_DRV_CTRL *)(p)->pDrvCtrl)->regBase

/*
 * Yuk.  This should be done in a more portable manner.
 */
#define BUS_SPACE_MAXADDR_32BIT 0xffffffff
#define BUS_SPACE_MAXSIZE_32BIT 0xffffffff
#define BUS_SPACE_MAXADDR       BUS_SPACE_MAXADDR_32BIT

#define HDA_CMD_VERB_MASK               0x000fffff
#define HDA_CMD_VERB_SHIFT              0
#define HDA_CMD_VERB_4BIT_SHIFT         16
#define HDA_CMD_VERB_12BIT_SHIFT        8

/* verbs */

#define HDA_CMD_VERB_GET_PARAMETER                      0xf00
#define HDA_CMD_VERB_GET_CONN_SELECT_CONTROL            0xf01
#define HDA_CMD_VERB_SET_CONN_SELECT_CONTROL            0x701
#define HDA_CMD_VERB_GET_CONN_LIST_ENTRY                0xf02
#define HDA_CMD_VERB_GET_PROCESSING_STATE               0xf03
#define HDA_CMD_VERB_SET_PROCESSING_STATE               0x703
#define HDA_CMD_VERB_GET_COEFF_INDEX                    0xd
#define HDA_CMD_VERB_SET_COEFF_INDEX                    0x5
#define HDA_CMD_VERB_GET_PROCESSING_COEFF               0xc
#define HDA_CMD_VERB_SET_PROCESSING_COEFF               0x4
#define HDA_CMD_VERB_GET_AMP_GAIN_MUTE                  0xb
#define HDA_CMD_VERB_SET_AMP_GAIN_MUTE                  0x3
#define HDA_CMD_VERB_GET_CONV_FMT                       0xa
#define HDA_CMD_VERB_SET_CONV_FMT                       0x2
#define HDA_CMD_VERB_GET_DIGITAL_CONV_FMT1              0xf0d
#define HDA_CMD_VERB_GET_DIGITAL_CONV_FMT2              0xf0e
#define HDA_CMD_VERB_SET_DIGITAL_CONV_FMT1              0x70d
#define HDA_CMD_VERB_SET_DIGITAL_CONV_FMT2              0x70e
#define HDA_CMD_VERB_GET_POWER_STATE                    0xf05
#define HDA_CMD_VERB_SET_POWER_STATE                    0x705
#define HDA_CMD_VERB_GET_CONV_STREAM_CHAN               0xf06
#define HDA_CMD_VERB_SET_CONV_STREAM_CHAN               0x706
#define HDA_CMD_VERB_GET_INPUT_CONVERTER_SDI_SELECT     0xf04
#define HDA_CMD_VERB_SET_INPUT_CONVERTER_SDI_SELECT     0x704
#define HDA_CMD_VERB_GET_PIN_WIDGET_CTRL                0xf07
#define HDA_CMD_VERB_SET_PIN_WIDGET_CTRL                0x707
#define HDA_CMD_VERB_GET_UNSOLICITED_RESPONSE           0xf08
#define HDA_CMD_VERB_SET_UNSOLICITED_RESPONSE           0x708
#define HDA_CMD_VERB_GET_PIN_SENSE                      0xf09
#define HDA_CMD_VERB_SET_PIN_SENSE                      0x709
#define HDA_CMD_VERB_GET_EAPD_BTL_ENABLE                0xf0c
#define HDA_CMD_VERB_SET_EAPD_BTL_ENABLE                0x70c
#define HDA_CMD_VERB_GET_GPI_DATA                       0xf10
#define HDA_CMD_VERB_SET_GPI_DATA                       0x710
#define HDA_CMD_VERB_GET_GPI_WAKE_ENABLE_MASK           0xf11
#define HDA_CMD_VERB_SET_GPI_WAKE_ENABLE_MASK           0x711
#define HDA_CMD_VERB_GET_GPI_UNSOLICITED_ENABLE_MASK    0xf12
#define HDA_CMD_VERB_SET_GPI_UNSOLICITED_ENABLE_MASK    0x712
#define HDA_CMD_VERB_GET_GPI_STICKY_MASK                0xf13
#define HDA_CMD_VERB_SET_GPI_STICKY_MASK                0x713
#define HDA_CMD_VERB_GET_GPO_DATA                       0xf14
#define HDA_CMD_VERB_SET_GPO_DATA                       0x714
#define HDA_CMD_VERB_GET_GPIO_DATA                      0xf15
#define HDA_CMD_VERB_SET_GPIO_DATA                      0x715
#define HDA_CMD_VERB_GET_GPIO_ENABLE_MASK               0xf16
#define HDA_CMD_VERB_SET_GPIO_ENABLE_MASK               0x716
#define HDA_CMD_VERB_GET_GPIO_DIRECTION                 0xf17
#define HDA_CMD_VERB_SET_GPIO_DIRECTION                 0x717
#define HDA_CMD_VERB_GET_GPIO_WAKE_ENABLE_MASK          0xf18
#define HDA_CMD_VERB_SET_GPIO_WAKE_ENABLE_MASK          0x718
#define HDA_CMD_VERB_GET_GPIO_UNSOLICITED_ENABLE_MASK   0xf19
#define HDA_CMD_VERB_SET_GPIO_UNSOLICITED_ENABLE_MASK   0x719
#define HDA_CMD_VERB_GET_GPIO_STICKY_MASK               0xf1a
#define HDA_CMD_VERB_SET_GPIO_STICKY_MASK               0x71a
#define HDA_CMD_VERB_GET_BEEP_GENERATION                0xf0a
#define HDA_CMD_VERB_SET_BEEP_GENERATION                0x70a
#define HDA_CMD_VERB_GET_VOLUME_KNOB                    0xf0f
#define HDA_CMD_VERB_SET_VOLUME_KNOB                    0x70f
#define HDA_CMD_VERB_GET_SUBSYSTEM_ID                   0xf20
#define HDA_CMD_VERB_SET_SUSBYSTEM_ID1                  0x720
#define HDA_CMD_VERB_SET_SUBSYSTEM_ID2                  0x721
#define HDA_CMD_VERB_SET_SUBSYSTEM_ID3                  0x722
#define HDA_CMD_VERB_SET_SUBSYSTEM_ID4                  0x723
#define HDA_CMD_VERB_GET_CONFIGURATION_DEFAULT          0xf1c
#define HDA_CMD_VERB_SET_CONFIGURATION_DEFAULT1         0x71c
#define HDA_CMD_VERB_SET_CONFIGURATION_DEFAULT2         0x71d
#define HDA_CMD_VERB_SET_CONFIGURATION_DEFAULT3         0x71e
#define HDA_CMD_VERB_SET_CONFIGURATION_DEFAULT4         0x71f
#define HDA_CMD_VERB_GET_STRIPE_CONTROL                 0xf24
#define HDA_CMD_VERB_SET_STRIPE_CONTROL                 0x724
#define HDA_CMD_VERB_GET_CONV_CHAN_COUNT                0xf2d
#define HDA_CMD_VERB_SET_CONV_CHAN_COUNT                0x72d 
#define HDA_CMD_VERB_GET_HDMI_DIP_SIZE                  0xf2e 
#define HDA_CMD_VERB_GET_HDMI_ELDD                      0xf2f 
#define HDA_CMD_VERB_GET_HDMI_DIP_INDEX                 0xf30 
#define HDA_CMD_VERB_SET_HDMI_DIP_INDEX                 0x730 
#define HDA_CMD_VERB_GET_HDMI_DIP_DATA                  0xf31 
#define HDA_CMD_VERB_SET_HDMI_DIP_DATA                  0x731 
#define HDA_CMD_VERB_GET_HDMI_DIP_XMIT                  0xf32 
#define HDA_CMD_VERB_SET_HDMI_DIP_XMIT                  0x732 
#define HDA_CMD_VERB_GET_HDMI_CP_CTRL                   0xf33 
#define HDA_CMD_VERB_SET_HDMI_CP_CTRL                   0x733 
#define HDA_CMD_VERB_GET_HDMI_CHAN_SLOT                 0xf34 
#define HDA_CMD_VERB_SET_HDMI_CHAN_SLOT                 0x734 
#define HDA_CMD_VERB_FUNCTION_RESET                     0x7ff

/* parameters */

#define HDA_PARAM_VENDOR_ID                             0x00
#define HDA_PARAM_REVISION_ID                           0x02
#define HDA_PARAM_SUB_NODE_COUNT                        0x04
#define HDA_PARAM_FCT_GRP_TYPE                          0x05
#define HDA_PARAM_AUDIO_FCT_GRP_CAP                     0x08
#define HDA_PARAM_AUDIO_WIDGET_CAP                      0x09
#define HDA_PARAM_SUPP_PCM_SIZE_RATE                    0x0a
#define HDA_PARAM_SUPP_STREAM_FORMATS                   0x0b
#define HDA_PARAM_PIN_CAP                               0x0c
#define HDA_PARAM_INPUT_AMP_CAP                         0x0d
#define HDA_PARAM_CONN_LIST_LENGTH                      0x0e
#define HDA_PARAM_SUPP_POWER_STATES                     0x0f
#define HDA_PARAM_PROCESSING_CAP                        0x10
#define HDA_PARAM_GPIO_COUNT                            0x11
#define HDA_PARAM_VOLUME_KNOB_CAP                       0x13

/* HD Audio controller register offsets */

#define HDAC_GCAP       0x00
#define HDAC_VMIN       0x02
#define HDAC_VMAJ       0x03
#define HDAC_OUTPAY     0x04
#define HDAC_INPAY      0x06
#define HDAC_GCTL       0x08
#define HDAC_WAKEEN     0x0c
#define HDAC_STATESTS   0x0e
#define HDAC_GSTS       0x10
#define HDAC_OUTSTRMPAY 0x18
#define HDAC_INSTRMPAY  0x1a
#define HDAC_INTCTL     0x20
#define HDAC_INTSTS     0x24
#define HDAC_WALCLK     0x30
#define HDAC_SSYNC      0x38
#define HDAC_CORBLBASE  0x40
#define HDAC_CORBUBASE  0x44
#define HDAC_CORBWP     0x48
#define HDAC_CORBRP     0x4a
#define HDAC_CORBCTL    0x4c
#define HDAC_CORBSTS    0x4d
#define HDAC_CORBSIZE   0x4e
#define HDAC_RIRBLBASE  0x50
#define HDAC_RIRBUBASE  0x54
#define HDAC_RIRBWP     0x58
#define HDAC_RINTCNT    0x5a
#define HDAC_RIRBCTL    0x5c
#define HDAC_RIRBSTS    0x5d
#define HDAC_RIRBSIZE   0x5e
#define HDAC_ICOI       0x60
#define HDAC_ICII       0x64
#define HDAC_ICIS       0x68
#define HDAC_DPIBLBASE  0x70
#define HDAC_DPIBUBASE  0x74
#define HDAC_SDCTL0     0x80
#define HDAC_SDCTL1     0x81
#define HDAC_SDCTL2     0x82
#define HDAC_SDSTS      0x83
#define HDAC_SDLPIB     0x84
#define HDAC_SDCBL      0x88
#define HDAC_SDLVI      0x8C
#define HDAC_SDFIFOS    0x90
#define HDAC_SDFMT      0x92
#define HDAC_SDBDPL     0x98
#define HDAC_SDBDPU     0x9C

#define _HDAC_ISDOFFSET(n, iss, oss)    (0x80 + ((n) * 0x20))
#define _HDAC_ISDCTL(n, iss, oss)   (0x00 + _HDAC_ISDOFFSET(n, iss, oss))
#define _HDAC_ISDSTS(n, iss, oss)   (0x03 + _HDAC_ISDOFFSET(n, iss, oss))
#define _HDAC_ISDPICB(n, iss, oss)  (0x04 + _HDAC_ISDOFFSET(n, iss, oss))
#define _HDAC_ISDCBL(n, iss, oss)   (0x08 + _HDAC_ISDOFFSET(n, iss, oss))
#define _HDAC_ISDLVI(n, iss, oss)   (0x0c + _HDAC_ISDOFFSET(n, iss, oss))
#define _HDAC_ISDFIFOD(n, iss, oss) (0x10 + _HDAC_ISDOFFSET(n, iss, oss))
#define _HDAC_ISDFMT(n, iss, oss)   (0x12 + _HDAC_ISDOFFSET(n, iss, oss))
#define _HDAC_ISDBDPL(n, iss, oss)  (0x18 + _HDAC_ISDOFFSET(n, iss, oss))
#define _HDAC_ISDBDPU(n, iss, oss)  (0x1c + _HDAC_ISDOFFSET(n, iss, oss))

#define _HDAC_OSDOFFSET(n, iss, oss)    (0x80 + ((iss) * 0x20) + ((n) * 0x20))
#define _HDAC_OSDCTL(n, iss, oss)   (0x00 + _HDAC_OSDOFFSET(n, iss, oss))
#define _HDAC_OSDSTS(n, iss, oss)   (0x03 + _HDAC_OSDOFFSET(n, iss, oss))
#define _HDAC_OSDPICB(n, iss, oss)  (0x04 + _HDAC_OSDOFFSET(n, iss, oss))
#define _HDAC_OSDCBL(n, iss, oss)   (0x08 + _HDAC_OSDOFFSET(n, iss, oss))
#define _HDAC_OSDLVI(n, iss, oss)   (0x0c + _HDAC_OSDOFFSET(n, iss, oss))
#define _HDAC_OSDFIFOD(n, iss, oss) (0x10 + _HDAC_OSDOFFSET(n, iss, oss))
#define _HDAC_OSDFMT(n, iss, oss)   (0x12 + _HDAC_OSDOFFSET(n, iss, oss))
#define _HDAC_OSDBDPL(n, iss, oss)  (0x18 + _HDAC_OSDOFFSET(n, iss, oss))
#define _HDAC_OSDBDPU(n, iss, oss)  (0x1c + _HDAC_OSDOFFSET(n, iss, oss))

#define _HDAC_BSDOFFSET(n, iss, oss)    (0x80 + ((iss) * 0x20) + ((oss) * 0x20) + ((n) * 0x20))
#define _HDAC_BSDCTL(n, iss, oss)   (0x00 + _HDAC_BSDOFFSET(n, iss, oss))
#define _HDAC_BSDSTS(n, iss, oss)   (0x03 + _HDAC_BSDOFFSET(n, iss, oss))
#define _HDAC_BSDPICB(n, iss, oss)  (0x04 + _HDAC_BSDOFFSET(n, iss, oss))
#define _HDAC_BSDCBL(n, iss, oss)   (0x08 + _HDAC_BSDOFFSET(n, iss, oss))
#define _HDAC_BSDLVI(n, iss, oss)   (0x0c + _HDAC_BSDOFFSET(n, iss, oss))
#define _HDAC_BSDFIFOD(n, iss, oss) (0x10 + _HDAC_BSDOFFSET(n, iss, oss))
#define _HDAC_BSDFMT(n, iss, oss)   (0x12 + _HDAC_BSDOFFSET(n, iss, oss))
#define _HDAC_BSDBDPL(n, iss, oss)  (0x18 + _HDAC_BSDOFFSET(n, iss, oss))
#define _HDAC_BSDBDBU(n, iss, oss)  (0x1c + _HDAC_BSDOFFSET(n, iss, oss))

/****************************************************************************
 * HDA Controller Register Fields
 ****************************************************************************/

/* GCAP - Global Capabilities */
#define HDAC_GCAP_64OK          0x0001
#define HDAC_GCAP_NSDO_MASK     0x0006
#define HDAC_GCAP_NSDO_SHIFT        1
#define HDAC_GCAP_BSS_MASK      0x00f8
#define HDAC_GCAP_BSS_SHIFT     3
#define HDAC_GCAP_ISS_MASK      0x0f00
#define HDAC_GCAP_ISS_SHIFT     8
#define HDAC_GCAP_OSS_MASK      0xf000
#define HDAC_GCAP_OSS_SHIFT     12

#define HDAC_GCAP_NSDO_1SDO     0x00
#define HDAC_GCAP_NSDO_2SDO     0x02
#define HDAC_GCAP_NSDO_4SDO     0x04

#define HDAC_GCAP_BSS(gcap)                     \
    (((gcap) & HDAC_GCAP_BSS_MASK) >> HDAC_GCAP_BSS_SHIFT)
#define HDAC_GCAP_ISS(gcap)                     \
    (((gcap) & HDAC_GCAP_ISS_MASK) >> HDAC_GCAP_ISS_SHIFT)
#define HDAC_GCAP_OSS(gcap)                     \
    (((gcap) & HDAC_GCAP_OSS_MASK) >> HDAC_GCAP_OSS_SHIFT)
#define HDAC_GCAP_NSDO(gcap)                        \
    (((gcap) & HDAC_GCAP_NSDO_MASK) >> HDAC_GCAP_NSDO_SHIFT)

/* GCTL - Global Control */
#define HDAC_GCTL_CRST          0x00000001
#define HDAC_GCTL_FCNTRL        0x00000002
#define HDAC_GCTL_UNSOL         0x00000100

/* WAKEEN - Wake Enable */
#define HDAC_WAKEEN_SDIWEN_MASK     0x7fff
#define HDAC_WAKEEN_SDIWEN_SHIFT    0

/* STATESTS - State Change Status */
#define HDAC_STATESTS_SDIWAKE_MASK  0x7fff
#define HDAC_STATESTS_SDIWAKE_SHIFT 0

#define HDAC_STATESTS_SDIWAKE(statests, n)              \
    (((((statests) & HDAC_STATESTS_SDIWAKE_MASK) >>         \
    HDAC_STATESTS_SDIWAKE_SHIFT) >> (n)) & 0x0001)

/* GSTS - Global Status */
#define HDAC_GSTS_FSTS          0x0002

/* INTCTL - Interrut Control */
#define HDAC_INTCTL_SIE_MASK        0x3fffffff
#define HDAC_INTCTL_SIE_SHIFT       0
#define HDAC_INTCTL_CIE         0x40000000
#define HDAC_INTCTL_GIE         0x80000000

/* INTSTS - Interrupt Status */
#define HDAC_INTSTS_SIS_MASK        0x3fffffff
#define HDAC_INTSTS_SIS_SHIFT       0
#define HDAC_INTSTS_CIS         0x40000000
#define HDAC_INTSTS_GIS         0x80000000

/* SSYNC - Stream Synchronization */
#define HDAC_SSYNC_SSYNC_MASK       0x3fffffff
#define HDAC_SSYNC_SSYNC_SHIFT      0

/* CORBWP - CORB Write Pointer */
#define HDAC_CORBWP_CORBWP_MASK     0x00ff
#define HDAC_CORBWP_CORBWP_SHIFT    0

/* CORBRP - CORB Read Pointer */
#define HDAC_CORBRP_CORBRP_MASK     0x00ff
#define HDAC_CORBRP_CORBRP_SHIFT    0
#define HDAC_CORBRP_CORBRPRST       0x8000

/* CORBCTL - CORB Control */
#define HDAC_CORBCTL_CMEIE      0x01
#define HDAC_CORBCTL_CORBRUN        0x02

/* CORBSTS - CORB Status */
#define HDAC_CORBSTS_CMEI       0x01

/* CORBSIZE - CORB Size */
#define HDAC_CORBSIZE_CORBSIZE_MASK 0x03
#define HDAC_CORBSIZE_CORBSIZE_SHIFT    0
#define HDAC_CORBSIZE_CORBSZCAP_MASK    0xf0
#define HDAC_CORBSIZE_CORBSZCAP_SHIFT   4

#define HDAC_CORBSIZE_CORBSIZE_2    0x00
#define HDAC_CORBSIZE_CORBSIZE_16   0x01
#define HDAC_CORBSIZE_CORBSIZE_256  0x02

#define HDAC_CORBSIZE_CORBSZCAP_2   0x10
#define HDAC_CORBSIZE_CORBSZCAP_16  0x20
#define HDAC_CORBSIZE_CORBSZCAP_256 0x40

#define HDAC_CORBSIZE_CORBSIZE(corbsize)                \
    (((corbsize) & HDAC_CORBSIZE_CORBSIZE_MASK) >> HDAC_CORBSIZE_CORBSIZE_SHIFT)

/* RIRBWP - RIRB Write Pointer */
#define HDAC_RIRBWP_RIRBWP_MASK     0x00ff
#define HDAC_RIRBWP_RIRBWP_SHIFT    0
#define HDAC_RIRBWP_RIRBWPRST       0x8000

/* RINTCTN - Response Interrupt Count */
#define HDAC_RINTCNT_MASK       0x00ff
#define HDAC_RINTCNT_SHIFT      0

/* RIRBCTL - RIRB Control */
#define HDAC_RIRBCTL_RINTCTL        0x01
#define HDAC_RIRBCTL_RIRBDMAEN      0x02
#define HDAC_RIRBCTL_RIRBOIC        0x04

/* RIRBSTS - RIRB Status */
#define HDAC_RIRBSTS_RINTFL     0x01
#define HDAC_RIRBSTS_RIRBOIS        0x04

/* RIRBSIZE - RIRB Size */
#define HDAC_RIRBSIZE_RIRBSIZE_MASK 0x03
#define HDAC_RIRBSIZE_RIRBSIZE_SHIFT    0
#define HDAC_RIRBSIZE_RIRBSZCAP_MASK    0xf0
#define HDAC_RIRBSIZE_RIRBSZCAP_SHIFT   4

#define HDAC_RIRBSIZE_RIRBSIZE_2    0x00
#define HDAC_RIRBSIZE_RIRBSIZE_16   0x01
#define HDAC_RIRBSIZE_RIRBSIZE_256  0x02

#define HDAC_RIRBSIZE_RIRBSZCAP_2   0x10
#define HDAC_RIRBSIZE_RIRBSZCAP_16  0x20
#define HDAC_RIRBSIZE_RIRBSZCAP_256 0x40

#define HDAC_RIRBSIZE_RIRBSIZE(rirbsize)                \
    (((rirbsize) & HDAC_RIRBSIZE_RIRBSIZE_MASK) >> HDAC_RIRBSIZE_RIRBSIZE_SHIFT)

/* DPLBASE - DMA Position Lower Base Address */
#define HDAC_DPLBASE_DPLBASE_MASK   0xffffff80
#define HDAC_DPLBASE_DPLBASE_SHIFT  7
#define HDAC_DPLBASE_DPLBASE_DMAPBE 0x00000001

/* SDCTL - Stream Descriptor Control */
#define HDAC_SDCTL_SRST         0x000001
#define HDAC_SDCTL_RUN          0x000002
#define HDAC_SDCTL_IOCE         0x000004
#define HDAC_SDCTL_FEIE         0x000008
#define HDAC_SDCTL_DEIE         0x000010
#define HDAC_SDCTL2_STRIPE_MASK     0x03
#define HDAC_SDCTL2_STRIPE_SHIFT    0
#define HDAC_SDCTL2_TP          0x04
#define HDAC_SDCTL2_DIR         0x08
#define HDAC_SDCTL2_STRM_MASK       0xf0
#define HDAC_SDCTL2_STRM_SHIFT      4

#define HDAC_SDSTS_DESE         (1 << 4)
#define HDAC_SDSTS_FIFOE        (1 << 3)
#define HDAC_SDSTS_BCIS         (1 << 2)


#endif /* __INChdAudioh */

