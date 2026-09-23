// qpro_miceq: wrapper around the stock audio HAL that replaces mic capture
// data with our own chain fed from the codec's direct mic slots.
//
// Loaded by android.hardware.audio.service in place of audio.primary.kona.so.
// The stock library is dlopen'd from ORIG_PATH and everything passes through
// except open/close_input_stream and the stream read. A tap thread captures
// all six TDM TX slots on a spare front end, runs the chain, and the hooked
// reads hand that mono result to Android instead of the HAL's data.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>
#include <sound/asound.h>

#include "chain.h"
#include "hal_abi.h"

#define TAG "miceq"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// stock HAL, bind-mounted by service.sh over the unused default stub
#define ORIG_PATH "/vendor/lib/hw/audio.primary.default.so"
#define CONF_PATH "/data/local/tmp/qpro_miceq/miceq.conf"
#define DUMP_PATH "/data/local/tmp/qpro_miceq/dump.raw"

// Our tap: MultiMedia10 front end, all TDM TX slots.
#define TAP_PCM "/dev/snd/pcmC0D12c"
// asked-for period; the q6 driver may pick another, and every read must be
// exactly one period or its buffer bookkeeping breaks (EFAULT)
#define TAP_PERIOD CHAIN_FRAME
#define TAP_PERIODS 8   // the driver's maximum; an overrun corrupts its buffer offsets
static unsigned g_tap_period;   // what the driver actually granted
#define RING_FRAMES (CHAIN_RATE / 2)
#define TAP_GRACE_US 5000000

#define CTL_DEV "/dev/snd/controlC0"
#define CODEC_NODE "/sys/devices/platform/soc/a80000.i2c/i2c-0/0-002d/cm7120codec"
#define TDM_CTRL1_REG 0x0038
#define TDM_CTRL1_6SLOT 0x8ef0

// ---- config -----------------------------------------------------------------

struct conf {
	int enabled;
	int dump;       // 1: what Android receives, 2: the raw 6-slot tap
	int debug;      // log stage levels once a second
	struct chain_conf chain;
};

static struct conf g_conf;
static struct timespec g_conf_mtime;
static off_t g_conf_size;
static pthread_mutex_t g_conf_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_conf_gen;

static void conf_defaults(struct conf *c)
{
	memset(c, 0, sizeof(*c));
	c->enabled = 1;
	c->chain.mic_w[0] = 1;
	c->chain.aec_tail_ms = 64;
	c->chain.aec_delay_ms = 40;
	c->chain.ns_mix = 1;
	c->chain.gate_range_db = 20;
	c->chain.comp_ratio = 2;
	c->chain.comp_attack_ms = 5;
	c->chain.comp_release_ms = 80;
}

static int split(char *s, float *v, int max)
{
	int n = 0;
	for (char *t = strtok(s, ", "); t && n < max; t = strtok(NULL, ", "))
		v[n++] = strtof(t, NULL);
	return n;
}

static void conf_apply(struct conf *c, const char *key, char *val)
{
	float v[8];
	struct chain_conf *k = &c->chain;
	if (!strcmp(key, "enabled")) c->enabled = atoi(val);
	else if (!strcmp(key, "dump")) c->dump = atoi(val);
	else if (!strcmp(key, "debug")) c->debug = atoi(val);
	else if (!strcmp(key, "gain_db")) k->gain_db = strtof(val, NULL);
	else if (!strcmp(key, "source")) k->source_dsp = !strcmp(val, "dsp");
	else if (!strcmp(key, "mics")) {
		// list of mic numbers, equal weights: "1,2,3" or "1"
		int n = split(val, v, 3);
		memset(k->mic_w, 0, sizeof(k->mic_w));
		for (int i = 0; i < n; i++)
			if (v[i] >= 1 && v[i] <= 3)
				k->mic_w[(int)v[i] - 1] = 1.0f / n;
	} else if (!strcmp(key, "mic_weights")) {
		int n = split(val, v, 3);
		for (int i = 0; i < n; i++)
			k->mic_w[i] = v[i];
	} else if (!strcmp(key, "aec")) k->aec = atoi(val);
	else if (!strcmp(key, "aec_tail_ms")) k->aec_tail_ms = atoi(val);
	else if (!strcmp(key, "aec_delay_ms")) k->aec_delay_ms = atoi(val);
	else if (!strcmp(key, "ns")) k->ns = atoi(val);
	else if (!strcmp(key, "ns_mix")) k->ns_mix = strtof(val, NULL);
	else if (!strncmp(key, "eq", 2) && key[2] >= '1' && key[2] <= '8' && !key[3]) {
		// eqN=type,freq,gain_db,q   (type: pk ls hs hp lp notch off)
		struct eq_band *b = &k->eq[key[2] - '1'];
		char *t = strtok(val, ", ");
		memset(b, 0, sizeof(*b));
		if (!t) return;
		if (!strcmp(t, "pk")) b->type = EQ_PK;
		else if (!strcmp(t, "ls")) b->type = EQ_LS;
		else if (!strcmp(t, "hs")) b->type = EQ_HS;
		else if (!strcmp(t, "hp")) b->type = EQ_HP;
		else if (!strcmp(t, "lp")) b->type = EQ_LP;
		else if (!strcmp(t, "notch")) b->type = EQ_NOTCH;
		else b->type = EQ_OFF;
		int n = split(NULL, v, 3);
		b->freq = n > 0 ? v[0] : 0;
		b->gain_db = n > 1 ? v[1] : 0;
		b->q = n > 2 ? v[2] : 0.707f;
	} else if (!strcmp(key, "gate_db")) k->gate_db = strtof(val, NULL);
	else if (!strcmp(key, "gate_range_db")) k->gate_range_db = strtof(val, NULL);
	else if (!strcmp(key, "comp")) {
		// comp=threshold_db,ratio,attack_ms,release_ms,makeup_db
		int n = split(val, v, 5);
		k->comp_thr_db = n > 0 ? v[0] : 0;
		k->comp_ratio = n > 1 ? v[1] : 2;
		k->comp_attack_ms = n > 2 ? v[2] : 5;
		k->comp_release_ms = n > 3 ? v[3] : 80;
		k->comp_makeup_db = n > 4 ? v[4] : 0;
	} else if (!strcmp(key, "limit_db")) k->limit_db = strtof(val, NULL);
}

static void conf_reload(void)
{
	struct stat st;
	// size check: the shell truncates before it writes, so an empty file is
	// a write in progress, not a new config
	if (stat(CONF_PATH, &st) || st.st_size == 0 ||
	    (st.st_mtim.tv_sec == g_conf_mtime.tv_sec &&
	     st.st_mtim.tv_nsec == g_conf_mtime.tv_nsec && st.st_size == g_conf_size))
		return;
	g_conf_mtime = st.st_mtim;
	g_conf_size = st.st_size;
	FILE *f = fopen(CONF_PATH, "r");
	if (!f)
		return;
	struct conf c;
	conf_defaults(&c);
	char line[160];
	while (fgets(line, sizeof(line), f)) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = 0;
		if (line[0] == '#' || !line[0])
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		conf_apply(&c, line, eq + 1);
	}
	fclose(f);
	if (!c.enabled) {
		// bypass = the DSP's own processed output, untouched, which is stock
		struct conf d;
		conf_defaults(&d);
		d.enabled = 0;
		d.dump = c.dump;
		d.debug = c.debug;
		d.chain.source_dsp = 1;
		c = d;
	}
	pthread_mutex_lock(&g_conf_lock);
	g_conf = c;
	g_conf_gen++;
	pthread_mutex_unlock(&g_conf_lock);
	int neq = 0;
	for (int i = 0; i < CHAIN_MAX_EQ; i++)
		neq += c.chain.eq[i].type != EQ_OFF;
	LOGI("config: enabled=%d src=%s mics=%.2f/%.2f/%.2f aec=%d ns=%d(%.2f) eq=%d gate=%.0f comp=%.0f gain=%.1f dump=%d",
	     c.enabled, c.chain.source_dsp ? "dsp" : "raw", c.chain.mic_w[0], c.chain.mic_w[1],
	     c.chain.mic_w[2], c.chain.aec, c.chain.ns, c.chain.ns_mix, neq,
	     c.chain.gate_db, c.chain.comp_thr_db, c.chain.gain_db, c.dump);
}

// ---- mixer and codec setup ----------------------------------------------------

// Set an ALSA mixer control by name. Enum values are matched by item name.
static int ctl_set(const char *name, const char *value)
{
	int fd = open(CTL_DEV, O_RDWR);
	if (fd < 0)
		return -1;
	struct snd_ctl_elem_list list;
	memset(&list, 0, sizeof(list));
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list)) {
		close(fd);
		return -1;
	}
	struct snd_ctl_elem_id *ids = calloc(list.count, sizeof(*ids));
	list.space = list.count;
	list.pids = ids;
	int ret = -1;
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0) {
		for (unsigned i = 0; i < list.used; i++) {
			if (strcmp((char *)ids[i].name, name))
				continue;
			struct snd_ctl_elem_info info;
			memset(&info, 0, sizeof(info));
			info.id = ids[i];
			if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info))
				break;
			struct snd_ctl_elem_value ev;
			memset(&ev, 0, sizeof(ev));
			ev.id = ids[i];
			if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
				int item = -1;
				for (unsigned k = 0; k < info.value.enumerated.items; k++) {
					info.value.enumerated.item = k;
					if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info))
						break;
					if (!strcmp((char *)info.value.enumerated.name, value)) {
						item = k;
						break;
					}
				}
				if (item < 0)
					break;
				for (unsigned k = 0; k < info.count; k++)
					ev.value.enumerated.item[k] = item;
			} else {
				for (unsigned k = 0; k < info.count; k++)
					ev.value.integer.value[k] = atoi(value);
			}
			ret = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev);
			break;
		}
	}
	free(ids);
	close(fd);
	if (ret)
		LOGE("mixer set '%s'='%s' failed", name, value);
	return ret;
}

// Codec TDM TX slot count. The stock firmware table sets 2 slots on every
// codec power-up, so this is re-checked periodically.
static void codec_ensure_6slot(void)
{
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "0x%04x", TDM_CTRL1_REG);
	int fd = open(CODEC_NODE, O_RDWR);
	if (fd < 0)
		return;
	write(fd, buf, n);
	lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = 0;
	unsigned cur = strtoul(buf, NULL, 16);
	if (cur != TDM_CTRL1_6SLOT) {
		n = snprintf(buf, sizeof(buf), "0x%04x 0x%04x", TDM_CTRL1_REG, TDM_CTRL1_6SLOT);
		write(fd, buf, n);
		LOGI("codec TDM ctrl1 0x%04x -> 0x%04x", cur, TDM_CTRL1_6SLOT);
	}
	close(fd);
}

// What the DSP puts on slots 0/1: its processed mic (selectors 0) or the
// speaker echo reference (3/4) that the AEC needs.
static void dsp_select_slots(int want_ref)
{
	static int cur = -1;
	if (cur == want_ref)
		return;
	cur = want_ref;
	ctl_set("MIC TO CHANNEL0", want_ref ? "3" : "0");
	ctl_set("MIC TO CHANNEL1", want_ref ? "4" : "0");
}

// ---- debug dump --------------------------------------------------------------

static int g_dump_fd = -1;

static void dump_write(const void *buf, size_t n)
{
	if (!g_conf.dump) {
		if (g_dump_fd >= 0) {
			close(g_dump_fd);
			g_dump_fd = -1;
		}
		return;
	}
	if (g_dump_fd < 0)
		g_dump_fd = open(DUMP_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (g_dump_fd >= 0)
		write(g_dump_fd, buf, n);
}

// ---- tap capture + processing thread ---------------------------------------

static pthread_t g_tap_thread;
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t g_ring[RING_FRAMES];   // processed mono
static uint64_t g_ring_wr;           // total frames written
static volatile int g_tap_run;
static volatile int g_tap_alive;
static int g_tap_users;
static struct timespec g_tap_last_close;

static void mask_set(struct snd_pcm_hw_params *p, int param, unsigned int bit)
{
	struct snd_mask *m = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
	memset(m, 0, sizeof(*m));
	m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void interval_set(struct snd_pcm_hw_params *p, int param, unsigned int v)
{
	struct snd_interval *i = &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
	i->min = i->max = v;
	i->integer = 1;
}

static int tap_open(void)
{
	ctl_set("MultiMedia10 Mixer PRI_TDM_TX_0", "1");
	int fd = open(TAP_PCM, O_RDWR);
	if (fd < 0) {
		LOGE("tap open %s: %s", TAP_PCM, strerror(errno));
		return -1;
	}
	struct snd_pcm_hw_params hp;
	memset(&hp, 0, sizeof(hp));
	for (int n = 0; n <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; n++)
		memset(&hp.masks[n], 0xff, sizeof(hp.masks[n]));
	for (int n = 0; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n++) {
		hp.intervals[n].min = 0;
		hp.intervals[n].max = ~0u;
	}
	hp.rmask = ~0u;
	mask_set(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	mask_set(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	mask_set(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_FRAME_BITS, 16 * CHAIN_TAP_CH);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, CHAIN_TAP_CH);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_RATE, CHAIN_RATE);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, TAP_PERIOD);
	interval_set(&hp, SNDRV_PCM_HW_PARAM_PERIODS, TAP_PERIODS);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp)) {
		LOGE("tap hw_params: %s", strerror(errno));
		close(fd);
		return -1;
	}
	g_tap_period = hp.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
	if (g_tap_period == 0 || g_tap_period > CHAIN_FRAME * 4) {
		LOGE("tap: unusable period %u", g_tap_period);
		close(fd);
		return -1;
	}
	if (g_tap_period != TAP_PERIOD)
		LOGI("tap: driver period %u frames", g_tap_period);
	struct snd_pcm_sw_params sp;
	memset(&sp, 0, sizeof(sp));
	sp.period_step = 1;
	sp.avail_min = TAP_PERIOD;
	sp.start_threshold = 1;
	sp.stop_threshold = TAP_PERIOD * TAP_PERIODS;
	sp.boundary = TAP_PERIOD * TAP_PERIODS;
	while (sp.boundary * 2 <= 0x7fffffff - TAP_PERIOD * TAP_PERIODS)
		sp.boundary *= 2;
	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) || ioctl(fd, SNDRV_PCM_IOCTL_PREPARE)) {
		LOGE("tap sw_params/prepare: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static void *tap_main(void *arg)
{
	(void)arg;
	{
		// sm8250: cpus 4-7 are the gold/prime cores
		cpu_set_t set;
		CPU_ZERO(&set);
		for (int i = 4; i < 8; i++)
			CPU_SET(i, &set);
		if (sched_setaffinity(0, sizeof(set), &set))
			LOGI("big-core affinity not applied: %s", strerror(errno));
	}
	// FIFO of tap frames: one driver period in, one chain frame out
	static int16_t fifo[CHAIN_FRAME * 6 * CHAIN_TAP_CH];
	int16_t buf[CHAIN_FRAME * CHAIN_TAP_CH];
	int16_t mono[CHAIN_FRAME];
	unsigned have = 0;   // frames waiting in the FIFO
	int fails = 0;  // consecutive read errors
	int fd = -1;
	struct chain *chain = chain_create();
	struct chain_conf cur;
	memset(&cur, 0, sizeof(cur));
	int gen = -1;
	int since_check = 0;
	while (g_tap_run) {
		if (gen != g_conf_gen) {
			pthread_mutex_lock(&g_conf_lock);
			struct chain_conf k = g_conf.chain;
			gen = g_conf_gen;
			pthread_mutex_unlock(&g_conf_lock);
			// AEC/NS setup allocates and initializes models, which is too
			// slow for the capture loop: drop the PCM around it. Everything
			// else (EQ, gain, dynamics) is cheap and applies in place.
			int heavy = k.aec != cur.aec || k.ns != cur.ns || k.source_dsp != cur.source_dsp ||
				    k.aec_tail_ms != cur.aec_tail_ms || k.aec_delay_ms != cur.aec_delay_ms;
			if (memcmp(&k, &cur, sizeof(k))) {
				if (heavy && fd >= 0) {
					close(fd);
					fd = -1;
					have = 0;
				}
				chain_configure(chain, &k);
				dsp_select_slots(k.aec && !k.source_dsp);
				cur = k;
			}
		}
		if (fd < 0) {
			fd = tap_open();
			if (fd < 0) {
				usleep(200000);
				continue;
			}
			LOGI("tap running");
		}
		if (have + g_tap_period > CHAIN_FRAME * 6)
			have = 0;   // cannot happen with a sane period, but never overrun
		struct snd_xferi x = { .buf = fifo + have * CHAIN_TAP_CH, .frames = g_tap_period };
		if (ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x)) {
			if (errno == EPIPE) {
				ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
				continue;
			}
			// EFAULT here is the q6 driver's "empty DSP buffer": the shared
			// backend is being restarted by the HAL (audioserver opens and
			// closes mic streams in bursts). Data resumes on its own, so
			// wait it out; only a stream that stays dead gets reopened.
			if (++fails < 6000) {   // 30 s; audioserver can take that long to bring streams up
				if (fails == 1)
					LOGI("tap: waiting for backend");
				usleep(5000);
				continue;
			}
			LOGE("tap read: %s, reopening", strerror(errno));
			close(fd);
			fd = -1;
			fails = 0;
			usleep(200000);
			continue;
		}
		if (fails) {
			LOGI("tap read ok after %d retries", fails);
			fails = 0;
		}
		have += (unsigned)x.result;
		if (have < CHAIN_FRAME)
			continue;
		memcpy(buf, fifo, sizeof(buf));
		have -= CHAIN_FRAME;
		memmove(fifo, fifo + CHAIN_FRAME * CHAIN_TAP_CH, have * CHAIN_TAP_CH * sizeof(int16_t));

		if (++since_check >= 100) {   // once a second
			since_check = 0;
			if (g_tap_users == 0) {
				// debug run with no streams: nobody else polls the config
				conf_reload();
				if (!g_conf.debug)
					break;
			}
		}
		chain_process(chain, buf, mono);
		if (g_conf.dump == 2)
			dump_write(buf, sizeof(buf));
		if (g_conf.debug && since_check % 100 == 50) {
			struct chain_levels l;
			chain_levels(chain, &l);
			LOGI("levels dBFS: sum %.1f ref %.1f aec %.1f ns %.1f eq %.1f out %.1f",
			     l.sum, l.ref, l.aec, l.ns, l.eq, l.out);
		}

		pthread_mutex_lock(&g_ring_lock);
		for (int i = 0; i < CHAIN_FRAME; i++)
			g_ring[(g_ring_wr + i) % RING_FRAMES] = mono[i];
		g_ring_wr += CHAIN_FRAME;
		pthread_mutex_unlock(&g_ring_lock);
	}
	if (fd >= 0)
		close(fd);
	chain_destroy(chain);
	g_tap_alive = 0;
	LOGI("tap stopped");
	return NULL;
}

static void tap_launch(void)
{
	if (g_tap_run && g_tap_alive)
		return;
	if (g_tap_run)
		pthread_join(g_tap_thread, NULL);
	g_tap_run = 1;
	g_tap_alive = 1;
	pthread_create(&g_tap_thread, NULL, tap_main, NULL);
}

static void tap_start(void)
{
	g_tap_users++;
	tap_launch();
}

// Streams come and go in bursts (audioserver probes several at open), so the
// thread lingers a few seconds after the last one closes.
static void tap_stop(void)
{
	if (--g_tap_users > 0)
		return;
	clock_gettime(CLOCK_MONOTONIC, &g_tap_last_close);
}

static void tap_idle_check(void)
{
	if (g_tap_users > 0 || !g_tap_run || g_conf.debug)
		return;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long us = (now.tv_sec - g_tap_last_close.tv_sec) * 1000000L +
		  (now.tv_nsec - g_tap_last_close.tv_nsec) / 1000;
	if (us > TAP_GRACE_US) {
		g_tap_run = 0;
		pthread_join(g_tap_thread, NULL);
	}
}

// ---- per-stream state ---------------------------------------------------------

#define MAX_STREAMS 8

struct stream_ctx {
	struct audio_stream_in *stream;
	ssize_t (*orig_read)(struct audio_stream_in *, void *, size_t);
	unsigned channels;
	uint64_t ring_rd;
};

static struct stream_ctx g_streams[MAX_STREAMS];
static pthread_mutex_t g_streams_lock = PTHREAD_MUTEX_INITIALIZER;

static struct stream_ctx *ctx_find(struct audio_stream_in *s)
{
	for (int i = 0; i < MAX_STREAMS; i++)
		if (g_streams[i].stream == s)
			return &g_streams[i];
	return NULL;
}

static ssize_t wrapped_read(struct audio_stream_in *stream, void *buffer, size_t bytes)
{
	struct stream_ctx *c = ctx_find(stream);
	if (!c)
		return -ENODEV;
	// The stock read paces us and keeps the HAL's stream state sane.
	ssize_t got = c->orig_read(stream, buffer, bytes);
	if (got <= 0)
		return got;
	conf_reload();
	{
		// the stock firmware table resets the codec to 2 TX slots on every
		// power-up, so re-check about once a second from here, off the
		// capture thread (it is an I2C transaction through the DSP)
		static uint64_t frames_since_check;
		frames_since_check += (size_t)got / (2 * c->channels);
		if (frames_since_check >= CHAIN_RATE) {
			frames_since_check = 0;
			codec_ensure_6slot();
		}
	}

	unsigned ch = c->channels;
	size_t frames = (size_t)got / (2 * ch);
	int16_t *out = buffer;

	pthread_mutex_lock(&g_ring_lock);
	// Follow the writer with a small fixed lag; resync if we fall out of range.
	if (c->ring_rd + frames > g_ring_wr || g_ring_wr - c->ring_rd > RING_FRAMES / 2)
		c->ring_rd = g_ring_wr > frames + CHAIN_FRAME ? g_ring_wr - frames - CHAIN_FRAME : 0;
	for (size_t i = 0; i < frames; i++) {
		int16_t s = g_ring[(c->ring_rd + i) % RING_FRAMES];
		for (unsigned k = 0; k < ch; k++)
			out[i * ch + k] = s;
	}
	c->ring_rd += frames;
	pthread_mutex_unlock(&g_ring_lock);
	if (g_conf.dump == 1)
		dump_write(buffer, got);
	return got;
}

// ---- device hooks ----------------------------------------------------------------

static int (*orig_open_input_stream)(struct audio_hw_device *, audio_io_handle_t,
	audio_devices_t, struct audio_config *, struct audio_stream_in **,
	audio_input_flags_t, const char *, audio_source_t);
static void (*orig_close_input_stream)(struct audio_hw_device *, struct audio_stream_in *);

static int wrapped_open_input_stream(struct audio_hw_device *dev, audio_io_handle_t handle,
	audio_devices_t devices, struct audio_config *config, struct audio_stream_in **stream_in,
	audio_input_flags_t flags, const char *address, audio_source_t source)
{
	conf_reload();
	ctl_set("PRI_TDM_TX_0 Channels", "Six");
	codec_ensure_6slot();
	int ret = orig_open_input_stream(dev, handle, devices, config, stream_in, flags, address, source);
	if (ret || !*stream_in)
		return ret;
	struct audio_stream_in *s = *stream_in;
	unsigned ch = __builtin_popcount(s->common.get_channels(&s->common));
	if (s->common.get_format(&s->common) != AUDIO_FORMAT_PCM_16_BIT || ch == 0 || ch > 2 ||
	    s->common.get_sample_rate(&s->common) != CHAIN_RATE) {
		LOGI("input stream handle %d: format not handled, passing through", handle);
		return ret;
	}
	pthread_mutex_lock(&g_streams_lock);
	tap_idle_check();
	struct stream_ctx *c = ctx_find(NULL);
	if (c) {
		c->stream = s;
		c->orig_read = s->read;
		c->channels = ch;
		c->ring_rd = 0;
		s->read = wrapped_read;
		tap_start();
		LOGI("hooked input stream handle %d source %d ch %u", handle, source, ch);
	}
	pthread_mutex_unlock(&g_streams_lock);
	return ret;
}

static void wrapped_close_input_stream(struct audio_hw_device *dev, struct audio_stream_in *s)
{
	pthread_mutex_lock(&g_streams_lock);
	struct stream_ctx *c = ctx_find(s);
	if (c) {
		s->read = c->orig_read;
		memset(c, 0, sizeof(*c));
		tap_stop();
	}
	pthread_mutex_unlock(&g_streams_lock);
	orig_close_input_stream(dev, s);
}

// ---- module glue ------------------------------------------------------------------

static struct hw_module_t *g_orig_module;

static int wrapped_open(const struct hw_module_t *module, const char *id, struct hw_device_t **device)
{
	(void)module;
	int ret = g_orig_module->methods->open(g_orig_module, id, device);
	if (ret || !*device)
		return ret;
	struct audio_hw_device *adev = (struct audio_hw_device *)*device;
	if (!orig_open_input_stream) {
		orig_open_input_stream = adev->open_input_stream;
		orig_close_input_stream = adev->close_input_stream;
	}
	adev->open_input_stream = wrapped_open_input_stream;
	adev->close_input_stream = wrapped_close_input_stream;
	LOGI("stock HAL opened, input hooks installed (api 0x%x)", adev->common.version);
	if (g_conf.debug) {
		ctl_set("PRI_TDM_TX_0 Channels", "Six");
		codec_ensure_6slot();
		pthread_mutex_lock(&g_streams_lock);
		tap_launch();
		pthread_mutex_unlock(&g_streams_lock);
	}
	return 0;
}

static struct hw_module_methods_t g_methods = { .open = wrapped_open };

struct audio_module HAL_MODULE_INFO_SYM;

__attribute__((constructor)) static void miceq_init(void)
{
	conf_defaults(&g_conf);
	void *h = dlopen(ORIG_PATH, RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		LOGE("dlopen %s: %s", ORIG_PATH, dlerror());
		return;
	}
	g_orig_module = dlsym(h, HAL_MODULE_INFO_SYM_AS_STR);
	if (!g_orig_module) {
		LOGE("no HMI in stock HAL");
		return;
	}
	HAL_MODULE_INFO_SYM.common = *g_orig_module;
	HAL_MODULE_INFO_SYM.common.methods = &g_methods;
	HAL_MODULE_INFO_SYM.common.dso = NULL;
	conf_reload();
	LOGI("wrapper loaded over %s (%s)", g_orig_module->id, g_orig_module->name);
}
