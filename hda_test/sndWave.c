/* sndWave.c - utility functions to handle a wave file */

/* Copyright 2000 - 2006 Wind River Systems, Inc. */

/*
modification history
--------------------
01d,10jan06,jlb  correct compiler warning
01c,08mar02,gav  Refgen cleaup
01b,11sep00,jlb  Use soundcard.h verses sound.h
01a,20mar00,jlb  written
*/

/*

DESCRIPTION

This file provides utility functions for handling wave files.
*/

/* Includes */
#include <vxWorks.h>
#include <stdlib.h>
#include <stdio.h>
#include <ioLib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "sndWave.h"


#define BUFFERSIZE 1024

/*****************************************************************************
*
* wavChunkFind - locate a chunk within a wave form audio file 
*
* This routine locates a chunk within an audio wave form file.  The 
* starting location to locate the chunk is specified by <pStart> and
* the chunk to locate is specified by <fourcc>.  The length of the data
* section to search is provided in <n>. 
*
* RETURNS: Pointer to the start of the chunk when found; otherwise 
*       NULL
*
* ERRNO: N/A
*
* SEE ALSO: 
*/
char *wavChunkFind 
    (
    char *pstart,               /* start of the search */
    char *fourcc,               /* chunk type */
    size_t n                    /* length of the search */
    )

    {
    char	*pend;
    int	k, test;
    
    for (pend = pstart + n; pstart < pend; pstart++)
        {
        if (*pstart == *fourcc)
            {
            for (test = TRUE, k = 1; fourcc[k]; k++)
                if (test)
                    test = (pstart[k] == fourcc[k]);
            if (test) 
                return (pstart);
            }
        }
    
    return NULL;
    }



/*****************************************************************************
*
* wavHeaderRead - read a WAVE form header from file 
*
* This routine reads the WAVE form file <fd> to retrieve its
* header information.  The number of channels, sample rate, number of bits
* per sample, number of samples, and the starting location of the data 
* are returned as parameters.
*
* RETURNS: OK when header successfully read; otherwise ERROR
*
* ERRNO: N/A
*
* NOTES:
*   This function is not reentrant
*
* SEE ALSO: 
*
*/

STATUS wavHeaderRead 
    (
    int fd,                 /* file descriptor of wave form file */
    int *pChannels,         /* number of channels */
    UINT32 *pSampleRate,    /* sample rate */
    int *pSampleBits,       /* size of sample */
    UINT32 *pSamples,       /* number of samples */
    UINT32 *pDataStart      /* start of data section */
    )
    {
    static WAVEFORMAT  waveformat ;
    static char buffer[BUFFERSIZE];
    char*   ptr ;
    UINT32  databytes ;
    
    /* Position to start of file */
    if (lseek (fd, 0L, SEEK_SET)) 
        return  (ERROR);

    /* Read enough data from the file to get the header */
    read (fd, buffer, BUFFERSIZE) ;
    
    /* Locate the required chunks */
    if (wavChunkFind (buffer, "RIFF", BUFFERSIZE) != buffer) 
        return (ERROR);
    if (!wavChunkFind (buffer, "WAVE", BUFFERSIZE)) 
        return (ERROR);
    ptr = wavChunkFind (buffer, "fmt ", BUFFERSIZE) ;
    if (!ptr) 
        return (ERROR);
    
    ptr += 4 ;	/* Move past "fmt "*/

    ptr += 4 ;	/* Move past subchunk1 size*/

    memcpy (&waveformat, ptr, sizeof (WAVEFORMAT)) ;
    
    if (waveformat.formatTag != WAVE_FORMAT_PCM) 
        return (ERROR);
    
    ptr = wavChunkFind (buffer, "data", BUFFERSIZE) ;
    
    if (! ptr) 
        return (ERROR);
    ptr += 4 ;	/* Move past "data".*/
    
    memcpy (&databytes, ptr, sizeof (UINT32)) ;
    
    /* Header information available, pass to caller */
    *pChannels   = waveformat.channels ;
    *pSampleRate = waveformat.samplesPerSec ;
    *pSampleBits = waveformat.bitsPerSample ;
    *pSamples    = databytes/waveformat.blockAlign ;
    *pDataStart  = ((UINT32) (ptr + 4)) - ((UINT32) (&(buffer[0]))) ;
    
    /* validate that the collected data is reasonable */
    if (waveformat.samplesPerSec !=
            waveformat.avgBytesPerSec / waveformat.blockAlign) 
        return (ERROR);
    
    if (waveformat.samplesPerSec !=
            waveformat.avgBytesPerSec/waveformat.channels/
            ((waveformat.bitsPerSample == 16) ? 2 : 1)) 
        return (ERROR);
    
    return  (OK);
    }


/*****************************************************************************
*
* wavHeaderRead - read a WAVE form header from file 
*
* This routine reads the WAVE form file <fd> to retrieve its
* header information.  The number of channels, sample rate, number of bits
* per sample, number of samples, and the starting location of the data 
* are returned as parameters.
*
* RETURNS: OK when header successfully read; otherwise ERROR
*
* ERRNO: N/A
*
* NOTES:
*   This function is not reentrant
*
* SEE ALSO: 
*
*/

STATUS wavHeaderWrite
    (
    int fd,               /* file descriptor of wave form file */
    int NumChannels,         /* number of channels */
    UINT32 SampleRate,    /* sample rate */
    int SampleBits,       /* size of sample */
    UINT32 Samples        /* number of samples */
    )
    {
    static WAVEFORMAT  waveformat ;
    unsigned int ChunkSize = 4;
    unsigned int SubChunk1Size = 16;
    unsigned int SubChunk2Size = 0;
    unsigned int datasize = Samples * NumChannels * (SampleBits / 8);
    char SubChunk1ID[4] = {'f','m','t',' '};
    char SubChunk2ID[4] = {'d','a','t','a'};
    char ChunkID[4] = {'R','I','F','F'};
    char Format[4] = {'W','A','V','E'};

    /* Position to start of file */
    if (lseek (fd, 0L, SEEK_SET)) 
        return  (ERROR);
    
    SubChunk2Size += datasize;
    ChunkSize += SubChunk2Size + SubChunk1Size + 8 + 8;

    waveformat.channels = NumChannels;
    waveformat.samplesPerSec = SampleRate;
    waveformat.bitsPerSample = SampleBits;
    waveformat.avgBytesPerSec = SampleRate * NumChannels * (SampleBits/8);
    waveformat.blockAlign = NumChannels * (SampleBits / 8);
    waveformat.formatTag = WAVE_FORMAT_PCM;
    
    write (fd, (char*)&ChunkID, sizeof(ChunkID));
    write (fd, (char*)&ChunkSize, sizeof(ChunkSize));
    write (fd, (char*)&Format, sizeof(Format));

    write (fd, (char*)&SubChunk1ID, sizeof(SubChunk1ID));
    write (fd, (char*)&SubChunk1Size, sizeof(SubChunk1Size));
    write (fd, (char*)&waveformat, sizeof(waveformat));

    write (fd, (char*)&SubChunk2ID, sizeof(SubChunk2ID));
    write (fd, (char*)&SubChunk2Size, sizeof(SubChunk2Size));
    write (fd, (char*)&datasize, sizeof(datasize));
    
    return  (OK);
    }

