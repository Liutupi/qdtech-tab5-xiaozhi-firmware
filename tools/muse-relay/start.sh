#!/bin/sh
# Container entry: relay on :8787 + Cloudflare quick tunnel (no account needed).
# Everything persistent lives in /data (host folder).
cd /data || exit 1
ARCH=$(uname -m)
case "$ARCH" in
  x86_64) CF=cloudflared-linux-amd64 ;;
  aarch64) CF=cloudflared-linux-arm64 ;;
  *) CF=cloudflared-linux-amd64 ;;
esac
if [ ! -x /data/cloudflared ]; then
  echo "downloading cloudflared ($CF)"
  wget -q -O /data/cloudflared.tmp "https://github.com/cloudflare/cloudflared/releases/latest/download/$CF" \
    && chmod +x /data/cloudflared.tmp && mv /data/cloudflared.tmp /data/cloudflared \
    || echo "cloudflared download failed; relay runs LAN-only"
fi
if [ -x /data/cloudflared ]; then
  (
    while true; do
      : > /data/tunnel.log
      /data/cloudflared tunnel --no-autoupdate --url http://127.0.0.1:8787 >> /data/tunnel.log 2>&1
      echo "cloudflared exited, restarting in 10 s" >> /data/tunnel.log
      sleep 10
    done
  ) &
fi
exec node /data/relay.js
