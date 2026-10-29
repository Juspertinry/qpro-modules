#!/system/bin/sh
MODDIR=$(dirname "$(readlink -f "$0")")
. "$MODDIR/common.sh"

rm -f "$MODDIR/last_error"
[ -f "$STATE" ] || exit 0

# runs before mrsystemservice starts, so no restart needed here
if [ "$(lib_sha)" = "$STOCK_SHA" ]; then
    mount_gray
else
    rm -f "$STATE"
    echo "stock libmrengine changed, grayscale disabled" > "$MODDIR/last_error"
fi
