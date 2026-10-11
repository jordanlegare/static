# Software-only capture mode (no external probes)

**Goal:** monitor what the computer's CPU, memory subsystem, and Linux kernel expose to software, using built-in CPU features and kernel interfaces. No external probes, FPGA, interposer, or capture card is required.

**Important limit:** this mode does not record the electrical DDR data/address/command wires and cannot reconstruct literal bytes lost on a physical memory bus. CPU execution trace, memory-controller performance counters, interrupt tracepoints, and memory-error reports are distinct observability sources. They must be labeled accurately in the UI.

## Architecture

~~~text
Built-in machine observability
  ├─ tracefs/ftrace: IRQ entry/exit and other available kernel events
  ├─ perf_event_open: supported core PMU events
  ├─ CPU trace: Intel PT when advertised and permitted
  ├─ uncore PMUs: memory-controller/cache/bus counters when supported
  └─ EDAC/RAS/MCE: hardware-reported memory and machine-check errors
             |
    software-only collector (userspace preferred)
             |
     normalized event stream + loss/availability metadata
             |
     GTK 4 / C++ frontend
       ├─ interrupt timeline
       ├─ CPU trace availability and trace loss
       ├─ memory bandwidth / supported counters
       └─ EDAC/MCE error counters and events
~~~

## Sources and what they actually mean

### 1. Interrupts and kernel tracepoints

Use tracefs/ftrace events exposed by the running kernel, including irq:irq_handler_entry and irq:irq_handler_exit where available. Enumerate actual names in /sys/kernel/tracing/available_events; event availability varies by kernel configuration and architecture.

The events describe host interrupt activity, handler identity and timing. They are not the bytes that happened to travel on an external CPU-memory interface.

A custom kernel module is usually unnecessary for consuming tracepoints: Linux's event tracing infrastructure supports enabling available trace events through tracefs. A module should be reserved for a platform-specific requirement that cannot be met through supported interfaces.

### 2. CPU execution trace

On compatible Intel systems, Intel Processor Trace (Intel PT) can record highly compressed software execution-flow information, including control flow, execution mode and timing metadata, through Linux perf. It does not provide a raw dump of all memory values or DDR bus cycles. Feature support and permissions must be checked at runtime.

Other processors may expose other vendor-specific trace or sampling features. The collector must discover and label each supported capability rather than assuming Intel PT is available.

### 3. Performance monitoring counters (PMU)

Use perf_event_open or the perf tool for architectural core counters and any uncore/memory-controller PMUs registered by the running kernel. Depending on the CPU/platform, these can expose event counts such as cycles, cache events, memory bandwidth, or read/write transaction counts.

Uncore events are platform-specific. Some expose aggregate counts only, do not support sampling, or can be measured only system-wide. Treat unavailable or unsupported events as unavailable; do not substitute a different counter and keep the same label. PMU counts do not expose every read/write data byte.

### 4. Memory errors and RAS

Read supported EDAC sysfs counters under /sys/devices/system/edac/, and consume available RAS/MCE trace or log events when configured. A hardware/driver combination may report correctable and uncorrectable memory errors, error syndromes, channels, or DIMM locations.

This provides error telemetry, not a universal detector for every electrical disturbance. Many non-ECC systems report little or no per-DIMM error information, and errors not detected/reported by the platform cannot be inferred merely from a counter sample.

## Recommended implementation for staticbus

Keep the existing /dev/staticbus0 FIFO for synthetic-data testing and replay, but do not make it the live source for software-only telemetry. Add a separate C++ collector library/backend that gathers native telemetry from supported Linux APIs and provides normalized events to the GTK UI.

Suggested normalized record:

~~~cpp
struct TelemetryRecord {
    uint64_t monotonic_timestamp_ns;
    uint32_t source;       // tracefs, perf, Intel PT, uncore PMU, EDAC/RAS
    uint32_t event_id;     // stable ID within the source adapter
    uint32_t flags;        // loss, unsupported, partial, hardware-reported error
    uint32_t payload_size;
    // source-specific, versioned payload follows
};
~~~

This is an internal design sketch, not an existing public ABI. Preserve each source's native payload and metadata when useful; avoid casting arbitrary bytes from unrelated sources into a fake bus stream.

### Collector behavior

1. Discover interfaces at startup (e.g. tracefs event list, /sys/bus/event_source/devices/, CPU feature flags and EDAC sysfs).
2. Expose the exact source capabilities and permissions in the UI.
3. Use supported system calls/APIs rather than directly guessing MSR addresses or reading undocumented registers.
4. Track loss per source: perf lost samples/AUX truncation, trace-buffer overruns, consumer queue drops, and hardware-reported error counts. A trace-buffer overrun is not a lost DDR byte.
5. Use a bounded userspace event queue; surface its own drops explicitly.
6. Save source name, CPU/platform identification, event configuration and kernel version with every capture so measurements can be reproduced.

### Security and overhead

Access to performance counters and kernel tracing may be restricted by perf_event_paranoid, tracefs permissions, lockdown policy or container configuration. Prefer least privilege; do not disable kernel security settings globally just to make the GUI work. Intel PT and high-volume tracing can produce large data volumes and add runtime overhead. Start with bounded interrupt/error telemetry and selected PMU counters, then enable execution tracing only when available and needed.

## Check what this computer exposes

Run these commands locally:

~~~sh
lscpu
perf list | grep -Ei 'intel_pt|ibs|uncore|imc|ddr|memory|cache'
find /sys/bus/event_source/devices -maxdepth 1 -mindepth 1 -type d -printf '%f\n'
cat /sys/kernel/tracing/available_events 2>/dev/null | grep -E '^(irq|ras|edac|mce):'
find /sys/devices/system/edac -maxdepth 4 -type f 2>/dev/null
~~~

Not every path or event exists on every machine. The inventory determines which backends staticbus can enable on the current computer.

## Verification and interpretation

- Check event counts against /proc/interrupts or another independent host counter when appropriate.
- Confirm PMU units, scope (per-core/system-wide), multiplexing and availability before interpreting values.
- Verify trace loss through each source's documented counters.
- For EDAC, validate the interface and current counters rather than intentionally injecting memory faults on a production system.
- Label dashboards as **CPU execution trace**, **interrupts**, **memory-controller counters**, **trace loss**, or **hardware-reported memory errors**. Do not label these as raw DDR samples.

## References

- [Linux perf: Intel Processor Trace](https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf-intel-pt.txt)
- [Linux perf stat and PMU event syntax](https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf-stat.txt)
- [Linux RAS / EDAC documentation](https://docs.kernel.org/admin-guide/RAS/main.html)
- [Linux event tracing documentation](https://docs.kernel.org/6.3/trace/events.html)
