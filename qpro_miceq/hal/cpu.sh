# CPU of the HAL process with the chain running (debug keeps the tap alive)
D=/data/local/tmp/qpro_miceq
sed -i "s/^debug=.*/debug=1/" $D/miceq.conf
P=$(pidof android.hardware.audio.service); sleep 4
a=$(cut -d' ' -f14,15 /proc/$P/stat | tr ' ' '+' | xargs expr 2>/dev/null || awk '{print $14+$15}' /proc/$P/stat)
sleep 5
b=$(awk '{print $14+$15}' /proc/$P/stat)
echo "hal cpu: $(( (b - a) * 100 / 500 ))% of one core"
sed -i "s/^debug=.*/debug=0/" $D/miceq.conf
