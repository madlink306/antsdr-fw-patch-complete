# ANTSDR E200 GPIO Firmware Update Guide

## Understanding the Virtual Filesystem

The ANTSDR E200 uses a **read-only squashfs root filesystem** that completely resets on every reboot. This is normal behavior for embedded PlutoSDR-based systems. Files copied via SSH will always disappear after reboot.

## Firmware Update Methods

### Method 1: Web Interface Upload (Recommended)

1. **Open web browser** and navigate to: `https://192.168.1.12`
2. **Accept security certificate** warning
3. **Look for firmware upload section** (usually under "System" or "Firmware")
4. **Upload the file**: `build/e200.frm`
5. **Wait for upload and automatic reboot**

### Method 2: USB Mass Storage Mode

1. **Trigger mass storage mode** via SSH:
   ```bash
   ssh root@192.168.1.12
   # Try one of these commands:
   device_reboot sf
   # OR
   fw_setenv dfu_alt_info_sf
   reboot
   ```

2. **Wait for board to reboot** and appear as USB mass storage device
3. **Copy firmware** to the mass storage device:
   ```bash
   cp build/e200.frm /media/PLUTO/firmware.frm
   sync
   ```
4. **Safely eject** and the board will automatically update

### Method 3: DFU Mode (Hardware Button)

1. **Power off** the ANTSDR E200 board
2. **Hold the DFU button** (small button near USB connector)
3. **Power on** while holding DFU button for 3-5 seconds
4. **Check DFU mode**:
   ```bash
   sudo dfu-util -l
   ```
5. **Flash firmware**:
   ```bash
   sudo dfu-util -a firmware.dfu -D build/e200.dfu
   sudo dfu-util -a firmware.dfu -e  # Reset after flash
   ```

### Method 4: Network-based Firmware Update

Some ANTSDR boards support network-based updates:

```bash
# Copy firmware to board's /tmp (will survive until reboot)
scp build/e200.frm root@192.168.1.12:/tmp/

# SSH to board and trigger firmware update
ssh root@192.168.1.12 "
# Look for firmware update utility
which update_firmware || which fw_update || which pluto_update

# If available, use it:
# update_firmware /tmp/e200.frm
# OR manually trigger update by writing to specific device
# dd if=/tmp/e200.frm of=/dev/mtdblock1 bs=1M
"
```

## Automated Update Script

Create an automated update script:

```bash
#!/bin/bash
# auto_update_antsdr.sh

BOARD_IP="192.168.1.12"
FIRMWARE="build/e200.frm"

echo "=== ANTSDR E200 Auto Firmware Update ==="

# Method 1: Try web interface upload via curl
echo "Attempting web interface upload..."
if curl -k -F "firmware=@$FIRMWARE" https://$BOARD_IP/upload 2>/dev/null; then
    echo "✅ Web upload successful"
    exit 0
fi

# Method 2: Try mass storage trigger
echo "Attempting mass storage mode..."
ssh root@$BOARD_IP "device_reboot sf" 2>/dev/null
sleep 30

# Check for mass storage device
if ls /media/PLUTO* 2>/dev/null; then
    cp $FIRMWARE /media/PLUTO*/firmware.frm
    sync
    echo "✅ Mass storage update successful"
    exit 0
fi

# Method 3: Try DFU mode (requires manual button press)
echo "❌ Automatic methods failed"
echo "Please put board in DFU mode manually and run:"
echo "sudo dfu-util -a firmware.dfu -D build/e200.dfu"
```

## Verification After Update

After successful firmware update, the board will reboot with the new GPIO-enabled firmware. Verify using:

```bash
# Check kernel version and GPIO support
ssh root@192.168.1.12 "
uname -a
lsmod | grep antsdr_dma
ls /sys/class/gpio/
dmesg | grep -i gpio | tail -5
"
```

## Troubleshooting

### Board Won't Boot After Update
- **DFU Recovery**: Put board in DFU mode and flash a known good firmware
- **Web Recovery**: Many boards have a recovery web interface on a different port

### Update Fails
- **Check file size**: Ensure firmware file is complete (should be ~15MB)
- **Verify MD5**: Check build/e200.frm.md5 matches the file
- **Try different method**: If web fails, try mass storage; if that fails, try DFU

### SSH Connection Issues
- **Remember**: SSH connections will be lost during firmware update
- **Wait**: Allow 30-60 seconds for reboot after update
- **IP Change**: Some updates might change the IP address

## Why Virtual Filesystem?

The read-only filesystem provides:
- **Reliability**: System always boots to known good state
- **Security**: Prevents accidental corruption
- **Consistency**: Ensures reproducible behavior
- **Recovery**: Easy to recover from bad configurations

The GPIO driver is built into the firmware image itself, so once the firmware is updated, the GPIO functionality will be permanently available.
