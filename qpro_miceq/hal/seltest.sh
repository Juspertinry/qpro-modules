# Records the raw tap for a few selector pairs so what each DSP dump selector
# puts on slots 0/1 can be checked offline (correlate against the raw mics).
# usage: seltest.sh "3,4" "3,3" ...   (pairs for aec_ref_sel)
D=/data/local/tmp/qpro_miceq; T=/data/local/tmp
for pair in "$@"; do
	n=$(echo $pair | tr -d ',')
	sed -i "s/^aec_ref_sel=.*/aec_ref_sel=$pair/" $D/miceq.conf
	grep -q '^aec_ref_sel=' $D/miceq.conf || echo "aec_ref_sel=$pair" >> $D/miceq.conf
	sleep 2.5
	rm -f $D/dump.raw
	sed -i "s/^dump=.*/dump=2/" $D/miceq.conf; sleep 2.2
	sed -i "s/^dump=.*/dump=0/" $D/miceq.conf; sleep 0.5
	cp $D/dump.raw $T/sel_$n.raw; chmod 644 $T/sel_$n.raw
	echo "$pair: $(stat -c %s $T/sel_$n.raw) bytes, outcfg $($T/dspmem x 0x5ffc0004 1 | tail -1 | sed 's/.* //')"
done
sed -i "s/^aec_ref_sel=.*/aec_ref_sel=3,4/" $D/miceq.conf
logcat -d -s miceq:* | grep -E "selector|config:" | tail -4 | cut -c32-140
