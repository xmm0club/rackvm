#!/bin/sh
set -eu

rackvm=$1
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

"$rackvm" --version | grep -q '^RackVM '
"$rackvm" --help | grep -q 'devirtualise'

dd if=/dev/zero of="$temporary/vmlinuz" bs=1 count=4096 status=none
printf '\125\252' | dd of="$temporary/vmlinuz" bs=1 seek=510 conv=notrunc status=none
printf 'HdrS' | dd of="$temporary/vmlinuz" bs=1 seek=514 conv=notrunc status=none
printf '\006\002' | dd of="$temporary/vmlinuz" bs=1 seek=518 conv=notrunc status=none
printf '\001' | dd of="$temporary/vmlinuz" bs=1 seek=529 conv=notrunc status=none
printf '\001' | dd of="$temporary/vmlinuz" bs=1 seek=566 conv=notrunc status=none
printf 'payload' >> "$temporary/vmlinuz"
printf 'initramfs' > "$temporary/initramfs"

sed "s|KERNEL|$temporary/vmlinuz|; s|INITRD|$temporary/initramfs|" > "$temporary/rackvm.conf" <<'EOF'
name = "RackVM Check"
kernel = "KERNEL"
initrd = "INITRD"
memory = 128
cpus = 2
cmdline = "console=ttyS0 panic=-1"
interactive = false
EOF

"$rackvm" validate "$temporary/rackvm.conf" | grep -q 'Configuration is valid.'
"$rackvm" inspect "$temporary/vmlinuz" | grep -q 'Architecture:      x86-64'
"$rackvm" verify "$temporary/rackvm.conf" | grep -q 'Guest image is bootable by RackVM.'
sed 's/cpus = 2/cpus = 64/' "$temporary/rackvm.conf" > "$temporary/rackvm-64.conf"
"$rackvm" verify "$temporary/rackvm-64.conf" | grep -q 'vCPUs:      64'
"$rackvm" devirtualise "$temporary/rackvm.conf" --output "$temporary/bare-metal" | grep -q 'Bare-metal bundle created'
test -s "$temporary/bare-metal/vmlinuz"
test -s "$temporary/bare-metal/initramfs"
test -s "$temporary/bare-metal/grub.cfg"
test -s "$temporary/bare-metal/manifest.json"
test -s "$temporary/bare-metal/SHA256SUMS"
(cd "$temporary/bare-metal" && sha256sum -c SHA256SUMS >/dev/null)
grep -q 'rackvm-bare-metal-v1' "$temporary/bare-metal/manifest.json"

echo "All RackVM checks passed."
