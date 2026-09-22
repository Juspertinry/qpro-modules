#!/system/bin/sh

SKIPUNZIP=0

ui_print "- Quest LED"

if [ ! -e /sys/class/leds/red/brightness ]; then
    ui_print "! /sys/class/leds/red/brightness not found."
    ui_print "! This module targets Quest headsets with the qcom RGB status LED."
    ui_print "! Installing anyway; the daemon will exit if the node never appears."
fi

# Settings survive module updates.
if [ ! -f /data/adb/questled.conf ]; then
    ui_print "- Writing default config to /data/adb/questled.conf"
    cat > /data/adb/questled.conf <<'CONF'
# Quest LED settings. Reboot after editing, or restart the daemon from an
# interactive root shell (inside su -c "..." pkill -f matches its own shell):
#   pkill -f /data/adb/modules/questled/questled.sh; nohup sh /data/adb/modules/questled/questled.sh &

# 0 stops the daemon without uninstalling.
ENABLED=1

# rainbow | battery | off
AWAKE_EFFECT=rainbow

# Seconds for one full rainbow rotation.
SPEED=8

# 60 costs about 4% of one core. 30 halves it.
FPS=60

# Brightness ceiling while awake, 0-255.
BRIGHT=255

# Lower, so it is not glaring in a dark room.
SLEEP_BRIGHT=90

# Seconds between battery checks while asleep. Whole seconds.
SLEEP_POLL=1

# Blank after this long asleep and on battery. On a charger it stays lit.
# 0 never blanks.
SLEEP_TIMEOUT=90

# Gamma correction, so the ramp looks even.
GAMMA=1

# 1 logs every sleep poll to /data/adb/questled.log with a hardware read-back.
DEBUG=0
CONF
    chmod 644 /data/adb/questled.conf
else
    ui_print "- Keeping existing /data/adb/questled.conf"
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/questled.sh" 0 0 0755
set_perm "$MODPATH/service.sh"  0 0 0755

ui_print "- Done. Reboot to start."
