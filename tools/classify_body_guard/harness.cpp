// classify_body_guard — item 5a (26 Sep 2026): /api/classify carries `messages`, the
// SAME turns the writer's body carries, turn for turn.
//
// WHY THIS SHAPE. A guard cannot watch a POST, so both bodies are read from the
// functions that produce the bytes: EchoJayAPI::buildClassifyRequestBody (what
// classify() posts, verbatim) and EchoJayAPI::buildChatRequestBody (what /api/chat
// gets). The comparison is role-and-content over the writer's array with its leading
// SYSTEM message dropped - a system prompt is not a turn, and classify has no use for
// it - and the two arrays must then be equal element for element.
//
// RED BEFORE THE FIX: the classify body had no `messages` at all.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayAPI.h"
#include <cstdio>

struct EchoJayAPIRequestPin
{
    static juce::String chat (EchoJayAPI& a, const juce::StringArray& r, const juce::StringArray& c,
                              const juce::String& sys, const juce::String& mb)
    { return a.buildChatRequestBody (r, c, sys, mb); }
    static juce::String classifyBody (EchoJayAPI& a, const EchoJayAPI::ClassifyRequest& q)
    { return a.buildClassifyRequestBody (q); }
};

// BY VALUE, NOT A POINTER INTO A TEMPORARY. getProperty returns a var by value, so
// returning its getArray() would hand back a pointer into an object already destroyed -
// the rule temp_var_grep_guard enforces on the product, which holds here too.
static juce::var turnsOf (const juce::var& body)
{
    return body.getProperty ("messages", juce::var());
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ej_classify_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);

    int failures = 0;
    auto check = [&] (bool ok, const juce::String& what, const juce::String& detail = {})
    {
        std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                     detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
        if (! ok) ++failures;
    };

    EchoJayAPI api;

    // A real chat for one channel: five turns, the injected blocks where the composer
    // puts them (history turns carry them too, exactly as stored).
    const juce::String feat = "\n\n[ECHOJAY FEATURES v1 - TABS, in order: DASHBOARD...]";
    const juce::String plugins = "\n\n[AVAILABLE PLUGINS - the ONLY plugins: \"Pro-Q 3\"]";
    juce::StringArray roles    { "user", "assistant", "user", "assistant", "user" };
    juce::StringArray contents { "build me a vocal chain" + feat + plugins,
                                 "Quick brief first: what is the material?",
                                 "Lead vocal, pop" + feat,
                                 "Here is a chain: tuner, EQ, compressor.",
                                 "make the compressor harder" + feat + plugins };

    EchoJayAPI::ClassifyRequest req;
    req.message          = contents[4];      // the composed turn, as the composer sends it
    req.channel          = "Lead Vocal";
    req.priorAssistant   = contents[3];
    req.turnType         = "chat";
    req.historyRoles     = roles;            // THE SAME SNAPSHOT the writer gets
    req.historyContents  = contents;

    const auto chatBody     = EchoJayAPIRequestPin::chat (api, roles, contents, "You're EchoJay.", {});
    const auto classifyText = EchoJayAPIRequestPin::classifyBody (api, req);

    const juce::var chatVar     = juce::JSON::parse (chatBody);
    const juce::var classifyVar = juce::JSON::parse (classifyText);
    const juce::var chatTurnsVar = turnsOf (chatVar);      // the roots stay alive for the
    const juce::var clsTurnsVar  = turnsOf (classifyVar);  // pointers taken from them
    auto* chatMsgs = chatTurnsVar.getArray();
    auto* clsMsgs  = clsTurnsVar.getArray();

    check (clsMsgs != nullptr, "the classify body carries messages[]  (RED as it stood: it carried none)",
           clsMsgs == nullptr ? juce::String ("absent") : juce::String (clsMsgs->size()) + " turn(s)");
    check (chatMsgs != nullptr && chatMsgs->size() == roles.size() + 1,
           "the writer's body carries the system message plus every turn",
           juce::String (chatMsgs != nullptr ? chatMsgs->size() : -1));

    if (chatMsgs != nullptr && clsMsgs != nullptr)
    {
        // The writer's element 0 is the system prompt; a system prompt is not a turn.
        check ((*chatMsgs)[0].getProperty ("role", {}).toString() == "system",
               "...with the system prompt first, and it is not a turn");
        check (clsMsgs->size() == chatMsgs->size() - 1,
               "classify carries the same NUMBER of turns as the writer",
               juce::String (clsMsgs->size()) + " vs " + juce::String (chatMsgs->size() - 1));

        int mismatches = 0; juce::String firstBad;
        const int n = juce::jmin (clsMsgs->size(), chatMsgs->size() - 1);
        for (int i = 0; i < n; ++i)
        {
            const auto cr = (*clsMsgs)[i].getProperty ("role", {}).toString();
            const auto wr = (*chatMsgs)[i + 1].getProperty ("role", {}).toString();
            const auto cc = (*clsMsgs)[i].getProperty ("content", {}).toString();
            const auto wc = (*chatMsgs)[i + 1].getProperty ("content", {}).toString();
            if (cr != wr || cc != wc)
            {
                ++mismatches;
                if (firstBad.isEmpty())
                    firstBad = "turn " + juce::String (i) + ": \"" + cr + "\"/" + juce::String (cc.length())
                             + "ch vs \"" + wr + "\"/" + juce::String (wc.length()) + "ch";
            }
        }
        check (mismatches == 0 && n == roles.size(),
               "every turn matches the writer's TURN FOR TURN, role and content",
               mismatches == 0 ? juce::String (n) + " of " + juce::String (roles.size()) + " compared" : firstBad);

        // The newest turn is the one being classified, and it keeps its blocks - the
        // server strips them, the client does not pre-judge what the router may read.
        if (clsMsgs->size() > 0)
        {
            const auto newest = (*clsMsgs)[clsMsgs->size() - 1].getProperty ("content", {}).toString();
            check (newest.contains ("[AVAILABLE PLUGINS") && newest.contains ("[ECHOJAY FEATURES"),
                   "the newest turn's blocks are left in place for the server to strip",
                   juce::String (newest.length()) + "ch");
            check ((*clsMsgs)[clsMsgs->size() - 1].getProperty ("role", {}).toString() == "user",
                   "...and it is the user's turn, which is the one being classified");
        }
    }

    // A client that sends no history omits the field entirely, which is what keeps the
    // server's "(not sent by this client)" fact honest instead of sending an empty array.
    {
        EchoJayAPI::ClassifyRequest bare;
        bare.message = "make it brighter";
        const juce::var body = juce::JSON::parse (EchoJayAPIRequestPin::classifyBody (api, bare));
        check (! turnsOf (body).isArray() && ! body.hasProperty ("messages"),
               "no history from the composer -> no messages field at all (not an empty array)");
        check (body.getProperty ("message", {}).toString() == "make it brighter",
               "...and the body is otherwise unchanged");
    }

    // ---- ISOLATION IS A PRECONDITION FOR EVIDENCE ------------------------------
    // This guard found the violation, so this guard holds the assertion. Dev mode is
    // ON for every process on this Mac (devModeActive() reads an absolute path), so
    // composing a chat body writes the dev-mode dump - and until today it wrote into
    // the LIVE ~/Documents/EchoJay, whose rolling 20-file history it then evicted.
    // Read-only observation of the live path is how "did not touch it" is proved.
    {
        auto live = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                        .getChildFile ("EchoJay").getChildFile ("chat-body-debug.json");
        const auto liveStampBefore = live.getLastModificationTime();
        const int  liveHistBefore  = live.getParentDirectory()
                                         .getNumberOfChildFiles (juce::File::findFiles, "chat-body-2*.json");
        (void) EchoJayAPIRequestPin::chat (api, roles, contents, "You're EchoJay.", {});
        auto isolated = juce::File (juce::String::fromUTF8 (getenv ("ECHOJAY_STATE_HOME") != nullptr
                                                               ? getenv ("ECHOJAY_STATE_HOME") : ""))
                            .getChildFile ("Documents").getChildFile ("EchoJay")
                            .getChildFile ("chat-body-debug.json");
        check (isolated.existsAsFile(),
               "the dev-mode body dump lands inside the isolated root  (RED as it stood: it went to the live "
               "~/Documents/EchoJay and its history evicted real sessions)",
               isolated.getFullPathName());
        check (live.getLastModificationTime() == liveStampBefore
               && live.getParentDirectory().getNumberOfChildFiles (juce::File::findFiles, "chat-body-2*.json") == liveHistBefore,
               "...and the live ~/Documents/EchoJay dump and its rolling history are untouched",
               "history files " + juce::String (liveHistBefore));
    }

    std::printf ("\n==== classify_body_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
