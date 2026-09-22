#!/system/bin/sh
# The only change is a bind mount made at late start, and it is gone after a
# reboot. Nothing outside the module directory to clean up.

log -t qpro_audiofx "removed, reboot to restore the stock audio config"
exit 0
