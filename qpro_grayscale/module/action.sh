#!/system/bin/sh
MODDIR=$(dirname "$(readlink -f "$0")")
. "$MODDIR/common.sh"

engine_pid() { pidof mrsystemservice; }

# wait for a fresh mrsystemservice to log its consumer count (1 = gray, 2+ = color)
wait_engine() {
    i=0
    while [ $i -lt 30 ]; do
        sleep 1
        i=$((i + 1))
        pid=$(engine_pid)
        [ -n "$pid" ] && [ "$pid" != "$1" ] || continue
        n=$(logcat -d | grep " $pid .*FrameSetConsumerMap populated" | tail -n1 | grep -o "[0-9]* consumers" | cut -d' ' -f1)
        [ -n "$n" ] && { echo "$n"; return 0; }
    done
    return 1
}

if is_mounted; then
    echo "- Switching to COLOR passthrough"
    umount_gray || { echo "! unmount failed"; exit 1; }
    rm -f "$STATE"
else
    cur=$(lib_sha)
    if [ "$cur" != "$STOCK_SHA" ]; then
        echo "! libmrengine.so isn't the v207 stock build, refusing"
        exit 1
    fi
    echo "- Switching to GRAYSCALE passthrough"
    mount_gray || { echo "! mount failed"; exit 1; }
    touch "$STATE"
fi

echo "- Restarting mrsystemservice"
old=$(engine_pid)
kill $old
n=$(wait_engine "$old")
if [ -z "$n" ]; then
    echo "! engine didn't come back cleanly, check logcat"
elif [ "$n" = "1" ]; then
    echo "- Engine up: grayscale only"
else
    echo "- Engine up: color ($n consumers)"
fi

# vrshell caches passthrough state from the old engine and won't pick up
# the new mode in home until it's relaunched
echo "- Restarting VR shell"
am force-stop com.oculus.vrshell
i=0
while [ $i -lt 15 ] && [ -z "$(pidof com.oculus.vrshell)" ]; do
    sleep 1
    i=$((i + 1))
done
[ -n "$(pidof com.oculus.vrshell)" ] && echo "- Done" || echo "! vrshell didn't relaunch"
