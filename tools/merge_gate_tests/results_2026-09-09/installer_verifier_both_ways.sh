set -u
HERE="/nonexistent"; DEST="/nonexistent"
WANT_V2="55EFA2E8-383D-3639-8CFF-9C02746569FC"
source /private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad/verify_fns.sh
echo "=== [A] KNOWN-GOOD: Sean's staging ~/Desktop/ejinstall_buildG (must be OK):"
verify "$HOME/Desktop/ejinstall_buildG/EchoJay V2.aaxplugin" "$WANT_V2" "EchoJay V2 (Build G)"; echo "   exit=$?"
echo "=== [B] KNOWN-BAD: a bundle whose x86_64 UUID is 1718F0F1, not 55EFA2E8 (must be MISMATCH, exit 1):"
verify "/tmp/ejbad.vMPP/EchoJay V2.aaxplugin" "$WANT_V2" "EchoJay V2 (Build G)"; echo "   exit=$?"
echo "=== [C] MISSING: a path with no bundle (must be NOT FOUND, exit 2 - NOT a silent mismatch):"
verify "/tmp/does-not-exist/EchoJay V2.aaxplugin" "$WANT_V2" "EchoJay V2 (Build G)"; echo "   exit=$?"
echo "=== [D] locate() finds Sean's staging by name:"
ARG_DIR=""; locate 'EchoJay V2.aaxplugin' && echo "   (located)" || echo "   NOT located"
