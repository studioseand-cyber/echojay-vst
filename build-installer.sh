#!/bin/bash
# ============================================================================
# EchoJay V2 Professional Installer Builder
# Creates a .pkg installer with welcome, license, and component selection.
# Packages BOTH plugins: EchoJay V2 (VST3/AU/AAX) and EchoJay Link (VST3/AU).
# Uses pkgbuild + productbuild (built into Xcode command line tools)
#
# Usage: ./build-installer.sh
# Run AFTER: cmake --build build --config Release
# Then: productsign + notarytool + stapler (see RELEASE.md)
# ============================================================================

set -e

PLUGIN_NAME="EchoJay V2"
LINK_NAME="EchoJay Link"
IDENTIFIER="com.echojay.plugin.v2"
LINK_IDENTIFIER="com.echojay.link"
VERSION="2.23.63"
# Display-only: v2.MM.PP padding for FILENAMES and human text; every
# parsed field (pkgbuild --version, plists) keeps the numeric $VERSION.
DISPLAY_VERSION=$(echo "$VERSION" | awk -F. '{printf "%d.%02d.%02d", $1, $2, $3}')
LINK_VERSION="0.8.4"
BUILD_DIR="build"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_DIR="/tmp/EchoJayV2_pkg_build"
RESOURCES_DIR="${SCRIPT_DIR}/installer"
OUTPUT="${SCRIPT_DIR}/EchoJay-V2-v${DISPLAY_VERSION}-Installer.pkg"

echo ""
echo "  ========================================"
echo "   Building ${PLUGIN_NAME} v${DISPLAY_VERSION} Installer"
echo "   (includes ${LINK_NAME} v${LINK_VERSION})"
echo "  ========================================"
echo ""

# --- Find built plugins (paths verified against the actual build tree) ---
MAIN_ART="${BUILD_DIR}/EchoJay_artefacts/Release"
LINK_ART="${BUILD_DIR}/EchoJayLink_artefacts/Release"

VST3_PATH="";      [ -d "${MAIN_ART}/VST3/${PLUGIN_NAME}.vst3" ]      && VST3_PATH="${MAIN_ART}/VST3/${PLUGIN_NAME}.vst3"
AU_PATH="";        [ -d "${MAIN_ART}/AU/${PLUGIN_NAME}.component" ]   && AU_PATH="${MAIN_ART}/AU/${PLUGIN_NAME}.component"
AAX_PATH="";       [ -d "${MAIN_ART}/AAX/${PLUGIN_NAME}.aaxplugin" ]  && AAX_PATH="${MAIN_ART}/AAX/${PLUGIN_NAME}.aaxplugin"
LINK_VST3_PATH=""; [ -d "${LINK_ART}/VST3/${LINK_NAME}.vst3" ]        && LINK_VST3_PATH="${LINK_ART}/VST3/${LINK_NAME}.vst3"
LINK_AU_PATH="";   [ -d "${LINK_ART}/AU/${LINK_NAME}.component" ]     && LINK_AU_PATH="${LINK_ART}/AU/${LINK_NAME}.component"
LINK_AAX_PATH="";  [ -d "${LINK_ART}/AAX/${LINK_NAME}.aaxplugin" ]    && LINK_AAX_PATH="${LINK_ART}/AAX/${LINK_NAME}.aaxplugin"

if [ -z "$VST3_PATH" ] && [ -z "$AU_PATH" ] && [ -z "$AAX_PATH" ]; then
    echo "  ERROR: No built EchoJay V2 plugins found."
    echo "  Run: cmake --build build --config Release"
    exit 1
fi
if [ -z "$LINK_VST3_PATH" ] && [ -z "$LINK_AU_PATH" ]; then
    echo "  ERROR: No built EchoJay Link plugins found."
    echo "  Run: cmake --build build --config Release"
    exit 1
fi

echo "  V2 VST3:   ${VST3_PATH:-not found}"
echo "  V2 AU:     ${AU_PATH:-not found}"
echo "  V2 AAX:    ${AAX_PATH:-not found}"
echo "  Link VST3: ${LINK_VST3_PATH:-not found}"
echo "  Link AU:   ${LINK_AU_PATH:-not found}"
echo "  Link AAX:  ${LINK_AAX_PATH:-not found}"

# Background mapper harness (universal console binary, opt-in choice)
MAPPER_PATH=""; [ -f "${BUILD_DIR}/ejextract_artefacts/Release/ejextract" ] && MAPPER_PATH="${BUILD_DIR}/ejextract_artefacts/Release/ejextract"
echo "  Mapper:    ${MAPPER_PATH:-not found (opt-in analysis choice will be omitted)}"
echo ""

# --- Create installer resources (always overwrite to keep in sync) ---
mkdir -p "$RESOURCES_DIR"

# Welcome HTML - ASCII only, no special characters
cat > "${RESOURCES_DIR}/welcome.html" << 'WELCOME'
<html>
<head><meta charset="utf-8"></head>
<body style="font-family: -apple-system, Helvetica Neue, sans-serif; margin: 20px; color: #1d1d1f;">
<h1 style="font-size: 22px; font-weight: 600;">EchoJay V2 - AI Mix Assistant</h1>
<p style="font-size: 14px; line-height: 1.6; color: #424245;">
This installer will set up EchoJay V2 on your system. The following components will be installed:
</p>
<ul style="font-size: 14px; line-height: 1.8; color: #424245;">
<li><strong>EchoJay V2 VST3</strong> - for Ableton, FL Studio, Reaper, Studio One, Cubase, Bitwig</li>
<li><strong>EchoJay V2 Audio Unit (AU)</strong> - for Logic Pro, GarageBand, and AU-compatible hosts</li>
<li><strong>EchoJay V2 AAX</strong> - for Pro Tools</li>
<li><strong>EchoJay Link VST3, AU, and AAX</strong> - the per-channel companion plugin</li>
</ul>
<p style="font-size: 14px; line-height: 1.6; color: #424245;">
Click <strong>Continue</strong> to proceed.
</p>
<p style="font-size: 12px; color: #86868b; margin-top: 30px;">
EchoJay V2 v2.23.2 | echojay.ai
</p>
</body>
</html>
WELCOME
# Welcome heredoc is quoted (no expansion): stamp the CURRENT display version
# into the footer so it can never drift from VERSION again (was hardcoded).
sed -i '' "s/EchoJay V2 v[0-9][0-9.]*/EchoJay V2 v${DISPLAY_VERSION}/" "${RESOURCES_DIR}/welcome.html"
echo "  Created: installer/welcome.html"

# License - ASCII only
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

# Conclusion - ASCII only
cat > "${RESOURCES_DIR}/conclusion.html" << 'CONCLUSION'
<html>
<head><meta charset="utf-8"></head>
<body style="font-family: -apple-system, Helvetica Neue, sans-serif; margin: 20px; color: #1d1d1f;">
<h1 style="font-size: 22px; font-weight: 600;">Installation Complete</h1>
<p style="font-size: 14px; line-height: 1.6; color: #424245;">
EchoJay V2 has been installed successfully.
</p>
<p style="font-size: 14px; line-height: 1.6; color: #424245;">
<strong>Next steps:</strong>
</p>
<ol style="font-size: 14px; line-height: 1.8; color: #424245;">
<li>Open your DAW</li>
<li>Rescan your plugins if needed</li>
<li>Insert <strong>EchoJay V2</strong> on your mix bus and log in</li>
<li>Insert <strong>EchoJay Link</strong> on channels or buses you want EchoJay to see</li>
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
mkdir -p "${PKG_DIR}/vst3_payload" "${PKG_DIR}/au_payload" "${PKG_DIR}/aax_payload" \
         "${PKG_DIR}/link_vst3_payload" "${PKG_DIR}/link_au_payload" "${PKG_DIR}/link_aax_payload" \
         "${PKG_DIR}/scripts" "${PKG_DIR}/aax-scripts" "${PKG_DIR}/components" \
         "${PKG_DIR}/mapper_payload" "${PKG_DIR}/mapper-scripts"

# --- Stage payloads with correct install paths ---

if [ -n "$VST3_PATH" ]; then
    mkdir -p "${PKG_DIR}/vst3_payload/Library/Audio/Plug-Ins/VST3"
    cp -R "$VST3_PATH" "${PKG_DIR}/vst3_payload/Library/Audio/Plug-Ins/VST3/"
    echo "  Staged V2 VST3 payload"
fi

if [ -n "$AU_PATH" ]; then
    mkdir -p "${PKG_DIR}/au_payload/Library/Audio/Plug-Ins/Components"
    cp -R "$AU_PATH" "${PKG_DIR}/au_payload/Library/Audio/Plug-Ins/Components/"
    echo "  Staged V2 AU payload"
fi

# AAX installs to /Library/Application Support/Avid/Audio/Plug-Ins/ (system-wide)
if [ -n "$AAX_PATH" ]; then
    mkdir -p "${PKG_DIR}/aax_payload/Library/Application Support/Avid/Audio/Plug-Ins"
    cp -R "$AAX_PATH" "${PKG_DIR}/aax_payload/Library/Application Support/Avid/Audio/Plug-Ins/"
    echo "  Staged V2 AAX payload"
fi

if [ -n "$LINK_VST3_PATH" ]; then
    mkdir -p "${PKG_DIR}/link_vst3_payload/Library/Audio/Plug-Ins/VST3"
    cp -R "$LINK_VST3_PATH" "${PKG_DIR}/link_vst3_payload/Library/Audio/Plug-Ins/VST3/"
    echo "  Staged Link VST3 payload"
fi

if [ -n "$LINK_AU_PATH" ]; then
    mkdir -p "${PKG_DIR}/link_au_payload/Library/Audio/Plug-Ins/Components"
    cp -R "$LINK_AU_PATH" "${PKG_DIR}/link_au_payload/Library/Audio/Plug-Ins/Components/"
    echo "  Staged Link AU payload"
fi

if [ -n "$LINK_AAX_PATH" ]; then
    mkdir -p "${PKG_DIR}/link_aax_payload/Library/Application Support/Avid/Audio/Plug-Ins"
    cp -R "$LINK_AAX_PATH" "${PKG_DIR}/link_aax_payload/Library/Application Support/Avid/Audio/Plug-Ins/"
    echo "  Staged Link AAX payload"
fi

# --- Background mapper: payload, LaunchAgent plist, codesign, postinstall ---
if [ -n "$MAPPER_PATH" ]; then
    mkdir -p "${PKG_DIR}/mapper_payload/Library/Application Support/EchoJay"
    mkdir -p "${PKG_DIR}/mapper_payload/Library/LaunchAgents"
    cp "$MAPPER_PATH" "${PKG_DIR}/mapper_payload/Library/Application Support/EchoJay/ejextract"
    chmod 755 "${PKG_DIR}/mapper_payload/Library/Application Support/EchoJay/ejextract"

    # Hardened-runtime Developer ID signature so the harness notarizes with
    # the pkg. Signing the staged COPY keeps the build artefact untouched.
    APP_SIGN_ID="Developer ID Application: Sean Donoghue (8BT5F9B887)"
    if security find-identity -v -p codesigning | grep -q "$APP_SIGN_ID"; then
        codesign --force --options runtime --timestamp --sign "$APP_SIGN_ID" \
            "${PKG_DIR}/mapper_payload/Library/Application Support/EchoJay/ejextract"
        echo "  Signed mapper harness (hardened runtime)"
    else
        echo "  WARNING: Developer ID Application identity not found; mapper left unsigned"
    fi

    cat > "${PKG_DIR}/mapper_payload/Library/LaunchAgents/com.echojay.parammapper.plist" << 'MAPPERPLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>com.echojay.parammapper</string>
    <key>ProgramArguments</key>
    <array>
        <string>/Library/Application Support/EchoJay/ejextract</string>
        <string>--bootstrap</string>
    </array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><false/>
    <key>ProcessType</key><string>Background</string>
    <key>Nice</key><integer>19</integer>
    <key>LowPriorityIO</key><true/>
    <key>LowPriorityBackgroundIO</key><true/>
    <key>StandardOutPath</key><string>/tmp/echojay-mapper.log</string>
    <key>StandardErrorPath</key><string>/tmp/echojay-mapper.log</string>
</dict>
</plist>
MAPPERPLIST
    echo "  Staged mapper payload + LaunchAgent plist"

    # Kick the agent off right away for the user sitting at the machine;
    # for everyone else RunAtLoad starts it at next login. The harness is
    # resumable and exits instantly once its ledger says done.
    cat > "${PKG_DIR}/mapper-scripts/postinstall" << 'MAPPERPOST'
#!/bin/bash
CONSOLE_USER=$(stat -f%Su /dev/console 2>/dev/null)
CONSOLE_UID=$(id -u "$CONSOLE_USER" 2>/dev/null)
USER_HOME=$(dscl . -read "/Users/$CONSOLE_USER" NFSHomeDirectory 2>/dev/null | awk '{print $2}')
# Default consent: fetch-only (auto-dial works, nothing shared). Written when
# no marker exists or a legacy in-app "declined" would block the scan the
# user just enabled by ticking this component. The Share choice's own
# postinstall runs AFTER this one and upgrades to fetch-and-contribute.
if [ -n "$USER_HOME" ] && [ -d "$USER_HOME" ]; then
    DIR="$USER_HOME/Library/EchoJay"
    mkdir -p "$DIR"
    if [ ! -f "$DIR/mapping_consent.json" ] || grep -q '"declined"' "$DIR/mapping_consent.json" 2>/dev/null; then
        printf '{"consent":"fetch-only","source":"installer-default","at":"%s"}' "$(date -u +%FT%TZ)" > "$DIR/mapping_consent.json"
    fi
    chown "$CONSOLE_USER" "$DIR" "$DIR/mapping_consent.json" 2>/dev/null || true
fi
if [ -n "$CONSOLE_UID" ] && [ "$CONSOLE_UID" -ge 500 ]; then
    launchctl bootstrap gui/"$CONSOLE_UID" /Library/LaunchAgents/com.echojay.parammapper.plist 2>/dev/null || true
fi
exit 0
MAPPERPOST
    chmod +x "${PKG_DIR}/mapper-scripts/postinstall"
fi

# --- Post-install script to reset AU cache ---
cat > "${PKG_DIR}/scripts/postinstall" << 'POSTINSTALL'
#!/bin/bash
killall -9 AudioComponentRegistrar 2>/dev/null || true
exit 0
POSTINSTALL
chmod +x "${PKG_DIR}/scripts/postinstall"

# --- Post-install script to reset Pro Tools AAX validation cache ---
# Pro Tools caches plugin validation results per-user; without this, an
# in-place AAX update wont be detected until a manual rescan. This script
# nukes the InstalledAAXPlugIns cache for every real user on the system,
# so Pro Tools rescans (including this plugin) on next launch.
cat > "${PKG_DIR}/aax-scripts/postinstall" << 'AAXPOSTINSTALL'
#!/bin/bash
# Find every real user (UID >= 501 on macOS) and clear their PT cache.
# We use dscl to enumerate users so this works on multi-user studio setups.
USERS=$(dscl . list /Users UniqueID | awk '$2 >= 501 && $2 < 1000 {print $1}')

for u in $USERS; do
    USER_HOME=$(dscl . -read "/Users/$u" NFSHomeDirectory 2>/dev/null | awk '{print $2}')
    if [ -z "$USER_HOME" ] || [ ! -d "$USER_HOME" ]; then
        continue
    fi
    PT_PREFS="$USER_HOME/Library/Preferences/Avid/Pro Tools"
    if [ -d "$PT_PREFS" ]; then
        # Remove validation cache so PT rescans on next launch
        rm -f "$PT_PREFS/InstalledAAXPlugIns" 2>/dev/null || true
        rm -f "$PT_PREFS/InstalledAAXPlugIns.xml" 2>/dev/null || true
    fi
    # Also clear the broader Avid cache directory if present
    AVID_CACHE="$USER_HOME/Library/Caches/Avid"
    if [ -d "$AVID_CACHE" ]; then
        rm -rf "$AVID_CACHE" 2>/dev/null || true
    fi
done

# Touch the plugin bundles so Pro Tools detects them as freshly modified
for PLUGIN_BUNDLE in \
    "/Library/Application Support/Avid/Audio/Plug-Ins/EchoJay V2.aaxplugin" \
    "/Library/Application Support/Avid/Audio/Plug-Ins/EchoJay Link.aaxplugin"; do
    if [ -d "$PLUGIN_BUNDLE" ]; then
        touch "$PLUGIN_BUNDLE"
    fi
done

exit 0
AAXPOSTINSTALL
chmod +x "${PKG_DIR}/aax-scripts/postinstall"

# --- Build component packages ---
echo ""
echo "  Building component packages..."

if [ -n "$VST3_PATH" ]; then
    pkgbuild \
        --root "${PKG_DIR}/vst3_payload" \
        --install-location "/" \
        --identifier "${IDENTIFIER}.vst3" \
        --version "$VERSION" \
        "${PKG_DIR}/components/EchoJayV2-VST3.pkg"
    echo "  Done: V2 VST3 component"
fi

if [ -n "$AU_PATH" ]; then
    pkgbuild \
        --root "${PKG_DIR}/au_payload" \
        --install-location "/" \
        --identifier "${IDENTIFIER}.au" \
        --version "$VERSION" \
        --scripts "${PKG_DIR}/scripts" \
        "${PKG_DIR}/components/EchoJayV2-AU.pkg"
    echo "  Done: V2 AU component"
fi

# AAX installs system-wide (needs admin for /Library/)
if [ -n "$AAX_PATH" ]; then
    pkgbuild \
        --root "${PKG_DIR}/aax_payload" \
        --install-location "/" \
        --identifier "${IDENTIFIER}.aax" \
        --version "$VERSION" \
        --scripts "${PKG_DIR}/aax-scripts" \
        "${PKG_DIR}/components/EchoJayV2-AAX.pkg"
    echo "  Done: V2 AAX component"
fi

if [ -n "$LINK_VST3_PATH" ]; then
    pkgbuild \
        --root "${PKG_DIR}/link_vst3_payload" \
        --install-location "/" \
        --identifier "${LINK_IDENTIFIER}.vst3" \
        --version "$LINK_VERSION" \
        "${PKG_DIR}/components/EchoJayLink-VST3.pkg"
    echo "  Done: Link VST3 component"
fi

if [ -n "$LINK_AU_PATH" ]; then
    pkgbuild \
        --root "${PKG_DIR}/link_au_payload" \
        --install-location "/" \
        --identifier "${LINK_IDENTIFIER}.au" \
        --version "$LINK_VERSION" \
        --scripts "${PKG_DIR}/scripts" \
        "${PKG_DIR}/components/EchoJayLink-AU.pkg"
    echo "  Done: Link AU component"
fi

# Link AAX installs system-wide (needs admin for /Library/)
if [ -n "$LINK_AAX_PATH" ]; then
    pkgbuild \
        --root "${PKG_DIR}/link_aax_payload" \
        --install-location "/" \
        --identifier "${LINK_IDENTIFIER}.aax" \
        --version "$LINK_VERSION" \
        --scripts "${PKG_DIR}/aax-scripts" \
        "${PKG_DIR}/components/EchoJayLink-AAX.pkg"
    echo "  Done: Link AAX component"
fi

if [ -n "$MAPPER_PATH" ]; then
    pkgbuild \
        --root "${PKG_DIR}/mapper_payload" \
        --install-location "/" \
        --identifier "com.echojay.parammapper" \
        --version "$VERSION" \
        --scripts "${PKG_DIR}/mapper-scripts" \
        "${PKG_DIR}/components/EchoJay-ParamMapper.pkg"
    echo "  Done: background mapper component"

    # Sharing consent is a CUSTOM INSTALLER PANE (installer-pane/), a real
    # either/or with Continue blocked until a pick: built, signed, attached
    # below via productbuild --plugins. The pane writes mapping_consent.json
    # directly; the mapper postinstall's fetch-only default remains as the
    # fallback if the pane could not be shown.
    PANE_DIR="${PKG_DIR}/plugins"
    PANE_BUNDLE="${PANE_DIR}/EchoJayConsentPane.bundle"
    mkdir -p "${PANE_BUNDLE}/Contents/MacOS" "${PANE_BUNDLE}/Contents/Resources"
    clang -bundle -fobjc-arc -arch arm64 -arch x86_64 \
        -framework Cocoa -framework InstallerPlugins \
        -o "${PANE_BUNDLE}/Contents/MacOS/EchoJayConsentPane" \
        "${SCRIPT_DIR}/installer-pane/EchoJayConsentPane.m"
    ibtool --compile "${PANE_BUNDLE}/Contents/Resources/EchoJayConsentPane.nib" \
        "${SCRIPT_DIR}/installer-pane/EchoJayConsentPane.xib"
    cp "${SCRIPT_DIR}/installer-pane/Info.plist" "${PANE_BUNDLE}/Contents/"
    if security find-identity -v -p codesigning | grep -q "$APP_SIGN_ID"; then
        codesign --force --options runtime --timestamp --sign "$APP_SIGN_ID" "$PANE_BUNDLE"
        echo "  Signed consent pane bundle"
    else
        echo "  WARNING: consent pane left unsigned"
    fi
    cat > "${PANE_DIR}/InstallerSections.plist" << 'SECTIONS'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>SectionOrder</key>
    <array>
        <string>Introduction</string>
        <string>ReadMe</string>
        <string>License</string>
        <string>Target</string>
        <string>PackageSelection</string>
        <string>EchoJayConsentPane.bundle</string>
        <string>Install</string>
    </array>
</dict>
</plist>
SECTIONS
    echo "  Consent pane staged (plugins dir)"
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
            title=\"EchoJay V2 VST3\"
            description=\"For Ableton, FL Studio, Reaper, Studio One, Cubase, Bitwig, and other VST3 hosts.\"
            selected=\"true\">
        <pkg-ref id=\"${IDENTIFIER}.vst3\"/>
    </choice>"
    PKG_REFS="${PKG_REFS}
    <pkg-ref id=\"${IDENTIFIER}.vst3\" version=\"${VERSION}\">EchoJayV2-VST3.pkg</pkg-ref>"
fi

if [ -n "$AU_PATH" ]; then
    CHOICES_OUTLINE="${CHOICES_OUTLINE}        <line choice=\"au\"/>\n"
    CHOICES="${CHOICES}
    <choice id=\"au\"
            title=\"EchoJay V2 Audio Unit\"
            description=\"For Logic Pro, GarageBand, and other AU-compatible hosts.\"
            selected=\"true\">
        <pkg-ref id=\"${IDENTIFIER}.au\"/>
    </choice>"
    PKG_REFS="${PKG_REFS}
    <pkg-ref id=\"${IDENTIFIER}.au\" version=\"${VERSION}\">EchoJayV2-AU.pkg</pkg-ref>"
fi

if [ -n "$AAX_PATH" ]; then
    CHOICES_OUTLINE="${CHOICES_OUTLINE}        <line choice=\"aax\"/>\n"
    CHOICES="${CHOICES}
    <choice id=\"aax\"
            title=\"EchoJay V2 AAX (Pro Tools)\"
            description=\"For Avid Pro Tools. Requires admin password during installation.\"
            selected=\"true\">
        <pkg-ref id=\"${IDENTIFIER}.aax\"/>
    </choice>"
    PKG_REFS="${PKG_REFS}
    <pkg-ref id=\"${IDENTIFIER}.aax\" version=\"${VERSION}\">EchoJayV2-AAX.pkg</pkg-ref>"
fi

if [ -n "$LINK_VST3_PATH" ]; then
    CHOICES_OUTLINE="${CHOICES_OUTLINE}        <line choice=\"linkvst3\"/>\n"
    CHOICES="${CHOICES}
    <choice id=\"linkvst3\"
            title=\"EchoJay Link VST3\"
            description=\"The per-channel companion plugin, VST3 format.\"
            selected=\"true\">
        <pkg-ref id=\"${LINK_IDENTIFIER}.vst3\"/>
    </choice>"
    PKG_REFS="${PKG_REFS}
    <pkg-ref id=\"${LINK_IDENTIFIER}.vst3\" version=\"${LINK_VERSION}\">EchoJayLink-VST3.pkg</pkg-ref>"
fi

if [ -n "$LINK_AU_PATH" ]; then
    CHOICES_OUTLINE="${CHOICES_OUTLINE}        <line choice=\"linkau\"/>\n"
    CHOICES="${CHOICES}
    <choice id=\"linkau\"
            title=\"EchoJay Link Audio Unit\"
            description=\"The per-channel companion plugin, AU format.\"
            selected=\"true\">
        <pkg-ref id=\"${LINK_IDENTIFIER}.au\"/>
    </choice>"
    PKG_REFS="${PKG_REFS}
    <pkg-ref id=\"${LINK_IDENTIFIER}.au\" version=\"${LINK_VERSION}\">EchoJayLink-AU.pkg</pkg-ref>"
fi

if [ -n "$LINK_AAX_PATH" ]; then
    CHOICES_OUTLINE="${CHOICES_OUTLINE}        <line choice=\"linkaax\"/>\n"
    CHOICES="${CHOICES}
    <choice id=\"linkaax\"
            title=\"EchoJay Link AAX (Pro Tools)\"
            description=\"The per-channel companion plugin for Avid Pro Tools. Requires admin password during installation.\"
            selected=\"true\">
        <pkg-ref id=\"${LINK_IDENTIFIER}.aax\"/>
    </choice>"
    PKG_REFS="${PKG_REFS}
    <pkg-ref id=\"${LINK_IDENTIFIER}.aax\" version=\"${LINK_VERSION}\">EchoJayLink-AAX.pkg</pkg-ref>"
fi

if [ -n "$MAPPER_PATH" ]; then
    CHOICES_OUTLINE="${CHOICES_OUTLINE}        <line choice=\"mapper\"/>\n"
    CHOICES="${CHOICES}
    <choice id=\"mapper\"
            title=\"Set up plugin auto-mapping\"
            description=\"Analyzes the plugins on this computer in a low-priority background process (outside your DAW) and fetches their control maps from EchoJay, so suggested chain settings can be dialed in automatically. Read-only: this setup shares nothing.\"
            selected=\"true\">
        <pkg-ref id=\"com.echojay.parammapper\"/>
    </choice>"
    PKG_REFS="${PKG_REFS}
    <pkg-ref id=\"com.echojay.parammapper\" version=\"${VERSION}\">EchoJay-ParamMapper.pkg</pkg-ref>"
fi

cat > "${PKG_DIR}/distribution.xml" << DISTXML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>EchoJay V2 v${VERSION}</title>
    <organization>${IDENTIFIER}</organization>
    <options customize="always" require-scripts="false" hostArchitectures="x86_64,arm64"/>

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
    --plugins "${PKG_DIR}/plugins" \
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
    echo "  Next: productsign + notarytool + stapler (see RELEASE.md)"
    echo ""
else
    echo "ERROR: Installer build failed."
    exit 1
fi

# Optional: wrap in DMG (unsigned quick-share only; use package-dmg.sh for the
# signed/notarized release DMG)
read -p "  Wrap in a DMG for distribution? (y/n) " WRAP_DMG
if [ "$WRAP_DMG" = "y" ] || [ "$WRAP_DMG" = "Y" ]; then
    DMG_OUTPUT="${SCRIPT_DIR}/EchoJay-V2-v${DISPLAY_VERSION}.dmg"
    DMG_STAGING="/tmp/EchoJayV2_dmg_wrap"
    rm -rf "$DMG_STAGING" "$DMG_OUTPUT"
    mkdir -p "$DMG_STAGING"
    cp "$OUTPUT" "$DMG_STAGING/"

    hdiutil create \
        -volname "EchoJay V2 Installer" \
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
