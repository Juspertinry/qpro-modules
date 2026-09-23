// Mic processing chain: 6-slot tap frames in, mono 16-bit out.
#pragma once
#include <stdint.h>

#define CHAIN_RATE 48000
#define CHAIN_FRAME 480       // 10 ms, what RNNoise and Speex want
#define CHAIN_TAP_CH 6
#define CHAIN_MAX_EQ 8
#define CHAIN_MAX_DELAY_MS 200

enum eq_type { EQ_OFF, EQ_PK, EQ_LS, EQ_HS, EQ_HP, EQ_LP, EQ_NOTCH };

struct eq_band {
	enum eq_type type;
	float freq, gain_db, q;
};

struct chain_conf {
	float mic_w[3];        // weights for Mic1, Mic2, Mic3 (slots 4, 5, 3)
	int source_dsp;        // 1: take the DSP's processed output (slot 0) instead
	int aec;
	int aec_tail_ms;
	int aec_delay_ms;      // mic delay so the reference is not behind the echo
	int ns;
	float ns_mix;          // 0 = dry, 1 = fully denoised
	struct eq_band eq[CHAIN_MAX_EQ];
	float gate_db;         // 0 = off; downward expander threshold
	float gate_range_db;
	float comp_thr_db;     // 0 = off
	float comp_ratio, comp_attack_ms, comp_release_ms, comp_makeup_db;
	float gain_db;
	float limit_db;        // 0 = off; soft limiter ceiling
};

struct chain;

struct chain *chain_create(void);
void chain_destroy(struct chain *c);
// Rebuilds state whenever the config changes; safe to call every frame.
void chain_configure(struct chain *c, const struct chain_conf *conf);
// Process exactly CHAIN_FRAME tap frames (interleaved 6 x int16) into mono.
void chain_process(struct chain *c, const int16_t *tap, int16_t *out);
// Levels (dBFS) of the last processed frame at each stage, for debugging.
struct chain_levels { float sum, ref, aec, ns, eq, out; int aec_resets; };
void chain_levels(const struct chain *c, struct chain_levels *l);
