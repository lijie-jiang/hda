/* sndPlay.c - Wind River Media Library Audio file playing demonstration */

/* Copyright 2000-2005 Wind River Systems, Inc. */

/*

modification history
--------------------
01k,10jun05,rfm  Changed to make mixer optional
01j,31aug04,jlb  Allow execution from within RTP
01i,03apr03,jlb  Correct Diab compile warning (prototype match)
01h,14nov01,jlb  Fix fragment size setting (SPR 69972)
01g,20dec00,gav  Missed change to variable name.
01f,20dec00,gav  Entry point identical to filename w/o extension.
01e,06dec00,jlb  Open sound device read/write, fix compile warnings
01d,22nov00,gav  Added entry point using same form as other examples.
01c,14nov00,jlb  Changed name of wavplay.c to sndPlay.c and added
                 support for au (Sun) type of audio files
01b,11sep00,jlb  Use soundcard.h verses sound.h
01a,15mar00,jlb  written

*/

/*

DESCRIPTION

This file provides a demonstration of the use of the audio device
driver.  An audio file formatted as an <au> or a <wav>  file is played on
the audio device.   It demonstrates the method to open the audio and mixer
devices and to:

\ml
\m  Determine whether the audio stream is formatted as an <au> or a <wav> file
\m  Identify characteristics of the audio stream, such as number of channels, 
  number of samples, and size of each sample 
\m  Set the volume
\m  Select the number of audio channels
\m  Select the sample rate for the audio stream
\m  Select the size of a sample
\m  Initialize the buffering (set size of each fragment) and send audio fragments
 to the sound device.
\me

The program is started as follows in the kernel mode:

-> sndPlay <fileName>

In the RTP mode, the program is started as follows:

-> rtpSp "sndPlay \"<fileName>\""

The <fileName> specifies the file that contains the sound clip that is to be played.


*/


/* Includes */
#include <vxWorks.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <ioLib.h>
#include <unistd.h>
#include <string.h>
#include <selectLib.h>
#include <drv/sound/soundcard.h>
#include <semLib.h>
#include "sndWave.h"

#define AUDIO_DEVICE "/dev/dsp0"
#define MIXER_DEVICE "/dev/mixer0"

/* Define USE_SELECT to use select to wait for audio completion.  Undefine to
wait on write to device.
*/
#define TEST_TRIGGER
#undef TEST_TRIGGER

#define USE_SELECT
#undef USE_SELECT

#define TEST_SYNC
#undef TEST_SYNC

LOCAL BOOL exitFlag = FALSE;
LOCAL BOOL pauseFlag = FALSE;
LOCAL SEM_ID pauseSem  = NULL;
LOCAL UINT32 filePercent = 100;

STATUS soundPlay (char *);

IMPORT int sysClkRateGet(void);

/*****************************************************************************
*
* soundPlay - play a sound file 
*
* This routine plays the sound file <filename> on an audio device.  The audio
* file is read to obtain the header information. The audio stream is checked
* to see if the file is formated as a wav or an au file.   Using the header
* information the audio device is placed in the proper mode and then the
* audio stream is sent to the audio device .
*
* RETURNS: OK when the audio file was successfully played; otherwise ERROR
*
* ERRNO: N/A
*
*
* SEE ALSO: 
*
*
* NOMANUAL
*/
STATUS soundPlay 
    (
    char *filename              /* Audio file */
    )
    {
    unsigned char *buffer;
    int buffer_size, samplebits, i, blockSize, blockSize2;
    int size = 0;
    int fd, sd, md;
    int channels;
    int format = -1;
    UINT32 samplerate, samples;
    volatile UINT32 datastart;
    audio_buf_info     info;
    int file_size; 
    int stream_size;
    int stream_start;

#undef  SET_DEFAULT_VOL

    #define SET_DEFAULT_VOL

#ifdef SET_DEFAULT_VOL
    unsigned int vol_level = 60; /* percent value of maximum vloume */
    unsigned int maxVol = (vol_level << 8) | vol_level;
#endif /* SET_DEFAULT_VOL */

#ifdef USE_SELECT 
    fd_set writeFD;
#endif /* USE_SELECT */

#ifdef TEST_TRIGGER
    BOOL triggered = FALSE;
#endif

#if defined(TEST_TRIGGER) || defined (TEST_SYNC)
    int ioctl_bits = PCM_ENABLE_OUTPUT;
#endif

    /* Open the audio file */
    fd = open (filename, O_RDONLY, 0666);
    if (fd < 0)
        {
        printf ("Error opening file %s\n", filename);
        goto play_err;
        }

    /* retrieve file size */
    file_size = lseek(fd, 0, SEEK_END);

    /* Assume a wav file and read the wav form header file */
    if (wavHeaderRead (fd, &channels, &samplerate, &samplebits, 
                        &samples, (UINT32 *)&datastart) == OK)
        {
        /* Set the audio format */
        if (samplebits == 16)
            format = AFMT_S16_LE;
        else
            format = AFMT_U8;
        }
    else
        printf("wavHeaderRead return err\n");

    /* if format was set, then sound format was recognized */
    if (format == -1)
        {
        printf ("Sound file format not recognized\n");
        close (fd);
        goto play_err;
        }

    /* Position to the start of the audio data */
    if (lseek (fd, datastart, SEEK_SET) != datastart)
        {
        printf ("Sound file is corrupted\n");
        close (fd);
        goto play_err;
        }

    /* Open the audio device */
    sd = open (AUDIO_DEVICE, O_WRONLY, 0666);
    if (sd < 0)
        {
        printf("Unable to open the sound device - %s\n",AUDIO_DEVICE);
        close (fd);
        goto play_err;
        }

    /* Open the mixer device */
    md = open (MIXER_DEVICE, O_RDWR, 0666);
    if (md < 0)
        {
        printf("Mixer device unavailable - %s\n",MIXER_DEVICE);
        }

    /* Print characteristics of the sound data stream  */
    printf ("File name:   %s\n", filename);
    switch (format)
        {
        case AFMT_S16_LE:
            printf("WAV file - 16 bit signed little endian\n");
            size = samples * channels * (samplebits >> 3);
            break;
        case AFMT_U8:
            printf("WAV file - 8 bit unsigned\n");
            size = samples * channels * (samplebits >> 3);
            break;
        case AFMT_MU_LAW:
            printf("AU file - 8 bit muLaw\n");
            size = samples * channels;
            samplebits = 8;
            break;
        }

    printf ("Channels:    %d\n", channels);
    printf ("Sample Rate: %d\n", (int)samplerate);
    printf ("Sample Bits: %d\n", samplebits);
    printf ("samples:     %d\n", (int)samples);
    printf ("total size:  %d\n", size);

    /* get stream size */
    stream_size = size;
    stream_start = file_size - size;
    printf("stream_start = %d\n", stream_start);

#ifdef SET_DEFAULT_VOL
    /* Although the mixer has default volume settings, lets set the
     * volume for both channels to max
     */
    if (md > 0)
        {
        ioctl (md, SOUND_MIXER_WRITE_VOLUME, (int)&maxVol);
        }
#endif /* SET_DEFAULT_VOL */
    
    /* Set the device in proper mode for audio form characteristics */

    ioctl (sd, SNDCTL_DSP_CHANNELS, (int)&channels);
    ioctl (sd, SNDCTL_DSP_SPEED, (int)&samplerate);
    ioctl (sd, SNDCTL_DSP_SETFMT, (int)&format);

    /* Although the driver has a default fragment size, lets set 
     * to a size of 4k and use 2 fragments.
     * The argument to this call is an integer encoded as 0xMMMMSSSS
     * (in hex). The 16 least significant bits determine the
     * fragment size. The size is 2^SSSS. For example SSSS=0008 gives
     * fragment size of 256 bytes (2^8). The minimum is 16 bytes (SSSS=4)
     * and the maximum is total buffer size/2. Some devices or processor
     * architectures may require larger fragments - in this case the
     * requested fragment size is automatically increased.
     */
#if 1
    blockSize = blockSize2 = (4 << 16) | 12;
    ioctl (sd, SNDCTL_DSP_SETFRAGMENT, (int)&blockSize2);
#endif
    /* get the maximum data transfer size and allocate buffer.  The
     * size of the buffer to pass to the audio device is such that
     * each buffer is the maximum that the audio device can handle
     * without blocking.  It is important that the buffer size
     * be a multiple of the fragment size.
     */
    ioctl (sd, SNDCTL_DSP_GETOSPACE, (int)&info);
    blockSize = info.fragstotal * info.fragsize * 2;
    blockSize = info.fragstotal * info.fragsize;
    blockSize = info.fragsize;
    buffer = malloc (blockSize);
    printf("%s: blockSize= x%x, fragstotal= %d\n",__func__,blockSize,info.fragstotal);

    if (!buffer)
        {
        close (fd);
        goto play_err;
        }

#ifdef TEST_TRIGGER
    /* clear trigger bit */
    format = ~PCM_ENABLE_OUTPUT;
    ioctl (sd, SNDCTL_DSP_SETTRIGGER, &format);
#endif

    /* Create semaphore for pause */
    pauseSem = semBCreate(SEM_Q_PRIORITY, SEM_EMPTY);
    if (pauseSem == NULL)
        printf("Pause semaphore creating failed\n");

    /* Loop reading audio file and sending to audio device */
    while (size > 0)
        {
        int leftover;

        /* Pause */
        if (pauseFlag)
            {
            close(sd);
            semTake(pauseSem, WAIT_FOREVER);
            sd = open (AUDIO_DEVICE, O_WRONLY, 0666);
            ioctl (sd, SNDCTL_DSP_CHANNELS, (int)&channels);
            ioctl (sd, SNDCTL_DSP_SPEED, (int)&samplerate);
            ioctl (sd, SNDCTL_DSP_SETFMT, (int)&format);
            blockSize2 = (4 << 16) | 12;
            ioctl (sd, SNDCTL_DSP_SETFRAGMENT, (int)&blockSize2);
            }

        /* Exit playback */
        if (exitFlag)
            {
            exitFlag = FALSE;
            goto play_exit;
            }

        /* Set playback position */
        if (filePercent < 100)
            {
            long cur_position = (stream_size * filePercent) / 100;

            cur_position = blockSize * (cur_position / blockSize);
            size = stream_size - cur_position;
            lseek(fd, (cur_position + stream_start), SEEK_SET);
#if 0
            printf("cur_position = %d\n", cur_position);
            printf("size = %d\n", size);
            printf("lseek= %d\n", lseek(fd, (cur_position + stream_start), SEEK_SET));
#endif
            filePercent = 100;

            /* re-send audio stream */
            close(sd);
            sd = open (AUDIO_DEVICE, O_WRONLY, 0666);
            ioctl (sd, SNDCTL_DSP_CHANNELS, (int)&channels);
            ioctl (sd, SNDCTL_DSP_SPEED, (int)&samplerate);
            ioctl (sd, SNDCTL_DSP_SETFMT, (int)&format);
            blockSize2 = (4 << 16) | 12;
            ioctl (sd, SNDCTL_DSP_SETFRAGMENT, (int)&blockSize2);
            }
        
        /* If the audio stream has more than the block size, then
         * select a read of the block size, otherwise only
         * read the remaining data from the audio stream.
         */
        buffer_size = size > blockSize ? blockSize : size;

        /* Read a block of audio data */
        i = read (fd, (char *)buffer, buffer_size);
        datastart += buffer_size;

        /* Send audio data to audio device */
        {
        int len = buffer_size;
        unsigned char * ptr = buffer;
        do
            {
            leftover = write (sd, (char *)ptr, len);
            ptr += leftover;
            len -= leftover;
            } while (len > 0);
        }

        /* Optionally use the select processing */
#ifdef USE_SELECT 
        /* Pend until write complete */
        FD_ZERO (&writeFD);
        FD_SET (sd, &writeFD);
        select (FD_SETSIZE, NULL, &writeFD, NULL, NULL);
#endif /* USE_SELECT */

        /* Update remaining size of audio data */
        
        size -= buffer_size;

#ifdef TEST_TRIGGER
        if (!triggered)
            {
            printf("Hang out and trigger \n");
            taskDelay(sysClkRateGet() * 5);
            /* set trigger bit  */
            ioctl_bits = PCM_ENABLE_OUTPUT;
            ioctl (sd, SNDCTL_DSP_SETTRIGGER, &ioctl_bits);
            triggered = TRUE;
            }
#endif
        }

#ifdef TEST_SYNC
    printf("Hang out and sync @ %d\n", tickGet());
    ioctl (sd,  SNDCTL_DSP_SYNC, &ioctl_bits);
    printf("Hang out and sync, Sync Completed @ %d\n", tickGet());
#endif

play_exit:
    /* Close the audio file and the audio device */
    free (buffer);
    close (fd);
    close (sd);
    close (md);

    semDelete(pauseSem);
    pauseSem = NULL;
    
    return (OK);

play_err:
    return ERROR;
    }

void soundRepeat (char *filename, int iterations)
    {
    while (iterations >= 0)
        {
        soundPlay (filename);
        iterations--;
        taskDelay(sysClkRateGet() * 3);
        }
    }

/* Parameter:
 * left_lvl : left channel volume percent
 * right_lvl : right channel volume percent
 */

void soundVolSet (unsigned int left_lvl, unsigned int right_lvl)
    {
    int md;
    unsigned int input_lvl = (left_lvl&0xff) | ((right_lvl&0xff) << 8);

    md = open (MIXER_DEVICE, O_RDWR, 0666);
    if (md < 0)
        {
        printf("Mixer device unavailable - %s\n",MIXER_DEVICE);
        return;
        }

    ioctl (md, SOUND_MIXER_WRITE_VOLUME, (int)&input_lvl);   
    close (md);
    }

void soundPause(void)
    {
    pauseFlag = TRUE;
    }

void soundResume(void)
    {
    if (pauseSem != NULL)
        semGive(pauseSem);
    
    pauseFlag = FALSE;
    }

void soundExit(void)
    {
    exitFlag = TRUE;
    }

void soundPosition(UINT32 percent)
    {
    filePercent = percent;
    }
    
