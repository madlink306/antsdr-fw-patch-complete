# ANTSDR PC Remote Control Setup Guide

## Overview
The enhanced ANTSDR test application now supports comprehensive PC-side remote control for mode setup and transfer operations. This allows you to control the ANTSDR device from your PC without manual intervention on the board.

## Device Setup (ANTSDR Board Side)

### 1. Deploy Updated Firmware
```bash
# Copy the updated driver and test application to the board
scp build/antsdr_dma.ko root@192.168.1.12:/lib/modules/$(uname -r)/kernel/drivers/
scp antsdr_dma_test_enhanced root@192.168.1.12:/usr/bin/
```

### 2. Load the Driver
```bash
# On the ANTSDR board
insmod /lib/modules/$(uname -r)/kernel/drivers/antsdr_dma.ko
```

### 3. Start Remote Control Daemon
```bash
# On the ANTSDR board - run in daemon mode
./antsdr_dma_test_enhanced -D -c 12346

# Or with specific settings
./antsdr_dma_test_enhanced -D -m 0 -c 12346
```

## PC Control Methods

### Method 1: Python Client (Recommended)
```bash
# Get device information
python3 antsdr_remote_client.py 192.168.1.12 info

# Set operation mode
python3 antsdr_remote_client.py 192.168.1.12 set_mode 1

# Configure transfer settings
python3 antsdr_remote_client.py 192.168.1.12 set_buffer 4096
python3 antsdr_remote_client.py 192.168.1.12 set_dest 192.168.1.100 12345

# Start streaming
python3 antsdr_remote_client.py 192.168.1.12 start

# Monitor statistics
python3 antsdr_remote_client.py 192.168.1.12 monitor 30

# Stop streaming
python3 antsdr_remote_client.py 192.168.1.12 stop

# Run complete test sequence
python3 antsdr_remote_client.py 192.168.1.12 test_sequence
```

### Method 2: Shell Script
```bash
# Get status
./antsdr_control.sh 192.168.1.12 status

# Set mode and start
./antsdr_control.sh 192.168.1.12 set_mode 1
./antsdr_control.sh 192.168.1.12 start

# Get statistics
./antsdr_control.sh 192.168.1.12 stats

# Stop and reset
./antsdr_control.sh 192.168.1.12 stop
./antsdr_control.sh 192.168.1.12 reset
```

### Method 3: Direct UDP Commands
```bash
# Using netcat directly
echo "info" | nc -u 192.168.1.12 12346
echo "set_mode 1" | nc -u 192.168.1.12 12346
echo "start" | nc -u 192.168.1.12 12346
echo "get_stats" | nc -u 192.168.1.12 12346
```

## Available Commands

### Device Information
- `info` - Get device information and capabilities
- `status` - Get current streaming status, mode, and buffer size
- `get_mode` - Get current operation mode
- `get_stats` - Get transfer statistics

### Configuration Commands
- `set_mode <0|1>` - Set operation mode (0 or 1)
- `set_buffer <size>` - Set buffer size (512-65536 bytes, 4-byte aligned)
- `set_dest <ip> <port>` - Set UDP destination for data stream

### Control Commands
- `start` - Start DMA streaming
- `stop` - Stop DMA streaming
- `reset` - Stop streaming and reset statistics

## Example PC Control Workflow

### 1. Check Device Status
```bash
python3 antsdr_remote_client.py 192.168.1.12 info
python3 antsdr_remote_client.py 192.168.1.12 status
```

### 2. Configure Operation Mode
```bash
# Set to mode 0 (normal operation)
python3 antsdr_remote_client.py 192.168.1.12 set_mode 0

# Or set to mode 1 (alternate operation)
python3 antsdr_remote_client.py 192.168.1.12 set_mode 1
```

### 3. Configure Transfer Parameters
```bash
# Set buffer size for optimal performance
python3 antsdr_remote_client.py 192.168.1.12 set_buffer 4096

# Set where to send the data
python3 antsdr_remote_client.py 192.168.1.12 set_dest 192.168.1.100 12345
```

### 4. Start and Monitor Transfer
```bash
# Start streaming
python3 antsdr_remote_client.py 192.168.1.12 start

# Monitor in real-time
python3 antsdr_remote_client.py 192.168.1.12 monitor 60

# Or check periodically
while true; do
    python3 antsdr_remote_client.py 192.168.1.12 stats
    sleep 5
done
```

### 5. Stop and Analyze
```bash
# Stop streaming
python3 antsdr_remote_client.py 192.168.1.12 stop

# Get final statistics
python3 antsdr_remote_client.py 192.168.1.12 stats

# Reset for next test
python3 antsdr_remote_client.py 192.168.1.12 reset
```

## Response Format

All commands return structured responses:

### Success Responses
```
INFO: device=/dev/antsdr_dma mode=1 buffer_size=4096 max_buffer=65536
STATUS: streaming=active mode=1 buffer_size=4096
MODE: 1
STATS: bytes=1048576 packets=256 completions=256 errors=0
start: OK
set_mode: OK (mode=1)
```

### Error Responses
```
ERROR: Invalid mode: 2 (must be 0 or 1)
ERROR: Failed to get operation mode
ERROR: Timeout - no response from 192.168.1.12:12346
```

## Integration with Test Scripts

### Automated Testing
```python
#!/usr/bin/env python3
import subprocess
import time

def run_antsdr_test(mode, buffer_size, duration):
    controller = ANTSDRController('192.168.1.12')
    
    print(f"Testing mode {mode} with buffer {buffer_size}")
    
    # Configure
    controller.set_mode(mode)
    controller.set_buffer_size(buffer_size)
    controller.set_destination('192.168.1.100', 12345)
    
    # Run test
    controller.start_streaming()
    time.sleep(duration)
    controller.stop_streaming()
    
    # Get results
    stats = controller.get_stats()
    print(f"Results: {stats}")
    
    controller.reset()

# Test different configurations
for mode in [0, 1]:
    for buffer_size in [2048, 4096, 8192]:
        run_antsdr_test(mode, buffer_size, 30)
```

## Troubleshooting

### Connection Issues
1. **Check network connectivity**: `ping 192.168.1.12`
2. **Verify daemon is running**: Check board console for "Control server listening"
3. **Check firewall**: Ensure UDP port 12346 is open

### Command Issues
1. **Timeout errors**: Increase timeout in client scripts
2. **Invalid responses**: Check daemon logs on board
3. **Mode setting fails**: Verify GPIO drivers are loaded

### Performance Issues
1. **High latency**: Use faster network connection
2. **Packet loss**: Reduce buffer size or increase network bandwidth
3. **DMA errors**: Check buffer size vs PL transfer size alignment

## Features

✅ **Complete PC Control**: Set mode, configure transfers, start/stop remotely
✅ **Real-time Monitoring**: Get statistics and status from PC
✅ **Multiple Interfaces**: Python client, shell script, direct UDP
✅ **Daemon Mode**: Device runs autonomously waiting for PC commands
✅ **Error Handling**: Comprehensive error reporting and recovery
✅ **Test Automation**: Easy integration with automated test scripts

The ANTSDR device can now be fully controlled from your PC without any manual intervention on the board itself!
