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

#include "audio/ossAudio.h"
#include "audio/vxbHdAudio.h"
#include "audio/vxbHdWidget.h"

#include <ffsLib.h>


const char *HDA_COLORS[16] = {"Unknown", "Black", "Grey", "Blue", "Green", "Red",
    "Orange", "Yellow", "Purple", "Pink", "Res.A", "Res.B", "Res.C", "Res.D",
    "White", "Other"};

const char *HDA_DEVS[16] = {"Line-out", "Speaker", "Headphones", "CD",
    "SPDIF-out", "Digital-out", "Modem-line", "Modem-handset", "Line-in",
    "AUX", "Mic", "Telephony", "SPDIF-in", "Digital-in", "Res.E", "Other"};

const char *HDA_CONNS[4] = {"Jack", "None", "Fixed", "Both"};

const char *HDA_CONNECTORS[16] = {
    "Unknown", "1/8", "1/4", "ATAPI", "RCA", "Optical", "Digital", "Analog",
    "DIN", "XLR", "RJ-11", "Combo", "0xc", "0xd", "0xe", "Other" };

const char *HDA_LOCS[64] = {
    "0x00", "Rear", "Front", "Left", "Right", "Top", "Bottom", "Rear-panel",
	"Drive-bay", "0x09", "0x0a", "0x0b", "0x0c", "0x0d", "0x0e", "0x0f",
    "Internal", "0x11", "0x12", "0x13", "0x14", "0x15", "0x16", "Riser",
	"0x18", "Onboard", "0x1a", "0x1b", "0x1c", "0x1d", "0x1e", "0x1f",
    "External", "Ext-Rear", "Ext-Front", "Ext-Left", "Ext-Right", "Ext-Top", "Ext-Bottom", "0x07",
	"0x28", "0x29", "0x2a", "0x2b", "0x2c", "0x2d", "0x2e", "0x2f",
    "Other", "0x31", "0x32", "0x33", "0x34", "0x35", "Other-Bott", "Lid-In",
	"Lid-Out", "0x39", "0x3a", "0x3b", "0x3c", "0x3d", "0x3e", "0x3f" };

const char *HDA_GPIO_ACTIONS[8] = {
    "keep", "set", "clear", "disable", "input", "0x05", "0x06", "0x07"};

const char *HDA_HDMI_CODING_TYPES[18] = {
    "undefined", "LPCM", "AC-3", "MPEG1", "MP3", "MPEG2", "AAC-LC", "DTS",
    "ATRAC", "DSD", "E-AC-3", "DTS-HD", "MLP", "DST", "WMAPro", "HE-AAC",
    "HE-AACv2", "MPEG-Surround"
};


const static char *ossnames[] = SOUND_DEVICE_NAMES;


LOCAL char * hdaa_audio_ctl_ossmixer_mask2allname(UINT32 mask, char *buf, size_t len)
{
	int i, first = 1;

	bzero(buf, len);
	for (i = 0; i < SOUND_MIXER_NRDEVICES; i++) {
		if (mask & (1 << i)) {
			if (first == 0)
				strlcat(buf, ", ", len);
			strlcat(buf, ossnames[i], len);
			first = 0;
		}
	}
	return (buf);
}


LOCAL void hdaa_dump_ctls(PCM_DEVINFO *pdevinfo, const char *banner, UINT32 flag)
    {
    HDCODEC_ID codec = pdevinfo->codec;
	AUDIO_CTL *ctl;
	char buf[64];
	int i, j, printed;

	if (flag == 0) {
		flag = ~(SOUND_MASK_VOLUME | SOUND_MASK_PCM |
		    SOUND_MASK_CD | SOUND_MASK_LINE | SOUND_MASK_RECLEV |
		    SOUND_MASK_MIC | SOUND_MASK_SPEAKER | SOUND_MASK_IGAIN |
		    SOUND_MASK_OGAIN | SOUND_MASK_IMIX | SOUND_MASK_MONITOR);
	}

	for (j = 0; j < SOUND_MIXER_NRDEVICES; j++) {
		if ((flag & (1 << j)) == 0)
			continue;
		i = 0;
		printed = 0;
		while ((ctl = audio_ctl_each(codec, &i)) != NULL) {
			if (ctl->enable == 0 ||
			    ctl->widget->enable == 0)
				continue;
			if (!((pdevinfo->playas >= 0 &&
			    ctl->widget->bindas == pdevinfo->playas) ||
			    (pdevinfo->recas >= 0 &&
			    ctl->widget->bindas == pdevinfo->recas) ||
			    (ctl->widget->bindas == -2 && pdevinfo->index == 0)))
				continue;
			if ((ctl->ossmask & (1 << j)) == 0)
				continue;

			if (printed == 0) {
				HDA_DBG(HDA_DBG_INFO, "\n");
				if (banner != NULL) {
					HDA_DBG(HDA_DBG_INFO, "%s", banner);
				} else {
					HDA_DBG(HDA_DBG_INFO, "Unknown Ctl");
				}
				HDA_DBG(HDA_DBG_INFO, " (OSS: %s)",
				    hdaa_audio_ctl_ossmixer_mask2allname(1 << j,
				    buf, sizeof(buf)));
				if (pdevinfo->ossmask & (1 << j)) {
					HDA_DBG(HDA_DBG_INFO, ": %+d/%+ddB\n",
					    pdevinfo->minamp[j] / 4,
					    pdevinfo->maxamp[j] / 4);
				} else
					HDA_DBG(HDA_DBG_INFO, "\n");
				HDA_DBG(HDA_DBG_INFO, "   |\n");
				printed = 1;
			}
			HDA_DBG(HDA_DBG_INFO, "   +- ctl %2d (nid %3d %s", i,
				ctl->widget->nid,
				(ctl->ndir == HDAA_CTL_IN)?"in ":"out");
			if (ctl->ndir == HDAA_CTL_IN && ctl->ndir == ctl->dir)
                {
				HDA_DBG(HDA_DBG_INFO, " %2d): ", ctl->index);
                }
			else
                {
				HDA_DBG(HDA_DBG_INFO, "):    ");
                }
			if (ctl->step > 0) {
				HDA_DBG(HDA_DBG_INFO, "%+d/%+ddB (%d steps)%s\n",
				    MINQDB(ctl) / 4,
				    MAXQDB(ctl) / 4,
				    ctl->step + 1,
				    ctl->mute?" + mute":"");
			} else
                {
				HDA_DBG(HDA_DBG_INFO, "%s\n", ctl->mute?"mute":"");
                }
		}
	}
}

LOCAL void hdaa_dump_audio_formats(VXB_DEVICE_ID dev, UINT32 fcap, UINT32 pcmcap)
{
	UINT32 cap;

	cap = fcap;
	if (cap != 0) {
		HDA_DBG(HDA_DBG_INFO, "     Stream cap: 0x%08x\n", cap);
		HDA_DBG(HDA_DBG_INFO, "                ");
		if (HDA_PARAM_SUPP_STREAM_FORMATS_AC3(cap))
			HDA_DBG(HDA_DBG_INFO, " AC3");
		if (HDA_PARAM_SUPP_STREAM_FORMATS_FLOAT32(cap))
			HDA_DBG(HDA_DBG_INFO, " FLOAT32");
		if (HDA_PARAM_SUPP_STREAM_FORMATS_PCM(cap))
			HDA_DBG(HDA_DBG_INFO, " PCM");
		HDA_DBG(HDA_DBG_INFO, "\n");
	}
	cap = pcmcap;
	if (cap != 0) {
		HDA_DBG(HDA_DBG_INFO, "        PCM cap: 0x%08x\n", cap);
		HDA_DBG(HDA_DBG_INFO, "                ");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_8BIT(cap))
			HDA_DBG(HDA_DBG_INFO, " 8");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_16BIT(cap))
			HDA_DBG(HDA_DBG_INFO, " 16");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_20BIT(cap))
			HDA_DBG(HDA_DBG_INFO, " 20");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_24BIT(cap))
			HDA_DBG(HDA_DBG_INFO, " 24");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_32BIT(cap))
			HDA_DBG(HDA_DBG_INFO, " 32");
		HDA_DBG(HDA_DBG_INFO, " bits,");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_8KHZ(cap))
			HDA_DBG(HDA_DBG_INFO, " 8");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_11KHZ(cap))
			HDA_DBG(HDA_DBG_INFO, " 11");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_16KHZ(cap))
			HDA_DBG(HDA_DBG_INFO, " 16");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_22KHZ(cap))
			HDA_DBG(HDA_DBG_INFO, " 22");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_32KHZ(cap))
			HDA_DBG(HDA_DBG_INFO, " 32");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_44KHZ(cap))
			HDA_DBG(HDA_DBG_INFO, " 44");
		HDA_DBG(HDA_DBG_INFO, " 48");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_88KHZ(cap))
			HDA_DBG(HDA_DBG_INFO, " 88");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_96KHZ(cap))
			HDA_DBG(HDA_DBG_INFO, " 96");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_176KHZ(cap))
			HDA_DBG(HDA_DBG_INFO, " 176");
		if (HDA_PARAM_SUPP_PCM_SIZE_RATE_192KHZ(cap))
			HDA_DBG(HDA_DBG_INFO, " 192");
		HDA_DBG(HDA_DBG_INFO, " KHz\n");
	}
}

LOCAL void hdaa_dump_pin(WIDGET *w)
{
	UINT32 pincap;

	pincap = w->wclass.pin.cap;

	HDA_DBG(HDA_DBG_INFO, "        Pin cap: 0x%08x\n", pincap);
	HDA_DBG(HDA_DBG_INFO, "                ");
	if (HDA_PARAM_PIN_CAP_IMP_SENSE_CAP(pincap))
		HDA_DBG(HDA_DBG_INFO, " ISC");
	if (HDA_PARAM_PIN_CAP_TRIGGER_REQD(pincap))
		HDA_DBG(HDA_DBG_INFO, " TRQD");
	if (HDA_PARAM_PIN_CAP_PRESENCE_DETECT_CAP(pincap))
		HDA_DBG(HDA_DBG_INFO, " PDC");
	if (HDA_PARAM_PIN_CAP_HEADPHONE_CAP(pincap))
		HDA_DBG(HDA_DBG_INFO, " HP");
	if (HDA_PARAM_PIN_CAP_OUTPUT_CAP(pincap))
		HDA_DBG(HDA_DBG_INFO, " OUT");
	if (HDA_PARAM_PIN_CAP_INPUT_CAP(pincap))
		HDA_DBG(HDA_DBG_INFO, " IN");
	if (HDA_PARAM_PIN_CAP_BALANCED_IO_PINS(pincap))
		HDA_DBG(HDA_DBG_INFO, " BAL");
	if (HDA_PARAM_PIN_CAP_HDMI(pincap))
		HDA_DBG(HDA_DBG_INFO, " HDMI");
	if (HDA_PARAM_PIN_CAP_VREF_CTRL(pincap)) {
		HDA_DBG(HDA_DBG_INFO, " VREF[");
		if (HDA_PARAM_PIN_CAP_VREF_CTRL_50(pincap))
			HDA_DBG(HDA_DBG_INFO, " 50");
		if (HDA_PARAM_PIN_CAP_VREF_CTRL_80(pincap))
			HDA_DBG(HDA_DBG_INFO, " 80");
		if (HDA_PARAM_PIN_CAP_VREF_CTRL_100(pincap))
			HDA_DBG(HDA_DBG_INFO, " 100");
		if (HDA_PARAM_PIN_CAP_VREF_CTRL_GROUND(pincap))
			HDA_DBG(HDA_DBG_INFO, " GROUND");
		if (HDA_PARAM_PIN_CAP_VREF_CTRL_HIZ(pincap))
			HDA_DBG(HDA_DBG_INFO, " HIZ");
		HDA_DBG(HDA_DBG_INFO, " ]");
	}
	if (HDA_PARAM_PIN_CAP_EAPD_CAP(pincap))
		HDA_DBG(HDA_DBG_INFO, " EAPD");
	if (HDA_PARAM_PIN_CAP_DP(pincap))
		HDA_DBG(HDA_DBG_INFO, " DP");
	if (HDA_PARAM_PIN_CAP_HBR(pincap))
		HDA_DBG(HDA_DBG_INFO, " HBR");
	HDA_DBG(HDA_DBG_INFO, "\n");
	HDA_DBG(HDA_DBG_INFO, "     Pin config: 0x%08x\n",
	    w->wclass.pin.config);
	HDA_DBG(HDA_DBG_INFO, "    Pin control: 0x%08x", w->wclass.pin.ctrl);
	if (w->wclass.pin.ctrl & HDA_CMD_SET_PIN_WIDGET_CTRL_HPHN_ENABLE)
		HDA_DBG(HDA_DBG_INFO, " HP");
	if (w->wclass.pin.ctrl & HDA_CMD_SET_PIN_WIDGET_CTRL_IN_ENABLE)
		HDA_DBG(HDA_DBG_INFO, " IN");
	if (w->wclass.pin.ctrl & HDA_CMD_SET_PIN_WIDGET_CTRL_OUT_ENABLE)
		HDA_DBG(HDA_DBG_INFO, " OUT");
	if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(w->param.widget_cap)) {
		if ((w->wclass.pin.ctrl &
		    HDA_CMD_SET_PIN_WIDGET_CTRL_VREF_ENABLE_MASK) == 0x03)
            {
			HDA_DBG(HDA_DBG_INFO, " HBR");
            }
		else if ((w->wclass.pin.ctrl &
		    HDA_CMD_SET_PIN_WIDGET_CTRL_VREF_ENABLE_MASK) != 0)
            {
			HDA_DBG(HDA_DBG_INFO, " EPTs");
            }
	} else {
		if ((w->wclass.pin.ctrl &
		    HDA_CMD_SET_PIN_WIDGET_CTRL_VREF_ENABLE_MASK) != 0)
            {
			HDA_DBG(HDA_DBG_INFO, " VREFs");
            }
	}
	HDA_DBG(HDA_DBG_INFO, "\n");
}

LOCAL void hdaa_dump_pin_config(WIDGET *w, UINT32 conf)
{

        HDA_DBG(HDA_DBG_INFO, "%2d %08x %-2d %-2d "
	    "%-13s %-5s %-7s %-10s %-7s %d%s\n",
	    w->nid, conf,
	    HDA_CONFIG_DEFAULTCONF_ASSOCIATION(conf),
	    HDA_CONFIG_DEFAULTCONF_SEQUENCE(conf),
	    HDA_DEVS[HDA_CONFIG_DEFAULTCONF_DEVICE(conf)],
	    HDA_CONNS[HDA_CONFIG_DEFAULTCONF_CONNECTIVITY(conf)],
	    HDA_CONNECTORS[HDA_CONFIG_DEFAULTCONF_CONNECTION_TYPE(conf)],
	    HDA_LOCS[HDA_CONFIG_DEFAULTCONF_LOCATION(conf)],
	    HDA_COLORS[HDA_CONFIG_DEFAULTCONF_COLOR(conf)],
	    HDA_CONFIG_DEFAULTCONF_MISC(conf),
	    (w->enable == 0)?" DISA":"");
}

LOCAL void hdaa_dump_pin_configs (HDCODEC* codec)
{
	WIDGET *w;
	int i;

	HDA_DBG(HDA_DBG_INFO, "nid   0x    as seq "
	    "device       conn  jack    loc        color   misc\n");
	for (i = codec->startnode; i < codec->endnode; i++) {
		w = widget_get(codec, i);
		if (w == NULL)
			continue;
		if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
			continue;
		hdaa_dump_pin_config(w, w->wclass.pin.config);
	}
}

LOCAL void hdaa_dump_amp (VXB_DEVICE_ID dev, UINT32 cap, char *banner)
{
	HDA_DBG(HDA_DBG_INFO, "     %s amp: 0x%08x\n", banner, cap);
	HDA_DBG(HDA_DBG_INFO, "                 "
	    "mute=%d step=%d size=%d offset=%d\n",
	    HDA_PARAM_OUTPUT_AMP_CAP_MUTE_CAP(cap),
	    HDA_PARAM_OUTPUT_AMP_CAP_NUMSTEPS(cap),
	    HDA_PARAM_OUTPUT_AMP_CAP_STEPSIZE(cap),
	    HDA_PARAM_OUTPUT_AMP_CAP_OFFSET(cap));
}

LOCAL void hdaa_dump_nodes (HDCODEC* codec)
{
	WIDGET *w, *cw;
	char buf[64];
	int i, j;

	HDA_DBG(HDA_DBG_INFO, "\n");
	HDA_DBG(HDA_DBG_INFO, "Default Parameter\n");
	HDA_DBG(HDA_DBG_INFO, "-----------------\n");
	hdaa_dump_audio_formats(codec->pDev,
                            codec->supp_stream_formats,
                            codec->supp_pcm_size_rate);
	HDA_DBG(HDA_DBG_INFO, "         IN amp: 0x%08x\n",
	    codec->inamp_cap);
	HDA_DBG(HDA_DBG_INFO, "        OUT amp: 0x%08x\n",
	    codec->outamp_cap);
	for (i = codec->startnode; i < codec->endnode; i++) {
		w = widget_get(codec, i);
		if (w == NULL) {
			HDA_DBG(HDA_DBG_INFO, "Ghost widget nid=%d\n", i);
			continue;
		}
		HDA_DBG(HDA_DBG_INFO, "\n");
		HDA_DBG(HDA_DBG_INFO, "            nid: %d%s\n", w->nid,
		    (w->enable == 0) ? " [DISABLED]" : "");
/*		HDA_DBG(HDA_DBG_INFO, "           Name: %s\n", w->name); */
		HDA_DBG(HDA_DBG_INFO, "     Widget cap: 0x%08x\n",
		    w->param.widget_cap);
		if (w->param.widget_cap & 0x0ee1) {
			HDA_DBG(HDA_DBG_INFO, "                ");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_LR_SWAP(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " LRSWAP");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_POWER_CTRL(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " PWR");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " DIGITAL");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_UNSOL_CAP(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " UNSOL");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_PROC_WIDGET(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " PROC");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_STRIPE(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " STRIPE(x%d)",
				1 << (ffsLsb(w->wclass.conv.stripecap) - 1));
			j = HDA_PARAM_AUDIO_WIDGET_CAP_CC(w->param.widget_cap);
			if (j == 1)
                {
			    HDA_DBG(HDA_DBG_INFO, " STEREO");
                }
			else if (j > 1)
                {
			    HDA_DBG(HDA_DBG_INFO, " %dCH", j + 1);
                }
			HDA_DBG(HDA_DBG_INFO, "\n");
		}
		if (w->bindas != -1) {
			HDA_DBG(HDA_DBG_INFO, "    Association: %d (0x%08x)\n",
			    w->bindas, w->bindseqmask);
		}
if(w->nid==10)
printf("nid(10): ossmask= %d, ossdev= %d\n", w->ossmask, w->ossdev);
		if (w->ossmask != 0 || w->ossdev >= 0) {
			HDA_DBG(HDA_DBG_INFO, "            OSS: %s",
			    hdaa_audio_ctl_ossmixer_mask2allname(w->ossmask, buf, sizeof(buf)));
			if (w->ossdev >= 0)
			    HDA_DBG(HDA_DBG_INFO, " (%s)", ossnames[w->ossdev]);
			HDA_DBG(HDA_DBG_INFO, "\n");
		}
		if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_OUTPUT ||
		    w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT) {
            hdaa_dump_audio_formats(codec->pDev,
			    w->param.supp_stream_formats,
			    w->param.supp_pcm_size_rate);
		} else if (w->type ==
		    HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX || w->waspin)
			hdaa_dump_pin(w);
		if (w->param.eapdbtl != HDA_INVALID)
			HDA_DBG(HDA_DBG_INFO, "           EAPD: 0x%08x\n",
			    w->param.eapdbtl);
		if (HDA_PARAM_AUDIO_WIDGET_CAP_OUT_AMP(w->param.widget_cap) &&
		    w->param.outamp_cap != 0)
			hdaa_dump_amp(codec->pDev, w->param.outamp_cap, "Output");
		if (HDA_PARAM_AUDIO_WIDGET_CAP_IN_AMP(w->param.widget_cap) &&
		    w->param.inamp_cap != 0)
			hdaa_dump_amp(codec->pDev, w->param.inamp_cap, " Input");
		if (w->nconns > 0) {
			HDA_DBG(HDA_DBG_INFO, "    connections: %d\n", w->nconns);
			HDA_DBG(HDA_DBG_INFO, "          |\n");
		}
		for (j = 0; j < w->nconns; j++) {
			cw = widget_get(codec, w->conns[j]);
			HDA_DBG(HDA_DBG_INFO, "          + %s<- nid=%d [%s]",
                            (w->connsenable[j] == 0)?"[DISABLED] ":"",
                          w->conns[j], (cw == NULL) ? "GHOST!" : "name_here"); /* cw->name); */
			if (cw == NULL)
                {
				HDA_DBG(HDA_DBG_INFO, " [UNKNOWN]");
                }
			else if (cw->enable == 0)
                {
				HDA_DBG(HDA_DBG_INFO, " [DISABLED]");
                }
			if (w->nconns > 1 && w->selconn == j && w->type !=
			    HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER)
				HDA_DBG(HDA_DBG_INFO, " (selected)");
			HDA_DBG(HDA_DBG_INFO, "\n");
		}
	}

}

LOCAL void hdaa_dump_dst_nid(PCM_DEVINFO *pdevinfo, nid_t nid, int depth)
{
    HDCODEC_ID codec = pdevinfo->codec;
	WIDGET *w, *cw;
	char buf[64];
	int i, printed = 0;

	if (depth > HDA_PARSE_MAXDEPTH)
		return;

	w = widget_get(codec, nid);
	if (w == NULL || w->enable == 0)
		return;

	if (depth == 0)
        {
		HDA_DBG(HDA_DBG_INFO, "%*s", 4, "");
        }
	else
        {
		HDA_DBG(HDA_DBG_INFO, "%*s  + <- ", 4 + (depth - 1) * 7, "");
        }
/*	HDA_DBG(HDA_DBG_INFO, "nid=%d [%s]", w->nid, w->name); */

	if (depth > 0) {
		if (w->ossmask == 0) {
			HDA_DBG(HDA_DBG_INFO, "\n");
			return;
		}
		HDA_DBG(HDA_DBG_INFO, " [src: %s]", 
		    hdaa_audio_ctl_ossmixer_mask2allname(
			w->ossmask, buf, sizeof(buf)));
		if (w->ossdev >= 0) {
			HDA_DBG(HDA_DBG_INFO, "\n");
			return;
		}
	}
	HDA_DBG(HDA_DBG_INFO, "\n");

	for (i = 0; i < w->nconns; i++) {
		if (w->connsenable[i] == 0)
			continue;
		cw = widget_get(codec, w->conns[i]);
		if (cw == NULL || cw->enable == 0 || cw->bindas == -1)
			continue;
		if (printed == 0) {
			HDA_DBG(HDA_DBG_INFO, "%*s  |\n", 4 + (depth) * 7, "");
			printed = 1;
		}
		hdaa_dump_dst_nid(pdevinfo, w->conns[i], depth + 1);
	}

}

LOCAL void hdaa_dump_dac(PCM_DEVINFO *pdevinfo)
    {
	ASSOC *as;
	WIDGET *w;
	int i, printed = 0;
    HDCODEC_ID codec = pdevinfo->codec;

	if (pdevinfo->playas < 0)
		return;

	as = &codec->assoc_table[pdevinfo->playas];
	for (i = 0; i < 16; i++) {
		if (as->pins[i] <= 0)
			continue;
		w = widget_get(codec, as->pins[i]);
		if (w == NULL || w->enable == 0)
			continue;
		if (printed == 0) {
			printed = 1;
			HDA_DBG(HDA_DBG_INFO, "\n");
			HDA_DBG(HDA_DBG_INFO, "Playback:\n");
		}
		HDA_DBG(HDA_DBG_INFO, "\n");
		hdaa_dump_dst_nid(pdevinfo, as->pins[i], 0);
	}
}

LOCAL void hdaa_dump_adc(PCM_DEVINFO *pdevinfo)
    {
	WIDGET *w;
	int i;
	int printed = 0;
    HDCODEC_ID codec = pdevinfo->codec;

	if (pdevinfo->recas < 0)
		return;

	for (i = codec->startnode; i < codec->endnode; i++) {
		w = widget_get(codec, i);
		if (w == NULL || w->enable == 0)
			continue;
		if (w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT)
			continue;
		if (w->bindas != pdevinfo->recas)
			continue;
		if (printed == 0) {
			printed = 1;
			HDA_DBG(HDA_DBG_INFO, "\n");
			HDA_DBG(HDA_DBG_INFO, "Record:\n");
		}
		HDA_DBG(HDA_DBG_INFO, "\n");
		hdaa_dump_dst_nid(pdevinfo, i, 0);
	}
}

LOCAL void hdaa_dump_mix(PCM_DEVINFO *pdevinfo)
    {
	WIDGET *w;
	int i;
	int printed = 0;
    HDCODEC_ID codec = pdevinfo->codec;

	for (i = codec->startnode; i < codec->endnode; i++) {
		w = widget_get(codec, i);
		if (w == NULL || w->enable == 0)
			continue;
		if (w->ossdev != SOUND_MIXER_IMIX)
			continue;
		if (w->bindas != pdevinfo->recas)
			continue;
		if (printed == 0) {
			printed = 1;
			HDA_DBG(HDA_DBG_INFO, "\n");
			HDA_DBG(HDA_DBG_INFO, "Input Mix:\n");
		}
		HDA_DBG(HDA_DBG_INFO, "\n");
		hdaa_dump_dst_nid(pdevinfo, i, 0);
	}
}

LOCAL void hdaa_dump_pcmchannels(PCM_DEVINFO *pdevinfo)
    {
	nid_t *nids;
	int chid, i;
    HDCODEC_ID codec = pdevinfo->codec;

	if (pdevinfo->playas >= 0) {
		HDA_DBG(HDA_DBG_INFO, "\n");
		HDA_DBG(HDA_DBG_INFO, "Playback:\n");
		HDA_DBG(HDA_DBG_INFO, "\n");
		chid = codec->assoc_table[pdevinfo->playas].chans[0];
		hdaa_dump_audio_formats(pdevinfo->pDev,
		    codec->hdaa_chan_table[chid].supp_stream_formats,
		    codec->hdaa_chan_table[chid].supp_pcm_size_rate);
		for (i = 0; i < codec->assoc_table[pdevinfo->playas].num_chans; i++) {
			chid = codec->assoc_table[pdevinfo->playas].chans[i];
			HDA_DBG(HDA_DBG_INFO, "            DAC:");
			for (nids = codec->hdaa_chan_table[chid].io; *nids != -1; nids++)
				HDA_DBG(HDA_DBG_INFO, " %d", *nids);
			HDA_DBG(HDA_DBG_INFO, "\n");
		}
	}
	if (pdevinfo->recas >= 0) {
		HDA_DBG(HDA_DBG_INFO, "\n");
		HDA_DBG(HDA_DBG_INFO, "Record:\n");
		HDA_DBG(HDA_DBG_INFO, "\n");
		chid = codec->assoc_table[pdevinfo->recas].chans[0];
		hdaa_dump_audio_formats(pdevinfo->pDev,
		    codec->hdaa_chan_table[chid].supp_stream_formats,
		    codec->hdaa_chan_table[chid].supp_pcm_size_rate);
		for (i = 0; i < codec->assoc_table[pdevinfo->recas].num_chans; i++) {
			chid = codec->assoc_table[pdevinfo->recas].chans[i];
			HDA_DBG(HDA_DBG_INFO, "            ADC:");
			for (nids = codec->hdaa_chan_table[chid].io; *nids != -1; nids++)
				HDA_DBG(HDA_DBG_INFO, " %d", *nids);
			HDA_DBG(HDA_DBG_INFO, "\n");
		}
	}
}

LOCAL void dumpDetails (PCM_DEVINFO * pdevinfo)
    {
    HDA_DBG(HDA_DBG_INFO, "+--------------------------------------+\n");
    HDA_DBG(HDA_DBG_INFO, "| DUMPING PCM Playback/Record Channels |\n");
    HDA_DBG(HDA_DBG_INFO, "+--------------------------------------+\n");
    hdaa_dump_pcmchannels(pdevinfo);
    HDA_DBG(HDA_DBG_INFO, "\n");
    HDA_DBG(HDA_DBG_INFO, "+-------------------------------+\n");
    HDA_DBG(HDA_DBG_INFO, "| DUMPING Playback/Record Paths |\n");
    HDA_DBG(HDA_DBG_INFO, "+-------------------------------+\n");
    hdaa_dump_dac(pdevinfo);
    hdaa_dump_adc(pdevinfo);
    hdaa_dump_mix(pdevinfo);
    HDA_DBG(HDA_DBG_INFO, "\n");
    HDA_DBG(HDA_DBG_INFO, "+-------------------------+\n");
    HDA_DBG(HDA_DBG_INFO, "| DUMPING Volume Controls |\n");
    HDA_DBG(HDA_DBG_INFO, "+-------------------------+\n");
    hdaa_dump_ctls(pdevinfo, "Master Volume", SOUND_MASK_VOLUME);
    hdaa_dump_ctls(pdevinfo, "PCM Volume", SOUND_MASK_PCM);
    hdaa_dump_ctls(pdevinfo, "CD Volume", SOUND_MASK_CD);
    hdaa_dump_ctls(pdevinfo, "Microphone Volume", SOUND_MASK_MIC);
    hdaa_dump_ctls(pdevinfo, "Microphone2 Volume", SOUND_MASK_MONITOR);
    hdaa_dump_ctls(pdevinfo, "Line-in Volume", SOUND_MASK_LINE);
    hdaa_dump_ctls(pdevinfo, "Speaker/Beep Volume", SOUND_MASK_SPEAKER);
    hdaa_dump_ctls(pdevinfo, "Recording Level", SOUND_MASK_RECLEV);
    hdaa_dump_ctls(pdevinfo, "Input Mix Level", SOUND_MASK_IMIX);
    hdaa_dump_ctls(pdevinfo, "Input Monitoring Level", SOUND_MASK_IGAIN);
    hdaa_dump_ctls(pdevinfo, NULL, 0);
    HDA_DBG(HDA_DBG_INFO, "\n");
    }

LOCAL void hdaa_dump_all_nodes(void)
{
	WIDGET *w, *cw;
	char buf[64];
	int i, j;
	HDCODEC* codec = global_codec;

	HDA_DBG(HDA_DBG_INFO, "\n");
	HDA_DBG(HDA_DBG_INFO, "Default Parameter\n");
	HDA_DBG(HDA_DBG_INFO, "-----------------\n");
	hdaa_dump_audio_formats(codec->pDev,
                            codec->supp_stream_formats,
                            codec->supp_pcm_size_rate);
	HDA_DBG(HDA_DBG_INFO, "         IN amp: 0x%08x\n",
	    codec->inamp_cap);
	HDA_DBG(HDA_DBG_INFO, "        OUT amp: 0x%08x\n",
	    codec->outamp_cap);
	for (i = 2; i < 39; i++) {
		w = widget_get(codec, i);
		if (w == NULL) {
			HDA_DBG(HDA_DBG_INFO, "Ghost widget nid=%d\n", i);
			continue;
		}
		HDA_DBG(HDA_DBG_INFO, "\n");
		HDA_DBG(HDA_DBG_INFO, "            nid: %d%s\n", w->nid,
		    (w->enable == 0) ? " [DISABLED]" : "");
/*		HDA_DBG(HDA_DBG_INFO, "           Name: %s\n", w->name); */
		HDA_DBG(HDA_DBG_INFO, "     Widget cap: 0x%08x\n",
		    w->param.widget_cap);
		if (w->param.widget_cap & 0x0ee1) {
			HDA_DBG(HDA_DBG_INFO, "                ");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_LR_SWAP(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " LRSWAP");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_POWER_CTRL(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " PWR");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " DIGITAL");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_UNSOL_CAP(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " UNSOL");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_PROC_WIDGET(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " PROC");
			if (HDA_PARAM_AUDIO_WIDGET_CAP_STRIPE(w->param.widget_cap))
			    HDA_DBG(HDA_DBG_INFO, " STRIPE(x%d)",
				1 << (ffsLsb(w->wclass.conv.stripecap) - 1));
			j = HDA_PARAM_AUDIO_WIDGET_CAP_CC(w->param.widget_cap);
			if (j == 1)
                {
			    HDA_DBG(HDA_DBG_INFO, " STEREO");
                }
			else if (j > 1)
                {
			    HDA_DBG(HDA_DBG_INFO, " %dCH", j + 1);
                }
			HDA_DBG(HDA_DBG_INFO, "\n");
		}
		if (w->bindas != -1) {
			HDA_DBG(HDA_DBG_INFO, "    Association: %d (0x%08x)\n",
			    w->bindas, w->bindseqmask);
		}
if(w->nid==10)
printf("nid(10): ossmask= %d, ossdev= %d\n", w->ossmask, w->ossdev);
		if (w->ossmask != 0 || w->ossdev >= 0) {
			HDA_DBG(HDA_DBG_INFO, "            OSS: %s",
			    hdaa_audio_ctl_ossmixer_mask2allname(w->ossmask, buf, sizeof(buf)));
			if (w->ossdev >= 0)
			    HDA_DBG(HDA_DBG_INFO, " (%s)", ossnames[w->ossdev]);
			HDA_DBG(HDA_DBG_INFO, "\n");
		}
		if (w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_OUTPUT ||
		    w->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT) {
            hdaa_dump_audio_formats(codec->pDev,
			    w->param.supp_stream_formats,
			    w->param.supp_pcm_size_rate);
		} else if (w->type ==
		    HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX || w->waspin)
			hdaa_dump_pin(w);
		if (w->param.eapdbtl != HDA_INVALID)
			HDA_DBG(HDA_DBG_INFO, "           EAPD: 0x%08x\n",
			    w->param.eapdbtl);
		if (HDA_PARAM_AUDIO_WIDGET_CAP_OUT_AMP(w->param.widget_cap) &&
		    w->param.outamp_cap != 0)
			hdaa_dump_amp(codec->pDev, w->param.outamp_cap, "Output");
		if (HDA_PARAM_AUDIO_WIDGET_CAP_IN_AMP(w->param.widget_cap) &&
		    w->param.inamp_cap != 0)
			hdaa_dump_amp(codec->pDev, w->param.inamp_cap, " Input");
		if (w->nconns > 0) {
			HDA_DBG(HDA_DBG_INFO, "    connections: %d\n", w->nconns);
			HDA_DBG(HDA_DBG_INFO, "          |\n");
		}
		for (j = 0; j < w->nconns; j++) {
			cw = widget_get(codec, w->conns[j]);
			HDA_DBG(HDA_DBG_INFO, "          + %s<- nid=%d [%s]",
                            (w->connsenable[j] == 0)?"[DISABLED] ":"",
                          w->conns[j], (cw == NULL) ? "GHOST!" : "name_here"); /* cw->name); */
			if (cw == NULL)
                {
				HDA_DBG(HDA_DBG_INFO, " [UNKNOWN]");
                }
			else if (cw->enable == 0)
                {
				HDA_DBG(HDA_DBG_INFO, " [DISABLED]");
                }
			if (w->nconns > 1 && w->selconn == j && w->type !=
			    HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_MIXER)
				HDA_DBG(HDA_DBG_INFO, " (selected)");
			HDA_DBG(HDA_DBG_INFO, "\n");
		}
	}

}
