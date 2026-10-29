# Shared by post-fs-data.sh and action.sh. Expects MODDIR to be set.

TARGET=/system_ext/lib64/libmrengine.so
PATCHED=$MODDIR/patched/libmrengine.so
# outside MODDIR so module updates don't reset it
STATE=/data/adb/qpro_grayscale.enabled

# Patched lib is only valid against this exact stock build. After an OTA the
# offsets move, and a stale lib would crash-loop mrsystemservice.
STOCK_SHA=fe833a588b4a504eb514cc0c9f763350f4a3207870cc7af28b03e3f52505a1bf
PATCHED_SHA=7cf44a0a370dc4a2d988e90aa36494785f19c10668d96c80fdb910141b8e776b

# action.sh may not run in the global mount namespace, so mount ops go through init's
# (toybox nsenter mis-parses -t, so name the ns file directly)
gmnt() { nsenter --mount=/proc/1/ns/mnt -- "$@"; }

lib_sha() { gmnt sha256sum "$TARGET" | cut -d' ' -f1; }

is_mounted() { [ "$(lib_sha)" = "$PATCHED_SHA" ]; }

mount_gray() {
    chcon u:object_r:system_lib_file:s0 "$PATCHED"
    gmnt mount -o bind "$PATCHED" "$TARGET"
}

# plain umount fails with EBUSY while mrsystemservice has the lib mapped
umount_gray() {
    while is_mounted; do
        gmnt umount -l "$TARGET" || return 1
    done
}
