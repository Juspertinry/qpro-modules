#!/bin/sh
# Builds chain_test for the host with mingw so recordings can be run through
# the chain offline. Needs the RNNoise 0.2 and speexdsp 1.2.1 source trees.
set -e
: "${RNNOISE_SRC:?set RNNOISE_SRC to the rnnoise-0.2 source tree}"
: "${SPEEXDSP_SRC:?set SPEEXDSP_SRC to the speexdsp-1.2.1 source tree}"
cd "$(dirname "$0")"
O=${TMPDIR:-/tmp}/miceq_host
mkdir -p "$O"
for f in denoise rnn rnnoise_data rnnoise_tables pitch kiss_fft celt_lpc nnet nnet_default parse_lpcnet_weights; do
	gcc -O2 -w -I"$RNNOISE_SRC/include" -I"$RNNOISE_SRC/src" -c "$RNNOISE_SRC/src/$f.c" -o "$O/rn_$f.o"
done
for f in mdf preprocess filterbank fftwrap kiss_fft kiss_fftr; do
	gcc -O2 -w -DFLOATING_POINT -DUSE_KISS_FFT -DEXPORT= -I"$SPEEXDSP_SRC/include" -Ithird_party/include \
		-I"$SPEEXDSP_SRC/libspeexdsp" -c "$SPEEXDSP_SRC/libspeexdsp/$f.c" -o "$O/spx_$f.o"
done
gcc -O2 -Wall -Wextra -std=gnu11 -Ithird_party/include -o chain_test chain_test.c chain.c "$O"/*.o -lm
ls -la chain_test*
