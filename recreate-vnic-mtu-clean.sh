#!/bin/bash
set -e

echo "=== Recreating vnic-mtu-fix on clean net-next/main ==="
echo ""

# Save current patches
echo "1. Saving current patches..."
mkdir -p /tmp/vnic-mtu-patches
git format-patch -2 vnic-mtu-fix -o /tmp/vnic-mtu-patches/
echo "   Saved patches:"
ls -lh /tmp/vnic-mtu-patches/

# Fetch and sync with latest upstream net-next
echo ""
echo "2. Fetching latest upstream net-next..."
git fetch net-next

echo ""
echo "3. Syncing net-next/main with upstream..."
git checkout net-next/main
git pull --ff-only net-next main
echo "   Latest net-next commit:"
git log --oneline -1 net-next/main

# Backup old branch
echo ""
echo "4. Backing up old vnic-mtu-fix branch..."
git branch -m vnic-mtu-fix vnic-mtu-fix-old-veth-base

# Create new branch from clean net-next/main
echo ""
echo "5. Creating new vnic-mtu-fix from latest net-next/main..."
git checkout -b vnic-mtu-fix net-next/main

# Apply patches
echo ""
echo "6. Applying ibmvnic MTU patches..."
git am /tmp/vnic-mtu-patches/*.patch

# Verify
echo ""
echo "=== Verification ==="
echo "New branch history:"
git log --oneline --graph --decorate -5

echo ""
echo "Base commit:"
git log --oneline -1 net-next/main

echo ""
echo "=== Success! ==="
echo ""
echo "✅ New branch: vnic-mtu-fix (based on clean net-next/main)"
echo "📦 Old branch: vnic-mtu-fix-old-veth-base (preserved for reference)"
echo ""
echo "Next steps:"
echo "1. Push new branch: git push -f origin vnic-mtu-fix"
echo "2. On lab server: git fetch origin && git checkout vnic-mtu-fix"
echo "3. Build kernel on lab"

# Made with Bob
