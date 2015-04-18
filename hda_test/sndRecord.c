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
#include "drv/sound/soundcard.h"
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
/*
static char * snd_dev_labels[SOUND_MIXER_NRDEVICES] = SOUND_DEVICE_LABELS;
*/

#define LEFT_CHANNEL_VOLUME(v)  ((v) & 0xff)
#define RIGHT_CHANNEL_VOLUME(v) (((v) & 0xff00) >> 8)

/*****************************************************************************
*
* soundRecord - play a sound file 
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
STATUS soundRecord
    (
    char *filename,              /* Audio file */
    int seconds,
    int volume,
    int rec_vol
    )
    {
    unsigned char *buffer;
    int buffer_size, samplebits, i, blockSize;
    int size = 0;
    int fd, sd;/*, md;*/
    int channels;
    UINT32 format;
    UINT32 samplerate, samples;
    audio_buf_info     info;
/*    int left = 0, right = 0;
    int vol = 0;

    int reclev = (rec_vol << 8) | rec_vol;
*/
volume = (volume << 8) | volume;

#ifdef TEST_TRIGGER
    BOOL triggered = FALSE;
#endif
/*    int ioctl_bits = PCM_ENABLE_OUTPUT;*/

    /* Open the audio file */
    fd = open (filename, O_WRONLY | O_CREAT, 0666);
    if (fd < 0)
        {
        printf ("Error opening file %s\n", filename);
        return (ERROR);
        }

    samplebits = 16;
    format = AFMT_S16_LE;
    samplerate = 44100;
    channels = 2;

    if (seconds > 0)
        samples = seconds * samplerate;
    else
        samples = 981504;

    wavHeaderWrite(fd, channels, samplerate, samplebits, samples);

    /* Open the audio device */
    sd = open (AUDIO_DEVICE, O_RDONLY, 0666);
    if (sd < 0)
        {
        printf("Unable to open the sound device - %s\n",AUDIO_DEVICE);
        close (fd);
        return (ERROR);
        }
#if 0
    /* Open the mixer device */
    md = open (MIXER_DEVICE, O_RDWR, 0666);
    if (md < 0)
        {
        printf("Mixer device unavailable - %s\n",MIXER_DEVICE);
        }
#endif
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

#if 0
    ioctl(md, SOUND_MIXER_READ_VOLUME, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("VOLUME Left (%d%%) Right(%d%%)\n", left, right);

    /* Although the mixer has default volume settings, lets set the
     * volume for both channels to max
     */
    if (md > 0)
        {
        ioctl (md, SOUND_MIXER_WRITE_VOLUME, (int)&volume);
        }

    ioctl(md, SOUND_MIXER_READ_VOLUME, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("VOLUME Left (%d%%) Right(%d%%)\n", left, right);
#endif

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
    blockSize = (4 << 16) | 12;
    ioctl (sd, SNDCTL_DSP_SETFRAGMENT, (int)&blockSize);

    /* get the maximum data transfer size and allocate buffer.  The
     * size of the buffer to pass to the audio device is such that
     * each buffer is the maximum that the audio device can handle
     * without blocking.  It is important that the buffer size
     * be a multiple of the fragment size.
     */
    ioctl (sd, SNDCTL_DSP_GETISPACE, (int)&info);
    blockSize = info.fragstotal * info.fragsize * 2;
    blockSize = info.fragstotal * info.fragsize;
    blockSize = info.fragsize;
    buffer = calloc (1,blockSize);
    if (!buffer)
        {
        close (fd);
        return -1;
        }
#if 0
    ioctl (md, SOUND_MIXER_READ_RECMASK, &ioctl_bits);
    printf ("SOUND_MIXER_RECMASK: 0x%x\n", ioctl_bits);

    ioctl (md, SOUND_MIXER_READ_RECSRC, &ioctl_bits);

    printf ("SOUND_MIXER_RECSRC: %s 0x%x\n", snd_dev_labels[ffsLsb(ioctl_bits) - 1], ioctl_bits);
#if 0
    if (ioctl_bits > 0)
        
        printf ("SOUND_MIXER_RECSRC: %s 0x%x\n", snd_dev_labels[ffsLsb(ioctl_bits) - 1], ioctl_bits);
    else
        printf ("SOUND_MIXER_RECSRC: no recording source selected\n");
#endif
    ioctl (md, SOUND_MIXER_READ_DEVMASK, &ioctl_bits);
    printf ("SOUND_MIXER_DEVMASK: 0x%x\n", ioctl_bits);

#if 0
    ioctl_bits = SOUND_MASK_MONITOR;
    ioctl_bits = SOUND_MASK_LINE;
    ioctl_bits = SOUND_MASK_MONITOR;
#endif
    ioctl_bits = SOUND_MASK_MIC;

    ioctl (md, SOUND_MIXER_WRITE_RECSRC, &ioctl_bits);

    ioctl (md, SOUND_MIXER_READ_RECSRC, &ioctl_bits);
    printf ("SOUND_MIXER_RECSRC: %s 0x%x\n", snd_dev_labels[ffsLsb(ioctl_bits) - 1], ioctl_bits);
#if 0
    if (ioctl_bits > 0)
        printf ("SOUND_MIXER_RECSRC: %s 0x%x\n", snd_dev_labels[ffsLsb(ioctl_bits) - 1], ioctl_bits);
    else
        printf ("SOUND_MIXER_RECSRC: no recording source selected\n");
#endif

    /* reclev */
    ioctl(md, SOUND_MIXER_READ_RECLEV, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("RECLEV Left (%d%%) Right(%d%%)\n", left, right);

    ioctl (md, SOUND_MIXER_WRITE_RECLEV, (int)&reclev);

    ioctl(md, SOUND_MIXER_READ_RECLEV, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("RECLEV Left (%d%%) Right(%d%%)\n", left, right);

    /* line in*/
    ioctl(md, SOUND_MIXER_READ_RECLEV, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("RECLEV Left (%d%%) Right(%d%%)\n", left, right);

    ioctl (md, SOUND_MIXER_WRITE_RECLEV, (int)&reclev);

    ioctl(md, SOUND_MIXER_READ_RECLEV, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("RECLEV Left (%d%%) Right(%d%%)\n", left, right);

    /* mic */
    ioctl(md, SOUND_MIXER_READ_MIC, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("MIC Left (%d%%) Right(%d%%)\n", left, right);

    ioctl (md, SOUND_MIXER_WRITE_MIC, (int)&reclev);

    ioctl(md, SOUND_MIXER_READ_MIC, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("MIC Left (%d%%) Right(%d%%)\n", left, right);

    /* IGAIN */
    ioctl(md, SOUND_MIXER_READ_IGAIN, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("IGAIN Left (%d%%) Right(%d%%)\n", left, right);

    ioctl (md, SOUND_MIXER_WRITE_IGAIN, (int)&reclev);

    ioctl(md, SOUND_MIXER_READ_IGAIN, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("IGAIN Left (%d%%) Right(%d%%)\n", left, right);

    /* IMIX */
    ioctl(md, SOUND_MIXER_READ_IMIX, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("IMIX Left (%d%%) Right(%d%%)\n", left, right);

    ioctl (md, SOUND_MIXER_WRITE_IMIX, (int)&reclev);

    ioctl(md, SOUND_MIXER_READ_IMIX, &vol);
    left = LEFT_CHANNEL_VOLUME(vol);
    right = RIGHT_CHANNEL_VOLUME(vol);
    printf("IMIX Left (%d%%) Right(%d%%)\n", left, right);
#endif

#ifdef TEST_TRIGGER
    /* clear trigger bit */
    format = ~PCM_ENABLE_INPUT;
    ioctl (sd, SNDCTL_DSP_SETTRIGGER, &format);
#endif

    /* Loop reading audio file and sending to audio device */
    while (size > 0)
        {
        int leftover;
        
        /* If the audio stream has more than the block size, then
         * select a read of the block size, otherwise only
         * read the remaining data from the audio stream.
         */
        buffer_size = size > blockSize ? blockSize : size;

        /* Send audio data to audio device */
        {
        int len = buffer_size;
        unsigned char * ptr = buffer;
        do
            {
            leftover = read (sd, (char *)ptr, len);
            ptr += leftover;
            len -= leftover;
            } while (len > 0);
        }

        /* Update remaining size of audio data */
        
        size -= buffer_size;

#ifdef TEST_TRIGGER
        if (!triggered)
            {
            printf("Hang out and trigger \n");
            taskDelay(sysClkRateGet() * 5);
            /* set trigger bit  */
            ioctl_bits = PCM_ENABLE_INPUT;
            ioctl (sd, SNDCTL_DSP_SETTRIGGER, &ioctl_bits);
            triggered = TRUE;
            }
#endif

        /* Write a block of audio data */
        i = write (fd, (char *)buffer, buffer_size);

        }

    /* Close the audio file and the audio device */
    free (buffer);
    if (close (fd) == ERROR)
        printf ("Failed to close fd\n");
    if (close (sd) == ERROR)
        printf ("Failed to close sd\n");
#if 0
    if (close (md) == ERROR)
        printf ("Failed to close md\n");
#endif
    return (OK);
    }
