#!/bin/bash
# ============================================================================
# EchoJay DMG Installer Builder
# Requires: brew install create-dmg
# Run AFTER a successful cmake build
# Usage: ./package-dmg.sh
# ============================================================================

set -e

PLUGIN_NAME="EchoJay"
VERSION="1.0.9"
DMG_NAME="${PLUGIN_NAME}-v${VERSION}"
BUILD_DIR="build"
STAGING_DIR="/tmp/${PLUGIN_NAME}_dmg_staging"
DMG_OUTPUT="${DMG_NAME}.dmg"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BG_IMAGE="${SCRIPT_DIR}/Resources/dmg-background.png"

echo "=== Packaging ${PLUGIN_NAME} v${VERSION} ==="

# --- Check create-dmg is installed ---
if ! command -v create-dmg &> /dev/null; then
    echo "ERROR: create-dmg not found. Install it with:"
    echo "  brew install create-dmg"
    exit 1
fi

# --- Find built plugins ---
VST3_PATH=""
AU_PATH=""

for searchDir in \
    "${BUILD_DIR}/${PLUGIN_NAME}_artefacts/Release/VST3" \
    "${BUILD_DIR}/${PLUGIN_NAME}_artefacts/VST3" \
    "${BUILD_DIR}/EchoJay_artefacts/Release/VST3" \
    "${BUILD_DIR}/EchoJay_artefacts/VST3"; do
    if [ -d "${searchDir}/${PLUGIN_NAME}.vst3" ]; then
        VST3_PATH="${searchDir}/${PLUGIN_NAME}.vst3"
        break
    fi
done

for searchDir in \
    "${BUILD_DIR}/${PLUGIN_NAME}_artefacts/Release/AU" \
    "${BUILD_DIR}/${PLUGIN_NAME}_artefacts/AU" \
    "${BUILD_DIR}/EchoJay_artefacts/Release/AU" \
    "${BUILD_DIR}/EchoJay_artefacts/AU"; do
    if [ -d "${searchDir}/${PLUGIN_NAME}.component" ]; then
        AU_PATH="${searchDir}/${PLUGIN_NAME}.component"
        break
    fi
done

if [ -z "$VST3_PATH" ] && [ -z "$AU_PATH" ]; then
    echo "ERROR: No built plugins found in ${BUILD_DIR}/"
    echo "Run: cmake --build build --config Release"
    exit 1
fi

echo "Found VST3: ${VST3_PATH:-not found}"
echo "Found AU:   ${AU_PATH:-not found}"

# --- Clean and create staging folder ---
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"

# Copy plugins
[ -n "$VST3_PATH" ] && cp -R "$VST3_PATH" "$STAGING_DIR/"
[ -n "$AU_PATH" ] && cp -R "$AU_PATH" "$STAGING_DIR/"

# --- Create the install script (installs to correct audio plugin folders) ---
cat > "${STAGING_DIR}/Install EchoJay.command" << 'INSTALL_SCRIPT'
#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN="EchoJay"

echo ""
echo "  ╔══════════════════════════════════════╗"
echo "  ║     Installing EchoJay plugins...    ║"
echo "  ╚══════════════════════════════════════╝"
echo ""

INSTALLED=0

if [ -d "${SCRIPT_DIR}/${PLUGIN}.vst3" ]; then
    DIR="$HOME/Library/Audio/Plug-Ins/VST3"
    mkdir -p "$DIR"
    rm -rf "${DIR}/${PLUGIN}.vst3"
    cp -R "${SCRIPT_DIR}/${PLUGIN}.vst3" "$DIR/"
    echo "  ✓  VST3  →  ${DIR}/${PLUGIN}.vst3"
    INSTALLED=1
fi

if [ -d "${SCRIPT_DIR}/${PLUGIN}.component" ]; then
    DIR="$HOME/Library/Audio/Plug-Ins/Components"
    mkdir -p "$DIR"
    rm -rf "${DIR}/${PLUGIN}.component"
    cp -R "${SCRIPT_DIR}/${PLUGIN}.component" "$DIR/"
    echo "  ✓  AU    →  ${DIR}/${PLUGIN}.component"
    INSTALLED=1
fi

if [ $INSTALLED -eq 0 ]; then
    echo "  ✗  No plugins found. Re-download the DMG."
    exit 1
fi

echo ""
echo "  Done — restart your DAW to load EchoJay."
echo ""
read -p "  Press Enter to close..."
INSTALL_SCRIPT
chmod +x "${STAGING_DIR}/Install EchoJay.command"

# --- Remove old DMG ---
rm -f "$DMG_OUTPUT"

# --- Build the DMG ---
echo ""
echo "Building DMG..."

CMD=(create-dmg
    --volname "$PLUGIN_NAME"
    --window-pos 200 120
    --window-size 600 400
    --icon-size 72
    --text-size 13
    --hide-extension "EchoJay.vst3"
    --hide-extension "EchoJay.component"
    --hide-extension "Install EchoJay.command"
    --no-internet-enable
)

# Add background if it exists
if [ -f "$BG_IMAGE" ]; then
    CMD+=(--background "$BG_IMAGE")
fi

# Position icons to match the background layout
if [ -n "$VST3_PATH" ] && [ -n "$AU_PATH" ]; then
    CMD+=(--icon "EchoJay.vst3" 150 200)
    CMD+=(--icon "EchoJay.component" 300 200)
    CMD+=(--icon "Install EchoJay.command" 450 200)
elif [ -n "$VST3_PATH" ]; then
    CMD+=(--icon "EchoJay.vst3" 200 200)
    CMD+=(--icon "Install EchoJay.command" 400 200)
else
    CMD+=(--icon "EchoJay.component" 200 200)
    CMD+=(--icon "Install EchoJay.command" 400 200)
fi

CMD+=("$DMG_OUTPUT" "$STAGING_DIR")

# create-dmg returns non-zero if signing fails (expected without a cert)
"${CMD[@]}" || true

# --- Check output ---
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
    echo "  'Install EchoJay' → restarts DAW."
    echo ""
else
    echo "ERROR: DMG creation failed."
    exit 1
fi

# --- Cleanup ---
rm -rf "$STAGING_DIR"
