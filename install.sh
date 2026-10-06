#!/usr/bin/env bash
set -e

echo "=== Installing dependencies for WPE URL Extractor ==="

# Check OS and install distro packages
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "Detected OS: $PRETTY_NAME"
fi

sudo apt-get update

# Install Debian / Ubuntu packages
sudo apt-get install -y \
    libwpewebkit-2.0-dev \
    libwpebackend-fdo-1.0-dev \
    libwpe-1.0-dev \
    pkg-config \
    build-essential \
    cmake \
    libglib2.0-dev \
    libsoup-3.0-dev \
    time \
    curl

echo "=== Dependencies installed successfully! ==="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "=== Building wpe-url-extractor ==="
mkdir -p "$SCRIPT_DIR/build"
cd "$SCRIPT_DIR/build"
cmake ..
cmake --build . -j1

echo "=== Build completed successfully! Executable: $SCRIPT_DIR/build/wpe-url-extractor ==="
