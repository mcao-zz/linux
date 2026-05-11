#!/bin/bash
# rebootveth.sh - Rebuild and install updated ibmveth module, then reboot
# Usage: ./rebootveth.sh

set -e  # Exit on error

# Auto-detect kernel version
KERNEL_VERSION=$(uname -r)
MODULE_PATH="drivers/net/ethernet/ibm"
INSTALL_PATH="/lib/modules/${KERNEL_VERSION}/kernel/${MODULE_PATH}"
INITRAMFS="/boot/initramfs-${KERNEL_VERSION}.img"

echo "Detected kernel version: ${KERNEL_VERSION}"

echo "=== Rebuilding ibmveth Module ==="
make M=${MODULE_PATH}

echo ""
echo "=== Installing New Module ==="
sudo cp ${MODULE_PATH}/ibmveth.ko ${INSTALL_PATH}/

echo ""
echo "=== Verifying Module Size ==="
ls -lh ${INSTALL_PATH}/ibmveth.ko

echo ""
echo "=== Backing Up Old Initramfs ==="
sudo cp ${INITRAMFS} ${INITRAMFS}.backup.$(date +%Y%m%d_%H%M%S)

echo ""
echo "=== Rebuilding Initramfs ==="
sudo dracut --force ${INITRAMFS} ${KERNEL_VERSION}

echo ""
echo "=== Verifying Module in Initramfs ==="
lsinitrd ${INITRAMFS} | grep "ibmveth.ko$"

echo ""
echo "=== Module Update Complete ==="
echo "New module installed and initramfs rebuilt."
echo ""
read -p "Reboot now? (y/n) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Rebooting in 5 seconds..."
    sleep 5
    sudo reboot
else
    echo "Reboot cancelled. Run 'sudo reboot' when ready."
fi

# Made with Bob
