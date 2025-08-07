# GPIO Mode Integration Complete

## Overview
Successfully integrated the new .xsa file with `ctrl_enable` and `ctrl_mode` GPIO controllers and updated the device tree and test module accordingly.

## Hardware Changes (from .xsa analysis)
1. **ctrl_enable** - AXI GPIO at address `0x41210000` (replaces old `axi_gpio_0`)
   - Connected to `top_0.block_enable`
   - Single bit output GPIO for enable control

2. **ctrl_mode** - AXI GPIO at address `0x41200000` (new addition)
   - Connected to `top_0.mode`
   - Single bit output GPIO for operation mode control

## Device Tree Updates
Updated `/linux/arch/arm/boot/dts/zynq-e200.dtsi`:

1. **Replaced old `axi_gpio_0` with two separate GPIO controllers:**
   ```dts
   /* Xilinx AXI GPIO for mode control */
   ctrl_mode: axi-gpio@41200000 {
       reg = <0x41200000 0x1000>;
       xlnx,gpio-width = <1>;
       ...
   };

   /* Xilinx AXI GPIO for enable control */
   ctrl_enable: axi-gpio@41210000 {
       reg = <0x41210000 0x1000>;
       xlnx,gpio-width = <1>;
       ...
   };
   ```

2. **Updated ANTSDR DMA device reference:**
   ```dts
   antsdr_dma: antsdr-dma@0 {
       enable-gpios = <&ctrl_enable 0 0>;
       mode-gpios = <&ctrl_mode 0 0>;
   };
   ```

## Driver Updates
Updated `/linux/drivers/misc/antsdr_dma/antsdr_dma.c`:

1. **Added mode GPIO to device structure:**
   ```c
   struct antsdr_dma_dev {
       struct gpio_desc *gpio_enable;
       struct gpio_desc *gpio_mode;    // NEW
       ...
   };
   ```

2. **Added new IOCTL commands:**
   ```c
   #define ANTSDR_IOC_SET_MODE  _IOW(ANTSDR_IOC_MAGIC, 7, uint32_t)
   #define ANTSDR_IOC_GET_MODE  _IOR(ANTSDR_IOC_MAGIC, 8, uint32_t)
   ```

3. **Added GPIO initialization in probe function:**
   ```c
   dma_dev->gpio_mode = devm_gpiod_get_optional(&pdev->dev, "mode", GPIOD_OUT_LOW);
   ```

4. **Added IOCTL handlers for mode control:**
   - `ANTSDR_IOC_SET_MODE`: Sets operation mode (0 or 1)
   - `ANTSDR_IOC_GET_MODE`: Gets current operation mode

## Test Module Updates
Updated `/antsdr_dma_test_enhanced.c`:

1. **Added mode parameter support:**
   - Command line option `-m MODE` for setting operation mode
   - Mode validation (must be 0 or 1)

2. **Added mode control functions:**
   ```c
   static int set_mode(uint32_t mode)
   static int get_mode(uint32_t *mode)
   ```

3. **Added remote control commands:**
   - `set_mode <mode>` - Set operation mode remotely
   - `get_mode` - Get current operation mode remotely

4. **Updated help and usage information** to include mode control options

## Build Status
- ✅ **Driver compiled successfully** with TARGET=e200
- ✅ **Test module compiled successfully** with pthread support
- ✅ **New .xsa file copied** to build directory for consistency

## Usage Examples

### Command Line
```bash
# Set mode 0 and run test
./antsdr_dma_test_enhanced -m 0 -i 192.168.1.100 -p 12345

# Set mode 1 with different buffer size
./antsdr_dma_test_enhanced -m 1 -b 4096 -d 30
```

### Remote Control
```bash
# Set operation mode to 1
echo "set_mode 1" | nc -u 192.168.1.12 12346

# Get current operation mode
echo "get_mode" | nc -u 192.168.1.12 12346
```

## Hardware Configuration Summary
- **ctrl_enable (0x41210000)**: Controls block enable signal to PL
- **ctrl_mode (0x41200000)**: Controls operation mode signal to PL
- Both are 1-bit GPIO controllers
- Default state: both GPIOs start at 0 (LOW)

## Next Steps
1. Deploy updated firmware to test hardware functionality
2. Verify GPIO control works with PL design
3. Test different operation modes
4. Validate mode switching during runtime

## Files Modified
1. `/linux/arch/arm/boot/dts/zynq-e200.dtsi` - Device tree GPIO configuration
2. `/linux/drivers/misc/antsdr_dma/antsdr_dma.c` - Driver with mode GPIO support  
3. `/antsdr_dma_test_enhanced.c` - Test module with mode control
4. `/build/system_top.xsa` - Updated hardware description file
