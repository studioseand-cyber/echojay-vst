#!/bin/bash
# ============================================================================
# EchoJay Professional Installer Builder
# Creates a .pkg installer with welcome, license, and component selection
# Uses pkgbuild + productbuild (built into Xcode command line tools)
#
# Usage: ./build-installer.sh
# Run AFTER: cmake --build build --config Release
# ============================================================================

set -e

PLUGIN_NAME="EchoJay"
IDENTIFIER="com.echojay.plugin"
VERSION="1.0.9"
BUILD_DIR="build"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_DIR="/tmp/${PLUGIN_NAME}_pkg_build"
RESOURCES_DIR="${SCRIPT_DIR}/installer"
OUTPUT="${SCRIPT_DIR}/${PLUGIN_NAME}-v${VERSION}-Installer.pkg"

echo ""
echo "  ========================================"
echo "   Building ${PLUGIN_NAME} v${VERSION} Installer"
echo "  ========================================"
echo ""

# --- Find built plugins ---
VST3_PATH=""
AU_PATH=""
AAX_PATH=""

for d in "${BUILD_DIR}/${PLUGIN_NAME}_artefacts/Release/VST3" \
         "${BUILD_DIR}/${PLUGIN_NAME}_artefacts/VST3" \
         "${BUILD_DIR}/EchoJay_artefacts/Release/VST3" \
         "${BUILD_DIR}/EchoJay_artefacts/VST3"; do
    [ -d "${d}/${PLUGIN_NAME}.vst3" ] && VST3_PATH="${d}/${PLUGIN_NAME}.vst3" && break
done

for d in "${BUILD_DIR}/${PLUGIN_NAME}_artefacts/Release/AU" \
         "${BUILD_DIR}/${PLUGIN_NAME}_artefacts/AU" \
         "${BUILD_DIR}/EchoJay_artefacts/Release/AU" \
         "${BUILD_DIR}/EchoJay_artefacts/AU"; do
    [ -d "${d}/${PLUGIN_NAME}.component" ] && AU_PATH="${d}/${PLUGIN_NAME}.component" && break
done

for d in "${BUILD_DIR}/${PLUGIN_NAME}_artefacts/Release/AAX" \
         "${BUILD_DIR}/${PLUGIN_NAME}_artefacts/AAX" \
         "${BUILD_DIR}/EchoJay_artefacts/Release/AAX" \
         "${BUILD_DIR}/EchoJay_artefacts/AAX"; do
    [ -d "${d}/${PLUGIN_NAME}.aaxplugin" ] && AAX_PATH="${d}/${PLUGIN_NAME}.aaxplugin" && break
done

if [ -z "$VST3_PATH" ] && [ -z "$AU_PATH" ] && [ -z "$AAX_PATH" ]; then
    echo "  ERROR: No built plugins found."
    echo "  Run: cmake --build build --config Release"
    exit 1
fi

echo "  VST3: ${VST3_PATH:-not found}"
echo "  AU:   ${AU_PATH:-not found}"
echo "  AAX:  ${AAX_PATH:-not found}"
echo ""

# --- Create installer resources (always overwrite to keep in sync) ---
mkdir -p "$RESOURCES_DIR"

# Welcome HTML — ASCII only, no special characters
cat > "${RESOURCES_DIR}/welcome.html" << 'WELCOME'
<html>
<head><meta charset="utf-8"></head>
<body style="font-family: -apple-system, Helvetica Neue, sans-serif; margin: 20px; color: #1d1d1f;">
<h1 style="font-size: 22px; font-weight: 600;">EchoJay - AI Mix Assistant</h1>
<p style="font-size: 14px; line-height: 1.6; color: #424245;">
This installer will set up EchoJay on your system. The following components will be installed:
</p>
<ul style="font-size: 14px; line-height: 1.8; color: #424245;">
<li><strong>VST3 plugin</strong> - for Ableton, FL Studio, Reaper, Studio One, Cubase, Bitwig</li>
<li><strong>Audio Unit (AU) plugin</strong> - for Logic Pro, GarageBand, and AU-compatible hosts</li>
<li><strong>AAX plugin</strong> - for Pro Tools</li>
</ul>
<p style="font-size: 14px; line-height: 1.6; color: #424245;">
Click <strong>Continue</strong> to proceed.
</p>
<p style="font-size: 12px; color: #86868b; margin-top: 30px;">
EchoJay v1.0.9 | echojay.ai
</p>
</body>
</html>
WELCOME
echo "  Created: installer/welcome.html"

# License — ASCII only
cat > "${RESOURCES_DIR}/license.html" << 'LICENSE'
<html>
<head><meta charset="utf-8"></head>
<body style="font-family: -apple-system, Helvetica Neue, sans-serif; margin: 20px; color: #1d1d1f;">
<h2 style="font-size: 18px; font-weight: 600;">End User License Agreement</h2>
<p style="font-size: 13px; line-height: 1.6; color: #424245;">
Copyright 2026 EchoJay. All rights reserved.
</p>
<p style="font-size: 13px; line-height: 1.6; color: #424245;">
This software is licensed, not sold. By installing EchoJay, you agree to the following terms:
</p>
<ol style="font-size: 13px; line-height: 1.8; color: #424245;">
<li>You may install and use EchoJay on any computers you personally own.</li>
<li>You may not redistribute, reverse-engineer, or modify the software.</li>
<li>The software is provided "as is" without warranty of any kind.</li>
<li>EchoJay reserves the right to update these terms with new versions.</li>
<li>Your use of EchoJay's AI features is subject to the terms at echojay.ai</li>
</ol>
<p style="font-size: 13px; line-height: 1.6; color: #424245;">
For questions, contact hello@echojay.ai
</p>
</body>
</html>
LICENSE
echo "  Created: installer/license.html"

# Conclusion — ASCII only, no checkmark, correct support URL
cat > "${RESOURCES_DIR}/conclusion.html" << 'CONCLUSION'
<html>
<head><meta charset="utf-8"></head>
<body style="font-family: -apple-system, Helvetica Neue, sans-serif; margin: 20px; color: #1d1d1f;">
<h1 style="font-size: 22px; font-weight: 600;">Installation Complete</h1>
<p style="font-size: 14px; line-height: 1.6; color: #424245;">
EchoJay has been installed successfully.
</p>
<p style="font-size: 14px; line-height: 1.6; color: #424245;">
<strong>Next steps:</strong>
</p>
<ol style="font-size: 14px; line-height: 1.8; color: #424245;">
<li>Open your DAW</li>
<li>Rescan your plugins if needed</li>
<li>Find <strong>EchoJay</strong> in your plugin list</li>
<li>Insert it on your mix bus and log in</li>
</ol>
<p style="font-size: 13px; color: #86868b; margin-top: 30px;">
Need help? Visit echojay.ai
</p>
</body>
</html>
CONCLUSION
echo "  Created: installer/conclusion.html"

# --- Clean previous build ---
rm -rf "$PKG_DIR"
mkdir -p "${PKG_DIR}/vst3_payload" "${PKG_DIR}/au_payload" "${PKG_DIR}/aax_payload" "${PKG_DIR}/scripts" "${PKG_DIR}/components"

# --- Stage payloads with correct install paths ---

if [ -n "$VST3_PATH" ]; then
    mkdir -p "${PKG_DIR}/vst3_payload/Library/Audio/Plug-Ins/VST3"
    cp -R "$VST3_PATH" "${PKG_DIR}/vst3_payload/Library/Audio/Plug-Ins/VST3/"
    echo "  Staged VST3 payload"
fi

if [ -n "$AU_PATH" ]; then
    mkdir -p "${PKG_DIR}/au_payload/Library/Audio/Plug-Ins/Components"
    cp -R "$AU_PATH" "${PKG_DIR}/au_payload/Library/Audio/Plug-Ins/Components/"
    echo "  Staged AU payload"
fi

# AAX installs to /Library/Application Support/Avid/Audio/Plug-Ins/ (system-wide)
if [ -n "$AAX_PATH" ]; then
    mkdir -p "${PKG_DIR}/aax_payload/Library/Application Support/Avid/Audio/Plug-Ins"
    cp -R "$AAX_PATH" "${PKG_DIR}/aax_payload/Library/Application Support/Avid/Audio/Plug-Ins/"
    echo "  Staged AAX payload"
fi

# --- Post-install script to reset AU cache ---
cat > "${PKG_DIR}/scripts/postinstall" << 'POSTINSTALL'
#!/bin/bash
killall -9 AudioComponentRegistrar 2>/dev/null || true
exit 0
POSTINSTALL
chmod +x "${PKG_DIR}/scripts/postinstall"

# --- Build component packages ---
echo ""
echo "  Building component packages..."

if [ -n "$VST3_PATH" ]; then
    pkgbuild \
        --root "${PKG_DIR}/vst3_payload" \
        --install-location "$HOME" \
        --identifier "${IDENTIFIER}.vst3" \
        --version "$VERSION" \
        "${PKG_DIR}/components/EchoJay-VST3.pkg"
    echo "  Done: VST3 component"
fi

if [ -n "$AU_PATH" ]; then
    pkgbuild \
        --root "${PKG_DIR}/au_payload" \
        --install-location "$HOME" \
        --identifier "${IDENTIFIER}.au" \
        --version "$VERSION" \
        --scripts "${PKG_DIR}/scripts" \
        "${PKG_DIR}/components/EchoJay-AU.pkg"
    echo "  Done: AU component"
fi

# AAX installs system-wide (needs admin for /Library/)
if [ -n "$AAX_PATH" ]; then
    pkgbuild \
        --root "${PKG_DIR}/aax_payload" \
        --install-location "/" \
        --identifier "${IDENTIFIER}.aax" \
        --version "$VERSION" \
        "${PKG_DIR}/components/EchoJay-AAX.pkg"
    echo "  Done: AAX component"
fi

# --- Build distribution XML ---
# Dynamically build choices based on what was found
CHOICES_OUTLINE=""
CHOICES=""
PKG_REFS=""

if [ -n "$VST3_PATH" ]; then
    CHOICES_OUTLINE="${CHOICES_OUTLINE}        <line choice=\"vst3\"/>\n"
    CHOICES="${CHOICES}
    <choice id=\"vst3\"
            title=\"VST3 Plugin\"
            description=\"For Ableton, FL Studio, Reaper, Studio One, Cubase, Bitwig, and other VST3 hosts.\"
            selected=\"true\">
        <pkg-ref id=\"${IDENTIFIER}.vst3\"/>
    </choice>"
    PKG_REFS="${PKG_REFS}
    <pkg-ref id=\"${IDENTIFIER}.vst3\" version=\"${VERSION}\">EchoJay-VST3.pkg</pkg-ref>"
fi

if [ -n "$AU_PATH" ]; then
    CHOICES_OUTLINE="${CHOICES_OUTLINE}        <line choice=\"au\"/>\n"
    CHOICES="${CHOICES}
    <choice id=\"au\"
            title=\"Audio Unit Plugin\"
            description=\"For Logic Pro, GarageBand, and other AU-compatible hosts.\"
            selected=\"true\">
        <pkg-ref id=\"${IDENTIFIER}.au\"/>
    </choice>"
    PKG_REFS="${PKG_REFS}
    <pkg-ref id=\"${IDENTIFIER}.au\" version=\"${VERSION}\">EchoJay-AU.pkg</pkg-ref>"
fi

if [ -n "$AAX_PATH" ]; then
    CHOICES_OUTLINE="${CHOICES_OUTLINE}        <line choice=\"aax\"/>\n"
    CHOICES="${CHOICES}
    <choice id=\"aax\"
            title=\"AAX Plugin (Pro Tools)\"
            description=\"For Avid Pro Tools. Requires admin password during installation.\"
            selected=\"true\">
        <pkg-ref id=\"${IDENTIFIER}.aax\"/>
    </choice>"
    PKG_REFS="${PKG_REFS}
    <pkg-ref id=\"${IDENTIFIER}.aax\" version=\"${VERSION}\">EchoJay-AAX.pkg</pkg-ref>"
fi

cat > "${PKG_DIR}/distribution.xml" << DISTXML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>EchoJay v${VERSION}</title>
    <organization>${IDENTIFIER}</organization>
    <options customize="allow" require-scripts="false" hostArchitectures="x86_64,arm64"/>

    <welcome file="welcome.html"/>
    <license file="license.html"/>
    <conclusion file="conclusion.html"/>

    <domains enable_localSystem="true" enable_currentUserHome="true"/>

    <choices-outline>
$(echo -e "$CHOICES_OUTLINE")    </choices-outline>
${CHOICES}
${PKG_REFS}
</installer-gui-script>
DISTXML

# --- Build the final product installer ---
echo ""
echo "  Building installer package..."

rm -f "$OUTPUT"

productbuild \
    --distribution "${PKG_DIR}/distribution.xml" \
    --resources "$RESOURCES_DIR" \
    --package-path "${PKG_DIR}/components" \
    "$OUTPUT"

# --- Done ---
if [ -f "$OUTPUT" ]; then
    echo ""
    echo "  ========================================"
    echo "   Installer ready"
    echo "  ========================================"
    echo ""
    echo "  File: $(basename "$OUTPUT")"
    echo "  Size: $(du -h "$OUTPUT" | cut -f1)"
    echo "  Path: ${OUTPUT}"
    echo ""
else
    echo "ERROR: Installer build failed."
    exit 1
fi

# Optional: wrap in DMG
read -p "  Wrap in a DMG for distribution? (y/n) " WRAP_DMG
if [ "$WRAP_DMG" = "y" ] || [ "$WRAP_DMG" = "Y" ]; then
    DMG_OUTPUT="${SCRIPT_DIR}/${PLUGIN_NAME}-v${VERSION}.dmg"
    DMG_STAGING="/tmp/${PLUGIN_NAME}_dmg_wrap"
    rm -rf "$DMG_STAGING" "$DMG_OUTPUT"
    mkdir -p "$DMG_STAGING"
    cp "$OUTPUT" "$DMG_STAGING/"

    hdiutil create \
        -volname "${PLUGIN_NAME} Installer" \
        -srcfolder "$DMG_STAGING" \
        -ov -format UDZO \
        -imagekey zlib-level=9 \
        "$DMG_OUTPUT"

    rm -rf "$DMG_STAGING"
    echo ""
    echo "  DMG: $(basename "$DMG_OUTPUT") ($(du -h "$DMG_OUTPUT" | cut -f1))"
fi

rm -rf "$PKG_DIR"
echo ""
echo "  Done."
