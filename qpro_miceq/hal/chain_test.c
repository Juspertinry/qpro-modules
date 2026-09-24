// Offline harness: runs the chain over a raw 6-slot tap capture so AEC and
// delay tracking can be checked on recordings instead of live on the headset.
// usage: chain_test <in6.raw> <out.raw> [key=value ...]   (keys as in miceq.conf)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain.h"

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s in6.raw out.raw [key=value ...]\n", argv[0]);
		return 1;
	}
	struct chain_conf k;
	memset(&k, 0, sizeof(k));
	k.mic_w[0] = k.mic_w[1] = k.mic_w[2] = 1.0f / 3;
	k.aec = 1; k.aec_stereo = 0; k.aec_tail_ms = 32; k.aec_delay_ms = 37; k.aec_delay_auto = 1;
	k.aec_res = 1; k.aec_res_db = -40; k.aec_res_active_db = -15;
	k.ns = 0; k.ns_mix = 1;
	for (int i = 3; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		if (!eq) continue;
		*eq = 0;
		const char *key = argv[i], *val = eq + 1;
		if (!strcmp(key, "aec")) k.aec = atoi(val);
		else if (!strcmp(key, "aec_stereo")) k.aec_stereo = atoi(val);
		else if (!strcmp(key, "aec_tail_ms")) k.aec_tail_ms = atoi(val);
		else if (!strcmp(key, "aec_delay_ms")) { k.aec_delay_auto = !strcmp(val, "auto"); if (!k.aec_delay_auto) k.aec_delay_ms = atoi(val); }
		else if (!strcmp(key, "aec_res")) k.aec_res = atoi(val);
		else if (!strcmp(key, "aec_res_db")) k.aec_res_db = -abs(atoi(val));
		else if (!strcmp(key, "aec_res_active_db")) k.aec_res_active_db = -abs(atoi(val));
		else if (!strcmp(key, "ns")) k.ns = atoi(val);
		else if (!strcmp(key, "gain_db")) k.gain_db = atof(val);
	}
	FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
	if (!in || !out) { perror("open"); return 1; }
	struct chain *c = chain_create();
	chain_configure(c, &k);
	int16_t tap[CHAIN_FRAME * CHAIN_TAP_CH], mono[CHAIN_FRAME];
	int n = 0, last_delay = -1;
	while (fread(tap, sizeof(tap), 1, in) == 1) {
		chain_process(c, tap, mono);
		fwrite(mono, sizeof(mono), 1, out);
		struct chain_levels l;
		chain_levels(c, &l);
		if (l.aec_delay != last_delay || (n % 50 == 49)) {
			printf("t=%5.2f sum %6.1f ref %6.1f aec %6.1f out %6.1f | delay %.2f ms meas %.2f ms corr %.3f resets %d\n",
			       n * 0.01, l.sum, l.ref, l.aec, l.out, l.aec_delay * 1000.0 / CHAIN_RATE,
			       l.aec_delay_meas * 1000.0 / CHAIN_RATE, l.aec_corr, l.aec_resets);
			last_delay = l.aec_delay;
		}
		n++;
	}
	chain_destroy(c);
	fclose(in);
	fclose(out);
	return 0;
}
