#!/bin/bash
# ============================================================
#  deploy.sh - RK3568 Deployment Script
#  Copies compiled binaries to the RK3568 development board.
#
#  Usage:
#    ./scripts/deploy.sh [target_ip]
#    ./scripts/deploy.sh 192.168.1.10
#
#  Defaults:
#    RK3568_IP from environment variable, or 192.168.1.100
#    Target user: root
#    Target path: /home/root/ai_voice/
# ============================================================

set -e

# ─── Configuration ───
RK3568_IP="${1:-${RK3568_IP:-192.168.1.100}}"
RK3568_USER="${RK3568_USER:-root}"
TARGET_PATH="${TARGET_PATH:-/home/root/ai_voice}"
BIN_DIR="./bin"
IMG_DIR="./img"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "========================================"
echo "  RK3568 AI Voice System - Deploy"
echo "========================================"
echo ""

# ─── Check prerequisites ───
if [ ! -d "$BIN_DIR" ] || [ -z "$(ls -A $BIN_DIR 2>/dev/null)" ]; then
    echo -e "${YELLOW}[WARN]${NC} bin/ is empty. Run 'make' first to compile."
    echo "  Hint: make CROSS_COMPILE=aarch64-linux-gnu-"
fi

# ─── Ping check ───
echo -n "Checking connectivity to $RK3568_IP... "
if ping -c 1 -W 2 "$RK3568_IP" > /dev/null 2>&1; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${RED}FAILED${NC}"
    echo "Cannot reach RK3568 at $RK3568_IP."
    echo "Check the network connection and IP address."
    exit 1
fi

# ─── Create target directory ───
echo -n "Creating target directory on RK3568... "
ssh "${RK3568_USER}@${RK3568_IP}" "mkdir -p ${TARGET_PATH}" 2>/dev/null
echo -e "${GREEN}OK${NC}"

# ─── Copy binaries ───
echo "Copying files..."
if [ -d "$BIN_DIR" ] && [ -n "$(ls -A $BIN_DIR 2>/dev/null)" ]; then
    echo "  -> Binaries..."
    scp -q "${BIN_DIR}"/* "${RK3568_USER}@${RK3568_IP}:${TARGET_PATH}/"
    echo -e "  ${GREEN}Binaries copied${NC}"
fi

# ─── Copy images ───
if [ -d "$IMG_DIR" ] && [ -n "$(ls -A $IMG_DIR 2>/dev/null)" ]; then
    echo "  -> Images..."
    scp -q "${IMG_DIR}"/*.bmp "${RK3568_USER}@${RK3568_IP}:${TARGET_PATH}/" 2>/dev/null || true
    echo -e "  ${GREEN}Images copied${NC}"
fi

# ─── Set permissions ───
echo -n "Setting executable permissions... "
ssh "${RK3568_USER}@${RK3568_IP}" "chmod +x ${TARGET_PATH}/*" 2>/dev/null
echo -e "${GREEN}OK${NC}"

echo ""
echo -e "${GREEN}Deploy complete!${NC}"
echo ""
echo "Run on RK3568:"
echo "  ssh ${RK3568_USER}@${RK3568_IP}"
echo "  cd ${TARGET_PATH}"
echo "  ./tcp_client <server_ip>"
echo "  ./fb_lcd"
echo ""
echo "On server (Ubuntu):"
echo "  cd server/"
echo "  python3 tcp_server.py"
echo ""
