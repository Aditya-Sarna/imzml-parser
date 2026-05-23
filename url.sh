#!/bin/zsh
# Show the current localhost.run public URL
# Run: ./url.sh

LOG=/Users/adityasarna/imzml/logs/tunnel.log

if [[ ! -f "$LOG" ]]; then
    echo "Tunnel log not found. Is the tunnel running?"
    exit 1
fi

URL=$(grep -Eo 'https://[a-z0-9]+\.lhr\.life' "$LOG" | tail -1)
if [[ -z "$URL" ]]; then
    echo "URL not yet assigned — tunnel may still be connecting."
    echo "Last few log lines:"
    tail -5 "$LOG"
else
    echo "Public URL: $URL"
fi
