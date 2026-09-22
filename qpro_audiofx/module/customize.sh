#!/system/bin/sh

SRC_FX=/vendor/etc/audio_effects.xml
SRC_AP=/vendor/etc/audio_policy_configuration.xml
OUT=$MODPATH/audio_policy_configuration.xml

FAST='flags="AUDIO_OUTPUT_FLAG_FAST AUDIO_OUTPUT_FLAG_PRIMARY"'
DEEP='flags="AUDIO_OUTPUT_FLAG_PRIMARY AUDIO_OUTPUT_FLAG_DEEP_BUFFER"'

DEV=$(getprop ro.product.device)
[ "$DEV" = "seacliff" ] || abort "! Quest Pro (seacliff) only, found '$DEV'"
[ -f "$SRC_FX" ] || abort "! $SRC_FX missing"
[ -f "$SRC_AP" ] || abort "! $SRC_AP missing"

# Unlike the Quest 1, stock already registers the AOSP effects. Only check.
for E in equalizer dynamics_processing bassboost virtualizer loudness_enhancer; do
  grep -q "name=\"$E\"" $SRC_FX || ui_print "! $E not registered in $SRC_FX, EQ apps may fail"
done

# A reinstall reads through our own bind mount, so accept an already patched
# file as well as stock.
if grep -q "$FAST" $SRC_AP; then
  sed "s#$FAST#$DEEP#" $SRC_AP > $TMPDIR/ap.xml
elif grep -q "$DEEP" $SRC_AP; then
  cat $SRC_AP > $TMPDIR/ap.xml
else
  abort "! primary output flags are not what this module expects"
fi

# raw is a second FAST port routed to every sink. Left in, fast requests would
# open it and get a FastMixer back.
sed '/<route /s#,raw,#,#' $TMPDIR/ap.xml > $OUT

grep -q "$DEEP" $OUT || abort "! policy patch did not apply"
grep '<route ' $OUT | grep -q ',raw,' && abort "! raw still routed"

set_perm_recursive $MODPATH 0 0 0755 0644
set_perm $OUT 0 0 0644 u:object_r:vendor_configs_file:s0
set_perm $MODPATH/service.sh 0 0 0755

ui_print "- Primary output moved to deep buffer, raw fast port unrouted"
ui_print "- Applied at late start by restarting audioserver"
ui_print "- Output latency rises to roughly 180-230ms. Reboot to apply."
