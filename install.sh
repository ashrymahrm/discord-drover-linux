#!/usr/bin/env bash
set -e

# Discord Drover for Linux - CLI Installer
# Automatically installs libdrover.so and configures Discord desktop integration

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$SCRIPT_DIR"

echo "=================================================="
echo "    Discord Drover for Linux - Installer         "
echo "=================================================="

# Check and build shared library
if [ ! -f "libdrover.so" ]; then
    echo "[*] Building libdrover.so..."
    make
fi

HOME_DIR="${HOME:-/home/$(whoami)}"
INSTALL_DIR="$HOME_DIR/.config/discord"
APPS_DIR="$HOME_DIR/.local/share/applications"

# Check if user has vesktop or native discord
if [ -d "$HOME_DIR/.config/vesktop" ] && [ ! -d "$INSTALL_DIR" ]; then
    INSTALL_DIR="$HOME_DIR/.config/vesktop"
    CLIENT_NAME="Vesktop"
else
    CLIENT_NAME="Discord"
fi

mkdir -p "$INSTALL_DIR"
mkdir -p "$APPS_DIR"

echo "[*] Installing to $INSTALL_DIR..."
cp -f libdrover.so "$INSTALL_DIR/libdrover.so"
cp -f drover-packet.bin "$INSTALL_DIR/drover-packet.bin"

if [ ! -f "$INSTALL_DIR/drover.ini" ]; then
    cp -f drover.ini "$INSTALL_DIR/drover.ini"
fi

# Configure desktop launcher
DESKTOP_FILE="$APPS_DIR/discord.desktop"
echo "[*] Creating desktop launcher override: $DESKTOP_FILE"

cat << EOF > "$DESKTOP_FILE"
[Desktop Entry]
Name=Discord
StartupWMClass=discord
Comment=Discord with Drover Direct Mode VoIP bypass
GenericName=Internet Messenger
Exec=env LD_PRELOAD="$INSTALL_DIR/libdrover.so" DROVER_DIR="$INSTALL_DIR" discord %U
Icon=discord
Type=Application
Categories=Network;InstantMessaging;
MimeType=x-scheme-handler/discord;
EOF

chmod +x "$DESKTOP_FILE"
update-desktop-database "$APPS_DIR" 2>/dev/null || true

echo ""
echo "[✓] Installation completed successfully!"
echo "    - Library: $INSTALL_DIR/libdrover.so"
echo "    - Config:  $INSTALL_DIR/drover.ini"
echo "    - Launcher: $DESKTOP_FILE"
echo ""
echo "To launch Discord with Drover now, run:"
echo "    env LD_PRELOAD=\"$INSTALL_DIR/libdrover.so\" discord &"
echo ""
echo "Or run the Graphical Installer / Config tool anytime:"
echo "    ./drover"
echo "=================================================="
