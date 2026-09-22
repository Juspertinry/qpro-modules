// pcmplay: raw ALSA playback of a 16-bit PCM WAV, no tinyalsa needed.
// usage: pcmplay <card> <device> <in.wav>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

static void mask_set(struct snd_pcm_hw_params *p, int param, unsigned int bit)
{
	struct snd_mask *m = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
	memset(m, 0, sizeof(*m));
	m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void interval_set(struct snd_pcm_hw_params *p, int param, unsigned int v)
{
	struct snd_interval *i =
		&p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
	i->min = i->max = v;
	i->integer = 1;
}

static void params_init(struct snd_pcm_hw_params *p)
{
	memset(p, 0, sizeof(*p));
	for (int n = 0; n <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; n++)
		memset(&p->masks[n], 0xff, sizeof(p->masks[n]));
	for (int n = 0; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n++) {
		p->intervals[n].min = 0;
		p->intervals[n].max = ~0u;
	}
	p->rmask = ~0u;
}

int main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: %s card dev in.wav\n", argv[0]);
		return 1;
	}
	FILE *in = fopen(argv[3], "rb");
	if (!in) { perror(argv[3]); return 1; }
	uint8_t hdr[44];
	if (fread(hdr, 1, 44, in) != 44 || memcmp(hdr, "RIFF", 4)) {
		fprintf(stderr, "not a canonical wav\n");
		return 1;
	}
	unsigned ch = hdr[22] | hdr[23] << 8;
	unsigned rate = hdr[24] | hdr[25] << 8 | hdr[26] << 16 | hdr[27] << 24;

	char path[64];
	snprintf(path, sizeof(path), "/dev/snd/pcmC%sD%sp", argv[1], argv[2]);
	int fd = open(path, O_RDWR);
	if (fd < 0) { perror(path); return 1; }

	const unsigned period = 960, periods = 4;
	struct snd_pcm_hw_params hp;
	params_init(&hp);
	mask_set(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	mask_set(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	mask_set(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_FRAME_BITS, 16 * ch);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, ch);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_RATE, rate);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_PERIODS, periods);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp)) { perror("hw_params"); return 1; }

	struct snd_pcm_sw_params sp;
	memset(&sp, 0, sizeof(sp));
	sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	sp.period_step = 1;
	sp.avail_min = period;
	sp.start_threshold = period * 2;
	sp.stop_threshold = period * periods;
	sp.boundary = period * periods;
	while (sp.boundary * 2 <= 0x7fffffff - period * periods)
		sp.boundary *= 2;
	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp)) { perror("sw_params"); return 1; }
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE)) { perror("prepare"); return 1; }

	int16_t *buf = malloc(period * ch * 2);
	size_t n;
	while ((n = fread(buf, ch * 2, period, in)) > 0) {
		struct snd_xferi x = { .buf = buf, .frames = n };
		if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x)) {
			perror("write");
			ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
		}
	}
	ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
	close(fd);
	return 0;
}
