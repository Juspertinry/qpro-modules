# One-off runtime deploy for testing. The module's service.sh does the same at boot.
D=/data/local/tmp/qpro_miceq
K=/vendor/lib/hw/audio.primary.kona.so
S=/vendor/lib/hw/audio.primary.default.so

# the running HAL keeps the mounted files busy, so stop it first, then
# peel off every stacked mount before touching the files underneath
stop audioserver; stop vendor.audio-hal; sleep 1
for i in 1 2 3 4 5 6 7 8; do umount $K 2>/dev/null; umount $S 2>/dev/null; done
if grep -q audio.primary /proc/mounts; then echo "still mounted, abort"; exit 1; fi
# the real stock library is ~646 KB and never mentions us
if strings $K | grep -q miceq || [ "$(stat -c %s $K)" -lt 500000 ]; then echo "$K is not stock, abort"; exit 1; fi
cp $K $D/stock.so
chmod 644 $D/stock.so $D/wrapper.so; chmod 666 $D/miceq.conf
chcon u:object_r:vendor_file:s0 $D/stock.so $D/wrapper.so 2>/dev/null
mount --bind $D/stock.so $S || exit 1
mount --bind $D/wrapper.so $K || exit 1
chmod 666 /sys/devices/platform/soc/a80000.i2c/i2c-0/0-002d/cm7120codec
logcat -c
start vendor.audio-hal; start audioserver
sleep 6
am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null
echo "--- logcat"; logcat -d -s miceq:* | grep -E "loaded|config|E miceq" | tail -4
echo "--- hal/audioserver pids:"; pidof android.hardware.audio.service audioserver
echo "--- mixer:"; tinymix -D 0 "PRI_TDM_TX_0 Channels" | cut -c1-60; tinymix -D 0 "MultiMedia10 Mixer PRI_TDM_TX_0"
C=/sys/devices/platform/soc/a80000.i2c/i2c-0/0-002d/cm7120codec; echo 0x0038 > $C; echo "--- codec 0x38: $(cat $C)"
