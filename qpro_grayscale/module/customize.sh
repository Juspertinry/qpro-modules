#!/system/bin/sh

SKIPUNZIP=0

ui_print "- Quest Pro Grayscale Passthrough"

MODDIR=$MODPATH
. "$MODPATH/common.sh"

# Meta's lib isn't shipped. Copy the device's own and flip one instruction:
# the b.ne at 0x698874 that keeps non-Quest 2 devices off the mono-only
# FrameSetConsumer table becomes a nop.
PATCH_OFF=$((0x698874))

cur=$(lib_sha)
mkdir -p "$MODPATH/patched"
if [ "$cur" = "$STOCK_SHA" ]; then
    cp "$TARGET" "$PATCHED"
    printf '\037\040\003\325' | dd of="$PATCHED" bs=1 seek=$PATCH_OFF conv=notrunc 2>/dev/null
elif [ "$cur" = "$PATCHED_SHA" ]; then
    # reinstall while grayscale is mounted
    cp "$TARGET" "$PATCHED"
else
    abort "! libmrengine.so isn't the HorizonOS v207 build this patch targets"
fi

[ "$(sha256sum "$PATCHED" | cut -d' ' -f1)" = "$PATCHED_SHA" ] || abort "! patched lib hash mismatch"

set_perm "$PATCHED" 0 0 0644 u:object_r:system_lib_file:s0
ui_print "- Starts in color. Use the Action button to toggle grayscale."
