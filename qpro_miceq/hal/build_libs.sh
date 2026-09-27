#!/bin/sh
# Builds the vendored RNNoise 0.2 and speexdsp 1.2.1 static libs for the
# 32-bit ARM HAL. No fast-math: Speex's echo canceller checks its own energies
# for NaN and negative values and restarts itself when they go bad, and
# -ffast-math lets the compiler delete that check.
set -e
: "${NDK_HOME:?set NDK_HOME to an NDK (r27c)}"
: "${RNNOISE_SRC:?set RNNOISE_SRC to the rnnoise-0.2 source tree}"
: "${SPEEXDSP_SRC:?set SPEEXDSP_SRC to the speexdsp-1.2.1 source tree}"
cd "$(dirname "$0")"
BIN=$NDK_HOME/toolchains/llvm/prebuilt/windows-x86_64/bin
CC=$BIN/armv7a-linux-androideabi29-clang.cmd
AR=$BIN/llvm-ar
O=${TMPDIR:-/tmp}/miceq_libs
rm -rf "$O"; mkdir -p "$O"
CF="-O2 -mfpu=neon -fPIC -fvisibility=hidden -w"
for f in denoise rnn rnnoise_data rnnoise_tables pitch kiss_fft celt_lpc nnet nnet_default parse_lpcnet_weights; do
	"$CC" $CF -I"$RNNOISE_SRC/include" -I"$RNNOISE_SRC/src" -c "$RNNOISE_SRC/src/$f.c" -o "$O/rn_$f.o"
done
"$AR" rcs third_party/lib/librnnoise.a "$O"/rn_*.o
for f in mdf preprocess filterbank fftwrap kiss_fft kiss_fftr; do
	"$CC" $CF -DFLOATING_POINT -DUSE_KISS_FFT -DEXPORT= -I"$SPEEXDSP_SRC/include" -Ithird_party/include \
		-I"$SPEEXDSP_SRC/libspeexdsp" -c "$SPEEXDSP_SRC/libspeexdsp/$f.c" -o "$O/spx_$f.o"
done
"$AR" rcs third_party/lib/libspeexdsp.a "$O"/spx_*.o
ls -la third_party/lib/
