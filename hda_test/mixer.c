#include <vxWorks.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <ioLib.h>
#include <unistd.h>
#include <string.h>
#include <selectLib.h>
#include <drv/sound/soundcard.h>

#define AUDIO_DEVICE "/dev/dsp0"
#define MIXER_DEVICE "/dev/mixer0"

static GETOPT opt_struct;

static char * snd_dev_names[SOUND_MIXER_NRDEVICES] = SOUND_DEVICE_NAMES;
static char * snd_dev_labels[SOUND_MIXER_NRDEVICES] = SOUND_DEVICE_LABELS;

LOCAL void soundMixSet (int md, int dev, int left, int right);
LOCAL void soundMixGet (int md, int dev, int *left, int *right);

IMPORT int ffsLsb(UINT32);

LOCAL UINT32 recmask;
LOCAL UINT32 devmask;
LOCAL UINT32 recsrc;

enum verb {set, get};
enum command {input, output, volume};

LOCAL int Command = -1;
LOCAL int Verb = -1;
LOCAL int Parameter = 0;
LOCAL int lVol = -1;
LOCAL int rVol = -1;

struct mixer_control
    {
    int         index;
    BOOL        cap_capable;
    BOOL        cap_selected;
    BOOL        play;
    char        name[32];
    };

static struct mixer_control control_table[32];

LOCAL void MixerReset()
    {
    Verb = Command = lVol = rVol = -1;
    Parameter = 0;
    }


LOCAL  UINT32 ioctl_handler
    (
    int fd,
    UINT32 command,
    UINT32 arg,
    STATUS *status
    )
    {
    UINT32 ioctl_bits = arg;

    *status = ioctl (fd, command, &ioctl_bits);

    if (*status != ERROR)
        return ioctl_bits;
    else
        return 0;
    }


LOCAL int setargs(char * cmd, char *argv[])
    {
    int cnt = 1;
    const char space = ' ';
    BOOL findspace = FALSE;

    argv[0] = '\0';
    
    while (cmd && (*cmd))
        {
        if (!findspace && (*cmd != space))
            {
            argv[cnt] = cmd;
            findspace = TRUE;
            cnt++;
            }

        if (*cmd == space)
            {
            *cmd = '\0';
            findspace = FALSE;
            }

        cmd++;
        }

    return cnt;
    }

STATUS mixer (char *cmd)
    {
    STATUS flag;
    int fd, i;
    char *argv[10];
    int argc;
    char c;

    argc = setargs(cmd, argv);
/*
    for (i = 0; i < argc; i++)
        printf("%s\n", argv[i]);
*/
    getoptInit(&opt_struct);

    while ((c = getopt_r (argc, argv, "c:", &opt_struct)) != -1)
        {
        switch (c)
            {
            default:
                break;
            }
        }

    for (i = opt_struct.optind; i < argc; i++)
        {
        int j;
        

        if (Verb == -1)
            {
            if (strcmp ("set", argv[i]) == 0)
                Verb = set;
            else if (strcmp ("get", argv[i]) == 0)
                Verb = get;
            continue;
            }

        if (Command == -1)
            {
            if (strcmp ("input", argv[i]) == 0)
                Command = input;
            else if (strcmp ("output", argv[i]) == 0)
                Command = output;
            else if (strcmp ("volume", argv[i]) == 0)
                Command = volume;

            continue;
            }

        for (j = 0; j < SOUND_MIXER_NRDEVICES; j++)
            {
            if (strcmp(snd_dev_names[j], argv[i]) == 0)
                {
                Parameter |= (1 << j);
                i++;
                break;
                }

            }
        
        
        if ((Verb == set) && (Command == volume))
            {
            if (lVol == -1)
                {
                lVol = atoi(argv[i]);
                continue;
                }
            
            if (rVol == -1)
                {
                rVol = atoi(argv[i]);
                continue;
                }
            }
        }

    /* clean up arguments */
    if (Command == volume)
        {
        if ((lVol != -1) && (rVol == -1))
            rVol = lVol;
        }

    fd = open (MIXER_DEVICE, O_RDWR, 0666);
    
    devmask = ioctl_handler (fd, SOUND_MIXER_READ_DEVMASK, 0, &flag);
    recmask = ioctl_handler (fd, SOUND_MIXER_READ_RECMASK, 0, &flag);
    recsrc  = ioctl_handler (fd, SOUND_MIXER_READ_RECSRC, 0, &flag);

    if ((Verb == set) && (Command == input))
        {
        ioctl_handler (fd, SOUND_MIXER_WRITE_RECSRC, Parameter, &flag);
        }
    else if ((Verb == get))
        {
        soundMixGet(fd, ffsLsb(Parameter) - 1, &lVol, &rVol);
        printf ("%s left %d right %d\n", snd_dev_labels[ffsLsb(Parameter)-1], lVol, rVol);
        }
    else if ((Verb == set) && (Command == volume))
        {
        soundMixSet(fd, ffsLsb(Parameter) - 1, lVol, rVol);
        }
    else
        {

        for (i = 0; i < 32; i++)
            {
            int pin = ffsLsb(devmask);

            if (pin == (i + 1))
                {
                control_table[i].index = pin;
                snprintf(control_table[i].name,
                         sizeof(control_table[i].name), "%s", 
                         snd_dev_labels[pin - 1]);

                devmask = devmask & ~(1 << (pin - 1));
                }
            }

        for (i = 0; i < 32; i++)
            {
            control_table[i].cap_capable = FALSE;
            control_table[i].cap_selected = FALSE;

            if (recmask & 1)
                control_table[i].cap_capable = TRUE;
            recmask = recmask >> 1;

            if (recsrc & 1)
                control_table[i].cap_selected = TRUE;
            recsrc = recsrc >> 1;
            }

        for (i = 0; i < 32; i++)
            {
            if (control_table[i].index > 0)
                {
                int left = 0;
                int right = 0;

                /* remote name of control */
                printf("Simple mixer control '%s',0\n",
                       control_table[i].name);

                /* report volume settings */
                soundMixGet (fd, control_table[i].index - 1, &left, &right);
                printf ("  values=%d,%d\n", left, right);

                /* report features */
                printf("  Capabilities: ");
                if (control_table[i].cap_capable)
                    printf ("cenum");

                printf("\n");
                }
            }

        printf ("Capture Sources Selected:");
        for (i = 0; i < 32; i++)
            {
            if (control_table[i].index > 0)
                {
                if (control_table[i].cap_selected)
                    printf(" '%s'", control_table[i].name);
                }
            }
        printf("\n");
        }
    
    MixerReset();

    close (fd);
    return OK;
    }


LOCAL void soundMixSet (int md, int dev, int left, int right)
    {
    int level = 0;

    level = left << 8 | right;
    
    ioctl (md, MIXER_WRITE(dev), (int)&level);
    
    }

LOCAL void soundMixGet (int md, int dev, int *left, int *right)
    {
    int level = 0;

    ioctl (md, MIXER_READ(dev), (int)&level);

    *left = (level >> 8) & 0xff;
    *right =  (level & 0xff);
    }
