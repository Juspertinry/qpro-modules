#!/system/bin/sh

SKIPUNZIP=0

ui_print "- Quest Pro Grayscale Passthrough"

MODDIR=$MODPATH
. "$MODPATH/common.sh"

find_build "$(lib_sha)" || abort "! libmrengine.so isn't a HorizonOS build this patch supports ($(getprop ro.build.version.incremental))"
make_patched || abort "! couldn't build the patched lib"

set_perm "$PATCHED" 0 0 0644 u:object_r:system_lib_file:s0
ui_print "- Starts in color. Use the Action button to toggle grayscale."
