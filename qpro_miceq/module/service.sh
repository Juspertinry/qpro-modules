#!/system/bin/sh
#
# Root on this headset comes up after the audio HAL has loaded its library, so
# the wrapper is bind-mounted over it at late start and the HAL is restarted.
# Bind mounts rather than overlay: no dependency on magisk_overlayfs, and a
# reboot drops them.

MODDIR=${0%/*}
TAG=qpro_miceq
K=/vendor/lib/hw/audio.primary.kona.so
S=/vendor/lib/hw/audio.primary.default.so
CONF=/data/local/tmp/qpro_miceq
CODEC=/sys/devices/platform/soc/a80000.i2c/i2c-0/0-002d/cm7120codec

until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 1; done

# The root enabler can be run twice in one boot.
if grep -q " $K " /proc/mounts; then
  log -t $TAG "already mounted"
  exit 0
fi

# The wrapper dlopens the stock library through the unused default stub. Take
# the copy from the live file: after a reboot nothing is mounted over it.
if strings $K | grep -q miceq || [ "$(stat -c %s $K)" -lt 500000 ]; then
  log -t $TAG "$K is not the stock HAL, refusing to mount"
  exit 1
fi
cp $K $MODDIR/stock.so || exit 1
chmod 644 $MODDIR/stock.so $MODDIR/audio.primary.kona.so
chcon u:object_r:vendor_file:s0 $MODDIR/stock.so $MODDIR/audio.primary.kona.so 2>/dev/null

# Config lives where the HAL (uid audioserver) can read it and adb can edit it.
mkdir -p $CONF/presets
[ -f $CONF/miceq.conf ] || cp $MODDIR/miceq.conf $CONF/miceq.conf
cp $MODDIR/presets/*.conf $CONF/presets/
chmod 777 $CONF $CONF/presets; chmod 666 $CONF/miceq.conf $CONF/presets/*.conf

# The wrapper re-applies the codec's 6-slot TDM setting after every sleep.
chmod 666 $CODEC

mount -o bind $MODDIR/stock.so $S || { log -t $TAG "bind mount of stock failed"; exit 1; }
mount -o bind $MODDIR/audio.primary.kona.so $K || { log -t $TAG "bind mount of wrapper failed"; umount $S; exit 1; }

OLD=$(pidof android.hardware.audio.service)
stop audioserver
stop vendor.audio-hal
sleep 1
start vendor.audio-hal
start audioserver
sleep 3
NEW=$(pidof android.hardware.audio.service)
log -t $TAG "wrapper mounted, audio hal $OLD -> ${NEW:-<not running>}"
exit 0
