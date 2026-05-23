#!/usr/bin/env bash
# =============================================================================
# push_to_vps.sh  —  Run on your Mac to push code + data to the Hetzner VPS
# Usage: bash deploy/push_to_vps.sh <VPS_IP>
# =============================================================================
set -euo pipefail

VPS_IP="${1:?Usage: $0 <VPS_IP>}"
VPS_USER="root"
DATA_DIR="/Users/adityasarna/Downloads/untitled folder 2"

echo "==> Syncing source code  → $VPS_IP:/opt/imzml/src"
rsync -avz --progress \
    --exclude='.git' \
    --exclude='build/' \
    /Users/adityasarna/imzml/ \
    "$VPS_USER@$VPS_IP:/opt/imzml/src/"

echo ""
echo "==> Syncing imzML data   → $VPS_IP:/opt/imzml/data"
rsync -avz --progress \
    "$DATA_DIR/" \
    "$VPS_USER@$VPS_IP:/opt/imzml/data/"

echo ""
echo "==> Done.  Next steps on the VPS:"
echo "    ssh root@$VPS_IP"
echo "    bash /opt/imzml/src/deploy/setup_vps.sh     # first-run only"
echo "    bash /opt/imzml/src/deploy/build_on_vps.sh"
echo "    bash /opt/imzml/src/deploy/deploy_service.sh"
