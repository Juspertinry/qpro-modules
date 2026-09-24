// Mic processing chain. Everything runs in float at 48 kHz on 10 ms frames.
// Order: mic sum -> alignment delay -> AEC -> RNNoise -> EQ -> gate ->
// compressor -> gain -> limiter.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <rnnoise.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include "chain.h"

struct biquad {
	float b0, b1, b2, a1, a2;
	float z1, z2;
};

// Delay estimator: mic and reference decimated to 8 kHz, half a second of
// each, cross-correlated over 0..80 ms of mic lead. The reference comes out
// of the codec DSP about 35 ms after the echo reaches the mics, so the mics
// are delayed to match; measuring it keeps the filter's first tap on the
// direct path if the DSP path ever changes (firmware, sample rate, mode).
#define DE_DEC 6
#define DE_WIN (CHAIN_RATE / DE_DEC / 2)
#define DE_MAXLAG (80 * CHAIN_RATE / DE_DEC / 1000)
#define DE_MARGIN (2 * CHAIN_RATE / 1000)
#define DL_N (CHAIN_MAX_DELAY_MS * CHAIN_RATE / 1000 + CHAIN_FRAME)

struct chain {
	struct chain_conf conf;
	int configured;

	// alignment delay line (mono ring, read delay_len behind the write point)
	float dl[DL_N];
	int dl_w, delay_len;

	float de_mic[DE_WIN], de_ref[DE_WIN];
	int de_fill;
	int de_last;           // previous estimate, samples at 48 kHz
	int de_agree;

	SpeexEchoState *aec;
	SpeexPreprocessState *pp;
	DenoiseState *ns;

	struct biquad eq[CHAIN_MAX_EQ];
	int n_eq;

	float gate_env;
	float gate_gain;       // current gate gain, smoothed
	int gate_open;
	int gate_hold;         // samples of hold left
	float comp_env;
	struct chain_levels lv;
};

static float lvl(const float *x, int n, int stride)
{
	double e = 0;
	for (int i = 0; i < n; i++)
		e += (double)x[i * stride] * x[i * stride];
	return 10 * log10f(e / n / (32768.0f * 32768.0f) + 1e-12f);
}

void chain_levels(const struct chain *c, struct chain_levels *l)
{
	*l = c->lv;
}

static float db2lin(float db) { return powf(10.0f, db / 20.0f); }

// RBJ cookbook
static void biquad_design(struct biquad *b, const struct eq_band *e)
{
	float A = powf(10.0f, e->gain_db / 40.0f);
	float w = 2 * (float)M_PI * e->freq / CHAIN_RATE;
	float cw = cosf(w), sw = sinf(w);
	float q = e->q > 0.05f ? e->q : 0.707f;
	float al = sw / (2 * q);
	float b0, b1, b2, a0, a1, a2;
	switch (e->type) {
	case EQ_PK:
		b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A;
		a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A;
		break;
	case EQ_LS: {
		float s = 2 * sqrtf(A) * al;
		b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw);
		b2 = A * ((A + 1) - (A - 1) * cw - s);
		a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw);
		a2 = (A + 1) + (A - 1) * cw - s;
		break;
	}
	case EQ_HS: {
		float s = 2 * sqrtf(A) * al;
		b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw);
		b2 = A * ((A + 1) + (A - 1) * cw - s);
		a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw);
		a2 = (A + 1) - (A - 1) * cw - s;
		break;
	}
	case EQ_HP:
		b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
		a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al;
		break;
	case EQ_LP:
		b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
		a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al;
		break;
	case EQ_NOTCH:
		b0 = 1; b1 = -2 * cw; b2 = 1;
		a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al;
		break;
	default:
		b0 = a0 = 1; b1 = b2 = a1 = a2 = 0;
	}
	b->b0 = b0 / a0; b->b1 = b1 / a0; b->b2 = b2 / a0;
	b->a1 = a1 / a0; b->a2 = a2 / a0;
	b->z1 = b->z2 = 0;
}

static inline float biquad_run(struct biquad *b, float x)
{
	// transposed direct form II
	float y = b->b0 * x + b->z1;
	b->z1 = b->b1 * x - b->a1 * y + b->z2;
	b->z2 = b->b2 * x - b->a2 * y;
	return y;
}

struct chain *chain_create(void)
{
	return calloc(1, sizeof(struct chain));
}

void chain_destroy(struct chain *c)
{
	if (!c)
		return;
	if (c->pp)
		speex_preprocess_state_destroy(c->pp);
	if (c->aec)
		speex_echo_state_destroy(c->aec);
	if (c->ns)
		rnnoise_destroy(c->ns);
	free(c);
}

static void set_delay(struct chain *c, int samples)
{
	if (samples < 0) samples = 0;
	if (samples > CHAIN_MAX_DELAY_MS * CHAIN_RATE / 1000) samples = CHAIN_MAX_DELAY_MS * CHAIN_RATE / 1000;
	c->delay_len = samples;
	c->lv.aec_delay = samples;
}

void chain_reset_aec(struct chain *c)
{
	if (c->aec)
		speex_echo_state_reset(c->aec);
	c->de_fill = 0;
	c->de_last = -1;
	c->lv.aec_delay_meas = -1;
}

// Returns the mic lead over the reference in 48 kHz samples, or -1 when the
// window has no usable correlation (no playback, or too little of it).
static int estimate_delay(struct chain *c)
{
	const float *m = c->de_mic, *r = c->de_ref;
	double em = 0, er = 0;
	for (int i = 0; i < DE_WIN; i++) {
		em += (double)m[i] * m[i];
		er += (double)r[i] * r[i];
	}
	// no playback (reference under -55 dBFS) means nothing to measure; the
	// mic side can be much quieter than that at low volume, so only rule
	// out silence there and let the correlation quality decide
	if (er / DE_WIN < 32768.0 * 32768.0 * 3.2e-6 || em / DE_WIN < 32768.0 * 32768.0 * 1e-8)
		return -1;
	int best = -1;
	double bestc = 0, sumsq = 0;
	int n = DE_WIN - DE_MAXLAG;
	for (int lag = 0; lag < DE_MAXLAG; lag++) {
		float acc = 0;
		const float *rr = r + lag;
		for (int i = 0; i < n; i++)
			acc += m[i] * rr[i];
		double cc = fabs(acc) / sqrt(em * er);
		sumsq += cc * cc;
		if (cc > bestc) {
			bestc = cc;
			best = lag;
		}
	}
	double noise = sqrt(sumsq / DE_MAXLAG);
	c->lv.aec_corr = (float)bestc;
	if (bestc < 0.08 || bestc < 4 * noise)
		return -1;
	return best * DE_DEC;
}

void chain_configure(struct chain *c, const struct chain_conf *conf)
{
	if (c->configured && !memcmp(&c->conf, conf, sizeof(*conf)))
		return;
	c->conf = *conf;
	c->configured = 1;

	set_delay(c, (conf->aec ? conf->aec_delay_ms : 0) * CHAIN_RATE / 1000);
	c->de_fill = 0;
	c->de_last = -1;
	c->de_agree = 0;
	c->lv.aec_delay_meas = -1;
	c->lv.aec_corr = 0;

	if (c->pp) {
		speex_preprocess_state_destroy(c->pp);
		c->pp = NULL;
	}
	if (c->aec) {
		speex_echo_state_destroy(c->aec);
		c->aec = NULL;
	}
	if (conf->aec) {
		int tail = (conf->aec_tail_ms > 0 ? conf->aec_tail_ms : 32) * CHAIN_RATE / 1000;
		c->aec = speex_echo_state_init_mc(CHAIN_FRAME, tail, 1, conf->aec_stereo ? 2 : 1);
		int rate = CHAIN_RATE;
		speex_echo_ctl(c->aec, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
		if (conf->aec_res) {
			// Speex only applies its echo floor inside the denoise stage, so
			// that stays on with 0 dB of noise suppression: bins the filter
			// says are residual echo get pushed down, everything else passes
			// untouched and RNNoise remains the noise suppressor.
			c->pp = speex_preprocess_state_init(CHAIN_FRAME, CHAIN_RATE);
			spx_int32_t on = 1, off = 0, zero = 0;
			spx_int32_t sup = conf->aec_res_db ? conf->aec_res_db : -40;
			spx_int32_t act = conf->aec_res_active_db ? conf->aec_res_active_db : -15;
			speex_preprocess_ctl(c->pp, SPEEX_PREPROCESS_SET_DENOISE, &on);
			speex_preprocess_ctl(c->pp, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &zero);
			speex_preprocess_ctl(c->pp, SPEEX_PREPROCESS_SET_AGC, &off);
			speex_preprocess_ctl(c->pp, SPEEX_PREPROCESS_SET_VAD, &off);
			speex_preprocess_ctl(c->pp, SPEEX_PREPROCESS_SET_DEREVERB, &off);
			speex_preprocess_ctl(c->pp, SPEEX_PREPROCESS_SET_ECHO_STATE, c->aec);
			speex_preprocess_ctl(c->pp, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &sup);
			speex_preprocess_ctl(c->pp, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &act);
		}
	}

	if (!c->ns && conf->ns)
		c->ns = rnnoise_create(NULL);

	c->n_eq = 0;
	for (int i = 0; i < CHAIN_MAX_EQ; i++)
		if (conf->eq[i].type != EQ_OFF && conf->eq[i].freq > 10 && conf->eq[i].freq < 23000)
			biquad_design(&c->eq[c->n_eq++], &conf->eq[i]);

	c->gate_env = 0;
	c->gate_gain = 1;
	c->gate_open = 1;
	c->gate_hold = 0;
	c->comp_env = 0;
}

void chain_process(struct chain *c, const int16_t *tap, int16_t *out)
{
	const struct chain_conf *k = &c->conf;
	float x[CHAIN_FRAME];

	// mic sum (slots: 4 = Mic1, 5 = Mic2, 3 = Mic3) or the DSP output on slot 0
	if (k->source_dsp) {
		for (int i = 0; i < CHAIN_FRAME; i++)
			x[i] = tap[i * CHAIN_TAP_CH + 0];
	} else {
		for (int i = 0; i < CHAIN_FRAME; i++) {
			const int16_t *f = tap + i * CHAIN_TAP_CH;
			x[i] = k->mic_w[0] * f[4] + k->mic_w[1] * f[5] + k->mic_w[2] * f[3];
		}
	}

	c->lv.sum = lvl(x, CHAIN_FRAME, 1);
	{
		float r[CHAIN_FRAME];
		for (int i = 0; i < CHAIN_FRAME; i++)
			r[i] = tap[i * CHAIN_TAP_CH + 0];
		c->lv.ref = lvl(r, CHAIN_FRAME, 1);
	}

	// track the mic-to-reference offset from the undelayed signals
	if (c->aec && k->aec_delay_auto) {
		for (int i = 0; i < CHAIN_FRAME; i += DE_DEC) {
			float sm = 0, sr = 0;
			for (int j = 0; j < DE_DEC; j++) {
				const int16_t *f = tap + (i + j) * CHAIN_TAP_CH;
				sm += x[i + j];
				sr += f[0] + f[1];
			}
			c->de_mic[c->de_fill] = sm / DE_DEC;
			c->de_ref[c->de_fill] = sr / DE_DEC;
			if (++c->de_fill == DE_WIN) {
				c->de_fill = 0;
				int d = estimate_delay(c);
				c->lv.aec_delay_meas = d;
				// two windows in a row within 2 ms of each other, and more
				// than 2 ms off what is in use: move and let the filter restart
				// (the tail absorbs anything smaller, a restart costs more)
				if (d >= 0 && c->de_last >= 0 && abs(d - c->de_last) <= 2 * DE_DEC) {
					if (abs(d + DE_MARGIN - c->delay_len) > 2 * CHAIN_RATE / 1000) {
						set_delay(c, d + DE_MARGIN);
						speex_echo_state_reset(c->aec);
					}
				}
				c->de_last = d;
			}
		}
	}

	// alignment delay so the DSP-side echo reference leads the echo
	for (int i = 0; i < CHAIN_FRAME; i++) {
		c->dl[c->dl_w] = x[i];
		x[i] = c->dl[(c->dl_w - c->delay_len + DL_N) % DL_N];
		if (++c->dl_w == DL_N)
			c->dl_w = 0;
	}

	// echo cancellation against the reference the DSP dumps on slots 0/1,
	// one filter per speaker when stereo
	if (c->aec) {
		int16_t rec[CHAIN_FRAME], play[2 * CHAIN_FRAME], est[CHAIN_FRAME];
		for (int i = 0; i < CHAIN_FRAME; i++) {
			float v = x[i];
			rec[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
			const int16_t *f = tap + i * CHAIN_TAP_CH;
			if (k->aec_stereo) {
				play[2 * i] = f[0];
				play[2 * i + 1] = f[1];
			} else {
				// the codec only ever fills one of the two reference
				// slots (selector 4 stays silent), so sum rather than
				// average and the reference keeps its level
				int s = f[0] + f[1];
				play[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s;
			}
		}
		speex_echo_cancellation(c->aec, rec, play, est);
		// an adaptive filter that diverges makes things louder, never
		// quieter: treat that as broken, pass the input and start over
		float in_e = 0, out_e = 0;
		for (int i = 0; i < CHAIN_FRAME; i++) {
			in_e += (float)rec[i] * rec[i];
			out_e += (float)est[i] * est[i];
		}
		if (out_e > in_e * 2.0f + 1e3f) {
			speex_echo_state_reset(c->aec);
			c->lv.aec_resets++;
		} else {
			if (c->pp)
				speex_preprocess_run(c->pp, est);
			for (int i = 0; i < CHAIN_FRAME; i++)
				x[i] = est[i];
		}
	}
	c->lv.aec = lvl(x, CHAIN_FRAME, 1);

	// noise suppression
	if (c->ns && k->ns) {
		float y[CHAIN_FRAME];
		rnnoise_process_frame(c->ns, y, x);
		float m = k->ns_mix < 0 ? 0 : k->ns_mix > 1 ? 1 : k->ns_mix;
		for (int i = 0; i < CHAIN_FRAME; i++)
			x[i] = x[i] + m * (y[i] - x[i]);
	}
	c->lv.ns = lvl(x, CHAIN_FRAME, 1);

	for (int n = 0; n < c->n_eq; n++)
		for (int i = 0; i < CHAIN_FRAME; i++)
			x[i] = biquad_run(&c->eq[n], x[i]);
	c->lv.eq = lvl(x, CHAIN_FRAME, 1);

	// gate: opens above gate_db, closes gate_hyst_db below it after gate_hold_ms,
	// and the gain moves smoothly so nothing clicks. Meant to separate a
	// close-talking voice from people across the room.
	if (k->gate_db < 0) {
		float open_thr = db2lin(k->gate_db) * 32768.0f;
		float close_thr = db2lin(k->gate_db - (k->gate_hyst_db > 0 ? k->gate_hyst_db : 6)) * 32768.0f;
		float floor_g = db2lin(-(k->gate_range_db > 0 ? k->gate_range_db : 30));
		int hold = (int)((k->gate_hold_ms > 0 ? k->gate_hold_ms : 200) * 0.001f * CHAIN_RATE);
		// envelope: 1 ms attack, 40 ms release; gain: 2 ms open, 60 ms close
		float env_att = expf(-1.0f / (0.001f * CHAIN_RATE)), env_rel = expf(-1.0f / (0.040f * CHAIN_RATE));
		float g_open = expf(-1.0f / (0.002f * CHAIN_RATE)), g_close = expf(-1.0f / (0.060f * CHAIN_RATE));
		for (int i = 0; i < CHAIN_FRAME; i++) {
			float a = fabsf(x[i]);
			c->gate_env = a > c->gate_env ? env_att * c->gate_env + (1 - env_att) * a
						       : env_rel * c->gate_env + (1 - env_rel) * a;
			if (c->gate_env >= open_thr) {
				c->gate_open = 1;
				c->gate_hold = hold;
			} else if (c->gate_open && c->gate_env < close_thr) {
				if (c->gate_hold > 0)
					c->gate_hold--;
				else
					c->gate_open = 0;
			}
			float target = c->gate_open ? 1.0f : floor_g;
			float coef = target > c->gate_gain ? g_open : g_close;
			c->gate_gain = coef * c->gate_gain + (1 - coef) * target;
			x[i] *= c->gate_gain;
		}
	}

	// compressor: peak detector, log-domain gain computer
	if (k->comp_thr_db < 0) {
		float thr = k->comp_thr_db, ratio = k->comp_ratio > 1 ? k->comp_ratio : 2;
		float att = expf(-1.0f / ((k->comp_attack_ms > 0 ? k->comp_attack_ms : 5) * 0.001f * CHAIN_RATE));
		float rel = expf(-1.0f / ((k->comp_release_ms > 0 ? k->comp_release_ms : 80) * 0.001f * CHAIN_RATE));
		float makeup = db2lin(k->comp_makeup_db);
		for (int i = 0; i < CHAIN_FRAME; i++) {
			float a = fabsf(x[i]) / 32768.0f;
			c->comp_env = a > c->comp_env ? att * c->comp_env + (1 - att) * a
						       : rel * c->comp_env + (1 - rel) * a;
			float lvl = 20 * log10f(c->comp_env + 1e-6f);
			float over = lvl - thr;
			float g = over > 0 ? db2lin(-over * (1 - 1 / ratio)) : 1.0f;
			x[i] *= g * makeup;
		}
	}

	float gain = db2lin(k->gain_db);
	float ceil = k->limit_db < 0 ? db2lin(k->limit_db) * 32768.0f : 32767.0f;
	for (int i = 0; i < CHAIN_FRAME; i++) {
		float v = x[i] * gain;
		// soft knee above 0.7 of the ceiling, hard at the ceiling
		float a = fabsf(v), knee = 0.7f * ceil;
		if (a > knee) {
			float t = (a - knee) / (ceil - knee);
			a = knee + (ceil - knee) * (1 - expf(-t));
			v = v < 0 ? -a : a;
		}
		out[i] = (int16_t)v;
		x[i] = v;
	}
	c->lv.out = lvl(x, CHAIN_FRAME, 1);
}
