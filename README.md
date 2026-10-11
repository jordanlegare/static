# staticbus

`staticbus` is a Linux-kernel character-device bridge and GTK 4 desktop monitor for byte streams produced by an external bus-capture source. The kernel module exposes `/dev/staticbus0`; a trusted capture program writes captured bytes to the device, and the C++ GTK frontend reads and visualizes them.

> **Hardware limitation:** a normal Linux kernel module cannot passively read arbitrary electrical CPU package/bus pins on a modern PC. Most CPU-internal buses are not exposed as host-readable interfaces. Capturing physical bus activity requires compatible external hardware (such as an FPGA/logic analyzer) and a producer/driver that converts its samples into bytes. This project provides the kernel-to-GTK streaming path; it is not itself a physical bus probe or a CPU instrumentation tool.

## Features

- Kernel module with a bounded FIFO and `/dev/staticbus0` character device.
- Blocking writes with backpressure when the reader falls behind; nonblocking reads for a responsive GUI.
- `poll(2)` support and an `ioctl(2)` statistics API.
- C++ GTK 4 frontend with a rolling byte-frequency histogram, recent hexadecimal bytes, queue occupancy, byte counters, Shannon entropy, and average changed bits per byte.
- Binary capture replay utility for testing without capture hardware.
- Build/install/unload steps documented below.

## Architecture

```text
Bus capture hardware (FPGA / logic analyzer)
             |
    hardware-specific producer
             | write(2) byte stream
             v
        /dev/staticbus0  <--- staticbus Linux kernel module
             | read(2) + STATICBUS_IOC_GET_STATS
             v
       staticbus-gtk (C++ / GTK 4)
```

The device is a byte FIFO, not a packet protocol: write boundaries are not preserved, and reads return whatever number of bytes is currently available. Use your capture source's own framing information if sample boundaries matter.

## Requirements

- Linux kernel headers for the running kernel (`linux-headers-$(uname -r)` on Debian/Ubuntu).
- A C/C++ toolchain, `make`, and `pkg-config`.
- GTK 4 development files (`libgtk-4-dev` on Debian/Ubuntu).
- Permissions to load kernel modules and open the character device.

On Debian/Ubuntu:

```sh
sudo apt update
sudo apt install build-essential pkg-config linux-headers-$(uname -r) libgtk-4-dev
```

## Build

From the repository root:

```sh
make
```

Or build parts individually:

```sh
make -C kernel
make -C frontend
make -C tools
```

The kernel module is built against `/lib/modules/$(uname -r)/build`. If your target kernel differs, pass `KDIR=/path/to/kernel/build` to `make -C kernel`.

## Load the kernel module

```sh
sudo insmod kernel/staticbus.ko
ls -l /dev/staticbus0
```

The default FIFO is 64 KiB. To request a different capacity (the module FIFO rounds to a supported capacity), pass a `buffer_size` parameter between 4096 and 16777216 bytes:

```sh
sudo insmod kernel/staticbus.ko buffer_size=262144
```

Check kernel messages with `dmesg | tail`. The module creates a character device with the device-node permissions supplied by the system's devtmpfs/udev rules. For desktop use, configure a dedicated udev rule and group rather than running the GUI as root. Example rule (review the group choice for your host):

```text
# /etc/udev/rules.d/70-staticbus.rules
KERNEL=="staticbus0", GROUP="plugdev", MODE="0660"
```

Then reload rules and re-trigger the device, or unload/reload the module. Confirm the actual group exists on your distribution before applying the rule.

## Run the GTK frontend

```sh
./frontend/staticbus-gtk
```

The default device is `/dev/staticbus0`. Override it with either form:

```sh
./frontend/staticbus-gtk --device /dev/staticbus0
# or
STATICBUS_DEVICE=/dev/staticbus0 ./frontend/staticbus-gtk
```

The UI reconnects periodically if the device is not yet available. “Pause view” pauses the displayed history while continuing to drain the kernel FIFO so a producer is not stalled; “Clear view” clears only the current visualization, not kernel counters or buffered data.

## Smoke test without hardware

Generate a small binary test stream and replay it into the device while the GUI is running:

```sh
python3 - <<'PY'
from pathlib import Path
# Repeated ranges plus deterministic noisy bytes for a visible histogram.
Path("sample.bin").write_bytes(bytes(range(256)) * 8 + bytes((i * 73 + 19) % 256 for i in range(2048)))
PY
sudo ./tools/staticbus-replay /dev/staticbus0 sample.bin
```

`staticbus-replay` reads a binary file and writes it to the kernel FIFO. It applies backpressure if the GTK frontend is not draining bytes. The replay utility requires write permission on the device; use a group/udev rule where appropriate instead of making the device world-writable.

For a quick one-off test without the replay tool, another process can write binary data to `/dev/staticbus0`. Do not send terminal text to a live hardware device unless that is intended.

## Kernel/userspace API

Header: [`include/staticbus_ioctl.h`](include/staticbus_ioctl.h)

| Interface | Meaning |
|---|---|
| `open("/dev/staticbus0", O_WRONLY)` | Open the producer side. |
| `write(fd, bytes, length)` | Enqueue raw captured bytes. Blocking when the FIFO is full; partial writes are possible. |
| `open("/dev/staticbus0", O_RDONLY)` | Open the consumer side. |
| `read(fd, buffer, length)` | Drain available bytes. Returns `-EAGAIN` for an empty nonblocking read. |
| `poll(2)` | Reports readable data and writable FIFO capacity. |
| `ioctl(fd, STATICBUS_IOC_GET_STATS, &stats)` | Obtain ABI version, queue capacity/occupancy, bytes written, and bytes read. |

`struct staticbus_stats` is the stable versioned stats structure. The ioctl is declared with `_IOR('S', 0x01, struct staticbus_stats)`. The frontend uses the same public header, avoiding a private layout duplicated in C++.

### Data access and safety

The character device is a privileged interface: processes with write access can submit arbitrary bytes to the stream, and a slow or absent reader can backpressure blocking writers. Restrict access to a trusted group. The module does not access physical memory, DMA, model-specific registers, or undocumented CPU interfaces. It only buffers byte sequences submitted through its device node.

## Unload and clean

Close the frontend and stop producers, then unload the module:

```sh
sudo rmmod staticbus
make clean
```

## Troubleshooting

- **`/dev/staticbus0` missing:** check `sudo dmesg | tail`, confirm the module is loaded with `lsmod | grep staticbus`, and inspect your devtmpfs/udev setup.
- **`insmod: Invalid module format`:** build against the exact target kernel headers and check `uname -r`.
- **Permission denied:** check the device's owner/group/mode; do not default to world-writable permissions.
- **The GUI shows no bytes:** start a producer or replay a capture file. The module does not synthesize physical bus data by itself.
- **Build failure in GTK frontend:** ensure `pkg-config --modversion gtk4` succeeds and install GTK 4 development headers.

## Status

Prototype/reference implementation. Test the module against your target kernel before deployment. A real bus acquisition workflow still needs a hardware-specific capture source and a documented sample format (including clocking, width, edge selection, and any framing metadata).
