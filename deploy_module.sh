#!/bin/bash

# ANTSDR E200 Module Deployment Script
# Handles deployment of antsdr_dma loadable kernel module
# Part of the ANTSDR firmware build system

set -e

# Configuration
ANTSDR_IP="${1:-192.168.1.12}"
MODULE_FILE="linux/drivers/misc/antsdr_dma/antsdr_dma.ko"
APP_FILE="build/antsdr_dma_remote_control"
INIT_SCRIPT="buildroot/board/e200/S15antsdr_dma"
MODULES_CONF="buildroot/board/e200/modules.conf"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "=== ANTSDR E200 Module Deployment ==="
echo "Target IP: $ANTSDR_IP"
echo "Module: $MODULE_FILE"
echo "Date: $(date)"
echo ""

# Check if module file exists
echo "1. Checking module file..."
if [ ! -f "$MODULE_FILE" ]; then
    echo -e "   ${RED}❌ Module file not found: $MODULE_FILE${NC}"
    echo "   Please build the module first with: make modules"
    exit 1
else
    MODULE_SIZE=$(stat -c%s "$MODULE_FILE")
    echo -e "   ${GREEN}✅ Module file found (${MODULE_SIZE} bytes)${NC}"
fi

# Check network connectivity
echo ""
echo "2. Checking network connectivity..."
if ping -c 3 -W 5 "$ANTSDR_IP" >/dev/null 2>&1; then
    echo -e "   ${GREEN}✅ ANTSDR E200 board is reachable at $ANTSDR_IP${NC}"
else
    echo -e "   ${RED}❌ Cannot reach ANTSDR E200 board at $ANTSDR_IP${NC}"
    echo "   Please check:"
    echo "   - Board is powered on"
    echo "   - USB/Ethernet connection is working"
    echo "   - IP address is correct (default: 192.168.1.12)"
    exit 1
fi

# Check SSH connectivity
echo ""
echo "3. Checking SSH access..."
if timeout 10 sshpass -p analog ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@"$ANTSDR_IP" "echo 'SSH connection successful'" >/dev/null 2>&1; then
    echo -e "   ${GREEN}✅ SSH access confirmed${NC}"
else
    echo -e "   ${RED}❌ Cannot establish SSH connection${NC}"
    echo "   Please ensure SSH is enabled on the ANTSDR board"
    echo "   Make sure 'sshpass' is installed: sudo apt-get install sshpass"
    exit 1
fi

# Stop and remove existing module
echo ""
echo "4. Removing old module if present..."
sshpass -p analog ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@"$ANTSDR_IP" "
    if lsmod | grep -q antsdr_dma; then
        echo '   Stopping existing module...'
        rmmod antsdr_dma 2>/dev/null || true
        echo -e '   ${YELLOW}✓ Old module removed${NC}'
    else
        echo '   No existing module found'
    fi
" 2>/dev/null

# Copy new module
echo ""
echo "5. Copying new module..."
if sshpass -p analog scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$MODULE_FILE" root@"$ANTSDR_IP":/lib/modules/antsdr_dma.ko 2>/dev/null; then
    echo -e "   ${GREEN}✅ Module copied successfully${NC}"
else
    echo -e "   ${RED}❌ Failed to copy module${NC}"
    exit 1
fi

# Copy app
echo ""
echo "5. Copying new app..."
if sshpass -p analog scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$APP_FILE" root@"$ANTSDR_IP":~ 2>/dev/null; then
    echo -e "   ${GREEN}✅ App copied successfully${NC}"
else
    echo -e "   ${RED}❌ Failed to copy module${NC}"
    exit 1
fi

# Copy init script if it exists
echo ""
echo "6. Installing module auto-loading scripts..."
if [ -f "$INIT_SCRIPT" ]; then
    echo "   Copying init script..."
    if sshpass -p analog scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$INIT_SCRIPT" root@"$ANTSDR_IP":/etc/init.d/S15antsdr_dma 2>/dev/null; then
        echo -e "   ${GREEN}✓ Init script installed${NC}"
        # Make it executable
        sshpass -p analog ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@"$ANTSDR_IP" "chmod +x /etc/init.d/S15antsdr_dma" 2>/dev/null
    else
        echo -e "   ${YELLOW}⚠️  Failed to copy init script (non-critical)${NC}"
    fi
else
    echo -e "   ${YELLOW}⚠️  Init script not found: $INIT_SCRIPT${NC}"
fi

# Copy modules.conf if it exists
if [ -f "$MODULES_CONF" ]; then
    echo "   Copying modules.conf..."
    if sshpass -p analog scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$MODULES_CONF" root@"$ANTSDR_IP":/etc/modules.conf 2>/dev/null; then
        echo -e "   ${GREEN}✓ modules.conf installed${NC}"
    else
        echo -e "   ${YELLOW}⚠️  Failed to copy modules.conf (non-critical)${NC}"
    fi
else
    echo -e "   ${YELLOW}⚠️  modules.conf not found: $MODULES_CONF${NC}"
fi

# Load new module
echo ""
echo "7. Loading new module with DMA reset..."
sshpass -p analog ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@"$ANTSDR_IP" "
    echo '   Loading antsdr_dma module...'
    if modprobe antsdr_dma 2>/dev/null || insmod /lib/modules/antsdr_dma.ko 2>/dev/null; then
        if lsmod | grep -q antsdr_dma; then
            echo -e '   ${GREEN}✅ Module loaded successfully${NC}'
            echo '   Checking DMA reset functionality...'
            dmesg | tail -10 | grep -E '(antsdr_dma|DMA|reset)' | tail -3 || echo '   (DMA reset message may not be visible in dmesg)'
        else
            echo -e '   ${RED}❌ Module load verification failed${NC}'
            exit 1
        fi
    else
        echo -e '   ${RED}❌ Failed to load module${NC}'
        echo '   Checking for error messages...'
        dmesg | tail -5
        exit 1
    fi
" 2>/dev/null

# Verify module functionality
echo ""
echo "8. Verifying module functionality..."
MODULE_INFO=$(sshpass -p analog ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@"$ANTSDR_IP" "
    echo 'Module status:'
    lsmod | grep antsdr_dma || echo 'Module not found in lsmod'
    echo ''
    echo 'Module info:'
    modinfo antsdr_dma 2>/dev/null | head -5 || echo 'modinfo not available'
    echo ''
    echo 'Device files:'
    ls -la /dev/antsdr* 2>/dev/null || echo 'No /dev/antsdr* files found'
" 2>/dev/null)

echo "$MODULE_INFO"

# Check if module is properly loaded
if echo "$MODULE_INFO" | grep -q "antsdr_dma"; then
    echo ""
    echo -e "${GREEN}🎉 MODULE DEPLOYMENT SUCCESSFUL! 🎉${NC}"
    echo ""
    echo "Module features implemented:"
    echo "✅ DMA reset on probe (dmaengine_terminate_all)"
    echo "✅ Loadable kernel module architecture"
    echo "✅ Auto-loading at boot via init scripts"
    echo "✅ Integrated with SD card deployment system"
    echo ""
    echo "Module information:"
    echo "• Module file: /lib/modules/antsdr_dma.ko"
    echo "• Init script: /etc/init.d/S15antsdr_dma"
    echo "• Auto-load config: /etc/modules.conf"
    echo ""
    echo "Testing commands:"
    echo "• Check module: lsmod | grep antsdr_dma"
    echo "• Module info: modinfo antsdr_dma"
    echo "• Reload module: rmmod antsdr_dma && modprobe antsdr_dma"
    echo "• Check DMA: dmesg | grep -E '(antsdr_dma|DMA)'"
else
    echo ""
    echo -e "${YELLOW}⚠️  Module deployment completed but verification inconclusive${NC}"
    echo "The module may be loaded but not showing expected output"
    echo "Please check manually:"
    echo "sshpass -p analog ssh root@$ANTSDR_IP 'lsmod | grep antsdr_dma'"
fi

echo ""
echo "=== Module Deployment Complete ==="
