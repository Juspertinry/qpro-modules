#!/system/bin/sh
MODDIR=$(dirname "$(readlink -f "$0")")
. "$MODDIR/common.sh"

rm -f "$MODDIR/last_error"
[ -f "$STATE" ] || exit 0

# runs before mrsystemservice starts, so no restart needed here. After an OTA
# to another supported build the patched copy is regenerated from the new stock.
if make_patched; then
    mount_gray
else
    rm -f "$STATE"
    echo "libmrengine isn't a supported build, grayscale disabled" > "$MODDIR/last_error"
fi
