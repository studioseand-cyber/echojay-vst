#!/bin/bash
# ============================================================================
# EchoJay DMG Builder — wraps the signed .pkg installer in a DMG
# Run AFTER build-installer.sh, productsign, and notarytool
# Usage: ./package-dmg.sh
# ============================================================================

set -e

PLUGIN_NAME="EchoJay"
VERSION="1.1.0"
DMG_OUTPUT="${PLUGIN_NAME}-v${VERSION}.dmg"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Find the signed pkg
PKG_PATH="${SCRIPT_DIR}/${PLUGIN_NAME}-v${VERSION}-Installer-Signed.pkg"
if [ ! -f "$PKG_PATH" ]; then
    PKG_PATH="${SCRIPT_DIR}/${PLUGIN_NAME}-v${VERSION}-Installer.pkg"
fi

if [ ! -f "$PKG_PATH" ]; then
    echo "ERROR: No installer .pkg found."
    echo "Run build-installer.sh and productsign first."
    exit 1
fi

echo "=== Packaging ${PLUGIN_NAME} v${VERSION} DMG ==="
echo "Using: $(basename "$PKG_PATH")"

STAGING_DIR="/tmp/${PLUGIN_NAME}_dmg_staging"
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"
cp "$PKG_PATH" "$STAGING_DIR/Install EchoJay.pkg"

rm -f "$DMG_OUTPUT"

hdiutil create \
    -volname "${PLUGIN_NAME} Installer" \
    -srcfolder "$STAGING_DIR" \
    -ov -format UDZO \
    -imagekey zlib-level=9 \
    "$DMG_OUTPUT"

rm -rf "$STAGING_DIR"

if [ -f "$DMG_OUTPUT" ]; then
    echo ""
    echo "  ╔══════════════════════════════════════╗"
    echo "  ║           DMG ready to send          ║"
    echo "  ╚══════════════════════════════════════╝"
    echo ""
    echo "  File: ${DMG_OUTPUT}"
    echo "  Size: $(du -h "$DMG_OUTPUT" | cut -f1)"
    echo ""
    echo "  User opens DMG → double-clicks"
    echo "  'Install EchoJay.pkg' → follows installer."
    echo ""
else
    echo "ERROR: DMG creation failed."
    exit 1
fi
