/**
 * @file antsdr_dma_remote_control.c
 * @brief ANTSDR DMA Remote Control Application - Waits for PC commands
 * 
 * This application starts in standby mode and waits for UDP commands from PC.
 * It handles mode changes properly by stopping streaming, changing mode, then restarting.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <pthread.h>

// IIO includes for AD9361 control
// Note: Using sysfs direct access for compatibility
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

/* ANTSDR packet protocol definitions */
#define ANTSDR_PROTOCOL_VERSION     1
#define ANTSDR_PACKET_START_MARKER  0xABCD1234
#define ANTSDR_PACKET_END_MARKER    0x5678DCBA
#define ANTSDR_MAX_PAYLOAD_SIZE     1360  /* 1400 - header size */

/* Packet header structure (40 bytes total) */
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
    uint32_t checksum;          /* CRC32 of payload data */
    uint32_t end_marker;        /* 0x5678DCBA - header end identifier */
} __attribute__((packed));

#define ANTSDR_PACKET_HEADER_SIZE   sizeof(struct antsdr_packet_header)

// AD9361 sysfs paths
#define AD9361_PHY_PATH "/sys/bus/iio/devices/iio:device1"
#define RX_LO_PATH AD9361_PHY_PATH "/out_altvoltage0_RX_LO_frequency"
#define TX_LO_PATH AD9361_PHY_PATH "/out_altvoltage1_TX_LO_frequency"

// RX Channel 0 paths
#define RX0_RF_BW_PATH AD9361_PHY_PATH "/in_voltage0_rf_bandwidth"
#define RX0_SAMPLE_RATE_PATH AD9361_PHY_PATH "/in_voltage_sampling_frequency"
#define RX0_RF_PORT_PATH AD9361_PHY_PATH "/in_voltage0_rf_port_select"
#define RX0_GAIN_CONTROL_PATH AD9361_PHY_PATH "/in_voltage0_gain_control_mode"
#define RX0_HARDWAREGAIN_PATH AD9361_PHY_PATH "/in_voltage0_hardwaregain"

// RX Channel 1 paths
#define RX1_RF_BW_PATH AD9361_PHY_PATH "/in_voltage1_rf_bandwidth"
#define RX1_RF_PORT_PATH AD9361_PHY_PATH "/in_voltage1_rf_port_select"
#define RX1_GAIN_CONTROL_PATH AD9361_PHY_PATH "/in_voltage1_gain_control_mode"
#define RX1_HARDWAREGAIN_PATH AD9361_PHY_PATH "/in_voltage1_hardwaregain"

// TX Channel 0 paths
#define TX0_RF_BW_PATH AD9361_PHY_PATH "/out_voltage0_rf_bandwidth"
#define TX0_SAMPLE_RATE_PATH AD9361_PHY_PATH "/out_voltage_sampling_frequency"
#define TX0_RF_PORT_PATH AD9361_PHY_PATH "/out_voltage0_rf_port_select"
#define TX0_HARDWAREGAIN_PATH AD9361_PHY_PATH "/out_voltage0_hardwaregain"

// TX Channel 1 paths
#define TX1_RF_BW_PATH AD9361_PHY_PATH "/out_voltage1_rf_bandwidth"
#define TX1_RF_PORT_PATH AD9361_PHY_PATH "/out_voltage1_rf_port_select"
#define TX1_HARDWAREGAIN_PATH AD9361_PHY_PATH "/out_voltage1_hardwaregain"

// Global settings paths
#define ENSM_MODE_PATH AD9361_PHY_PATH "/ensm_mode"
#define CALIB_MODE_PATH AD9361_PHY_PATH "/calib_mode"
#define FILTER_FIR_RX_PATH AD9361_PHY_PATH "/in_voltage_filter_fir_en"
#define FILTER_FIR_TX_PATH AD9361_PHY_PATH "/out_voltage_filter_fir_en"
#define DCXO_TUNE_COARSE_PATH AD9361_PHY_PATH "/dcxo_tune_coarse"
#define DCXO_TUNE_FINE_PATH AD9361_PHY_PATH "/dcxo_tune_fine"

// Available options paths (read-only)
#define RX_RF_PORT_AVAILABLE_PATH AD9361_PHY_PATH "/in_voltage0_rf_port_select_available"
#define TX_RF_PORT_AVAILABLE_PATH AD9361_PHY_PATH "/out_voltage0_rf_port_select_available"
#define GAIN_CONTROL_MODE_AVAILABLE_PATH AD9361_PHY_PATH "/in_voltage0_gain_control_mode_available"
#define ENSM_MODE_AVAILABLE_PATH AD9361_PHY_PATH "/ensm_mode_available"

#define DEVICE_NAME "/dev/antsdr_dma"

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

#define DEFAULT_CONTROL_PORT 12346
#define MAX_COMMAND_LEN 256

/* Fixed transfer size - must match driver */
#define FIXED_TRANSFER_SIZE (512 * 4)   /* Fixed at 512 words = 2048 bytes */
#define SHORT_PULSE_SIZE FIXED_TRANSFER_SIZE   /* Legacy compatibility */
#define LONG_PULSE_SIZE FIXED_TRANSFER_SIZE    /* Legacy compatibility */
#define DEFAULT_BUFFER_SIZE FIXED_TRANSFER_SIZE

// Helper macros for frequency conversion
#define MHZ(x) ((long long)(x*1000000.0 + .5))
#define GHZ(x) ((long long)(x*1000000000.0 + .5))

#define IIO_ENSURE(expr) { \
    if (!(expr)) { \
        printf("IIO assertion failed (%s:%d): %s\n", __FILE__, __LINE__, #expr); \
        return -1; \
    } \
}

// RX is input, TX is output
enum iodev { RX, TX };

// RF streaming configuration
struct rf_config {
    // RX Channel 0 configuration
    long long rx_bw_hz;      // RX bandwidth in Hz
    long long rx_fs_hz;      // RX sample rate in Hz  
    long long rx_lo_hz;      // RX LO frequency in Hz
    const char* rx_rfport;   // RX port name
    const char* rx_gain_mode; // RX gain control mode ("manual", "slow_attack", "fast_attack")
    double rx_gain_db;       // RX hardware gain in dB
    
    // TX Channel 0 configuration
    long long tx_bw_hz;      // TX bandwidth in Hz
    long long tx_fs_hz;      // TX sample rate in Hz
    long long tx_lo_hz;      // TX LO frequency in Hz
    const char* tx_rfport;   // TX port name
    double tx_gain_db;       // TX hardware gain in dB
    
    // Global settings
    int tx_enabled;          // TX enable flag
    const char* ensm_mode;   // ENSM mode ("sleep", "alert", "fdd", "tdd")
    int fir_filter_en;       // FIR filter enable (0=disabled, 1=enabled)
    int dcxo_tune_coarse;    // DCXO coarse tune
    int dcxo_tune_fine;      // DCXO fine tune
};

// Function declarations
static int write_sysfs_string(const char *path, const char *value);
static int write_sysfs_longlong(const char *path, long long value);
static int read_sysfs_string(const char *path, char *buffer, size_t size);
int configure_rf_parameters(struct rf_config *cfg);
int verify_rf_parameters(struct rf_config *cfg);

/* UDP destination structure - matches driver */
struct antsdr_udp_dest {
    unsigned int ip;
    unsigned short port;
};

/* DMA statistics structure - matches driver */
struct antsdr_dma_stats {
    unsigned long transfers_completed;
    unsigned long bytes_transferred;
    unsigned long udp_packets_sent;
    unsigned long errors;
    unsigned long valid_frames;
    unsigned long invalid_frames;
    unsigned long extracted_frames;
};

typedef enum {
    STATE_STANDBY,      // Device ready, waiting for commands
    STATE_STREAMING,    // Actively streaming data
    STATE_STOPPING,     // In the process of stopping
    STATE_CHANGING_MODE // Changing operation mode
} app_state_t;

static int device_fd = -1;
static volatile int keep_running = 1;
static volatile app_state_t current_state = STATE_STANDBY;
static int control_sock = -1;
static pthread_t control_thread;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

// Forward declarations
static void cleanup_rf_context(void);

// Current configuration
static uint32_t current_buffer_size = 2048;
static uint32_t current_mode = 0;  // 0=real data, 1=simulation
static uint32_t current_pulse_mode = 0;
static uint32_t current_tdd_mode = 0;
static struct sockaddr_in current_dest = {0};
static int dest_configured = 0;

// RF configuration via sysfs
static struct rf_config rf_cfg = {
    .rx_bw_hz = MHZ(15),         // 15 MHz RX bandwidth
    .rx_fs_hz = MHZ(30),         // 30 MS/s RX sample rate
    .rx_lo_hz = GHZ(5.0),        // 5.0 GHz RX frequency
    .rx_rfport = "A_BALANCED",   // RX port A balanced
    .rx_gain_mode = "slow_attack", // RX AGC mode
    .rx_gain_db = 30.0,          // 30 dB RX gain (for manual mode)
    
    .tx_bw_hz = MHZ(15),         // 15 MHz TX bandwidth  
    .tx_fs_hz = MHZ(30),         // 30 MS/s TX sample rate
    .tx_lo_hz = GHZ(5.0),        // 5.0 GHz TX frequency
    .tx_rfport = "A",            // TX port A
    .tx_gain_db = -10.0,         // -10 dB TX gain
    
    .tx_enabled = 0,             // TX disabled by default
    .ensm_mode = "fdd",          // FDD mode
    .fir_filter_en = 1,          // FIR filter enabled
    .dcxo_tune_coarse = 8,       // DCXO coarse tune default
    .dcxo_tune_fine = 5920       // DCXO fine tune default
};
static int rf_configured = 0;

// AD9361 sysfs paths
#define AD9361_PHY_PATH "/sys/bus/iio/devices/iio:device1"
#define RX_LO_PATH AD9361_PHY_PATH "/out_altvoltage0_RX_LO_frequency"
#define TX_LO_PATH AD9361_PHY_PATH "/out_altvoltage1_TX_LO_frequency"
#define RX0_RF_BW_PATH AD9361_PHY_PATH "/in_voltage0_rf_bandwidth"
#define RX0_SAMPLE_RATE_PATH AD9361_PHY_PATH "/in_voltage_sampling_frequency"
#define RX0_RF_PORT_PATH AD9361_PHY_PATH "/in_voltage0_rf_port_select"
#define TX0_RF_BW_PATH AD9361_PHY_PATH "/out_voltage0_rf_bandwidth"
#define TX0_SAMPLE_RATE_PATH AD9361_PHY_PATH "/out_voltage_sampling_frequency"
#define TX0_RF_PORT_PATH AD9361_PHY_PATH "/out_voltage0_rf_port_select"

static void signal_handler(int signo)
{
    printf("\nReceived signal %d, shutting down...\n", signo);
    keep_running = 0;
    
    pthread_mutex_lock(&state_mutex);
    if (current_state == STATE_STREAMING) {
        current_state = STATE_STOPPING;
        ioctl(device_fd, ANTSDR_IOC_STOP_STREAMING, 0);
    }
    pthread_mutex_unlock(&state_mutex);
    
    cleanup_rf_context();  // Clean up on signal
}

// Function to configure AD9361 RF parameters via sysfs
int configure_rf_parameters(struct rf_config *cfg) {
    char value_str[64];
    
    printf("Configuring AD9361 RF parameters:\n");
    
    // Configure RX LO frequency
    snprintf(value_str, sizeof(value_str), "%lld", cfg->rx_lo_hz);
    if (write_sysfs_string(RX_LO_PATH, value_str) < 0) {
        printf("Failed to set RX LO frequency\n");
        return -1;
    }
    printf("Set RX LO frequency: %lld Hz\n", cfg->rx_lo_hz);
    
    // Configure TX LO frequency  
    snprintf(value_str, sizeof(value_str), "%lld", cfg->tx_lo_hz);
    if (write_sysfs_string(TX_LO_PATH, value_str) < 0) {
        printf("Failed to set TX LO frequency\n");
        return -1;
    }
    printf("Set TX LO frequency: %lld Hz\n", cfg->tx_lo_hz);
    
    // Configure RX bandwidth
    snprintf(value_str, sizeof(value_str), "%lld", cfg->rx_bw_hz);
    if (write_sysfs_string(RX0_RF_BW_PATH, value_str) < 0) {
        printf("Failed to set RX bandwidth\n");
        return -1;
    }
    printf("Set RX bandwidth: %lld Hz\n", cfg->rx_bw_hz);
    
    // Configure RX sample rate
    snprintf(value_str, sizeof(value_str), "%lld", cfg->rx_fs_hz);
    if (write_sysfs_string(RX0_SAMPLE_RATE_PATH, value_str) < 0) {
        printf("Failed to set RX sample rate\n");
        return -1;
    }
    printf("Set RX sample rate: %lld Hz\n", cfg->rx_fs_hz);
    
    // Configure RX port
    if (write_sysfs_string(RX0_RF_PORT_PATH, cfg->rx_rfport) < 0) {
        printf("Failed to set RX port\n");
        return -1;
    }
    printf("Set RX port: %s\n", cfg->rx_rfport);
    
    // Configure RX gain mode
    if (write_sysfs_string(RX0_GAIN_CONTROL_PATH, cfg->rx_gain_mode) < 0) {
        printf("Failed to set RX gain mode\n");
        return -1;
    }
    printf("Set RX gain mode: %s\n", cfg->rx_gain_mode);
    
    // Configure RX gain (if in manual mode)
    if (strcmp(cfg->rx_gain_mode, "manual") == 0) {
        snprintf(value_str, sizeof(value_str), "%.2f", cfg->rx_gain_db);
        if (write_sysfs_string(RX0_HARDWAREGAIN_PATH, value_str) < 0) {
            printf("Failed to set RX gain\n");
            return -1;
        }
        printf("Set RX gain: %.2f dB\n", cfg->rx_gain_db);
    }
    
    // Configure TX bandwidth
    snprintf(value_str, sizeof(value_str), "%lld", cfg->tx_bw_hz);
    if (write_sysfs_string(TX0_RF_BW_PATH, value_str) < 0) {
        printf("Failed to set TX bandwidth\n");
        return -1;
    }
    printf("Set TX bandwidth: %lld Hz\n", cfg->tx_bw_hz);
    
    // Configure TX sample rate
    snprintf(value_str, sizeof(value_str), "%lld", cfg->tx_fs_hz);
    if (write_sysfs_string(TX0_SAMPLE_RATE_PATH, value_str) < 0) {
        printf("Failed to set TX sample rate\n");
        return -1;
    }
    printf("Set TX sample rate: %lld Hz\n", cfg->tx_fs_hz);
    
    // Configure TX port
    if (write_sysfs_string(TX0_RF_PORT_PATH, cfg->tx_rfport) < 0) {
        printf("Failed to set TX port\n");
        return -1;
    }
    printf("Set TX port: %s\n", cfg->tx_rfport);
    
    // Configure TX gain
    snprintf(value_str, sizeof(value_str), "%.2f", cfg->tx_gain_db);
    if (write_sysfs_string(TX0_HARDWAREGAIN_PATH, value_str) < 0) {
        printf("Failed to set TX gain\n");
        return -1;
    }
    printf("Set TX gain: %.2f dB\n", cfg->tx_gain_db);
    
    // Configure ENSM mode
    if (write_sysfs_string(ENSM_MODE_PATH, cfg->ensm_mode) < 0) {
        printf("Failed to set ENSM mode\n");
        return -1;
    }
    printf("Set ENSM mode: %s\n", cfg->ensm_mode);
    
    // Configure FIR filter (if path exists) - Note: Path may not be available
    // FIR filter control is typically done through device tree or driver initialization
    
    printf("RF parameter configuration complete.\n");
    return 0;
}

// Function to read and verify RF parameters from AD9361
int verify_rf_parameters(struct rf_config *cfg) {
    char value[256];
    long long read_value;
    
    printf("Verifying RF parameters:\n");
    
    // Verify RX bandwidth
    if (read_sysfs_string(RX0_RF_BW_PATH, value, sizeof(value)) == 0) {
        read_value = atoll(value);
        printf("RX Bandwidth: Set=%lld Hz, Read=%lld Hz %s\n", 
               cfg->rx_bw_hz, read_value, 
               (abs(read_value - cfg->rx_bw_hz) < 1000) ? "✓" : "✗");
    }
    
    // Verify RX sample rate
    if (read_sysfs_string(RX0_SAMPLE_RATE_PATH, value, sizeof(value)) == 0) {
        read_value = atoll(value);
        printf("RX Sample Rate: Set=%lld Hz, Read=%lld Hz %s\n", 
               cfg->rx_fs_hz, read_value,
               (abs(read_value - cfg->rx_fs_hz) < 1000) ? "✓" : "✗");
    }
    
    // Verify RX LO frequency
    if (read_sysfs_string(RX_LO_PATH, value, sizeof(value)) == 0) {
        read_value = atoll(value);
        printf("RX LO Frequency: Set=%lld Hz, Read=%lld Hz %s\n", 
               cfg->rx_lo_hz, read_value,
               (abs(read_value - cfg->rx_lo_hz) < 1000) ? "✓" : "✗");
    }
    
    // Verify RX port
    if (read_sysfs_string(RX0_RF_PORT_PATH, value, sizeof(value)) == 0) {
        printf("RX RF Port: Set=%s, Read=%s %s\n", 
               cfg->rx_rfport, value,
               (strcmp(value, cfg->rx_rfport) == 0) ? "✓" : "✗");
    }
    
    // Verify RX gain mode
    if (read_sysfs_string(RX0_GAIN_CONTROL_PATH, value, sizeof(value)) == 0) {
        printf("RX Gain Mode: Set=%s, Read=%s %s\n", 
               cfg->rx_gain_mode, value,
               (strcmp(value, cfg->rx_gain_mode) == 0) ? "✓" : "✗");
    }
    
    // Verify RX gain (if in manual mode)
    if (strcmp(cfg->rx_gain_mode, "manual") == 0) {
        if (read_sysfs_string(RX0_HARDWAREGAIN_PATH, value, sizeof(value)) == 0) {
            double read_gain = atof(value);
            printf("RX Gain: Set=%.2f dB, Read=%.2f dB %s\n", 
                   cfg->rx_gain_db, read_gain,
                   (fabs(read_gain - cfg->rx_gain_db) < 0.5) ? "✓" : "✗");
        }
    }
    
    // Verify TX bandwidth
    if (read_sysfs_string(TX0_RF_BW_PATH, value, sizeof(value)) == 0) {
        read_value = atoll(value);
        printf("TX Bandwidth: Set=%lld Hz, Read=%lld Hz %s\n", 
               cfg->tx_bw_hz, read_value,
               (abs(read_value - cfg->tx_bw_hz) < 1000) ? "✓" : "✗");
    }
    
    // Verify TX sample rate
    if (read_sysfs_string(TX0_SAMPLE_RATE_PATH, value, sizeof(value)) == 0) {
        read_value = atoll(value);
        printf("TX Sample Rate: Set=%lld Hz, Read=%lld Hz %s\n", 
               cfg->tx_fs_hz, read_value,
               (abs(read_value - cfg->tx_fs_hz) < 1000) ? "✓" : "✗");
    }
    
    // Verify TX LO frequency
    if (read_sysfs_string(TX_LO_PATH, value, sizeof(value)) == 0) {
        read_value = atoll(value);
        printf("TX LO Frequency: Set=%lld Hz, Read=%lld Hz %s\n", 
               cfg->tx_lo_hz, read_value,
               (abs(read_value - cfg->tx_lo_hz) < 1000) ? "✓" : "✗");
    }
    
    // Verify TX port
    if (read_sysfs_string(TX0_RF_PORT_PATH, value, sizeof(value)) == 0) {
        printf("TX RF Port: Set=%s, Read=%s %s\n", 
               cfg->tx_rfport, value,
               (strcmp(value, cfg->tx_rfport) == 0) ? "✓" : "✗");
    }
    
    // Verify TX gain
    if (read_sysfs_string(TX0_HARDWAREGAIN_PATH, value, sizeof(value)) == 0) {
        double read_gain = atof(value);
        printf("TX Gain: Set=%.2f dB, Read=%.2f dB %s\n", 
               cfg->tx_gain_db, read_gain,
               (fabs(read_gain - cfg->tx_gain_db) < 0.5) ? "✓" : "✗");
    }
    
    // Verify ENSM mode
    if (read_sysfs_string(ENSM_MODE_PATH, value, sizeof(value)) == 0) {
        printf("ENSM Mode: Set=%s, Read=%s %s\n", 
               cfg->ensm_mode, value,
               (strcmp(value, cfg->ensm_mode) == 0) ? "✓" : "✗");
    }
    
    printf("Parameter verification complete.\n\n");
    return 0;
}

static const char* state_to_string(app_state_t state)
{
    switch (state) {
        case STATE_STANDBY: return "STANDBY";
        case STATE_STREAMING: return "STREAMING";
        case STATE_STOPPING: return "STOPPING";
        case STATE_CHANGING_MODE: return "CHANGING_MODE";
        default: return "UNKNOWN";
    }
}

// Helper functions for AD9361 control via sysfs
static int write_sysfs_string(const char *path, const char *value)
{
    FILE *fp = fopen(path, "w");
    if (!fp) {
        printf("ERROR: Cannot open %s for writing\n", path);
        return -1;
    }
    
    if (fprintf(fp, "%s", value) < 0) {
        printf("ERROR: Failed to write '%s' to %s\n", value, path);
        fclose(fp);
        return -1;
    }
    
    fclose(fp);
    printf("DEBUG: Wrote '%s' to %s\n", value, path);
    return 0;
}

static int write_sysfs_longlong(const char *path, long long value)
{
    char str_value[32];
    snprintf(str_value, sizeof(str_value), "%lld", value);
    return write_sysfs_string(path, str_value);
}

static int read_sysfs_string(const char *path, char *buffer, size_t size)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("ERROR: Cannot open %s for reading\n", path);
        return -1;
    }
    
    if (!fgets(buffer, size, fp)) {
        printf("ERROR: Failed to read from %s\n", path);
        fclose(fp);
        return -1;
    }
    
    fclose(fp);
    
    // Remove trailing newline
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len-1] == '\n') {
        buffer[len-1] = '\0';
    }
    
    return 0;
}

static int configure_ad9361_rx(void)
{
    printf("DEBUG: Configuring AD9361 RX\n");
    
    // Set RX LO frequency
    if (write_sysfs_longlong(RX_LO_PATH, rf_cfg.rx_lo_hz) < 0) {
        printf("ERROR: Failed to set RX LO frequency\n");
        return -1;
    }
    
    // Set RX bandwidth
    if (write_sysfs_longlong(RX0_RF_BW_PATH, rf_cfg.rx_bw_hz) < 0) {
        printf("ERROR: Failed to set RX bandwidth\n");
        return -1;
    }
    
    // Set RX sample rate
    if (write_sysfs_longlong(RX0_SAMPLE_RATE_PATH, rf_cfg.rx_fs_hz) < 0) {
        printf("ERROR: Failed to set RX sample rate\n");
        return -1;
    }
    
    // Set RX port
    if (write_sysfs_string(RX0_RF_PORT_PATH, rf_cfg.rx_rfport) < 0) {
        printf("ERROR: Failed to set RX port\n");
        return -1;
    }
    
    printf("DEBUG: RX configured: LO=%lld Hz, BW=%lld Hz, FS=%lld Hz, Port=%s\n",
           rf_cfg.rx_lo_hz, rf_cfg.rx_bw_hz, rf_cfg.rx_fs_hz, rf_cfg.rx_rfport);
    
    return 0;
}

static int configure_ad9361_tx(void)
{
    if (!rf_cfg.tx_enabled) {
        printf("DEBUG: TX disabled, skipping TX configuration\n");
        return 0;
    }
    
    printf("DEBUG: Configuring AD9361 TX\n");
    
    // Set TX LO frequency
    if (write_sysfs_longlong(TX_LO_PATH, rf_cfg.tx_lo_hz) < 0) {
        printf("ERROR: Failed to set TX LO frequency\n");
        return -1;
    }
    
    // Set TX bandwidth
    if (write_sysfs_longlong(TX0_RF_BW_PATH, rf_cfg.tx_bw_hz) < 0) {
        printf("ERROR: Failed to set TX bandwidth\n");
        return -1;
    }
    
    // Set TX sample rate
    if (write_sysfs_longlong(TX0_SAMPLE_RATE_PATH, rf_cfg.tx_fs_hz) < 0) {
        printf("ERROR: Failed to set TX sample rate\n");
        return -1;
    }
    
    // Set TX port
    if (write_sysfs_string(TX0_RF_PORT_PATH, rf_cfg.tx_rfport) < 0) {
        printf("ERROR: Failed to set TX port\n");
        return -1;
    }
    
    printf("DEBUG: TX configured: LO=%lld Hz, BW=%lld Hz, FS=%lld Hz, Port=%s\n",
           rf_cfg.tx_lo_hz, rf_cfg.tx_bw_hz, rf_cfg.tx_fs_hz, rf_cfg.tx_rfport);
    
    return 0;
}

static int check_ad9361_available(void)
{
    char buffer[256];
    
    // Check if AD9361 PHY is available
    if (read_sysfs_string(AD9361_PHY_PATH "/name", buffer, sizeof(buffer)) < 0) {
        printf("ERROR: AD9361 PHY not found at %s\n", AD9361_PHY_PATH);
        return -1;
    }
    
    printf("DEBUG: Found AD9361 device: %s\n", buffer);
    return 0;
}

static int configure_rf_for_real_data(void)
{
    printf("DEBUG: Configuring RF for real data mode\n");
    
    if (check_ad9361_available() < 0) {
        printf("ERROR: AD9361 device not available\n");
        return -1;
    }
    
    // Configure RX
    if (configure_ad9361_rx() < 0) {
        printf("ERROR: Failed to configure RX\n");
        return -1;
    }
    
    // Configure TX if enabled
    if (configure_ad9361_tx() < 0) {
        printf("ERROR: Failed to configure TX\n");
        return -1;
    }
    
    rf_configured = 1;
    printf("DEBUG: RF configuration completed successfully\n");
    return 0;
}

static void cleanup_rf_context(void)
{
    printf("DEBUG: RF context cleanup\n");
    rf_configured = 0;
}

static void print_usage(const char *progname)
{
    printf("Usage: %s [options]\n", progname);
    printf("ANTSDR DMA Remote Control Application\n");
    printf("=====================================\n");
    printf("\nOptions:\n");
    printf("  -c CONTROL_PORT Set control port for UDP commands (default: 12346)\n");
    printf("  -v              Enable verbose logging\n");
    printf("  -h              Show this help\n");
    printf("\nRemote Control Commands (send as UDP packets):\n");
    printf("  setup_stream <ip> <port> <buffer_size> - Setup streaming parameters (buffer_size=2048)\n");
    printf("  start_stream                           - Enable data generation and start streaming\n");
    printf("  stop_stream                            - Stop streaming and disable data generation\n");
    printf("  set_mode <mode>                        - Change mode (0=real, 1=simulation)\n");
    printf("  get_mode                               - Get current operation mode\n");
    printf("  set_pulse_mode <0|1>                   - Enable/disable pulse mode (fixed 2048-byte transfers)\n");
    printf("  get_pulse_mode                         - Get current pulse mode status\n");
    printf("  set_tdd_mode <0|1>                     - Enable/disable TDD mode (fixed 2048-byte transfers)\n");
    printf("  get_tdd_mode                           - Get current TDD mode status\n");
    printf("  get_stats                              - Get streaming statistics\n");
    printf("  get_status                             - Get current device status\n");
    printf("  reset                                  - Reset device to standby state\n");
    printf("  ping                                   - Simple connectivity test\n");
    printf("\nRF Configuration Commands (for real data mode):\n");
    printf("  set_rx_freq <freq_hz>                  - Set RX LO frequency in Hz\n");
    printf("  set_tx_freq <freq_hz>                  - Set TX LO frequency in Hz\n");
    printf("  set_rx_bw <bw_hz>                      - Set RX bandwidth in Hz\n");
    printf("  set_tx_bw <bw_hz>                      - Set TX bandwidth in Hz\n");
    printf("  set_rx_fs <fs_hz>                      - Set RX sample rate in Hz\n");
    printf("  set_tx_fs <fs_hz>                      - Set TX sample rate in Hz\n");
    printf("  set_tx_enable <0|1>                    - Enable/disable TX transmission\n");
    printf("  get_rf_config                          - Get current RF configuration\n");
    printf("\nMode Change Protocol:\n");
    printf("  1. System automatically stops streaming when changing mode\n");
    printf("  2. Changes the operation mode\n");
    printf("  3. Restarts streaming if it was previously active\n");
    printf("\nResponse Format: All commands return JSON-like status responses\n");
    printf("\nExample Usage:\n");
    printf("  # Setup streaming to PC\n");
    printf("  echo 'setup_stream 192.168.1.125 12345 2048' | nc -u 192.168.1.12 12346\n");
    printf("  \n");
    printf("  # Start streaming with real data\n");
    printf("  echo 'start_stream' | nc -u 192.168.2.1 12346\n");
    printf("  \n");
    printf("  # Change to simulation mode\n");
    printf("  echo 'set_mode 1' | nc -u 192.168.2.1 12346\n");
    printf("  \n");
    printf("  # Stop streaming\n");
    printf("  echo 'stop_stream' | nc -u 192.168.2.1 12346\n");
}

static int setup_streaming_params(const char *dest_ip, int dest_port, uint32_t buffer_size)
{
    int ret;
    
    // // Validate parameters - buffer size must be fixed transfer size
    // if (buffer_size != FIXED_TRANSFER_SIZE) {
    //     printf("ERROR: Invalid buffer size %u (must be %d - fixed transfer size)\n", 
    //            buffer_size, FIXED_TRANSFER_SIZE);
    //     return -1;
    // }
    
    // Setup destination address
    memset(&current_dest, 0, sizeof(current_dest));
    current_dest.sin_family = AF_INET;
    current_dest.sin_port = htons(dest_port);
    if (inet_pton(AF_INET, dest_ip, &current_dest.sin_addr) != 1) {
        printf("ERROR: Invalid IP address: %s\n", dest_ip);
        return -1;
    }
    
    // // Set buffer size if different
    // if (buffer_size != current_buffer_size) {
    //     ret = ioctl(device_fd, ANTSDR_IOC_SET_BUFFER_SIZE, &buffer_size);
    //     if (ret < 0) {
    //         perror("Failed to set buffer size");
    //         return ret;
    //     }
    //     current_buffer_size = buffer_size;
    //     printf("Buffer size set to %u bytes\n", buffer_size);
    // }
    
    // Set destination in driver
    struct antsdr_udp_dest udp_dest;
    udp_dest.ip = current_dest.sin_addr.s_addr;
    udp_dest.port = ntohs(current_dest.sin_port);
    
    ret = ioctl(device_fd, ANTSDR_IOC_SET_UDP_DEST, &udp_dest);
    if (ret < 0) {
        perror("Failed to set UDP destination");
        return ret;
    }
    
    dest_configured = 1;
    printf("Streaming configured: %s:%d, buffer: %u bytes\n", dest_ip, dest_port, buffer_size);
    return 0;
}

static int start_streaming(void)
{
    int ret;
    
    pthread_mutex_lock(&state_mutex);
    
    if (current_state == STATE_STREAMING) {
        pthread_mutex_unlock(&state_mutex);
        printf("Already streaming\n");
        return 0;
    }
    
    if (!dest_configured) {
        pthread_mutex_unlock(&state_mutex);
        printf("ERROR: Streaming not configured. Use setup_stream first.\n");
        return -1;
    }
    
    current_state = STATE_STREAMING;
    pthread_mutex_unlock(&state_mutex);

    // Don't start data reader thread - let driver handle UDP streaming
    printf("NOTE: Using driver UDP streaming, no data reader thread needed\n");
    
    ret = ioctl(device_fd, ANTSDR_IOC_START_STREAMING, 0);
    if (ret < 0) {
        perror("Failed to start streaming");
        pthread_mutex_lock(&state_mutex);
        current_state = STATE_STANDBY;
        pthread_mutex_unlock(&state_mutex);
        return ret;
    }

    printf("Started streaming (mode=%u) with driver UDP streaming\n", current_mode);
    return 0;
}

static int stop_streaming(void)
{
    int ret;
    
    pthread_mutex_lock(&state_mutex);
    
    if (current_state == STATE_STANDBY) {
        pthread_mutex_unlock(&state_mutex);
        printf("Already stopped\n");
        return 0;
    }
    
    current_state = STATE_STOPPING;
    pthread_mutex_unlock(&state_mutex);
    
    ret = ioctl(device_fd, ANTSDR_IOC_STOP_STREAMING, 0);
    if (ret < 0) {
        perror("Failed to stop streaming");
    } else {
        printf("Stopped streaming\n");
    }
    
    // No data reader thread to stop - using driver UDP streaming
    
    pthread_mutex_lock(&state_mutex);
    current_state = STATE_STANDBY;
    pthread_mutex_unlock(&state_mutex);
    
    return ret;
}

static int change_mode(uint32_t new_mode)
{
    int ret;
    int was_streaming = 0;
    
    if (new_mode > 1) {
        printf("ERROR: Invalid mode %u (must be 0 or 1)\n", new_mode);
        return -1;
    }
    
    if (new_mode == current_mode) {
        printf("Mode already set to %u\n", new_mode);
        return 0;
    }
    
    pthread_mutex_lock(&state_mutex);
    
    // Check if we were streaming
    if (current_state == STATE_STREAMING) {
        was_streaming = 1;
        current_state = STATE_CHANGING_MODE;
        pthread_mutex_unlock(&state_mutex);
        
        // Stop streaming first
        printf("Stopping streaming to change mode...\n");
        ret = ioctl(device_fd, ANTSDR_IOC_STOP_STREAMING, 0);
        if (ret < 0) {
            perror("Failed to stop streaming for mode change");
            pthread_mutex_lock(&state_mutex);
            current_state = STATE_STANDBY;
            pthread_mutex_unlock(&state_mutex);
            return ret;
        }
        
        // Brief delay to ensure clean stop
        usleep(100000); // 100ms
    } else {
        pthread_mutex_unlock(&state_mutex);
    }
    
    // Change the mode
    ret = ioctl(device_fd, ANTSDR_IOC_SET_MODE, &new_mode);
    if (ret < 0) {
        perror("Failed to set operation mode");
        pthread_mutex_lock(&state_mutex);
        current_state = was_streaming ? STATE_STANDBY : current_state;
        pthread_mutex_unlock(&state_mutex);
        return ret;
    }
    
    current_mode = new_mode;
    printf("Operation mode changed to %u (%s)\n", new_mode, new_mode ? "simulation" : "real data");
    
    // Configure RF if switching to real data mode (mode 0)
    if (new_mode == 0) {
        printf("DEBUG: Switching to real data mode - configuring RF\n");
        if (configure_rf_for_real_data() < 0) {
            printf("WARNING: RF configuration failed, but continuing with mode change\n");
        }
    } else {
        printf("DEBUG: Switching to simulation mode - RF configuration not needed\n");
        // Clean up RF context when not in real data mode
        cleanup_rf_context();
    }
    
    // Restart streaming if we were streaming before
    if (was_streaming) {
        printf("Restarting streaming with new mode...\n");
        // Brief delay to ensure mode change is applied
        usleep(100000); // 100ms
        
        ret = ioctl(device_fd, ANTSDR_IOC_START_STREAMING, 0);
        if (ret < 0) {
            perror("Failed to restart streaming after mode change");
            pthread_mutex_lock(&state_mutex);
            current_state = STATE_STANDBY;
            pthread_mutex_unlock(&state_mutex);
            return ret;
        }
        
        pthread_mutex_lock(&state_mutex);
        current_state = STATE_STREAMING;
        pthread_mutex_unlock(&state_mutex);
        
        printf("Streaming restarted with mode %u\n", new_mode);
    } else {
        pthread_mutex_lock(&state_mutex);
        current_state = STATE_STANDBY;
        pthread_mutex_unlock(&state_mutex);
    }
    
    return 0;
}

static void process_control_command(const char *command, struct sockaddr_in *client_addr)
{
    char response[512];
    char action[32];
    char dest_ip[16];
    uint16_t dest_port;
    uint32_t buffer_size, mode;
    struct antsdr_dma_stats stats;
    int ret;
    
    // Parse command
    if (sscanf(command, "%31s", action) != 1) {
        snprintf(response, sizeof(response), "ERROR: Invalid command format\n");
        goto send_response;
    }
    
    printf("Received command: %s\n", command);
    
    if (strcmp(action, "ping") == 0) {
        snprintf(response, sizeof(response), "PONG: Device ready, state=%s\n", state_to_string(current_state));
        
    } else if (strcmp(action, "setup_stream") == 0) {
        if (sscanf(command, "%31s %15s %hu %u", action, dest_ip, &dest_port, &buffer_size) == 4) {
            ret = setup_streaming_params(dest_ip, dest_port, buffer_size);
            snprintf(response, sizeof(response), "SETUP_STREAM: %s (%s:%u, %u bytes)\n", 
                     ret == 0 ? "OK" : "FAILED", dest_ip, dest_port, buffer_size);
        } else {
            snprintf(response, sizeof(response), "ERROR: setup_stream requires <ip> <port> <buffer_size>\n");
        }
        
    } else if (strcmp(action, "start_stream") == 0) {
        ret = start_streaming();
        snprintf(response, sizeof(response), "START_STREAM: %s\n", ret == 0 ? "OK" : "FAILED");
        
    } else if (strcmp(action, "stop_stream") == 0) {
        ret = stop_streaming();
        snprintf(response, sizeof(response), "STOP_STREAM: %s\n", ret == 0 ? "OK" : "FAILED");
        
    } else if (strcmp(action, "set_mode") == 0) {
        if (sscanf(command, "%31s %u", action, &mode) == 2) {
            ret = change_mode(mode);
            snprintf(response, sizeof(response), "SET_MODE: %s (mode=%u)\n", 
                     ret == 0 ? "OK" : "FAILED", mode);
        } else {
            snprintf(response, sizeof(response), "ERROR: set_mode requires mode parameter (0 or 1)\n");
        }
        
    } else if (strcmp(action, "get_mode") == 0) {
        snprintf(response, sizeof(response), "MODE: %u (%s)\n", current_mode, 
                 current_mode ? "simulation" : "real_data");
        
    } else if (strcmp(action, "get_stats") == 0) {
        ret = ioctl(device_fd, ANTSDR_IOC_GET_STATS, &stats);
        if (ret == 0) {
            snprintf(response, sizeof(response), 
                     "STATS: bytes=%lu packets=%lu completions=%lu errors=%lu valid=%lu invalid=%lu extracted=%lu\n",
                     stats.bytes_transferred, stats.udp_packets_sent,
                     stats.transfers_completed, stats.errors,
                     stats.valid_frames, stats.invalid_frames, stats.extracted_frames);
        } else {
            snprintf(response, sizeof(response), "ERROR: Failed to get statistics\n");
        }
        
    } else if (strcmp(action, "get_status") == 0) {
        pthread_mutex_lock(&state_mutex);
        app_state_t state = current_state;
        pthread_mutex_unlock(&state_mutex);
        
        snprintf(response, sizeof(response), 
                 "STATUS: state=%s mode=%u buffer=%u dest_configured=%s\n",
                 state_to_string(state), current_mode, current_buffer_size,
                 dest_configured ? "yes" : "no");
        
    } else if (strcmp(action, "reset") == 0) {
        // Stop streaming and reset to initial state
        stop_streaming();
        current_mode = 0;
        current_pulse_mode = 0;
        current_tdd_mode = 0;
        dest_configured = 0;
        ioctl(device_fd, ANTSDR_IOC_SET_MODE, &current_mode);
        ioctl(device_fd, ANTSDR_IOC_SET_PULSE_MODE, &current_pulse_mode);
        ioctl(device_fd, ANTSDR_IOC_SET_TDD_MODE, &current_tdd_mode);
        cleanup_rf_context(); // Clean up RF configuration
        snprintf(response, sizeof(response), "RESET: OK (back to standby mode)\n");
        
    } else if (strcmp(action, "set_rx_freq") == 0) {
        long long freq_hz;
        if (sscanf(command, "%31s %lld", action, &freq_hz) == 2) {
            rf_cfg.rx_lo_hz = freq_hz;
            if (current_mode == 0 && rf_configured) {
                ret = configure_ad9361_rx();
                snprintf(response, sizeof(response), "SET_RX_FREQ: %s (%lld Hz)\n", ret == 0 ? "OK" : "FAILED", freq_hz);
            } else {
                snprintf(response, sizeof(response), "SET_RX_FREQ: OK (stored, will apply in real data mode)\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_rx_freq requires frequency in Hz\n");
        }
        
    } else if (strcmp(action, "set_tx_freq") == 0) {
        long long freq_hz;
        if (sscanf(command, "%31s %lld", action, &freq_hz) == 2) {
            rf_cfg.tx_lo_hz = freq_hz;
            if (current_mode == 0 && rf_configured && rf_cfg.tx_enabled) {
                ret = configure_ad9361_tx();
                snprintf(response, sizeof(response), "SET_TX_FREQ: %s (%lld Hz)\n", ret == 0 ? "OK" : "FAILED", freq_hz);
            } else {
                snprintf(response, sizeof(response), "SET_TX_FREQ: OK (stored, will apply in real data mode)\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_tx_freq requires frequency in Hz\n");
        }
        
    } else if (strcmp(action, "set_rx_bw") == 0) {
        long long bw_hz;
        if (sscanf(command, "%31s %lld", action, &bw_hz) == 2) {
            rf_cfg.rx_bw_hz = bw_hz;
            if (current_mode == 0 && rf_configured) {
                ret = configure_ad9361_rx();
                snprintf(response, sizeof(response), "SET_RX_BW: %s (%lld Hz)\n", ret == 0 ? "OK" : "FAILED", bw_hz);
            } else {
                snprintf(response, sizeof(response), "SET_RX_BW: OK (stored, will apply in real data mode)\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_rx_bw requires bandwidth in Hz\n");
        }
        
    } else if (strcmp(action, "set_tx_bw") == 0) {
        long long bw_hz;
        if (sscanf(command, "%31s %lld", action, &bw_hz) == 2) {
            rf_cfg.tx_bw_hz = bw_hz;
            if (current_mode == 0 && rf_configured && rf_cfg.tx_enabled) {
                ret = configure_ad9361_tx();
                snprintf(response, sizeof(response), "SET_TX_BW: %s (%lld Hz)\n", ret == 0 ? "OK" : "FAILED", bw_hz);
            } else {
                snprintf(response, sizeof(response), "SET_TX_BW: OK (stored, will apply in real data mode)\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_tx_bw requires bandwidth in Hz\n");
        }
        
    } else if (strcmp(action, "set_rx_fs") == 0) {
        long long fs_hz;
        if (sscanf(command, "%31s %lld", action, &fs_hz) == 2) {
            rf_cfg.rx_fs_hz = fs_hz;
            if (current_mode == 0 && rf_configured) {
                ret = configure_ad9361_rx();
                snprintf(response, sizeof(response), "SET_RX_FS: %s (%lld Hz)\n", ret == 0 ? "OK" : "FAILED", fs_hz);
            } else {
                snprintf(response, sizeof(response), "SET_RX_FS: OK (stored, will apply in real data mode)\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_rx_fs requires sample rate in Hz\n");
        }
        
    } else if (strcmp(action, "set_tx_fs") == 0) {
        long long fs_hz;
        if (sscanf(command, "%31s %lld", action, &fs_hz) == 2) {
            rf_cfg.tx_fs_hz = fs_hz;
            if (current_mode == 0 && rf_configured && rf_cfg.tx_enabled) {
                ret = configure_ad9361_tx();
                snprintf(response, sizeof(response), "SET_TX_FS: %s (%lld Hz)\n", ret == 0 ? "OK" : "FAILED", fs_hz);
            } else {
                snprintf(response, sizeof(response), "SET_TX_FS: OK (stored, will apply in real data mode)\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_tx_fs requires sample rate in Hz\n");
        }
        
    } else if (strcmp(action, "set_tx_enable") == 0) {
        uint32_t tx_enable;
        if (sscanf(command, "%31s %u", action, &tx_enable) == 2) {
            rf_cfg.tx_enabled = tx_enable ? 1 : 0;
            if (current_mode == 0 && rf_configured) {
                ret = configure_ad9361_tx();
                snprintf(response, sizeof(response), "SET_TX_ENABLE: %s (TX %s)\n", 
                         ret == 0 ? "OK" : "FAILED", rf_cfg.tx_enabled ? "enabled" : "disabled");
            } else {
                snprintf(response, sizeof(response), "SET_TX_ENABLE: OK (stored, will apply in real data mode)\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_tx_enable requires 0 or 1\n");
        }
        
    } else if (strcmp(action, "set_rx_gain_mode") == 0) {
        char gain_mode[32];
        if (sscanf(command, "%31s %31s", action, gain_mode) == 2) {
            if (strcmp(gain_mode, "manual") == 0 || strcmp(gain_mode, "slow_attack") == 0 || 
                strcmp(gain_mode, "fast_attack") == 0) {
                rf_cfg.rx_gain_mode = strdup(gain_mode);
                if (current_mode == 0 && rf_configured) {
                    ret = configure_rf_parameters(&rf_cfg);
                    snprintf(response, sizeof(response), "SET_RX_GAIN_MODE: %s (%s)\n", 
                             ret == 0 ? "OK" : "FAILED", gain_mode);
                } else {
                    snprintf(response, sizeof(response), "SET_RX_GAIN_MODE: OK (stored, will apply in real data mode)\n");
                }
            } else {
                snprintf(response, sizeof(response), "ERROR: Invalid gain mode. Use manual, slow_attack, or fast_attack\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_rx_gain_mode requires gain mode parameter\n");
        }
        
    } else if (strcmp(action, "set_rx_gain") == 0) {
        double gain_db;
        if (sscanf(command, "%31s %lf", action, &gain_db) == 2) {
            rf_cfg.rx_gain_db = gain_db;
            if (current_mode == 0 && rf_configured && strcmp(rf_cfg.rx_gain_mode, "manual") == 0) {
                ret = configure_rf_parameters(&rf_cfg);
                snprintf(response, sizeof(response), "SET_RX_GAIN: %s (%.2f dB)\n", 
                         ret == 0 ? "OK" : "FAILED", gain_db);
            } else {
                snprintf(response, sizeof(response), "SET_RX_GAIN: OK (stored, requires manual gain mode to apply)\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_rx_gain requires gain in dB\n");
        }
        
    } else if (strcmp(action, "set_tx_gain") == 0) {
        double gain_db;
        if (sscanf(command, "%31s %lf", action, &gain_db) == 2) {
            rf_cfg.tx_gain_db = gain_db;
            if (current_mode == 0 && rf_configured && rf_cfg.tx_enabled) {
                ret = configure_rf_parameters(&rf_cfg);
                snprintf(response, sizeof(response), "SET_TX_GAIN: %s (%.2f dB)\n", 
                         ret == 0 ? "OK" : "FAILED", gain_db);
            } else {
                snprintf(response, sizeof(response), "SET_TX_GAIN: OK (stored, will apply when TX enabled)\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_tx_gain requires gain in dB\n");
        }
        
    } else if (strcmp(action, "set_rx_port") == 0) {
        char rx_port[32];
        if (sscanf(command, "%31s %31s", action, rx_port) == 2) {
            rf_cfg.rx_rfport = strdup(rx_port);
            if (current_mode == 0 && rf_configured) {
                ret = configure_rf_parameters(&rf_cfg);
                snprintf(response, sizeof(response), "SET_RX_PORT: %s (%s)\n", 
                         ret == 0 ? "OK" : "FAILED", rx_port);
            } else {
                snprintf(response, sizeof(response), "SET_RX_PORT: OK (stored, will apply in real data mode)\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_rx_port requires port name (A_BALANCED, B_BALANCED, etc.)\n");
        }
        
    } else if (strcmp(action, "set_tx_port") == 0) {
        char tx_port[32];
        if (sscanf(command, "%31s %31s", action, tx_port) == 2) {
            rf_cfg.tx_rfport = strdup(tx_port);
            if (current_mode == 0 && rf_configured && rf_cfg.tx_enabled) {
                ret = configure_rf_parameters(&rf_cfg);
                snprintf(response, sizeof(response), "SET_TX_PORT: %s (%s)\n", 
                         ret == 0 ? "OK" : "FAILED", tx_port);
            } else {
                snprintf(response, sizeof(response), "SET_TX_PORT: OK (stored, will apply when TX enabled)\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_tx_port requires port name (A, B)\n");
        }
        
    } else if (strcmp(action, "set_ensm_mode") == 0) {
        char ensm_mode[32];
        if (sscanf(command, "%31s %31s", action, ensm_mode) == 2) {
            if (strcmp(ensm_mode, "sleep") == 0 || strcmp(ensm_mode, "alert") == 0 || 
                strcmp(ensm_mode, "fdd") == 0 || strcmp(ensm_mode, "tdd") == 0) {
                rf_cfg.ensm_mode = strdup(ensm_mode);
                if (current_mode == 0 && rf_configured) {
                    ret = configure_rf_parameters(&rf_cfg);
                    snprintf(response, sizeof(response), "SET_ENSM_MODE: %s (%s)\n", 
                             ret == 0 ? "OK" : "FAILED", ensm_mode);
                } else {
                    snprintf(response, sizeof(response), "SET_ENSM_MODE: OK (stored, will apply in real data mode)\n");
                }
            } else {
                snprintf(response, sizeof(response), "ERROR: Invalid ENSM mode. Use sleep, alert, fdd, or tdd\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_ensm_mode requires mode parameter\n");
        }
        
    } else if (strcmp(action, "verify_rf_params") == 0) {
        if (current_mode == 0 && rf_configured) {
            ret = verify_rf_parameters(&rf_cfg);
            snprintf(response, sizeof(response), "VERIFY_RF_PARAMS: %s (check console output for details)\n", 
                     ret == 0 ? "OK" : "FAILED");
        } else {
            snprintf(response, sizeof(response), "VERIFY_RF_PARAMS: Not available (real data mode not active)\n");
        }
        
    } else if (strcmp(action, "configure_rf") == 0) {
        if (current_mode == 0) {
            ret = configure_rf_parameters(&rf_cfg);
            if (ret == 0) {
                rf_configured = 1;
                // Also verify the parameters after configuration
                verify_rf_parameters(&rf_cfg);
                snprintf(response, sizeof(response), "CONFIGURE_RF: OK (all parameters applied and verified)\n");
            } else {
                snprintf(response, sizeof(response), "CONFIGURE_RF: FAILED\n");
            }
        } else {
            snprintf(response, sizeof(response), "CONFIGURE_RF: Not available (only in real data mode)\n");
        }
        
    } else if (strcmp(action, "get_rf_config") == 0) {
        snprintf(response, sizeof(response), 
                 "RF_CONFIG: RX_FREQ=%lld RX_BW=%lld RX_FS=%lld RX_GAIN_MODE=%s RX_GAIN=%.2f RX_PORT=%s "
                 "TX_FREQ=%lld TX_BW=%lld TX_FS=%lld TX_GAIN=%.2f TX_PORT=%s TX_EN=%d ENSM=%s\n",
                 rf_cfg.rx_lo_hz, rf_cfg.rx_bw_hz, rf_cfg.rx_fs_hz, rf_cfg.rx_gain_mode, rf_cfg.rx_gain_db, rf_cfg.rx_rfport,
                 rf_cfg.tx_lo_hz, rf_cfg.tx_bw_hz, rf_cfg.tx_fs_hz, rf_cfg.tx_gain_db, rf_cfg.tx_rfport, 
                 rf_cfg.tx_enabled, rf_cfg.ensm_mode);
        
    } else if (strcmp(action, "set_pulse_mode") == 0) {
        uint32_t pulse_mode;
        if (sscanf(command, "%31s %u", action, &pulse_mode) == 2) {
            ret = ioctl(device_fd, ANTSDR_IOC_SET_PULSE_MODE, &pulse_mode);
            if (ret == 0) {
                current_pulse_mode = pulse_mode;
                snprintf(response, sizeof(response), "SET_PULSE_MODE: OK (pulse_mode=%s)\n", 
                         pulse_mode ? "enabled" : "disabled");
            } else {
                snprintf(response, sizeof(response), "SET_PULSE_MODE: FAILED\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_pulse_mode requires mode parameter (0 or 1)\n");
        }
        
    } else if (strcmp(action, "get_pulse_mode") == 0) {
        ret = ioctl(device_fd, ANTSDR_IOC_GET_PULSE_MODE, &current_pulse_mode);
        if (ret == 0) {
            snprintf(response, sizeof(response), "PULSE_MODE: %u (%s)\n", current_pulse_mode,
                     current_pulse_mode ? "enabled" : "disabled");
        } else {
            snprintf(response, sizeof(response), "ERROR: Failed to get pulse mode\n");
        }
        
    } else if (strcmp(action, "set_tdd_mode") == 0) {
        uint32_t tdd_mode;
        if (sscanf(command, "%31s %u", action, &tdd_mode) == 2) {
            ret = ioctl(device_fd, ANTSDR_IOC_SET_TDD_MODE, &tdd_mode);
            if (ret == 0) {
                current_tdd_mode = tdd_mode;
                snprintf(response, sizeof(response), "SET_TDD_MODE: OK (tdd_mode=%s)\n", 
                         tdd_mode ? "enabled" : "disabled");
            } else {
                snprintf(response, sizeof(response), "SET_TDD_MODE: FAILED\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: set_tdd_mode requires mode parameter (0 or 1)\n");
        }
        
    } else if (strcmp(action, "get_tdd_mode") == 0) {
        ret = ioctl(device_fd, ANTSDR_IOC_GET_TDD_MODE, &current_tdd_mode);
        if (ret == 0) {
            snprintf(response, sizeof(response), "TDD_MODE: %u (%s)\n", current_tdd_mode,
                     current_tdd_mode ? "enabled" : "disabled");
        } else {
            snprintf(response, sizeof(response), "ERROR: Failed to get TDD mode\n");
        }
        
    } else {
        snprintf(response, sizeof(response), "ERROR: Unknown command '%s'\n", action);
    }

send_response:
    printf("Response: %s", response);
    
    // Send response back to client
    if (control_sock >= 0) {
        sendto(control_sock, response, strlen(response), 0,
               (struct sockaddr *)client_addr, sizeof(*client_addr));
    }
}

static void *control_thread_func(void *arg)
{
    int control_port = *(int *)arg;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[MAX_COMMAND_LEN];
    int ret;
    
    control_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (control_sock < 0) {
        perror("Failed to create control socket");
        return NULL;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(control_port);
    
    if (bind(control_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to bind control socket");
        close(control_sock);
        control_sock = -1;
        return NULL;
    }
    
    printf("ANTSDR Remote Control ready on port %d\n", control_port);
    printf("Waiting for PC commands...\n");
    
    while (keep_running) {
        fd_set readfds;
        struct timeval timeout;
        
        FD_ZERO(&readfds);
        FD_SET(control_sock, &readfds);
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        ret = select(control_sock + 1, &readfds, NULL, NULL, &timeout);
        if (ret < 0) {
            if (errno != EINTR) {
                perror("Control socket select error");
                break;
            }
            continue;
        }
        
        if (ret > 0 && FD_ISSET(control_sock, &readfds)) {
            ssize_t received = recvfrom(control_sock, buffer, sizeof(buffer) - 1, 0,
                                      (struct sockaddr *)&client_addr, &client_len);
            if (received > 0) {
                buffer[received] = '\0';
                // Remove trailing newline if present
                if (buffer[received - 1] == '\n') {
                    buffer[received - 1] = '\0';
                }
                process_control_command(buffer, &client_addr);
            }
        }
    }
    
    close(control_sock);
    control_sock = -1;
    return NULL;
}

int main(int argc, char *argv[])
{
    int control_port = DEFAULT_CONTROL_PORT;
    int verbose = 0;
    int opt;
    int ret;
    
    while ((opt = getopt(argc, argv, "c:vh")) != -1) {
        switch (opt) {
        case 'c':
            control_port = atoi(optarg);
            if (control_port <= 0 || control_port > 65535) {
                fprintf(stderr, "Invalid control port: %s\n", optarg);
                return 1;
            }
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("ANTSDR DMA Remote Control Application\n");
    printf("====================================\n");
    printf("Control Port: %d\n", control_port);
    printf("Verbose Mode: %s\n", verbose ? "enabled" : "disabled");
    printf("\n");
    
    // Open device
    device_fd = open(DEVICE_NAME, O_RDWR);
    if (device_fd < 0) {
        perror("Failed to open device");
        return 1;
    }
    
    printf("Device %s opened successfully\n", DEVICE_NAME);
    
    // Initialize device to known state
    current_mode = 0;
    ioctl(device_fd, ANTSDR_IOC_SET_MODE, &current_mode);
    
    printf("Device initialized: mode=%u\n", current_mode);
    printf("State: %s\n", state_to_string(current_state));
    printf("\n");
    
    // Start control thread
    ret = pthread_create(&control_thread, NULL, control_thread_func, &control_port);
    if (ret != 0) {
        fprintf(stderr, "Failed to create control thread: %s\n", strerror(ret));
        close(device_fd);
        return 1;
    }
    
    // Main loop - just monitor state and wait for signals
    while (keep_running) {
        sleep(5);
        
        if (verbose) {
            pthread_mutex_lock(&state_mutex);
            printf("Status: state=%s, mode=%u, dest_configured=%s\n",
                   state_to_string(current_state), current_mode, 
                   dest_configured ? "yes" : "no");
            pthread_mutex_unlock(&state_mutex);
        }
    }
    
    printf("\nShutting down...\n");
    
    // Clean shutdown
    if (current_state == STATE_STREAMING) {
        stop_streaming();
    }
    
    pthread_join(control_thread, NULL);
    cleanup_rf_context();  // Clean up RF resources
    close(device_fd);
    
    printf("Application terminated\n");
    return 0;
}