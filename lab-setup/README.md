# Lab Setup Documentation

Complete documentation for IBM Power Systems lab environments, including system configurations, installation guides, and troubleshooting resources.

---

## 📋 Lab Systems

### 🖥️ bk-denali01
**Location:** `lab-setup/bk-denali/`
**System Type:** POWER9/POWER10
**Purpose:** SLES testing and performance comparison

**Key Documentation:**
- [bk-denali README](bk-denali/README.md) - Complete system overview
- [FINAL-SLES-SETUP-PLAN.md](bk-denali/FINAL-SLES-SETUP-PLAN.md) - Main installation guide
- [SLES-NETWORK-SETUP.md](bk-denali/SLES-NETWORK-SETUP.md) - Network configuration
- [Troubleshooting Guides](bk-denali/README.md#troubleshooting--issues) - Jenkins NIM issues

**Quick Start:**
```bash
cd lab-setup/bk-denali
cat README.md
cat FINAL-SLES-SETUP-PLAN.md
```

---

### 🖥️ ltcd51
**Location:** `lab-setup/ltcd51/`
**System Type:** POWER9
**Purpose:** veth/vNIC performance testing

**Key Documentation:**
- [ltcd51 README](ltcd51/README.md) - System overview
- [CURRENT-RESOURCE-INVENTORY.md](ltcd51/CURRENT-RESOURCE-INVENTORY.md) - Complete resource inventory
- [SYSTEM-ACCESS-INFO.md](ltcd51/SYSTEM-ACCESS-INFO.md) - Access credentials and network info
- [LTCD51-VNIC-REORG-PLAN.md](ltcd51/LTCD51-VNIC-REORG-PLAN.md) - LPAR reorganization plan

**Quick Start:**
```bash
cd lab-setup/ltcd51
cat README.md
cat CURRENT-RESOURCE-INVENTORY.md
```

---

## 📚 Documentation Structure

```
lab-setup/
├── README.md (this file)
│
├── bk-denali/
│   ├── README.md                      # System overview and index
│   ├── FINAL-SLES-SETUP-PLAN.md      # Main installation guide ⭐
│   ├── LP2-INSTALLATION-PLAN.md      # Alternative lp2 plan
│   ├── SLES-NETWORK-SETUP.md         # Network configuration
│   ├── BOOT-MESSAGE-SETUP.md         # Boot message config
│   ├── MAC-ADDRESS-CASE-ISSUE.md     # Jenkins NIM troubleshooting
│   └── SLES-GRUB-MYSTERY.md          # GRUB issues analysis
│
└── ltcd51/
    ├── README.md                      # System overview
    ├── CURRENT-RESOURCE-INVENTORY.md  # Resource inventory
    ├── SYSTEM-ACCESS-INFO.md          # Access info
    └── LTCD51-VNIC-REORG-PLAN.md     # Reorganization plan
```

---

## 🚀 Quick Navigation

### By Task

**Installing SLES:**
1. [bk-denali/FINAL-SLES-SETUP-PLAN.md](bk-denali/FINAL-SLES-SETUP-PLAN.md) - Complete guide
2. [bk-denali/SLES-NETWORK-SETUP.md](bk-denali/SLES-NETWORK-SETUP.md) - Network setup

**Troubleshooting Jenkins NIM:**
1. [bk-denali/MAC-ADDRESS-CASE-ISSUE.md](bk-denali/MAC-ADDRESS-CASE-ISSUE.md) - MAC case sensitivity
2. [bk-denali/SLES-GRUB-MYSTERY.md](bk-denali/SLES-GRUB-MYSTERY.md) - GRUB issues

**System Access:**
1. [ltcd51/SYSTEM-ACCESS-INFO.md](ltcd51/SYSTEM-ACCESS-INFO.md) - Credentials and IPs
2. [bk-denali/README.md](bk-denali/README.md#system-overview) - bk-denali access

**Resource Planning:**
1. [ltcd51/CURRENT-RESOURCE-INVENTORY.md](ltcd51/CURRENT-RESOURCE-INVENTORY.md) - ltcd51 resources
2. [ltcd51/LTCD51-VNIC-REORG-PLAN.md](ltcd51/LTCD51-VNIC-REORG-PLAN.md) - Reorganization

---

## 🎯 Common Tasks

### Task 1: Install SLES on bk-denali01-lp1

```bash
# 1. Read the plan
cat lab-setup/bk-denali/FINAL-SLES-SETUP-PLAN.md

# 2. SSH to lp1
ssh root@bk-denali01-lp1

# 3. Check disk layout
lsblk
parted /dev/nvme0n1 print

# 4. Follow installation steps in FINAL-SLES-SETUP-PLAN.md
```

### Task 2: Access ltcd51 System

```bash
# 1. Get access info
cat lab-setup/ltcd51/SYSTEM-ACCESS-INFO.md

# 2. Access HMC
# URL: https://ltcvhmc8b.ltc.tadn.ibm.com/hmc/connect
# Password: Pow3rGetsReady4Bob!

# 3. SSH to LPARs (see SYSTEM-ACCESS-INFO.md for IPs)
```

### Task 3: Troubleshoot Jenkins NIM

```bash
# 1. Check MAC address case
cat lab-setup/bk-denali/MAC-ADDRESS-CASE-ISSUE.md

# 2. Check GRUB issues
cat lab-setup/bk-denali/SLES-GRUB-MYSTERY.md

# 3. Use lowercase MAC address in Jenkins
```

---

## 📖 Documentation Standards

### File Naming Conventions

- **README.md** - System overview and index
- **UPPERCASE-WITH-DASHES.md** - Major documentation files
- **lowercase-with-dashes.sh** - Scripts and utilities

### Document Categories

- **Planning:** Installation strategies, resource allocation
- **Configuration:** Network, boot, system settings
- **Troubleshooting:** Known issues, solutions, workarounds
- **Reference:** Access info, inventories, quick references

---

## 🔗 Related Documentation

### Testing Documentation
- `../work/veth/mss/` - veth MSS testing
- `../work/vnic/mtu/` - vNIC MTU testing
- `../work/vethmq/` - veth multiqueue testing

### Upstream Documentation
- `../work/upstream-guide/` - Kernel patch submission guides
- `../work/IBM-GITHUB-SETUP-GUIDE.md` - IBM GitHub setup

---

## 🆘 Getting Help

### For bk-denali Issues
See [bk-denali/README.md](bk-denali/README.md#getting-help)

### For ltcd51 Issues
See [ltcd51/README.md](ltcd51/README.md)

### General Lab Questions
- **Contact:** Ming (ming@ibm.com)
- **Lab:** IBM Power Systems Lab

---

## 📝 Contributing

When adding new documentation:

1. **Choose the right system directory** (bk-denali or ltcd51)
2. **Use descriptive filenames** (UPPERCASE-WITH-DASHES.md)
3. **Update the system README.md** to reference new docs
4. **Update this master README.md** if adding major documentation
5. **Follow existing formatting** for consistency

---

## 📊 Documentation Status

### bk-denali
- ✅ Complete installation guides
- ✅ Network configuration documented
- ✅ Troubleshooting guides created
- ✅ Boot configuration documented
- ⏳ Pending: Actual installations

### ltcd51
- ✅ Resource inventory complete
- ✅ Access information documented
- ✅ Reorganization plan created
- ⏳ Pending: Execute reorganization

---

**Last Updated:** 2026-05-04
**Maintained by:** Ming
**Purpose:** Lab system documentation and guides