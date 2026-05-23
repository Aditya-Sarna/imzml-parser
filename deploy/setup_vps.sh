#!/usr/bin/env bash
# =============================================================================
# setup_vps.sh  —  Run this on a fresh Hetzner Ubuntu 24.04 VPS as root
# =============================================================================
set -euo pipefail

DOMAIN="imzmlparser.com"
APP_USER="imzml"
APP_DIR="/opt/imzml"
DATA_DIR="/opt/imzml/data"
PORT=8080

echo "==> Creating app user"
id -u "$APP_USER" &>/dev/null || useradd -m -s /bin/bash "$APP_USER"

echo "==> Installing system packages"
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential cmake git curl wget \
    libxerces-c-dev \
    nginx certbot python3-certbot-nginx \
    ufw \
    ca-certificates

echo "==> Firewall"
ufw allow OpenSSH
ufw allow 'Nginx Full'
ufw --force enable

echo "==> Installing Miniforge (conda) for OpenMS"
MINIFORGE=/opt/miniforge3
if [[ ! -d "$MINIFORGE" ]]; then
    wget -q -O /tmp/miniforge.sh \
        "https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh"
    bash /tmp/miniforge.sh -b -p "$MINIFORGE"
    rm /tmp/miniforge.sh
fi
export PATH="$MINIFORGE/bin:$PATH"

echo "==> Creating openms_env conda environment"
if ! conda env list | grep -q "openms_env"; then
    conda create -y -n openms_env -c bioconda -c conda-forge openms=3.5.0 2>&1 | tail -5
fi

echo "==> Creating directory structure"
mkdir -p "$APP_DIR/build" "$DATA_DIR" "$APP_DIR/logs"
chown -R "$APP_USER:$APP_USER" "$APP_DIR"

echo ""
echo "==> DONE with system setup."
echo ""
echo "Next steps:"
echo "  1.  From your Mac, upload the project and data:"
echo "      rsync -avz --progress /Users/adityasarna/imzml/ root@<VPS_IP>:$APP_DIR/src/"
echo "      rsync -avz --progress '/Users/adityasarna/Downloads/untitled folder 2/' root@<VPS_IP>:$DATA_DIR/"
echo ""
echo "  2.  Run:  bash $APP_DIR/src/deploy/build_on_vps.sh"
echo "  3.  Run:  bash $APP_DIR/src/deploy/certbot_setup.sh"
