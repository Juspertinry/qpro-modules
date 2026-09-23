#!/bin/sh
# Builds the 32-bit HAL wrapper. Point NDK_HOME at an NDK (r27c is what I use).
set -e
NDK_HOME=${NDK_HOME:-$HOME/android-ndk-r27c}
NDK=$NDK_HOME/toolchains/llvm/prebuilt/windows-x86_64/bin
CC=$NDK/armv7a-linux-androideabi29-clang.cmd
cd "$(dirname "$0")"
"$CC" -O2 -mfpu=neon -Wall -Wextra -shared -fPIC -Ithird_party/include \
	-o audio.primary.kona.so miceq_hal.c chain.c \
	third_party/lib/librnnoise.a third_party/lib/libspeexdsp.a \
	-llog -lm -Wl,--no-undefined -Wl,--exclude-libs,ALL
ls -la audio.primary.kona.so
