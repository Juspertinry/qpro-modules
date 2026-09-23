// Mic processing chain. Everything runs in float at 48 kHz on 10 ms frames.
// Order: mic sum -> alignment delay -> AEC -> RNNoise -> EQ -> gate ->
// compressor -> gain -> limiter.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <rnnoise.h>
#include <speex/speex_echo.h>

#include "chain.h"

struct biquad {
	float b0, b1, b2, a1, a2;
	float z1, z2;
};

struct chain {
	struct chain_conf conf;
	int configured;

	// alignment delay line (mono)
	float delay[CHAIN_MAX_DELAY_MS * CHAIN_RATE / 1000 + CHAIN_FRAME];
	int delay_len, delay_pos;

	SpeexEchoState *aec;
	DenoiseState *ns;

	struct biquad eq[CHAIN_MAX_EQ];
	int n_eq;

	float gate_env;
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
	if (c->aec)
		speex_echo_state_destroy(c->aec);
	if (c->ns)
		rnnoise_destroy(c->ns);
	free(c);
}

void chain_configure(struct chain *c, const struct chain_conf *conf)
{
	if (c->configured && !memcmp(&c->conf, conf, sizeof(*conf)))
		return;
	c->conf = *conf;
	c->configured = 1;

	int d = conf->aec ? conf->aec_delay_ms : 0;
	if (d < 0) d = 0;
	if (d > CHAIN_MAX_DELAY_MS) d = CHAIN_MAX_DELAY_MS;
	c->delay_len = d * CHAIN_RATE / 1000;
	c->delay_pos = 0;
	memset(c->delay, 0, sizeof(c->delay));

	if (c->aec) {
		speex_echo_state_destroy(c->aec);
		c->aec = NULL;
	}
	if (conf->aec) {
		int tail = (conf->aec_tail_ms > 0 ? conf->aec_tail_ms : 64) * CHAIN_RATE / 1000;
		c->aec = speex_echo_state_init(CHAIN_FRAME, tail);
		int rate = CHAIN_RATE;
		speex_echo_ctl(c->aec, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
	}

	if (!c->ns && conf->ns)
		c->ns = rnnoise_create(NULL);

	c->n_eq = 0;
	for (int i = 0; i < CHAIN_MAX_EQ; i++)
		if (conf->eq[i].type != EQ_OFF && conf->eq[i].freq > 10 && conf->eq[i].freq < 23000)
			biquad_design(&c->eq[c->n_eq++], &conf->eq[i]);

	c->gate_env = 0;
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

	// alignment delay so the DSP-side echo reference leads the echo
	if (c->delay_len > 0) {
		for (int i = 0; i < CHAIN_FRAME; i++) {
			float v = c->delay[c->delay_pos];
			c->delay[c->delay_pos] = x[i];
			x[i] = v;
			if (++c->delay_pos >= c->delay_len)
				c->delay_pos = 0;
		}
	}

	// echo cancellation against the reference the DSP dumps on slots 0/1
	if (c->aec) {
		int16_t rec[CHAIN_FRAME], play[CHAIN_FRAME], est[CHAIN_FRAME];
		for (int i = 0; i < CHAIN_FRAME; i++) {
			float v = x[i];
			rec[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
			play[i] = (tap[i * CHAIN_TAP_CH + 0] + tap[i * CHAIN_TAP_CH + 1]) / 2;
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

	// gate: downward expander on a fast RMS envelope
	if (k->gate_db < 0) {
		float thr = db2lin(k->gate_db) * 32768.0f;
		float range = db2lin(-(k->gate_range_db > 0 ? k->gate_range_db : 20));
		float att = expf(-1.0f / (0.002f * CHAIN_RATE)), rel = expf(-1.0f / (0.080f * CHAIN_RATE));
		for (int i = 0; i < CHAIN_FRAME; i++) {
			float a = fabsf(x[i]);
			c->gate_env = a > c->gate_env ? att * c->gate_env + (1 - att) * a
						       : rel * c->gate_env + (1 - rel) * a;
			float g = c->gate_env >= thr ? 1.0f : range + (1 - range) * (c->gate_env / thr);
			x[i] *= g;
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
