# Reproduce tap stalls on purpose while recording what apps receive, so the
# recording can be checked for repeated (looped) audio versus silence gaps.
D=/data/local/tmp/qpro_miceq
P=$(pidof android.hardware.audio.service)
hot() { for t in /proc/$P/task/*; do echo "$(basename $t) $(awk '{print $14+$15}' $t/stat)"; done; }
hot > /data/local/tmp/c1; sleep 3; hot > /data/local/tmp/c2
echo "hot threads:"; paste /data/local/tmp/c1 /data/local/tmp/c2 | awk '{d=($4-$2)*100/300; if (d>3) printf "  tid %s %.0f%%\n", $1, d}'
rm -f $D/dump.raw
sed -i "s/^dump=.*/dump=1/" $D/miceq.conf; sleep 3
echo "stall 1: aec off (tap reopen)"; sed -i "s/^aec=.*/aec=0/" $D/miceq.conf; sleep 4
echo "stall 2: aec on (tap reopen)";  sed -i "s/^aec=.*/aec=1/" $D/miceq.conf; sleep 4
echo "stall 3: ns off (tap reopen)";  sed -i "s/^ns=.*/ns=0/" $D/miceq.conf; sleep 4
echo "stall 4: ns on (tap reopen)";   sed -i "s/^ns=.*/ns=1/" $D/miceq.conf; sleep 4
sed -i "s/^dump=.*/dump=0/" $D/miceq.conf; sleep 0.5
cp $D/dump.raw /data/local/tmp/repro.raw; chmod 644 /data/local/tmp/repro.raw; rm -f $D/dump.raw
ls -la /data/local/tmp/repro.raw
logcat -d -s miceq:* | grep -E "tap|E miceq" | tail -8
