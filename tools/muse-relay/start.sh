#!/bin/sh
# Container entry: relay on :8787 + Cloudflare quick tunnel (no account needed).
# Everything persistent lives in /data (host folder).
#
# The relay runs in a restart loop so it can be updated without restarting the
# container: `pkill -f "node /data/relay.js"` reloads it while cloudflared, and
# therefore the public tunnel hostname, keeps running.
cd /data || exit 1
trap 'kill 0; exit 0' TERM INT
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
  # Watchdog: a quick tunnel can lose its edge connection while cloudflared keeps
  # running (HTTP 530 forever). Probe the public URL; after 5 failed minutes,
  # restart cloudflared. The relay publishes the new hostname for the Tab5.
  (
    failures=0
    sleep 120
    while true; do
      base=$(grep -o 'https://[a-z0-9-]*\.trycloudflare\.com' /data/tunnel.log 2>/dev/null | tail -1)
      if [ -n "$base" ] && ! wget -q -T 15 -O /dev/null "$base/health"; then
        failures=$((failures + 1))
        echo "tunnel probe failed ($failures/5): $base"
        if [ "$failures" -ge 5 ]; then
          echo "tunnel unreachable, restarting cloudflared"
          pkill -f "/data/cloudflared tunnel"
          failures=0
          sleep 60
        fi
      else
        failures=0
      fi
      sleep 60
    done
  ) &
fi
while true; do
  node /data/relay.js &
  wait $!
  echo "relay exited ($?), restarting in 2 s"
  sleep 2
done
