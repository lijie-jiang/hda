/* sndWave.h - waveform audio support header file */

/* Copyright 2000 Wind River Systems, Inc. */

/*
modification history
--------------------
01a,15mar00,jlb  written
*/


#ifndef __INCsndwavh
#define __INCsndwavh


#if __cplusplus
extern "C" {
#endif

/*

DESCRIPTION 
 
This file defines miscellaneous definitions to process wave form audio. 

 
*/

typedef struct _riffchunk {
	char		id[4];
	uint32_t	size;
} RIFFCHUNK;

/* Waveform header */
typedef  struct
    {
	uint16_t    formatTag;           /* type of wave form data */
	uint16_t    channels;            /* number of audio channels */
	uint32_t    samplesPerSec;       /* audio samples per second */
	uint32_t    avgBytesPerSec;      /* average transfer rate */
	uint16_t    blockAlign;          /* bytes required for a single sample */
	uint16_t    bitsPerSample;       /* bits per sample */
    } WAVEFORMAT;


/* Type of wave forms */
#define WAVE_FORMAT_PCM     1


extern STATUS wavHeaderRead (int fd, int *pChannels, UINT32 *pSampleRate,
                      int *pSampleBits, UINT32 *pSamples, UINT32 *pDataStart);

extern STATUS wavHeaderWrite
    (
    int fd,               /* file descriptor of wave form file */
    int NumChannels,      /* number of channels */
    UINT32 SampleRate,    /* sample rate */
    int SampleBits,       /* size of sample */
    UINT32 Samples        /* number of samples */
    );

#if __cplusplus
} /* extern "C" */
#endif

#endif  /* __INCsndwavh */
