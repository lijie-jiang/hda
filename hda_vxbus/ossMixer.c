/* ossMixer.c - OSS Mixer driver header */

/*
modification history
--------------------
01b,28may28,dmh  fix B0003
01a,06mar12,dmh  written.
*/


#include <ioLib.h>
#include <iosLib.h>
#include <fcntl.h>
#include <semLib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <drv/sound/soundcard.h>
#include <selectLib.h>
#include <lstLib.h>
#include <logLib.h>

#include "audio/ossAudio.h"

#define OSS_DEBUG

LOCAL int ossMixerDrvNum = -1;

/* import */
IMPORT int audio_ctl_ossmixer_init(SND_MIXER *m);
IMPORT UINT32 audio_ctl_ossmixer_setrecsrc(SND_MIXER *m, UINT32 src);
IMPORT int audio_ctl_ossmixer_set(SND_MIXER *m, unsigned dev, unsigned left, unsigned right);

LOCAL void *  ossMixerOpen (DEV_HDR * pDevHdr, const char * fileName, int flags, int mode);
LOCAL int     ossMixerClose (void * pFileDesc);
LOCAL int     ossMixerIoctl (void * pFileDesc, UINT32 function,  _Vx_ioctl_arg_t arg);

LOCAL STATUS ossMixerFreeFd(MIXER_DEV *pDspDev, MIXER_FD *pFd);
LOCAL MIXER_FD * ossMixerAllocFd(MIXER_DEV *pDspDev, int flags);


STATUS ossMixerInit ()
    {
    ossMixerDrvNum = iosDrvInstall( NULL, NULL,
                                    (void*)ossMixerOpen, ossMixerClose,
                                    NULL, NULL, ossMixerIoctl );

    return OK;
    }

void ossDeleteMixer (MIXER_DEV* pMixerDev)
    {
    semTake (pMixerDev->mutex, NO_WAIT);
    
    semDelete (pMixerDev->mutex);

    iosDevDelete ((DEV_HDR*)pMixerDev);

    ossmixer_delete (pMixerDev->mixer);
    
    free (pMixerDev);
    }

MIXER_DEV * ossCreateMixer (void)
    {
    MIXER_DEV* pMixerDev;
    char path[64];

    static int i = 0;
    
    if (ossMixerDrvNum == -1)
        ossMixerInit();

    snprintf(path, sizeof(path), "/dev/mixer%d", i++);

    pMixerDev = calloc(1, sizeof(MIXER_DEV));
    pMixerDev->mutex = semMCreate (SEM_Q_FIFO);

    lstInit (&pMixerDev->fdList);
             
    iosDevAdd((DEV_HDR*)pMixerDev, path, ossMixerDrvNum);

    return pMixerDev;
    }

int ossmixer_delete (struct snd_mixer *m)
    {
    free (m);
    return 0;
    }

STATUS ossmixer_init (MIXER_DEV * pMixerDev, void *pdevinfo)
    {
    struct snd_mixer *m;

    m = calloc (1, sizeof(struct snd_mixer));
    m->pdevinfo = pdevinfo;

    pMixerDev->mixer = m;
    audio_ctl_ossmixer_init(m);

    return OK;
    }

int ossmixer_setrecsrc (SND_MIXER *m, unsigned int src)
    {
    unsigned int recsrc;

    recsrc = audio_ctl_ossmixer_setrecsrc(m, src);
    if (recsrc > 0)
        m->recsrc = recsrc;

    return recsrc;
    }

int ossmixer_set (SND_MIXER *m, unsigned int dev, unsigned int level)
    {
    unsigned int left, right;
    
    right = (level >> 8) & 0xff;
    left = (level & 0xff);

    level = audio_ctl_ossmixer_set(m, dev, left, right);
    m->level[dev] = level;

    return level;
    }

int ossmixer_get (SND_MIXER *m, unsigned int dev)
    {
	if ((dev < SOUND_MIXER_NRDEVICES) && (m->devs & (1 << dev)))
        {
		return m->level[dev];
        }
	else
		return -1;
    }


LOCAL STATUS ossMixerFreeFd(MIXER_DEV *pMixerDev, MIXER_FD *pFd)
    {
    if (pFd == NULL)
        return ERROR;

    lstDelete (&pMixerDev->fdList, &pFd->fdNode);

    free(pFd);
    
    return OK;
    }

LOCAL MIXER_FD * ossMixerAllocFd(MIXER_DEV *pMixerDev, int flags)
    {
    MIXER_FD *openFd;

    openFd = calloc(1, sizeof(MIXER_FD));

    if (openFd != NULL)
        {
        openFd->pMixerDev = pMixerDev;
        lstAdd (&pMixerDev->fdList, &openFd->fdNode);
        }

    return openFd;
    }

LOCAL void *  ossMixerOpen (DEV_HDR * pDevHdr, const char * filename, int flags, int mode)
    {
    MIXER_FD * pFd = (void*)ERROR;
    MIXER_DEV * pMixerDev = (MIXER_DEV*)pDevHdr;

    if (pDevHdr->drvNum == ossMixerDrvNum)
        {
        semTake (pMixerDev->mutex, WAIT_FOREVER);

        if ((pFd = ossMixerAllocFd (pMixerDev, flags)) == NULL)
            {
            semGive (pMixerDev->mutex);
            return (void*)ERROR;
            }

        }

    semGive (pMixerDev->mutex);
    return pFd;
    }

LOCAL int ossMixerClose (void * pFileDesc)
    {
    MIXER_FD *pFd = (MIXER_FD*)pFileDesc;
    MIXER_DEV *pMixerDev = pFd->pMixerDev;
    STATUS rval;
    
    semTake (pMixerDev->mutex, WAIT_FOREVER);
    
    rval = ossMixerFreeFd(pMixerDev, pFd);

    semGive (pMixerDev->mutex);

	return rval;
    }


#ifdef OSS_DEBUG
static struct ioctl_str_t {
        unsigned int    cmd;
        const char     *str;
} ioctl_str[] = {
        {SNDCTL_DSP_RESET, "SNDCTL_DSP_RESET"},
        {SNDCTL_DSP_SYNC, "SNDCTL_DSP_SYNC"},
        {SNDCTL_DSP_SPEED, "SNDCTL_DSP_SPEED"},
        {SNDCTL_DSP_STEREO, "SNDCTL_DSP_STEREO"},
        {SNDCTL_DSP_GETBLKSIZE, "SNDCTL_DSP_GETBLKSIZE"},
        {SNDCTL_DSP_CHANNELS, "SNDCTL_DSP_CHANNELS"},
        {SOUND_PCM_WRITE_CHANNELS, "SOUND_PCM_WRITE_CHANNELS"},
        {SOUND_PCM_WRITE_FILTER, "SOUND_PCM_WRITE_FILTER"},
        {SNDCTL_DSP_POST, "SNDCTL_DSP_POST"},
        {SNDCTL_DSP_SUBDIVIDE, "SNDCTL_DSP_SUBDIVIDE"},
        {SNDCTL_DSP_SETFRAGMENT, "SNDCTL_DSP_SETFRAGMENT"},
        {SNDCTL_DSP_GETFMTS, "SNDCTL_DSP_GETFMTS"},
        {SNDCTL_DSP_SETFMT, "SNDCTL_DSP_SETFMT"},
        {SNDCTL_DSP_GETOSPACE, "SNDCTL_DSP_GETOSPACE"},
        {SNDCTL_DSP_GETISPACE, "SNDCTL_DSP_GETISPACE"},
        {SNDCTL_DSP_NONBLOCK, "SNDCTL_DSP_NONBLOCK"},
        {SNDCTL_DSP_GETCAPS, "SNDCTL_DSP_GETCAPS"},
        {SNDCTL_DSP_GETTRIGGER, "SNDCTL_DSP_GETTRIGGER"},
        {SNDCTL_DSP_SETTRIGGER, "SNDCTL_DSP_SETTRIGGER"},
        {SNDCTL_DSP_GETIPTR, "SNDCTL_DSP_GETIPTR"},
        {SNDCTL_DSP_GETOPTR, "SNDCTL_DSP_GETOPTR"},
        {SNDCTL_DSP_MAPINBUF, "SNDCTL_DSP_MAPINBUF"},
        {SNDCTL_DSP_MAPOUTBUF, "SNDCTL_DSP_MAPOUTBUF"},
        {SNDCTL_DSP_SETSYNCRO, "SNDCTL_DSP_SETSYNCRO"},
        {SNDCTL_DSP_SETDUPLEX, "SNDCTL_DSP_SETDUPLEX"},
        {SNDCTL_DSP_GETODELAY, "SNDCTL_DSP_GETODELAY"},
        {SOUND_PCM_READ_RATE, "SOUND_PCM_READ_RATE"},
        {SOUND_PCM_READ_CHANNELS, "SOUND_PCM_READ_CHANNELS"},
        {SOUND_PCM_READ_BITS, "SOUND_PCM_READ_BITS"},
        {SOUND_PCM_READ_FILTER, "SOUND_PCM_READ_FILTER"},
        {OSS_GETVERSION, "OSS_GETVERSION"},
        {SOUND_MIXER_PRIVATE1, "SOUND_MIXER_PRIVATE1"},
        {SOUND_MIXER_PRIVATE2, "SOUND_MIXER_PRIVATE2"},
        {SOUND_MIXER_PRIVATE3, "SOUND_MIXER_PRIVATE3"},
        {SOUND_MIXER_PRIVATE4, "SOUND_MIXER_PRIVATE4"},
        {SOUND_MIXER_PRIVATE5, "SOUND_MIXER_PRIVATE5"},
        {SOUND_MIXER_ACCESS,   "SOUND_MIXER_ACCESS"},
        {SOUND_MIXER_INFO,     "SOUND_MIXER_INFO"},
        {SOUND_OLD_MIXER_INFO,       "SOUND_OLD_MIXER_INFO"},
        {SOUND_MIXER_READ_VOLUME, "SOUND_MIXER_READ_VOLUME"},
        {SOUND_MIXER_READ_BASS, "SOUND_MIXER_READ_BASS"},
        {SOUND_MIXER_READ_TREBLE, "SOUND_MIXER_READ_TREBLE"},
        {SOUND_MIXER_READ_SYNTH, "SOUND_MIXER_READ_SYNTH"},
        {SOUND_MIXER_READ_PCM, "SOUND_MIXER_READ_PCM"},
        {SOUND_MIXER_READ_SPEAKER,  "SOUND_MIXER_READ_SPEAKER"},
        {SOUND_MIXER_READ_LINE, "SOUND_MIXER_READ_LINE"},
        {SOUND_MIXER_READ_MIC, "SOUND_MIXER_READ_MIC"},
        {SOUND_MIXER_READ_CD    , "SOUND_MIXER_READ_CD"},
        {SOUND_MIXER_READ_IMIX, "SOUND_MIXER_READ_IMIX"},
        {SOUND_MIXER_READ_ALTPCM, "SOUND_MIXER_READ_ALTPCM"},
        {SOUND_MIXER_READ_RECLEV, "SOUND_MIXER_READ_RECLEV"},
        {SOUND_MIXER_READ_IGAIN, "SOUND_MIXER_READ_IGAIN"},
        {SOUND_MIXER_READ_OGAIN, "SOUND_MIXER_READ_OGAIN"},
        {SOUND_MIXER_READ_LINE1, "SOUND_MIXER_READ_LINE1"},
        {SOUND_MIXER_READ_LINE2, "SOUND_MIXER_READ_LINE2"},
        {SOUND_MIXER_READ_LINE3, "SOUND_MIXER_READ_LINE3"},
        {SOUND_MIXER_READ_RECSRC, "SOUND_MIXER_READ_RECSRC"},
        {SOUND_MIXER_READ_DEVMASK,  "SOUND_MIXER_READ_DEVMASK"},
        {SOUND_MIXER_READ_RECMASK,  "SOUND_MIXER_READ_RECMASK"},
        {SOUND_MIXER_READ_STEREODEVS ,    "SOUND_MIXER_READ_STEREODEVS"},
        {SOUND_MIXER_READ_CAPS, "SOUND_MIXER_READ_CAPS"},
        {SOUND_MIXER_WRITE_VOLUME,  "SOUND_MIXER_WRITE_VOLUME"},
        {SOUND_MIXER_WRITE_BASS, "SOUND_MIXER_WRITE_BASS"},
        {SOUND_MIXER_WRITE_TREBLE,  "SOUND_MIXER_WRITE_TREBLE"},
        {SOUND_MIXER_WRITE_SYNTH, "SOUND_MIXER_WRITE_SYNTH"},
        {SOUND_MIXER_WRITE_PCM, "SOUND_MIXER_WRITE_PCM"},
        {SOUND_MIXER_WRITE_SPEAKER ,  "SOUND_MIXER_WRITE_SPEAKER"},
        {SOUND_MIXER_WRITE_LINE, "SOUND_MIXER_WRITE_LINE"},
        {SOUND_MIXER_WRITE_MIC, "SOUND_MIXER_WRITE_MIC"},
        {SOUND_MIXER_WRITE_CD, "SOUND_MIXER_WRITE_CD"},
        {SOUND_MIXER_WRITE_IMIX, "SOUND_MIXER_WRITE_IMIX"},
        {SOUND_MIXER_WRITE_ALTPCM,  "SOUND_MIXER_WRITE_ALTPCM"},
        {SOUND_MIXER_WRITE_RECLEV,  "SOUND_MIXER_WRITE_RECLEV"},
        {SOUND_MIXER_WRITE_IGAIN, "SOUND_MIXER_WRITE_IGAIN"},
        {SOUND_MIXER_WRITE_IGAIN, "SOUND_MIXER_WRITE_IGAIN"},
        {SOUND_MIXER_WRITE_OGAIN, "SOUND_MIXER_WRITE_OGAIN"},
        {SOUND_MIXER_WRITE_LINE1, "SOUND_MIXER_WRITE_LINE1"},
        {SOUND_MIXER_WRITE_LINE2, "SOUND_MIXER_WRITE_LINE2"},
        {SOUND_MIXER_WRITE_LINE3, "SOUND_MIXER_WRITE_LINE3"},
        {SOUND_MIXER_WRITE_RECSRC,  "SOUND_MIXER_WRITE_RECSRC"}

};
#endif

LOCAL int     ossMixerIoctl (void * pFileDesc, UINT32 function,  _Vx_ioctl_arg_t arg)
    {
    MIXER_FD * pFd = (MIXER_FD*)pFileDesc;
    MIXER_DEV *pMixerDev = pFd->pMixerDev;
    UINT32 cmd = function & 0xff;
    
    unsigned int ioctl_code = ((function >> 8) & 0xff);
    unsigned int data_direction = (function & SIOC_INOUT);
#if 0
    unsigned int data_size_bytes = ((function >> 16) & SIOCPARM_MASK);
    unsigned int data_size_dwords = data_size_bytes >> 2;
#endif
    unsigned int *data_buffer = NULL;

#ifdef OSS_DEBUG
    {
    int count;
    for (count = 0; count < NELEMENTS(ioctl_str); count++)
        {
        if (ioctl_str[count].cmd == function)
            break;
        }

    if (count < NELEMENTS(ioctl_str))
        logMsg("ioctl %s, arg=0x%lx\n", (int)ioctl_str[count].str, arg,3,4,5,6);
    else
        logMsg("ioctl 0x%x unknown, arg=0x%lx\n", function, arg,3,4,5,6);
    }
#endif

    if (data_direction & (SIOC_IN | SIOC_OUT))
        data_buffer = (unsigned int *)arg;

    /* ioctl code key
       'P' PCM "/dev/dsp" and "/dev/audio"
       'M' Mixer "/dev/mixer"
       'm' Midi, 'T' Timer, and 'Q' Sequencer
           for /dev/midi /dev/music and /dev/sequencer
           If someone recognizes the purpose for 'C' codes let me know
    */

#ifdef OSS_DEBUG
    logMsg("code= %c, cmd=0x%lx\n", (int)ioctl_code, cmd,3,4,5,6);
#endif

    if (ioctl_code == 'M')
        {
        int temp1;
        int temp2;
        temp1 = MIXER_READ(0);
        temp2 = MIXER_READ(SOUND_MIXER_LINE1);

        if ((function & ~0xff) == MIXER_WRITE(0))
            {
            switch (cmd)
                {
                UINT32 level, dev;

                case SOUND_MIXER_RECSRC:
                    data_buffer[0] = ossmixer_setrecsrc (pMixerDev->mixer, data_buffer[0]);
                    break;

                default:
                    level = data_buffer[0];
                    dev = function & 0xff;
                    data_buffer[0] = ossmixer_set (pMixerDev->mixer, dev, level);
                    break;
                }
            }
        if ((function & ~0xff) == MIXER_READ(0))
            {
            switch (cmd)
                {
                UINT32 dev;

                case SOUND_MIXER_DEVMASK:
                case SOUND_MIXER_CAPS:
                case SOUND_MIXER_STEREODEVS:
                    data_buffer[0] = ossmix_getdevs(pMixerDev->mixer);
                    break;
                case SOUND_MIXER_RECMASK:
                    data_buffer[0] = ossmix_getrecdevs(pMixerDev->mixer);
                    break;
                case SOUND_MIXER_RECSRC:
                    data_buffer[0] = ossmix_getrecsrc(pMixerDev->mixer);
                    break;
                default:
                    dev = function & 0xff;
                    data_buffer[0] = ossmix_get(pMixerDev->mixer, dev);
                    break;
                }
            }

        }
    
    semGive (pMixerDev->mutex);
    return OK;
    }

