# RackVM

RackVM is a compact x86-64 virtual machine monitor written in C. It uses the Linux KVM API for hardware-assisted CPU execution, creates an in-kernel interrupt controller and programmable timer, loads Linux through the x86 boot protocol, exposes an interactive 16550-compatible serial console, and presents an Intel MultiProcessor table to the guest.

RackVM boots a Linux `bzImage` directly. An initramfs supplies the guest userspace, so a complete appliance can run without emulated storage. The `devirtualise` command exports the same kernel, initramfs, command line, checksums, and a GRUB entry as a bare-metal boot bundle.

## Requirements

- An x86-64 Linux host
- A CPU and kernel with KVM support
- Read and write access to `/dev/kvm`
- A C11 compiler, GNU Make, and Linux userspace headers
- A Linux `bzImage` and, for a useful guest, an initramfs

Run `rackvm doctor` to inspect the host. Membership in the host's `kvm` group is commonly required for unprivileged access to `/dev/kvm`.

## Building

```console
$ make
$ make check
$ sudo make install
```

The binary is written to `build/rackvm`. Installation defaults to `/usr/local/bin/rackvm` and installs the manual page as `rackvm(1)`.

## Creating a Guest

Place a Linux kernel at `examples/assets/vmlinuz`. To create the included BusyBox initramfs, install a statically linked BusyBox executable and run:

```console
$ make demo-initramfs
```

Then boot the example:

```console
$ build/rackvm run examples/rackvm.conf
```

The terminal becomes the guest's serial console. Press Ctrl+C to stop RackVM, or run `poweroff -f` inside the guest.

RackVM can also run without a configuration file:

```console
$ build/rackvm run --kernel ./vmlinuz --initrd ./initramfs --memory 512 --cpus 2 --name Demo
```

## Configuration

Configuration files use one `key = value` pair per line. Relative kernel and initramfs paths are resolved from the configuration file's directory.

```ini
name = "RackVM Showcase"
kernel = "assets/vmlinuz"
initrd = "assets/initramfs"
memory = 512
cpus = 2
cmdline = "console=ttyS0,115200n8 panic=-1 reboot=k rdinit=/init"
interactive = true
```

Memory is measured in MiB. RackVM supports between one and 64 configured vCPUs, subject to the host's KVM limit.

## Inspection and Validation

```console
$ rackvm validate examples/rackvm.conf
$ rackvm verify examples/rackvm.conf
$ rackvm inspect examples/assets/vmlinuz
$ rackvm doctor
```

`validate` checks configuration fields and asset readability. `verify` performs the complete guest-memory layout without starting KVM, including kernel expansion space, initramfs placement, command-line limits, and SMP-table generation. `inspect` decodes the Linux boot-protocol header. `doctor` checks the architecture, KVM API, guest-memory capability, interrupt controller, timer, and vCPU limit.

## Devirtualisation

```console
$ rackvm devirtualise examples/rackvm.conf --output ./rackvm-bare-metal
```

The output directory contains:

- `vmlinuz`
- `initramfs`, when configured
- `grub.cfg`
- `manifest.json`
- `SHA256SUMS`

Copy these files to `/rackvm` on a GRUB-bootable machine, then include the generated `grub.cfg` from the machine's main GRUB configuration. The guest kernel must contain drivers for the target hardware. RackVM preserves the boot artifacts and configuration; it cannot manufacture missing physical-device drivers.

## Security Model

The VMM is intentionally small, but a virtual machine boundary still handles hostile state. Run untrusted guests under a dedicated unprivileged account, apply host security updates, and avoid granting the RackVM process capabilities it does not need. RackVM does not require root when `/dev/kvm` permissions allow access.

## Current Device Model

RackVM provides KVM virtual CPUs, guest RAM, local APICs, an I/O APIC, the legacy interrupt controller, PIT2, an Intel MultiProcessor table, and a userspace 16550 serial device. It does not currently emulate PCI, a framebuffer, disks, or networking. Guest workloads should be packaged into an initramfs and use `console=ttyS0`.
