#!/bin/bash
set -euo pipefail

REPO_URL="https://github.com/ActuallyFrogDev/leaf.git"
TMP_DIR=$(mktemp -d)

cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

echo "=== Leaf Package Manager Installer ==="
echo ""

# --- Detect distro ---
DISTRO=$(cat /etc/*-release 2>/dev/null | tr '[:upper:]' '[:lower:]' | grep -Poi '(arch|debian|ubuntu|fedora|opensuse|nix)' | head -1)
if [ -z "$DISTRO" ]; then
    DISTRO='unknown'
fi

echo "Detected distribution: $DISTRO"

# --- Install build dependencies ---
install_deps() {
    echo "Installing build dependencies..."
    case "$DISTRO" in
        arch)
            sudo pacman -S --needed --noconfirm base-devel curl git
            ;;
        debian|ubuntu)
            sudo apt-get update -qq
            sudo apt-get install -y build-essential libcurl4-openssl-dev git
            ;;
        fedora)
            sudo dnf install -y gcc make libcurl-devel git
            ;;
        opensuse)
            sudo zypper install -y gcc make libcurl-devel git
            ;;
        nix)
            echo "On NixOS, ensure gcc, gnumake, curl, and git are in your environment."
            ;;
        unknown)
            echo "Warning: Unknown distribution. Make sure you have gcc, make, libcurl-dev, and git installed."
            ;;
    esac
}

install_deps

# --- Clone and build ---
echo ""
echo "Cloning leaf..."
git clone --depth 1 --filter=blob:none --sparse "$REPO_URL" "$TMP_DIR/leaf"
cd "$TMP_DIR/leaf"
git sparse-checkout set --no-cone '/*' '!web'

echo "Building leaf..."
make leaf

# --- Install ---
echo "Installing leaf to /usr/bin..."
sudo make install

echo ""
echo "leaf $(leaf --version) installed successfully!"
echo "Run 'leaf --help' to get started."

