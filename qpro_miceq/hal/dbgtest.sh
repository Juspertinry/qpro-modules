D=/data/local/tmp/qpro_miceq; T=/data/local/tmp
sed -i "s/^aec=.*/aec=1/; s/^ns=.*/ns=1/; s/^dump=.*/dump=0/" $D/miceq.conf; grep -q '^debug=' $D/miceq.conf || echo debug=1 >> $D/miceq.conf; sed -i "s/^debug=.*/debug=1/" $D/miceq.conf
sleep 4; echo "--- selectors:"; tinymix -D 0 "MIC TO CHANNEL0" | cut -c1-22; tinymix -D 0 "MIC TO CHANNEL1" | cut -c1-22
logcat -c
tinymix -D 0 "PRI_TDM_RX_0 Audio Mixer MultiMedia2" 1
rm -f $D/dump.raw; $T/pcmplay 0 1 $T/pink.wav & P=$!; sleep 1.5; sed -i "s/^dump=.*/dump=2/" $D/miceq.conf; sleep 4; sed -i "s/^dump=.*/dump=0/" $D/miceq.conf; wait $P
tinymix -D 0 "PRI_TDM_RX_0 Audio Mixer MultiMedia2" 0
sleep 3
echo "--- levels during noise then quiet:"; logcat -d -s miceq:* | grep -E "levels|E miceq" | tail -14
mv $D/dump.raw $D/tap6.raw; ls -la $D/tap6.raw
sed -i "s/^debug=.*/debug=0/" $D/miceq.conf
