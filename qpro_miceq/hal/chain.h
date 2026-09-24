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
	int aec_stereo;        // model left and right speaker paths separately
	int ref_sel[2];        // DSP dump selectors for slots 0/1 while AEC runs (3,4 = echo ref L/R)
	int aec_tail_ms;
	int aec_delay_ms;      // mic delay so the reference is not behind the echo
	int aec_delay_auto;    // measure that delay from the signals and track it
	int aec_res;           // residual echo suppression after the linear filter
	int aec_res_db;        // suppression when only echo is present
	int aec_res_active_db; // suppression while the user talks over it
	int ns;
	float ns_mix;          // 0 = dry, 1 = fully denoised
	struct eq_band eq[CHAIN_MAX_EQ];
	float gate_db;         // 0 = off; gate opens above this level (dBFS)
	float gate_range_db;   // attenuation when closed
	float gate_hold_ms;    // stays open this long after the level drops
	float gate_hyst_db;    // closes this far below the open threshold
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
// Forget what the echo canceller learned (the reference changed under it).
void chain_reset_aec(struct chain *c);
// Levels (dBFS) of the last processed frame at each stage, for debugging.
struct chain_levels {
	float sum, ref, aec, ns, eq, out;
	int aec_resets;
	int aec_delay;         // mic delay in use, samples
	int aec_delay_meas;    // last measured mic lead over the reference, samples (-1: none)
	float aec_corr;        // normalized correlation of that measurement
};
void chain_levels(const struct chain *c, struct chain_levels *l);
