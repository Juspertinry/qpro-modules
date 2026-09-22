#!/system/bin/sh
# The bind mounts are made at late start and are gone after a reboot. The
# config directory is left in place so tuning survives a reinstall.

log -t qpro_miceq "removed, reboot to restore the stock mic path"
exit 0
