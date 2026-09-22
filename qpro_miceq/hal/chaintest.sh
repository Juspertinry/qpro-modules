D=/data/local/tmp/qpro_miceq; T=/data/local/tmp
setc() { sed -i "s/^aec=.*/aec=$1/; s/^ns=.*/ns=$2/; s/^dump=.*/dump=$3/" $D/miceq.conf; }
tinymix -D 0 "PRI_TDM_RX_0 Audio Mixer MultiMedia2" 1
echo "--- echo tests (pink noise on speaker)"
for cfg in "0 0 echo_noaec" "1 0 echo_aec" "1 1 echo_aec_ns"; do set -- $cfg
  rm -f $D/dump.raw; setc $1 $2 0; sleep 3; $T/pcmplay 0 1 $T/pink.wav & P=$!; sleep 2.5; setc $1 $2 1; sleep 3; setc $1 $2 0; wait $P; sleep 0.3; mv $D/dump.raw $D/d_$3.raw; done
tinymix -D 0 "PRI_TDM_RX_0 Audio Mixer MultiMedia2" 0
echo "--- quiet tests"
for cfg in "0 0 quiet_dry" "0 1 quiet_ns"; do set -- $cfg
  rm -f $D/dump.raw; setc $1 $2 0; sleep 2; setc $1 $2 1; sleep 3; setc $1 $2 0; sleep 0.3; mv $D/dump.raw $D/d_$3.raw; done
setc 1 1 0
echo "--- cpu: hal process, 5 s window"; P=$(pidof android.hardware.audio.service); a=$(awk '{print $14+$15}' /proc/$P/stat); sleep 5; b=$(awk '{print $14+$15}' /proc/$P/stat); echo "ticks/5s=$((b-a)) => $(( (b-a) * 100 / 500 ))% of one core"
logcat -d -s miceq:* | grep -E "E miceq|tap|codec" | tail -5
ls -la $D/d_*.raw
