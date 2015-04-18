#include <vxWorks.h>
#include <stdio.h>
#include <semLib.h>
#include <sysLib.h>
#include <taskLib.h>
#include <vxBusLib.h>
#include <cacheLib.h>
#include <ioLib.h>
#include <iosLib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <hwif/vxbus/vxBus.h>
#include <hwif/vxbus/hwConf.h>
#include <hwif/util/vxbParamSys.h>
#include <hwif/util/vxbDmaBufLib.h>

#include <hwif/vxbus/vxbPciLib.h>
#include <hwif/vxbus/hwConf.h>
#include <drv/pci/pciConfigLib.h>
#include <drv/pci/pciIntLib.h>

#include <vxbus/vxbAccess.h>

/* imports from the FreeBSD snd_hda driver... */
#include "audio/vxbHdWidget.h"

#include "audio/ossAudio.h"
#include "audio/vxbHdAudio.h"
/* #include "bucket.h" */


#define NEW_HDA_DRV_CTRL HDA_DRV_CTRL
    
#if 0
#define HDA_CMD_VERB_MASK               0x000fffff
#define HDA_CMD_VERB_SHIFT              0
#define HDA_CMD_NID_MASK                0x0ff00000
#define HDA_CMD_NID_SHIFT               20
#define HDA_CMD_CAD_MASK                0xf0000000
#define HDA_CMD_CAD_SHIFT               28

#define HDA_CMD_VERB_4BIT_SHIFT             16
#define HDA_CMD_VERB_12BIT_SHIFT            8

#define HDA_CMD_VERB_4BIT(verb, payload)            \
    (((verb) << HDA_CMD_VERB_4BIT_SHIFT) | (payload))

#define HDA_CMD_4BIT(cad, nid, verb, payload)       \
    (((cad) << HDA_CMD_CAD_SHIFT) |                 \
    ((nid) << HDA_CMD_NID_SHIFT) |                  \
    (HDA_CMD_VERB_4BIT((verb), (payload))))

#define HDA_CMD_VERB_12BIT(verb, payload)           \
    (((verb) << HDA_CMD_VERB_12BIT_SHIFT) | (payload))

#define HDA_CMD_12BIT(cad, nid, verb, payload)      \
    (((cad) << HDA_CMD_CAD_SHIFT) |                 \
    ((nid) << HDA_CMD_NID_SHIFT) |                  \
    (HDA_CMD_VERB_12BIT((verb), (payload))))
#endif

void vxbHdAudioShowAmp(NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose);
void vxbHdAudioShowNode (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose);
void vxbHdAudioShowNodes(NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose);
void vxbHdAudioShowConverter (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose);
void vxbHdAudioShowConfigDefaults (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose);
void vxbHdAudioShowConnections (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose);
void vxbHdAudioShowUnsolicited (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose);
void vxbHdAudioShowPinCtrl (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose);
void vxbHdAudioShowPinSense (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose);

static struct param_str_t
    {
    UINT32      cmd;
    const char *str;
    } param_str[] = {
    {HDA_PARAM_VENDOR_ID           , "HDA_PARAM_VENDOR_ID"          },
    {HDA_PARAM_REVISION_ID         , "HDA_PARAM_REVISION_ID"        },
    {HDA_PARAM_SUB_NODE_COUNT      , "HDA_PARAM_SUB_NODE_COUNT"     },
    {HDA_PARAM_FCT_GRP_TYPE        , "HDA_PARAM_FCT_GRP_TYPE"       },
    {HDA_PARAM_AUDIO_FCT_GRP_CAP   , "HDA_PARAM_AUDIO_FCT_GRP_CAP"  },
    {HDA_PARAM_AUDIO_WIDGET_CAP    , "HDA_PARAM_AUDIO_WIDGET_CAP"   },
    {HDA_PARAM_SUPP_PCM_SIZE_RATE  , "HDA_PARAM_SUPP_PCM_SIZE_RATE" },
    {HDA_PARAM_SUPP_STREAM_FORMATS , "HDA_PARAM_SUPP_STREAM_FORMATS"},
    {HDA_PARAM_PIN_CAP             , "HDA_PARAM_PIN_CAP"            },
    {HDA_PARAM_INPUT_AMP_CAP       , "HDA_PARAM_INPUT_AMP_CAP"      },
    {HDA_PARAM_OUTPUT_AMP_CAP      , "HDA_PARAM_OUTPUT_AMP_CAP"      },
    {HDA_PARAM_CONN_LIST_LENGTH    , "HDA_PARAM_CONN_LIST_LENGTH"   },
    {HDA_PARAM_SUPP_POWER_STATES   , "HDA_PARAM_SUPP_POWER_STATES"  },
    {HDA_PARAM_PROCESSING_CAP      , "HDA_PARAM_PROCESSING_CAP"     },
    {HDA_PARAM_GPIO_COUNT          , "HDA_PARAM_GPIO_COUNT"         },
    {HDA_PARAM_VOLUME_KNOB_CAP     , "HDA_PARAM_VOLUME_KNOB_CAP"    }
};

#define HDA_WIDGET_TYPE_AUDIO_OUTPUT            0
#define HDA_WIDGET_TYPE_AUDIO_INPUT             1
#define HDA_WIDGET_TYPE_AUDIO_MIXER             2
#define HDA_WIDGET_TYPE_AUDIO_SELECTOR          3
#define HDA_WIDGET_TYPE_AUDIO_PINCOMPLEX        4
#define HDA_WIDGET_TYPE_AUDIO_POWER             5
#define HDA_WIDGET_TYPE_AUDIO_VOLUMEKNOB        6
#define HDA_WIDGET_TYPE_AUDIO_BEEPGEN           7
#define HDA_WIDGET_TYPE_AUDIO_VENDORSPEC        15

#if 0
static struct widget_str_t
    {
    UINT8       type;
    const char  *str;
    } widget_str[] = {
    {HDA_WIDGET_TYPE_AUDIO_OUTPUT,  "HDA_WIDGET_TYPE_AUDIO_OUTPUT"},
    {HDA_WIDGET_TYPE_AUDIO_INPUT, "HDA_WIDGET_TYPE_AUDIO_INPUT"},
    {HDA_WIDGET_TYPE_AUDIO_MIXER, "HDA_WIDGET_TYPE_AUDIO_MIXER"},
    {HDA_WIDGET_TYPE_AUDIO_SELECTOR, "HDA_WIDGET_TYPE_AUDIO_SELECTOR"},
    {HDA_WIDGET_TYPE_AUDIO_PINCOMPLEX, "HDA_WIDGET_TYPE_AUDIO_PINCOMPLEX"},
    {HDA_WIDGET_TYPE_AUDIO_POWER, "HDA_WIDGET_TYPE_AUDIO_POWER"},
    {HDA_WIDGET_TYPE_AUDIO_VOLUMEKNOB, "HDA_WIDGET_TYPE_AUDIO_VOLUMEKNOB"},
    {HDA_WIDGET_TYPE_AUDIO_BEEPGEN, "HDA_WIDGET_TYPE_AUDIO_BEEPGEN"},
    {HDA_WIDGET_TYPE_AUDIO_VENDORSPEC, "HDA_WIDGET_TYPE_AUDIO_VENDORSPEC"}
};
#endif

static struct stream_format_t
    {
    UINT8       type;
    const char  *str;
    } stream_format_str[] = {
    {HDA_PARAM_SUPP_STREAM_FORMATS_PCM_MASK, "PCM"},
    {HDA_PARAM_SUPP_STREAM_FORMATS_FLOAT32_MASK, "FLOAT32"},
    {HDA_PARAM_SUPP_STREAM_FORMATS_AC3_MASK, "AC3"}
};

static struct size_rate_t
    {
    UINT32       type;
    const char  *str;
    } size_rate [] = {
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_32BIT_MASK,     "32bit"}, 
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_24BIT_MASK,    "24bit"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_20BIT_MASK,    "20bit"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_16BIT_MASK,    "16bit"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_8BIT_MASK,    "8bit"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_8KHZ_MASK,    "8KHz"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_11KHZ_MASK,    "11KHz"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_16KHZ_MASK,    "16KHz"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_22KHZ_MASK,    "22KHz"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_32KHZ_MASK,    "32KHz"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_44KHZ_MASK,    "44KHz"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_48KHZ_MASK,    "48KHz"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_88KHZ_MASK,    "88KHz"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_96KHZ_MASK,    "96KHz"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_176KHZ_MASK,    "176KHz"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_192KHZ_MASK,    "192KHz"},
    {HDA_PARAM_SUPP_PCM_SIZE_RATE_384KHZ_MASK,    "384KHz"}
};

static struct pcm_format_t
    {
    const char *str;
    } pcm_format [] = {
    {"PCM"},
    {"Non-PCM"}
};
#if 0
static struct pcm_base_rate_t
    {
    const char *str;
    } pcm_base_rate [] = {
    {"48 KHz"},
    {"44.1 Khz"}
};

static struct pcm_multi_rate_t
    {
    const char *str;
    } pcm_multi_rate [] = {
    {"x1"},
    {"x2"},
    {"x3"},
    {"x4"}
};

static struct pcm_div_rate_t
    {
    const char *str;
    } pcm_div_rate [] = {
    {"div 1"},
    {"div 2"},
    {"div 3"},
    {"div 4"},
    {"div 5"},
    {"div 6"},
    {"div 7"},
    {"div 8"},
};
#endif
static struct pcm_bits_t
    {
    const char *str;
    } pcm_bits [] = {
    {" 8 bits"},
    {"16 bits"},
    {"20 bits"},
    {"24 bits"},
    {"32 bits"},
};

static struct config_port_color_t
    {
    const char *str;
    } config_port_color [] = {
    {"Unknown"},
    {"Black"},
    {"Grey"},
    {"Blue"},
    {"Green"},
    {"Red"},
    {"Orange"},
    {"Yellow"},
    {"Purple"},
    {"Pink"},
    {"Reserved"},
    {"Reserved"},
    {"Reserved"},
    {"Reserved"},
    {"White"},
    {"Other"}
};

static struct connection_type_t
    {
    const char *str;
    } connection_type [] = {
    {"Unknown"},
    {"1/8\" stereo/mono"},
    {"1/4\" stereo/mono"},
    {"ATAPI internal"},
    {"RCA"},
    {"Optical"},
    {"Other Digital"},
    {"Other Analog"},
    {"Multichannel Analog (DIN)"},
    {"XLR/Professional"},
    {"RJ-11 (Modem)"},
    {"Combination"},
    {"Other"}
};

static struct default_device_t
    {
    const char *str;
    } default_device [] = {
    {"Line Out"},
    {"Speaker"},
    {"HP Out"},
    {"CD"},
    {"SPDIF Out"},
    {"Digital Other Out"},
    {"Modem Line Side"},
    {"Modem Handset Side"},
    {"Line In"},
    {"AUX"},
    {"Mic In"},
    {"Telephony"},
    {"SPDIF In"},
    {"Digital Other In"},
    {"Reserved"},
    {"Other"}
};

static struct location_t
    {
    const char *str;
    } location [] = {
    {"N/A"},
    {"Rear"},
    {"Front"},
    {"Left"},
    {"Right"},
    {"Top"},
    {"Bottom"},
    {"Special"},
    {"Special"},
    {"Special"},
    {"Reserved"},
    {"Reserved"},
    {"Reserved"},
    {"Reserved"},
    {"Reserved"},
    {"Reserved"},
};
    

struct param_val_t
    {
    UINT32         cmd;
    UINT32         val;
    };

typedef struct widget_param_t
    {
    int          cad;
    int          nid;
    struct param_val_t params[NELEMENTS(param_str)];
    } WIDGET_PARAM;

#if 0
static int widgetIndex (UINT32 type)
    {
    int count;
    for (count = 0; count < NELEMENTS(widget_str); count++)
        {
        if (type == widget_str[count].type)
            return count;
        }

    return -1;
    }
#endif

static int paramIndex(UINT32 param)
    {
    int count;
    for (count = 0; count < NELEMENTS(param_str); count++)
        {
        if (param == param_str[count].cmd)
            return count;
        }

    return -1;
    }


void vxbAudioShowWidgetGPIO (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose)
    {
    UINT32 verb, val;
    
    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_GET_GPI_DATA, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    printf ("\tGET_GPI_DATA: 0x%x\n", val);

    verb = vxbHdAudioFormVerb(cad, nid,HDA_CMD_VERB_GET_GPI_WAKE_ENABLE_MASK, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    printf ("\tGET_GPI_WAKE_ENABLE_MASK: 0x%x\n", val);

    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_GET_GPI_UNSOLICITED_ENABLE_MASK, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    printf ("\tGET_GPI_UNSOLICITED_ENABLE_MASK: 0x%x\n", val);

    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_GET_GPI_STICKY_MASK, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    printf ("\tGET_GPI_STICKY_MASK: 0x%x\n", val);

    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_GET_GPO_DATA, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    printf ("\tVERB_GET_GPO_DATA: 0x%x\n", val);

    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_GET_GPIO_DATA, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    printf ("\tGET_GPIO_DATA: 0x%x\n", val);
    
    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_GET_GPIO_DIRECTION, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    printf ("\tGET_GPIO_DIRECTION: 0x%x\n", val);

    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_GET_GPIO_WAKE_ENABLE_MASK, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    printf ("\tGET_GPIO_WAKE_ENABLE_MASK: 0x%x\n", val);

    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_GET_GPIO_UNSOLICITED_ENABLE_MASK, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    printf ("\tGET_GPIO_UNSOLICITED_ENABLE_MASK: 0x%x\n", val);

    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_GET_GPIO_STICKY_MASK, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    printf ("\tGET_GPIO_STICKY_MASK: 0x%x\n", val);
    }


void vxbAudioShowAFG (NEW_HDA_DRV_CTRL* pDrvCtrl, WIDGET_PARAM w, int verbose)
    {
    int i, features;
    printf("Audio Function Group cad[%d] nid[%d]\n", w.cad, w.nid);
    printf("\tStream Caps:\n\t");
    features = w.params[paramIndex(HDA_PARAM_SUPP_STREAM_FORMATS)].val;

    /* Stream Formats */
    for (i = 0; i < NELEMENTS(stream_format_str); i++)
        {
        if (features & stream_format_str[i].type)
            printf(" %s", stream_format_str[i].str);
        }

    /* Size and Rates */
    features = w.params[paramIndex(HDA_PARAM_SUPP_PCM_SIZE_RATE)].val;

    for (i = 0; i < NELEMENTS(size_rate); i++)
        {
        if (features & size_rate[i].type)
            printf(" %s", size_rate[i].str);
        }

    printf("\n");

    /* Amp Caps */
    features = w.params[paramIndex(HDA_PARAM_INPUT_AMP_CAP)].val;
    printf ("\tInput Amp Cap: 0x%x\n", features);
    
    features = w.params[paramIndex(HDA_PARAM_OUTPUT_AMP_CAP)].val;
    printf ("\tOutput Amp Cap: 0x%x\n", features);
    printf ("\t  [Mute]-[StepSize]-[NumSteps]-[Offset]: %d-%d-%d-%d\n", 
        (features&HDA_PARAM_OUTPUT_AMP_CAP_MUTE_CAP_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_MUTE_CAP_SHIFT,
        (features&HDA_PARAM_OUTPUT_AMP_CAP_STEPSIZE_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_STEPSIZE_SHIFT,
        (features&HDA_PARAM_OUTPUT_AMP_CAP_NUMSTEPS_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_NUMSTEPS_SHIFT,
        (features&HDA_PARAM_OUTPUT_AMP_CAP_OFFSET_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_OFFSET_SHIFT
        );

    /* GPIO */
    features = w.params[paramIndex(HDA_PARAM_GPIO_COUNT)].val;
    printf ("\tGPIO Count: 0x%x\n", features);

    vxbAudioShowWidgetGPIO (pDrvCtrl, w.cad, w.nid, verbose);
    }

void vxbAudioShowWidgetOutput (NEW_HDA_DRV_CTRL* pDrvCtrl, WIDGET_PARAM w, int verbose)
    {
    int i, features;
    UINT32 verb, val;

    printf("Output Converter Widget cad[%d] nid[%d]\n", w.cad, w.nid);
    printf("\tStream Caps:\n\t");
    features = w.params[paramIndex(HDA_PARAM_SUPP_STREAM_FORMATS)].val;

    /* Stream Formats */
    for (i = 0; i < NELEMENTS(stream_format_str); i++)
        {
        if (features & stream_format_str[i].type)
            printf(" %s", stream_format_str[i].str);
        }

    /* Size and Rates */
    features = w.params[paramIndex(HDA_PARAM_SUPP_PCM_SIZE_RATE)].val;

    for (i = 0; i < NELEMENTS(size_rate); i++)
        {
        if (features & size_rate[i].type)
            printf(" %s", size_rate[i].str);
        }

    printf("\n");

    /* Amp Caps */
    features = w.params[paramIndex(HDA_PARAM_OUTPUT_AMP_CAP)].val;
    printf ("\tOutput Amp Cap: 0x%x\n", features);
    printf ("\t  [Mute]-[StepSize]-[NumSteps]-[Offset]: %d-%d-%d-%d\n", 
        (features&HDA_PARAM_OUTPUT_AMP_CAP_MUTE_CAP_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_MUTE_CAP_SHIFT,
        (features&HDA_PARAM_OUTPUT_AMP_CAP_STEPSIZE_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_STEPSIZE_SHIFT,
        (features&HDA_PARAM_OUTPUT_AMP_CAP_NUMSTEPS_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_NUMSTEPS_SHIFT,
        (features&HDA_PARAM_OUTPUT_AMP_CAP_OFFSET_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_OFFSET_SHIFT
        );

    /* stream/channel */
    verb = vxbHdAudioFormVerb(w.cad, w.nid, HDA_CMD_VERB_GET_CONV_STREAM_CHAN, 0);
    val = vxbHdAudioCommand(pDrvCtrl, w.cad, verb);
    printf ("\t Stream and Channel: 0x%x\n", val);
    }

void vxbAudioShowWidgetInput (NEW_HDA_DRV_CTRL* pDrvCtrl, WIDGET_PARAM w, int verbose)
    {
    int i, features;
    UINT32 verb, val;

    printf("Input Converter Widget cad[%d] nid[%d]\n", w.cad, w.nid);
    printf("\tStream Caps:\n\t");

    /* Stream Formats */
    features = w.params[paramIndex(HDA_PARAM_SUPP_STREAM_FORMATS)].val;

    for (i = 0; i < NELEMENTS(stream_format_str); i++)
        {
        if (features & stream_format_str[i].type)
            printf(" %s", stream_format_str[i].str);
        }
    /* Size and Rates */
    features = w.params[paramIndex(HDA_PARAM_SUPP_PCM_SIZE_RATE)].val;

    for (i = 0; i < NELEMENTS(size_rate); i++)
        {
        if (features & size_rate[i].type)
            printf(" %s", size_rate[i].str);
        }

    printf("\n");

    /* Amp Caps */
    features = w.params[paramIndex(HDA_PARAM_INPUT_AMP_CAP)].val;
    printf ("\tInput Amp Cap: 0x%x\n", features);
    
    features = w.params[paramIndex(HDA_PARAM_OUTPUT_AMP_CAP)].val;
    printf ("\tOutput Amp Cap: 0x%x\n", features);
    printf ("\t  [Mute]-[StepSize]-[NumSteps]-[Offset]: %d-%d-%d-%d\n", 
        (features&HDA_PARAM_OUTPUT_AMP_CAP_MUTE_CAP_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_MUTE_CAP_SHIFT,
        (features&HDA_PARAM_OUTPUT_AMP_CAP_STEPSIZE_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_STEPSIZE_SHIFT,
        (features&HDA_PARAM_OUTPUT_AMP_CAP_NUMSTEPS_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_NUMSTEPS_SHIFT,
        (features&HDA_PARAM_OUTPUT_AMP_CAP_OFFSET_MASK) >> HDA_PARAM_OUTPUT_AMP_CAP_OFFSET_SHIFT
        );
   
    /* stream/channel */
    verb = vxbHdAudioFormVerb(w.cad, w.nid, HDA_CMD_VERB_GET_CONV_STREAM_CHAN, 0);
    val = vxbHdAudioCommand(pDrvCtrl, w.cad, verb);
    printf ("\t Stream and Channel: 0x%x\n", val);
    }

void vxbAudioShowWidgetMixer (NEW_HDA_DRV_CTRL* pDrvCtrl, WIDGET_PARAM w, int verbose)
    {
    int features;
    printf("Mixer Widget cad[%d] nid[%d]\n", w.cad, w.nid);

    /* Amp Caps */
    features = w.params[paramIndex(HDA_PARAM_INPUT_AMP_CAP)].val;
    printf ("\tInput Amp Cap: 0x%x\n", features);
    
    features = w.params[paramIndex(HDA_PARAM_OUTPUT_AMP_CAP)].val;
    printf ("\tOutput Amp Cap: 0x%x\n", features);
    }

void vxbAudioShowWidgetSelector (NEW_HDA_DRV_CTRL* pDrvCtrl, WIDGET_PARAM w, int verbose)
    {
    int features;
    printf("Selector Widget cad[%d] nid[%d]\n", w.cad, w.nid);

    /* Amp Caps */
    features = w.params[paramIndex(HDA_PARAM_INPUT_AMP_CAP)].val;
    printf ("\tInput Amp Cap: 0x%x\n", features);

    features = w.params[paramIndex(HDA_PARAM_OUTPUT_AMP_CAP)].val;
    printf ("\tOutput Amp Cap: 0x%x\n", features);
    }

void vxbAudioShowWidgetPincomplex (NEW_HDA_DRV_CTRL* pDrvCtrl, WIDGET_PARAM w, int verbose)
    {
    int features;
    printf("PinComplex Widget cad[%d] nid[%d]\n", w.cad, w.nid);

    /* Amp Caps */
    features = w.params[paramIndex(HDA_PARAM_INPUT_AMP_CAP)].val;
    printf ("\tInput Amp Cap: 0x%x\n", features);

    features = w.params[paramIndex(HDA_PARAM_OUTPUT_AMP_CAP)].val;
    printf ("\tOutput Amp Cap: 0x%x\n", features);

    }

void vxbHdAudioShowNode (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose)
    {
    UINT32 verb;
    UINT32 val;
    int count;
    WIDGET_PARAM w;

    w.cad = cad;
    w.nid = nid;
    
    for (count = 0; count < NELEMENTS(param_str); count++)
        {
        verb = vxbHdAudioFormVerb(cad, nid,
                                  HDA_CMD_VERB_GET_PARAMETER,
                                  param_str[count].cmd);
        
        w.params[count].cmd = param_str[count].cmd;
        
        val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
        w.params[count].val = val;

        if (verbose > 0x10)
            printf("cad[%d] nid[%d] [0x%8x]:%s\n", cad, nid, val, param_str[count].str);
        }

    printf ("\n");
    
    if ((w.params[paramIndex(HDA_PARAM_FCT_GRP_TYPE)].val & 1) &&
        (w.params[paramIndex(HDA_PARAM_SUB_NODE_COUNT)].val > 0))
        {
        vxbAudioShowAFG(pDrvCtrl, w, verbose);
        }

    if (w.params[paramIndex(HDA_PARAM_SUB_NODE_COUNT)].val == 0)
        {
        UINT8 type = 0xF & (w.params[paramIndex(HDA_PARAM_AUDIO_WIDGET_CAP)].val >> 20);

        printf ("\tAudio Widget Cap: 0x%x\n",
                w.params[paramIndex(HDA_PARAM_AUDIO_WIDGET_CAP)].val);

        switch (type)
            {
            case HDA_WIDGET_TYPE_AUDIO_OUTPUT:
                vxbHdAudioShowAmp(pDrvCtrl, cad, nid, verbose);
                vxbAudioShowWidgetOutput(pDrvCtrl, w, verbose);
                vxbHdAudioShowConverter (pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowAmp(pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowUnsolicited (pDrvCtrl, cad, nid, verbose);
                
                break;
            case HDA_WIDGET_TYPE_AUDIO_INPUT:
                vxbAudioShowWidgetInput(pDrvCtrl, w, verbose);
                vxbHdAudioShowConverter (pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowConnections (pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowAmp(pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowUnsolicited (pDrvCtrl, cad, nid, verbose);
                break;

            case HDA_WIDGET_TYPE_AUDIO_MIXER:
                vxbAudioShowWidgetMixer(pDrvCtrl, w, verbose);
                vxbHdAudioShowConnections (pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowAmp(pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowUnsolicited (pDrvCtrl, cad, nid, verbose);
                break;

            case HDA_WIDGET_TYPE_AUDIO_SELECTOR:
                vxbAudioShowWidgetSelector(pDrvCtrl, w, verbose);
                vxbHdAudioShowConnections (pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowAmp(pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowUnsolicited (pDrvCtrl, cad, nid, verbose);
                break;

            case HDA_WIDGET_TYPE_AUDIO_PINCOMPLEX:
                vxbAudioShowWidgetPincomplex(pDrvCtrl, w, verbose);
                vxbHdAudioShowPinCtrl(pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowPinSense(pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowConfigDefaults (pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowConnections (pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowAmp(pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowUnsolicited (pDrvCtrl, cad, nid, verbose);
                break;

            case HDA_WIDGET_TYPE_AUDIO_POWER:
                vxbHdAudioShowConnections (pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowUnsolicited (pDrvCtrl, cad, nid, verbose);
                break;

            case HDA_WIDGET_TYPE_AUDIO_VOLUMEKNOB:
                vxbHdAudioShowConnections (pDrvCtrl, cad, nid, verbose);
                vxbHdAudioShowUnsolicited (pDrvCtrl, cad, nid, verbose);
                break;
            case HDA_WIDGET_TYPE_AUDIO_BEEPGEN:
            case HDA_WIDGET_TYPE_AUDIO_VENDORSPEC:
            default:
                break;
            }
                
        }
    
    }

void vxbHdAudioShowNodes(NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose)
    {
    int subnode = 0;
    int size = 0;
    UINT32 verb;
    UINT32 val;

    vxbHdAudioShowNode(pDrvCtrl, cad, nid, verbose);

    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_PARAMETER,
                              HDA_PARAM_SUB_NODE_COUNT);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);

    subnode = val >> 16;
    size = val & 0xffff;

    for (;size > 0; size--)
        vxbHdAudioShowNodes (pDrvCtrl, cad, subnode++, verbose);
    
    }

UINT32 vxbHdAudioVerbNode (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, UINT32 cmd, UINT32 payload)
    {
    UINT32 verb;


    if (cmd & 0x700)
        {
        /* get/set 12 bit cmd 8 bit data */
        verb = HDA_CMD_12BIT((cad), (nid), cmd, payload);
        }
    else
        {
        /* get/set 4 bit cmd 12 bit data */
        verb = HDA_CMD_4BIT((cad), (nid), cmd, payload);
        }
    
    printf("cad[%d] nid[%d] [0x%8x]:%s\n", cad, nid, cmd, verb);

    return verb;
    }

void vxbHdAudioVerbNodes(NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, UINT32 cmd, UINT32 payload)
    {
    int subnode = 0;
    int size = 0;
    UINT32 val;
    UINT32 verb;
    
    vxbHdAudioVerbNode (pDrvCtrl, cad, nid, cmd, payload);
    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_GET_PARAMETER, HDA_PARAM_SUB_NODE_COUNT);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);

    subnode = val >> 16;
    size = val & 0xffff;

    for (;size > 0; size--)
        vxbHdAudioVerbNode (pDrvCtrl, cad, subnode++, cmd, payload);
    
    }


struct dma_pcb
    {
    UINT32 pos;
    UINT32 res;
    };
    
void vxbHdAudioDmaDebug (NEW_HDA_DRV_CTRL* pDrvCtrl)
    {
    int i = 0;
    int ss = pDrvCtrl->num_ss;
    struct dma_pcb *buf;

    buf = (struct dma_pcb*)pDrvCtrl->pos_dma.dma_paddr;
    
    for (i = 0; i < ss; i++)
        {
        printf ("dma_pos_buf[%d]: 0x%8.8x\n", i, buf[i]);
        }
    }

#define HDA_CMD_VERB_GET_CONV_STREAM_CHAN		0xf06
#define HDA_CMD_VERB_SET_CONV_STREAM_CHAN		0x706
#define HDA_CMD_VERB_GET_CONV_FMT			0xa
#define HDA_CMD_VERB_SET_CONV_FMT			0x2

void vxbHdAudioPrepConverter (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, UINT32 chan, UINT32 format, UINT32 count)
    {
    UINT32 verb;
#if 1
    UINT32 val;
    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_SET_CONV_FMT, format);
    vxbHdAudioCommand(pDrvCtrl, cad, verb);
#endif
    verb = vxbHdAudioFormVerb(cad, nid, HDA_CMD_VERB_SET_CONV_STREAM_CHAN, chan);
    vxbHdAudioCommand(pDrvCtrl, cad, verb);

    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_SET_CONV_CHAN_COUNT, count);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    }

void vxbHdAudioShowConverter (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose)
    {
    UINT32 verb;
    UINT32 val;
    UINT32 rate;
    UINT32 hrate, lrate;
    
    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_CONV_FMT, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);

    printf ("\t");
    rate = ((val >> 14) & 1) ? 44100 : 48000;
    rate = rate * (((val >> 11) & 7) + 1);
    rate = rate / (((val >> 8) & 7) + 1);

    hrate = rate / 1000;
    lrate = rate % 1000;
    
    printf ("%s %d.%d KHz %s %d channel(s)\n",
            pcm_format[(val >> 15) & 1],
            hrate, lrate,
            pcm_bits[(val >> 4) & 7],
            1 + (val & 0xf));

    if (verbose)
        printf("cad[%d] nid[%d]:format 0x%x\n", cad, nid, val);
    
    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_CONV_STREAM_CHAN, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);

    printf("\tstream %d channel %d\n", val >> 4, val & 0xf);

    if (verbose)
        printf("cad[%d] nid[%d]:channel 0x%x\n", cad, nid, val);

    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_CONV_CHAN_COUNT, 0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);

    printf("\tchannel count %d\n", val + 1);

    if (verbose)
        printf("cad[%d] nid[%d]:channel count %d\n", cad, nid, val);
    }

void vxbHdAudioShowConfigDefaults (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose)
    {
    UINT32 verb;
    UINT32 val;

    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_CONFIGURATION_DEFAULT,
                              0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);

    printf ("\t\t   port connectivity %d\n", (val >> 30) &    3);
    printf ("\t\t            location %d %s\n", (val >> 26) & 3, location[(val >> 24) & 0xf]);
    printf ("\t\t      default device %s\n", default_device[(val >> 20) & 0xf]);
    printf ("\t\t     connection type %s\n", connection_type[(val >> 16) & 0xf]);
    printf ("\t\t               color %s\n", config_port_color[(val >> 12) & 0xf]);
    printf ("\t\t                misc %d\n", (val >> 8)  & 0xf);
    printf ("\t\t default association %d\n", (val >> 4)  & 0xf);
    printf ("\t\t            sequence %d\n", (val & 0xf));
    
    if (verbose)
        printf("cad[%d] nid[%d]:config default 0x%x\n", cad, nid, val);
    }    

void vxbHdAudioShowPinCtrl (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose)
    {
    UINT32 verb;
    UINT32 val;

    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_PIN_WIDGET_CTRL,
                              0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);

    
    printf("\tPin Control 0x%x\n", cad, nid, val);
    
    if (verbose)
        printf("cad[%d] nid[%d]:Pin Control 0x%x\n", cad, nid, val);
    }    

void vxbHdAudioShowPinSense (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose)
    {
    UINT32 verb;
    UINT32 val;

    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_PIN_SENSE,
                              0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    if (verbose)
        printf("cad[%d] nid[%d]:Pin Sense 0x%x\n", cad, nid, val);
    }    

void vxbHdAudioSelectSource (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int index)
    {
    UINT32 verb;
    UINT32 val;

    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_SET_CONN_SELECT_CONTROL,
                              index);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    }

void vxbHdAudioShowSelector (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose)
    {
    UINT32 verb;
    UINT32 val;

    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_CONN_SELECT_CONTROL,
                              0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    if (verbose)
        printf("cad[%d] nid[%d]:conn select 0x%x\n", cad, nid, val);
    }

void vxbHdAudioShowUnsolicited (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose)
    {
    UINT32 verb;
    UINT32 val;

    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_UNSOLICITED_RESPONSE,
                              0);
    val = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    if (verbose)
        printf("cad[%d] nid[%d]:unsolicited 0x%x\n", cad, nid, val);
    }    

int vxbHdAudioGetConnectionListLength (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid)
    {
    UINT32 verb;
    UINT32 length;

    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_PARAMETER,
                              HDA_PARAM_CONN_LIST_LENGTH);
    length = vxbHdAudioCommand(pDrvCtrl, cad, verb);

    length &= HDA_PARAM_CONN_LIST_LENGTH_LIST_LENGTH_MASK;
    return length;
    }

int vxbHdAudioLongFormP (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid)
    {
    UINT32 verb;
    UINT32 length;
    int form = FALSE;
    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_PARAMETER,
                              HDA_PARAM_CONN_LIST_LENGTH);
    length = vxbHdAudioCommand(pDrvCtrl, cad, verb);

    if (length & HDA_PARAM_CONN_LIST_LENGTH_LONG_FORM_MASK)
        form = TRUE;

    return form;
    }

void vxbHdAudioShowConnections (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose)
    {
    UINT32 verb;
    UINT32 val, length;
    int i, form = FALSE;

    length = vxbHdAudioGetConnectionListLength (pDrvCtrl, cad, nid);
    form = vxbHdAudioLongFormP (pDrvCtrl, cad, nid);

    if (verbose)
        printf("cad[%d] nid[%d]:conn list length 0x%x\n", cad, nid,
               HDA_PARAM_CONN_LIST_LENGTH_LIST_LENGTH_MASK & length);

    if (form)
        for (i = 0; i < length; )
            {
            verb = vxbHdAudioFormVerb(cad, nid,
                                      HDA_CMD_VERB_GET_CONN_LIST_ENTRY, i);
            val = vxbHdAudioCommand(pDrvCtrl, cad, verb);

            printf("\tConnection List Entry %d %d\n", i, val & 0xffff);
            if (verbose)
                printf("cad[%d] nid[%d]:conn list entry %d 0x%x\n", cad, nid, i, val & 0xffff);
            if (++i >= length)
                break;
            
            val = val >> 16;

            printf("\tConnection List Entry %d %d\n", i, val & 0xffff);
            if (verbose)
                printf("cad[%d] nid[%d]:conn list entry %d 0x%x\n", cad, nid, i+1, val & 0xffff);
            if (++i >= length)
                break;
            }
    else
        for (i = 0; i < length; )
            {
            verb = vxbHdAudioFormVerb(cad, nid,
                                      HDA_CMD_VERB_GET_CONN_LIST_ENTRY, i);
            val = vxbHdAudioCommand(pDrvCtrl, cad, verb);

            printf("\tConnection List Entry %d %d\n", i, val & 0xff);
            if (verbose)
                printf("cad[%d] nid[%d]:conn list entry %d 0x%x\n", cad, nid, i, val & 0xff);
            if (++i >= length)
                break;
            
            val = val >> 8;

            printf("\tConnection List Entry %d %d\n", i, val & 0xff);
            if (verbose)
                printf("cad[%d] nid[%d]:conn list entry %d 0x%x\n", cad, nid, i+1, val & 0xff);
            if (++i >= length)
                break;
            
            val = val >> 8;

            printf("\tConnection List Entry %d %d\n", i, val & 0xff);
            if (verbose)
                printf("cad[%d] nid[%d]:conn list entry %d 0x%x\n", cad, nid, i+2, val & 0xff);
            if (++i >= length)
                break;
            
            val = val >> 8;

            printf("\tConnection List Entry %d %d\n", i, val & 0xff);
            if (verbose)
                printf("cad[%d] nid[%d]:conn list entry %d 0x%x\n", cad, nid, i+3, val & 0xff);
            if (++i >= length)
                break;
            
            }
    }

void vxbHdAudioSetGain (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, UINT16 arg)
    {
    UINT32 verb;
    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_SET_AMP_GAIN_MUTE,
                              arg);
    vxbHdAudioCommand(pDrvCtrl, cad, verb);
    }

#define HDA_CMD_SET_AMP_GAIN_MUTE_OUTPUT	0x8000
#define HDA_CMD_SET_AMP_GAIN_MUTE_INPUT		0x4000
#define HDA_CMD_SET_AMP_GAIN_MUTE_LEFT		0x2000
#define HDA_CMD_SET_AMP_GAIN_MUTE_RIGHT		0x1000
#define HDA_CMD_SET_AMP_GAIN_MUTE_INDEX_MASK	0x0f00
#define HDA_CMD_SET_AMP_GAIN_MUTE_INDEX_SHIFT	8
#define HDA_CMD_SET_AMP_GAIN_MUTE_MUTE		0x0080
#define HDA_CMD_SET_AMP_GAIN_MUTE_GAIN_MASK	0x0007
#define HDA_CMD_SET_AMP_GAIN_MUTE_GAIN_SHIFT	0

/* Parameter
 * left : 0 - left, 1 - right
 * step : step value of output amplifier capabilitier
 */
void vxbHdSetOutAmp(NEW_HDA_DRV_CTRL* pDrvCtrl, nid_t nid, nid_t left, UINT8 step)
    {
    UINT16 payload = 0;

    payload |= HDA_CMD_SET_AMP_GAIN_MUTE_OUTPUT; /* Set Output Amp */
    payload |= HDA_CMD_SET_AMP_GAIN_MUTE_LEFT >> (left&1);
    payload |= step&0x7f;

    vxbHdAudioSetGain (pDrvCtrl, 0, nid, payload);
    }

/*
 * set output amplifier for stereo
 */
void vxbHdSetOutAmpStereo(NEW_HDA_DRV_CTRL* pDrvCtrl, nid_t nid, UINT8 step)
    {
    vxbHdSetOutAmp(pDrvCtrl, nid, 0, step);
    vxbHdSetOutAmp(pDrvCtrl, nid, 1, step);
    }

void inputGainPowerUp (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int port, UINT16 level)
    {
    UINT16 gain;
    gain = HDA_CMD_SET_AMP_GAIN_MUTE_INPUT;
    gain |= HDA_CMD_SET_AMP_GAIN_MUTE_LEFT | HDA_CMD_SET_AMP_GAIN_MUTE_RIGHT;
    gain |= (port << HDA_CMD_SET_AMP_GAIN_MUTE_INDEX_SHIFT);
    gain |= level;
    
    vxbHdAudioSetGain (pDrvCtrl, cad, nid, gain);
    }

void outputGainPowerUp (NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int port, UINT16 level)
    {
    UINT16 gain;
    gain = HDA_CMD_SET_AMP_GAIN_MUTE_OUTPUT;
    gain |= HDA_CMD_SET_AMP_GAIN_MUTE_LEFT | HDA_CMD_SET_AMP_GAIN_MUTE_RIGHT;
    gain |= (port << HDA_CMD_SET_AMP_GAIN_MUTE_INDEX_SHIFT);
    gain |= level;
    
    vxbHdAudioSetGain (pDrvCtrl, cad, nid, gain);
    }

void vxbHdAudioShowAmp(NEW_HDA_DRV_CTRL* pDrvCtrl, cad_t cad, nid_t nid, int verbose)
    {
    int inputs, i;
    UINT8 lgain, rgain;
    UINT16 payload;
    UINT32 verb;
    
    inputs = vxbHdAudioGetConnectionListLength (pDrvCtrl, cad, nid);

    printf ("  amp gain values left:right\n");
    for (i = 0; i < inputs; i++)
        {
        payload = i | HDA_CMD_GET_AMP_GAIN_MUTE_INPUT | HDA_CMD_GET_AMP_GAIN_MUTE_LEFT;
        verb = vxbHdAudioFormVerb(cad, nid,
                                  HDA_CMD_VERB_GET_AMP_GAIN_MUTE,
                                  payload);
        lgain = vxbHdAudioCommand(pDrvCtrl, cad, verb);
        
        payload = i | HDA_CMD_GET_AMP_GAIN_MUTE_INPUT | HDA_CMD_GET_AMP_GAIN_MUTE_RIGHT;
        verb = vxbHdAudioFormVerb(cad, nid,
                                  HDA_CMD_VERB_GET_AMP_GAIN_MUTE,
                                  payload);
        rgain = vxbHdAudioCommand(pDrvCtrl, cad, verb);
        
        
        printf ("widget input %d amp:   0x%x : 0x%x\n", i, lgain, rgain);
        }

    payload = HDA_CMD_GET_AMP_GAIN_MUTE_OUTPUT | HDA_CMD_GET_AMP_GAIN_MUTE_LEFT;
    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_AMP_GAIN_MUTE,
                              payload);
    lgain = vxbHdAudioCommand(pDrvCtrl, cad, verb);

    payload = HDA_CMD_GET_AMP_GAIN_MUTE_OUTPUT | HDA_CMD_GET_AMP_GAIN_MUTE_RIGHT;
    verb = vxbHdAudioFormVerb(cad, nid,
                              HDA_CMD_VERB_GET_AMP_GAIN_MUTE,
                              payload);
    rgain = vxbHdAudioCommand(pDrvCtrl, cad, verb);
    
    
    printf ("widget output  amp:   0x%x : 0x%x\n", lgain, rgain);

    }


int vxbHdVerb (nid_t nid, UINT32 verb, UINT16 arg)
    {
    verb = vxbHdAudioFormVerb(0, nid,
                              verb,
                              arg);
    return(vxbHdAudioCommand(global_controller, 0, verb));
    }

