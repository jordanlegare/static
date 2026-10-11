# staticbus

**staticbus** is a Linux/C++ project with two GTK 4 applications:

1. **Byte-stream monitor** — visualizes bytes written to the `/dev/staticbus0` character device by a producer or replay tool.
2. **Software telemetry monitor** — reads selected CPU/memory-controller performance counters and host telemetry exposed by Linux, without external capture probes.

The repository also contains a binary replay utility, a small kernel character-device module, a shared ioctl header, and design documents for possible future physical-bus capture hardware.

> **Important scope limitation:** this code does not passively sample raw electrical CPU/DDR bus signals. A Linux module cannot generically access internal CPU buses or recover exact bytes that were lost on a physical bus. The no-probe telemetry app reports the machine's available counters and error telemetry; the byte-stream app displays data submitted to its character device. The hardware-backed capture interface described later is a proposal, not an implemented driver.

## Current capabilities

### 1. Linux byte-stream device

The `kernel/staticbus.c` module creates `/dev/staticbus0`, backed by a bounded kernel FIFO. A producer writes arbitrary bytes to the device; a consumer reads them.

Implemented interfaces:

- Blocking `write(2)` with backpressure when the FIFO is full.
- Nonblocking `read(2)` support; an empty nonblocking read returns `EAGAIN`.
- `poll(2)` readiness for readable data and writable FIFO space.
- `STATICBUS_IOC_GET_STATS` to return ABI version, FIFO capacity, current occupancy, cumulative bytes written and cumulative bytes read.
- Configurable FIFO size through the `buffer_size` module parameter (4 KiB through 16 MiB requested size, subject to kernel FIFO sizing).

The device is a **raw byte FIFO**, not a framed protocol. Write boundaries are not preserved; the reader receives whatever amount is currently available. The module does not acquire bus samples itself, decode transactions, use DMA, register a hardware interrupt handler, or inspect CPU-internal signals.

### 2. GTK byte-stream monitor

`frontend/staticbus-gtk.cpp` is the GTK 4/C++ viewer for `/dev/staticbus0`. It:

- Reconnects periodically when the device is unavailable.
- Reads the device without blocking the GTK event loop.
- Draws a byte-frequency histogram over a rolling 2,048-byte window.
- Displays recent data as hexadecimal bytes.
- Reports kernel FIFO occupancy/capacity and cumulative byte counters through the stats ioctl.
- Computes Shannon entropy over the rolling byte window.
- Computes the average number of changed bits between adjacent bytes in the displayed stream.
- Provides **Pause view**, which pauses updating the displayed history while the app continues draining the FIFO, and **Clear view**, which clears its local visualization.

The GUI is a general byte-stream viewer. Entropy and bit-transition measurements describe the submitted byte stream; they are not, by themselves, proof of electrical noise or missing bus data.

### 3. No-probe software telemetry monitor

`frontend/staticbus-telemetry-gtk.cpp` is a separate GTK 4/C++ app. It uses Linux interfaces and counters advertised by the current host:

- **Memory-controller PMU counters:** attempts to open `uncore_imc/data_reads` and `uncore_imc/data_writes` through `perf_event_open(2)`. It reads event `.scale` and `.unit` metadata when exported by sysfs and applies the scale to the measured rate. The exact meaning and unit depend on the host's PMU event definition.
- **CPU cache misses:** attempts to count the standard hardware cache-misses event across online CPUs, subject to kernel permissions and PMU availability.
- **Aggregate host interrupts:** samples the total interrupt count in `/proc/stat` and displays its change per second. This is an aggregate count, not a per-interrupt trace or a byte stream.
- **EDAC memory error counters:** reads `ce_count` and `ue_count` from active EDAC memory-controller directories when those files exist.
- **Intel Processor Trace availability:** checks whether `/sys/bus/event_source/devices/intel_pt` exists. The app does **not** start, save, decode or analyze Intel PT recordings.

The telemetry app reports unavailable sources when it cannot discover or open them. Event availability and interpretation depend on the CPU, firmware, kernel, PMU driver and permissions. A PMU counter can estimate activity or traffic, but it does not expose each raw DDR data byte.

The no-probe frontend does not currently collect tracefs interrupt events, perform live Intel PT capture, or provide automatic recovery/reconstruction of missing data. Access to performance events can be restricted by `perf_event_paranoid` and other system security settings.

### 4. Binary replay utility

`tools/staticbus-replay.cpp` reads a binary file and writes it to the selected character device. It handles partial writes and interrupted system calls. It is useful for testing the byte-stream viewer without capture hardware.

Usage:

```sh
./tools/staticbus-replay DEVICE CAPTURE.bin
```

For example, with the module loaded and correct device permissions:

```sh
./tools/staticbus-replay /dev/staticbus0 sample.bin
```

The utility does not parse capture file formats; it copies the file's bytes as-is.

## Architecture

### Byte-stream path

```text
binary file / trusted producer
             |
             | write(2)
             v
       /dev/staticbus0
       kernel FIFO module
             |
             | read(2) + GET_STATS ioctl
             v
       staticbus-gtk
       GTK 4 byte viewer
```

### No-probe telemetry path

```text
Linux perf PMUs (when available) -----+
/proc/stat aggregate interrupt count -+--> staticbus-telemetry-gtk
EDAC sysfs error counters ------------+    GTK 4 telemetry dashboard
Intel PT PMU availability check ------+
```

These are two separate data paths and two separate applications. The telemetry app does not send PMU data through `/dev/staticbus0`.

## Repository layout

| Path | Purpose | Status |
|---|---|---|
| `kernel/staticbus.c` | Character-device FIFO module | Implemented |
| `include/staticbus_ioctl.h` | ABI for `/dev/staticbus0` stats ioctl | Implemented |
| `frontend/staticbus-gtk.cpp` | GTK byte-stream viewer | Implemented |
| `frontend/staticbus-telemetry-gtk.cpp` | Native no-probe PMU/host telemetry dashboard | Implemented prototype |
| `telemetry-web/server.js` | Local Node.js server, perf JSON collector, SSE updates and host telemetry | Implemented prototype |
| `telemetry-web/public/` | Responsive live line charts and status panels | Implemented prototype |
| `telemetry-web/package.json` | Node.js start and syntax-check scripts | Implemented |
| `tools/staticbus-replay.cpp` | Binary file-to-device replay tool | Implemented |
| `include/staticbus_capture_uapi.h` | Proposed high-throughput hardware capture UAPI | Design only |
| `docs/SOFTWARE_ONLY_CAPTURE.md` | No-probe telemetry architecture and limits | Documentation |
| `docs/PHYSICAL_CAPTURE_DESIGN.md` | Optional external capture hardware design | Documentation only |
| `Makefile` and component Makefiles | Build and clean targets | Implemented |

## Requirements

For Debian/Ubuntu:

```sh
sudo apt update
sudo apt install build-essential g++ make pkg-config \
    linux-headers-$(uname -r) libgtk-4-dev
```

- A running Linux system and kernel headers matching the target kernel for the kernel module.
- A C++17 compiler, GNU make and `pkg-config`.
- GTK 4 development files for both GUI applications.
- Permission to load kernel modules for the byte-stream device.
- Permission to open the requested perf events for the software telemetry app.

You do not need to load the kernel module to run `staticbus-telemetry-gtk`; it reads the host's perf/sysfs/`/proc/stat` sources directly.

## Build

Build all current components from the repository root:

```sh
make
```

This builds the kernel module, both GTK applications, and the replay utility. The kernel component uses `/lib/modules/$(uname -r)/build` by default.

Build only the telemetry GUI (no kernel headers or module load needed):

```sh
make -C frontend staticbus-telemetry-gtk
```

Build the byte-stream GUI:

```sh
make -C frontend staticbus-gtk
```

Build the replay utility:

```sh
make -C tools
```

If the module should target a different kernel build tree, pass `KDIR` to the kernel make invocation, for example:

```sh
make -C kernel KDIR=/path/to/kernel/build
```

Clean build products:

```sh
make clean
```

## Live web dashboard (Node.js)

For a responsive browser-based interface with dedicated live line charts, use the optional dependency-free Node.js dashboard in `telemetry-web/`. It runs locally, starts `perf stat` at one-second intervals, and streams updates to the page using Server-Sent Events. The charts show memory-controller reads and writes using perf's reported event unit/scale, CPU cache-miss rate, and aggregate interrupt rate. Separate status fields report Intel PT availability and EDAC error counters when exposed.

Requirements: Node.js 18 or later, Linux `perf`, and permission to open the selected perf events. No `npm install` is needed.

```sh
cd telemetry-web
npm start
# Open http://127.0.0.1:8090 in your browser
```

Or run `node server.js` directly. The service binds to `127.0.0.1` by default; override the port with `STATICBUS_WEB_PORT=8091` if needed. Use `PERF_BIN` to select a non-default perf executable. The Start/Stop buttons control the `perf stat` collector. Stop the server with Ctrl+C.

The dashboard is intentionally localhost-only and has no external JavaScript or charting dependencies. It does not expose a general command execution API. Perf's JSON interval output supplies PMU samples; `/proc/stat` supplies aggregate interrupt counts; EDAC values are read from sysfs. A chart reflects these sampled metrics, not individual DRAM transactions or raw DDR signal bytes. If counters do not appear, review the diagnostic panel and run the same `perf stat` command from a terminal with the required permissions.

Check source syntax with:

```sh
npm run check
```

## Run: software-only telemetry

Launch the no-probe monitor:

```sh
./frontend/staticbus-telemetry-gtk
```

The dashboard attempts to open PMU events exported by the running kernel and samples the aggregate interrupt count and EDAC counters. If a source is missing or not permitted, its panel reports that the source is unavailable. The Intel PT panel only reports whether a PMU is present; it does not perform a recording.

### Verify memory-controller PMU access

The actual event aliases are host-specific. To test the same type of counters shown by the frontend, run:

```sh
perf list | grep -Ei 'intel_pt|uncore_imc|cache-misses'
sudo perf stat -a -I 1000 \
  -e uncore_imc/data_reads/,uncore_imc/data_writes/ \
  -- sleep 5
```

If `perf stat` works with elevated privileges but the GUI does not, the GUI may lack permission to open the system-wide events. Do not run the entire desktop session as root just to work around perf permissions; follow your distribution's security and access-control policy instead.

The exported PMU event `.scale` and `.unit` files describe how the platform's event value is represented. Do not interpret a counter as exact physical bytes unless the event documentation defines that unit and scope.

## Run: byte-stream device

Load the module after building it:

```sh
sudo insmod kernel/staticbus.ko
ls -l /dev/staticbus0
dmesg | tail
```

The default requested FIFO size is 64 KiB. To request a different size between 4 KiB and 16 MiB:

```sh
sudo insmod kernel/staticbus.ko buffer_size=262144
```

The kernel FIFO may round the requested size to a supported capacity; check the module log or stats ioctl for the actual capacity. Device-node ownership and permissions depend on the host's devtmpfs/udev configuration. For normal desktop use, prefer a dedicated group/udev rule with minimal permissions instead of running the GUI as root or making the device world-writable.

Launch the viewer:

```sh
./frontend/staticbus-gtk
```

The viewer uses `/dev/staticbus0` by default. A different path can be selected with either:

```sh
./frontend/staticbus-gtk --device /dev/staticbus0
# or
STATICBUS_DEVICE=/dev/staticbus0 ./frontend/staticbus-gtk
```

The option is useful for testing with another compatible character device, but the viewer expects the staticbus stats ioctl for its kernel statistics panel.

## Smoke test the byte-stream viewer

After loading the module, create a repeatable binary pattern:

```sh
python3 - <<'PY'
from pathlib import Path
pattern = bytes(range(256)) * 8
noisy = bytes((i * 73 + 19) % 256 for i in range(2048))
Path("sample.bin").write_bytes(pattern + noisy)
print("Wrote sample.bin")
PY
```

Then open the GTK byte viewer and replay the file:

```sh
./frontend/staticbus-gtk
```

In another terminal:

```sh
./tools/staticbus-replay /dev/staticbus0 sample.bin
```

Use `sudo` for the replay command only if the device permissions require it. The replay writer blocks when the kernel FIFO is full until the consumer frees space.

## Current kernel/userspace API

Header: [`include/staticbus_ioctl.h`](include/staticbus_ioctl.h)

The existing device supports:

| Interface | Behavior |
|---|---|
| `open("/dev/staticbus0", O_WRONLY)` | Open producer side |
| `write(fd, data, length)` | Enqueue bytes; blocking with backpressure unless opened nonblocking |
| `open("/dev/staticbus0", O_RDONLY)` | Open consumer side |
| `read(fd, buffer, length)` | Consume available bytes; an empty nonblocking read returns `EAGAIN` |
| `poll(2)` | Readable when data exists; writable while FIFO has room |
| `ioctl(fd, STATICBUS_IOC_GET_STATS, &stats)` | Retrieve ABI version, FIFO capacity/occupancy and cumulative byte counts |

`struct staticbus_stats` includes `abi_version`, `buffer_capacity`, `buffer_used`, `bytes_written` and `bytes_read`. The ioctl uses `_IOR('S', 0x01, struct staticbus_stats)`.

The current device API does not implement capture configuration, hardware trigger controls, DMA, a memory-mapped capture ring, raw CPU trace acquisition, or recovery commands.

## Future physical-bus capture design

`include/staticbus_capture_uapi.h` defines a **proposed** interface for a potential hardware-backed high-throughput capture driver, including capability discovery, capture configuration, start/stop, status counters, block descriptors and consume acknowledgement. No PCIe/FPGA driver currently implements that header, and the current `staticbus.ko` does not use it.

Physical capture of an external DDR interface requires compatible acquisition hardware/interposer and a validated high-speed probe front end. CPU-internal buses are generally not exposed to normal software. See [`docs/PHYSICAL_CAPTURE_DESIGN.md`](docs/PHYSICAL_CAPTURE_DESIGN.md) for the design constraints and loss-detection model.

## Limitations and data interpretation

- A memory-controller PMU counter is not a dump of raw DDR bytes or transaction contents.
- Aggregate interrupt counts from `/proc/stat` do not identify a particular IRQ handler and do not represent lost bus bytes.
- Discovering an Intel PT PMU does not mean the frontend is collecting an Intel PT trace.
- EDAC counters appear only if the platform and kernel driver expose them; no counters does not prove the absence of memory errors.
- The byte viewer only analyzes data written into `/dev/staticbus0`. It does not synthesize data from CPU hardware or discover lost bytes.
- Entropy, histograms and bit transitions can characterize an input stream, but they cannot by themselves determine why a byte is unusual.
- Missing data cannot be reconstructed reliably unless an underlying source or protocol provides enough redundancy, ECC, retransmission, or other recovery information.

## Troubleshooting

- **`/dev/staticbus0` is missing:** check `lsmod | grep staticbus`, `dmesg | tail`, and your devtmpfs/udev setup.
- **Module build fails:** install headers matching `uname -r`; use `KDIR=/path/to/kernel/build` only when that tree matches the target kernel.
- **Telemetry counters say unavailable:** inspect `perf list`, `/sys/bus/event_source/devices/` and `/proc/sys/kernel/perf_event_paranoid`. Permissions and event aliases vary by host.
- **Memory counters work under `sudo perf stat` but not in the GUI:** the GUI process may lack perf permissions. Avoid running the whole desktop as root.
- **EDAC reports no active counters:** the running kernel/platform may not expose an EDAC memory-controller instance.
- **Byte viewer shows no data:** run a producer or `staticbus-replay`; the kernel module does not generate bus samples itself.
- **GTK build fails:** confirm `pkg-config --modversion gtk4` succeeds and that GTK 4 development headers are installed.

## Project status

This repository currently contains a prototype byte-stream FIFO module, two GTK 4 frontends, a binary replay utility, a shared stats ioctl header, and documentation/design artifacts. The software telemetry frontend is an initial implementation and does not yet collect tracefs event streams or record Intel PT data. Hardware-backed physical-bus capture remains design-only.

Kernel code is marked SPDX GPL-2.0; the userspace source files are marked SPDX GPL-2.0-or-later; the current ioctl header uses GPL-2.0 with the Linux syscall-note exception. Review the individual source headers for applicable licensing.
