#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
binary=${RACKVM_BINARY:-$root/build/rackvm}
kernel=${1:-/tmp/rackvm-guest/vmlinuz-virt}
initrd=${2:-/tmp/rackvm-guest/initramfs-virt}
font=${RACKVM_VGA_FONT:-/home/rack/.local/share/fonts/OldschoolPC/PxPlus_IBM_VGA_8x16.ttf}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

mkdir -p "$root/demo"

sed "s|KERNEL|$kernel|; s|INITRD|$initrd|" > "$work/showcase.conf" <<'EOF'
name = "Alpine Showcase"
kernel = "KERNEL"
initrd = "INITRD"
memory = 512
cpus = 4
cmdline = "console=ttyS0,115200n8 earlyprintk=serial,ttyS0,115200 panic=-1 reboot=k"
interactive = true
EOF

{
    printf '\033[1;32mrack@host\033[0m:\033[1;34m~/rackvm\033[0m$ ./build/rackvm --version\n'
    "$binary" --version
    printf '\n\033[1;32mrack@host\033[0m:\033[1;34m~/rackvm\033[0m$ ./build/rackvm inspect alpine/vmlinuz-virt\n'
    "$binary" inspect "$kernel" | sed "s|$kernel|alpine/vmlinuz-virt|"
} > "$work/frame1.ansi"

{
    printf '\033[1;32mrack@host\033[0m:\033[1;34m~/rackvm\033[0m$ ./build/rackvm verify showcase.conf\n'
    "$binary" verify "$work/showcase.conf" | sed "s|$kernel|alpine/vmlinuz-virt|; s|$initrd|alpine/initramfs-virt|"
    printf '\n\033[1;36mBoot layout complete: kernel + initramfs + SMP table\033[0m\n'
} > "$work/frame2.ansi"

{
    printf '\033[1;32mrack@host\033[0m:\033[1;34m~/rackvm\033[0m$ ./build/rackvm devirtualise showcase.conf \\\n'
    printf '  --output ./bare-metal\n'
    "$binary" devirtualise "$work/showcase.conf" --output "$work/bare-metal" | sed "s|$work/bare-metal|./bare-metal|"
    printf '\n\033[1;32mrack@host\033[0m:\033[1;34m~/rackvm\033[0m$ ls -lh bare-metal\n'
    find "$work/bare-metal" -maxdepth 1 -type f -printf '%-16f %6k KiB\n' | sort
    printf '\n\033[1;36mVM configuration exported for physical hardware.\033[0m\n'
} > "$work/frame3.ansi"

{
    printf '\033[1;32mrack@host\033[0m:\033[1;34m~/rackvm\033[0m$ make check\n'
    (cd "$root" && make check) | tail -n 2
    printf '\n\033[1;32mrack@host\033[0m:\033[1;34m~/rackvm\033[0m$ ./build/rackvm --help\n'
    "$binary" --help | sed -n '1,13p'
} > "$work/frame4.ansi"

for number in 1 2 3 4; do
    ansi2txt < "$work/frame$number.ansi" > "$work/frame$number.txt" 2>/dev/null || sed 's/\x1b\[[0-9;]*m//g' "$work/frame$number.ansi" > "$work/frame$number.txt"
    magick -size 640x360 xc:'#080c12' \
        -fill '#151c27' -draw 'rectangle 0,0 639,30' \
        -fill '#ff5f57' -draw 'circle 16,15 21,15' \
        -fill '#febc2e' -draw 'circle 34,15 39,15' \
        -fill '#28c840' -draw 'circle 52,15 57,15' \
        -font "$font" -pointsize 14 -fill '#9aa8b8' -gravity north \
        -annotate +0+8 'RackVM — 76×19' \
        -gravity northwest -pointsize 16 -fill '#d8dee9' -interline-spacing 1 \
        -annotate +16+46 "@$work/frame$number.txt" \
        -fill none -stroke '#283444' -strokewidth 1 -draw 'rectangle 0,0 639,359' \
        "$work/frame$number.png"
done

magick -delay 240 "$work/frame1.png" -delay 260 "$work/frame2.png" -delay 280 "$work/frame3.png" -delay 320 "$work/frame4.png" -loop 0 -layers Optimize "$root/demo/rackvm.gif"
printf 'Created %s\n' "$root/demo/rackvm.gif"
