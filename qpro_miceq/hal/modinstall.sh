# Copies a pushed module/ tree into the installed Magisk module in place, so a
# new build persists across reboots without going through the installer.
# usage: modinstall.sh <staged module dir>
S=$1
M=/data/adb/modules/qpro_miceq
[ -f "$S/module.prop" ] || { echo "no module at $S"; exit 1; }
mkdir -p $M/presets $M/licenses
for f in audio.primary.kona.so module.prop miceq.conf service.sh customize.sh uninstall.sh; do
	cp -f $S/$f $M/$f || exit 1
done
cp -f $S/presets/*.conf $M/presets/
cp -f $S/licenses/* $M/licenses/
chmod 755 $M/service.sh $M/customize.sh $M/uninstall.sh
chmod 644 $M/audio.primary.kona.so $M/module.prop $M/miceq.conf $M/presets/* $M/licenses/*
# the live presets follow the module; the live conf is the user's and stays
mkdir -p /data/local/tmp/qpro_miceq/presets
cp -f $S/presets/*.conf /data/local/tmp/qpro_miceq/presets/
chmod 666 /data/local/tmp/qpro_miceq/presets/*.conf
echo "installed: $(grep ^version= $M/module.prop)"
for f in audio.primary.kona.so service.sh miceq.conf presets/studio.conf; do
	echo "  $(md5sum $M/$f | cut -c1-12) $f"
done
grep -c 'start audioserver' $M/service.sh >/dev/null && echo "  service.sh intact"
[ -e $M/disable ] && echo "  module DISABLED" || echo "  module enabled"
