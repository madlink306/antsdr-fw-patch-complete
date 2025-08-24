# ANTSDR E200 Project Structure with PlutoSDR Submodule

## Overview

This project now includes the `plutosdr-fw` as a proper Git submodule, preserving all the optimizations and modifications made to the original PlutoSDR firmware while maintaining clean repository structure.

## Repository Structure

```
antsdr-fw-patch-complete/
├── plutosdr-fw/                    # Submodule: Modified PlutoSDR firmware
│   ├── linux/drivers/misc/antsdr_dma/  # High-performance DMA driver
│   ├── deploy_module.sh            # Automated deployment script
│   ├── udp_receiver.py             # Performance testing utility
│   ├── antsdr_remote_client.py     # Remote control client
│   └── [complete PlutoSDR fw]      # Full firmware build system
├── drivers/antsdr_dma/             # Local DMA driver (legacy)
├── antsdr_dma_test_enhanced.c      # Enhanced test application
├── README.md                       # Main project documentation
└── SUBMODULE_SETUP.md             # This file
```

## Submodule Information

- **Repository**: https://github.com/madlink306/plutosdr-fw-modified
- **Purpose**: Contains the complete modified PlutoSDR firmware optimized for ANTSDR E200
- **Key Features**:
  - High-performance DMA driver with stack overflow prevention
  - Dynamic memory allocation preventing kernel crashes
  - Threaded processing architecture
  - Comprehensive performance optimizations (50-90× throughput improvement)

## Working with the Submodule

### Cloning the Project

When cloning this repository for the first time:

```bash
# Clone with submodules
git clone --recursive https://github.com/madlink306/antsdr-fw-patch-complete.git

# Or clone first, then initialize submodules
git clone https://github.com/madlink306/antsdr-fw-patch-complete.git
cd antsdr-fw-patch-complete
git submodule update --init --recursive
```

### Updating the Submodule

To pull the latest changes from the submodule:

```bash
cd plutosdr-fw
git pull origin main
cd ..
git add plutosdr-fw
git commit -m "Update plutosdr-fw submodule"
```

### Accessing the Optimized Components

All the optimized firmware components are now accessible through the submodule:

```bash
# Access the high-performance DMA driver
cd plutosdr-fw/linux/drivers/misc/antsdr_dma/
ls antsdr_dma.c

# Use the deployment tools
./plutosdr-fw/deploy_module.sh

# Run performance tests
python3 plutosdr-fw/udp_receiver.py

# Use remote control
python3 plutosdr-fw/antsdr_remote_client.py
```

## Key Optimizations in PlutoSDR Submodule

### 1. DMA Driver Optimizations
- **File**: `plutosdr-fw/linux/drivers/misc/antsdr_dma/antsdr_dma.c`
- **Dynamic Memory Allocation**: Prevents stack overflow crashes
- **Frame Optimization**: 403-word frames matching FPGA output
- **Buffer Scaling**: 16 DMA buffers (4× increase)
- **Burst Optimization**: 64-word bursts (4× increase)
- **Batch Processing**: 50 frames per work invocation (5× increase)

### 2. Performance Metrics
- **Target Throughput**: 157.4 MB/s
- **Baseline**: 1.3 MB/s
- **Improvement**: 50-90× performance increase
- **Frame Rate**: 97,656 fps at 403-word frames

### 3. Support Tools
- **deploy_module.sh**: Automated kernel module deployment
- **udp_receiver.py**: Real-time performance monitoring
- **antsdr_remote_client.py**: Remote DMA control and testing

## Development Workflow

### Making Changes to PlutoSDR Firmware

1. **Work in the submodule**:
   ```bash
   cd plutosdr-fw
   # Make your changes
   git add .
   git commit -m "Your changes"
   git push origin main
   ```

2. **Update main project**:
   ```bash
   cd ..
   git add plutosdr-fw
   git commit -m "Update plutosdr-fw with new changes"
   git push origin main
   ```

### Building the Firmware

From the submodule directory:

```bash
cd plutosdr-fw
# Follow the standard PlutoSDR build process
# All our optimizations are preserved in this build
```

## Migration from Direct Files

Previously, the project had the PlutoSDR firmware files directly included. We've migrated to a submodule structure while preserving all modifications:

- **Before**: Direct files in project root
- **After**: Submodule pointing to dedicated repository
- **Benefit**: Cleaner structure, proper version control, easier maintenance

## Repository Links

- **Main Project**: https://github.com/madlink306/antsdr-fw-patch-complete
- **PlutoSDR Submodule**: https://github.com/madlink306/plutosdr-fw-modified
- **Original PlutoSDR**: https://github.com/analogdevicesinc/plutosdr-fw

## Verification

To verify the submodule is working correctly:

```bash
# Check submodule status
git submodule status

# Verify key files exist
ls plutosdr-fw/linux/drivers/misc/antsdr_dma/antsdr_dma.c
ls plutosdr-fw/deploy_module.sh
ls plutosdr-fw/udp_receiver.py

# Check that the optimized driver is accessible
head -20 plutosdr-fw/linux/drivers/misc/antsdr_dma/antsdr_dma.c
```

This setup ensures all your PlutoSDR firmware modifications are preserved and accessible while maintaining a clean and professional repository structure.
