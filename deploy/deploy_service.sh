#!/usr/bin/env bash
# =============================================================================
# deploy_service.sh  —  Run as root on the VPS (AFTER build_on_vps.sh)
# Installs systemd service, nginx reverse proxy, and obtains SSL cert.
# =============================================================================
set -euo pipefail

SRC="/opt/imzml/src"

# --------------------------------------------------------------------------- #
# 1.  Dedicated system user (no login shell, no home dir)
# --------------------------------------------------------------------------- #
if ! id -u imzml &>/dev/null; then
    useradd --system --no-create-home --shell /usr/sbin/nologin imzml
    echo "==> Created system user 'imzml'"
fi
chown -R imzml:imzml /opt/imzml

# --------------------------------------------------------------------------- #
# 2.  systemd service
# --------------------------------------------------------------------------- #
cp "$SRC/deploy/imzml-server.service" /etc/systemd/system/imzml-server.service
systemctl daemon-reload
systemctl enable imzml-server
systemctl restart imzml-server
echo "==> imzml-server service started"
systemctl status imzml-server --no-pager

# --------------------------------------------------------------------------- #
# 3.  nginx config
# --------------------------------------------------------------------------- #
cp "$SRC/deploy/nginx_imzml.conf" /etc/nginx/sites-available/imzml
ln -sf /etc/nginx/sites-available/imzml /etc/nginx/sites-enabled/imzml
# Remove the default site if present
rm -f /etc/nginx/sites-enabled/default
nginx -t
systemctl reload nginx
echo "==> nginx configured for imzmlparser.com"

# --------------------------------------------------------------------------- #
# 4.  UFW firewall
# --------------------------------------------------------------------------- #
ufw allow OpenSSH
ufw allow 'Nginx Full'
ufw --force enable
echo "==> UFW rules applied (SSH + HTTP + HTTPS)"

# --------------------------------------------------------------------------- #
# 5.  SSL certificate via Certbot
# --------------------------------------------------------------------------- #
DOMAIN="imzmlparser.com"
EMAIL="${CERTBOT_EMAIL:-}"

if [[ -z "$EMAIL" ]]; then
    read -rp "Enter your e-mail for Let's Encrypt notifications: " EMAIL
fi

certbot --nginx \
    -d "$DOMAIN" -d "www.$DOMAIN" \
    --non-interactive \
    --agree-tos \
    -m "$EMAIL" \
    --redirect

echo ""
echo "============================================================"
echo " Done!  imzmlparser.com is now live with HTTPS."
echo " Monitor logs: journalctl -fu imzml-server"
echo "============================================================"
