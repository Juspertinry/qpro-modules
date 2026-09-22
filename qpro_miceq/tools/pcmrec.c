// pcmrec: raw ALSA capture to WAV, no tinyalsa needed.
// usage: pcmrec <card> <device> <channels> <rate> <seconds> <out.wav>
// Always captures S16_LE; the ADM converts from the TDM backend format.
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

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv)
{
	if (argc != 7) {
		fprintf(stderr, "usage: %s card dev ch rate secs out.wav\n", argv[0]);
		return 1;
	}
	int card = atoi(argv[1]), dev = atoi(argv[2]), ch = atoi(argv[3]);
	unsigned rate = atoi(argv[4]), secs = atoi(argv[5]);
	char path[64];
	snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%dc", card, dev);
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
	sp.start_threshold = 1;
	sp.stop_threshold = period * periods;
	sp.boundary = period * periods;
	while (sp.boundary * 2 <= 0x7fffffff - period * periods)
		sp.boundary *= 2;
	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp)) { perror("sw_params"); return 1; }
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE)) { perror("prepare"); return 1; }

	FILE *out = fopen(argv[6], "wb");
	if (!out) { perror(argv[6]); return 1; }
	uint32_t total = rate * secs, bytes = total * ch * 2;
	fwrite("RIFF", 1, 4, out); put32(out, 36 + bytes);
	fwrite("WAVEfmt ", 1, 8, out); put32(out, 16); put16(out, 1); put16(out, ch);
	put32(out, rate); put32(out, rate * ch * 2); put16(out, ch * 2); put16(out, 16);
	fwrite("data", 1, 4, out); put32(out, bytes);

	int16_t *buf = malloc(period * ch * 2);
	uint32_t got = 0;
	while (got < total) {
		struct snd_xferi x = { .buf = buf, .frames = period };
		if (ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x)) {
			perror("read");
			ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
			continue;
		}
		fwrite(buf, ch * 2, x.result, out);
		got += x.result;
	}
	fclose(out);
	close(fd);
	return 0;
}
