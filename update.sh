#!/bin/bash
set -euo pipefail

REMOTE_VERSION_URL="https://raw.githubusercontent.com/ActuallyFrogDev/leaf/master/version.txt"
REPO_URL="https://github.com/ActuallyFrogDev/leaf.git"
TMP_DIR=$(mktemp -d)

cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

# --- Get local version from the installed CLI ---
if ! command -v leaf &>/dev/null; then
    echo "Error: 'leaf' is not installed. Run install.sh first."
    exit 1
fi

LOCAL_VERSION=$(leaf --version | grep -oP '[0-9]+\.[0-9]+\.[0-9]+')
if [ -z "$LOCAL_VERSION" ]; then
    echo "Error: Could not determine local leaf version."
    exit 1
fi

# --- Fetch remote version ---
REMOTE_VERSION=$(curl -fsSL "$REMOTE_VERSION_URL" | tr -d '[:space:]')
if [ -z "$REMOTE_VERSION" ]; then
    echo "Error: Could not fetch remote version."
    exit 1
fi

echo "Installed version: $LOCAL_VERSION"
echo "Latest version:    $REMOTE_VERSION"

# --- Semantic version comparison ---
# Returns 0 if $1 < $2 (i.e. update needed)
version_lt() {
    local IFS='.'
    local i a=($1) b=($2)
    for ((i = 0; i < 3; i++)); do
        local va=${a[i]:-0}
        local vb=${b[i]:-0}
        if ((va < vb)); then return 0; fi
        if ((va > vb)); then return 1; fi
    done
    return 1  # equal → no update
}

if ! version_lt "$LOCAL_VERSION" "$REMOTE_VERSION"; then
    echo "You are already on the latest version."
    exit 0
fi

echo "Updating leaf $LOCAL_VERSION -> $REMOTE_VERSION ..."

# --- Clone, build, install ---
git clone --depth 1 --filter=blob:none --sparse "$REPO_URL" "$TMP_DIR/leaf"
cd "$TMP_DIR/leaf"
git sparse-checkout set --no-cone '/*' '!web'
make leaf
sudo make install

echo "leaf updated to $REMOTE_VERSION successfully."
