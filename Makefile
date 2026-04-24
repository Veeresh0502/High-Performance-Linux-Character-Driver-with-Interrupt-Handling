# Makefile for Linux Kernel Modules
#
# This Makefile uses the kernel's build system (kbuild) to compile
# multiple modules with the correct flags and settings.

# List of kernel modules to build (without .ko extension)
# Each module name corresponds to a .c file
obj-m := hello.o char_driver.o

# Path to kernel headers
# This should point to your currently running kernel's build directory
# If you get errors, you may need to install kernel headers:
#   Ubuntu/Debian: sudo apt-get install linux-headers-$(uname -r)
#   Fedora/RHEL:   sudo dnf install kernel-devel
#   Arch:          sudo pacman -S linux-headers
KERNEL_DIR := /lib/modules/$(shell uname -r)/build

# Current directory where this Makefile resides
PWD := $(shell pwd)

# Default target - builds all modules
all:
	# Invoke the kernel build system
	# -C changes to kernel directory
	# M= specifies where our module source is
	# modules tells kbuild to build external modules
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

# Clean target - removes generated files
clean:
	# Ask kernel build system to clean our modules
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

# Install target - installs modules to system (requires root)
install:
	# Copy the compiled .ko files to kernel modules directory
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules_install

# Load the hello module (requires root privileges)
load-hello:
	sudo insmod hello.ko
	dmesg | tail -5

# Unload the hello module (requires root privileges)
unload-hello:
	sudo rmmod hello
	dmesg | tail -5

# Load the character device driver (requires root privileges)
load-char:
	sudo insmod char_driver.ko
	dmesg | tail -20
	@echo ""
	@echo "Device node should be at: /dev/mychardev"
	@echo "Check with: ls -l /dev/mychardev"

# Unload the character device driver (requires root privileges)
unload-char:
	sudo rmmod char_driver
	dmesg | tail -10

# Show information about compiled modules
info-hello:
	modinfo hello.ko

info-char:
	modinfo char_driver.ko

# List currently loaded modules
list:
	@echo "=== Hello Module ==="
	@lsmod | grep -E "^Module|hello" || echo "hello module not loaded"
	@echo ""
	@echo "=== Character Driver Module ==="
	@lsmod | grep -E "^Module|char_driver" || echo "char_driver module not loaded"

# Show device information (for char driver)
device-info:
	@echo "=== Device Files ==="
	@ls -l /dev/mychardev 2>/dev/null || echo "/dev/mychardev not found"
	@echo ""
	@echo "=== /proc/devices (character devices) ==="
	@grep mychardev /proc/devices 2>/dev/null || echo "mychardev not in /proc/devices"
	@echo ""
	@echo "=== Device Class ==="
	@ls -l /sys/class/mychardev 2>/dev/null || echo "Class not found"

# Help target - shows available commands
help:
	@echo "Available targets:"
	@echo ""
	@echo "  make              - Build all kernel modules"
	@echo "  make clean        - Remove generated files"
	@echo "  make install      - Install modules (requires root)"
	@echo ""
	@echo "Hello World Module:"
	@echo "  make load-hello   - Load hello module and show messages"
	@echo "  make unload-hello - Unload hello module"
	@echo "  make info-hello   - Display hello module information"
	@echo ""
	@echo "Character Device Driver:"
	@echo "  make load-char    - Load char driver and show messages"
	@echo "  make unload-char  - Unload char driver"
	@echo "  make info-char    - Display char driver information"
	@echo "  make device-info  - Show device node and registration info"
	@echo ""
	@echo "General:"
	@echo "  make list         - List all loaded modules"
	@echo ""
	@echo "Quick test sequence for char driver:"
	@echo "  1. make"
	@echo "  2. make load-char"
	@echo "  3. make device-info"
	@echo "  4. make unload-char"

# Mark these targets as not being file names
.PHONY: all clean install load-hello unload-hello load-char unload-char
.PHONY: info-hello info-char list device-info help
