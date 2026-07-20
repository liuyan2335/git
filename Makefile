# ============================================================
#  RK3568 AI Voice System - Makefile
#  Cross-compilation for RK3568 (ARM Cortex-A55, aarch64)
# ============================================================

# Toolchain
CROSS_COMPILE ?= aarch64-linux-gnu-
CC      = $(CROSS_COMPILE)gcc
STRIP   = $(CROSS_COMPILE)strip
OBJCOPY = $(CROSS_COMPILE)objcopy

# Compiler flags
CFLAGS   = -Wall -Wextra -O2 -std=c11
LDFLAGS  = -static -lpthread
INCLUDES = -I./include

# Source files
SRC_DIR  = src
BIN_DIR  = bin
IMG_DIR  = img

SRCS     = $(wildcard $(SRC_DIR)/*.c)
TARGETS  = $(patsubst $(SRC_DIR)/%.c, $(BIN_DIR)/%, $(SRCS))

# Default target
.PHONY: all clean deploy help

all: dirs $(TARGETS)
	@echo "========================================"
	@echo "  Build complete!"
	@echo "  Targets: $(notdir $(TARGETS))"
	@echo "========================================"

# Create output directories
dirs:
	@mkdir -p $(BIN_DIR) $(IMG_DIR)

# Compile each .c file to a standalone executable
# Each source can be built standalone with -DSTANDALONE flag
$(BIN_DIR)/fb_lcd: $(SRC_DIR)/fb_lcd.c include/fb_lcd.h
	@echo "[CC] $@"
	$(CC) $(CFLAGS) $(INCLUDES) -DFB_LCD_STANDALONE $< -o $@ $(LDFLAGS)
	$(STRIP) $@ 2>/dev/null || true

$(BIN_DIR)/touch_input: $(SRC_DIR)/touch_input.c include/touch_input.h
	@echo "[CC] $@"
	$(CC) $(CFLAGS) $(INCLUDES) -DTOUCH_INPUT_STANDALONE $< -o $@ $(LDFLAGS)
	$(STRIP) $@ 2>/dev/null || true

$(BIN_DIR)/tcp_client: $(SRC_DIR)/tcp_client.c include/tcp_client.h
	@echo "[CC] $@"
	$(CC) $(CFLAGS) $(INCLUDES) -DTCP_CLIENT_STANDALONE $< -o $@ $(LDFLAGS)
	$(STRIP) $@ 2>/dev/null || true

$(BIN_DIR)/audio_record: $(SRC_DIR)/audio_record.c include/audio_record.h
	@echo "[CC] $@"
	$(CC) $(CFLAGS) $(INCLUDES) -DAUDIO_RECORD_STANDALONE $< -o $@ $(LDFLAGS)
	$(STRIP) $@ 2>/dev/null || true

# Build with native gcc for local testing
native: CROSS_COMPILE =
native: CFLAGS += -g
native: LDFLAGS = -lpthread
native: all

# Install to a mounted RK3568 rootfs or via adb/scp
deploy: all
	@echo "Deploying binaries to RK3568..."
	@if [ -z "$(DEPLOY_PATH)" ]; then \
		echo "Usage: make deploy DEPLOY_PATH=/mnt/rk3568/bin"; \
		echo "   or: make deploy DEPLOY_PATH=root@192.168.1.10:/home/root/"; \
		exit 1; \
	fi
	scp $(BIN_DIR)/* $(DEPLOY_PATH)/ 2>/dev/null || \
	cp $(BIN_DIR)/* $(DEPLOY_PATH)/ 2>/dev/null || \
	(echo "Deploy failed. Set DEPLOY_PATH to target directory." && exit 1)
	@echo "Deployed to $(DEPLOY_PATH)"

# Clean build artifacts
clean:
	@echo "Cleaning..."
	@rm -rf $(BIN_DIR)/*
	@echo "Clean complete."

# Remove all generated files
distclean: clean
	@rm -rf audio_data/ __pycache__/ server/__pycache__/ *.log

# Help
help:
	@echo "RK3568 AI Voice System - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Cross-compile all C programs (default)"
	@echo "              Uses $(CROSS_COMPILE)gcc"
	@echo "  native    - Compile with native gcc for testing"
	@echo "  deploy    - Copy binaries to RK3568 (set DEPLOY_PATH)"
	@echo "  clean     - Remove build artifacts"
	@echo "  distclean - Remove all generated files"
	@echo ""
	@echo "Variables:"
	@echo "  CROSS_COMPILE - Toolchain prefix (default: aarch64-linux-gnu-)"
	@echo "  DEPLOY_PATH   - Target path for 'make deploy'"
	@echo ""
	@echo "Examples:"
	@echo "  make                          # Cross-compile all"
	@echo "  make native                   # Build for local testing"
	@echo "  make deploy DEPLOY_PATH=root@192.168.1.10:/home/root/"
	@echo "  make CROSS_COMPILE=arm-linux-gnueabihf- all"
