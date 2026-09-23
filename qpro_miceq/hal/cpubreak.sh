# per-stage CPU cost of the HAL process; needs an app holding the mic
D=/data/local/tmp/qpro_miceq
m() { P=$(pidof android.hardware.audio.service); a=$(awk '{print $14+$15}' /proc/$P/stat); sleep 5; b=$(awk '{print $14+$15}' /proc/$P/stat); echo "$1: $(( (b - a) * 100 / 500 ))%"; }
cfg() { sed -i "s/^aec=.*/aec=$1/; s/^ns=.*/ns=$2/" $D/miceq.conf; sleep 2; }
cfg 1 1; m "aec+ns"
cfg 0 1; m "ns only"
cfg 1 0; m "aec only"
cfg 0 0; m "neither"
cfg 1 1
