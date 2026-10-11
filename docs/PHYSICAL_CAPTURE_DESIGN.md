# Physical CPU/memory-bus capture design

**Status:** architecture and proposed userspace ABI. The repository does **not** yet contain a driver for a specific PCIe/FPGA capture device. The current `kernel/staticbus.c` is a software-fed byte FIFO; it cannot probe CPU package pins or sample a DDR channel by itself.

## 1. Define the target precisely

“CPU bus” can mean two very different things:

1. **CPU-internal fabric**: core-to-core interconnects, internal cache/coherence traffic, and private on-die buses. These are not generally exposed on external connector pins and cannot be captured by an ordinary Linux module. Access requires a platform/vendor-supported debug/trace feature or specialized silicon validation equipment.
2. **External memory interface**: DDR signals between the CPU's integrated memory controller and DIMM/on-board DRAM. These are physical high-speed signals and can be captured only with suitable probes, an interposer, and a DDR-generation-compatible protocol analyzer or a custom FPGA capture appliance.

This design targets the second category. It will not claim to capture internal CPU buses. If the real target is on-die CPU trace, select a vendor-supported trace/debug facility instead of the capture-card driver described here.

## 2. Hardware requirements

### Recommended first prototype

Use a DDR protocol analyzer/interposer rated for the exact machine's memory generation, channel topology, data rate, and DIMM form factor. Verify that the analyzer exposes raw samples or decoded events via a documented host API. A protocol analyzer is the lowest-risk way to prove the electrical capture and decoding path before writing a Linux PCIe driver.

### Custom capture appliance

A custom FPGA design must provide all of the following:

- **Physical access:** a compatible DIMM interposer/test fixture or motherboard-specific probe points; correct impedance, termination, voltage thresholds, ground references, short probe connections, and channel mapping.
- **Adequate analog/sample front-end:** sampling and timing resolution for the target data rate and both DDR edges; per-lane deskew and calibration where required. Commodity GPIO pins are not a DDR protocol analyzer.
- **Hardware buffering:** on-FPGA block RAM and/or attached capture memory so the hardware can continue sampling while Linux is busy.
- **A loss-accountable format:** monotonically increasing block and/or sample sequence numbers; a hardware timestamp/counter; hardware FIFO overrun counters; clock/PLL lock status; optional per-block CRC; trigger markers and a resynchronization indicator.
- **A documented host interface:** preferably PCIe with a DMA engine and MSI-X or MSI completion interrupts, BAR-mapped control/status registers, and a stable hardware register specification. USB/Ethernet may work for slower decoded records but are not a default choice for raw high-rate samples.
- **Defined firmware/register ABI:** vendor/device IDs, BAR layouts, reset semantics, DMA descriptor format, interrupt causes/acknowledgement behavior, supported sample formats, and buffer ownership transitions. These cannot be guessed generically by a kernel driver.

For high-speed physical DDR signals, the probe/interposer and analog capture path are the hard part. An FPGA plus a PCIe endpoint is not sufficient without a validated signal front-end. The probe itself can perturb signal integrity; validate the target system and capture quality with the analyzer vendor's setup procedure.

## 3. Proposed architecture

```text
DDR wires (CPU memory controller <-> DIMM)
      |
generation-compatible probes / interposer
      |
FPGA sampler + calibrated clock + trigger + sequence numbers
      |
on-card FIFO/capture RAM + overrun/clock/CRC status
      |
PCIe DMA engine writes completed blocks to host buffers
      |                                  MSI-X/MSI per batch
      v                                           |
staticbus-capture kernel driver <-----------------+
      |
driver-owned ring of immutable capture blocks + loss counters
      |
/dev/staticbus-capture0
      | mmap(read-only) + poll(2) + ioctl(2)
      v
C++ GTK frontend: waveforms, decoded events, gaps, overruns, timestamps
```

Keep the existing `/dev/staticbus0` device for synthetic data and replay. Create a distinct `/dev/staticbus-capture0` device for the high-rate, hardware-backed path so the low-rate test ABI does not become overloaded with incompatible framing.

### Kernel driver responsibilities

Implement a **hardware-specific PCIe driver** only after a specific FPGA/analyzer register and DMA specification exists:

1. Enable the PCI device and claim its BARs.
2. Set and check the device's DMA mask with the Linux DMA API; never substitute CPU virtual/physical addresses for DMA addresses.
3. Allocate/map DMA buffers and descriptor rings with the DMA API and perform the documented ownership/memory-barrier transitions.
4. Quiesce/acknowledge the device before enabling interrupts; use `pci_alloc_irq_vectors()` for MSI-X/MSI when supported and `request_threaded_irq()` (or a minimal hard-IRQ plus deferred processing path) for completions.
5. Keep hard-IRQ work bounded: read/ack the interrupt cause, mask or schedule work as the device requires, and queue completed DMA blocks. Never perform protocol decoding or per-sample logging in hard IRQ context.
6. Publish complete blocks with monotonic sequence numbers and release-order synchronization, then wake `poll(2)` readers. Stop safely on host-ring full by default; never silently overwrite an unconsumed block.
7. On stop/remove, halt capture and DMA, synchronize IRQs/deferred work, unmap/free buffers, and only then release BARs and PCI resources.

Linux's DMA API must be used because a device DMA address is not necessarily a CPU physical address, especially with IOMMU translation. PCI MSI/MSI-X and threaded interrupt setup follow the Linux PCI and generic IRQ APIs (links below).

### Where losses can occur

Track loss domains independently. A single generic “dropped bytes” counter cannot tell users which part of the acquisition failed.

| Counter / flag | Meaning |
|---|---|
| `hardware_overruns` / `HW_OVERRUN` | FPGA sampler/FIFO could not retain all sampled data. Lost samples cannot be recovered after the fact unless the appliance retained or can retransmit them. |
| `dma_overruns` / `DMA_OVERRUN` | Hardware completed more DMA blocks than its DMA ring could safely accommodate, or firmware reported a DMA-ring fault. |
| `host_ring_overruns` | The userspace consumer failed to release blocks fast enough. Under the default stop-on-full policy this should stop capture before reusing unread slots. |
| `sequence_gaps` / `SEQUENCE_GAP` | A monotonic hardware/block sequence jumped, proving missing blocks/samples when interpreted using that hardware's sequence semantics. |
| `crc_errors` / `CRC_ERROR` | A block failed a configured checksum. CRC detects many corruptions; it does not reconstruct the original data. |
| `clock_unlocks` / `CLOCK_UNLOCK` | The sampler lost its valid clock/PLL condition; affected intervals must be marked invalid. |
| `RESYNC` | The decoder reacquired a known framing/protocol boundary after a loss or clock fault. |

Do not fill gaps with guessed bytes. A UI may attempt protocol-specific reconstruction only when the protocol has sufficient redundancy, ECC, retransmission, or explicit state to support it; report reconstructed values separately from captured values.

## 4. Kernel/userspace API

The proposed UAPI is in [`include/staticbus_capture_uapi.h`](../include/staticbus_capture_uapi.h). It is a design contract, **not an implemented ABI yet**; a real driver must implement and test it against the selected card's hardware/firmware spec.

Device: `/dev/staticbus-capture0`.

### Control ioctls

| Request | Contract |
|---|---|
| `STATICBUS_CAPTURE_IOC_GET_CAPS` | Return ABI version, formats, maximum block/ring sizes, channel count, feature bits, maximum sample rate. |
| `STATICBUS_CAPTURE_IOC_CONFIGURE` | Configure format, channels, block size, ring depth, trigger settings and sample-rate request while stopped. Reject unsupported values rather than silently clamp. |
| `STATICBUS_CAPTURE_IOC_START` / `STOP` | Start/stop the hardware acquisition and DMA pipeline. |
| `STATICBUS_CAPTURE_IOC_GET_STATS` | Snapshot interrupt, block, byte/sample, overrun, sequence-gap, CRC, clock-unlock and trigger counters. |
| `STATICBUS_CAPTURE_IOC_CONSUME` | Acknowledge the exclusive next sequence number consumed; do not let it move backward or beyond the published producer sequence. |
| `STATICBUS_CAPTURE_IOC_RESET_STATS` | Reset software statistics only where safe; must not erase an unreported hardware-overrun condition. |

All structures carry an ABI version and size. Use fixed-width `__u32` and `__aligned_u64` types. Add new fields only through reserved space or a versioned extension, and test 32-bit compat ioctl/mmap behavior.

### Memory-mapped data path

- `mmap(fd, ..., offset = 0)`: read-only ring metadata plus fixed-size descriptors.
- `mmap(fd, ..., offset = PAGE_SIZE)`: read-only fixed-size payload slots.
- `poll(2)`: readable when at least one complete block is published; error/hangup when the capture device has failed or been removed.
- Descriptor sequence numbers are monotonic. Slot index is `sequence % descriptor_count`; payload slot index uses the same sequence modulo the configured ring size. Consumers must check the sequence before and after reading a descriptor to detect wrap/invalidation.
- `producer_seq` means “next sequence to publish”; `consumer_seq` means “next sequence not yet acknowledged”. The producer must release-publish descriptor and payload contents before advancing the producer sequence. Userspace must acquire-load it before reading them.
- One exclusive capture consumer is the first implementation target. Multi-reader sessions need separately managed consumer positions or separate capture sessions; do not share a single acknowledgement cursor between independent consumers.
- Ring capacity, payload lengths and offsets are validated at configure/mmap time. Never expose device registers or arbitrary DMA addresses to userspace.

### Example userspace flow (pseudocode)

```cpp
int fd = open("/dev/staticbus-capture0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
ioctl(fd, STATICBUS_CAPTURE_IOC_GET_CAPS, &caps);
ioctl(fd, STATICBUS_CAPTURE_IOC_CONFIGURE, &config);

mmap_ring = mmap(nullptr, ring_mapping_bytes, PROT_READ, MAP_SHARED, fd, 0);
mmap_data = mmap(nullptr, data_mapping_bytes, PROT_READ, MAP_SHARED, fd, PAGE_SIZE);

ioctl(fd, STATICBUS_CAPTURE_IOC_START);
while (running) {
    poll(fd, ...);                  // block until completed data/status
    // acquire-load producer_seq; inspect descriptors in sequence order
    // verify per-block flags, sequence and optional CRC; display or save data
    ioctl(fd, STATICBUS_CAPTURE_IOC_CONSUME, &next_unread_sequence);
}
ioctl(fd, STATICBUS_CAPTURE_IOC_STOP);
```

The production C++ client must use proper acquire/release atomic access to shared sequence fields and account for the exact ring/mmap format specified by the implementing driver. The snippet is workflow pseudocode, not a compilable example.

## 5. GTK frontend behavior

Keep the existing byte histogram for synthetic/replay mode. Add a hardware-capture page with:

- raw sampled waveform/edge view (downsampled for rendering without discarding the saved capture);
- decoded command/address/data event table when a matching decoder exists;
- hardware/host timestamps and trigger markers;
- separate counters for hardware FIFO overrun, DMA overrun, host ring full, sequence gaps, CRC failures and clock unlocks;
- an explicit quality/status marker for every interval affected by loss;
- binary capture export preserving block headers, sequence numbers, timestamps and flags.

A histogram or a count of bytes is not enough to prove that a physical bus capture is complete. The GUI must preserve the loss and synchronization metadata.

## 6. Verification plan

1. **API-only tests:** UAPI size/version checks, invalid ioctl values, overflow/underflow of sequence numbers, ring full and consumer-lag tests.
2. **Hardware loopback:** known pattern through capture logic, verify exact sample count, sequence progression, CRC, timestamp monotonicity and trigger position.
3. **Stress:** saturate the DMA/host path and intentionally slow userspace; confirm stop-on-full or explicit overrun markers rather than silent overwrite.
4. **Fault injection:** simulate hardware FIFO overrun, a missing sequence, CRC failure, clock unlock and disconnect; confirm each is visible in stats and block flags.
5. **Signal validation:** compare captured timing/decoded transactions with a known-good compatible protocol analyzer before claiming accuracy.

## References

- [Linux DMA API HOWTO](https://docs.kernel.org/core-api/dma-api-howto.html)
- [Linux PCI driver guide](https://docs.kernel.org/PCI/pci.html)
- [Linux generic IRQ handling](https://docs.kernel.org/core-api/genericirq.html)
- [Linux IRQ tracepoints](https://docs.kernel.org/core-api/tracepoint.html)

Linux IRQ tracepoints are useful for correlating host interrupt timing, but they do not sample the electrical DDR waveform. 
