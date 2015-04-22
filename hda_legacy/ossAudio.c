/* ossAudio.c - OSS Audio driver header */

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
01a,06mar12,dmh  written.
01b,20nov12,jlj  support playback and recording for 8-bit,
                 one channel PCM audio.
*/

#include <ioLib.h>
#include <iosLib.h>
#include <fcntl.h>
#include <semLib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <drv/sound/soundcard.h>

#include <lstLib.h>
#include <logLib.h>

#include "audio/ossAudio.h"
#include "audio/hdAudio.h"

LOCAL int ossAudioDrvNum = -1;

/* imports */

IMPORT void channel_stop(PCM_CHANNEL* chan);
IMPORT int channel_setfragments(PCM_CHANNEL* chan, UINT32 blksz, UINT32 blkcnt);
IMPORT UINT32 channel_setspeed(PCM_CHANNEL* chan, UINT32 speed);
IMPORT int channel_setformat(PCM_CHANNEL *chan, UINT32 format);
IMPORT UINT32 channel_getptr(PCM_CHANNEL* chan);
IMPORT PCMCHAN_CAPS * channel_getcaps(PCM_CHANNEL* chan);
IMPORT int channel_trigger(PCM_CHANNEL* chan, int go);
IMPORT void * channel_init(void *data, struct snd_buf *b, PCM_CHANNEL *chan, int dir);

LOCAL void *  ossAudioOpen (DEV_HDR * pDevHdr, const char * fileName, int flags, int mode);
LOCAL int     ossAudioClose (void * pFileDesc);
LOCAL ssize_t ossAudioRead  (void * pFileDesc, char * buffer, size_t maxBytes);
LOCAL ssize_t ossAudioWrite (void * pFileDesc, char * buffer, size_t  maxBytes);
LOCAL int     ossAudioIoctl (void * pFileDesc, UINT32 function,  _Vx_ioctl_arg_t arg);

LOCAL PCM_CHANNEL* ossAudioFindChannel (DSP_DEV *pDspDev, int dir);
LOCAL STATUS ossAudioFreeFd(DSP_DEV *pDspDev, DSP_FD *pFd);
LOCAL DSP_FD * ossAudioAllocFd(DSP_DEV *pDspDev, int flags);
LOCAL ssize_t ossAudioIo (DSP_DEV *pDspDev, PCM_CHANNEL * pChan, char * buffer, size_t size, int dir);
LOCAL void ossAudioSync(DSP_DEV *pDspDev, PCM_CHANNEL* pChan);

STATUS ossAudioInit ()
    {
    if (ossAudioDrvNum > 0)
        return OK;
    
    ossAudioDrvNum = iosDrvInstall( NULL, NULL,
                                    (void*)ossAudioOpen, ossAudioClose,
                                    ossAudioRead, ossAudioWrite, ossAudioIoctl );
    if (ossAudioDrvNum > 0)
        return OK;

    return ERROR;
    }

void ossDeleteDsp (DSP_DEV* pDspDev)
    {
    int j;
    semTake (pDspDev->mutex, NO_WAIT);

    iosDevDelete ((DEV_HDR*)pDspDev);

    for (j = 0; j < pDspDev->num_chan; j++)
        {
        PCM_CHANNEL* pChan;

        pChan = &pDspDev->channel[j];
        semTake (pChan->sem, NO_WAIT);
        sndbuf_destroy (pChan->sndbuf);
        semDelete (pChan->sem);
        }
    
    semDelete (pDspDev->mutex);

    free (pDspDev->channel);

    free (pDspDev);
    }

DSP_DEV * ossCreateDsp (void)
    {
    DSP_DEV * pDspDev;
    char path[64];
    int j;

    static int i = 0;

    if (ossAudioInit() != OK)
        return NULL;

    snprintf(path, sizeof(path), "/dev/dsp%d", i++);

    pDspDev = calloc(1, sizeof(DSP_DEV));
    pDspDev->mutex = semMCreate (SEM_Q_FIFO);
    pDspDev->num_chan = 0;
    lstInit (&pDspDev->fdList);
    
    selWakeupListInit (&pDspDev->selWakeupList);

    pDspDev->num_chan = 16;
    pDspDev->channel = (struct pcm_channel *) calloc(pDspDev->num_chan, sizeof(PCM_CHANNEL));

    for (j = 0; j < pDspDev->num_chan; j++)
        {
        PCM_CHANNEL* pChan;
        
        pChan = &pDspDev->channel[j];
        pChan->sem = semCCreate (0, SEM_EMPTY);
        pChan->msem = semMCreate (SEM_Q_FIFO);
        pChan->sndbuf = sndbuf_create(pChan);
        }

    iosDevAdd((DEV_HDR*)pDspDev, path, ossAudioDrvNum);

    return pDspDev;
    }
    
LOCAL PCM_CHANNEL* ossAudioFindChannel (DSP_DEV *pDspDev, int dir)
    {
    int i;
    PCM_CHANNEL * pChan;
    
    for (i = 0; i < pDspDev->num_chan; i++)
        {
        pChan = &pDspDev->channel[i];
        
        if (pChan->dir == dir)
            {
            if (dir != PCM_DIR_NONE)
                {
                if (pChan->refcount == 0)
                    pChan->flags |= CHAN_FLAG_TRIGGER;
                pChan->refcount++;
                }
            return pChan;
            }
        }

    return NULL;
    }

LOCAL STATUS ossAudioFreeFd(DSP_DEV *pDspDev, DSP_FD *pFd)
    {
    if (pFd == NULL)
        return ERROR;

    if (pFd->record)
        pFd->record->refcount--;

    if (pFd->play)
        pFd->play->refcount--;

    lstDelete (&pDspDev->fdList, &pFd->fdNode);

    free(pFd);
    
    return OK;
    }

LOCAL DSP_FD * ossAudioAllocFd(DSP_DEV *pDspDev, int flags)
    {
    DSP_FD *pFd = NULL;
    DSP_FD *pLocalFd = NULL;
    NODE * pNode;
    STATUS rval = OK;
    
    pFd = calloc(1, sizeof(DSP_FD));
    if (pFd != NULL)
        {
        pFd->pDspDev = pDspDev;
        for(pNode = lstFirst(&pDspDev->fdList); pNode != NULL; pNode = lstNext(pNode))
            {
            pLocalFd = (DSP_FD *)pNode;
            switch (flags)
                {
                case O_RDONLY:
                    if (pLocalFd->record)
                        rval = ERROR;
                    break;

                case O_WRONLY:
                    if (pLocalFd->play)
                        rval = ERROR;
                    break;

                case O_RDWR:
                    if ((pLocalFd->play) || (pLocalFd->record))
                        rval = ERROR;
                    break;

                default:
                    rval = ERROR;
                    break;
                }
            }

        /* DSP file descriptor can be opened once concurrently */
        
        if(rval == OK)
            lstAdd (&pDspDev->fdList, &pFd->fdNode);
        else
            return NULL;
        }

    return pFd;
    }


LOCAL void *  ossAudioOpen (DEV_HDR * pDevHdr, const char * filename, int flags, int mode)
    {
    DSP_FD * pFd = NULL;
    DSP_DEV * pDspDev = (DSP_DEV*)pDevHdr;
    STATUS rval = ERROR;
    
    if (pDevHdr->drvNum == ossAudioDrvNum)
        {
        semTake (pDspDev->mutex, WAIT_FOREVER);

        if ((pFd = ossAudioAllocFd (pDspDev, flags)) == NULL)
            {
            semGive (pDspDev->mutex);
            return (void*)ERROR;
            }

        switch (flags)
            {
            case O_RDONLY:
                pFd->record = ossAudioFindChannel (pDspDev, PCM_DIR_REC);
                if (pFd->record)
                    rval = OK;
                break;

            case O_WRONLY:
                pFd->play = ossAudioFindChannel (pDspDev, PCM_DIR_PLAY);
                if (pFd->play)
                    rval = OK;
                break;

            case O_RDWR:
                pFd->play = ossAudioFindChannel (pDspDev, PCM_DIR_PLAY);
                pFd->record = ossAudioFindChannel (pDspDev, PCM_DIR_REC);
                if ((pFd->play) && (pFd->record))
                    rval = OK;
                break;

            default:
                rval = ERROR;
                break;
            }

        semGive (pDspDev->mutex);
        }

    if (rval == ERROR)
        {
        ossAudioFreeFd (pDspDev, pFd);
        pFd = (void*)ERROR;
        }
    
    return pFd;
    }

LOCAL int ossAudioClose (void * pFileDesc)
    {
    DSP_FD *pFd = (DSP_FD*)pFileDesc;
    DSP_DEV *pDspDev = pFd->pDspDev;

    if (pDspDev->devHdr.drvNum == ossAudioDrvNum)
        {
        semTake (pDspDev->mutex, WAIT_FOREVER);
    
        if ((pFd->record) && (pFd->record->refcount == 1))
            {
            /* channel stop operation*/
            channel_stop(pFd->record);

            /* channel reset operation */

            sndbuf_reset(pFd->record->sndbuf);
            }
        
        if ((pFd->play) && (pFd->play->refcount == 1))
            {
            ossAudioSync (pDspDev, pFd->play);

            /* channel reset operation */

            sndbuf_reset(pFd->play->sndbuf);
            }

        ossAudioFreeFd (pDspDev, pFd);
        
        /* release wake up list */
        selWakeupListTerm(&pDspDev->selWakeupList);
        
        semGive (pDspDev->mutex);
        }

	return OK;
    }

LOCAL ssize_t ossAudioIo (DSP_DEV *pDspDev, PCM_CHANNEL * pChan, char * buffer, size_t size, int dir)
    {
    signed int remainder = min(size, sndbuf_getsize(pChan->sndbuf));
    size_t bytes = 0;
    static int total = 0;

    while (remainder > 0)
        {
        if(semTake(pChan->sem, WAIT_FOREVER) != OK)
            {
            printf("%s: pChan->sem failed, bytes= x%x\n",__func__,bytes);
            return bytes;
            }
        pChan->semcnt--;

        if (dir == PCM_DIR_PLAY)
            bytes += sndbuf_copy (buffer + bytes, pChan->sndbuf,
                                  min (remainder, sndbuf_getblksz(pChan->sndbuf)));
        else
            bytes += sndbuf_read (buffer + bytes, pChan->sndbuf,
                                  min (remainder, sndbuf_getblksz(pChan->sndbuf)));

        if (pChan->flags & CHAN_FLAG_TRIGGER)
            {
            semTake (pDspDev->mutex, WAIT_FOREVER);
            if (channel_trigger(pChan, PCMTRIG_START) == OK)
                pChan->flags &= ~CHAN_FLAG_TRIGGER;
            semGive (pDspDev->mutex);
            }

        remainder -= bytes;
        total += bytes;
        }
    return bytes;
    }

LOCAL ssize_t ossAudioWrite (void * pFileDesc, char * buffer, size_t  maxBytes)
    {
    DSP_FD * pFd = (DSP_FD*)pFileDesc;
    DSP_DEV *pDspDev = pFd->pDspDev;
    PCM_CHANNEL * pChan = pFd->play;
    ssize_t bytes = 0;

    /*icelee, workaround for mono and/or 8bit stream.
     * 20/24bit is not considered here.*/
        
    INT32  val;
    INT16 *pNewBuffer;
    INT16 *ptr;
    int    i;
    int     newBytes;
    
#   define OSS_READ8   {val = ( ( (unsigned char)(buffer[i])-0x80 ) <<8 ),i++;}
#   define OSS_READ16  {val = (buffer[i] &0xFF) | ((buffer[i+1]<<8)); i+=2;}    
#   define OSS_WRITE16 {*ptr = val; ptr++;}     

    newBytes = maxBytes;
    if (pChan->afmt == AFMT_U8 )
    	newBytes <<= 1;
    if ( pChan->channels == 1  )
    	newBytes <<= 1;

#if 0
    ptr = pNewBuffer = (INT16 *)malloc(maxBytes * 4);

    if (pNewBuffer == NULL)
        {
        printf("%s: malloc failed!\n",__func__);
    	return 0;
    	}
#else
    if(pChan->sndbuf->shadow_buf_addr != NULL)
        ptr = pNewBuffer = pChan->sndbuf->shadow_buf_addr;
    else
        return 0;
#endif
    
    for (i=0; i<maxBytes; )
        {
	    if (pChan->afmt == AFMT_U8 )
	    	OSS_READ8
	    else
	    	OSS_READ16;
	    
	    OSS_WRITE16;
    	if ( pChan->channels == 1  )
            {
    	    OSS_WRITE16;
            }
        }

#   undef OSS_READ8
#   undef OSS_READ16
#   undef OSS_WRITE16

    semTake (pChan->msem, WAIT_FOREVER);

#if 0
    bytes = ossAudioIo (pDspDev, pFd->play, (char*)pNewBuffer, newBytes, PCM_DIR_PLAY);
    if (bytes < newBytes)
        {
        printf("%s: one more ossAudioIo\n",__func__);
        bytes = ossAudioIo (pDspDev, pFd->play, (char*)(pNewBuffer+(bytes/2)), newBytes-bytes, PCM_DIR_PLAY);
        }
#else    
    while(bytes < newBytes)
    {
    bytes += ossAudioIo (pDspDev, pChan, (char*)(pNewBuffer+(bytes/2)), newBytes-bytes, PCM_DIR_PLAY);
    }
#endif

    semGive (pChan->msem);
#if 0
    free(pNewBuffer);
#endif
    selWakeupAll (&pDspDev->selWakeupList, SELWRITE);
    return bytes;
    }

LOCAL ssize_t ossAudioRead  (void * pFileDesc, char * buffer, size_t maxBytes)
    {
    DSP_FD * pFd = (DSP_FD*)pFileDesc;
    DSP_DEV *pDspDev = pFd->pDspDev;
    ssize_t bytes = 0;
    PCM_CHANNEL * pChan = pFd->record;
    int channel, fmt;
    char * localBuffer = NULL;
    char * pU8;
    short * pU16;
    long * pU32;
    int i,j;
    int copyLen;

    semTake (pChan->msem, WAIT_FOREVER);
    
    fmt = pChan->afmt;
    channel = pChan->channels;

    if(channel == 1)
        maxBytes <<= 1;
    if(fmt == AFMT_U8)
        maxBytes <<= 1;

#if 0
    localBuffer = (char*)malloc(maxBytes);
    if(localBuffer == NULL)
        {
        printf("%s: malloc failed!\n",__func__);
        return 0;
        }
#else
    if(pChan->sndbuf->shadow_buf_addr != NULL)
        localBuffer = pChan->sndbuf->shadow_buf_addr;
    else
        return 0;
#endif

    bytes = ossAudioIo (pDspDev, pChan, localBuffer, maxBytes, PCM_DIR_REC);
    
    semGive (pChan->msem);

    pU8 = localBuffer;
    pU16 = (short*)localBuffer;
    pU32 = (long*)localBuffer;
    for(i=0, j=0; j < maxBytes/4; j++)
        {
        if(fmt == AFMT_U8)
            {
            short pcm16 = (short)(pU32[j] & 0xffff);
            pU8[i] = (pcm16 >> 8) + 128;
            }
        else
            pU16[i] = pU32[j];

        i++;

        if(channel == 2)
            {
            if(fmt == AFMT_U8)
                {
                short pcm16 = (short)(pU32[j] & 0xffff);
                pU8[i] = (pcm16 >> 8) + 128;
                }
            else
                pU16[i] = pU32[j];

            i++;
            }
        }    

    copyLen = bytes;
    if(channel == 1)
        copyLen /= 2;
    if(fmt == AFMT_U8)
        copyLen /= 2;
    
    bcopy(localBuffer, buffer, copyLen);
#if 0
    free(localBuffer);
#endif
    selWakeupAll (&pDspDev->selWakeupList, SELREAD);
    return copyLen;
    }

LOCAL void ossAudioSync(DSP_DEV *pDspDev, PCM_CHANNEL* pChan)
    {
    int count;

    count = sndbuf_getblkcnt(pChan->sndbuf) - pChan->semcnt;
    while (count > 0)
        {
        semTake (pChan->sem, WAIT_FOREVER);
        count--;
        }

    /* channel stop operation*/
    channel_stop(pChan);
    
    }

LOCAL int ossAudioIoctl (void * pFileDesc, UINT32 function,  _Vx_ioctl_arg_t arg)
    {
    DSP_FD * pFd = (DSP_FD*)pFileDesc;
    DSP_DEV * pDspDev = pFd->pDspDev;
    PCM_CHANNEL * pChan = NULL;

    /* ioctl code key
       'P' PCM "/dev/dsp" and "/dev/audio"
       'M' Mixer "/dev/mixer"
       'm' Midi, 'T' Timer, and 'Q' Sequencer
           for /dev/midi /dev/music and /dev/sequencer
           If someone recognizes the purpose for 'C' codes let me know
    */

    unsigned int ioctl_code = ((function >> 8) & 0xff);
    unsigned int data_direction = (function & SIOC_INOUT);
#if 0
    unsigned int data_size_bytes = ((function >> 16) & SIOCPARM_MASK);
    unsigned int data_size_dwords = data_size_bytes >> 2;
#endif
    unsigned int *data_buffer = NULL;

    if (data_direction & (SIOC_IN | SIOC_OUT))
        data_buffer = (unsigned int *)arg;

    if (ioctl_code == 'P')
        {        
        semTake (pDspDev->mutex, WAIT_FOREVER);
        switch (function)
            {
            case SNDCTL_DSP_SETFRAGMENT:
                pChan = pFd->record;
                if (pChan != NULL)
                    {
                    pChan->abinfo.fragsize = (1 << (data_buffer[0] & 0xffff));
                    pChan->abinfo.fragments = (data_buffer[0] >> 16);
                    pChan->abinfo.bytes = pChan->abinfo.fragments * pChan->abinfo.fragsize;
                    pChan->abinfo.fragstotal = pChan->abinfo.fragments;

                    channel_setfragments(pChan,
                                pChan->abinfo.fragsize, pChan->abinfo.fragments);

                    pChan->semcnt = sndbuf_getblkcnt(pChan->sndbuf);
                    semCInitialize((char*)pChan->sem, SEM_Q_FIFO , pChan->semcnt);
                    }

                pChan = pFd->play;
                if (pChan != NULL)
                    {
                    pChan->abinfo.fragsize = (1 << (data_buffer[0] & 0xffff));
                    pChan->abinfo.fragments = (data_buffer[0] >> 16);
                    pChan->abinfo.bytes = pChan->abinfo.fragments * pChan->abinfo.fragsize;
                    pChan->abinfo.fragstotal = pChan->abinfo.fragments;
                    
                    channel_setfragments(pChan,
                                pChan->abinfo.fragsize, pChan->abinfo.fragments);

                    pChan->semcnt = sndbuf_getblkcnt(pChan->sndbuf);
                    semCInitialize((char*)pChan->sem, SEM_Q_FIFO , pChan->semcnt);
                    }
                break;
                
#if 0
                /* OSS 4.0 feature */
            case SNDCTL_DSP_SETBLKSIZE:
                break;
#endif            

            case SNDCTL_DSP_GETBLKSIZE:
                pChan = ((pFd->play == NULL) ? pFd->record : pFd->play);
                if (pChan != NULL)
                    data_buffer[0] = pChan->abinfo.fragsize;
                break;

            case OSS_GETVERSION:
                data_buffer[0] = SOUND_VERSION;
                break;

            case SNDCTL_DSP_SPEED:
                pChan = pFd->play;
                if (pChan != NULL)
                    {
                    pChan->rate = data_buffer[0];
                    channel_setspeed(pChan, pChan->rate);
                    }

                pChan = pFd->record;
                if (pChan != NULL)
                    {
                    pChan->rate = data_buffer[0];
                    channel_setspeed(pChan, pChan->rate);
                    }
                break;

            case SNDCTL_DSP_SETFMT:
                pChan = pFd->play;
                if (pChan != NULL)
                    {
                    pChan->afmt = data_buffer[0];
                    channel_setformat(pChan, pChan->afmt);
                    }

                pChan = pFd->record;
                if (pChan != NULL)
                    {
                    pChan->afmt = data_buffer[0];
                    channel_setformat(pChan, pChan->afmt);
                    }
                break;
                
            case SNDCTL_DSP_STEREO:
                pChan = pFd->play;
                if (pChan != NULL)
                    {
                    if (data_buffer[0] == 1)
                        pChan->channels = 2;
                    else if (data_buffer[0] == 0)
                        pChan->channels = 1;
                    }
                
                pChan = pFd->record;
                if (pChan != NULL)
                    {
                    if (data_buffer[0] == 1)
                        pChan->channels = 2;
                    else if (data_buffer[0] == 0)
                        pChan->channels = 1;
                    }
                break;

            case SNDCTL_DSP_CHANNELS:
                pChan = pFd->play;
                if (pChan != NULL)
                    {
                    pChan->channels = data_buffer[0];
                    }
                
                pChan = pFd->record;
                if (pChan != NULL)
                    {
                    pChan->channels = data_buffer[0];
                    }
                break;
            
            case SNDCTL_DSP_GETFMTS:
                pChan = ((pFd->play == NULL) ? pFd->record : pFd->play);
                if (pChan != NULL)
                    data_buffer[0] = pChan->afmts;
                break;

            case SNDCTL_DSP_GETOSPACE:
                pChan = pFd->play;
                if ((pChan != NULL) && (data_buffer != NULL))
                    {
                    struct audio_buf_info* info = (struct audio_buf_info*)data_buffer;
                    memcpy (info, &pChan->abinfo, sizeof(struct audio_buf_info));
                    }
                break;
        
            case SNDCTL_DSP_GETISPACE:
                pChan = pFd->record;
                if ((pChan != NULL) && (data_buffer != NULL))
                    {
                    struct audio_buf_info* info = (struct audio_buf_info*)data_buffer;
                    memcpy (info, &pChan->abinfo, sizeof(struct audio_buf_info));
                    }
                break;
            
            case SNDCTL_DSP_GETIPTR:
                pChan = pFd->record;
                if (pChan != NULL)
                    data_buffer[0] = channel_getptr(pChan);
                break;

            case SNDCTL_DSP_GETOPTR:
                pChan = pFd->record;
                if (pChan != NULL)
                    data_buffer[0] = channel_getptr(pChan);
                break;

            case SNDCTL_DSP_GETCAPS:
                pChan = ((pFd->play == NULL) ? pFd->record : pFd->play);
                if (pChan != NULL)
                    {
                    PCMCHAN_CAPS *caps = (PCMCHAN_CAPS*) channel_getcaps(pChan);
                    data_buffer[0] = caps->caps;
                    }
                break;
                
            case SNDCTL_DSP_GETTRIGGER:
                if ((pFd->play) && (~pFd->play->flags & CHAN_FLAG_TRIGGER))
                    data_buffer[0] |= PCM_ENABLE_OUTPUT;
            
                if ((pFd->record) && (~pFd->play->flags & CHAN_FLAG_TRIGGER))
                    data_buffer[0] |= PCM_ENABLE_INPUT;
                break;

            case SNDCTL_DSP_SETTRIGGER:
                pChan = pFd->play;
                if (pChan)
                    {
                    if (data_buffer[0] & PCM_ENABLE_OUTPUT)
                        {
                        channel_trigger(pChan, PCMTRIG_START);
                        pChan->flags |= CHAN_FLAG_TRIGGER;
                        }
                    else
                        {
                        channel_trigger(pChan, PCMTRIG_ABORT);
                        pChan->flags &= ~CHAN_FLAG_TRIGGER;
                        }
                    }

                pChan = pFd->record;
                if (pChan)
                    {
                    if (data_buffer[0] & PCM_ENABLE_INPUT)
                        {
                        channel_trigger(pChan, PCMTRIG_START);
                        pChan->flags |= CHAN_FLAG_TRIGGER;
                        }
                    else
                        {
                        channel_trigger(pChan, PCMTRIG_ABORT);
                        pChan->flags &= ~CHAN_FLAG_TRIGGER;
                        }
                    }
                break;

            case SNDCTL_DSP_RESET:
                pChan = pFd->play;
                if (pChan)
                    {
                    channel_stop(pChan);
                    /* channel reset operation */
                    sndbuf_reset(pChan->sndbuf);
                    pChan->semcnt = sndbuf_getblkcnt(pChan->sndbuf);
                    semCInitialize((char*)pChan->sem, SEM_Q_FIFO , pChan->semcnt);
                    }
                pChan = pFd->record;
                if (pChan)
                    {
                    channel_stop(pChan);
                    /* channel reset operation */
                    sndbuf_reset(pChan->sndbuf);
                    pChan->semcnt = sndbuf_getblkcnt(pChan->sndbuf);
                    semCInitialize((char*)pChan->sem, SEM_Q_FIFO , pChan->semcnt);
                    }
                break;

            case SNDCTL_DSP_SYNC:
                pChan = pFd->play;
                if (pChan)
                    ossAudioSync(pDspDev, pFd->play);
                break;
                
                /* list of unsupported ioctls so far */                    
            case SNDCTL_DSP_POST:
            case SNDCTL_DSP_PROFILE:
            case SNDCTL_DSP_SETDUPLEX:
            case SNDCTL_DSP_NONBLOCK:
            case SNDCTL_DSP_GETODELAY:
            case SNDCTL_DSP_MAPINBUF:	
            case SNDCTL_DSP_MAPOUTBUF:	
            case SNDCTL_DSP_SETSYNCRO:
            case SOUND_PCM_READ_RATE:
            case SOUND_PCM_READ_BITS:
            case SOUND_PCM_WRITE_FILTER:
            case SOUND_PCM_READ_FILTER:
            case SNDCTL_DSP_SETOUTVOL:
            case SNDCTL_DSP_GETOUTVOL:
            default:
                semGive (pDspDev->mutex);
                return ERROR;
                break;
            }

        semGive (pDspDev->mutex);
        }

    /* select ioctl */

    semTake (pDspDev->mutex, WAIT_FOREVER);    
    switch (function)
        {
        case FIOSELECT:
            selNodeAdd (&pDspDev->selWakeupList, (SEL_WAKEUP_NODE *)arg);
            if(selWakeupType ((SEL_WAKEUP_NODE *) arg) == SELREAD)
                {
                pChan = pFd->record;
                if(pChan->semcnt == 0)
                    selWakeup ((SEL_WAKEUP_NODE *) arg);
                }
            else if(selWakeupType ((SEL_WAKEUP_NODE *) arg) == SELWRITE)
                {
                pChan = pFd->play;
                if(pChan->semcnt == 0)
                    selWakeup ((SEL_WAKEUP_NODE *) arg);
                }
            break;
        case FIOUNSELECT:
            selNodeDelete (&pDspDev->selWakeupList, (SEL_WAKEUP_NODE *)arg);
            break;
        default:
            break;
        }
    semGive (pDspDev->mutex);

    return OK;
    }

int osschannel_init (DSP_DEV * pDspDev, int dir, void * devinfo)
    {
    PCM_CHANNEL* pChan;

    pChan = ossAudioFindChannel (pDspDev, PCM_DIR_NONE);

    pChan->dir = dir;
    pChan->stream = devinfo;
    pChan->afmts = (AFMT_U8 | AFMT_S16_LE | AFMT_S32_LE);

    channel_init(devinfo, pChan->sndbuf, pChan, dir);

    pChan->semcnt = sndbuf_getblkcnt(pChan->sndbuf);
    semCInitialize((char*)pChan->sem, SEM_Q_FIFO, pChan->semcnt);

    return 0;
    }

/* runs in interrupt context from the audio controller driver interrupt handler */

void osschannel_intr (PCM_CHANNEL* pChan)
    {
    SND_BUF * b = pChan->sndbuf;

    b->tail += sndbuf_getblksz(b);
    b->tail = b->tail % sndbuf_getsize(b);
    pChan->semcnt++;
    semGive(pChan->sem);

#if 0
    if(pChan->dir == 1)
        logMsg("%s: b->tail= x%x, semcnt= %d\n", (int)__func__,(int)b->tail,pChan->semcnt,4,5,6);
    
    if (pChan->semcnt > sndbuf_getblkcnt(pChan->sndbuf))
        {
        logMsg("Problem, semcnt is %d\n", pChan->semcnt, 2,3,4,5,6);
        }
#endif
    }
