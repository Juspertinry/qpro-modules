# echo suppression and CPU vs AEC tail length; pink noise on the speaker
D=/data/local/tmp/qpro_miceq; T=/data/local/tmp
sed -i "s/^ns=.*/ns=0/; s/^aec=.*/aec=1/; s/^debug=.*/debug=1/" $D/miceq.conf
tinymix -D 0 "PRI_TDM_RX_0 Audio Mixer MultiMedia2" 1
for tail in 64 32 16; do
  sed -i "s/^aec_tail_ms=.*/aec_tail_ms=$tail/" $D/miceq.conf; sleep 2; logcat -c
  $T/pcmplay 0 1 $T/pink_quiet.wav & P=$!; sleep 2
  Q=$(pidof android.hardware.audio.service); a=$(awk '{print $14+$15}' /proc/$Q/stat); sleep 4; b=$(awk '{print $14+$15}' /proc/$Q/stat)
  wait $P
  echo "tail $tail ms: cpu $(( (b - a) * 100 / 400 ))%  $(logcat -d -s miceq:* | grep levels | tail -4 | awk '{s+=$8; e+=$12; n++} END {if (n) printf "sum %.1f -> aec %.1f dB (%d s)", s/n, e/n, n; else printf "no level logs"}')"
done
tinymix -D 0 "PRI_TDM_RX_0 Audio Mixer MultiMedia2" 0
sed -i "s/^ns=.*/ns=1/; s/^aec_tail_ms=.*/aec_tail_ms=64/; s/^debug=.*/debug=0/" $D/miceq.conf
