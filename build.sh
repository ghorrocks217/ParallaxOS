#!/bin/bash

# Navigate to the directory containing this script
cd "$(dirname "$0")" || exit 1

echo "=== Building ==="
make

if [ $? -ne 0 ]; then
    echo
    echo "BUILD FAILED"
    read -p "Press enter to exit..."
    exit 1
fi

echo "=== Booting in QEMU ==="
qemu-system-i386 -drive format=raw,file=os.img
