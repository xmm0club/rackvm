#!/bin/sh
set -eu

rackvm=$1
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

expect_failure()
{
    pattern=$1
    shift
    if "$@" >"$temporary/failure.log" 2>&1; then
        echo "expected command to fail: $*" >&2
        return 1
    fi
    grep -Fq "$pattern" "$temporary/failure.log"
}

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
dd if=/dev/zero of="$temporary/disk.img" bs=1M count=1 status=none

sed "s|KERNEL|$temporary/vmlinuz|; s|INITRD|$temporary/initramfs|" > "$temporary/rackvm.conf" <<'EOF'
name = "RackVM Check"
kernel = "KERNEL"
initrd = "INITRD"
disk = "DISK"
memory = 128
cpus = 2
cmdline = "console=ttyS0 panic=-1"
interactive = false
EOF
sed -i "s|DISK|$temporary/disk.img|" "$temporary/rackvm.conf"

"$rackvm" validate "$temporary/rackvm.conf" | grep -q 'Configuration is valid.'
"$rackvm" validate "$temporary/rackvm.conf" | grep -q "Disk:       $temporary/disk.img"
"$rackvm" inspect "$temporary/vmlinuz" | grep -q 'Architecture:      x86-64'
"$rackvm" verify "$temporary/rackvm.conf" | grep -q 'Guest image is bootable by RackVM.'
sed 's/cpus = 2/cpus = 64/' "$temporary/rackvm.conf" > "$temporary/rackvm-64.conf"
"$rackvm" verify "$temporary/rackvm-64.conf" | grep -q 'vCPUs:      64'

sed 's/memory = 128/memory = plenty/' "$temporary/rackvm.conf" > "$temporary/bad-memory.conf"
expect_failure "invalid value for 'memory'" "$rackvm" validate "$temporary/bad-memory.conf"
sed 's/interactive = false/interactive = perhaps/' "$temporary/rackvm.conf" > "$temporary/bad-bool.conf"
expect_failure "invalid value for 'interactive'" "$rackvm" validate "$temporary/bad-bool.conf"
printf 'kernel = "%s"\nunknown = true\n' "$temporary/vmlinuz" > "$temporary/unknown-key.conf"
expect_failure "unknown key 'unknown'" "$rackvm" validate "$temporary/unknown-key.conf"
printf 'kernel = "%s\n' "$temporary/vmlinuz" > "$temporary/unterminated.conf"
expect_failure 'unterminated quoted value' "$rackvm" validate "$temporary/unterminated.conf"
sed 's|disk = .*|disk = "/missing/rackvm-disk"|' "$temporary/rackvm.conf" > "$temporary/missing-disk.conf"
expect_failure '/missing/rackvm-disk' "$rackvm" validate "$temporary/missing-disk.conf"

cp "$temporary/vmlinuz" "$temporary/oversized-vmlinuz"
printf '\012\002' | dd of="$temporary/oversized-vmlinuz" bs=1 seek=518 conv=notrunc status=none
printf '\377\377\377\177' | dd of="$temporary/oversized-vmlinuz" bs=1 seek=608 conv=notrunc status=none
sed "s|$temporary/vmlinuz|$temporary/oversized-vmlinuz|" "$temporary/rackvm.conf" > "$temporary/oversized-kernel.conf"
expect_failure 'Kernel does not fit in guest memory' "$rackvm" verify "$temporary/oversized-kernel.conf"

cp "$temporary/vmlinuz" "$temporary/constrained-vmlinuz"
printf '\007\000\000\000' | dd of="$temporary/constrained-vmlinuz" bs=1 seek=556 conv=notrunc status=none
sed "s|$temporary/vmlinuz|$temporary/constrained-vmlinuz|" "$temporary/rackvm.conf" > "$temporary/oversized-initrd.conf"
expect_failure 'Initramfs does not fit in guest memory' "$rackvm" verify "$temporary/oversized-initrd.conf"

mkdir "$temporary/real-output"
ln -s "$temporary/real-output" "$temporary/linked-output"
expect_failure "cannot use output path component 'linked-output'" "$rackvm" devirtualise "$temporary/rackvm.conf" --output "$temporary/linked-output/bundle"
test ! -e "$temporary/real-output/bundle"

"$rackvm" devirtualise "$temporary/rackvm.conf" --output "$temporary/bare-metal" | grep -q 'Bare-metal bundle created'
test -s "$temporary/bare-metal/vmlinuz"
test -s "$temporary/bare-metal/initramfs"
test -s "$temporary/bare-metal/grub.cfg"
test -s "$temporary/bare-metal/manifest.json"
test -s "$temporary/bare-metal/SHA256SUMS"
(cd "$temporary/bare-metal" && sha256sum -c SHA256SUMS >/dev/null)
grep -q 'rackvm-bare-metal-v1' "$temporary/bare-metal/manifest.json"

echo "All RackVM checks passed."
