# Shared by customize.sh, post-fs-data.sh and action.sh. Expects MODDIR to be set.

TARGET=/system_ext/lib64/libmrengine.so
PATCHED=$MODDIR/patched/libmrengine.so
# outside MODDIR so module updates don't reset it
STATE=/data/adb/qpro_grayscale.enabled

# One line per supported build: stock sha, offset of the b.ne that keeps
# non-Quest 2 devices off the mono-only FrameSetConsumer table, patched sha.
# Anything else is refused; a stale patch would crash-loop mrsystemservice.
BUILDS="
fe833a588b4a504eb514cc0c9f763350f4a3207870cc7af28b03e3f52505a1bf 0x698874 7cf44a0a370dc4a2d988e90aa36494785f19c10668d96c80fdb910141b8e776b 51503870024400340
80949678b503532ebf440b431a7bb6d19fa783520ed2c09c336d49f918321d42 0x6ca8d4 a50a7ff4326020ab32c5703b73b43dadd73447029c47155481caf72beee0251b 51463340027700340
69452af74cce5c9e2df6bc252086a9895f3ec3026189b2c648ed9ed93be94839 0x68c694 e630ed9b8783470f9600148fb02f365acbce97b320060d48c13096b9ee3d46af 51483620032400340
"

# action.sh may not run in the global mount namespace, so mount ops go through init's
# (toybox nsenter mis-parses -t, so name the ns file directly)
gmnt() { nsenter --mount=/proc/1/ns/mnt -- "$@"; }

lib_sha() { gmnt sha256sum "$TARGET" | cut -d' ' -f1; }

# sets STOCK_SHA/PATCH_OFF/PATCHED_SHA for the build whose stock or patched sha is $1
find_build() {
    local line
    line=$(echo "$BUILDS" | grep -- "$1")
    [ -n "$line" ] || return 1
    set -- $line
    STOCK_SHA=$1 PATCH_OFF=$2 PATCHED_SHA=$3
}

is_mounted() { find_build "$(lib_sha)" && [ "$(lib_sha)" = "$PATCHED_SHA" ]; }

# rebuilds the patched copy from the device's own stock lib (Meta's isn't shipped)
make_patched() {
    find_build "$(lib_sha)" || return 1
    [ "$(sha256sum "$PATCHED" 2>/dev/null | cut -d' ' -f1)" = "$PATCHED_SHA" ] && return 0
    mkdir -p "${PATCHED%/*}"
    gmnt cat "$TARGET" > "$PATCHED"
    if [ "$(lib_sha)" = "$STOCK_SHA" ]; then
        printf '\037\040\003\325' | dd of="$PATCHED" bs=1 seek=$((PATCH_OFF)) conv=notrunc 2>/dev/null
    fi
    [ "$(sha256sum "$PATCHED" | cut -d' ' -f1)" = "$PATCHED_SHA" ] || return 1
    chcon u:object_r:system_lib_file:s0 "$PATCHED"
}

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
