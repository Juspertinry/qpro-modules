#!/system/bin/sh
#
# Root on this headset comes up after audioserver has already read its config,
# so a mounted file alone does nothing until audioserver restarts. Bind mount
# rather than overlay: no dependency on magisk_overlayfs, and a reboot drops it.

MODDIR=${0%/*}
TAG=qpro_audiofx
F=/vendor/etc/audio_policy_configuration.xml
SRC=$MODDIR/audio_policy_configuration.xml

until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 1; done

[ -f "$SRC" ] || { log -t $TAG "no generated policy, reinstall the module"; exit 1; }

# The root enabler can be run twice in one boot. Stacked binds are harmless
# but a second audioserver restart is not worth the dropout.
if grep -q ' /vendor/etc/audio_policy_configuration.xml ' /proc/mounts; then
  log -t $TAG "already mounted"
  exit 0
fi

mount -o bind "$SRC" "$F" || { log -t $TAG "bind mount failed"; exit 1; }

# MMAP streams skip the mixer entirely, so no EQ can reach them.
resetprop aaudio.mmap_policy 1
resetprop aaudio.mmap_exclusive_policy 1

OLD=$(pidof audioserver)
setprop ctl.restart audioserver
sleep 3
NEW=$(pidof audioserver)
if [ -n "$OLD" ] && [ "$OLD" = "$NEW" ]; then
  kill "$OLD" 2>/dev/null
  sleep 3
  NEW=$(pidof audioserver)
fi

log -t $TAG "policy mounted, audioserver $OLD -> ${NEW:-<not running>}"
exit 0
