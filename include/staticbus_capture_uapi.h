/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Proposed high-rate hardware-capture UAPI for staticbus.
 *
 * This is an ABI contract for a future hardware-backed capture driver. It is
 * intentionally separate from staticbus_ioctl.h, which describes the current
 * software-fed /dev/staticbus0 FIFO. No hardware PCIe driver implements this
 * header yet; see docs/PHYSICAL_CAPTURE_DESIGN.md.
 */
#ifndef STATICBUS_CAPTURE_UAPI_H
#define STATICBUS_CAPTURE_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define STATICBUS_CAPTURE_ABI_VERSION 1U
#define STATICBUS_CAPTURE_IOC_MAGIC 'C'

/* Capabilities feature bits. */
#define STATICBUS_CAP_HW_SEQUENCE       (1ULL << 0)
#define STATICBUS_CAP_HW_TIMESTAMP      (1ULL << 1)
#define STATICBUS_CAP_PAYLOAD_CRC32     (1ULL << 2)
#define STATICBUS_CAP_HW_OVERRUN_COUNT  (1ULL << 3)
#define STATICBUS_CAP_RAW_SAMPLES       (1ULL << 4)
#define STATICBUS_CAP_DECODED_RECORDS   (1ULL << 5)
#define STATICBUS_CAP_TRIGGER           (1ULL << 6)
#define STATICBUS_CAP_CLOCK_STATUS      (1ULL << 7)

/* Capture payload formats. Payload details are hardware/profile-specific. */
#define STATICBUS_FMT_PACKED_SAMPLES    1U
#define STATICBUS_FMT_DECODED_RECORDS   2U

/* Configure default behavior to stop rather than silently overwrite data. */
#define STATICBUS_CFG_STOP_ON_RING_FULL (1U << 0)
#define STATICBUS_CFG_HW_TIMESTAMP      (1U << 1)
#define STATICBUS_CFG_PAYLOAD_CRC32     (1U << 2)

/* Per-block status flags. A flag marks uncertainty; it never fabricates data. */
#define STATICBUS_BLOCK_HW_OVERRUN      (1U << 0)
#define STATICBUS_BLOCK_DMA_OVERRUN     (1U << 1)
#define STATICBUS_BLOCK_SEQUENCE_GAP    (1U << 2)
#define STATICBUS_BLOCK_CRC_ERROR       (1U << 3)
#define STATICBUS_BLOCK_TRIGGER_START   (1U << 4)
#define STATICBUS_BLOCK_TRIGGER_END     (1U << 5)
#define STATICBUS_BLOCK_CLOCK_UNLOCK    (1U << 6)
#define STATICBUS_BLOCK_RESYNC          (1U << 7)

#define STATICBUS_STATE_STOPPED         0U
#define STATICBUS_STATE_RUNNING         1U
#define STATICBUS_STATE_RING_FULL       2U
#define STATICBUS_STATE_HW_ERROR        3U

/* mmap offsets, expressed in pages (vm_pgoff). Both views are read-only. */
#define STATICBUS_CAPTURE_MMAP_RING_PGOFF 0U
#define STATICBUS_CAPTURE_MMAP_DATA_PGOFF 1U

struct staticbus_capture_caps {
    __u32 abi_version;
    __u32 struct_size;
    __u32 max_channels;
    __u32 sample_formats;       /* bitmask: 1U << STATICBUS_FMT_* */
    __u32 max_block_bytes;
    __u32 min_ring_blocks;
    __u32 max_ring_blocks;
    __u32 reserved0;
    __aligned_u64 feature_flags;
    __aligned_u64 max_sample_rate_hz;
    __aligned_u64 reserved[4];
};

struct staticbus_capture_config {
    __u32 abi_version;
    __u32 struct_size;
    __u32 sample_format;
    __u32 channel_mask;
    __u32 block_bytes;
    __u32 ring_blocks;
    __u32 flags;
    __u32 trigger_mode;         /* Hardware-defined; zero means disabled. */
    __aligned_u64 sample_rate_hz;
    __aligned_u64 reserved[4];
};

/*
 * Ring header resides at mmap offset 0, followed by descriptor_count
 * descriptors. producer_seq is the next sequence to be published; consumer_seq
 * is the next sequence not yet acknowledged by userspace. Use acquire loads for
 * producer_seq before reading a descriptor/payload.
 */
struct staticbus_capture_ring {
    __u32 abi_version;
    __u32 descriptor_count;
    __u32 descriptor_size;
    __u32 slot_size;
    __aligned_u64 producer_seq;
    __aligned_u64 consumer_seq;
    __u32 state;
    __u32 reserved0;
    __aligned_u64 reserved[4];
};

/*
 * Descriptor N is at descriptors[N % descriptor_count], payload slot N is at
 * data[N * slot_size]. sequence disambiguates wrap-around. A slot is valid only
 * after producer_seq has advanced past its sequence.
 */
struct staticbus_capture_desc {
    __aligned_u64 sequence;
    __aligned_u64 first_sample_index;
    __aligned_u64 hardware_timestamp_ticks;
    __u32 payload_bytes;
    __u32 flags;
    __u32 hardware_sequence;
    __u32 payload_crc32;
    __aligned_u64 reserved[2];
};

struct staticbus_capture_stats {
    __u32 abi_version;
    __u32 struct_size;
    __aligned_u64 interrupts;
    __aligned_u64 dma_completed_blocks;
    __aligned_u64 captured_bytes;
    __aligned_u64 captured_samples;
    __aligned_u64 hardware_overruns;
    __aligned_u64 dma_overruns;
    __aligned_u64 host_ring_overruns;
    __aligned_u64 sequence_gaps;
    __aligned_u64 crc_errors;
    __aligned_u64 clock_unlocks;
    __aligned_u64 trigger_events;
    __aligned_u64 last_hardware_timestamp_ticks;
    __aligned_u64 reserved[4];
};

/* next_sequence is exclusive: acknowledge every block with sequence < value. */
struct staticbus_capture_consume {
    __u32 abi_version;
    __u32 struct_size;
    __aligned_u64 next_sequence;
    __aligned_u64 reserved[2];
};

#define STATICBUS_CAPTURE_IOC_GET_CAPS \
    _IOR(STATICBUS_CAPTURE_IOC_MAGIC, 0x10, struct staticbus_capture_caps)
#define STATICBUS_CAPTURE_IOC_CONFIGURE \
    _IOW(STATICBUS_CAPTURE_IOC_MAGIC, 0x11, struct staticbus_capture_config)
#define STATICBUS_CAPTURE_IOC_START \
    _IO(STATICBUS_CAPTURE_IOC_MAGIC, 0x12)
#define STATICBUS_CAPTURE_IOC_STOP \
    _IO(STATICBUS_CAPTURE_IOC_MAGIC, 0x13)
#define STATICBUS_CAPTURE_IOC_GET_STATS \
    _IOR(STATICBUS_CAPTURE_IOC_MAGIC, 0x14, struct staticbus_capture_stats)
#define STATICBUS_CAPTURE_IOC_CONSUME \
    _IOW(STATICBUS_CAPTURE_IOC_MAGIC, 0x15, struct staticbus_capture_consume)
#define STATICBUS_CAPTURE_IOC_RESET_STATS \
    _IO(STATICBUS_CAPTURE_IOC_MAGIC, 0x16)

#endif /* STATICBUS_CAPTURE_UAPI_H */
