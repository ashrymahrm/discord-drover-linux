#!/usr/bin/env bash
set -e

# Discord Drover for Linux - Uninstaller

HOME_DIR="${HOME:-/home/$(whoami)}"
APPS_DIR="$HOME_DIR/.local/share/applications"

echo "=================================================="
echo "    Discord Drover for Linux - Uninstaller       "
echo "=================================================="

# Remove desktop entry override
if [ -f "$APPS_DIR/discord.desktop" ]; then
    echo "[*] Removing desktop entry override: $APPS_DIR/discord.desktop"
    rm -f "$APPS_DIR/discord.desktop"
    update-desktop-database "$APPS_DIR" 2>/dev/null || true
fi

# Remove library and packet from config directories
for dir in "$HOME_DIR/.config/discord" "$HOME_DIR/.config/vesktop" "$HOME_DIR/.var/app/com.discordapp.Discord/config/discord"; do
    if [ -d "$dir" ]; then
        if [ -f "$dir/libdrover.so" ]; then
            echo "[*] Removing $dir/libdrover.so"
            rm -f "$dir/libdrover.so"
        fi
        if [ -f "$dir/drover-packet.bin" ]; then
            rm -f "$dir/drover-packet.bin"
        fi
    fi
done

echo ""
echo "[✓] Discord Drover has been uninstalled successfully."
echo "=================================================="
