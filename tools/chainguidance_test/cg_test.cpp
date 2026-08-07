/*
    Chain-guidance injection self-test + byte dump.

    WHY THIS EXISTS. The conversation-conduct rule ("don't ask which channel;
    don't volunteer capture/meter status") shipped inside the [TARGET CHANNEL]
    declaration only, so it rode Link-targeted turns and nothing else. On the
    main plugin the model did exactly what the absent text forbids: asked
    which channel to work on and reported capture status nobody asked for.
    The fix factors ONE conduct tail behind two identity clauses
    (PluginEditor: chainConductRule / targetChannelDeclaration /
    mainChannelDeclaration). This binary holds that factoring to its claims.

    IT CALLS THE SHIPPING FUNCTIONS, through the friend struct below — the
    exact bytes standardChainInjections appends, not a copy of them. It
    proves: one byte-identical tail on both surfaces; the operative sentences
    present; both blocks opening with a marker EchoJayAPI::sendChat really
    strips from history (read from the ONE list the strip uses); and the
    documented classifier-safety constraint (no EDIT_REQUEST_RE verb
    families, no chain-request verb within 30 chars before "chain"). The
    server's regex source is not in this repo, so the lint encodes the
    RECORDED patterns — a reworded block passing here must still be checked
    against the live server.

    IT ALSO DUMPS the main-plugin declaration block and a representative
    main-plugin chain-turn injection (real feed builder + declaration, in
    compose order) so a human can read what the model receives.
*/

#include "PluginEditor.h"

#include <cstdio>

/** The friend declared in PluginEditor.h. Exists so the declaration builders
    stay private: a test is not a reason to widen a class's public surface. */
struct EchoJayChainGuidanceTestAccess
{
    static bool run() { return EchoJayEditor::runChainGuidanceSelfTest(); }
    static juce::String mainDecl() { return EchoJayEditor::mainChannelDeclaration(); }
    static juce::String targetDecl(const juce::String& phrase)
    {
        return EchoJayEditor::targetChannelDeclaration(phrase);
    }
};

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    const bool ok = EchoJayChainGuidanceTestAccess::run();

    // Byte dump: what a main-plugin chain turn actually carries, in the
    // order standardChainInjections appends it — the validated feed (real
    // shipping builder, sample names) followed by the [THIS CHANNEL]
    // declaration. Read this when reviewing a wording change.
    std::printf("\n===== [THIS CHANNEL] declaration (exact bytes) =====\n%s\n",
                EchoJayChainGuidanceTestAccess::mainDecl().toRawUTF8());
    std::printf("\n===== [TARGET CHANNEL] declaration (exact bytes) =====\n%s\n",
                EchoJayChainGuidanceTestAccess::targetDecl(
                    "the user's \"Lead Vox\" Link channel").toRawUTF8());
    std::printf("\n===== representative main-plugin chain-turn injection "
                "(feed builder + declaration, compose order) =====\n%s%s\n",
                EchoJayAPI::buildChainInjection({ "EchoJay EQ", "EchoJay Compressor" })
                    .toRawUTF8(),
                EchoJayChainGuidanceTestAccess::mainDecl().toRawUTF8());

    return ok ? 0 : 1;
}
