#!/bin/bash
# Usage: ./deploy_rsv.sh <username> <host>

set -e

if [ $# -ne 2 ]; then
    echo "Usage: $0 <username> <host>"
    exit 1
fi

USERNAME=$1
HOST=$2

# Prerequisites
clear
echo "Cleaning old executable on remote..."
ssh "${USERNAME}@${HOST}" "rm -f rsv" || true

# Compilation
echo "Building project..."
mkdir -p build
cd build
cmake ..
make

# Deployment
echo "Copying executable to remote host..."
scp ./rsv "${USERNAME}@${HOST}:/home/${USERNAME}/"

echo "Deployment complete!"
