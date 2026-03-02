#!/bin/bash
# Install Eucalypso module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/eucalypso" ]; then
    echo "Error: dist/eucalypso not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Eucalypso Module ==="

# Deploy to Move - midi_fx subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/midi_fx/eucalypso"
scp -r dist/eucalypso/* ableton@move.local:/data/UserData/move-anything/modules/midi_fx/eucalypso/

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    if ls src/chain_patches/*.json 1>/dev/null 2>&1; then
        scp src/chain_patches/*.json ableton@move.local:/data/UserData/move-anything/patches/
    fi
fi

# Set permissions so Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/midi_fx/eucalypso"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/midi_fx/eucalypso/"
echo ""
echo "Restart Move Anything to load the new module."
