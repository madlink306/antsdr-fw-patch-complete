/**
 * @file antsdr_dma.c
 * @brief ANTSDR DMA driver with raw data streaming as fallback
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/workqueue.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/completion.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <asm/cacheflush.h>
#include <net/sock.h>
#include <linux/crc32.h>

/* Boolean defines for kernel compatibility */
#ifndef bool
#define bool _Bool
#endif
#ifndef true  
#define true 1
#endif
#ifndef false
#define false 0
#endif

/* Debug configuration */
#define ANTSDR_DEBUG_CONSOLE 0  /* Set to 1 to enable console logging, 0 to disable */

/* Debug logging macro */
#if ANTSDR_DEBUG_CONSOLE
#define antsdr_debug_log(dev, fmt, ...) \
    do { \
        if (dev) { \
            dev_info(dev, "[DEBUG] " fmt, ##__VA_ARGS__); \
        } else { \
            printk(KERN_INFO "antsdr_dma: [DEBUG] " fmt, ##__VA_ARGS__); \
        } \
    } while(0)
#else
#define antsdr_debug_log(...) do { } while(0)
#endif

/* ANTSDR packet protocol definitions */
#define ANTSDR_PROTOCOL_VERSION     1
#define ANTSDR_PACKET_START_MARKER  0xABCD1234
#define ANTSDR_PACKET_END_MARKER    0x5678DCBA
#define ANTSDR_MAX_PAYLOAD_SIZE     1360  /* 1400 - header size */

/* Packet header structure (44 bytes total) */
struct antsdr_packet_header {
    uint32_t start_marker;      /* 0xABCD1234 - packet start identifier */
    uint32_t sequence_number;   /* Incremental packet sequence */
    uint32_t total_length;      /* Total length including header */
    uint32_t payload_length;    /* Data payload length only */
    uint32_t frame_id;          /* DMA frame identifier */
    uint32_t fragment_offset;   /* Offset within original DMA frame */
    uint32_t fragment_count;    /* Total fragments for this frame */
    uint32_t fragment_index;    /* Current fragment index (0-based) */
    uint32_t frame_payload_total; /* Total payload bytes for entire frame */
    uint32_t missing_frame_count; /* Number of missing frames detected */
    uint32_t checksum;          /* CRC32 of payload data */
    uint32_t end_marker;        /* 0x5678DCBA - header end identifier */
} __attribute__((packed));

#define ANTSDR_PACKET_HEADER_SIZE   sizeof(struct antsdr_packet_header)

#define DRIVER_NAME "antsdr_dma"
#define DEVICE_NAME DRIVER_NAME
#define NUM_BUFFERS 16                        /* Increased from 4 to 16 for high-speed buffering */
#define LONG_PULSE_TRANSFER_SIZE (403 * 4)   /* Long pulse: 403 words = 1612 bytes */
#define SHORT_PULSE_TRANSFER_SIZE (53 * 4)   /* Short pulse: 53 words = 212 bytes */
#define MAX_S2MM_TRANSFER_SIZE (512 * 4)     /* Maximum S2MM transfer: 512 words = 2048 bytes */
#define FPGA_LONG_TRANSFER_SIZE (FPGA_LONG_PULSE_WORDS * 4)  /* 403 words = 1612 bytes */
#define FPGA_SHORT_TRANSFER_SIZE (FPGA_SHORT_PULSE_WORDS * 4) /* 53 words = 212 bytes */
#define DEFAULT_BUFFER_SIZE LONG_PULSE_TRANSFER_SIZE  /* Use larger size as default */
#define UDP_BUFFER_SIZE (1024 * 1024)  /* Increased to 1MB for better buffering */
#define UDP_HIGH_WATER_MARK (UDP_BUFFER_SIZE / 2)  /* Trigger aggressive sending at 50% full */
#define UDP_PACKET_SIZE (ANTSDR_PACKET_HEADER_SIZE + ANTSDR_MAX_PAYLOAD_SIZE)  /* Header + payload = 1400 bytes */

/* IOCTL commands */
#define ANTSDR_IOC_MAGIC 'A'
#define ANTSDR_IOC_START_STREAMING  _IO(ANTSDR_IOC_MAGIC, 1)
#define ANTSDR_IOC_STOP_STREAMING   _IO(ANTSDR_IOC_MAGIC, 2)
#define ANTSDR_IOC_SET_UDP_DEST     _IOW(ANTSDR_IOC_MAGIC, 3, struct antsdr_udp_dest)
#define ANTSDR_IOC_GET_STATS        _IOR(ANTSDR_IOC_MAGIC, 4, struct antsdr_dma_stats)
#define ANTSDR_IOC_SET_BUFFER_SIZE  _IOW(ANTSDR_IOC_MAGIC, 5, unsigned int)
#define ANTSDR_IOC_GET_BUFFER_SIZE  _IOR(ANTSDR_IOC_MAGIC, 6, unsigned int)
#define ANTSDR_IOC_SET_PULSE_MODE   _IOW(ANTSDR_IOC_MAGIC, 7, unsigned int)
#define ANTSDR_IOC_SET_TDD_MODE     _IOW(ANTSDR_IOC_MAGIC, 8, unsigned int)
#define ANTSDR_IOC_GET_PULSE_MODE   _IOR(ANTSDR_IOC_MAGIC, 9, unsigned int)
#define ANTSDR_IOC_GET_TDD_MODE     _IOR(ANTSDR_IOC_MAGIC, 10, unsigned int)
#define ANTSDR_IOC_SET_MODE         _IOW(ANTSDR_IOC_MAGIC, 11, unsigned int)
#define ANTSDR_IOC_GET_MODE         _IOR(ANTSDR_IOC_MAGIC, 12, unsigned int)
#define ANTSDR_IOC_RESET_STATS      _IO(ANTSDR_IOC_MAGIC, 13)

/* Ring buffer configuration */
#define RING_BUFFER_COUNT           256     /* Increased from 64 to 256 for high-speed buffering */
#define RING_BUFFER_SIZE            1600    /* Size to accommodate largest payload (long pulse = 1600 bytes) */

/* Raw frame FIFO configuration for threaded processing */
#define RAW_FRAME_FIFO_SIZE         (256 * sizeof(struct antsdr_raw_frame))  /* Increased from 64 to 256 raw frames max */

/* Frame detection buffer configuration */
#define FRAME_DETECTION_BUFFER_SIZE (64 * 1024)  /* 64KB buffer for frame detection */
#define MAX_FRAME_ACCUMULATION      32            /* Maximum frames to accumulate before giving up */

/* Debug and logging configuration */
#define ANTSDR_ENABLE_FRAME_EXPORT  0             /* Enable frame export to files */
#define ANTSDR_MAX_FRAME_EXPORTS    100           /* Maximum number of frames to export */
#define ANTSDR_FRAMES_PER_FILE      20           /* Number of frames per export file */
#define ANTSDR_FRAME_EXPORT_PATH    "/tmp"        /* Export directory */

/* Frame validation constants - FPGA data format */
#define FPGA_HEADER_MARKER_1        0xFEFFFFFF  /* 0xFFFFFFFE in little-endian */
#define FPGA_HEADER_MARKER_2        0xFFFFFFFE  /* 0xFFFFFFFE direct */
#define FPGA_FOOTER_MARKER          0xFFFFFFFF  /* 0xFFFFFFFF */
#define FPGA_LONG_PULSE_WORDS       403         /* Total words in long pulse frame */
#define FPGA_SHORT_PULSE_WORDS      53          /* Total words in short pulse frame */
#define FPGA_LONG_PULSE_PAYLOAD     400         /* Payload words (excluding header/frame_counter/footer) */
#define FPGA_SHORT_PULSE_PAYLOAD    50          /* Payload words (excluding header/frame_counter/footer) */
#define FPGA_LONG_PULSE_BYTES       (FPGA_LONG_PULSE_WORDS * 4)   /* 1612 bytes */
#define FPGA_SHORT_PULSE_BYTES      (FPGA_SHORT_PULSE_WORDS * 4)  /* 212 bytes */
#define FPGA_LONG_PAYLOAD_BYTES     (FPGA_LONG_PULSE_PAYLOAD * 4) /* 1600 bytes */
#define FPGA_SHORT_PAYLOAD_BYTES    (FPGA_SHORT_PULSE_PAYLOAD * 4) /* 200 bytes */

/* Legacy frame constants - kept for potential future use */
#define FRAME_HEADER 0xFFFFFFFE
#define FRAME_FOOTER 0xFFFFFFFF

/* Statistics structure */
struct antsdr_dma_stats {
    unsigned long transfers_completed;
    unsigned long bytes_transferred;
    unsigned long udp_packets_sent;
    unsigned long errors;
    unsigned long valid_frames;
    unsigned long invalid_frames;
    unsigned long extracted_frames;
};

/* UDP destination structure */
struct antsdr_udp_dest {
    unsigned int ip;
    unsigned short port;
};

/* Raw frame data for FIFO processing - using dynamic allocation to prevent stack overflow */
struct antsdr_raw_frame {
    size_t data_len;
    uint8_t *data;  /* Dynamically allocated raw DMA data - prevents stack overflow */
};

/* Device structure */
struct antsdr_dma_dev {
    struct platform_device *pdev;
    struct device *dev;
    struct miscdevice misc_dev;
    struct dma_chan *rx_chan;
    
    /* Separate DMA buffers for different pulse modes */
    void *long_dma_buffers[NUM_BUFFERS];    /* 403×4 = 1612 bytes each */
    dma_addr_t long_dma_handles[NUM_BUFFERS];
    void *short_dma_buffers[NUM_BUFFERS];   /* 52×4 = 208 bytes each */
    dma_addr_t short_dma_handles[NUM_BUFFERS];
    
    size_t buffer_size;
    int current_buffer;
    bool streaming;
    spinlock_t lock;
    wait_queue_head_t wait_queue;
    struct completion dma_complete;
    struct antsdr_dma_stats stats;
    
    /* Ring buffer for high-performance data buffering */
    void **ring_buffers;          /* Array of buffer pointers */
    unsigned int ring_size;       /* Number of buffers in ring */
    unsigned int ring_head;       /* Write position */
    unsigned int ring_tail;       /* Read position */
    unsigned int ring_count;      /* Number of filled buffers */
    size_t ring_buffer_size;      /* Size of each ring buffer */
    spinlock_t ring_lock;         /* Ring buffer synchronization */

    uint32_t operation_mode;  /* 0 or 1 */
    
    /* UDP networking */
    struct socket *sock;
    struct sockaddr_in dest_addr;
    bool dest_set;
    struct work_struct udp_work;
    bool udp_work_pending;  /* Track if UDP work is already scheduled */
    
    /* Frame processing thread and buffer */
    struct workqueue_struct *frame_workqueue;
    struct work_struct frame_work;
    struct kfifo raw_frame_fifo;  /* FIFO for raw DMA data */
    spinlock_t raw_fifo_lock;     /* Protect raw frame FIFO */
    bool frame_work_pending;      /* Track if frame work is already scheduled */
    
    /* Packet protocol tracking */
    uint32_t packet_sequence_number;  /* Incremental packet sequence */
    uint32_t frame_id_counter;        /* DMA frame identifier counter */
    
    /* GPIO controls */
    struct gpio_desc *gpio_enable;
    struct gpio_desc *gpio_pulse_mode;
    struct gpio_desc *gpio_tdd_mode;
    struct gpio_desc *gpio_mode;

    /* Pulse mode settings */
    unsigned int pulse_mode;
    unsigned int tdd_mode;
    unsigned int mode;

    /* Frame detection buffer for cases where header/footer not found */
    void *frame_detection_buffer;     /* Large buffer for frame accumulation */
    size_t frame_buffer_size;         /* Size of frame detection buffer */
    size_t frame_buffer_used;         /* Current used bytes in buffer */
    unsigned int frames_accumulated;  /* Number of frames accumulated */
    spinlock_t frame_buffer_lock;     /* Frame buffer synchronization */

    /* Debug and frame export tracking */
    unsigned int frame_export_count;  /* Number of frames exported */
    unsigned int total_frames_processed; /* Total frames seen for debugging */
    unsigned int frames_in_current_file; /* Frames accumulated in current export file */
    unsigned int missing_frame_count;    /* Total missing frames detected */
    uint32_t last_frame_counter;         /* Last frame counter seen for gap detection */
    bool first_frame_received;           /* Flag to indicate if we've seen the first frame */
    
    /* S2MM transfer length tracking */
    size_t s2mm_requested_length;     /* Requested S2MM transfer length */
    size_t s2mm_actual_length;        /* Actual S2MM transfer length from register */

    /* Note: Frame reassembly no longer needed since we parse complete DMA transfers */
};

/* Function prototypes */
static int antsdr_submit_dma_transfer(struct antsdr_dma_dev *dma_dev);
static size_t antsdr_get_transfer_size(struct antsdr_dma_dev *dma_dev);
/* antsdr_reallocate_buffers removed - buffer sizes now fixed per pulse mode */
static int antsdr_dma_reset_and_restart(struct antsdr_dma_dev *dma_dev);
static int antsdr_parse_fpga_frame(struct antsdr_dma_dev *dma_dev, const uint8_t *data, size_t data_len, uint8_t **payload, size_t *payload_len);
static void antsdr_frame_work(struct work_struct *work);

/* Ring buffer management functions */
static int antsdr_ring_init(struct antsdr_dma_dev *dma_dev);
static void antsdr_ring_cleanup(struct antsdr_dma_dev *dma_dev);
static int antsdr_ring_put(struct antsdr_dma_dev *dma_dev, const void *data, size_t size);
static int antsdr_ring_get(struct antsdr_dma_dev *dma_dev, void **data, size_t *size);
static void antsdr_ring_return_buffer(struct antsdr_dma_dev *dma_dev);

/* Frame detection buffer functions */
static int antsdr_frame_buffer_init(struct antsdr_dma_dev *dma_dev);
static void antsdr_frame_buffer_cleanup(struct antsdr_dma_dev *dma_dev);
static int antsdr_frame_buffer_add(struct antsdr_dma_dev *dma_dev, const void *data, size_t size);
static int antsdr_frame_buffer_process(struct antsdr_dma_dev *dma_dev);
static void antsdr_frame_buffer_reset(struct antsdr_dma_dev *dma_dev);

/* Debug and frame export functions */
static int antsdr_export_frame_to_file(struct antsdr_dma_dev *dma_dev, const uint8_t *data, size_t data_len, const char *frame_type);

/* Parse FPGA frame and extract payload 
 * Returns: 0 on success, -1 on invalid frame
 * Sets payload pointer and payload_len for extracted data
 */
static int antsdr_parse_fpga_frame(struct antsdr_dma_dev *dma_dev, const uint8_t *data, size_t data_len, uint8_t **payload, size_t *payload_len)
{
    const uint32_t *words = (const uint32_t *)data;
    size_t word_count = data_len / 4;
    int header_pos = -1, footer_pos = -1;
    int expected_frame_words;
    uint32_t frame_counter;
    int i;
    
    /* Increment DMA callback counter */
    dma_dev->total_frames_processed++;
    
    antsdr_debug_log(dma_dev->dev, "DMA callback #%u: Parsing %zu bytes (%zu words), pulse_mode=%d\n", 
           dma_dev->total_frames_processed, data_len, word_count, dma_dev->pulse_mode);
    
    /* Export RAW frame to file for manual analysis */
    antsdr_export_frame_to_file(dma_dev, data, data_len, "raw_dma");
    
    /* Determine expected frame size based on pulse mode */
    expected_frame_words = dma_dev->pulse_mode ? FPGA_LONG_PULSE_WORDS : FPGA_SHORT_PULSE_WORDS;
    
    /* Step 1: Find header and footer positions */
    for (i = 0; i < word_count; i++) {
        if (words[i] == FPGA_HEADER_MARKER_1 || words[i] == FPGA_HEADER_MARKER_2) {
            if (header_pos == -1) {
                header_pos = i;
                antsdr_debug_log(dma_dev->dev, "Found header 0x%08x at position %d\n", words[i], i);
            }
        }
        if (words[i] == FPGA_FOOTER_MARKER) {
            footer_pos = i;
            antsdr_debug_log(dma_dev->dev, "Found footer 0x%08x at position %d\n", words[i], i);
        }
    }
    
    /* Step 2: Analyze what we found */
    if (header_pos != -1 && footer_pos != -1) {
        /* Both header and footer found - check frame length */
        int actual_frame_words = footer_pos - header_pos + 1;
        
        antsdr_debug_log(dma_dev->dev, "Complete frame: header@%d, footer@%d, length=%d words (expected %d)\n",
               header_pos, footer_pos, actual_frame_words, expected_frame_words);
        
        if (actual_frame_words == expected_frame_words) {
            /* Valid frame found! */
            
            /* Step 3: Extract frame counter (word before footer) */
            if (footer_pos > header_pos + 1) {
                frame_counter = words[footer_pos - 1];
                
                /* Gap-based missing frame detection */
                if (!dma_dev->first_frame_received) {
                    /* First frame - initialize tracking */
                    dma_dev->last_frame_counter = frame_counter;
                    dma_dev->first_frame_received = true;
                    antsdr_debug_log(dma_dev->dev, "First frame received: counter=%u\n", frame_counter);
                } else {
                    /* Check for gaps in frame counter sequence */
                    uint32_t expected_counter = dma_dev->last_frame_counter + 1;
                    if (frame_counter > expected_counter) {
                        /* Gap detected - frames were missing */
                        uint32_t missing_frames = frame_counter - expected_counter;
                        dma_dev->missing_frame_count += missing_frames;
                        antsdr_debug_log(dma_dev->dev, "Gap detected: expected=%u, received=%u, missing=%u frames, total_missing=%u\n",
                               expected_counter, frame_counter, missing_frames, dma_dev->missing_frame_count);
                    } else if (frame_counter < expected_counter) {
                        /* Frame counter wrapped around or out of order */
                        antsdr_debug_log(dma_dev->dev, "Frame counter anomaly: expected=%u, received=%u\n",
                               expected_counter, frame_counter);
                    }
                    dma_dev->last_frame_counter = frame_counter;
                }
                
                antsdr_debug_log(dma_dev->dev, "Frame counter: %u (DMA callback #%u, total_missing=%u)\n",
                       frame_counter, dma_dev->total_frames_processed, dma_dev->missing_frame_count);
            } else {
                frame_counter = 0;
                antsdr_debug_log(dma_dev->dev, "No frame counter available\n");
            }
            
            /* Step 4: Extract payload (exclude header, footer, and frame counter) */
            if (actual_frame_words >= 3) {  /* header + at least 1 payload word + frame_counter + footer */
                *payload = (uint8_t *)&words[header_pos + 1];  /* Start after header */
                *payload_len = (actual_frame_words - 3) * 4;   /* Exclude header, frame_counter, footer */
                
                antsdr_debug_log(dma_dev->dev, "Valid frame extracted: %zu payload bytes, frame_counter=%u\n", 
                       *payload_len, frame_counter);
                       
                /* Export parsed frame for analysis */
                antsdr_export_frame_to_file(dma_dev, (uint8_t *)&words[header_pos], actual_frame_words * 4, "parsed_valid");
                
                return 0;  /* Success */
            } else {
                antsdr_debug_log(dma_dev->dev, "Frame too short for payload extraction\n");
            }
        } else {
            /* Frame length mismatch - invalid frame, don't add to buffer */
            antsdr_debug_log(dma_dev->dev, "Frame length mismatch: got %d words, expected %d - discarding\n",
                   actual_frame_words, expected_frame_words);
            return -1;
        }
    } else if (header_pos != -1) {
        /* Only header found - add to detection buffer for further analysis */
        antsdr_debug_log(dma_dev->dev, "Only header found at position %d - adding to detection buffer\n", header_pos);
        
        int ret = antsdr_frame_buffer_add(dma_dev, data, data_len);
        if (ret == 0) {
            /* Check if we should process accumulated buffer */
            if (dma_dev->frames_accumulated >= 3 || 
                dma_dev->frame_buffer_used >= (FRAME_DETECTION_BUFFER_SIZE / 2)) {
                antsdr_debug_log(dma_dev->dev, "Processing accumulated frame buffer (%u frames, %zu bytes)\n",
                       dma_dev->frames_accumulated, dma_dev->frame_buffer_used);
                
                /* Try to find complete frames in accumulated buffer */
                int found_frames = antsdr_frame_buffer_process(dma_dev);
                if (found_frames > 0) {
                    antsdr_debug_log(dma_dev->dev, "Found %d complete frames in accumulated buffer\n", found_frames);
                    /* Buffer processing might have found valid frames and queued them for UDP */
                    return 0;
                }
            }
        }
        return -1;  /* Incomplete frame */
    } else {
        /* No header or footer found - skip this frame completely */
        antsdr_debug_log(dma_dev->dev, "No header or footer found - skipping frame\n");
        return -1;
    }
    
    return -1;  /* Should not reach here */
}

/* Get transfer size based on current pulse mode */
static size_t antsdr_get_transfer_size(struct antsdr_dma_dev *dma_dev)
{
    /* Use exact FPGA frame size based on pulse mode for optimal performance */
    if (dma_dev->pulse_mode) {
        return FPGA_LONG_PULSE_WORDS * 4;  /* 403 words = 1612 bytes */
    } else {
        return FPGA_SHORT_PULSE_WORDS * 4; /* 53 words = 212 bytes */
    }
}

/* Get current DMA buffer based on pulse mode */
static void* antsdr_get_current_buffer(struct antsdr_dma_dev *dma_dev)
{
    /* Use appropriate buffer based on pulse mode for optimal performance */
    if (dma_dev->pulse_mode) {
        return dma_dev->long_dma_buffers[dma_dev->current_buffer];
    } else {
        return dma_dev->short_dma_buffers[dma_dev->current_buffer];
    }
}

/* Get current DMA handle based on pulse mode */
static dma_addr_t antsdr_get_current_handle(struct antsdr_dma_dev *dma_dev)
{
    /* Use appropriate handle based on pulse mode for optimal performance */
    if (dma_dev->pulse_mode) {
        return dma_dev->long_dma_handles[dma_dev->current_buffer];
    } else {
        return dma_dev->short_dma_handles[dma_dev->current_buffer];
    }
}

/* Ring buffer management functions */
static int antsdr_ring_init(struct antsdr_dma_dev *dma_dev)
{
    int i;
    
    dma_dev->ring_size = RING_BUFFER_COUNT;
    dma_dev->ring_buffer_size = RING_BUFFER_SIZE;
    dma_dev->ring_head = 0;
    dma_dev->ring_tail = 0;
    dma_dev->ring_count = 0;
    spin_lock_init(&dma_dev->ring_lock);
    
    /* Allocate array of buffer pointers */
    dma_dev->ring_buffers = kzalloc(dma_dev->ring_size * sizeof(void*), GFP_KERNEL);
    if (!dma_dev->ring_buffers) {
        dev_err(dma_dev->dev, "Failed to allocate ring buffer array\n");
        return -ENOMEM;
    }
    
    /* Allocate individual ring buffers */
    for (i = 0; i < dma_dev->ring_size; i++) {
        dma_dev->ring_buffers[i] = kmalloc(dma_dev->ring_buffer_size, GFP_KERNEL);
        if (!dma_dev->ring_buffers[i]) {
            dev_err(dma_dev->dev, "Failed to allocate ring buffer %d\n", i);
            /* Clean up already allocated buffers */
            while (--i >= 0) {
                kfree(dma_dev->ring_buffers[i]);
            }
            kfree(dma_dev->ring_buffers);
            return -ENOMEM;
        }
    }
    
    dev_info(dma_dev->dev, "Ring buffer initialized: %d buffers of %zu bytes each\n", 
             dma_dev->ring_size, dma_dev->ring_buffer_size);
    return 0;
}

static void antsdr_ring_cleanup(struct antsdr_dma_dev *dma_dev)
{
    int i;
    
    if (dma_dev->ring_buffers) {
        for (i = 0; i < dma_dev->ring_size; i++) {
            kfree(dma_dev->ring_buffers[i]);
        }
        kfree(dma_dev->ring_buffers);
        dma_dev->ring_buffers = NULL;
    }
    
    dev_info(dma_dev->dev, "Ring buffer cleaned up\n");
}

static int antsdr_ring_put(struct antsdr_dma_dev *dma_dev, const void *data, size_t size)
{
    unsigned long flags;
    void *ring_buffer;
    unsigned int next_head;
    
    if (size > dma_dev->ring_buffer_size) {
        dev_warn(dma_dev->dev, "Data size %zu exceeds ring buffer size %zu\n", 
                 size, dma_dev->ring_buffer_size);
        return -EINVAL;
    }
    
    spin_lock_irqsave(&dma_dev->ring_lock, flags);
    
    /* Check if ring buffer is full */
    if (dma_dev->ring_count >= dma_dev->ring_size) {
        spin_unlock_irqrestore(&dma_dev->ring_lock, flags);
        dev_warn_ratelimited(dma_dev->dev, "Ring buffer full, dropping data\n");
        return -ENOSPC;
    }
    
    /* Get buffer at head position and copy data */
    ring_buffer = dma_dev->ring_buffers[dma_dev->ring_head];
    memcpy(ring_buffer, data, size);
    
    /* Advance head pointer */
    next_head = (dma_dev->ring_head + 1) % dma_dev->ring_size;
    dma_dev->ring_head = next_head;
    dma_dev->ring_count++;
    
    spin_unlock_irqrestore(&dma_dev->ring_lock, flags);
    
    dev_dbg(dma_dev->dev, "Ring put: %zu bytes, count=%u\n", size, dma_dev->ring_count);
    return 0;
}

static int antsdr_ring_get(struct antsdr_dma_dev *dma_dev, void **data, size_t *size)
{
    unsigned long flags;
    
    spin_lock_irqsave(&dma_dev->ring_lock, flags);
    
    /* Check if ring buffer is empty */
    if (dma_dev->ring_count == 0) {
        spin_unlock_irqrestore(&dma_dev->ring_lock, flags);
        return -ENODATA;
    }
    
    /* Get buffer at tail position */
    *data = dma_dev->ring_buffers[dma_dev->ring_tail];
    *size = dma_dev->ring_buffer_size;
    
    /* Don't advance tail yet - will be done in return_buffer */
    
    spin_unlock_irqrestore(&dma_dev->ring_lock, flags);
    
    dev_dbg(dma_dev->dev, "Ring get: buffer at tail %u, count=%u\n", 
            dma_dev->ring_tail, dma_dev->ring_count);
    return 0;
}

static void antsdr_ring_return_buffer(struct antsdr_dma_dev *dma_dev)
{
    unsigned long flags;
    
    spin_lock_irqsave(&dma_dev->ring_lock, flags);
    
    if (dma_dev->ring_count > 0) {
        /* Advance tail pointer */
        dma_dev->ring_tail = (dma_dev->ring_tail + 1) % dma_dev->ring_size;
        dma_dev->ring_count--;
        
        dev_dbg(dma_dev->dev, "Ring buffer returned, count=%u\n", dma_dev->ring_count);
    }
    
    spin_unlock_irqrestore(&dma_dev->ring_lock, flags);
}

/* Frame detection buffer management functions */
static int antsdr_frame_buffer_init(struct antsdr_dma_dev *dma_dev)
{
    dma_dev->frame_buffer_size = FRAME_DETECTION_BUFFER_SIZE;
    dma_dev->frame_buffer_used = 0;
    dma_dev->frames_accumulated = 0;
    spin_lock_init(&dma_dev->frame_buffer_lock);
    
    dma_dev->frame_detection_buffer = kmalloc(dma_dev->frame_buffer_size, GFP_KERNEL);
    if (!dma_dev->frame_detection_buffer) {
        dev_err(dma_dev->dev, "Failed to allocate frame detection buffer\n");
        return -ENOMEM;
    }
    
    dev_info(dma_dev->dev, "Frame detection buffer initialized: %zu bytes\n", 
             dma_dev->frame_buffer_size);
    return 0;
}

static void antsdr_frame_buffer_cleanup(struct antsdr_dma_dev *dma_dev)
{
    kfree(dma_dev->frame_detection_buffer);
    dma_dev->frame_detection_buffer = NULL;
    dev_info(dma_dev->dev, "Frame detection buffer cleaned up\n");
}

static int antsdr_frame_buffer_add(struct antsdr_dma_dev *dma_dev, const void *data, size_t size)
{
    unsigned long flags;
    int ret = 0;
    
    spin_lock_irqsave(&dma_dev->frame_buffer_lock, flags);
    
    /* Check if we have space */
    if (dma_dev->frame_buffer_used + size > dma_dev->frame_buffer_size) {
        dev_warn(dma_dev->dev, "Frame buffer overflow, resetting\n");
        dma_dev->frame_buffer_used = 0;
        dma_dev->frames_accumulated = 0;
        ret = -ENOSPC;
        goto unlock;
    }
    
    /* Copy data to buffer */
    memcpy((uint8_t *)dma_dev->frame_detection_buffer + dma_dev->frame_buffer_used, 
           data, size);
    dma_dev->frame_buffer_used += size;
    dma_dev->frames_accumulated++;
    
    antsdr_debug_log(dma_dev->dev, "Added %zu bytes to frame buffer (total: %zu bytes, %u frames)\n",
             size, dma_dev->frame_buffer_used, dma_dev->frames_accumulated);

unlock:
    spin_unlock_irqrestore(&dma_dev->frame_buffer_lock, flags);
    return ret;
}

static void antsdr_frame_buffer_reset(struct antsdr_dma_dev *dma_dev)
{
    unsigned long flags;
    
    spin_lock_irqsave(&dma_dev->frame_buffer_lock, flags);
    dma_dev->frame_buffer_used = 0;
    dma_dev->frames_accumulated = 0;
    spin_unlock_irqrestore(&dma_dev->frame_buffer_lock, flags);
    
    dev_info(dma_dev->dev, "Frame detection buffer reset\n");
}

static int antsdr_frame_buffer_process(struct antsdr_dma_dev *dma_dev)
{
    unsigned long flags;
    const uint32_t *words;
    size_t word_count;
    int i, j;
    int frames_found = 0;
    size_t expected_frame_size;
    
    spin_lock_irqsave(&dma_dev->frame_buffer_lock, flags);
    
    if (dma_dev->frame_buffer_used < 8) {
        spin_unlock_irqrestore(&dma_dev->frame_buffer_lock, flags);
        return 0; /* Not enough data */
    }
    
    words = (const uint32_t *)dma_dev->frame_detection_buffer;
    word_count = dma_dev->frame_buffer_used / 4;
    expected_frame_size = (dma_dev->pulse_mode == 0) ? FPGA_SHORT_PULSE_WORDS : FPGA_LONG_PULSE_WORDS;

    antsdr_debug_log(dma_dev->dev, "Processing frame buffer: %zu bytes (%zu words), looking for %s frames\n",
             dma_dev->frame_buffer_used, word_count,
             dma_dev->pulse_mode == 0 ? "short" : "long");
    
    /* Search for valid frame patterns */
    for (i = 0; i < word_count - 1; i++) {
        /* Check for header */
        if (words[i] == FPGA_HEADER_MARKER_1 || words[i] == FPGA_HEADER_MARKER_2) {
            antsdr_debug_log(dma_dev->dev, "Found header 0x%08x at word offset %d\n", words[i], i);
            
            /* Check if we have enough data for a complete frame */
            if (i + expected_frame_size <= word_count) {
                /* Check for footer at expected position */
                int footer_pos = i + expected_frame_size - 1;
                if (words[footer_pos] == FPGA_FOOTER_MARKER) {
                    /* Found complete frame! */
                    uint8_t *payload = (uint8_t *)&words[i + 1]; /* Skip header */
                    size_t payload_len = (expected_frame_size - 2) * 4; /* Skip header and footer */

                    antsdr_debug_log(dma_dev->dev, "Found complete frame at offset %d: header=0x%08x, footer=0x%08x, payload=%zu bytes\n",
                             i, words[i], words[footer_pos], payload_len);
                    
                    /* Queue the extracted payload */
                    int ret = antsdr_ring_put(dma_dev, payload, payload_len);
                    if (ret == 0) {
                        frames_found++;
                        spin_lock_irqsave(&dma_dev->lock, flags);
                        dma_dev->stats.valid_frames++;
                        dma_dev->stats.extracted_frames++;
                        spin_unlock_irqrestore(&dma_dev->lock, flags);
                        
                        /* Schedule UDP work if not already pending */
                        if (!dma_dev->udp_work_pending) {
                            dma_dev->udp_work_pending = true;
                            schedule_work(&dma_dev->udp_work);
                        }
                    }
                    
                    /* Skip past this frame */
                    i += expected_frame_size - 1;
                } else {
                    /* Header found but footer not at expected position, search for footer */
                    int search_limit = min((size_t)(i + expected_frame_size + 10), word_count);
                    for (j = i + 1; j < search_limit; j++) {
                        if (words[j] == FPGA_FOOTER_MARKER) {
                            antsdr_debug_log(dma_dev->dev, "Found misaligned frame: header at %d, footer at %d, size=%d words (expected %zu)\n",
                                     i, j, (j - i + 1), expected_frame_size);
                            break;
                        }
                    }
                }
            } else {
                antsdr_debug_log(dma_dev->dev, "Header found at %d but not enough data for complete frame (need %zu words, have %zu)\n",
                         i, expected_frame_size, word_count - i);
            }
        }
    }
    
    /* Reset buffer after processing */
    dma_dev->frame_buffer_used = 0;
    dma_dev->frames_accumulated = 0;
    
    spin_unlock_irqrestore(&dma_dev->frame_buffer_lock, flags);
    
    antsdr_debug_log(dma_dev->dev, "Frame buffer processing complete: found %d valid frames\n", frames_found);
    return frames_found;
}

/* Debug and frame export functions */

/* Export frame data to file for manual analysis */
static int antsdr_export_frame_to_file(struct antsdr_dma_dev *dma_dev, const uint8_t *data, size_t data_len, const char *frame_type)
{
#if ANTSDR_ENABLE_FRAME_EXPORT
    struct file *file;
    char filename[256];
    loff_t pos = 0;
    int ret = 0;
    const uint32_t *words = (const uint32_t *)data;
    size_t word_count = data_len / 4;
    char *buffer;
    int buffer_size = (word_count * 12) + 2048; /* Larger buffer for frame analysis */
    int written = 0;
    int i;
    bool new_file = false;
    
    /* Check export limit */
    if (dma_dev->frame_export_count >= ANTSDR_MAX_FRAME_EXPORTS) {
        return 0; /* Silently skip */
    }
    
    /* Check if we need to start a new file */
    if (dma_dev->frames_in_current_file == 0) {
        new_file = true;
    }
    
    /* Create filename for new file */
    if (new_file) {
        snprintf(filename, sizeof(filename), "%s/antsdr_frames_%03u_pulse%d.txt", 
                 ANTSDR_FRAME_EXPORT_PATH, 
                 dma_dev->frame_export_count / ANTSDR_FRAMES_PER_FILE,
                 dma_dev->pulse_mode);
    }
    
    /* Allocate buffer for formatted output */
    buffer = kmalloc(buffer_size, GFP_KERNEL);
    if (!buffer) {
        dev_err(dma_dev->dev, "Failed to allocate export buffer\n");
        return -ENOMEM;
    }
    
    /* Format the frame data */
    if (new_file) {
        /* File header for new file */
        written += snprintf(buffer + written, buffer_size - written,
                           "ANTSDR Raw Frame Export - Multiple Frames\n");
        written += snprintf(buffer + written, buffer_size - written,
                           "==========================================\n");
        written += snprintf(buffer + written, buffer_size - written,
                           "Pulse Mode: %d (%s)\n", dma_dev->pulse_mode,
                           dma_dev->pulse_mode ? "long" : "short");
        written += snprintf(buffer + written, buffer_size - written,
                           "S2MM Transfer: %zu bytes (512 words fixed)\n", MAX_S2MM_TRANSFER_SIZE);
        written += snprintf(buffer + written, buffer_size - written,
                           "Expected Frame Length: %d words\n", 
                           dma_dev->pulse_mode ? FPGA_LONG_PULSE_WORDS : FPGA_SHORT_PULSE_WORDS);
        written += snprintf(buffer + written, buffer_size - written, "\n");
    }
    
    /* Frame header */
    written += snprintf(buffer + written, buffer_size - written,
                       "--- FRAME #%u (File Frame #%u) ---\n", 
                       dma_dev->total_frames_processed, 
                       dma_dev->frames_in_current_file + 1);
    written += snprintf(buffer + written, buffer_size - written,
                       "Data Length: %zu bytes (%zu words)\n", data_len, word_count);
    written += snprintf(buffer + written, buffer_size - written,
                       "S2MM Requested: %zu bytes, Actual: %zu bytes\n",
                       dma_dev->s2mm_requested_length, dma_dev->s2mm_actual_length);
    written += snprintf(buffer + written, buffer_size - written, "\n");
    
    /* Add hex dump */
    written += snprintf(buffer + written, buffer_size - written,
                       "HEX DUMP (32-bit words):\n");
    for (i = 0; i < word_count; i++) {
        if (i % 8 == 0) {
            written += snprintf(buffer + written, buffer_size - written,
                               "%04d: ", i);
        }
        written += snprintf(buffer + written, buffer_size - written,
                           "0x%08x ", words[i]);
        if ((i + 1) % 8 == 0) {
            written += snprintf(buffer + written, buffer_size - written, "\n");
        }
        
        /* Safety check */
        if (written >= buffer_size - 200) {
            written += snprintf(buffer + written, buffer_size - written,
                               "\n... truncated ...\n");
            break;
        }
    }
    
    if (word_count % 8 != 0) {
        written += snprintf(buffer + written, buffer_size - written, "\n");
    }
    
    /* Add individual frame analysis */
    written += snprintf(buffer + written, buffer_size - written, "\n");
    written += snprintf(buffer + written, buffer_size - written,
                       "FRAME ANALYSIS:\n");
    
    /* Find headers and footers */
    int header_count = 0, footer_count = 0;
    for (i = 0; i < word_count; i++) {
        if (words[i] == FPGA_HEADER_MARKER_1) {
            written += snprintf(buffer + written, buffer_size - written,
                               "Header 0x%08x at word %d (byte %d)\n",
                               FPGA_HEADER_MARKER_1, i, i * 4);
            header_count++;
        }
        if (words[i] == FPGA_HEADER_MARKER_2) {
            written += snprintf(buffer + written, buffer_size - written,
                               "Header 0x%08x at word %d (byte %d)\n",
                               FPGA_HEADER_MARKER_2, i, i * 4);
            header_count++;
        }
        if (words[i] == FPGA_FOOTER_MARKER) {
            written += snprintf(buffer + written, buffer_size - written,
                               "Footer 0x%08x at word %d (byte %d)\n",
                               FPGA_FOOTER_MARKER, i, i * 4);
            footer_count++;
        }
    }
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Headers found: %d, Footers found: %d\n", header_count, footer_count);
    
    /* Check frame validity */
    if (header_count > 0 && footer_count > 0) {
        written += snprintf(buffer + written, buffer_size - written,
                           "Frame Status: VALID (has header and footer)\n");
    } else {
        written += snprintf(buffer + written, buffer_size - written,
                           "Frame Status: INVALID (missing header or footer)\n");
    }
    
    written += snprintf(buffer + written, buffer_size - written, "\n");
    
    /* Open file for writing (append if not new file) */
    if (new_file) {
        file = filp_open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    } else {
        /* Get filename from previous file */
        snprintf(filename, sizeof(filename), "%s/antsdr_frames_%03u_pulse%d.txt", 
                 ANTSDR_FRAME_EXPORT_PATH, 
                 dma_dev->frame_export_count / ANTSDR_FRAMES_PER_FILE,
                 dma_dev->pulse_mode);
        file = filp_open(filename, O_WRONLY | O_APPEND, 0644);
    }
    
    if (IS_ERR(file)) {
        dev_err(dma_dev->dev, "Failed to open export file %s: %ld\n", 
                filename, PTR_ERR(file));
        kfree(buffer);
        return PTR_ERR(file);
    }
    
    /* For append mode, seek to end */
    if (!new_file) {
        pos = file->f_inode->i_size;
    }
    
    /* Write data to file */
    ret = kernel_write(file, buffer, written, &pos);
    if (ret < 0) {
        dev_err(dma_dev->dev, "Failed to write to export file: %d\n", ret);
    } else {
        antsdr_debug_log(dma_dev->dev, "Exported frame #%u to %s (%d bytes written)\n", 
                 dma_dev->total_frames_processed, filename, ret);
        dma_dev->frame_export_count++;
        dma_dev->frames_in_current_file++;
        
        /* Reset file counter if we've reached the limit */
        if (dma_dev->frames_in_current_file >= ANTSDR_FRAMES_PER_FILE) {
            dma_dev->frames_in_current_file = 0;
        }
    }
    
    /* Clean up */
    filp_close(file, NULL);
    kfree(buffer);
    
    return ret > 0 ? 0 : ret;
#else
    return 0; /* Export disabled */
#endif
}

/* DMA completion callback - optimized for minimal processing */
static void antsdr_dma_callback(void *data)
{
    struct antsdr_dma_dev *dma_dev = (struct antsdr_dma_dev *)data;
    unsigned long flags;
    int ret;
    size_t transfer_size = antsdr_get_transfer_size(dma_dev);
    enum dma_status status;
    struct antsdr_raw_frame raw_frame;
    
    antsdr_debug_log(dma_dev->dev, "DMA callback triggered for buffer %d\n", dma_dev->current_buffer);
    
    /* Check DMA transfer status */
    status = dmaengine_tx_status(dma_dev->rx_chan, 0, NULL);
    if (status == DMA_ERROR) {
        dev_err(dma_dev->dev, "DMA transfer completed with error status\n");
        spin_lock_irqsave(&dma_dev->lock, flags);
        dma_dev->stats.errors++;
        spin_unlock_irqrestore(&dma_dev->lock, flags);
        
        /* Try to reset and restart */
        antsdr_dma_reset_and_restart(dma_dev);
        return;
    }
    
    antsdr_debug_log(dma_dev->dev, "DMA status check passed, status = %d\n", status);
    
    spin_lock_irqsave(&dma_dev->lock, flags);
    
    /* Update statistics */
    dma_dev->stats.transfers_completed++;
    dma_dev->stats.bytes_transferred += transfer_size;
    
    /* Store S2MM transfer lengths for debugging */
    dma_dev->s2mm_requested_length = transfer_size;
    dma_dev->s2mm_actual_length = transfer_size;
    
    spin_unlock_irqrestore(&dma_dev->lock, flags);
    
    antsdr_debug_log(dma_dev->dev, "DMA transfer complete, buffer %d, %zu bytes (pulse_mode %d)\n", 
            dma_dev->current_buffer, transfer_size, dma_dev->pulse_mode);
    
    /* Quick check: Only queue for processing if UDP destination is set */
    if (dma_dev->dest_set && transfer_size <= MAX_S2MM_TRANSFER_SIZE) {
        void *current_buffer = antsdr_get_current_buffer(dma_dev);
        
        /* Allocate memory for raw DMA data copy - prevents stack overflow */
        raw_frame.data = kmalloc(transfer_size, GFP_ATOMIC);
        if (!raw_frame.data) {
            dev_warn_ratelimited(dma_dev->dev, "Failed to allocate memory for raw frame, dropping %zu bytes\n", transfer_size);
            spin_lock_irqsave(&dma_dev->lock, flags);
            dma_dev->stats.errors++;
            spin_unlock_irqrestore(&dma_dev->lock, flags);
            goto skip_frame_processing;
        }
        
        /* Copy raw DMA data to allocated buffer for threaded processing */
        raw_frame.data_len = transfer_size;
        memcpy(raw_frame.data, current_buffer, transfer_size);
        
        /* Queue raw frame to FIFO (non-blocking) */
        spin_lock_irqsave(&dma_dev->raw_fifo_lock, flags);
        ret = kfifo_in(&dma_dev->raw_frame_fifo, &raw_frame, sizeof(raw_frame));
        spin_unlock_irqrestore(&dma_dev->raw_fifo_lock, flags);
        
        if (ret == sizeof(raw_frame)) {
            /* Successfully queued, schedule frame processing work */
            if (!dma_dev->frame_work_pending) {
                dma_dev->frame_work_pending = true;
                queue_work(dma_dev->frame_workqueue, &dma_dev->frame_work);
            }
            antsdr_debug_log(dma_dev->dev, "DMA callback: Queued %zu bytes for frame processing\n", transfer_size);
        } else {
            /* Failed to queue - free the allocated memory */
            kfree(raw_frame.data);
            dev_warn_ratelimited(dma_dev->dev, "Raw frame FIFO full, dropping %zu bytes\n", transfer_size);
            spin_lock_irqsave(&dma_dev->lock, flags);
            dma_dev->stats.errors++;
            spin_unlock_irqrestore(&dma_dev->lock, flags);
        }
    } else {
        antsdr_debug_log(dma_dev->dev, "DMA callback: UDP destination not set or invalid size, dropping data\n");
    }
    
skip_frame_processing:
    spin_lock_irqsave(&dma_dev->lock, flags);
    
    /* Check if we're still streaming before submitting next transfer */
    antsdr_debug_log(dma_dev->dev, "DMA callback: Checking streaming state = %s\n", 
             dma_dev->streaming ? "true" : "false");
    
    if (!dma_dev->streaming) {
        antsdr_debug_log(dma_dev->dev, "DMA callback: Stopping - streaming=false, completing DMA\n");
        spin_unlock_irqrestore(&dma_dev->lock, flags);
        complete(&dma_dev->dma_complete);
        return;
    }
    
    antsdr_debug_log(dma_dev->dev, "DMA callback: Continuing - streaming=true, submitting next transfer\n");
    
    /* Move to next buffer for continuous streaming */
    dma_dev->current_buffer = (dma_dev->current_buffer + 1) % NUM_BUFFERS;
    
    spin_unlock_irqrestore(&dma_dev->lock, flags);
    
    /* Wake up any waiting processes */
    wake_up_interruptible(&dma_dev->wait_queue);
    
    /* Submit next transfer for continuous streaming */
    ret = antsdr_submit_dma_transfer(dma_dev);
    if (ret) {
        dev_err(dma_dev->dev, "Failed to submit next DMA transfer: %d, performing reset\n", ret);
        spin_lock_irqsave(&dma_dev->lock, flags);
        dma_dev->stats.errors++;
        /* Check streaming state again before reset - avoid race condition */
        if (dma_dev->streaming) {
            spin_unlock_irqrestore(&dma_dev->lock, flags);
            /* Reset and restart DMA on error */
            antsdr_dma_reset_and_restart(dma_dev);
        } else {
            spin_unlock_irqrestore(&dma_dev->lock, flags);
            dev_info(dma_dev->dev, "Not performing reset - streaming already stopped\n");
        }
        return;
    }
    
    /* Complete the DMA operation */
    complete(&dma_dev->dma_complete);
}

/* Frame processing work function - runs in separate thread context */
static void antsdr_frame_work(struct work_struct *work)
{
    struct antsdr_dma_dev *dma_dev = container_of(work, struct antsdr_dma_dev, frame_work);
    struct antsdr_raw_frame raw_frame;
    unsigned long flags;
    uint8_t *payload;
    size_t payload_len;
    int ret;
    int processed_frames = 0;
    
    antsdr_debug_log(dma_dev->dev, "Frame work function started\n");
    
    /* Process up to 50 frames per work invocation for high throughput (increased from 10) */
    while (processed_frames < 50) {
        /* Get raw frame from FIFO */
        spin_lock_irqsave(&dma_dev->raw_fifo_lock, flags);
        ret = kfifo_out(&dma_dev->raw_frame_fifo, &raw_frame, sizeof(raw_frame));
        spin_unlock_irqrestore(&dma_dev->raw_fifo_lock, flags);
        
        if (ret != sizeof(raw_frame)) {
            /* No more frames to process */
            break;
        }
        
        processed_frames++;
        antsdr_debug_log(dma_dev->dev, "Frame work: Processing frame %d (%zu bytes)\n", 
                processed_frames, raw_frame.data_len);
        
        /* Parse the FPGA frame to extract valid payload */
        ret = antsdr_parse_fpga_frame(dma_dev, raw_frame.data, raw_frame.data_len, &payload, &payload_len);
        
        /* Free the allocated frame data after processing - prevents memory leak */
        kfree(raw_frame.data);
        
        if (ret == 0) {
            antsdr_debug_log(dma_dev->dev, "Frame work: Valid frame found, extracted %zu payload bytes\n", payload_len);
            
            /* Valid frame found - queue the extracted payload for UDP transmission */
            ret = antsdr_ring_put(dma_dev, payload, payload_len);
            if (ret == 0) {
                /* Successfully queued, schedule UDP work */
                if (!dma_dev->udp_work_pending) {
                    dma_dev->udp_work_pending = true;
                    schedule_work(&dma_dev->udp_work);
                }
                
                spin_lock_irqsave(&dma_dev->lock, flags);
                dma_dev->stats.valid_frames++;
                dma_dev->stats.extracted_frames++;
                spin_unlock_irqrestore(&dma_dev->lock, flags);
                
                antsdr_debug_log(dma_dev->dev, "Frame work: Queued %zu payload bytes, scheduling UDP work\n", payload_len);
            } else {
                dev_warn_ratelimited(dma_dev->dev, "Ring buffer full, dropping valid frame\n");
                spin_lock_irqsave(&dma_dev->lock, flags);
                dma_dev->stats.errors++;
                spin_unlock_irqrestore(&dma_dev->lock, flags);
            }
        } else {
            antsdr_debug_log(dma_dev->dev, "Frame work: Invalid FPGA frame detected, dropping %zu bytes\n", raw_frame.data_len);
            /* Invalid frame - update stats but don't queue */
            spin_lock_irqsave(&dma_dev->lock, flags);
            dma_dev->stats.invalid_frames++;
            spin_unlock_irqrestore(&dma_dev->lock, flags);
        }
    }
    
    /* Check if there are more frames to process */
    spin_lock_irqsave(&dma_dev->raw_fifo_lock, flags);
    if (!kfifo_is_empty(&dma_dev->raw_frame_fifo)) {
        /* More frames available, reschedule work */
        queue_work(dma_dev->frame_workqueue, &dma_dev->frame_work);
        antsdr_debug_log(dma_dev->dev, "Frame work: Rescheduling - more frames available\n");
    } else {
        /* No more frames, clear pending flag */
        dma_dev->frame_work_pending = false;
        antsdr_debug_log(dma_dev->dev, "Frame work: Completed - processed %d frames\n", processed_frames);
    }
    spin_unlock_irqrestore(&dma_dev->raw_fifo_lock, flags);
}

static void antsdr_udp_work(struct work_struct *work)
{
    struct antsdr_dma_dev *dma_dev = container_of(work, struct antsdr_dma_dev, udp_work);
    unsigned char *packet_buffer;
    struct antsdr_packet_header *header;
    unsigned char *packet_payload;
    struct msghdr msg;
    struct kvec iov;
    int ret;
    unsigned long flags;
    int packets_sent = 0; /* Limit work per invocation */

    packet_buffer = kmalloc(UDP_PACKET_SIZE, GFP_KERNEL);
    if (!packet_buffer) {
        dev_err(dma_dev->dev, "Failed to allocate UDP packet buffer\n");
        goto reset_flag_simple;
    }
    header = (struct antsdr_packet_header *)packet_buffer;
    packet_payload = packet_buffer + ANTSDR_PACKET_HEADER_SIZE;

    while (packets_sent < 200) { /* Increased packet limit from 50 to 200 for higher throughput */
        void *ring_data;
        size_t ring_size;
        size_t payload_len;
        uint32_t current_frame_id;
        size_t fragments_needed, fragment_offset = 0;

        ret = antsdr_ring_get(dma_dev, &ring_data, &ring_size);
        if (ret != 0)
            break; /* No more payload data */

        /* The ring now contains extracted payload data, not raw DMA data */
        payload_len = ring_size;
        
        /* Return ring buffer early */
        antsdr_ring_return_buffer(dma_dev);

        /* Fragment the payload if it's larger than max packet size */
        fragments_needed = (payload_len + ANTSDR_MAX_PAYLOAD_SIZE - 1) / ANTSDR_MAX_PAYLOAD_SIZE;

        spin_lock_irqsave(&dma_dev->lock, flags);
        current_frame_id = dma_dev->frame_id_counter++;
        spin_unlock_irqrestore(&dma_dev->lock, flags);

        for (size_t fragment_idx = 0; fragment_idx < fragments_needed; fragment_idx++) {
            size_t current_fragment_size = min(payload_len - fragment_offset,
                                              (size_t)ANTSDR_MAX_PAYLOAD_SIZE);
            
            /* Copy payload fragment to packet buffer */
            memcpy(packet_payload, (uint8_t *)ring_data + fragment_offset, current_fragment_size);

            /* Build packet header */
            header->start_marker = cpu_to_be32(ANTSDR_PACKET_START_MARKER);
            header->sequence_number = cpu_to_be32(dma_dev->packet_sequence_number++);
            header->total_length = cpu_to_be32(ANTSDR_PACKET_HEADER_SIZE + current_fragment_size);
            header->payload_length = cpu_to_be32(current_fragment_size);
            header->frame_id = cpu_to_be32(current_frame_id);
            header->fragment_offset = cpu_to_be32(fragment_offset);
            header->fragment_count = cpu_to_be32(fragments_needed);
            header->fragment_index = cpu_to_be32(fragment_idx);
            header->frame_payload_total = cpu_to_be32(payload_len);
            header->missing_frame_count = cpu_to_be32(dma_dev->missing_frame_count);
            header->checksum = cpu_to_be32(crc32(0, packet_payload, current_fragment_size));
            header->end_marker = cpu_to_be32(ANTSDR_PACKET_END_MARKER);

            /* Send UDP packet */
            if (dma_dev->sock && dma_dev->dest_set) {
                memset(&msg, 0, sizeof(msg));
                iov.iov_base = packet_buffer;
                iov.iov_len = ANTSDR_PACKET_HEADER_SIZE + current_fragment_size;
                msg.msg_name = &dma_dev->dest_addr;
                msg.msg_namelen = sizeof(dma_dev->dest_addr);
                
                ret = kernel_sendmsg(dma_dev->sock, &msg, &iov, 1, iov.iov_len);
                if (ret > 0) {
                    spin_lock_irqsave(&dma_dev->lock, flags);
                    dma_dev->stats.udp_packets_sent++;
                    spin_unlock_irqrestore(&dma_dev->lock, flags);
                    packets_sent++;
                    
                    dev_dbg(dma_dev->dev, "Sent UDP packet: frame_id=%u, fragment=%zu/%zu, size=%zu\n",
                            current_frame_id, fragment_idx + 1, fragments_needed, current_fragment_size);
                } else {
                    dev_err(dma_dev->dev, "UDP send error fragment %zu/%zu ret=%d\n",
                            fragment_idx + 1, fragments_needed, ret);
                    spin_lock_irqsave(&dma_dev->lock, flags);
                    dma_dev->stats.errors++;
                    spin_unlock_irqrestore(&dma_dev->lock, flags);
                    goto loop_end;
                }
            }
            fragment_offset += current_fragment_size;
        }
    }

loop_end:
    kfree(packet_buffer);

    /* Check if more data is available and reschedule if needed */
    spin_lock_irqsave(&dma_dev->lock, flags);
    if (dma_dev->ring_count > 0) {
        schedule_work(&dma_dev->udp_work);
    } else {
        dma_dev->udp_work_pending = false;
    }
    spin_unlock_irqrestore(&dma_dev->lock, flags);
    return;

reset_flag_simple:
    spin_lock_irqsave(&dma_dev->lock, flags);
    dma_dev->udp_work_pending = false;
    spin_unlock_irqrestore(&dma_dev->lock, flags);
}

static int antsdr_submit_dma_transfer(struct antsdr_dma_dev *dma_dev)
{
    struct dma_async_tx_descriptor *desc;
    dma_cookie_t cookie;
    size_t transfer_size;
    dma_addr_t dma_handle;
    enum dma_status status;
    void *buffer;
    struct dma_slave_config config;
    int ret;
    
    if (!dma_dev->rx_chan) {
        dev_err(dma_dev->dev, "No DMA channel available\n");
        return -ENODEV;
    }
    
    /* Get transfer parameters based on current pulse mode */
    transfer_size = antsdr_get_transfer_size(dma_dev);
    buffer = antsdr_get_current_buffer(dma_dev);
    dma_handle = antsdr_get_current_handle(dma_dev);
    
    /* Check current DMA status and terminate any pending operations */
    status = dmaengine_tx_status(dma_dev->rx_chan, 0, NULL);
    if (status == DMA_ERROR) {
        dev_warn(dma_dev->dev, "DMA channel in error state (%d), terminating\n", status);
        ret = dmaengine_terminate_all(dma_dev->rx_chan);
        if (ret) {
            dev_err(dma_dev->dev, "Failed to terminate DMA channel: %d\n", ret);
        }
        /* Minimal delay for error recovery */
        udelay(10);  /* Reduced from 100μs to 10μs for high-speed operation */
    }
    
    /* Skip unnecessary DMA resets for performance - only on errors */
    /* Wait for FIFO flush and DMA engine reset - minimized for speed */
    /* udelay(20); */ /* Removed 20μs delay to improve throughput */
    
    /* Configure DMA channel for S2MM (Stream to Memory Mapped) transfers */
    memset(&config, 0, sizeof(config));
    config.direction = DMA_DEV_TO_MEM;
    config.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;  /* 32-bit transfers */
    config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;  /* 32-bit transfers */
    config.src_maxburst = 64;  /* Increased burst size from 16 to 64 words (256 bytes) for higher throughput */
    config.dst_maxburst = 64;
    
    ret = dmaengine_slave_config(dma_dev->rx_chan, &config);
    if (ret) {
        dev_err(dma_dev->dev, "Failed to configure DMA channel: %d\n", ret);
        return ret;
    }
    
    /* Ensure buffer is properly aligned and cleared */
    if ((unsigned long)dma_handle & 0x3F) {
        dev_warn(dma_dev->dev, "Buffer %d not 64-byte aligned: 0x%llx\n", 
                 dma_dev->current_buffer, (unsigned long long)dma_handle);
    }
    
    dev_dbg(dma_dev->dev, "Starting DMA transfer - pulse_mode: %d, size: %zu bytes, buffer: %d\n",
            dma_dev->pulse_mode, transfer_size, dma_dev->current_buffer);
    
    /* Ensure cache coherency before DMA transfer */
    dma_sync_single_for_device(dma_dev->rx_chan->device->dev,
                              dma_handle,
                              transfer_size,
                              DMA_FROM_DEVICE);
    
    /* Prepare DMA transfer - use DMA_MEM_TO_MEM for simple transfers */
    desc = dmaengine_prep_slave_single(dma_dev->rx_chan,
                                       dma_handle,
                                       transfer_size,
                                       DMA_DEV_TO_MEM,
                                       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
    if (!desc) {
        dev_err(dma_dev->dev, "Failed to prepare DMA transfer (size=%zu)\n", transfer_size);
        return -ENOMEM;
    }
    
    /* Set callback */
    desc->callback = antsdr_dma_callback;
    desc->callback_param = dma_dev;
    
    /* Submit transfer */
    cookie = dmaengine_submit(desc);
    if (dma_submit_error(cookie)) {
        dev_err(dma_dev->dev, "Failed to submit DMA transfer\n");
        return -EIO;
    }

    /* Reinitialize completion */
    reinit_completion(&dma_dev->dma_complete);
    /* Start transfer */
    dma_async_issue_pending(dma_dev->rx_chan);
    
    // antsdr_debug_log("DMA transfer submitted (buffer %d, pulse_mode %d, size %zu bytes, addr=0x%pad)\n", 
    //         dma_dev->current_buffer, dma_dev->pulse_mode, transfer_size, &dma_handle);
    // antsdr_debug_log("S2MM: Using maximum transfer size %zu bytes (512 words) regardless of pulse mode\n", 
    //         transfer_size);
    
    return 0;
}

static int antsdr_dma_start_streaming(struct antsdr_dma_dev *dma_dev)
{
    int ret;
    unsigned long flags;
    size_t transfer_size = antsdr_get_transfer_size(dma_dev);
    
    dev_info(dma_dev->dev, "Starting streaming (S2MM transfer_size: %zu bytes = 512 words for all modes)...\n",
             transfer_size);
    dev_info(dma_dev->dev, "Previous mode-specific sizes: short=%zu bytes, long=%zu bytes\n",
             SHORT_PULSE_TRANSFER_SIZE, LONG_PULSE_TRANSFER_SIZE);
    
    spin_lock_irqsave(&dma_dev->lock, flags);
    if (dma_dev->streaming) {
        spin_unlock_irqrestore(&dma_dev->lock, flags);
        dev_warn(dma_dev->dev, "Streaming already active\n");
        return -EBUSY;
    }

    dma_dev->current_buffer = 0;
    
    /* Reset missing frame tracking for new streaming session */
    dma_dev->missing_frame_count = 0;
    dma_dev->last_frame_counter = 0;
    dma_dev->first_frame_received = false;
    
    spin_unlock_irqrestore(&dma_dev->lock, flags);
    
    dev_info(dma_dev->dev, "UDP destination: %s, pulse mode: %d, transfer size: %zu bytes\n",
             dma_dev->dest_set ? "set" : "not set", dma_dev->pulse_mode, transfer_size);
    
    /* Configure GPIO pins first */
    if (dma_dev->gpio_pulse_mode) {
        gpiod_set_value(dma_dev->gpio_pulse_mode, dma_dev->pulse_mode);
        dev_info(dma_dev->dev, "Set pulse mode GPIO to %d\n", dma_dev->pulse_mode);
    }
    
    if (dma_dev->gpio_mode) {
        gpiod_set_value(dma_dev->gpio_mode, dma_dev->operation_mode);
        dev_info(dma_dev->dev, "Set mode GPIO to %d\n", dma_dev->operation_mode);
    }
    
    /* Wait for hardware to stabilize */
    udelay(500);  /* 500μs - reduced from 10ms for 500μs packet timing */
    
    /* Set streaming flag to true BEFORE submitting DMA to avoid race condition */
    spin_lock_irqsave(&dma_dev->lock, flags);
    dev_info(dma_dev->dev, "IOCTL: Setting streaming=true - preparing for DMA start\n");
    dma_dev->streaming = true;
    spin_unlock_irqrestore(&dma_dev->lock, flags);

    
    /* Submit first DMA transfer if DMA is available */
    if (dma_dev->rx_chan) {
        /* Clear any old completions */
        // reinit_completion(&dma_dev->dma_complete);
        
        ret = antsdr_submit_dma_transfer(dma_dev);
        if (ret) {
            spin_lock_irqsave(&dma_dev->lock, flags);
            dev_info(dma_dev->dev, "START_STREAMING failed: Setting streaming=false due to submit failure\n");
            dma_dev->streaming = false;
            spin_unlock_irqrestore(&dma_dev->lock, flags);
            
            if (dma_dev->gpio_enable)
                gpiod_set_value(dma_dev->gpio_enable, 0);
            
            dev_err(dma_dev->dev, "Failed to submit initial DMA transfer: %d\n", ret);
            return ret;
        }
        dev_info(dma_dev->dev, "Streaming started with DMA\n");
    } else {
        dev_info(dma_dev->dev, "Streaming started without DMA (GPIO control only)\n");
    }

    /* Enable GPIO to start data generation after setup DMA to avoid DMA freeze due to data arrival before DMA
    setup */
    if (dma_dev->gpio_enable) {
        gpiod_set_value(dma_dev->gpio_enable, 1);
        dev_info(dma_dev->dev, "Enabled data generation GPIO\n");
        /* Give hardware time to start generating data */
        udelay(10);  /* 10μs - reduced from 50ms for 500μs packet timing */
    } else {
        dev_warn(dma_dev->dev, "No data generation GPIO available\n");
    }

    dev_info(dma_dev->dev, "IOCTL: DMA start completed successfully\n");
    
    return 0;
}

static int antsdr_dma_stop_streaming(struct antsdr_dma_dev *dma_dev)
{
    unsigned long flags;
    unsigned long timeout;
    int ret;
    
    spin_lock_irqsave(&dma_dev->lock, flags);
    if (!dma_dev->streaming) {
        spin_unlock_irqrestore(&dma_dev->lock, flags);
        return 0;
    }
    dev_info(dma_dev->dev, "STOP_STREAMING: Setting streaming=false (called by application)\n");
    dma_dev->streaming = false;
    spin_unlock_irqrestore(&dma_dev->lock, flags);
    
    /* Disable GPIO */
    if (dma_dev->gpio_enable) {
        gpiod_set_value(dma_dev->gpio_enable, 0);
        dev_info(dma_dev->dev, "Disabled data generation\n");
    }
    
    /* Terminate DMA first to stop any ongoing transfers */
    if (dma_dev->rx_chan) {
        dmaengine_terminate_async(dma_dev->rx_chan);
    }
    
    /* Wait for current transfer to complete with timeout */
    timeout = msecs_to_jiffies(1000);  /* 1 second timeout */
    ret = wait_for_completion_timeout(&dma_dev->dma_complete, timeout);
    if (ret == 0) {
        dev_warn(dma_dev->dev, "Timeout waiting for DMA completion, forcing stop\n");
        /* Reinitialize completion for next use */
        reinit_completion(&dma_dev->dma_complete);
    }
    
    /* Flush frame processing work to ensure all pending frames are processed */
    if (dma_dev->frame_workqueue) {
        flush_workqueue(dma_dev->frame_workqueue);
    }
    
    /* Clear any remaining frames in the FIFO */
    spin_lock_irqsave(&dma_dev->raw_fifo_lock, flags);
    kfifo_reset(&dma_dev->raw_frame_fifo);
    dma_dev->frame_work_pending = false;
    spin_unlock_irqrestore(&dma_dev->raw_fifo_lock, flags);
    
    dev_info(dma_dev->dev, "Streaming stopped\n");
    return 0;
}

static int antsdr_dma_reset_and_restart(struct antsdr_dma_dev *dma_dev)
{
    unsigned long flags;
    int ret;
    
    dev_warn(dma_dev->dev, "DMA error detected, performing reset and restart\n");
    
    /* Stop streaming first */
    spin_lock_irqsave(&dma_dev->lock, flags);
    if (!dma_dev->streaming) {
        spin_unlock_irqrestore(&dma_dev->lock, flags);
        dev_info(dma_dev->dev, "DMA reset: Not restarting - streaming=false (already stopped)\n");
        return 0;  /* Already stopped */
    }
    /* Don't set streaming = false here yet - we want to restart */
    spin_unlock_irqrestore(&dma_dev->lock, flags);
    
    dev_info(dma_dev->dev, "DMA reset: Restarting DMA after error - streaming=true\n");
    
    /* Reset GPIO to stop data generation */
    if (dma_dev->gpio_enable) {
        gpiod_set_value(dma_dev->gpio_enable, 0);
        udelay(500);  /* 500μs - reduced from 10ms for hardware reset timing */
        dev_info(dma_dev->dev, "GPIO reset completed\n");
    }

    /* Terminate all DMA operations */
    if (dma_dev->rx_chan) {
        dmaengine_terminate_async(dma_dev->rx_chan);
        dev_info(dma_dev->dev, "DMA channel terminated\n");
    }
    

    
    /* Clear any pending data in ring buffer */
    spin_lock_irqsave(&dma_dev->lock, flags);
    dma_dev->ring_head = 0;
    dma_dev->ring_tail = 0;
    dma_dev->ring_count = 0;
    spin_unlock_irqrestore(&dma_dev->lock, flags);
    dev_info(dma_dev->dev, "Ring buffer reset completed\n");
    
    /* Clear frame detection buffer */
    antsdr_frame_buffer_reset(dma_dev);
    
    /* Restart with fixed transfer size */
    udelay(1000);  /* 1ms - reduced from 50ms for hardware settling */
    
    /* Re-enable GPIO for data generation */
    if (dma_dev->gpio_enable) {
        gpiod_set_value(dma_dev->gpio_enable, 1);
        dev_info(dma_dev->dev, "Data generation re-enabled\n");
    }
    
    /* Submit new DMA transfer with fixed 512-word size */
    ret = antsdr_submit_dma_transfer(dma_dev);
    if (ret) {
        dev_err(dma_dev->dev, "Failed to restart DMA after reset: %d\n", ret);
        spin_lock_irqsave(&dma_dev->lock, flags);
        dev_info(dma_dev->dev, "DMA reset failed: Setting streaming=false due to restart failure\n");
        dma_dev->streaming = false;
        spin_unlock_irqrestore(&dma_dev->lock, flags);
        return ret;
    }
    
    dev_info(dma_dev->dev, "DMA reset and restart completed successfully\n");
    return 0;
}

/* Note: antsdr_reallocate_buffers function removed - buffer sizes are now fixed per pulse mode */

static long antsdr_dma_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct antsdr_dma_dev *dma_dev = file->private_data;
    int ret = 0;
    unsigned int value;
    struct antsdr_udp_dest udp_dest;
    struct antsdr_dma_stats stats;
    unsigned long flags;
    
    switch (cmd) {
    case ANTSDR_IOC_START_STREAMING:
        dev_info(dma_dev->dev, "IOCTL: START_STREAMING called by remote application\n");
        ret = antsdr_dma_start_streaming(dma_dev);
        break;
        
    case ANTSDR_IOC_STOP_STREAMING:
        dev_info(dma_dev->dev, "IOCTL: STOP_STREAMING called by remote application\n");
        ret = antsdr_dma_stop_streaming(dma_dev);
        break;
        
    case ANTSDR_IOC_SET_BUFFER_SIZE:
        {
            /* Buffer sizes are now fixed based on pulse mode - no longer configurable */
            dev_info(dma_dev->dev, "Buffer sizes are fixed: Long pulse = %d bytes, Short pulse = %d bytes\n",
                     LONG_PULSE_TRANSFER_SIZE, SHORT_PULSE_TRANSFER_SIZE);
            ret = -ENOTSUPP;  /* Operation not supported */
        }
        break;
        
    case ANTSDR_IOC_GET_STATS:
        spin_lock_irqsave(&dma_dev->lock, flags);
        stats = dma_dev->stats;
        spin_unlock_irqrestore(&dma_dev->lock, flags);
        
        if (copy_to_user((void __user *)arg, &stats, sizeof(stats))) {
            ret = -EFAULT;
        }
        break;
        
    case ANTSDR_IOC_SET_UDP_DEST:
        if (copy_from_user(&udp_dest, (void __user *)arg, sizeof(udp_dest))) {
            ret = -EFAULT;
            break;
        }
        
        dma_dev->dest_addr.sin_family = AF_INET;
        dma_dev->dest_addr.sin_addr.s_addr = udp_dest.ip;
        dma_dev->dest_addr.sin_port = htons(udp_dest.port);
        dma_dev->dest_set = true;
        
        dev_info(dma_dev->dev, "UDP destination set to %pI4:%u\n", 
                 &udp_dest.ip, udp_dest.port);
        break;
        
    case ANTSDR_IOC_RESET_STATS:
        spin_lock_irqsave(&dma_dev->lock, flags);
        memset(&dma_dev->stats, 0, sizeof(dma_dev->stats));
        spin_unlock_irqrestore(&dma_dev->lock, flags);
        dev_info(dma_dev->dev, "Statistics reset\n");
        break;
        
    case ANTSDR_IOC_SET_PULSE_MODE:
        if (copy_from_user(&value, (void __user *)arg, sizeof(value))) {
            ret = -EFAULT;
            break;
        }
        
        /* Check if we need to restart streaming for new transfer size */
        spin_lock_irqsave(&dma_dev->lock, flags);
        bool was_streaming = dma_dev->streaming;
        spin_unlock_irqrestore(&dma_dev->lock, flags);
        
        if (was_streaming) {
            antsdr_dma_stop_streaming(dma_dev);
        }
        
        dma_dev->pulse_mode = value ? 1 : 0;
        if (dma_dev->gpio_pulse_mode) {
            gpiod_set_value(dma_dev->gpio_pulse_mode, dma_dev->pulse_mode);
            dev_info(dma_dev->dev, "Pulse mode %s (transfer size: %zu bytes)\n", 
                     dma_dev->pulse_mode ? "enabled" : "disabled", 
                     antsdr_get_transfer_size(dma_dev));
        }
        
        /* Restart streaming if it was active */
        if (was_streaming) {
            ret = antsdr_dma_start_streaming(dma_dev);
        }
        break;
        
    case ANTSDR_IOC_SET_TDD_MODE:
        if (copy_from_user(&value, (void __user *)arg, sizeof(value))) {
            ret = -EFAULT;
            break;
        }
        
        if (dma_dev->gpio_tdd_mode) {
            gpiod_set_value(dma_dev->gpio_tdd_mode, value ? 1 : 0);
            dev_info(dma_dev->dev, "TDD mode %s\n", value ? "enabled" : "disabled");
        }
        break;
        
    case ANTSDR_IOC_GET_PULSE_MODE:
        value = dma_dev->pulse_mode;
        if (copy_to_user((void __user *)arg, &value, sizeof(value))) {
            ret = -EFAULT;
        }
        break;
        
    case ANTSDR_IOC_GET_TDD_MODE:
        value = dma_dev->gpio_tdd_mode ? gpiod_get_value(dma_dev->gpio_tdd_mode) : 0;
        if (copy_to_user((void __user *)arg, &value, sizeof(value))) {
            ret = -EFAULT;
        }
        break;
        
    case ANTSDR_IOC_GET_BUFFER_SIZE:
        value = dma_dev->buffer_size;
        if (copy_to_user((void __user *)arg, &value, sizeof(value))) {
            ret = -EFAULT;
        }
        break;
        
    case ANTSDR_IOC_SET_MODE:
        {
            uint32_t new_mode;
            if (copy_from_user(&new_mode, (void __user *)arg, sizeof(new_mode))) {
                ret = -EFAULT;
                break;
            }
            
            if (new_mode > 1) {
                ret = -EINVAL;
                break;
            }
            
            spin_lock_irqsave(&dma_dev->lock, flags);
            dma_dev->operation_mode = new_mode;
            spin_unlock_irqrestore(&dma_dev->lock, flags);
            
            /* Set the mode GPIO if available */
            if (dma_dev->gpio_mode) {
                gpiod_set_value(dma_dev->gpio_mode, new_mode);
                dev_info(dma_dev->dev, "Set operation mode to %u (%s)\n", 
                         new_mode, new_mode ? "simulation" : "real data");
            } else {
                dev_info(dma_dev->dev, "Set operation mode to %u (no GPIO control)\n", new_mode);
            }
        }
        break;
        
    case ANTSDR_IOC_GET_MODE:
       if (copy_to_user((void __user *)arg, &dma_dev->operation_mode, sizeof(dma_dev->operation_mode)))
            ret = -EFAULT;
        break;
        
    default:
        ret = -ENOTTY;
        break;
    }
    
    return ret;
}

static int antsdr_dma_open(struct inode *inode, struct file *file)
{
    struct antsdr_dma_dev *dma_dev = container_of(file->private_data, 
                                                   struct antsdr_dma_dev, misc_dev);
    
    file->private_data = dma_dev;
    
    dev_info(dma_dev->dev, "Device opened\n");
    return 0;
}

static int antsdr_dma_release(struct inode *inode, struct file *file)
{
    struct antsdr_dma_dev *dma_dev = file->private_data;
    
    /* Stop streaming if active */
    antsdr_dma_stop_streaming(dma_dev);
    
    dev_info(dma_dev->dev, "Device closed\n");
    return 0;
}

static ssize_t antsdr_dma_read(struct file *file, char __user *buf,
                               size_t count, loff_t *ppos)
{
    struct antsdr_dma_dev *dma_dev = file->private_data;
    unsigned char *kbuf;
    int ret;
    unsigned long flags;
    
    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf) {
        return -ENOMEM;
    }
    
    spin_lock_irqsave(&dma_dev->lock, flags);
    /* For ring buffer implementation, we need to get a complete buffer */
    void *ring_data;
    size_t ring_size;
    ret = antsdr_ring_get(dma_dev, &ring_data, &ring_size);
    if (ret == 0) {
        size_t copy_size = min(count, ring_size);
        memcpy(kbuf, ring_data, copy_size);
        antsdr_ring_return_buffer(dma_dev);
        ret = copy_size;
    } else {
        ret = 0; /* No data available */
    }
    spin_unlock_irqrestore(&dma_dev->lock, flags);
    
    if (ret > 0) {
        if (copy_to_user(buf, kbuf, ret)) {
            ret = -EFAULT;
        }
    }
    
    kfree(kbuf);
    return ret;
}

static unsigned int antsdr_dma_poll(struct file *file, poll_table *wait)
{
    struct antsdr_dma_dev *dma_dev = file->private_data;
    unsigned int mask = 0;
    unsigned long flags;
    
    poll_wait(file, &dma_dev->wait_queue, wait);
    
    spin_lock_irqsave(&dma_dev->lock, flags);
    if (dma_dev->ring_count > 0) {
        mask |= POLLIN | POLLRDNORM;
    }
    spin_unlock_irqrestore(&dma_dev->lock, flags);
    
    return mask;
}

static const struct file_operations antsdr_dma_fops = {
    .owner = THIS_MODULE,
    .open = antsdr_dma_open,
    .release = antsdr_dma_release,
    .read = antsdr_dma_read,
    .poll = antsdr_dma_poll,
    .unlocked_ioctl = antsdr_dma_ioctl,
};

static int antsdr_dma_probe(struct platform_device *pdev)
{
    struct antsdr_dma_dev *dma_dev;
    int ret, i;
    
    dma_dev = devm_kzalloc(&pdev->dev, sizeof(*dma_dev), GFP_KERNEL);
    if (!dma_dev) {
        return -ENOMEM;
    }
    
    dma_dev->pdev = pdev;
    dma_dev->dev = &pdev->dev;
    dma_dev->buffer_size = DEFAULT_BUFFER_SIZE;
    dma_dev->current_buffer = 0;
    dma_dev->streaming = false;
    dma_dev->dest_set = false;
    dma_dev->pulse_mode = 0;
    dma_dev->tdd_mode = 0;
    dma_dev->mode = 0;
    
    /* Initialize debug and frame export counters */
    dma_dev->frame_export_count = 0;
    dma_dev->total_frames_processed = 0;
    dma_dev->frames_in_current_file = 0;
    dma_dev->missing_frame_count = 0;
    dma_dev->last_frame_counter = 0;
    dma_dev->first_frame_received = false;
    
    /* Initialize S2MM tracking */
    dma_dev->s2mm_requested_length = 0;
    dma_dev->s2mm_actual_length = 0;
    
    platform_set_drvdata(pdev, dma_dev);
    
    /* Initialize synchronization objects */
    spin_lock_init(&dma_dev->lock);
    init_waitqueue_head(&dma_dev->wait_queue);
    init_completion(&dma_dev->dma_complete);
    
    /* Initialize ring buffer */
    ret = antsdr_ring_init(dma_dev);
    if (ret) {
        dev_err(&pdev->dev, "Failed to initialize ring buffer: %d\n", ret);
        return ret;
    }
    
    /* Initialize raw frame FIFO for threaded processing */
    spin_lock_init(&dma_dev->raw_fifo_lock);
    ret = kfifo_alloc(&dma_dev->raw_frame_fifo, RAW_FRAME_FIFO_SIZE, GFP_KERNEL);
    if (ret) {
        dev_err(&pdev->dev, "Failed to allocate raw frame FIFO: %d\n", ret);
        antsdr_ring_cleanup(dma_dev);
        return ret;
    }
    
    /* Create dedicated workqueue for frame processing */
    dma_dev->frame_workqueue = create_singlethread_workqueue("antsdr_frame_wq");
    if (!dma_dev->frame_workqueue) {
        dev_err(&pdev->dev, "Failed to create frame processing workqueue\n");
        kfifo_free(&dma_dev->raw_frame_fifo);
        antsdr_ring_cleanup(dma_dev);
        return -ENOMEM;
    }
    
    /* Initialize frame work */
    INIT_WORK(&dma_dev->frame_work, antsdr_frame_work);
    dma_dev->frame_work_pending = false;
    
    /* Initialize frame detection buffer */
    ret = antsdr_frame_buffer_init(dma_dev);
    if (ret) {
        dev_err(&pdev->dev, "Failed to initialize frame detection buffer: %d\n", ret);
        destroy_workqueue(dma_dev->frame_workqueue);
        kfifo_free(&dma_dev->raw_frame_fifo);
        antsdr_ring_cleanup(dma_dev);
        return ret;
    }
    
    /* Initialize work queue */
    INIT_WORK(&dma_dev->udp_work, antsdr_udp_work);
    dma_dev->udp_work_pending = false;
    
    /* Debug: Print device tree information */
    dev_info(&pdev->dev, "Device probe starting - checking DMA resources...\n");
    if (pdev->dev.of_node) {
        struct property *prop;
        const char *name;
        int index = 0;
        
        dev_info(&pdev->dev, "Device tree node found: %s\n", pdev->dev.of_node->name);
        
        /* Check dma-names property */
        of_property_for_each_string(pdev->dev.of_node, "dma-names", prop, name) {
            dev_info(&pdev->dev, "DMA name[%d]: %s\n", index++, name);
        }
        
        /* Check dmas property */
        if (of_find_property(pdev->dev.of_node, "dmas", NULL)) {
            dev_info(&pdev->dev, "dmas property found in device tree\n");
        } else {
            dev_warn(&pdev->dev, "No dmas property found in device tree\n");
        }
    } else {
        dev_warn(&pdev->dev, "No device tree node found\n");
    }
    
     /* Get DMA channel - following xilinx_axidma patterns */
    dma_dev->rx_chan = dma_request_slave_channel(&pdev->dev, "rx_channel");
    if (!dma_dev->rx_chan) {
        dev_warn(&pdev->dev, "Failed to get RX DMA channel, trying fallback\n");
        /* Try alternative channel name */
        dma_dev->rx_chan = dma_request_slave_channel(&pdev->dev, "s2mm_channel");
        if (!dma_dev->rx_chan) {
            dev_err(&pdev->dev, "Failed to get any DMA channel\n");
            ret = -ENODEV;
            goto err_frame_buffer;
        }
    }
    
    /* Reset DMA channel to ensure clean state */
    dev_info(&pdev->dev, "Resetting DMA channel for clean initialization\n");
    if (dma_dev->rx_chan && dma_dev->rx_chan->device && dma_dev->rx_chan->device->device_terminate_all) {
        ret = dmaengine_terminate_async(dma_dev->rx_chan);
        if (ret) {
            dev_warn(&pdev->dev, "Failed to terminate DMA channel: %d\n", ret);
        } else {
            dev_info(&pdev->dev, "DMA channel reset successfully\n");
        }
        /* Give DMA engine time to reset */
        udelay(500);  /* 500μs - reduced from 10ms for probe timing */
    } else {
        dev_info(&pdev->dev, "DMA channel does not support termination, skipping reset\n");
    }
    
    /* Print DMA channel information */
    dev_info(&pdev->dev, "Got DMA channel: %s (max_burst=%u)\n", 
             dma_chan_name(dma_dev->rx_chan), 
             dma_dev->rx_chan->device->max_burst);
    
    /* Check DMA capabilities */
    if (!dma_has_cap(DMA_SLAVE, dma_dev->rx_chan->device->cap_mask)) {
        dev_err(&pdev->dev, "DMA channel does not support slave transfers\n");
        ret = -EINVAL;
        goto err_dma_chan;
    }
    
    if (!dma_has_cap(DMA_PRIVATE, dma_dev->rx_chan->device->cap_mask)) {
        dev_warn(&pdev->dev, "DMA channel is not private\n");
    }
    
    /* Allocate DMA buffers only if we have a DMA channel */
    if (dma_dev->rx_chan) {
        /* Allocate DMA buffers for maximum transfer size (512 words = 2048 bytes) */
        for (i = 0; i < NUM_BUFFERS; i++) {
            dma_dev->long_dma_buffers[i] = dma_alloc_coherent(dma_dev->rx_chan->device->dev,
                                                              MAX_S2MM_TRANSFER_SIZE,
                                                              &dma_dev->long_dma_handles[i],
                                                              GFP_KERNEL | GFP_DMA);
            if (!dma_dev->long_dma_buffers[i]) {
                dev_err(&pdev->dev, "Failed to allocate DMA buffer %d (%zu bytes)\n", i, MAX_S2MM_TRANSFER_SIZE);
                ret = -ENOMEM;
                goto err_buffers;
            }
            
            /* Verify buffer alignment */
            if (!IS_ALIGNED((unsigned long)dma_dev->long_dma_buffers[i], 64)) {
                dev_warn(&pdev->dev, "DMA buffer %d not 64-byte aligned: %p\n", 
                         i, dma_dev->long_dma_buffers[i]);
            }
            
            if (!IS_ALIGNED(dma_dev->long_dma_handles[i], 64)) {
                dev_warn(&pdev->dev, "DMA handle %d not 64-byte aligned: 0x%pad\n", 
                         i, &dma_dev->long_dma_handles[i]);
            }
            
            dev_info(&pdev->dev, "DMA buffer %d: virt=%p dma=0x%pad size=%zu bytes\n",
                     i, dma_dev->long_dma_buffers[i], &dma_dev->long_dma_handles[i], MAX_S2MM_TRANSFER_SIZE);
        }
        
        dev_info(&pdev->dev, "Allocated %d DMA buffers of %zu bytes each (total %zu bytes)\n",
                 NUM_BUFFERS, MAX_S2MM_TRANSFER_SIZE, NUM_BUFFERS * MAX_S2MM_TRANSFER_SIZE);
        
        dev_info(&pdev->dev, "DMA buffers allocated successfully (long buffers only)\n");
    } else {
        dev_info(&pdev->dev, "Skipping DMA buffer allocation (no DMA channel)\n");
        /* Initialize buffer pointers to NULL */
        for (i = 0; i < NUM_BUFFERS; i++) {
            dma_dev->long_dma_buffers[i] = NULL;
            dma_dev->long_dma_handles[i] = 0;
        }
    }
    
    /* Get GPIO pins */
    dma_dev->gpio_enable = devm_gpiod_get_optional(&pdev->dev, "enable", GPIOD_OUT_LOW);
    if (IS_ERR(dma_dev->gpio_enable)) {
        ret = PTR_ERR(dma_dev->gpio_enable);
        dev_err(&pdev->dev, "Failed to get enable GPIO: %d\n", ret);
        goto err_buffers;
    }
    
    dma_dev->gpio_pulse_mode = devm_gpiod_get_optional(&pdev->dev, "pulse-mode", GPIOD_OUT_LOW);
    if (IS_ERR(dma_dev->gpio_pulse_mode)) {
        ret = PTR_ERR(dma_dev->gpio_pulse_mode);
        dev_err(&pdev->dev, "Failed to get pulse-mode GPIO: %d\n", ret);
        goto err_buffers;
    }
    
    dma_dev->gpio_tdd_mode = devm_gpiod_get_optional(&pdev->dev, "tdd-mode", GPIOD_OUT_LOW);
    if (IS_ERR(dma_dev->gpio_tdd_mode)) {
        ret = PTR_ERR(dma_dev->gpio_tdd_mode);
        dev_err(&pdev->dev, "Failed to get tdd-mode GPIO: %d\n", ret);
        goto err_buffers;
    }

    dma_dev->gpio_mode = devm_gpiod_get_optional(&pdev->dev, "mode", GPIOD_OUT_LOW);
    if (IS_ERR(dma_dev->gpio_mode)) {
        ret = PTR_ERR(dma_dev->gpio_mode);
        dev_err(&pdev->dev, "Failed to get mode GPIO: %d\n", ret);
        goto err_dma;
    }
    
    /* Create UDP socket */
    ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &dma_dev->sock);
    if (ret) {
        dev_err(&pdev->dev, "Failed to create UDP socket: %d\n", ret);
        goto err_buffers;
    }
    
    /* Set default UDP destination: 192.168.1.125:12288 */
    dma_dev->dest_addr.sin_family = AF_INET;
    dma_dev->dest_addr.sin_addr.s_addr = htonl((192 << 24) | (168 << 16) | (1 << 8) | 125); /* 192.168.1.125 */
    dma_dev->dest_addr.sin_port = htons(12288); /* Port 12288 */
    dma_dev->dest_set = true;
    
    dev_info(&pdev->dev, "Default UDP destination set to 192.168.1.125:12288\n");
    
    /* Register misc device */
    dma_dev->misc_dev.minor = MISC_DYNAMIC_MINOR;
    dma_dev->misc_dev.name = DEVICE_NAME;
    dma_dev->misc_dev.fops = &antsdr_dma_fops;
    
    ret = misc_register(&dma_dev->misc_dev);
    if (ret) {
        dev_err(&pdev->dev, "Failed to register misc device: %d\n", ret);
        goto err_socket;
    }
    
    dev_info(&pdev->dev, "ANTSDR DMA driver probed successfully\n");
    return 0;
    
err_socket:
    sock_release(dma_dev->sock);
err_dma_chan:
    dma_release_channel(dma_dev->rx_chan);
err_frame_buffer:
    antsdr_frame_buffer_cleanup(dma_dev);
err_buffers:
    if (dma_dev->rx_chan) {
        /* Free long pulse buffers only (we no longer use short buffers) */
        for (i = 0; i < NUM_BUFFERS; i++) {
            if (dma_dev->long_dma_buffers[i]) {
                dma_free_coherent(dma_dev->rx_chan->device->dev,
                                  MAX_S2MM_TRANSFER_SIZE,
                                  dma_dev->long_dma_buffers[i],
                                  dma_dev->long_dma_handles[i]);
            }
        }
    }
    
    /* Clean up frame processing resources */
    if (dma_dev->frame_workqueue) {
        destroy_workqueue(dma_dev->frame_workqueue);
    }
    kfifo_free(&dma_dev->raw_frame_fifo);
    antsdr_ring_cleanup(dma_dev);
    return ret;

err_dma:
    if (dma_dev->rx_chan)
        dma_release_channel(dma_dev->rx_chan);
    return ret;
}



static int antsdr_dma_remove(struct platform_device *pdev)
{
    struct antsdr_dma_dev *dma_dev = platform_get_drvdata(pdev);
    int i;
    
    /* Stop streaming */
    antsdr_dma_stop_streaming(dma_dev);
    
    /* Stop and destroy frame processing workqueue */
    if (dma_dev->frame_workqueue) {
        flush_workqueue(dma_dev->frame_workqueue);
        destroy_workqueue(dma_dev->frame_workqueue);
    }
    
    /* Free raw frame FIFO */
    kfifo_free(&dma_dev->raw_frame_fifo);
    
    /* Unregister misc device */
    misc_deregister(&dma_dev->misc_dev);
    
    /* Release socket */
    if (dma_dev->sock) {
        sock_release(dma_dev->sock);
    }
    
    /* Free DMA buffers and release channel if available */
    if (dma_dev->rx_chan) {
        /* Free long pulse buffers only (we no longer use short buffers) */
        for (i = 0; i < NUM_BUFFERS; i++) {
            if (dma_dev->long_dma_buffers[i]) {
                dma_free_coherent(dma_dev->rx_chan->device->dev,
                                  MAX_S2MM_TRANSFER_SIZE,
                                  dma_dev->long_dma_buffers[i],
                                  dma_dev->long_dma_handles[i]);
            }
        }
        
        /* Release DMA channel */
        dma_release_channel(dma_dev->rx_chan);
    }
    
    /* Free ring buffer */
    antsdr_ring_cleanup(dma_dev);
    
    /* Free frame detection buffer */
    antsdr_frame_buffer_cleanup(dma_dev);
    
    dev_info(&pdev->dev, "ANTSDR DMA driver removed\n");
    return 0;
}

static const struct of_device_id antsdr_dma_of_match[] = {
    { .compatible = "antsdr,antsdr-dma", },
    { /* end of list */ },
};
MODULE_DEVICE_TABLE(of, antsdr_dma_of_match);

static struct platform_driver antsdr_dma_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = antsdr_dma_of_match,
    },
    .probe = antsdr_dma_probe,
    .remove = antsdr_dma_remove,
};

module_platform_driver(antsdr_dma_driver);

MODULE_AUTHOR("ANTSDR Team");
MODULE_DESCRIPTION("ANTSDR DMA Driver with Raw Data Fallback");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");