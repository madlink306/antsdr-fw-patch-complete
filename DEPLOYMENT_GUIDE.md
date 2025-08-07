# ANTSDR E200 GPIO Firmware Deployment Guide

## Prerequisites
- ANTSDR E200 board connected via USB or Ethernet
- Board accessible at IP address (default: 192.168.2.1)
- SSH access to the board (username: root, no password by default)

## Method 1: Automated Deployment (Recommended)

Use the automated deployment script:

```bash
# Deploy with default IP (192.168.2.1)
./deploy_firmware.sh

# Or specify custom IP address
./deploy_firmware.sh 192.168.1.100
```

The script will:
1. ✅ Verify firmware integrity
2. ✅ Check network connectivity
3. ✅ Create firmware backup
4. ✅ Upload new firmware
5. ✅ Install and reboot
6. ✅ Run GPIO verification

## Method 2: Manual Deployment

If the automated script fails, follow these manual steps:

### Step 1: Check Board Connectivity
```bash
# Test network connection
ping 192.168.2.1

# Test SSH access
ssh root@192.168.2.1
```

### Step 2: Upload Firmware
```bash
# Copy firmware to board
scp build/e200.frm root@192.168.2.1:/opt/

# Copy verification script
scp verify_gpio_driver.sh root@192.168.2.1:/opt/
```

### Step 3: Install Firmware
```bash
# SSH to the board
ssh root@192.168.2.1

# Backup existing firmware (optional)
cp /lib/firmware/pluto.frm /opt/backup_firmware.frm

# Install new firmware
cp /opt/e200.frm /lib/firmware/pluto.frm
sync

# Reboot to apply new firmware
reboot
```

### Step 4: Verify GPIO Functionality
After the board reboots (30-60 seconds):

```bash
# SSH back to the board
ssh root@192.168.2.1

# Run GPIO verification
/opt/verify_gpio_driver.sh

# Or manually check GPIO functionality
lsmod | grep antsdr_dma
ls /sys/class/gpio/
dmesg | grep -i gpio
```

## Method 3: DFU Mode Deployment (If Board Won't Boot)

If the board fails to boot or network is not accessible:

### Step 1: Enter DFU Mode
1. Power off the ANTSDR E200
2. Hold the DFU button (small button near USB connector)
3. Power on while holding DFU button
4. Release DFU button after 3-5 seconds

### Step 2: Flash via DFU
```bash
# Check if board is in DFU mode
lsusb | grep -i "analog devices\|dfu"

# Flash firmware via DFU
sudo dfu-util -a firmware.dfu -D build/e200.dfu

# Reset the board
sudo dfu-util -a firmware.dfu -e
```

## Verification Checklist

After deployment, verify these items work:

- [ ] Board boots successfully
- [ ] SSH access works
- [ ] `antsdr_dma` module loads: `lsmod | grep antsdr_dma`
- [ ] GPIO devices available: `ls /dev/gpiochip*`
- [ ] AXI GPIO in device tree: `find /sys/firmware/devicetree/base -name "*axi*gpio*"`
- [ ] No GPIO errors in dmesg: `dmesg | grep -i "gpio\|antsdr"`

## Troubleshooting

### Board Won't Boot After Firmware Update
- Try DFU mode recovery (Method 3)
- Restore backup firmware: `cp /opt/backup_firmware.frm /lib/firmware/pluto.frm`

### GPIO Driver Not Loading
- Check kernel config: `zcat /proc/config.gz | grep GPIO_XILINX`
- Check device tree: `/opt/verify_gpio_driver.sh`
- Review dmesg: `dmesg | grep -i "gpio\|antsdr"`

### Network Connectivity Issues
- Check USB cable connection
- Verify IP configuration: `ip addr show`
- Try different IP address: `./deploy_firmware.sh 192.168.1.100`

## Success Indicators

You should see these confirmations after successful deployment:

```
✅ antsdr_dma module is loaded
✅ GPIO sysfs interface is available  
✅ CONFIG_GPIO_XILINX is enabled in kernel
✅ ANTSDR device tree node has GPIO properties
✅ AXI GPIO controller found in device tree
✅ GPIO character devices available
```

## Next Steps

After successful deployment:
1. Test GPIO functionality with your application
2. Monitor GPIO state during streaming operations
3. Check GPIO control with oscilloscope or logic analyzer
4. Verify streaming performance with GPIO enabled

The GPIO will be HIGH during active streaming and LOW when idle.
