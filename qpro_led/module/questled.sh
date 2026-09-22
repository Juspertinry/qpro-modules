#!/system/bin/sh
#
# Rainbow on the status LED while awake. Battery colour while asleep.
#
# Frame timing uses `read -t` on a fifo, not `sleep`. toybox sleep forks, which
# costs ~35ms, so a 5ms sleep really takes 41ms. The frame loop also avoids
# `$(...)` for the same reason. Costs about 4% of one core at 60fps.

LEDS=/sys/class/leds
R=$LEDS/red
G=$LEDS/green
B=$LEDS/blue
PANEL=/sys/class/graphics/fb0/show_blank_event
# Quest Pro has no fb0. DRM dpms reads On/Off directly.
DPMS=/sys/class/drm/card0-DSI-1/dpms
BL=$LEDS/lcd-backlight/brightness
[ -r "$BL" ] || BL=/sys/class/backlight/panel0-backlight/brightness
CAP=/sys/class/power_supply/battery/capacity
# usb and dc stay 0 on the Pro; the PD controller is what reports the cable.
ONLINE_NODES="usb dc cypd3177 pc_port"
BATT_STATUS=/sys/class/power_supply/battery/status
CONF=/data/adb/questled.conf
LOG=/data/adb/questled.log

ENABLED=1          # 0 disables the daemon without uninstalling
AWAKE_EFFECT=rainbow   # rainbow | battery | off
SPEED=8            # seconds per full hue rotation
FPS=60             # frame rate while awake
BRIGHT=255         # brightness ceiling while awake, 0-255
SLEEP_BRIGHT=90    # dimmer while asleep so it is not glaring in a dark room
SLEEP_POLL=1       # seconds between checks while asleep (whole seconds)
SLEEP_TIMEOUT=90   # blank the LED after this long asleep on battery; 0 = never
GAMMA=1            # perceptual correction
DEBUG=0            # 1 logs a line every sleep poll, for diagnosing behaviour

[ -r "$CONF" ] && . "$CONF"

[ "$ENABLED" = 1 ] || exit 0

# A typo in the config must not wedge the arithmetic below.
case $SPEED        in ''|*[!0-9]*) SPEED=8 ;;        esac
case $FPS          in ''|*[!0-9]*) FPS=60 ;;         esac
case $BRIGHT       in ''|*[!0-9]*) BRIGHT=255 ;;     esac
case $SLEEP_BRIGHT in ''|*[!0-9]*) SLEEP_BRIGHT=90 ;; esac
case $SLEEP_POLL   in ''|*[!0-9]*) SLEEP_POLL=1 ;;   esac
case $SLEEP_TIMEOUT in ''|*[!0-9]*) SLEEP_TIMEOUT=90 ;; esac
[ "$SPEED" -lt 1 ] && SPEED=1
[ "$FPS" -lt 1 ] && FPS=1
[ "$SLEEP_POLL" -lt 1 ] && SLEEP_POLL=1
[ "$BRIGHT" -gt 255 ] && BRIGHT=255
[ "$SLEEP_BRIGHT" -gt 255 ] && SLEEP_BRIGHT=255

TICKFD=9
HAVE_TICK=0
LED_R=-1; LED_G=-1; LED_B=-1
r=0; g=0; b=0
CUR_BRIGHT=$BRIGHT
BATT=50

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }

# Keep the log from growing without bound across reboots.
[ -f "$LOG" ] && [ "$(stat -c %s "$LOG" 2>/dev/null || echo 0)" -gt 65536 ] && : > "$LOG"

# late_start is late enough in practice, but probe order is not guaranteed.

n=0
while [ ! -w "$R/brightness" ]; do
    n=$(( n + 1 ))
    [ "$n" -gt 60 ] && { log "giving up, $R/brightness never appeared"; exit 1; }
    sleep 1
done

# A fifo held open at both ends blocks on read, which makes `read -t` an
# accurate builtin delay. Unlink it straight away; the fd keeps it alive.

open_tick() {
    _f=/data/local/tmp/.questled.$$
    rm -f "$_f"
    mkfifo "$_f" 2>/dev/null || return 1
    eval "exec $TICKFD<> \"\$_f\"" 2>/dev/null || { rm -f "$_f"; return 1; }
    rm -f "$_f"
    HAVE_TICK=1
    return 0
}

tick() {
    if [ "$HAVE_TICK" = 1 ]; then
        read -t "$1" -u$TICKFD _junk 2>/dev/null
    else
        sleep "$1"
    fi
    return 0
}

# Frame delay string, built once. Doing this per frame would fork printf.
ms=$(( 1000 / FPS ))
[ "$ms" -lt 1 ] && ms=1
if   [ "$ms" -ge 100 ]; then FRAME_DELAY="0.$ms"
elif [ "$ms" -ge 10 ];  then FRAME_DELAY="0.0$ms"
else                         FRAME_DELAY="0.00$ms"
fi

# Always write all three channels. The stock light HAL repaints this LED on
# charge state changes, and a skip-if-unchanged cache loses to it permanently.
put() {
    echo "$1" > "$R/brightness"
    echo "$2" > "$G/brightness"
    echo "$3" > "$B/brightness"
    LED_R=$1; LED_G=$2; LED_B=$3   # recorded for DEBUG only, never for control
    return 0
}

# Gamma 2.0 as v*v/255. Close enough to 2.2 here, and stays integer.
emit() {
    if [ "$GAMMA" = 1 ]; then
        put $(( $1 * $1 * CUR_BRIGHT / 65025 )) \
            $(( $2 * $2 * CUR_BRIGHT / 65025 )) \
            $(( $3 * $3 * CUR_BRIGHT / 65025 ))
    else
        put $(( $1 * CUR_BRIGHT / 255 )) \
            $(( $2 * CUR_BRIGHT / 255 )) \
            $(( $3 * CUR_BRIGHT / 255 ))
    fi
}

# Quest 1 blinks through a blink node. The Pro's light HAL uses the kernel
# timer trigger instead, which keeps toggling brightness under us.
clear_blink() {
    for _ch in red green blue; do
        [ -w "$LEDS/$_ch/blink" ] && echo 0 > "$LEDS/$_ch/blink"
        [ -w "$LEDS/$_ch/trigger" ] && echo none > "$LEDS/$_ch/trigger"
        [ -w "$LEDS/$_ch/breath" ] && echo 0 > "$LEDS/$_ch/breath"
    done
    return 0
}

# dpms or panel_power_on is the direct signal. Backlight is the fallback if
# neither node exists on another headset revision.

is_awake() {
    if [ -r "$DPMS" ]; then
        read -r _p < "$DPMS" 2>/dev/null
        case "$_p" in
            On) return 0 ;;
            Off|Standby|Suspend) return 1 ;;
        esac
    fi
    if [ -r "$PANEL" ]; then
        read -r _p < "$PANEL" 2>/dev/null
        case "$_p" in
            *"= 1") return 0 ;;
            *"= 0") return 1 ;;
        esac
    fi
    read -r _b < "$BL" 2>/dev/null || return 0
    [ "${_b:-1}" -gt 0 ]
}

# h is 0..1535.
h2rgb() {
    _h=$(( $1 % 1536 ))
    _f=$(( _h & 255 ))
    case $(( _h >> 8 )) in
        0) r=255;           g=$_f;           b=0             ;;
        1) r=$(( 255-_f )); g=255;           b=0             ;;
        2) r=0;             g=255;           b=$_f           ;;
        3) r=0;             g=$(( 255-_f )); b=255           ;;
        4) r=$_f;           g=0;             b=255           ;;
        *) r=255;           g=0;             b=$(( 255-_f )) ;;
    esac
}

# Two segments meeting at orange, so the midpoint reads amber rather than a
# muddy red-green blend.
batt_rgb() {
    _c=$1
    [ "$_c" -lt 0 ] 2>/dev/null && _c=0
    [ "$_c" -gt 100 ] 2>/dev/null && _c=100
    if [ "$_c" -le 50 ]; then
        r=255
        g=$(( _c * 128 / 50 ))
        b=0
    else
        r=$(( 255 - (_c - 50) * 255 / 50 ))
        g=$(( 128 + (_c - 50) * 127 / 50 ))
        b=0
    fi
}

read_batt() {
    read -r BATT < "$CAP" 2>/dev/null || BATT=50
    case "$BATT" in
        ''|*[!0-9]*) BATT=50 ;;
    esac
}

# usb/online stays 1 once the battery reads Full. status does not.
on_power() {
    for _n in $ONLINE_NODES; do
        read -r _o < "/sys/class/power_supply/$_n/online" 2>/dev/null && [ "$_o" = 1 ] && return 0
    done
    read -r _s < "$BATT_STATUS" 2>/dev/null
    case "$_s" in
        Charging|Full) return 0 ;;
    esac
    return 1
}

cleanup() {
    trap - INT TERM EXIT
    clear_blink
    put 0 0 0
    [ "$HAVE_TICK" = 1 ] && eval "exec $TICKFD>&-"
    log "stopped"
    exit 0
}
trap cleanup INT TERM EXIT

open_tick || log "no fifo available, falling back to sleep (frames will be choppy)"
clear_blink
log "started (awake=$AWAKE_EFFECT speed=${SPEED}s fps=$FPS bright=$BRIGHT sleep_bright=$SLEEP_BRIGHT sleep_timeout=${SLEEP_TIMEOUT}s)"

frames=$(( SPEED * FPS ))
i=0
state=-1
check=0
battwait=0
slept=0

while :; do
    # A file read every frame buys nothing. A sixth of a second of wake
    # latency is not noticeable.
    if [ "$check" -le 0 ]; then
        if is_awake; then now=1; else now=0; fi
        check=10
        if [ "$now" != "$state" ]; then
            state=$now
            battwait=0
            slept=0          # the blank timer starts fresh each time it sleeps
            if [ "$now" = 1 ]; then
                CUR_BRIGHT=$BRIGHT
            else
                CUR_BRIGHT=$SLEEP_BRIGHT
            fi
            [ "$DEBUG" = 1 ] && log "transition -> $([ "$now" = 1 ] && echo awake || echo asleep)"
        fi
    fi
    check=$(( check - 1 ))

    if [ "$state" = 1 ]; then
        case $AWAKE_EFFECT in
            rainbow)
                h2rgb $(( i * 1536 / frames ))
                emit $r $g $b
                i=$(( i + 1 ))
                [ "$i" -ge "$frames" ] && i=0
                ;;
            battery)
                [ "$battwait" -le 0 ] && { read_batt; batt_rgb "$BATT"; battwait=300; }
                battwait=$(( battwait - 1 ))
                emit $r $g $b
                ;;
            *)  emit 0 0 0 ;;
        esac
        tick "$FRAME_DELAY"
    else
        # On battery the gauge is only worth showing for a while. On external
        # power there is nothing to save, so leave it lit.
        if on_power; then
            pw=1
            slept=0
        else
            pw=0
            slept=$(( slept + SLEEP_POLL ))
        fi

        if [ "$SLEEP_TIMEOUT" -gt 0 ] && [ "$slept" -ge "$SLEEP_TIMEOUT" ]; then
            emit 0 0 0
            [ "$DEBUG" = 1 ] && log "sleep: slept=${slept}s powered=$pw -> BLANK"
        else
            # Cheap enough to re-read every pass, and tracks the drain live.
            read_batt
            batt_rgb "$BATT"
            emit $r $g $b
            if [ "$DEBUG" = 1 ]; then
                # Read back what the hardware holds, so HAL contention shows.
                read -r _hr < "$R/brightness" 2>/dev/null
                read -r _hg < "$G/brightness" 2>/dev/null
                read -r _hb < "$B/brightness" 2>/dev/null
                log "sleep: slept=${slept}s powered=$pw batt=${BATT}% wrote=$LED_R,$LED_G,$LED_B hw=$_hr,$_hg,$_hb"
            fi
        fi
        tick "$SLEEP_POLL"
        check=0   # notice the wake promptly rather than up to 10 polls later
    fi
done
