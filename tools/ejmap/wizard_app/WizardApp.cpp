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
  --auto-finish-at N: presses Finish now once N plugins are measured so far (13 Oct: a finish part-way, with one in flight).
  --snapshots: every screen (and the bars at each quarter) saved as a PNG of the window's content.
*/
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include "EchoJayLogo.h"
#include "EjmapWizardApp.h"
#include "EjmapWizardProgress.h"
#include "EjmapWizardServer.h"
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/IOKitLib.h>
#include <spawn.h>
#include <signal.h>
#include <atomic>
#include <csignal>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

extern char** environ;

namespace
{
using namespace ejmap;
using Screen = wizapp::Screen;

// THE BUILD'S MODE (Kathy, 13 Oct, beta item 4): the beta build (target ejmap-wizard-beta, EJ_WIZARD_BETA) saves the results as one file
// to email instead of uploading; no sign-in, no promo. The normal build keeps the stubbed server.
#if EJ_WIZARD_BETA
constexpr auto kMode = wizapp::Mode::beta;
#else
constexpr auto kMode = wizapp::Mode::normal;
#endif
#ifndef EJ_WIZARD_BETA_EMAIL
#define EJ_WIZARD_BETA_EMAIL "admin@signaturestudiosonline.com"   // Kathy, 13 Oct
#endif

struct Config
{
    juce::File work, db, store, engine, snapshots, reports;
    int jobs = 2; juce::StringArray only; bool autoMode = false; double autoCloseAfterS = 0.0; int autoFinishAt = 0;
};

juce::File homeChild (const juce::String& p) { return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (p); }

Config parseArgs (const juce::StringArray& a)
{
    Config c;
    c.work = homeChild ("Library/Application Support/" + wizapp::workFolderName (kMode));   // 13 Oct: beta and release apart
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
        else if (k == "--auto-finish-at") { c.autoFinishAt = v.getIntValue(); ++i; }   // rehearsals: Finish now once N are measured so far
        else if (k == "--auto-close-after") { c.autoCloseAfterS = v.getDoubleValue(); ++i; }   // rehearsals: the close button, pressed after N s of measuring
    }
    const auto app = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    if (c.db == juce::File()) c.db = wizapp::databaseFolder (kMode, app, c.work);       // beta: the bundled snapshot (read-only); release: the STUB
    if (c.store == juce::File()) c.store = c.work.getChildFile ("store");                // the wizard's own map store (never ~/Library/ejmap)
    if (! c.db.isAChildOf (app))
    {   // 13 Oct: the mock DB's 290 agreed references, from the app's Resources (assemble.sh), into the STUB db only (never into the bundle)
        const auto res = juce::File::getSpecialLocation (juce::File::currentApplicationFile).getChildFile ("Contents/Resources/mock_db/references");
        wizserver::stub::seedMockReferences (res, c.db);
    }
    return c;
}

// --- THE STUBBED SERVER: EjmapWizardServer.h (the base URL https://www.echojay.ai, both upload paths, the category table) -----------
namespace stub
{
inline juce::var signIn (const juce::File& w, const juce::String& e, const juce::String& p) { return wizserver::stub::signIn (w, e, p); }
inline juce::var upload (const juce::File& w, const juce::File& z, const juce::File& m, const juce::var& a, const juce::String& dev) { return wizserver::stub::upload (w, z, m, a, dev); }
inline juce::var promo (const juce::File& w, const juce::var& a, const juce::var& r) { return wizserver::stub::promo (w, a, r); }
inline juce::var lookup (const juce::File& w, const juce::var& a, const juce::String& dev, const juce::var& state) { return wizserver::stub::lookupEligibility (w, a.getProperty ("account_id", "").toString(), dev, state); }
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

// --- THE MAC'S DEVICE HASH (items 7 / 8): the hardware UUID (IOPlatformUUID), salted and hashed (wizserver::deviceHashOf) - the raw UUID
// never leaves the Mac. EJ_WIZARD_FAKE_DEVICE stands in for it in rehearsals (a "second Mac").
inline juce::String platformUuid()
{
    if (const char* f = std::getenv ("EJ_WIZARD_FAKE_DEVICE"); f != nullptr && *f) return f;
    juce::String out; io_service_t svc = IOServiceGetMatchingService (kIOMainPortDefault, IOServiceMatching ("IOPlatformExpertDevice"));
    if (svc) { if (CFTypeRef u = IORegistryEntryCreateCFProperty (svc, CFSTR ("IOPlatformUUID"), kCFAllocatorDefault, 0)) { char b[128] = {}; if (CFStringGetCString ((CFStringRef) u, b, sizeof b, kCFStringEncodingUTF8)) out = b; CFRelease (u); } IOObjectRelease (svc); }
    return out;
}

// --- CTRL-C (Kathy, 13 Oct): the app launched from a Terminal - SIGINT takes the close button's path (the engine is in its own group)
inline std::atomic<int>& appInterrupted() { static std::atomic<int> f { 0 }; return f; }

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
        addChildComponent (agree); agree.setButtonText (wizapp::consentTick()); agree.setColour (juce::ToggleButton::textColourId, ink); agree.setColour (juce::ToggleButton::tickColourId, accent);
        agree.setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (0xff8a90a0)); agree.onClick = [this] { primary.setEnabled (agree.getToggleState()); };
        device = wizserver::deviceHashOf (platformUuid());
        setSize (640, 460);
        // WHERE TO OPEN: from the files alone (a crash first)
        crashed = wizapp::recover (cfg.work, engineDir().getChildFile ("inflight"), cfg.reports, juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(),
                                   juce::SystemStats::getLogonName(), hostName());
        wizapp::Where w; const auto st = appState();
        w.signedIn = cfg.work.getChildFile ("account.json").existsAsFile(); w.scanned = (bool) st.getProperty ("scanned", false);
        w.measureStarted = (bool) st.getProperty ("measure_started", false); w.measureDone = (bool) st.getProperty ("measure_done", false);
        w.uploaded = (bool) st.getProperty ("uploaded", false); w.rewarded = cfg.work.getChildFile ("reward.json").existsAsFile(); w.crashed = (int) crashed.size();
        w.consented = (bool) st.getProperty ("consented", false); w.saved = (bool) st.getProperty ("saved", false);
        logLine ("open (" + juce::String (kMode == wizapp::Mode::beta ? "beta" : "normal") + "): " + wizapp::screenName (wizapp::openOn (w, kMode)) + (crashed.empty() ? juce::String() : " (" + juce::String ((int) crashed.size()) + " plugin(s) put on the never-retry list)"));
        show (wizapp::openOn (w, kMode));
        startTimer (250);
    }
    ~WizardComponent() override { stopTimer(); awake.hold (false); }

    // closing the window PAUSES: the engine stops cleanly (Phase B's SIGINT), the app quits when it has
    bool requestQuit (bool askFirst = true)
    {
        if (! engine.running()) return true;
        if (askFirst && ! quitting && ! finishing && screen == Screen::measuring && closeAsk == nullptr && ! cfg.autoMode)
        {   // FINISH NOW (Kathy, 13 Oct): closing while measuring asks - finish and save what's done, or pause and carry on later
            const auto b = wizapp::closeChoiceButtons();
            closeAsk = std::make_unique<juce::AlertWindow> ("Stop measuring?", soFar + ". Finish now and save what's done, or pause and carry on later.", juce::MessageBoxIconType::NoIcon, this);
            closeAsk->addButton (b[0], 1); closeAsk->addButton (b[1], 2); closeAsk->addButton ("Keep measuring", 0);
            closeAsk->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<WizardComponent> (this)] (int r)
            { if (safe == nullptr) return; safe->closeAsk.reset();
              if (r == 1) safe->finishNow ("the close button: finish");
              else if (r == 2) { safe->logLine ("close: pause chosen"); if (safe->requestQuit (false)) juce::JUCEApplication::getInstance()->quit(); } }));
            return false;
        }
        if (! quitting) { quitting = true; engine.pause(); pauseNote = wizapp::stoppedNote(); logLine ("close: engine stopped (SIGINT) on " + wizapp::screenName (screen)); repaint(); }
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
        g.drawFittedText (text, body.removeFromTop (screen == Screen::consent ? 150 : 64).reduced (10, 0), juce::Justification::centredTop, screen == Screen::consent ? 9 : 3);
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
        if (screen == Screen::measuring && soFar.isNotEmpty()) { g.setColour (ink); g.setFont (juce::FontOptions (15.0f, juce::Font::bold)); g.drawText (soFar, juce::Rectangle<int> (70, 216, getWidth() - 140, 24), juce::Justification::centred); }
        if (pauseNote.isNotEmpty()) { g.setColour (juce::Colour (0xff6b7285)); g.setFont (juce::FontOptions (13.0f)); g.drawFittedText (pauseNote, juce::Rectangle<int> (40, getHeight() - 92, getWidth() - 80, 20), juce::Justification::centred, 1); }
    }
    void resized() override
    {
        primary.setBounds (getWidth() / 2 - 110, getHeight() - 70, 220, 40);
        secondary.setBounds (getWidth() / 2 - 110, getHeight() - 116, 220, 36);
        email.setBounds (getWidth() / 2 - 160, 250, 320, 38); password.setBounds (getWidth() / 2 - 160, 296, 320, 38);
        agree.setBounds (getWidth() / 2 - 60, getHeight() - 118, 160, 30);
    }

private:
    Config cfg; Engine engine; KeepAwake awake;
    juce::Image logo; juce::TextButton primary, secondary; juce::TextEditor email, password; juce::ToggleButton agree; bool diskRefused = false; juce::String device; juce::File savedFile; juce::var eligibility;
    const juce::Colour accent { 0xff3d5afe }, ink { 0xff141824 };
    Screen screen = Screen::welcome; juce::String title, text, barLeft, barRight, bigCode, pauseNote; bool showBar = false; double barValue = 0.0;
    std::vector<wizapp::Crashed> crashed; bool quitting = false, finishing = false; juce::String soFar; std::unique_ptr<juce::AlertWindow> closeAsk; int autoTicks = 0, snapN = 0; double lastSnapQuarter = -1.0;
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
        if (const auto dv = wizapp::diskVerdict (wizapp::freeBytes (cfg.work), true); ! dv.ok)   // 13 Oct: never start below the floor
        { const auto c = wizapp::diskRefusedCopy (dv.words); title = c.title; text = c.text; showBar = false; primary.setButtonText (c.primary); primary.setVisible (true); diskRefused = true;
          logLine ("refused to start: " + dv.words); resized(); repaint(); return; }
        engineDir().createDirectory(); engineDir().getChildFile ("progress.json").deleteFile();   // the last run's "Done 100%" must not flash on the new screen
        if (! engine.start (cfg.engine, engineArgs (measure), cfg.work.getChildFile ("engine_output.txt"), measure ? "measure" : "scan"))
        { title = "Something went wrong"; text = "The measuring engine could not start (" + cfg.engine.getFullPathName() + ")."; repaint(); return; }
        awake.hold (true); logLine ("engine started: " + engine.mode + " (pid " + juce::String (engine.pid) + "), keep-awake held");
    }

    void show (Screen s)
    {
        screen = s; showBar = false; bigCode = {}; pauseNote = {}; primary.setVisible (false); secondary.setVisible (false); email.setVisible (false); password.setVisible (false);
        agree.setVisible (false); primary.setEnabled (true);
        autoTicks = 0; lastSnapQuarter = -1.0;
        auto copy = [&] (const wizapp::Copy& c) { title = c.title; text = c.text; primary.setButtonText (c.primary); primary.setVisible (c.primary.isNotEmpty());
                                                  secondary.setButtonText (c.secondary); secondary.setVisible (c.secondary.isNotEmpty()); };
        switch (s)
        {
            case Screen::welcome:
                title = "Help EchoJay learn your plugins";
                if (kMode == wizapp::Mode::beta) { text = "Thank you for testing the EchoJay Wizard. It measures the plugins on this Mac that EchoJay doesn't know yet."; primary.setButtonText ("Continue"); primary.setVisible (true); break; }
                text = "Sign in with your EchoJay account. What we measure is linked to your account - and so is your reward: a free month.";
                email.setVisible (true); password.setVisible (true); primary.setButtonText ("Sign in"); primary.setVisible (true); break;
            case Screen::consent:
                title = "Before we start"; text = wizapp::consentText (kMode);
                agree.setToggleState (false, juce::dontSendNotification); agree.setVisible (true); primary.setButtonText ("Continue"); primary.setVisible (true); primary.setEnabled (false); break;
            case Screen::scan:
                title = "Checking your plugins"; text = "We're looking at each plugin on this Mac and checking it against what EchoJay already knows.";
                showBar = true; barValue = 0.0; barLeft = "Starting"; barRight = {}; startEngine (false); break;
            case Screen::alreadyDone: copy (wizapp::alreadyDoneCopy()); break;
            case Screen::scanDone:
            {
                const auto ws = juce::JSON::parse (engineDir().getChildFile ("wizard_state.json").loadFileAsString());
                const double eta = (double) ws.getProperty ("to_measure_eta_s", 0.0);   // the per-category estimate (measured seconds once any exist, --jobs aware)
                const auto el = wizapp::eligibilityOf (juce::JSON::parse (cfg.work.getChildFile ("eligibility.json").loadFileAsString()));
                copy (wizapp::scanDoneCopy (kMode, el, wizapp::scanDoneTimeLine ((int) ws.getProperty ("to_measure", el.count), eta, wizapp::etaPhrase (eta))));
                break;
            }
            case Screen::crashRecovered:
            {
                juce::StringArray names; for (const auto& c : crashed) names.add (c.product);
                title = "Welcome back";
                text = "The wizard closed while checking " + names.joinIntoString (" and ") + " - we've skipped " + (names.size() == 1 ? "it" : "them") + " and carried on.";
                primary.setButtonText ("Carry on"); primary.setVisible (true); break;
            }
            case Screen::measuring:
                title = "Measuring"; text = "Each plugin takes a few minutes. Leave it running - the Mac stays awake. Press Finish now whenever you like: what's measured so far is kept.";
                showBar = true; barValue = 0.0; barLeft = "Getting ready"; barRight = {}; setState ("measure_started", true); finishing = false;
                secondary.setButtonText ("Finish now"); secondary.setVisible (true); secondary.setEnabled (false); soFar = wizapp::measuredSoFarWords (0);
                startEngine (true); break;
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
            case Screen::noCode:
            {
                const auto rc = juce::JSON::parse (cfg.work.getChildFile ("receipt.json").loadFileAsString());
                title = "Thank you - your results arrived"; text = rc.getProperty ("reward_message", "").toString();
                primary.setButtonText ("Done"); primary.setVisible (true); break;
            }
            case Screen::betaSaved:
            {
                savedFile = juce::File (appState().getProperty ("saved_file", "").toString());
                title = "Thank you - your results are ready";
                text = "Saved as " + savedFile.getFileName() + " on your Desktop. Email this file to " + juce::String (EJ_WIZARD_BETA_EMAIL) + ".";
                secondary.setButtonText ("Show in Finder"); secondary.setVisible (true); primary.setButtonText ("Done"); primary.setVisible (true); break;
            }
        }
        logLine ("screen: " + wizapp::screenName (s));
        resized(); repaint();
        juce::Timer::callAfterDelay (400, [safe = juce::Component::SafePointer<WizardComponent> (this), s] { if (safe != nullptr && safe->screen == s) safe->snap ("screen"); });
    }

    void onPrimary()
    {
        if (diskRefused) { logLine ("quit (not enough disk space)"); juce::JUCEApplication::getInstance()->systemRequestedQuit(); return; }
        switch (screen)
        {
            case Screen::welcome:
            {
                if (kMode == wizapp::Mode::beta) { show (Screen::consent); break; }
                account = stub::signIn (cfg.work, email.getText().trim(), password.getText());
                if (account.isVoid()) { text = "Please enter your EchoJay email and password."; repaint(); return; }
                logLine ("signed in (STUB): " + account.getProperty ("account_id", "").toString()); show (Screen::consent); break;
            }
            case Screen::consent:
                if (! agree.getToggleState()) return;   // the tick is required
                setState ("consented", true); logLine ("consent: I agree (" + juce::Time::getCurrentTime().toISO8601 (true) + ")");
                {   // /lookup before anything loads: an account or a Mac that has already taken part runs nothing (STUB)
                    if (account.isVoid()) account = juce::JSON::parse (cfg.work.getChildFile ("account.json").loadFileAsString());
                    const auto early = stub::lookup (cfg.work, account, device, juce::var());
                    logLine ("lookup before the scan (STUB): already_contributed " + juce::String ((int) (bool) early.getProperty ("already_contributed", false)));
                    show (wizapp::afterConsent (wizapp::eligibilityOf (early)));
                }
                break;
            case Screen::scanDone:
            {
                const auto el = wizapp::eligibilityOf (juce::JSON::parse (cfg.work.getChildFile ("eligibility.json").loadFileAsString()));
                if (el.count == 0 || (kMode == wizapp::Mode::normal && ! el.eligible)) { logLine ("not eligible / nothing to measure: done, nothing runs"); juce::JUCEApplication::getInstance()->systemRequestedQuit(); break; }
                show (Screen::measuring); break;
            }
            case Screen::crashRecovered:
            {
                crashed.clear(); const auto st = appState();
                show ((bool) st.getProperty ("measure_started", false) ? Screen::measuring : Screen::scan); break;
            }
            case Screen::alreadyDone: case Screen::reward: case Screen::noCode: case Screen::betaSaved: juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
            default: break;
        }
    }
    void onSecondary()
    {
        if (screen == Screen::reward) { juce::SystemClipboard::copyTextToClipboard (bigCode); secondary.setButtonText ("Copied"); logLine ("code copied"); }
        if (screen == Screen::betaSaved && savedFile.existsAsFile()) { savedFile.revealToUser(); logLine ("shown in Finder: " + savedFile.getFileName()); }
        if (screen == Screen::scanDone) { logLine ("contribute anyway (no reward)"); show (Screen::measuring); }
        if (screen == Screen::measuring && secondary.isEnabled()) finishNow ("the Finish now button");
    }
    void finishNow (const juce::String& from)
    {   // FINISH NOW (Kathy, 13 Oct): the request file, then the pause's clean stop - the engine packs what landed and ends 0
        if (finishing || ! engine.running()) return;
        if (! secondary.isEnabled() && from != "rehearsal")   // the close question before measuring began: nothing to pack - a pause
        { logLine ("finish asked before measuring began: paused instead"); if (requestQuit (false)) juce::JUCEApplication::getInstance()->quit(); return; }
        finishing = true; engineDir().getChildFile (wizapp::kFinishRequestFile).replaceWithText ("finish\n"); engine.pause();
        secondary.setEnabled (false); secondary.setButtonText ("Finishing..."); barLeft = "Finishing - saving what's measured"; barRight = {};
        logLine ("finish now (" + from + "): " + soFar + "; engine stopped (SIGINT) to pack"); repaint();
    }
    void saveBetaResults()
    {   // the contribution bundle as ONE file on the Desktop (never overwritten: -2, -3 ...)
        const char* rd = std::getenv ("EJ_WIZARD_RESULTS_DIR");   // rehearsals only: never the tester's Desktop
        const auto desk = rd != nullptr && *rd ? juce::File (rd) : juce::File::getSpecialLocation (juce::File::userDesktopDirectory); desk.createDirectory();
        auto f = desk.getChildFile (wizapp::resultsFileName (juce::Time::getCurrentTime()));
        for (int k = 2; f.existsAsFile(); ++k) f = desk.getChildFile (f.getFileNameWithoutExtension().upToLastOccurrenceOf ("-results-", true, false) + juce::Time::getCurrentTime().formatted ("%Y-%m-%d") + "-" + juce::String (k) + ".zip");
        if (engineDir().getChildFile ("contribution.zip").copyFileTo (f)) { auto st = appState(); st.getDynamicObject()->setProperty ("saved_file", f.getFullPathName()); cfg.work.getChildFile ("app_state.json").replaceWithText (juce::JSON::toString (st)); setState ("saved", true); logLine ("saved: " + f.getFullPathName()); }
        else logLine ("could not save the results file");
    }

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
        if (appInterrupted() != 0 && ! quitting) { appInterrupted() = 0; logLine ("Ctrl-C: a pause (no question asked)"); if (requestQuit (false)) juce::JUCEApplication::getInstance()->quit(); }
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
            barValue = wizapp::scanBarValue (p); barLeft = cur.isNotEmpty() ? cur : stage == "Checking" ? "Checking" : "Looking for plugins";
            const auto sc = p.getProperty ("stage_counts", {}); barRight = sc.isObject() ? juce::String ((int) sc.getProperty ("done", 0)) + " of " + juce::String ((int) sc.getProperty ("total", 0)) : juce::String();
        }
        else
        {   // measuring: the engine's Measuring band (15..97) is the bar; its rescan is "getting ready"
            if (! finishing) { soFar = wizapp::measuredSoFarWords (wizapp::measuredSoFar (p)); secondary.setEnabled (wizapp::finishAvailable (p)); }
            if (finishing && stage != "Packing" && stage != "Done") { barLeft = "Finishing - saving what's measured"; barRight = {}; }
            else if (stage == "Measuring") { barValue = juce::jlimit (0.0, 1.0, (pct - 15.0) / 82.0); barLeft = cur.isNotEmpty() ? cur : "Measuring"; barRight = p.getProperty ("eta_words", "").toString(); }
            else if (stage == "Packing" || stage == "Done") { barValue = 1.0; barLeft = "Packing your results"; barRight = {}; }
            else { barValue = 0.0; barLeft = "Getting ready"; barRight = {}; }
        }
        repaint(); snapAtQuarters();
        if (const auto code = engine.poll())
        {
            awake.hold (false); logLine ("engine ended: " + juce::String (*code) + ", keep-awake released");
            if (quitting) { juce::JUCEApplication::getInstance()->quit(); return; }
            switch (wizapp::afterEngine (screen, *code))
            {
                case wizapp::AfterEngine::scanDone:
                {   // /lookup's answer (STUB): already taken part -> nothing runs; else eligibility for the results screen
                    setState ("scanned", true);
                    if (account.isVoid()) account = juce::JSON::parse (cfg.work.getChildFile ("account.json").loadFileAsString());
                    eligibility = stub::lookup (cfg.work, account, device, juce::JSON::parse (engineDir().getChildFile ("wizard_state.json").loadFileAsString()));
                    cfg.work.getChildFile ("eligibility.json").replaceWithText (juce::JSON::toString (eligibility)); logLine ("lookup (STUB): " + juce::JSON::toString (eligibility, true));
                    show (wizapp::afterScan (wizapp::eligibilityOf (eligibility))); break;
                }
                case wizapp::AfterEngine::upload:
                    setState ("measure_done", true);
                    if (wizapp::afterMeasure (kMode) == Screen::betaSaved) saveBetaResults();
                    show (wizapp::afterMeasure (kMode)); break;
                case wizapp::AfterEngine::stopped:
                {   // the engine's own words when it said why (a low disk); else the stop note
                    const auto sum = juce::JSON::parse (engineDir().getChildFile ("progress.json").loadFileAsString()).getProperty ("summary", "").toString();
                    title = "Stopped"; text = sum.containsIgnoreCase ("disk space") ? sum : wizapp::stoppedNote() + " - open the wizard again to carry on."; showBar = false; repaint(); break;
                }
            }
        }
    }
    void tickUpload()
    {   // a short bar over the (stubbed) send, then the server's genuine check and the promo
        const double t = (juce::Time::getMillisecondCounterHiRes() - uploadT0) / 2500.0;
        barValue = juce::jlimit (0.0, 1.0, t); barLeft = barValue < 1.0 ? "Sending" : "Checking it arrived"; repaint(); snapAtQuarters();
        if (t < 1.3 || receipt.isObject()) return;
        if (account.isVoid()) account = juce::JSON::parse (cfg.work.getChildFile ("account.json").loadFileAsString());
        receipt = stub::upload (cfg.work, engineDir().getChildFile ("contribution.zip"), engineDir().getChildFile ("contribution/manifest.json"), account, device);
        cfg.work.getChildFile ("receipt.json").replaceWithText (juce::JSON::toString (receipt));
        setState ("uploaded", true); logLine ("uploaded (STUB): " + juce::JSON::toString (receipt, true));
        const auto pr = stub::promo (cfg.work, account, receipt);   // the server grants on the ACCEPTED results, never the client's count
        if (pr.isObject()) { cfg.work.getChildFile ("reward.json").replaceWithText (juce::JSON::toString (pr)); logLine ("promo (STUB): " + juce::JSON::toString (pr, true)); }
        show (wizapp::afterUpload (receipt, pr));
    }
    void autoAdvance()
    {   // rehearsals only: no hands
        ++autoTicks;
        if (screen == Screen::welcome && autoTicks == 6) { const char* e = std::getenv ("EJ_WIZARD_AUTO_EMAIL"); email.setText (e != nullptr && *e ? e : "rehearsal@example.com"); password.setText ("stub-password"); repaint(); snap ("filled"); }
        if (screen == Screen::welcome && autoTicks == 10) onPrimary();
        if (screen == Screen::consent && autoTicks == 6) { snap ("unticked"); agree.setToggleState (true, juce::sendNotification); repaint(); }
        if (screen == Screen::consent && autoTicks == 10) { snap ("ticked"); onPrimary(); }
        if (screen == Screen::scanDone && std::getenv ("EJ_WIZARD_AUTO_ANYWAY") != nullptr && autoTicks == 12 && secondary.isVisible()) { onSecondary(); return; }
        if ((screen == Screen::alreadyDone || screen == Screen::noCode || screen == Screen::betaSaved) && autoTicks == 12) onPrimary();
        if (screen == Screen::reward && autoTicks == 16) onPrimary();
        if ((screen == Screen::scanDone || screen == Screen::crashRecovered) && autoTicks == 12) onPrimary();
        if (screen == Screen::reward && autoTicks == 8) { onSecondary(); snap ("copied"); }
        if (screen == Screen::measuring && cfg.autoFinishAt > 0 && secondary.isEnabled()
            && wizapp::measuredSoFar (juce::JSON::parse (engineDir().getChildFile ("progress.json").loadFileAsString())) >= cfg.autoFinishAt)
        { cfg.autoFinishAt = 0; snap ("before_finish"); finishNow ("rehearsal"); snap ("finishing"); }
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
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }   // 13 Oct: each build its own name - the one-instance guard is keyed on it
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise (const juce::String&) override
    {
        std::signal (SIGINT, [] (int) { appInterrupted() = 1; }); std::signal (SIGTERM, [] (int) { appInterrupted() = 1; });
        window = std::make_unique<WizardWindow> (parseArgs (getCommandLineParameterArray()));
    }
    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { if (window == nullptr || window->content == nullptr || window->content->requestQuit()) quit(); }
private:
    std::unique_ptr<WizardWindow> window;
};
} // namespace

START_JUCE_APPLICATION (WizardApplication)
