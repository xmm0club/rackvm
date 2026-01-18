#!/bin/sh
set -eu

output=${1:-examples/assets/initramfs}
busybox=${BUSYBOX:-$(command -v busybox || true)}

if [ -z "$busybox" ]; then
    echo "RackVM: A statically linked BusyBox executable is required." >&2
    exit 1
fi

if ldd "$busybox" 2>&1 | grep -q '=>'; then
    echo "RackVM: $busybox is dynamically linked; use a static BusyBox build." >&2
    exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

mkdir -p "$work/bin" "$work/dev" "$work/etc" "$work/proc" "$work/sys" "$work/tmp" "$(dirname "$output")"
cp "$busybox" "$work/bin/busybox"
for applet in sh mount uname cat echo clear poweroff reboot dmesg ls ps free grep awk setsid; do
    ln -s busybox "$work/bin/$applet"
done
cp guest/init "$work/init"
chmod 0755 "$work/init"

(cd "$work" && find . -print0 | cpio --null -o --format=newc 2>/dev/null) | gzip -9 > "$output"
echo "Created $output"
