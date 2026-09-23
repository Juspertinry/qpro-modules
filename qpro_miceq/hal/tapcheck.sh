# is the tap thread alive and feeding Android? dump 2 s of what apps get, and
# per-thread CPU with the suppressor on and off
D=/data/local/tmp/qpro_miceq
P=$(pidof android.hardware.audio.service)
tcpu() { for t in /proc/$P/task/*; do echo "$(basename $t) $(awk '{print $14+$15}' $t/stat)"; done; }
rm -f $D/dump.raw; sed -i "s/^dump=.*/dump=1/" $D/miceq.conf; sleep 3; sed -i "s/^dump=.*/dump=0/" $D/miceq.conf; sleep 0.5
cp $D/dump.raw /data/local/tmp/dcheck.raw; chmod 644 /data/local/tmp/dcheck.raw; ls -la /data/local/tmp/dcheck.raw 2>&1
sed -i "s/^ns=.*/ns=1/" $D/miceq.conf; sleep 2; tcpu > /data/local/tmp/t1; sleep 5; tcpu > /data/local/tmp/t2
sed -i "s/^ns=.*/ns=0/" $D/miceq.conf; sleep 2; tcpu > /data/local/tmp/t3; sleep 5; tcpu > /data/local/tmp/t4
sed -i "s/^ns=.*/ns=1/" $D/miceq.conf
echo "tid  ns-on%  ns-off%"; paste /data/local/tmp/t1 /data/local/tmp/t2 /data/local/tmp/t3 /data/local/tmp/t4 | awk '{on=($4-$2)*100/500; off=($8-$6)*100/500; if (on>1||off>1) printf "%s %5.1f %5.1f\n",$1,on,off}'
