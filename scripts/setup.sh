#!/bin/bash
# ============================================================
#  setup.sh - Development Environment Setup
#  Installs required tools and dependencies.
#
#  Usage:
#    ./scripts/setup.sh
#    ./scripts/setup.sh --cross-compile
#    ./scripts/setup.sh --server
#    ./scripts/setup.sh --all
# ============================================================

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

echo "========================================"
echo "  RK3568 AI Voice System - Setup"
echo "========================================"
echo ""

MODE="all"

while [ $# -gt 0 ]; do
    case "$1" in
        --cross-compile) MODE="cross" ;;
        --server)        MODE="server" ;;
        --all)           MODE="all" ;;
        --help|-h)
            echo "Usage: $0 [--cross-compile|--server|--all]"
            echo ""
            echo "  --cross-compile  Install ARM cross-compilation toolchain"
            echo "  --server         Install Python server dependencies"
            echo "  --all            Install everything (default)"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

# ─── Cross-compilation toolchain ───
setup_cross_compile() {
    echo -e "${CYAN}[1/2] Installing ARM cross-compilation toolchain...${NC}"

    # Check if already installed
    if command -v aarch64-linux-gnu-gcc &>/dev/null; then
        echo -e "  ${GREEN}✓${NC} aarch64-linux-gnu-gcc found: $(aarch64-linux-gnu-gcc --version | head -1)"
    else
        echo "  Installing gcc-aarch64-linux-gnu..."
        if command -v apt &>/dev/null; then
            sudo apt update
            sudo apt install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
        elif command -v pacman &>/dev/null; then
            sudo pacman -S --noconfirm aarch64-linux-gnu-gcc
        else
            echo -e "  ${RED}✗${NC} Unsupported package manager. Install manually:"
            echo "    Ubuntu/Debian: sudo apt install gcc-aarch64-linux-gnu"
            echo "    Arch:          sudo pacman -S aarch64-linux-gnu-gcc"
            echo "    Fedora:        sudo dnf install gcc-aarch64-linux-gnu"
        fi
    fi

    # Verify
    if command -v aarch64-linux-gnu-gcc &>/dev/null; then
        echo -e "  ${GREEN}✓${NC} Cross-compilation toolchain ready."
    else
        echo -e "  ${RED}✗${NC} Installation failed. Continuing anyway."
    fi
    echo ""
}

# ─── Server (Python) dependencies ───
setup_server() {
    echo -e "${CYAN}[2/2] Installing Python server dependencies...${NC}"

    # Check Python version
    if command -v python3 &>/dev/null; then
        PY_VER=$(python3 --version 2>&1 | awk '{print $2}')
        echo "  Python version: ${PY_VER}"
    else
        echo -e "  ${RED}✗${NC} Python 3 not found. Please install Python 3.8+."
        return 1
    fi

    # Install pip packages
    echo "  Installing Python packages..."
    pip3 install --user -q requests 2>/dev/null || pip3 install requests
    pip3 install --user -q websocket-client 2>/dev/null || pip3 install websocket-client

    # Check installations
    echo ""
    for pkg in requests websocket; do
        if python3 -c "import ${pkg}" 2>/dev/null; then
            echo -e "  ${GREEN}✓${NC} ${pkg}"
        else
            echo -e "  ${YELLOW}⚠${NC} ${pkg} (optional, needed for full functionality)"
        fi
    done
    echo ""

    echo -e "${GREEN}  Server dependencies installed.${NC}"
    echo ""
}

# ─── Main ───
case "$MODE" in
    cross) setup_cross_compile ;;
    server) setup_server ;;
    all)
        setup_cross_compile
        setup_server
        ;;
esac

echo "========================================"
echo "  Setup complete!"
echo "========================================"
echo ""
echo "Next steps:"
echo "  1. Set API keys:"
echo "     export DEEPSEEK_API_KEY='your_key'"
echo "     export XF_APP_ID='your_app_id'"
echo "     export XF_API_KEY='your_api_key'"
echo "     export XF_API_SECRET='your_api_secret'"
echo ""
echo "  2. Build embedded binaries:"
echo "     make"
echo ""
echo "  3. Deploy to RK3568:"
echo "     make deploy DEPLOY_PATH=root@<rk3568_ip>:/home/root/ai_voice/"
echo ""
echo "  4. Start server:"
echo "     cd server/ && python3 tcp_server.py"
echo ""
