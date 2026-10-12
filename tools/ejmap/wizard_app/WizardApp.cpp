/*
  EchoJay Wizard - the window app (docs/WIZARD_SPEC.md, Kathy 11 Oct; feat/ejmap-wizard). A separate Mac app, nothing to do with the
  plugin: plain window, the EchoJay logo, one step at a time.

    Welcome + sign in -> Scan (a Logic-style bar, each plugin's name as it is checked) -> "N plugins we don't have yet - about X"
    -> Measuring (the plugin's name, time left; the Mac kept awake; closing pauses, reopening carries on) -> Upload -> Reward.
    A wizard that went down with a plugin loaded says so on the next launch, skips that plugin for good and carries on.

  The ENGINE is EJ Map's wizard (`ejmap --wizard`, beside this app's executable, with EchoJayProbe beside it): this app starts it in its
  own process group, reads its progress.json, and stops it with SIGINT (Phase B's clean stop) when the window closes.
  THE SERVER IS STUBBED (spec section 6): sign-in = a local account file; "which do you already have" = the mock database folder handed
  to the engine (--db); upload = the bundle copied into <work>/server_stub/uploads; the promo code = a local ledger, one per account.
  Nothing leaves the Mac.

    "EchoJay Wizard" [--work DIR] [--db DIR] [--store DIR] [--engine PATH] [--jobs N] [--only NAME]... [--snapshots DIR] [--auto]
  --auto (rehearsals only): fills the stub sign-in and presses each step's button after a pause, so a run needs no hands;
  --auto-close-after S: presses the close button S seconds into measuring (the pause path, as a user closing the window).
  --snapshots: every screen (and the bars at each quarter) saved as a PNG of the window's content.
*/
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include "EchoJayLogo.h"
#include "EjmapWizardApp.h"
#include "EjmapWizardProgress.h"
#include "EjmapWizardServer.h"
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

extern char** environ;

namespace
{
using namespace ejmap;
using Screen = wizapp::Screen;

constexpr bool kRewardWithoutNewProfiles = true;   // [Kathy to decide] a code even when nothing new was needed - proposed yes

struct Config
{
    juce::File work, db, store, engine, snapshots, reports;
    int jobs = 2; juce::StringArray only; bool autoMode = false; double autoCloseAfterS = 0.0;
};

juce::File homeChild (const juce::String& p) { return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (p); }

Config parseArgs (const juce::StringArray& a)
{
    Config c;
    c.work = homeChild ("Library/Application Support/EchoJay Wizard");
    c.engine = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("ejmap");
    c.reports = homeChild ("Library/Logs/DiagnosticReports");
    for (int i = 0; i < a.size(); ++i)
    {
        const auto k = a[i]; const auto v = i + 1 < a.size() ? a[i + 1] : juce::String();
        auto file = [&] { ++i; return juce::File::isAbsolutePath (v) ? juce::File (v) : juce::File::getCurrentWorkingDirectory().getChildFile (v); };
        if (k == "--work") c.work = file(); else if (k == "--db") c.db = file(); else if (k == "--store") c.store = file(); else if (k == "--engine") c.engine = file();
        else if (k == "--snapshots") c.snapshots = file(); else if (k == "--reports") c.reports = file();
        else if (k == "--jobs") { c.jobs = juce::jlimit (1, 8, v.getIntValue()); ++i; } else if (k == "--only") { c.only.add (v); ++i; }
        else if (k == "--auto") c.autoMode = true;
        else if (k == "--auto-close-after") { c.autoCloseAfterS = v.getDoubleValue(); ++i; }   // rehearsals: the close button, pressed after N s of measuring
    }
    if (c.db == juce::File()) c.db = c.work.getChildFile ("server_stub/db");             // STUB: the server's "which do you already have"
    if (c.store == juce::File()) c.store = c.work.getChildFile ("store");                // the wizard's own map store (never ~/Library/ejmap)
    return c;
}

// --- THE STUBBED SERVER: EjmapWizardServer.h (the base URL https://www.echojay.ai, both upload paths, the category table) -----------
namespace stub
{
inline juce::var signIn (const juce::File& w, const juce::String& e, const juce::String& p) { return wizserver::stub::signIn (w, e, p); }
inline juce::var upload (const juce::File& w, const juce::File& z, const juce::File& m, const juce::var& a) { return wizserver::stub::upload (w, z, m, a); }
inline juce::var promo (const juce::File& w, const juce::var& a, const juce::var& r) { return wizserver::stub::promo (w, a, r, kRewardWithoutNewProfiles); }
} // namespace stub

// --- THE ENGINE: ejmap --wizard in its own process group --------------------------------------------------------------------------------
struct Engine
{
    pid_t pid = -1; juce::String mode;
    bool start (const juce::File& exe, const juce::StringArray& args, const juce::File& log, const juce::String& m)
    {
        std::vector<std::string> sv { exe.getFullPathName().toStdString() }; for (const auto& a : args) sv.push_back (a.toStdString());
        std::vector<char*> argv; for (auto& s : sv) argv.push_back (s.data()); argv.push_back (nullptr);
        posix_spawnattr_t at; posix_spawnattr_init (&at); posix_spawnattr_setflags (&at, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup (&at, 0);
        posix_spawn_file_actions_t fa; posix_spawn_file_actions_init (&fa);
        const auto lp = log.getFullPathName().toStdString();
        posix_spawn_file_actions_addopen (&fa, 1, lp.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644); posix_spawn_file_actions_adddup2 (&fa, 1, 2);
        const int rc = posix_spawn (&pid, argv[0], &fa, &at, argv.data(), environ);
        posix_spawn_file_actions_destroy (&fa); posix_spawnattr_destroy (&at);
        if (rc != 0) { pid = -1; return false; }
        mode = m; return true;
    }
    bool running() const { return pid > 0; }
    std::optional<int> poll()   // the exit code once it has ended
    {
        if (pid <= 0) return std::nullopt; int st = 0;
        if (waitpid (pid, &st, WNOHANG) == pid) { pid = -1; return WIFEXITED (st) ? WEXITSTATUS (st) : 128 + WTERMSIG (st); }
        return std::nullopt;
    }
    void pause() { if (pid > 0) kill (-pid, SIGINT); }   // Phase B's clean stop: no new plugin, the in-flight ones run again next time
};

// --- KEEP THE MAC AWAKE while the engine runs ----------------------------------------------------------------------------------------
struct KeepAwake
{
    IOPMAssertionID id = 0; bool held = false;
    void hold (bool on)
    {
        if (on && ! held) held = IOPMAssertionCreateWithName (kIOPMAssertPreventUserIdleSystemSleep, kIOPMAssertionLevelOn, CFSTR ("EchoJay Wizard is measuring your plugins"), &id) == kIOReturnSuccess;
        else if (! on && held) { IOPMAssertionRelease (id); held = false; }
    }
    ~KeepAwake() { hold (false); }
};

// --- THE WINDOW'S CONTENT ------------------------------------------------------------------------------------------------------------
class WizardComponent : public juce::Component, private juce::Timer
{
public:
    explicit WizardComponent (Config c) : cfg (std::move (c))
    {
        cfg.work.createDirectory();
        logo = juce::ImageCache::getFromMemory (echoJayLogoPNG, echoJayLogoPNGSize);
        for (auto* b : { &primary, &secondary }) { addChildComponent (*b); b->setColour (juce::TextButton::buttonColourId, accent); b->setColour (juce::TextButton::textColourOffId, juce::Colours::white); }
        secondary.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe9ecf2)); secondary.setColour (juce::TextButton::textColourOffId, ink);
        for (auto* e : { &email, &password })
        { addChildComponent (*e); e->setFont (juce::FontOptions (15.0f)); e->setIndents (10, 10);
          e->setColour (juce::TextEditor::backgroundColourId, juce::Colours::white); e->setColour (juce::TextEditor::textColourId, ink);
          e->setColour (juce::TextEditor::outlineColourId, juce::Colour (0xffc9cfdb)); e->setColour (juce::TextEditor::focusedOutlineColourId, accent);
          e->setColour (juce::CaretComponent::caretColourId, ink); }
        email.setTextToShowWhenEmpty ("Email", juce::Colours::grey); password.setTextToShowWhenEmpty ("Password", juce::Colours::grey); password.setPasswordCharacter ((juce::juce_wchar) 0x2022);
        primary.onClick = [this] { onPrimary(); }; secondary.onClick = [this] { onSecondary(); };
        setSize (640, 460);
        // WHERE TO OPEN: from the files alone (a crash first)
        crashed = wizapp::recover (cfg.work, engineDir().getChildFile ("inflight"), cfg.reports, juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(),
                                   juce::SystemStats::getLogonName(), hostName());
        wizapp::Where w; const auto st = appState();
        w.signedIn = cfg.work.getChildFile ("account.json").existsAsFile(); w.scanned = (bool) st.getProperty ("scanned", false);
        w.measureStarted = (bool) st.getProperty ("measure_started", false); w.measureDone = (bool) st.getProperty ("measure_done", false);
        w.uploaded = (bool) st.getProperty ("uploaded", false); w.rewarded = cfg.work.getChildFile ("reward.json").existsAsFile(); w.crashed = (int) crashed.size();
        logLine ("open: " + wizapp::screenName (wizapp::openOn (w)) + (crashed.empty() ? juce::String() : " (" + juce::String ((int) crashed.size()) + " plugin(s) put on the never-retry list)"));
        show (wizapp::openOn (w));
        startTimer (250);
    }
    ~WizardComponent() override { stopTimer(); awake.hold (false); }

    // closing the window PAUSES: the engine stops cleanly (Phase B's SIGINT), the app quits when it has
    bool requestQuit()
    {
        if (! engine.running()) return true;
        if (! quitting) { quitting = true; engine.pause(); pauseNote = "Pausing - it carries on where it left off next time you open the wizard"; logLine ("close: engine paused (SIGINT)"); repaint(); }
        return false;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::white);
        const auto r = getLocalBounds().reduced (40, 28);
        if (logo.isValid())   // the logo is white on transparent (made for the plugin's dark UI): drawn in ink through its alpha
        { const float w = 210.0f, h = w * (float) logo.getHeight() / (float) logo.getWidth(); g.setColour (ink);
          g.drawImage (logo, juce::Rectangle<float> ((float) getWidth() / 2.0f - w / 2.0f, 30.0f, w, h), juce::RectanglePlacement::centred, true); }
        auto body = r.withTrimmedTop (92);
        g.setColour (ink); g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        g.drawFittedText (title, body.removeFromTop (40), juce::Justification::centred, 2);
        g.setColour (juce::Colour (0xff4a5163)); g.setFont (juce::FontOptions (15.5f));
        g.drawFittedText (text, body.removeFromTop (64).reduced (10, 0), juce::Justification::centredTop, 3);
        if (showBar)
        {
            const auto bar = juce::Rectangle<float> (70.0f, 268.0f, (float) getWidth() - 140.0f, 10.0f);
            g.setColour (juce::Colour (0xffe6e9ef)); g.fillRoundedRectangle (bar, 5.0f);
            g.setColour (accent); g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) juce::jlimit (0.0, 1.0, barValue)), 5.0f);
            g.setColour (juce::Colour (0xff6b7285)); g.setFont (juce::FontOptions (13.5f));
            g.drawText (barLeft, juce::Rectangle<int> (70, 286, getWidth() - 140, 20), juce::Justification::centredLeft);
            g.drawText (barRight, juce::Rectangle<int> (70, 286, getWidth() - 140, 20), juce::Justification::centredRight);
        }
        if (bigCode.isNotEmpty())
        {
            const auto box = juce::Rectangle<float> ((float) getWidth() / 2.0f - 150.0f, 250.0f, 300.0f, 54.0f);
            g.setColour (juce::Colour (0xfff2f5fb)); g.fillRoundedRectangle (box, 10.0f); g.setColour (accent); g.drawRoundedRectangle (box, 10.0f, 1.5f);
            g.setColour (ink); g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 24.0f, juce::Font::bold)); g.drawText (bigCode, box, juce::Justification::centred);
        }
        if (pauseNote.isNotEmpty()) { g.setColour (juce::Colour (0xff6b7285)); g.setFont (juce::FontOptions (13.0f)); g.drawFittedText (pauseNote, juce::Rectangle<int> (40, getHeight() - 92, getWidth() - 80, 20), juce::Justification::centred, 1); }
    }
    void resized() override
    {
        primary.setBounds (getWidth() / 2 - 110, getHeight() - 70, 220, 40);
        secondary.setBounds (getWidth() / 2 - 110, getHeight() - 116, 220, 36);
        email.setBounds (getWidth() / 2 - 160, 250, 320, 38); password.setBounds (getWidth() / 2 - 160, 296, 320, 38);
    }

private:
    Config cfg; Engine engine; KeepAwake awake;
    juce::Image logo; juce::TextButton primary, secondary; juce::TextEditor email, password;
    const juce::Colour accent { 0xff3d5afe }, ink { 0xff141824 };
    Screen screen = Screen::welcome; juce::String title, text, barLeft, barRight, bigCode, pauseNote; bool showBar = false; double barValue = 0.0;
    std::vector<wizapp::Crashed> crashed; bool quitting = false; int autoTicks = 0, snapN = 0; double lastSnapQuarter = -1.0;
    double uploadT0 = 0.0; juce::var account, receipt; const juce::String launchTag = juce::Time::getCurrentTime().formatted ("%H%M%S");

    juce::File engineDir() const { return cfg.work.getChildFile ("engine"); }
    static juce::String hostName() { char h[256] = {}; gethostname (h, sizeof h - 1); return juce::String (h).upToFirstOccurrenceOf (".", false, false); }
    juce::var appState() const { const auto v = juce::JSON::parse (cfg.work.getChildFile ("app_state.json").loadFileAsString()); return v.isObject() ? v : juce::var (new juce::DynamicObject()); }
    void setState (const char* k, bool v) { auto s = appState(); s.getDynamicObject()->setProperty (k, v); cfg.work.getChildFile ("app_state.json").replaceWithText (juce::JSON::toString (s)); }
    void logLine (const juce::String& s) { cfg.work.getChildFile ("app.log").appendText (juce::Time::getCurrentTime().toISO8601 (true) + " " + s + "\n"); }

    juce::StringArray engineArgs (bool measure) const
    {
        juce::StringArray a { "--wizard", "--db", cfg.db.getFullPathName(), "--out", engineDir().getFullPathName(), "--ejmap-ledger", cfg.store.getFullPathName(),
                              "--jobs", juce::String (cfg.jobs), "--skip", cfg.work.getChildFile ("skip.json").getFullPathName() };
        for (const auto& o : cfg.only) { a.add ("--only"); a.add (o); }
        if (measure) { a.add ("--measure"); a.add ("--bundle"); }
        return a;
    }
    void startEngine (bool measure)
    {
        engineDir().createDirectory(); engineDir().getChildFile ("progress.json").deleteFile();   // the last run's "Done 100%" must not flash on the new screen
        if (! engine.start (cfg.engine, engineArgs (measure), cfg.work.getChildFile ("engine_output.txt"), measure ? "measure" : "scan"))
        { title = "Something went wrong"; text = "The measuring engine could not start (" + cfg.engine.getFullPathName() + ")."; repaint(); return; }
        awake.hold (true); logLine ("engine started: " + engine.mode + " (pid " + juce::String (engine.pid) + "), keep-awake held");
    }

    void show (Screen s)
    {
        screen = s; showBar = false; bigCode = {}; pauseNote = {}; primary.setVisible (false); secondary.setVisible (false); email.setVisible (false); password.setVisible (false);
        autoTicks = 0; lastSnapQuarter = -1.0;
        switch (s)
        {
            case Screen::welcome:
                title = "Help EchoJay learn your plugins";
                text = "Sign in with your EchoJay account. What we measure is linked to your account - and so is your reward: a free month.";
                email.setVisible (true); password.setVisible (true); primary.setButtonText ("Sign in"); primary.setVisible (true); break;
            case Screen::scan:
                title = "Checking your plugins"; text = "We're looking at each plugin on this Mac and checking it against what EchoJay already knows.";
                showBar = true; barValue = 0.0; barLeft = "Starting"; barRight = {}; startEngine (false); break;
            case Screen::scanDone:
            {
                const auto ws = juce::JSON::parse (engineDir().getChildFile ("wizard_state.json").loadFileAsString());
                const int n = (int) ws.getProperty ("to_measure", 0); const double eta = (double) ws.getProperty ("to_measure_eta_s", 0.0);
                if (n == 0) { title = "We already know all your plugins"; text = "Nothing new to measure on this Mac. We'll send what we confirmed."; primary.setButtonText ("Continue"); }
                else { title = juce::String (n) + (n == 1 ? " plugin we don't have yet" : " plugins we don't have yet");
                       text = "About " + wizprog::etaWords (eta).replace (" left", "").replace ("about ", "") + ". You can keep working or leave it running - closing the window pauses it.";
                       primary.setButtonText ("Start"); }
                primary.setVisible (true); break;
            }
            case Screen::crashRecovered:
            {
                juce::StringArray names; for (const auto& c : crashed) names.add (c.product);
                title = "Welcome back";
                text = "The wizard closed while checking " + names.joinIntoString (" and ") + " - we've skipped " + (names.size() == 1 ? "it" : "them") + " and carried on.";
                primary.setButtonText ("Carry on"); primary.setVisible (true); break;
            }
            case Screen::measuring:
                title = "Measuring"; text = "Each plugin takes a few minutes. Leave it running - the Mac stays awake. Closing the window pauses it.";
                showBar = true; barValue = 0.0; barLeft = "Getting ready"; barRight = {}; setState ("measure_started", true); startEngine (true); break;
            case Screen::upload:
                title = "Sending your results"; text = "Just the measurements - no audio, no plugin files, nothing personal.";
                showBar = true; barValue = 0.0; barLeft = "Sending"; barRight = {}; uploadT0 = juce::Time::getMillisecondCounterHiRes(); break;
            case Screen::reward:
            {
                const auto rw = juce::JSON::parse (cfg.work.getChildFile ("reward.json").loadFileAsString());
                title = "Thank you - here's your free month"; bigCode = rw.getProperty ("code", "").toString();
                text = "Use this code in EchoJay. We've emailed it to you too (" + rw.getProperty ("emailed_to", "").toString() + ").";
                {   // nothing dropped (12 Oct): say what went for review instead of being measured
                    const auto m = juce::JSON::parse (engineDir().getChildFile ("contribution/manifest.json").loadFileAsString());
                    const int nc = m.getProperty ("needs_category", {}).size();
                    if (nc > 0) text << " " << nc << (nc == 1 ? " plugin we couldn't identify was" : " plugins we couldn't identify were") << " sent for review.";
                }
                secondary.setButtonText ("Copy code"); secondary.setVisible (true); primary.setButtonText ("Done"); primary.setVisible (true); break;
            }
        }
        logLine ("screen: " + wizapp::screenName (s));
        resized(); repaint();
        juce::Timer::callAfterDelay (400, [safe = juce::Component::SafePointer<WizardComponent> (this), s] { if (safe != nullptr && safe->screen == s) safe->snap ("screen"); });
    }

    void onPrimary()
    {
        switch (screen)
        {
            case Screen::welcome:
            {
                account = stub::signIn (cfg.work, email.getText().trim(), password.getText());
                if (account.isVoid()) { text = "Please enter your EchoJay email and password."; repaint(); return; }
                logLine ("signed in (STUB): " + account.getProperty ("account_id", "").toString()); show (Screen::scan); break;
            }
            case Screen::scanDone:
            {
                const auto ws = juce::JSON::parse (engineDir().getChildFile ("wizard_state.json").loadFileAsString());
                if ((int) ws.getProperty ("to_measure", 0) == 0) { setState ("measure_started", true); setState ("measure_done", true); startBundleOnly(); }
                else show (Screen::measuring);
                break;
            }
            case Screen::crashRecovered:
            {
                crashed.clear(); const auto st = appState();
                show ((bool) st.getProperty ("measure_started", false) ? Screen::measuring : Screen::scan); break;
            }
            case Screen::reward: juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
            default: break;
        }
    }
    void onSecondary() { if (screen == Screen::reward) { juce::SystemClipboard::copyTextToClipboard (bigCode); secondary.setButtonText ("Copied"); logLine ("code copied"); } }
    void startBundleOnly() { show (Screen::measuring); }   // nothing to measure: the engine still makes the bundle (confirmations)

    void snap (const juce::String& why)
    {
        if (cfg.snapshots == juce::File()) return;
        cfg.snapshots.createDirectory();
        const auto img = createComponentSnapshot (getLocalBounds(), true, 2.0f);
        const auto f = cfg.snapshots.getChildFile (launchTag + "_" + juce::String (++snapN).paddedLeft ('0', 2) + "_" + wizapp::screenName (screen) + "_" + why + ".png");   // launch time first: launches never share a name
        juce::FileOutputStream os (f); if (os.openedOk()) { os.setPosition (0); os.truncate(); juce::PNGImageFormat().writeImageToStream (img, os); }
    }
    void snapAtQuarters()
    {
        const double q = std::floor (barValue * 4.0) / 4.0;
        if (q > lastSnapQuarter && q > 0.0) { lastSnapQuarter = q; snap (juce::String ((int) (q * 100.0)) + "pct"); }
    }

    void timerCallback() override
    {
        if (screen == Screen::scan || screen == Screen::measuring) pollEngine();
        if (screen == Screen::upload) tickUpload();
        if (cfg.autoMode && ! quitting) autoAdvance();
    }
    void pollEngine()
    {
        const auto p = juce::JSON::parse (engineDir().getChildFile ("progress.json").loadFileAsString());
        const auto stage = p.getProperty ("stage", "").toString(); const double pct = (double) p.getProperty ("percent", 0.0);
        juce::String cur; if (const auto* a = p.getProperty ("current", {}).getArray()) if (! a->isEmpty()) cur = (*a)[0].toString();
        if (screen == Screen::scan)
        {   // the Logic-style bar: the scan is the engine's Scanning + Checking (0..15 of its bar)
            barValue = juce::jlimit (0.0, 1.0, pct / 15.0); barLeft = cur.isNotEmpty() ? cur : stage == "Checking" ? "Checking" : "Looking for plugins";
            const auto sc = p.getProperty ("stage_counts", {}); barRight = sc.isObject() ? juce::String ((int) sc.getProperty ("done", 0)) + " of " + juce::String ((int) sc.getProperty ("total", 0)) : juce::String();
        }
        else
        {   // measuring: the engine's Measuring band (15..97) is the bar; its rescan is "getting ready"
            if (stage == "Measuring") { barValue = juce::jlimit (0.0, 1.0, (pct - 15.0) / 82.0); barLeft = cur.isNotEmpty() ? cur : "Measuring"; barRight = p.getProperty ("eta_words", "").toString(); }
            else if (stage == "Packing" || stage == "Done") { barValue = 1.0; barLeft = "Packing your results"; barRight = {}; }
            else { barValue = 0.0; barLeft = "Getting ready"; barRight = {}; }
        }
        repaint(); snapAtQuarters();
        if (const auto code = engine.poll())
        {
            awake.hold (false); logLine ("engine ended: " + juce::String (*code) + ", keep-awake released");
            if (quitting) { juce::JUCEApplication::getInstance()->quit(); return; }
            if (screen == Screen::scan) { setState ("scanned", true); show (Screen::scanDone); }
            else if (*code == 0) { setState ("measure_done", true); show (Screen::upload); }
            else { title = "Paused"; text = "It stopped before the end. Open the wizard again to carry on."; showBar = false; repaint(); }
        }
    }
    void tickUpload()
    {   // a short bar over the (stubbed) send, then the server's genuine check and the promo
        const double t = (juce::Time::getMillisecondCounterHiRes() - uploadT0) / 2500.0;
        barValue = juce::jlimit (0.0, 1.0, t); barLeft = barValue < 1.0 ? "Sending" : "Checking it arrived"; repaint(); snapAtQuarters();
        if (t < 1.3 || receipt.isObject()) return;
        if (account.isVoid()) account = juce::JSON::parse (cfg.work.getChildFile ("account.json").loadFileAsString());
        receipt = stub::upload (cfg.work, engineDir().getChildFile ("contribution.zip"), engineDir().getChildFile ("contribution/manifest.json"), account);
        setState ("uploaded", true); logLine ("uploaded (STUB): " + juce::JSON::toString (receipt, true));
        const auto pr = stub::promo (cfg.work, account, receipt);
        if (pr.isObject()) { cfg.work.getChildFile ("reward.json").replaceWithText (juce::JSON::toString (pr)); logLine ("promo (STUB): " + juce::JSON::toString (pr, true)); show (Screen::reward); }
        else { title = "Thank you"; text = "Your results arrived. (No code this time.)"; showBar = false; repaint(); }
    }
    void autoAdvance()
    {   // rehearsals only: no hands
        ++autoTicks;
        if (screen == Screen::welcome && autoTicks == 6) { email.setText ("rehearsal@example.com"); password.setText ("stub-password"); repaint(); snap ("filled"); }
        if (screen == Screen::welcome && autoTicks == 10) onPrimary();
        if ((screen == Screen::scanDone || screen == Screen::crashRecovered) && autoTicks == 12) onPrimary();
        if (screen == Screen::reward && autoTicks == 8) { onSecondary(); snap ("copied"); }
        if (screen == Screen::measuring && cfg.autoCloseAfterS > 0.0 && autoTicks * 0.25 >= cfg.autoCloseAfterS)
        { cfg.autoCloseAfterS = 0.0; logLine ("rehearsal: the close button"); juce::JUCEApplication::getInstance()->systemRequestedQuit(); snap ("closing"); }
    }
};

class WizardWindow : public juce::DocumentWindow
{
public:
    explicit WizardWindow (Config c) : juce::DocumentWindow ("EchoJay Wizard", juce::Colours::white, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true); content = new WizardComponent (std::move (c)); setContentOwned (content, true);
        setResizable (false, false); centreWithSize (getWidth(), getHeight()); setVisible (true);
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    WizardComponent* content = nullptr;
};

class WizardApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "EchoJay Wizard"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise (const juce::String&) override { window = std::make_unique<WizardWindow> (parseArgs (getCommandLineParameterArray())); }
    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { if (window == nullptr || window->content == nullptr || window->content->requestQuit()) quit(); }
private:
    std::unique_ptr<WizardWindow> window;
};
} // namespace

START_JUCE_APPLICATION (WizardApplication)
