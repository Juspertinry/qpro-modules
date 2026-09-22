#!/system/bin/sh

DEV=$(getprop ro.product.device)
[ "$DEV" = "seacliff" ] || abort "! Quest Pro (seacliff) only, found '$DEV'"
[ -f /vendor/lib/hw/audio.primary.kona.so ] || abort "! stock audio HAL not found"
[ -f /vendor/lib/hw/audio.primary.default.so ] || abort "! default HAL stub not found, nothing to mount the stock copy on"
[ -d /sys/devices/platform/soc/a80000.i2c/i2c-0/0-002d ] || abort "! CM7120 codec not found"

set_perm_recursive $MODPATH 0 0 0755 0644
set_perm $MODPATH/service.sh 0 0 0755

ui_print "- Mic chain wrapper installed; applied at late start by restarting the audio HAL"
ui_print "- Config: /data/local/tmp/qpro_miceq/miceq.conf (edits apply live)"
ui_print "- Presets in /data/local/tmp/qpro_miceq/presets/. Reboot to apply."
