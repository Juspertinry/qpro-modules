# Reproduces what a suspend/resume does to the codec without sleeping: the
# firmware reload puts the DSP output selectors back to 0 (processed chain on
# slots 0/1) and TDM ctrl1 back to 2 TX slots. Watches how the wrapper reacts.
T=/data/local/tmp
C=/sys/devices/platform/soc/a80000.i2c/i2c-0/0-002d
codec() { echo "$1" > $C/cm7120codec; cat $C/cm7120codec; }
state() {
	echo "  is_dsp_mode=$(cat $C/is_dsp_mode)  reg38=$(codec 0x0038)  outcfg=$($T/dspmem x 0x5ffc0004 1 | tail -1 | awk '{print $NF}')  mixer=$(tinymix -D 0 'MIC TO CHANNEL0' | cut -d: -f2 | tr -d ' ')/$(tinymix -D 0 'MIC TO CHANNEL1' | cut -d: -f2 | tr -d ' ')"
}
echo "--- before:"; state
logcat -c
w=$($T/dspmem x 0x5ffc0004 1 | tail -1 | awk '{print $NF}')
w0=$(printf '0x%08x' $(( w & ~0xff )))
echo "--- simulating reload: outcfg $w -> $w0, reg38 -> 0x8cf0"
$T/dspmem w 0x5ffc0004 $w0 >/dev/null
echo "0x0038 0x8cf0" > $C/cm7120codec
sleep 1; echo "--- +1 s:"; state
sleep 5; echo "--- +6 s:"; state
echo "--- wrapper log:"; logcat -d -s miceq:* | grep -E "codec|levels|E miceq|selector|reset" | tail -8
echo "--- restoring selectors through the driver"
tinymix -D 0 'MIC TO CHANNEL0' 3 >/dev/null; tinymix -D 0 'MIC TO CHANNEL1' 4 >/dev/null
sleep 1; echo "--- after restore:"; state
