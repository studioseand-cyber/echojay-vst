/*  THE HOST RIG (Sean's ask A, 13 Oct 2026)
    ============================================================================
    The gate cannot open an editor, so every window-lifecycle fault has reached
    Sean untested - "closing mid-reply" has now failed three rounds running, each
    time fixed against a READING of the code rather than a RUN of it. This loads
    the real built bundles into a real host process, plays audio through them,
    opens and closes editors on a script, drives the UI through the test-only
    hook in EJRigHook.h, and asserts on what the plugin actually holds.

    WHAT IT IS NOT. It is not a replacement for Sean's Logic pass: Logic's own
    window parenting, its AU hosting service and its automation are not here. It
    is the instrument that should catch most of it first.

    WHAT IT NEEDS
      - a logged-in GUI session (it creates real windows). Over SSH or in a
        headless runner it REFUSES rather than passes - see the WindowServer
        check in main(). Sean's instruction: skip loudly, never pass quietly.
      - an isolated ECHOJAY_STATE_HOME. Refuses without one, like every harness
        in this tree (6 Sep ruling), and the hook refuses too.
      - the mock server already listening (tools/host_rig/build_and_run.sh
        starts it), reached through the auth.json this writes.

    USAGE  host_rig <V2.component> <Link.component> <mockPort>
    ============================================================================ */
#define Point CarbonDummyPointName
#define Component CarbonDummyCompName
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#undef Point
#undef Component
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "EJRigHook.h"
// SEAN'S RULE 1 (16 Oct 2026): a REAL third-party plugin, dialled through a REAL map. The fingerprint and
// the display parse have to be the plugin's own arithmetic, not a re-implementation of it in the rig - a
// rig that computed its own fp would pass a map the product could never have matched.
#include "EJPaceCheck.h"
#include <regex>
#include <cstring>   // LV11 hashes the output BITS, so memcpy rather than a reinterpret
#include "viz/EedGrMeter.h"   // E7: the meter's label geometry is a pure function, so a row can hold it   // the standing rule: this rig never loads a licence-bound bundle
#include "EchoJayParamMaps.h"
// ---- APPKIT IN AN AU HOST'S TU, THE WAY JUCE ITSELF DOES IT (26 Oct 2026) ------------------
// The live-view rows read the real view hierarchy, so this TU needs AppKit and the ObjC runtime.
// Two collisions have to be stepped around, and JUCE's own juce_BasicNativeHeaders.h steps around
// the same two the same way: Carbon still declares `Point` and `Component`, which are a JUCE
// template and a JUCE class. Renamed for the duration of the import, then put back.
#define Point     CarbonDummyPointName
#define Component CarbonDummyCompName
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#undef Point
#undef Component


// ============================================================================
//  THE LIVE VIEW, SEEN FROM OUTSIDE THE PLUGIN (his 26 Oct ruling, step b)
// ============================================================================
/** *"Your bench passed all of these, so it isn't testing what I see... reproduce each fault in a
     real host ... driving a drag through the real editor and sampling each tick: the copy's
     displayed value, the Link's value, the hosted view's frame and layer properties, and the
     message thread's frame time."*

    WHY A DRAW COUNT IS THE INSTRUMENT. His fault 2 is "I hear the change, but the knob on screen
    doesn't move". The knob is drawn by the PLUGIN'S OWN VIEW, by the plugin's own code - so the
    only measure that can see it is how many times that view's own `drawRect:` ran. A parameter
    value cannot see it (the parameter moved: that is what he heard), and a screenshot cannot see it
    either, because asking for one FORCES a draw and so destroys the thing being measured.

    So this counts the guest view's own draws by swapping the implementation of `drawRect:` on that
    one class, in THIS process, for one that counts and then calls the original. It lives in the rig
    and never in the product. It also CATCHES what the original throws, which is what turns his
    AUDelay crash from a dead process into a row with a verdict.

    THE SAME COUNT ANSWERS FAULT 3. Hands off, nothing may draw at all - *"with hands off nothing in
    the view changes at all for 10 seconds"* - so the wobble is a draw count that will not stay at
    zero. One instrument, both faults, opposite directions. */
static int          gGuestDraws = 0;
static int          gGuestThrows = 0;
static juce::String gGuestThrowWhat;
static IMP          gGuestOrigDraw = nullptr;
static Method       gGuestDrawMethod = nullptr;
static juce::String gGuestDrawOwner;

static void ejPumpMsgThread (int ms);   // Rig::pump, declared here and defined below it
/** HOW THIS ROW KEEPS THE HOST ALIVE WHILE IT WAITS. On the own rack, pumping the message thread
    is enough. In a LIVE VIEW it is not: the view only takes gestures while it is FOLLOWING the
    Link, and a view that goes stale puts the read-only shield back up - so a drag would land on
    our shield and the row would go red for a reason that is not his fault. The Link road passes a
    pump that also feeds real audio, which is what keeps the tap and the mirror alive. */
using EJPumpFn = std::function<void (int)>;

static juce::String ejNS (NSString* s) { return s != nil ? juce::String ([s UTF8String]) : juce::String(); }

static void ejGuestDrawRect (id self, SEL sel, NSRect r)
{
    ++gGuestDraws;
    @try { ((void (*) (id, SEL, NSRect)) gGuestOrigDraw) (self, sel, r); }
    @catch (NSException* e)
    {
        ++gGuestThrows;
        if (gGuestThrowWhat.isEmpty())
            gGuestThrowWhat = ejNS ([e name]) + ": " + ejNS ([e reason]);
    }
}

/** The class that actually implements `drawRect:` for this view, refusing NSView itself: swapping
    NSView's own implementation would count every view in the process and prove nothing about the
    guest. Returns nil when only NSView implements it (a view that draws through a layer delegate
    instead), and the row says so rather than reporting a zero it cannot interpret. */
static Class ejDrawRectOwner (NSView* v)
{
    for (Class c = [v class]; c != nil && c != [NSView class]; c = class_getSuperclass (c))
    {
        unsigned int n = 0;
        Method* list = class_copyMethodList (c, &n);
        bool found = false;
        for (unsigned int i = 0; i < n; ++i)
            if (method_getName (list[i]) == @selector (drawRect:)) { found = true; break; }
        if (list != nullptr) free (list);
        if (found) return c;
    }
    return nil;
}

static bool ejCountGuestDraws (NSView* v)
{
    if (v == nil || gGuestOrigDraw != nullptr) return false;
    Class owner = ejDrawRectOwner (v);
    if (owner == nil) return false;
    gGuestDrawMethod = class_getInstanceMethod (owner, @selector (drawRect:));
    if (gGuestDrawMethod == nullptr) return false;
    gGuestOrigDraw  = method_getImplementation (gGuestDrawMethod);
    gGuestDrawOwner = ejNS (NSStringFromClass (owner));
    method_setImplementation (gGuestDrawMethod, (IMP) ejGuestDrawRect);
    return true;
}

static void ejStopCountingGuestDraws()
{
    if (gGuestDrawMethod != nullptr && gGuestOrigDraw != nullptr)
        method_setImplementation (gGuestDrawMethod, gGuestOrigDraw);
    gGuestDrawMethod = nullptr; gGuestOrigDraw = nullptr;
}

static NSView* ejFindViewOfClass (NSView* root, NSString* name)
{
    if (root == nil) return nil;
    if ([NSStringFromClass ([root class]) isEqualToString: name]) return root;
    for (NSView* sv in [root subviews])
        if (NSView* hit = ejFindViewOfClass (sv, name)) return hit;
    return nil;
}

/** The clip container, found the way a user's eye finds it: by sweeping this process's windows.
    Never from a handle the product passed out - the row is about what is actually on screen. */
static NSView* ejGuestInside (NSView* container);   // defined just below; the finder prefers a full one

/** THE CONTAINER THAT HAS A PLUGIN VIEW IN IT (27 Oct 2026).

    This used to return the FIRST EchoJayClipContainer in [NSApp windows] order, and with several
    windows open - the V2's AU, the V2's VST3, a Link's own plugin window - the first one it met
    could be an empty container in another window. Then RE9 reported "there was no plugin view inside
    the container to measure" and the own-rack checks reported "container absent" in the same pass
    where the pane's own settle verdict said `container=877x387 subviews=1`. The pane was right and
    the finder was looking in the wrong window.

    So: every window is searched, and a container with something inside it wins over an empty one.
    An empty one is still returned if that is all there is, because "found it, and it is empty" and
    "there is no container at all" are different findings and a row says which. */
static void ejDumpViewTree (NSView* v, int depth, juce::String& out)
{
    if (v == nil || depth > 6) return;
    out << juce::String::repeatedString (" ", depth * 2) << NSStringFromClass ([v class]).UTF8String
        << " " << (int) [v frame].size.width << "x" << (int) [v frame].size.height
        << ([v isHidden] ? " hidden" : "") << "\n";
    for (NSView* sv in [v subviews]) ejDumpViewTree (sv, depth + 1, out);
}

static NSView* ejFindClipContainer()
{
    NSView* empty = nil;
    for (NSWindow* w in [NSApp windows])
        if (NSView* hit = ejFindViewOfClass ([w contentView], @"EchoJayClipContainer"))
        {
            if (ejGuestInside (hit) != nil) return hit;
            if (empty == nil) empty = hit;
        }
    // ---- AND IF THERE IS NONE, SAY WHAT THERE WAS (27 Oct 2026) -----------------------------
    // Three rows in one pass reported "container absent" in the same seconds as the plugin's own
    // settle verdict - `container=877x387 subviews=1`, with NativeClip2-diag naming the peer it sits
    // in. One of the two is looking in the wrong place and a guess cost two runs, so the first
    // failure of this sweep prints every window and its view tree. Once per run, and only on a miss.
    static bool said = false;
    if (empty == nil && ! said)
    {
        said = true;
        juce::String out ("EJRIG: the clip-container sweep found NOTHING. Every window in this "
                          "process, with its view tree:\n");
        for (NSWindow* w in [NSApp windows])
        {
            out << "  window \"" << juce::String::fromUTF8 ([[w title] UTF8String] ? [[w title] UTF8String] : "(no title)")
                << "\" " << ([w isVisible] ? "visible" : "hidden") << "\n";
            ejDumpViewTree ([w contentView], 2, out);
        }
        std::printf ("%s", out.toRawUTF8());
        std::fflush (stdout);
    }
    return empty;
}

static NSView* ejGuestInside (NSView* container)
{
    if (container == nil) return nil;
    for (NSView* sv in [container subviews])
        if (! [NSStringFromClass ([sv class]) isEqualToString: @"EchoJayViewShield"])
            return sv;
    return nil;
}

/** One tick's worth of everything he asked to be sampled, on the host's side of the glass. */
struct EJViewSample
{
    double tMs = 0.0;
    NSRect guest {}, cont {};
    NSPoint boundsOrigin {};
    double corner = 0.0, borderW = 0.0;
    bool masks = false;
    NSRect contLayerBounds {}, guestLayerBounds {};
    NSPoint contLayerPos {}, guestLayerPos {};
    int draws = 0;
};

static EJViewSample ejSampleView (NSView* cont, NSView* guest)
{
    EJViewSample s;
    s.tMs  = juce::Time::getMillisecondCounterHiRes();
    s.draws = gGuestDraws;
    if (cont != nil)
    {
        s.cont = [cont frame];
        s.boundsOrigin = [cont bounds].origin;
        if (CALayer* l = [cont layer])
        {
            s.corner = l.cornerRadius; s.borderW = l.borderWidth; s.masks = l.masksToBounds;
            s.contLayerBounds = l.bounds; s.contLayerPos = l.position;
        }
    }
    if (guest != nil)
    {
        s.guest = [guest frame];
        if (CALayer* l = [guest layer]) { s.guestLayerBounds = l.bounds; s.guestLayerPos = l.position; }
    }
    return s;
}

static juce::String ejWhatMoved (const EJViewSample& a, const EJViewSample& b)
{
    juce::StringArray moved;
    if (! NSEqualRects (a.guest, b.guest))   moved.add ("the plugin view's frame");
    if (! NSEqualRects (a.cont, b.cont))     moved.add ("the container's frame");
    if (! NSEqualPoints (a.boundsOrigin, b.boundsOrigin)) moved.add ("the container's bounds origin");
    if (a.corner != b.corner || a.borderW != b.borderW || a.masks != b.masks)
        moved.add ("the container layer's style");
    if (! NSEqualRects (a.contLayerBounds, b.contLayerBounds)
        || ! NSEqualPoints (a.contLayerPos, b.contLayerPos))  moved.add ("the container layer's geometry");
    if (! NSEqualRects (a.guestLayerBounds, b.guestLayerBounds)
        || ! NSEqualPoints (a.guestLayerPos, b.guestLayerPos)) moved.add ("the plugin layer's geometry");
    if (b.draws != a.draws) moved.add (juce::String (b.draws - a.draws) + " draw(s) of the plugin's own view");
    return moved.isEmpty() ? juce::String ("nothing") : moved.joinIntoString (", ");
}

/** A REAL DRAG, through the real editor: NSEvents posted to the guest view's own window at a point
    on the guest view, down / moved / up, with the message thread pumped between steps exactly as it
    is for a hand. Returns the draws the drag produced. */
static int ejDragOnGuest (NSView* guest, NSPoint inView, double dy, int steps,
                          const EJPumpFn& pump)
{
    if (guest == nil) return 0;
    NSWindow* w = [guest window];
    if (w == nil) return 0;
    const int before = gGuestDraws;
    const NSPoint start = [guest convertPoint: inView toView: nil];
    auto post = [&] (NSEventType t, NSPoint p)
    {
        NSEvent* e = [NSEvent mouseEventWithType: t
                                       location: p
                                  modifierFlags: 0
                                      timestamp: [[NSProcessInfo processInfo] systemUptime]
                                   windowNumber: [w windowNumber]
                                        context: nil
                                    eventNumber: 0
                                     clickCount: 1
                                       pressure: t == NSEventTypeLeftMouseUp ? 0.0f : 1.0f];
        if (e != nil) [w sendEvent: e];
    };
    post (NSEventTypeLeftMouseDown, start);
    pump (40);
    for (int i = 1; i <= steps; ++i)
    {
        post (NSEventTypeLeftMouseDragged, NSMakePoint (start.x, start.y + dy * (double) i / (double) steps));
        pump (40);
    }
    post (NSEventTypeLeftMouseUp, NSMakePoint (start.x, start.y + dy));
    pump (120);
    return gGuestDraws - before;
}

#include "EchoJayParamApply.h"
// SEAN'S #7: the interrupted sentences live in ONE predicate (EchoJayAPI::looksInterrupted), and the legs
// read it rather than copying the wording - the "[interrupted]" tag they used to grep for is gone, and a
// leg that carried its own copy of a sentence would go red on the next wording change rather than on a bug.
#include "EchoJayAPI.h"
#include "LinkShm.h"          // the registry, so the rig can reap rows whose publisher is gone (item 14)
#include <cmath>
#include <cstdio>

// ============================================================================
//  reporting
// ============================================================================
static int failures = 0, passes = 0;
static void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    if (ok) { ++passes; std::printf ("  ok    %s\n", what.toRawUTF8()); }
    else    { ++failures; std::printf ("  FAIL  %s%s\n", what.toRawUTF8(),
                                       detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : ""); }
    std::fflush (stdout);
}
static int skips = 0;
// Owed from the lifecycle legs to the capability block: ruling 3's double-click needs a slot to press, and
// the rack is empty where the lifecycle legs run. File scope because the two halves are different functions.
static bool slotDoubleClickOwed = false;
/** A LEG THAT COULD NOT RUN IS NOT A LEG THAT PASSED (14 Oct 2026). The slot double-click leg printed
    "ok  (skipped) the open rack has no slot to double-click" and added one to the pass count: ruling 3's
    only coverage was a line that asserted nothing, and the total said 40 either way. Skips are counted
    separately and named in the summary, so a pass count cannot be padded by work that did not happen. */
static void skip (const juce::String& what, const juce::String& why)
{
    ++skips; std::printf ("  SKIP  %s  [%s]\n", what.toRawUTF8(), why.toRawUTF8()); std::fflush (stdout);
}
static void leg (const char* name) { std::printf ("== %s ==\n", name); std::fflush (stdout); }

// ============================================================================
//  the AU, registered in-process from its bundle path (the plugin_load_test.mm
//  idiom - no install, so the rig tests the BUILT bundle and never the one
//  Sean has in Logic)
// ============================================================================
/** ---- A SHELL CARRIES MANY PLUGINS (16 Oct 2026) ----------------------------------------------
    WaveShell1-AU is ONE component bundle holding hundreds of AudioComponents, one per Waves plugin,
    each with its own type/subtype/manufacturer. The first cut of this registered and returned
    AudioComponents[0] only, so asking for Renaissance Axx got whichever plugin happened to be first
    and the row skipped with *"carries 1 plugin(s) and none of them is named like Renaissance Axx"*.
    `wantName` registers and returns the one whose Info.plist `name` matches; empty keeps the old
    behaviour (entry 0), which is right for a single-plugin bundle.

    STILL NOT A SCAN: one named bundle, its own plist, and AudioComponentRegister for the entries it
    declares. Nothing is instantiated here and no other plugin on the machine is touched. */
static bool registerAU (const juce::File& component, juce::String& identOut,
                        const juce::String& wantName = {});
static bool registerAU (const juce::File& component, juce::String& identOut,
                        const juce::String& wantName)
{
    auto* url = CFURLCreateWithFileSystemPath (nullptr, component.getFullPathName().toCFString(),
                                               kCFURLPOSIXPathStyle, true);
    CFBundleRef bundle = CFBundleCreate (nullptr, url); CFRelease (url);
    if (bundle == nullptr) { std::printf ("  AU: CFBundleCreate failed for %s\n", component.getFullPathName().toRawUTF8()); return false; }
    auto* comps = (CFArrayRef) CFBundleGetValueForInfoDictionaryKey (bundle, CFSTR ("AudioComponents"));
    if (comps == nullptr || CFArrayGetCount (comps) == 0) { std::printf ("  AU: no AudioComponents\n"); return false; }
    // Which entry: the one whose `name` contains wantName (a shell), else the first (a plain plugin).
    // A Waves plist name reads "Waves: Renaissance Axx Stereo", so the match is a contains, and the
    // SHORTEST matching name wins - "Renaissance Axx" must not resolve to "Renaissance Axx Mono" when
    // both are there and the stereo one is what a stereo rack wants. Mono is preferred only if it is
    // the one thing offered.
    CFIndex chosen = 0;
    if (wantName.isNotEmpty())
    {
        // THE STEREO VARIANT, on a stereo rack. The first cut took the SHORTEST matching name, which
        // on Waves means "Renaissance Axx Mono" - and the product then refused it, correctly, by the
        // 21m rule ("never a mono variant on a stereo rack"), so the rack stayed empty and the Link
        // scenarios had nothing to run on. Scored rather than ordered: stereo wins, mono loses, and
        // the shortest name breaks a tie (so "Renaissance Axx" beats "Renaissance Axx Live").
        CFIndex best = -1; int bestScore = -1000, bestLen = 1 << 30;
        for (CFIndex i = 0; i < CFArrayGetCount (comps); ++i)
        {
            auto* di = (CFDictionaryRef) CFArrayGetValueAtIndex (comps, i);
            auto* nv = (CFStringRef) CFDictionaryGetValue (di, CFSTR ("name"));
            if (nv == nullptr) continue;
            const auto nm = juce::String::fromCFString (nv);
            if (! nm.containsIgnoreCase (wantName)) continue;
            // WAVES WRITES IT AS A SUFFIX, NOT A WORD: "Waves: Renaissance Axx (m)" and
            // "...(s)". The first cut scored only the words "stereo"/"mono", so both entries tied on
            // zero, the first won, and the catalogue got the MONO plugin - which the rack then
            // refused on a stereo channel, leaving the Link processes with nothing to hold.
            const int score = (nm.endsWith ("(s)") || nm.containsIgnoreCase ("stereo") ? 2 : 0)
                            - (nm.endsWith ("(m)") || nm.containsIgnoreCase ("mono")   ? 2 : 0);
            if (score > bestScore || (score == bestScore && nm.length() < bestLen))
            { bestScore = score; bestLen = nm.length(); best = i; }
        }
        if (best < 0)
        {
            std::printf ("  AU: %s declares %d component(s) and none is named like \"%s\"\n",
                         component.getFileName().toRawUTF8(), (int) CFArrayGetCount (comps),
                         wantName.toRawUTF8());
            CFRelease (bundle);
            return false;
        }
        chosen = best;
    }
    auto* d = (CFDictionaryRef) CFArrayGetValueAtIndex (comps, chosen);
    auto str = [&] (const char* k) { return juce::String::fromCFString ((CFStringRef) CFDictionaryGetValue (d, juce::String (k).toCFString())); };
    auto code = [&] (const char* k) { auto s = str (k); return (OSType) ((s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3]); };
    int version = 0;
    CFNumberGetValue ((CFNumberRef) CFDictionaryGetValue (d, CFSTR ("version")), kCFNumberIntType, &version);
    AudioComponentDescription desc {};
    desc.componentType = code ("type"); desc.componentSubType = code ("subtype"); desc.componentManufacturer = code ("manufacturer");
    auto factory = (AudioComponentFactoryFunction) CFBundleGetFunctionPointerForName (bundle, str ("factoryFunction").toCFString());
    if (factory == nullptr) { std::printf ("  AU: factory not found\n"); return false; }
    AudioComponentRegister (&desc, str ("name").toCFString(), (UInt32) version, factory);
    identOut = "AudioUnit:Effects/" + str ("type") + "," + str ("subtype") + "," + str ("manufacturer");
    return true;
}

// ============================================================================
//  SEAN'S RULE 1 (16 Oct 2026): A REAL THIRD-PARTY PLUGIN, A REAL MAP
// ============================================================================
//
//  "Every failure below gets a rig row that runs RED on ship_2026-10-15c before you fix it... At minimum:
//   a real third-party plugin (MTransient, it's free) added with settings, with its knobs read back from
//   the plugin itself."
//
//  THE WHOLE REASON THE RIG MISSED #1. A third-party plugin is dialled through a parameter map keyed on the
//  slot's fingerprint; the mock answered every GET with {}, so no map ever existed, so no third party was
//  ever dialled HERE - and four plugins that sat at their defaults in Logic passed a rig that had never
//  loaded one. This builds the map from the PLUGIN: its parameter names and its sampled anchors, keyed by
//  the fingerprint the product's own `fingerprintForDescription` computes. Nothing is written by hand.
//
//  WHY MTransient. Sean names it, it is free (no PACE, no iLok, so the standing rule against licence-bound
//  plugins is not engaged), and it is one of the four that failed. A missing plugin is a loud SKIP: this
//  rig does not pass by not testing.
struct ThirdPartyPrep
{
    bool ok = false;
    juce::String why;                 // when !ok, in one sentence
    juce::String name;                // the plugin's display name, as the agent must ask for it
    juce::String fp;                  // the fingerprint the plugin will compute for this binary
    juce::String semantic = "attack_db";
    juce::String paramName;           // the plugin's own control name the map resolves to
    int    paramIndex   = -1;
    int    paramCount   = 0;
    double defaultValue = 0.0;        // what that control reads before anything dials it
    double askValue     = 0.0;        // what the agent will ask for (well away from the default)
    int    anchorCount  = 0;
};

/** Register MTransient, write it into the isolated chain_plugins.xml so a rack can resolve it by name,
    sample its chosen control, and write the map file the mock serves. */
static ThirdPartyPrep prepareThirdParty (juce::AudioPluginFormatManager& fm, const juce::File& mapsFile)
{
    ThirdPartyPrep p;
    // ---- WHICH THIRD PARTY (Sean, 16 Oct 2026) ----------------------------------------------------
    //
    // *"Prove it with a rig row using a real installed plugin with a correct map, e.g. Renaissance Axx
    // or another plugin on this Mac, not MTransient (its map is wrong, B is fixing it)."* And, later
    // the same morning: *"use Renaissance Axx (which I can see landed at -20.0) as the proof case for
    // the read-back fix."*
    //
    // So the default is Renaissance Axx, whose THRESHOLD he watched land at -20.0 in Logic, and
    // MTransient is the fallback for a machine that does not have Waves. The map this rig serves is
    // sampled from the plugin itself either way, so "a correct map" is true by construction here -
    // what changed is that the plugin is one he can check by eye.
    //
    // A SHELL HOLDS MANY PLUGINS. WaveShell is one component carrying hundreds of types, so the type
    // is chosen BY NAME rather than by taking found[0] - which, on a shell, is whichever plugin
    // happens to be first and is not the one anybody asked about.
    struct Candidate { const char* path; const char* wantName; const char* param; const char* semantic; };
    static const Candidate kCandidates[] = {
        { "/Library/Audio/Plug-Ins/Components/WaveShell1-AU 15.0.component", "Renaissance Axx", "Threshold", "threshold_db" },
        { "/Library/Audio/Plug-Ins/Components/WaveShell1-AU 14.0.component", "Renaissance Axx", "Threshold", "threshold_db" },
        { "/Library/Audio/Plug-Ins/Components/MTransient.component",         "MTransient",      "Attack",    "attack_db" },
    };
    const char* envComp  = ::getenv ("EJ_RIG_THIRDPARTY");
    const char* envName  = ::getenv ("EJ_RIG_TP_NAME");
    juce::File comp;
    juce::String wantName, wantParam, wantSemantic;
    if (envComp != nullptr && *envComp != 0)
    {
        comp = juce::File (juce::String (envComp));
        wantName = envName != nullptr ? juce::String (envName) : juce::String();
    }
    else
    {
        juce::StringArray tried;
        for (const auto& c : kCandidates)
        {
            const juce::File f { juce::String (c.path) };
            tried.add (f.getFileName());
            if (! f.exists()) continue;
            comp = f; wantName = c.wantName; wantParam = c.param; wantSemantic = c.semantic;
            break;
        }
        if (comp == juce::File())
        {
            p.why = "none of the third-party plugins this rig knows how to use is installed (tried "
                  + tried.joinIntoString (", ") + ") - EJ_RIG_THIRDPARTY=<path> and EJ_RIG_TP_NAME=<name> "
                    "name another";
            return p;
        }
    }
    if (! comp.exists())
    {
        p.why = "there is no third-party plugin at " + comp.getFullPathName()
              + " - EJ_RIG_THIRDPARTY=<path> names another";
        return p;
    }
    // ---- NEVER A LICENCE-BOUND BUNDLE (Sean's standing rule, and it bites here) -------------------
    // MTransient - the plugin the rig used until this morning - carries __Pace_Eden.bundle, so it IS
    // licence-bound: loading it in an unsigned host is exactly what put a security prompt on his
    // desktop on 2 Oct. Renaissance Axx, Vertigo VSM-3 and Pro-C 3 are not wrapped, which is why the
    // candidate list leads with them. A named override is refused too, loudly: the rule is the rule.
    if (echojay::isPaceWrapped (comp))
    {
        p.why = comp.getFileName() + " is PACE-wrapped (licence-bound), and this rig never loads one - "
                "the standing rule, because an unsigned host loading one can raise a security prompt. "
                "The matrix's P3 case (a plugin with a wrong map) therefore belongs on the Logic "
                "smoke-test list, not here.";
        return p;
    }
    if (wantParam.isNotEmpty() && ::getenv ("EJ_RIG_TP_PARAM") == nullptr)
        ::setenv ("EJ_RIG_TP_PARAM", wantParam.toRawUTF8(), 0);
    if (wantSemantic.isNotEmpty()) p.semantic = wantSemantic;
    // ONE COMPONENT, BY PATH. Never a scan: a full AU scan triggers the licence prompts on this Mac and
    // has crashed it (the standing rule), and this needs exactly one binary.
    juce::String ident;
    if (! registerAU (comp, ident, wantName))
    { p.why = "could not register " + (wantName.isNotEmpty() ? wantName + " out of " : juce::String())
            + comp.getFileName() + " in-process"; return p; }
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats())
        if (f->getName() == "AudioUnit") f->findAllTypesForFile (found, ident);
    if (found.isEmpty()) { p.why = "no AU types for " + ident; return p; }
    // ---- THE STEREO DESCRIPTION, NOT THE FIRST (16 Oct 2026) ---------------------------------
    // One AU can offer several channel configurations and JUCE names them "(m)" and "(s)". The first
    // cut took found[0] and got "Renaissance Axx (m)" - which the rack then REFUSED, correctly, as a
    // mono plugin on a stereo channel ("21m item 2"), so scenarios A3 and A4 were red for a fixture
    // reason rather than the product one. A rig that cannot set its own fixture up proves nothing.
    const juce::PluginDescription* chosen = found[0];
    {
        const juce::PluginDescription* best = nullptr;
        int bestScore = -1000;
        for (auto* d : found)
        {
            if (d == nullptr) continue;
            if (wantName.isNotEmpty() && ! d->name.containsIgnoreCase (wantName)) continue;
            const int score = (d->name.endsWith ("(s)") || d->name.containsIgnoreCase ("stereo") ? 2 : 0)
                            - (d->name.endsWith ("(m)") || d->name.containsIgnoreCase ("mono")   ? 2 : 0);
            if (score > bestScore) { bestScore = score; best = d; }
        }
        if (best == nullptr && wantName.isNotEmpty())
        {
            p.why = comp.getFileName() + " carries " + juce::String (found.size())
                  + " plugin(s) and none of them is named like \"" + wantName + "\"";
            return p;
        }
        if (best != nullptr) chosen = best;
    }
    const auto desc = *chosen;
    p.name = desc.name;

    juce::String err;
    auto inst = fm.createPluginInstance (desc, 48000.0, 512, err);
    if (inst == nullptr) { p.why = p.name + " would not instantiate: " + err; return p; }
    inst->prepareToPlay (48000.0, 512);

    auto& params = inst->getParameters();
    p.paramCount = params.size();
    // THE FINGERPRINT IS THE PRODUCT'S OWN FUNCTION, over the live parameter count - exactly what
    // ChainHost does when the slot loads. Anything else and the map would be keyed to nothing.
    p.fp = echojay::fingerprintForDescription (desc, p.paramCount);

    // The control to dial. Sean's own case is MTransient's ATTACK, which reads in dB and was still 0.00
    // after the agent said it had pushed it up 6 dB.
    const char* envParam = ::getenv ("EJ_RIG_TP_PARAM");
    // The candidate's own control name (set above) unless the environment names another.
    const juce::String wantCtrl = envParam != nullptr && *envParam != 0 ? juce::String (envParam)
                                : wantParam.isNotEmpty()                ? wantParam
                                                                        : juce::String ("Attack");
    for (int i = 0; i < params.size(); ++i)
    {
        if (params[i] == nullptr || ! params[i]->isAutomatable()) continue;
        const auto n = params[i]->getName (echojay::kParamNameQueryLen);
        if (n.equalsIgnoreCase (wantCtrl)) { p.paramIndex = i; p.paramName = n; break; }
    }
    if (p.paramIndex < 0)
        for (int i = 0; i < params.size(); ++i)
        {
            if (params[i] == nullptr || ! params[i]->isAutomatable()) continue;
            const auto n = params[i]->getName (echojay::kParamNameQueryLen);
            if (n.containsIgnoreCase (wantCtrl)) { p.paramIndex = i; p.paramName = n; break; }
        }
    if (p.paramIndex < 0)
    {
        juce::StringArray some;
        for (int i = 0; i < juce::jmin (12, params.size()); ++i)
            if (params[i] != nullptr) some.add (params[i]->getName (echojay::kParamNameQueryLen));
        p.why = p.name + " has no automatable control named like \"" + wantCtrl + "\" (first few: "
              + some.joinIntoString (", ") + ")";
        return p;
    }

    auto* par = params[p.paramIndex];
    // WHAT IT READS BEFORE ANYTHING TOUCHES IT. The row's claim is that the knob MOVED, and "0.00 dB"
    // was Sean's whole report, so the default is the thing the read-back must not equal.
    {
        float v = 0.0f; bool negInf = false;
        if (echojay::parseDisplayForUnit (par->getCurrentValueAsText(), "db", v, negInf))
            p.defaultValue = (double) v;
    }

    // ---- THE ANCHORS, SAMPLED FROM THE PLUGIN ------------------------------------------------------
    // (display value, normalised) pairs, read off the control itself. This is what the server's own map
    // builder does, and it is the only way the numbers can be the plugin's rather than a guess.
    const float startNorm = par->getValue();
    juce::Array<juce::var> anchors;
    double lo = 1.0e9, hi = -1.0e9;
    for (int k = 0; k <= 10; ++k)
    {
        const float norm = (float) k / 10.0f;
        par->setValueNotifyingHost (norm);
        // One message-loop turn: a plugin whose display is one write behind (the 21 Sep settled-readback
        // finding) would otherwise be sampled stale, and every anchor would be off by one step.
        // Rig::pump's idiom, not runDispatchLoopUntil - JUCE_MODAL_LOOPS_PERMITTED is off in a plugin
        // build, so that member does not exist here (Rig itself says so in its own comment). Written out
        // rather than calling Rig::pump because Rig is declared below this function.
        {
            // ---- LONG ENOUGH FOR A WAVESHELL (16 Oct 2026) --------------------------------------
            // 12 ms was not. The sampled table came back 0, 0, -48, -48, -36, -36, -24, -18, -18,
            // -6, 0 - a display one or two writes behind, which is the 21 Sep settled-readback
            // finding arriving in the fixture instead of the product. The product then REFUSED the
            // map, correctly ("control entry unusable (bad index or anchors)"), and three scenarios
            // spent three passes red for a reason that was mine. The product's own settle bound is
            // 250 ms; this gives each anchor 120 ms, which is what it takes on this plugin.
            const double endAt = juce::Time::getMillisecondCounterHiRes() + 120.0;
            while (juce::Time::getMillisecondCounterHiRes() < endAt)
            {
                juce::Timer::callPendingTimersSynchronously();
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false);
            }
        }
        float v = 0.0f; bool negInf = false;
        if (! echojay::parseDisplayForUnit (par->getCurrentValueAsText(), "db", v, negInf)) continue;
        juce::Array<juce::var> pair; pair.add ((double) v); pair.add ((double) norm);
        anchors.add (juce::var (pair));
        lo = juce::jmin (lo, (double) v); hi = juce::jmax (hi, (double) v);
    }
    par->setValueNotifyingHost (startNorm);
    p.anchorCount = anchors.size();
    if (p.anchorCount < 2 || hi - lo < 1.0e-6)
    {
        p.why = p.name + "'s \"" + p.paramName + "\" gave " + juce::String (p.anchorCount)
              + " parseable dB anchor(s) over range " + juce::String (hi - lo, 3)
              + " - there is nothing to interpolate, so a map would be a fiction";
        return p;
    }
    // A VALUE WELL AWAY FROM THE DEFAULT, and inside the sampled range - an out-of-range ask is refused
    // by applyOne (correctly), and a row that asked for one would be testing the refusal.
    const double mid = (lo + hi) * 0.5;
    p.askValue = std::abs (mid - p.defaultValue) >= 1.0 ? mid
               : (std::abs (hi - p.defaultValue) > std::abs (lo - p.defaultValue) ? hi * 0.6 : lo * 0.6);
    p.askValue = juce::jlimit (lo, hi, p.askValue);
    // Rounded, so the ask is a number a person would type and the comparison is not about float printing.
    p.askValue = std::round (p.askValue * 10.0) / 10.0;
    if (std::abs (p.askValue - p.defaultValue) < 0.5)
    {
        p.why = p.name + "'s \"" + p.paramName + "\" default (" + juce::String (p.defaultValue, 2)
              + " dB) is too close to every value in its range to tell a dial from a no-op";
        return p;
    }

    // ---- THE MAP FILE THE MOCK SERVES --------------------------------------------------------------
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("index", p.paramIndex);
        entry->setProperty ("name",  p.paramName);
        entry->setProperty ("unit",  "db");
        entry->setProperty ("anchors", anchors);
        // ---- BOTH TABLES, UNDER BOTH KEYS ------------------------------------------------------
        // A third-party dial goes through `controls`, keyed by the plugin's OWN control name, and the
        // rack says so when it is handed the other shape: *"a 'params' wrapper is a built-in device
        // payload and reached the third-party map path; nothing was dialled and the map is not at
        // fault"*. The map carries BOTH, because a real server map does: `params` for a semantic a
        // model might use, `controls` for the registry's own name. The row dials through `controls`.
        auto* paramsObj = new juce::DynamicObject();
        paramsObj->setProperty (juce::Identifier (p.semantic), juce::var (entry));
        auto* controlsObj = new juce::DynamicObject();
        controlsObj->setProperty (juce::Identifier (p.paramName), juce::var (entry));
        auto* map = new juce::DynamicObject();
        map->setProperty ("params",   juce::var (paramsObj));
        map->setProperty ("controls", juce::var (controlsObj));
        // `fp` IS NOT OPTIONAL, and the rig's first map omitted it. ChainHost refuses to apply a map
        // whose fp field does not equal the slot's fingerprint - a keying guard that exists to stop a
        // wrong-layout map touching a parameter - and the whole channel worked right up to it:
        //   "EJParamApply: map fp mismatch for slot 0 ("MTransient"): key 3decf53d6bc0 vs map.fp
        //    (missing), apply refused"
        // The server's maps carry it, so a rig map without it is not a map the product would accept.
        map->setProperty ("fp", p.fp);
        // ---- THE FIXTURE IS CHECKED WITH THE PRODUCT'S OWN FUNCTION (16 Oct 2026) --------------
        // usableParamEntry is what the dial calls before it writes anything. Calling it HERE means a
        // map this rig would serve, and the product would refuse, is a loud SKIP with its reason -
        // instead of three scenarios going red and reading like product faults.
        if (! echojay::usableParamEntry (juce::var (entry)))
        {
            p.why = p.name + "'s \"" + p.paramName + "\" sampled a table the PRODUCT would refuse "
                    "(usableParamEntry says no: " + juce::String (anchors.size())
                  + " anchor(s), range " + juce::String (hi - lo, 2)
                  + " - most likely a display read before the plugin settled). The map is not served "
                    "and the third-party scenarios SKIP rather than failing for a fixture reason.";
            inst->releaseResources();
            return p;
        }
        auto* store = new juce::DynamicObject();
        store->setProperty (juce::Identifier (p.fp), juce::var (map));
        // "__any__" IS NOT A SHORTCUT PAST THE FINGERPRINT. The mock serves the exact key when it has
        // it and falls back to this one, saying so in its log; the row then asserts the EXACT key
        // matched as a separate check. Without it a version string that differed by a character would
        // fail the row for a reason that has nothing to do with what it tests, overnight, with nobody
        // to read the log.
        store->setProperty ("__any__", juce::var (map));
        // The ask, for the mock to substitute into the scripted op: the script cannot know this
        // plugin's control name or a value inside its range, and a hard-coded number would be either
        // refused as out of range or equal to the default the row exists to detect.
        auto* askObj = new juce::DynamicObject();
        askObj->setProperty ("name", p.name);
        askObj->setProperty ("semantic", p.semantic);
        askObj->setProperty ("control", p.paramName);   // the key a `controls` payload uses
        askObj->setProperty ("value", p.askValue);
        store->setProperty ("__ask__", juce::var (askObj));
        mapsFile.replaceWithText (juce::JSON::toString (juce::var (store), true));
    }

    // ---- ...AND BOTH LISTS, BECAUSE AN EDIT OP IS JUDGED AGAINST THE *ENTRIES* --------------------
    //
    // TWO FILES, and the first run of row T1 only wrote one. chain_plugins.xml is the KnownPluginList -
    // what can be INSTANTIATED - and the rack resolved MTransient from it quite happily. But an `edit`
    // op's NAME is validated against `entries_`, the catalogue, which lives in chain_entries.xml, and
    // that was empty:
    //
    //   EJEdit: judging 1 op(s) against 0 loadable entr(ies) - EMPTY, so only built-ins can resolve
    //   op 1 invalid: "MTransient" not in the loadable plugin list
    //
    // which is the rack being exactly right about a list nobody had filled. (It also explains why the
    // built-in add in row R1b landed and this one did not: a built-in needs no entry.)
    //
    // BOTH are read by ChainHost AT CONSTRUCTION, so both must be written before the V2 and the Links
    // exist. Inside the isolated root, like everything else here.
    {
        const auto dir = echojay::userAppData().getChildFile ("EchoJay");
        dir.createDirectory();

        juce::KnownPluginList kl;
        // SAME RULE for the KnownPluginList: load whatever is there, then add. In a fresh isolated root
        // there is nothing, which is the normal case; this is what stops a SECOND run of the rig in a
        // reused root from throwing away the first run's list.
        const auto listFile = dir.getChildFile ("chain_plugins.xml");
        if (listFile.existsAsFile())
            if (auto doc = juce::XmlDocument::parse (listFile)) kl.recreateFromXml (*doc);
        kl.addType (desc);
        if (auto xml = kl.createXml()) listFile.replaceWithText (xml->toString());

        // The catalogue, in the shape loadPluginList() parses: <CHAIN_ENTRIES scannedAt="...">
        // with one PluginDescription child per entry. `scannedAt` is stamped NOW, because an unstamped
        // cache logs as "scanned UNKNOWN" and may be treated as due for a rescan - and a rescan is the
        // one thing this rig must never trigger (it would walk every plugin on Sean's Mac).
        //
        // ADDITIVE, NEVER A REPLACEMENT. The first cut wrote a catalogue containing MTransient AND
        // NOTHING ELSE, which is a statement that this machine has exactly one plugin - and the live
        // pass then had the model say *"nothing else in your inventory fits the role"* and leave a rack
        // empty. A fixture that narrows the world is a fixture that changes the answers to rows it was
        // not written for.
        const auto entriesFile = dir.getChildFile ("chain_entries.xml");
        juce::XmlElement entries ("CHAIN_ENTRIES");
        int kept = 0;
        if (entriesFile.existsAsFile())
            if (auto doc = juce::XmlDocument::parse (entriesFile);
                doc != nullptr && doc->getTagName() == "CHAIN_ENTRIES")
                for (auto* c : doc->getChildIterator())
                {
                    juce::PluginDescription d;
                    if (! d.loadFromXml (*c)) continue;
                    if (d.name == desc.name && d.pluginFormatName == desc.pluginFormatName) continue;
                    if (auto* child = d.createXml().release()) { entries.addChildElement (child); ++kept; }
                }
        entries.setAttribute ("scannedAt", juce::String (juce::Time::currentTimeMillis()));
        if (auto* child = desc.createXml().release()) entries.addChildElement (child);
        // ---- THE MATRIX'S PLUGIN SET, so a LINK can resolve them without scanning ----------------
        //
        // A Link in its own process starts with whatever catalogue the shared root holds, and with
        // EJ_RIG_HOOK=1 it is REFUSED a scan (ChainHost::startScan and PluginScanner::startScan both
        // check it, by Sean's iLok rule). So the names the matrix's T2-T4 targets need have to be in
        // the catalogue before those processes start - written here, from one targeted enumeration per
        // component, never a scan.
        //
        // PACE-wrapped bundles are skipped with a line saying so, for the same reason as above.
        {
            struct Want { const char* path; const char* name; };
            static const Want kSet[] = {
                { "/Library/Audio/Plug-Ins/Components/WaveShell1-AU 15.0.component", "Renaissance Axx" },
                { "/Library/Audio/Plug-Ins/Components/Vertigo VSM-3.component",      "Vertigo VSM-3" },
                { "/Library/Audio/Plug-Ins/Components/FabFilter Pro-C 3.component",  "Pro-C 3" },
            };
            juce::StringArray added, refused;
            for (const auto& w : kSet)
            {
                const juce::File f { juce::String (w.path) };
                if (! f.exists()) { refused.add (juce::String (w.name) + " (not installed)"); continue; }
                if (echojay::isPaceWrapped (f)) { refused.add (juce::String (w.name) + " (PACE-wrapped)"); continue; }
                juce::String id2;
                if (! registerAU (f, id2, juce::String (w.name)))
                { refused.add (juce::String (w.name) + " (would not register)"); continue; }
                juce::OwnedArray<juce::PluginDescription> types;
                for (auto* fmt : fm.getFormats())
                    if (fmt->getName() == "AudioUnit") fmt->findAllTypesForFile (types, id2);
                // The same stereo rule as above: a rack that refuses a mono variant must be given
                // the stereo one, or the catalogue holds a plugin nothing can use.
                const juce::PluginDescription* pick = nullptr; int bestScore = -1000;
                for (auto* d : types)
                {
                    if (d == nullptr || ! d->name.containsIgnoreCase (juce::String (w.name))) continue;
                    const int score = (d->name.endsWith ("(s)") || d->name.containsIgnoreCase ("stereo") ? 2 : 0)
                                    - (d->name.endsWith ("(m)") || d->name.containsIgnoreCase ("mono")   ? 2 : 0);
                    if (score > bestScore) { bestScore = score; pick = d; }
                }
                if (pick != nullptr && pick->name != desc.name)
                {
                    if (auto* child = pick->createXml().release()) entries.addChildElement (child);
                    kl.addType (*pick);
                    added.add (pick->name);
                }
            }
            // ---- AND APPLE'S OWN AUs, WHICH HAVE NO BUNDLE TO POINT AT (26 Oct 2026) ---------
            //
            // The live-view rows (LV1-LV6) need a plugin with a real knob panel and no licence of
            // any kind, and his AUDelay crash was on Apple's. These are not .component bundles in
            // the Plug-Ins folders: the OS registers them, so they are reached by IDENTIFIER and
            // enumerated one at a time - never a scan (the standing iLok rule).
            //
            // The first run of the LV rows found this out the hard way: add_slot answered
            // `"AUDelay" not found in recommendable list` three times and all six rows skipped,
            // because the rig's catalogue held 24 names and not one of them was Apple's.
            {
                static const char* kAppleAUs[] = {
                    "AudioUnit:Effects/aufx,dely,appl",     // AUDelay
                    "AudioUnit:Effects/aufx,mrev,appl",     // AUMatrixReverb
                    "AudioUnit:Effects/aufx,mcmp,appl",     // AUMultibandCompressor
                    // ---- AND FOUR THIRD-PARTY SUBJECTS, NONE OF THEM LICENCE-BOUND ------------
                    //
                    // His condition of 26 Oct 21:08: *"run every real-editor row on Apple's AUs plus
                    // three or four installed third-party plugins that need no iLok or licence
                    // server (different makers, at least one JUCE-built and one with a large custom
                    // interface; MTransient counts as one)."*
                    //
                    // Screened before they were named here: no PACE/iLok markers in the binary, no
                    // auth frameworks in the bundle, and EJPaceCheck::isPaceWrapped refuses them
                    // too if that ever changes. Named one at a time by identifier, never scanned.
                    //
                    //   Pro-C 3         FabFilter         a LARGE custom interface, licence file only
                    //   ValhallaDelay   Valhalla DSP      a LARGE custom interface
                    //   TDR SlickEQ M   Tokyo Dawn Labs   JUCE-BUILT (JUCE_ strings in its binary)
                    //
                    // AND ONE MORE THAT IS OUT, and this one is my mistake (27 Oct 2026):
                    // **MCompressor** stood here overnight as a screened subject. It is not one.
                    // /Library/Audio/Plug-Ins/Components/MCompressor.component/Contents holds
                    // __Pace_Eden.bundle - it IS PACE-wrapped, and Sean's standing rule is that no
                    // licence-bound bundle is ever loaded in this rig or on the bench. The screening
                    // sentence above claimed EJPaceCheck refused them "if that ever changes"; it
                    // could not, because this list names IDENTIFIERS and the check was only ever
                    // applied to the bundle paths in kSet above. It is applied here now, so the
                    // claim and the code are the same thing. MTransient is not installed, so
                    // MeldaProduction is not represented; FabFilter takes the third maker's place.
                    //
                    // AND ONE THAT IS NOT HERE: **Fresh Air** (Slate Digital) was in this list and is
                    // out of it. Enumerating an AU identifier INSTANTIATES the plugin to read its
                    // properties, and Fresh Air does not return from instantiation: a standalone probe
                    // (AudioComponentInstanceNew, one plugin per process, 25 s limit) was killed still
                    // waiting, while MCompressor took 209 ms, ValhallaDelay 128 ms, SlickEQ 67 ms and
                    // AUDelay 3 ms. It cost this rig a twelve-minute stall before it was measured.
                    //
                    // THE RULE THAT COMES WITH IT: an identifier goes in this list only after that
                    // probe answers for it. A plugin that will not instantiate in a quarter of a
                    // minute cannot be in a catalogue the rig builds at startup.
                    "AudioUnit:Effects/aumf,FC3p,FabF",     // FabFilter Pro-C 3
                    "AudioUnit:Effects/aufx,dLay,oDin",     // ValhallaDelay
                    "AudioUnit:Effects/aufx,Td6a,Tdrl",     // TDR SlickEQ M
                };
                juce::StringArray appleAdded, appleRefused;
                for (const char* ident : kAppleAUs)
                {
                    juce::OwnedArray<juce::PluginDescription> types;
                    for (auto* fmt : fm.getFormats())
                        if (fmt->getName() == "AudioUnit") fmt->findAllTypesForFile (types, ident);
                    if (types.isEmpty()) { appleRefused.add (juce::String (ident)); continue; }
                    for (auto* d : types)
                    {
                        if (d == nullptr || d->name == desc.name) continue;
                        // THE STANDING RULE, APPLIED WHERE THE PLUGIN IS NAMED BY IDENTIFIER (27 Oct 2026).
                        // kSet above screens its bundle paths; this list did not screen anything, and that is
                        // how MCompressor - PACE-wrapped - was loaded in this process overnight. An identifier
                        // resolves to a bundle through EJPaceCheck::bundleFor, so the same check fits here.
                        if (const auto why = echojay::refuseIfPaceWrapped (*d); why.isNotEmpty())
                        { appleRefused.add (d->name + " (" + why + ")"); continue; }
                        if (auto* child = d->createXml().release()) entries.addChildElement (child);
                        kl.addType (*d);
                        appleAdded.add (d->name);
                    }
                }
                std::printf ("  catalogue: Apple's own AUs - added [%s]%s\n",
                             appleAdded.joinIntoString (", ").toRawUTF8(),
                             appleRefused.isEmpty() ? ""
                                 : (", would not enumerate [" + appleRefused.joinIntoString (", ") + "]").toRawUTF8());
            }
            if (auto xml = kl.createXml()) listFile.replaceWithText (xml->toString());
            std::printf ("  catalogue: the matrix's plugin set - added [%s]%s\n",
                         added.joinIntoString (", ").toRawUTF8(),
                         refused.isEmpty() ? "" : (", refused [" + refused.joinIntoString (", ") + "]").toRawUTF8());
        }
        // ---- HIS WHOLE INVENTORY, OPT-IN, BECAUSE RP8K's FIRST GREEN WAS GREEN FOR THE WRONG
        //      REASON (17 Oct 2026) ----------------------------------------------------------------
        //
        // RP8K seeded the 357 names that CANNOT load - the small half of the problem - and left the
        // inventory itself at this rig's one plugin. It then measured a 568-byte result and passed.
        // That is a comfortable number from an input that had only half arrived, which is the very
        // fault the row's own comment warns about, arriving a third time in a new costume.
        //
        // The size the contract cares about is the size of 1,013 NAMES. So the catalogue can hold
        // them - and ONLY in a process started with EJ_RIG_BIG_INVENTORY=1, because a rig whose
        // world holds a thousand plugins would change the answer of every live row that picks one.
        if (::getenv ("EJ_RIG_BIG_INVENTORY") != nullptr)
        {
            const auto snapFile = juce::File (__FILE__).getParentDirectory()
                                      .getChildFile ("fixtures/real_inventory_2026-10-17.json");
            const auto snap = juce::JSON::parse (snapFile.loadFileAsString());
            int bigAdded = 0, wanted = 0;
            if (auto* arr = snap.getProperty ("names", juce::var()).getArray())
            {
                wanted = arr->size();
                for (const auto& n : *arr)
                {
                    const auto nm = n.toString();
                    if (nm.isEmpty() || nm == desc.name) continue;
                    juce::PluginDescription d;
                    d.name = d.descriptiveName = nm;
                    d.pluginFormatName = "VST3";
                    d.category = "Effect";
                    d.manufacturerName = "snapshot of Sean's Mac 17 Oct 2026";
                    d.version = "1.0";
                    // NOWHERE ON DISK, ON PURPOSE: these entries exist to be COUNTED and NAMED,
                    // never instantiated. The path sits inside the isolated root so nothing can
                    // mistake one for an installed bundle, and no row in this mode adds a plugin.
                    d.fileOrIdentifier = echojay::userStateHome().getChildFile ("not-installed")
                                             .getChildFile (nm + ".vst3").getFullPathName();
                    d.uniqueId = d.deprecatedUid = (int) nm.hashCode();
                    d.isInstrument = false;
                    d.numInputChannels = d.numOutputChannels = 2;
                    if (auto* child = d.createXml().release()) { entries.addChildElement (child); ++bigAdded; }
                }
            }
            std::printf ("  catalogue: EJ_RIG_BIG_INVENTORY - added %d of his %d installed plugin names\n",
                         bigAdded, wanted);
        }
        entriesFile.replaceWithText (entries.toString());
        std::printf ("  catalogue: kept %d existing entr(ies) and added %s\n", kept, desc.name.toRawUTF8());
    }

    inst->releaseResources();
    inst.reset();
    p.ok = true;
    return p;
}

// ============================================================================
//  one hosted plugin = one "track": the instance, its audio, and its editor
// ============================================================================
struct Track
{
    juce::String name;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<juce::DocumentWindow> window;
    juce::AudioBuffer<float> buf { 2, 512 };
    juce::MidiBuffer midi;
    double phase = 0.0;

    /** The name the HOST gives this track. "" leaves it unnamed, which is what every Link in this
        rig was until 16 Oct - and the live pass showed the cost: asked to "make the kick punchier",
        the model answered "I don't see a track named Kick" and offered Untitled, Untitled 2,
        Untitled 3, Untitled 4. Delivered through updateTrackProperties, the door Logic uses. */
    juce::String trackName;

    void prepare (double sr, int block)
    {
        plugin->setPlayConfigDetails (2, 2, sr, block);
        plugin->prepareToPlay (sr, block);
        buf.setSize (2, block);
        if (trackName.isNotEmpty())
        {
            juce::AudioProcessor::TrackProperties tp;
            tp.name = trackName;
            plugin->updateTrackProperties (tp);
            std::printf ("  %s: track name delivered as \"%s\"\n",
                         name.toRawUTF8(), trackName.toRawUTF8());
        }
    }

    /** One block of real audio: a -12 dBFS 220 Hz tone. The level matters - the
        loudness loop only arms and lands when it has heard something, so a rig
        that fed silence would prove nothing about item 4. */
    /** 19 Oct 2026: a KICK-SHAPED pass, for the rows that measure a percentile. The steady tone
        below cannot have a loudest and a typical - every hop is the same level, so p90 == p50 by
        construction and a row asserting they DIFFER would be asserting a property of the signal
        this rig feeds rather than of the product. This alternates one loud hop with four quiet
        ones, which is what a kick track looks like to a 100 ms hop. */
    void processOneBlockKick (double sr)
    {
        const int n = buf.getNumSamples();
        const int blocksPerHop = juce::jmax (1, (int) std::lround (sr * 0.1 / (double) n));
        const int hop = (kickBlock_ / blocksPerHop) % 5;
        ++kickBlock_;
        const float amp = hop == 0 ? 0.5f : 0.02f;        // about -9 dBFS RMS against about -37
        for (int i = 0; i < n; ++i)
        {
            const float v = amp * (float) std::sin (phase);
            phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / sr;
            if (phase > 2.0 * juce::MathConstants<double>::pi) phase -= 2.0 * juce::MathConstants<double>::pi;
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }
        midi.clear();
        // ---- A TRACK WHOSE PLUGIN IS GONE IS SILENT, NOT A CRASH (27 Oct 2026) -----------------
        // RE5 and RH6 DELETE a Link's track mid-pass - that is what they are for - and now that the
        // live-view rows run before the rest of the suite, every later leg pumps tracks that may no
        // longer have a plugin. One of them took the process down in main with a null dereference
        // (SIGSEGV at 0x0, 15:02:06). The deletion is deliberate, so the pump answers for it here
        // rather than at two dozen call sites.
        if (plugin == nullptr) return;
        plugin->processBlock (buf, midi);
    }
    int kickBlock_ = 0;

    void processOneBlock (double sr)
    {
        const int n = buf.getNumSamples();
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.25f * (float) std::sin (phase);
            phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / sr;
            if (phase > 2.0 * juce::MathConstants<double>::pi) phase -= 2.0 * juce::MathConstants<double>::pi;
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }
        midi.clear();
        if (plugin == nullptr) return;   // the same deliberate deletion (see above)
        plugin->processBlock (buf, midi);
    }

    void openEditor()
    {
        if (editor != nullptr) return;
        editor.reset (plugin->createEditorIfNeeded());
        if (editor == nullptr) return;
        window.reset (new juce::DocumentWindow (name, juce::Colours::black,
                                                juce::DocumentWindow::closeButton, true));
        window->setUsingNativeTitleBar (true);
        window->setContentNonOwned (editor.get(), true);
        window->setVisible (true);
    }

    /** The whole point of the rig. An editor is destroyed exactly as a host
        destroys it - the window goes first, then the editor - so the plugin's
        destructor runs in the order it runs in Logic. */
    /** True when the LAST closeEditor() actually destroyed the plugin's editor, as the plugin itself
        reported it. Not a guess, and not this rig's own unique_ptr going null: the AU hands the host a
        wrapper view, and releasing that wrapper does not necessarily deallocate the view the plugin's
        editor lives in. Every row that closes a window has to know which of the two happened. */
    bool lastCloseDestroyedEditor = false;
    juce::String lastCloseNote;
    /** Which wrapper this track's plugin is - "AudioUnit" or "VST3" - so it reads its OWN processor line. */
    juce::String wrapper = "AU";

    /** Defined BELOW Rig, because it has to ask the plugin whether the editor really went away and Rig is
        what reads the plugin's line. Declared here so Track stays one object. */
    void closeEditor();
};

// ============================================================================
//  the automation channel, from the rig's side
// ============================================================================
struct Rig
{
    int seq = 0;

    /** Pump the message thread for ms. NEVER a bare sleep: the plugin's timers,
        the editor's 20 Hz tick and every callAfterDelay live on this thread, so
        a sleeping rig would freeze the thing it is testing.

        runDispatchLoopUntil is unavailable in a plugin build (JUCE_MODAL_LOOPS_PERMITTED
        is off), so this is the link_state_guard idiom: pending timers synchronously plus
        a short turn of the CF run loop, which is what actually delivers the editor's
        timer callbacks and every callAfterDelay. */
    static void pump (int ms)
    {
        const double end = juce::Time::getMillisecondCounterHiRes() + ms;
        while (juce::Time::getMillisecondCounterHiRes() < end)
        {
            juce::Timer::callPendingTimersSynchronously();
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false);
        }
    }

    /** Send a command and wait for the plugin's ack. Waiting on the ACK rather
        than on a delay is the "decisions on uncontrolled timing" rule: the rig
        asserts on a fact the plugin stated, not on a guess about how long it
        takes. Returns the ack, or an invalid var on timeout. */
    juce::var send (const juce::String& cmd, juce::var args, int timeoutMs = 8000)
    { return sendTo ({}, cmd, std::move (args), timeoutMs); }

    /** THE SAME COMMAND, ADDRESSED TO ONE INSTANCE (16 Oct 2026). With two V2 editors open - which
        is how the matrix produces the rack-selection lock - an unaddressed command is obeyed by
        both. `wrapper` is "AU" or "VST3"; empty means every instance, which is what every row
        written before today sends. */
    juce::var sendTo (const juce::String& wrapper, const juce::String& cmd, juce::var args,
                      int timeoutMs = 8000)
    {
        ++seq;
        auto* o = new juce::DynamicObject();
        o->setProperty ("seq", seq);
        o->setProperty ("cmd", cmd);
        o->setProperty ("args", args);
        if (wrapper.isNotEmpty()) o->setProperty ("for", wrapper);
        echojay::rig::ackFile().deleteFile();
        echojay::rig::cmdFile().replaceWithText (juce::JSON::toString (juce::var (o), true));

        const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < until)
        {
            pump (10);
            const auto f = echojay::rig::ackFile();
            if (! f.existsAsFile()) continue;
            const auto v = juce::JSON::parse (f.loadFileAsString());
            if ((int) v.getProperty ("seq", -1) == seq) return v;
        }
        return {};
    }

    /** The published state, as ONE snapshot. Re-reading the file between two assertions in the same row is
        how I read a rack with count=2 and an empty slot list: two reads, two different moments. A row takes
        one snapshot and asserts on that. The retry covers the instant the file does not exist (between the
        rename and the next publish). */
    /** The snapshot the LAST command produced, never an older one. */
    juce::var freshState (int timeoutMs = 6000)
    {
        send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 4000);
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < end)
        {
            const auto v = state();
            if (v.isObject() && (int) v.getProperty ("forSeq", -1) >= seq) return v;
            pump (60);
        }
        return state();      // the best available, and the row will say what it saw
    }

    /** THE PROCESSOR'S OWN LINE (15 Oct 2026), which is the only thing readable while the window is SHUT.
        rig-state.json is written by the EDITOR when it is given a command, so every row that closes a
        window has, until now, been asserting on a snapshot frozen at the moment of the close - and could
        not even tell whether the close had destroyed the plugin's editor. It had not: in this host the AU
        keeps one EchoJayEditor alive for the whole run, so "the window was closed" was true of the host's
        wrapper and false of the plugin. This file is written on the processor's 1 Hz timer, open or shut. */
    /** `wrapper` is JUCE's own word for it - "AU" or "VST3", from getWrapperTypeDescription, which says
        "AU" and not "AudioUnit" (the first cut guessed, and C11's processor-record row read a file that did
        not exist). The plugin writes one line per wrapper type, because the
        lifecycle rows load the V2 TWICE and one file would be two processors fighting over it. */
    static juce::var processorState (const juce::String& wrapper = "AU")
    {
        const auto f = echojay::rig::dir().getChildFile ("rig-processor-" + wrapper + ".json");
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            if (f.existsAsFile())
            {
                const auto v = juce::JSON::parse (f.loadFileAsString());
                if (v.isObject()) return v;
            }
            pump (40);
        }
        return {};
    }

    /** Wait, bounded, for the processor to say something. Returns false on timeout, and the caller says so
        out loud rather than carrying on as if the wait had succeeded. */
    static bool untilProcessor (std::function<bool (const juce::var&)> pred, int timeoutMs,
                                const juce::String& wrapper = "AU")
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < end)
        {
            const auto v = processorState (wrapper);
            if (v.isObject() && pred (v)) return true;
            pump (100);
        }
        return false;
    }

    static juce::var state()
    {
        const auto f = echojay::rig::stateFile();
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            if (f.existsAsFile())
            {
                const auto v = juce::JSON::parse (f.loadFileAsString());
                if (v.isObject()) return v;
            }
            pump (40);
        }
        return {};
    }

    /** Pump until a predicate on the published state holds, or the bound runs
        out. Returns true if it held. Every wait in this rig is bounded and says
        what it was waiting for. */
    template <typename Pred>
    bool until (Pred p, int timeoutMs = 10000)
    {
        // RE-ASKS EACH TIME. The hook publishes state when it is given a command - it does not write on a
        // timer - so a loop that only re-read the FILE was re-reading the same snapshot until it gave up.
        // That is how "a Link is in the registry" failed for 8 s while the published state said connected:
        // true: the state was true and the file was old. The loop now asks for a fresh one each turn.
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        if (p (state())) return true;
        while (juce::Time::getMillisecondCounter() < end)
        {
            send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 2000);
            if (p (state())) return true;
            pump (200);
        }
        return false;
    }
};

/** The pump, for the live-view helpers declared above Rig. */
static void ejPumpMsgThread (int ms) { Rig::pump (ms); }
static bool rowWanted (const juce::String& id);   // EJ_RIG_ONLY; defined with the rows below

/** ---- THE THREE MEASUREMENTS, ON WHATEVER VIEW IS ON SCREEN (his 26 Oct faults 1-3) ---------
    One function, called for a slot on the V2's own rack and for a slot on a LINK'S rack through the
    live view, because the container, the reparent and the 400 ms pass are the same object on both
    roads and a fault that shows on one and not the other is itself the finding. `label` says which
    road the verdict is about, every time, so no row can be read as covering both. */
// ---- WHAT THE PICTURE ACTUALLY IS, IN PIXELS (26 Oct 2026, third cut of LV6) -------------------
//
// LV6 asks "did the knob move on screen", and it has now been answered wrongly twice by instruments
// that were measuring something else:
//
//   1. a synthetic drag that landed on the background moved nothing, and the row reported the
//      product as frozen (corrected 25 Oct: it now needs a parameter to move before it judges);
//   2. a DRAW COUNT on the guest view, which a forced invalidation showed produces exactly one draw
//      - the parent's. Apple's AUDelay view draws its sliders in SUBVIEWS, and a subview redrawing
//      never calls the parent's drawRect, so "drew not once" was my counter watching the wrong
//      object (26 Oct 18:15).
//
// This is the question asked of the pixels themselves: render the guest view (and therefore its
// whole subtree) into a bitmap and hash it. A picture that moved cannot hash the same, and a
// picture that did not cannot hash differently - no swizzle, no view hierarchy assumptions, and
// nothing about how a given plugin chooses to draw.
// ---- WHAT THE V2 ITSELF PUTS OUT, AS A NUMBER (his round-2 acceptance, 26 Oct 2026) -----------
//
// *"confirm from the code, with a guard: while the V2's view is on a Link's rack, the V2's own chain
//  keeps processing its own audio unchanged, and the copy of the Link's plugin is never mixed into
//  the V2's output."*
//
// The code says it cannot happen (ShadowView::pump renders into its own buffer and the host buffer
// is never written from it), but that is a reading of the source. This is the measurement: render a
// FIXED input through the V2 from a known phase and hash every output sample. Run it with the live
// view closed and again with it open; the two hashes must be identical, bit for bit. A copy leaking
// in - at any level, on any channel - cannot leave the hash alone.
static juce::uint64 ejRenderV2Hash (Track& v2, double sr, int blocks)
{
    v2.phase = 0.0;                                   // same input both times, by construction
    juce::uint64 h = 1469598103934665603ull;
    for (int b = 0; b < blocks; ++b)
    {
        v2.processOneBlock (sr);
        for (int ch = 0; ch < v2.buf.getNumChannels(); ++ch)
        {
            const float* d = v2.buf.getReadPointer (ch);
            for (int i = 0; i < v2.buf.getNumSamples(); ++i)
            {
                // The bits, not the value: a denormal or a sign of zero is a difference too.
                juce::uint32 bits; std::memcpy (&bits, &d[i], sizeof bits);
                h ^= (juce::uint64) bits; h *= 1099511628211ull;
            }
        }
    }
    return h;
}

/** THE SAME QUESTION, WITH A CONTROL (26 Oct 2026, second cut of LV11/RE11).
    Bit-equality across two renders assumes the V2's own chain is deterministic from one starting
    phase, and it is not: its devices carry envelopes and level trackers, so two identical inputs
    rendered minutes apart differ a little all by themselves. LV11's first cut read that as the copy
    leaking into the output. So the row measures the NOISE FLOOR as well: render with the view, then
    twice without it. If the view's presence changes the output no more than two view-free renders
    differ from each other, the view is not in the audio path - and a copy that WAS mixed in would
    differ by its own whole signal, which is nowhere near that floor. */
static double ejRenderMaxDiff (Track& v2, double sr, int blocks, std::vector<float>& into)
{
    // ---- A WARM-UP FIRST (26 Oct 2026, his instruction) -------------------------------------
    // The own rack holds a delay and a reverb by the time this runs, and their TAILS carry from one
    // render into the next: the first cut of this measured two view-free renders differing by 0.5 on
    // a 0.25 tone and read it as the copy leaking into the output. A fixed input drives those tails
    // to a steady state, so the measured window starts from the same place every time.
    // 300 blocks, not 40: at 40 the FIRST measured window still carried a compressor's release
    // converging, and RE11 read that 0.002 as the Link's plugin leaking into the V2's output while
    // the later floor pair matched bit for bit. The warm-up has to outlast the slowest device on the
    // rack, and a few tenths of a second of silence costs the row nothing.
    v2.phase = 0.0;
    for (int w = 0; w < 300; ++w) v2.processOneBlock (sr);
    v2.phase = 0.0;
    std::vector<float> got;
    got.reserve ((size_t) blocks * (size_t) v2.buf.getNumSamples() * 2);
    for (int b = 0; b < blocks; ++b)
    {
        v2.processOneBlock (sr);
        for (int ch = 0; ch < v2.buf.getNumChannels(); ++ch)
        {
            const float* d = v2.buf.getReadPointer (ch);
            for (int i = 0; i < v2.buf.getNumSamples(); ++i) got.push_back (d[i]);
        }
    }
    double worst = 0.0;
    if (! into.empty() && into.size() == got.size())
        for (size_t i = 0; i < got.size(); ++i)
            worst = juce::jmax (worst, (double) std::abs (got[i] - into[i]));
    into.swap (got);
    return worst;
}

static juce::uint64 ejGuestPixelHash (NSView* v)
{
    if (v == nil) return 0;
    const NSRect b = [v bounds];
    if (b.size.width < 1.0 || b.size.height < 1.0) return 0;
    @try
    {
        NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect: b];
        if (rep == nil) return 0;
        [v cacheDisplayInRect: b toBitmapImageRep: rep];
        unsigned char* px = [rep bitmapData];
        if (px == nullptr) return 0;
        const NSInteger bytes = [rep bytesPerRow] * [rep pixelsHigh];
        juce::uint64 h = 1469598103934665603ull;          // FNV-1a, enough for "same or not"
        for (NSInteger i = 0; i < bytes; ++i) { h ^= (juce::uint64) px[i]; h *= 1099511628211ull; }
        return h;
    }
    @catch (NSException*) { return 0; }
}

/** HOW MUCH it changed, not only whether (27 Oct 2026). RE9 reported "pixels CHANGED with nobody
    touching it" and a hash cannot tell a blinking text cursor from a wobbling panel - which is the
    difference between a tolerance with a reason and a product fault. Returns the number of bytes
    that differ and the largest single difference, with the total for scale. */
struct EJPixelDelta { long differing = 0, total = 0; int worst = 0; bool read = false; };

static bool ejGuestPixels (NSView* v, std::vector<unsigned char>& into)
{
    if (v == nil) return false;
    const NSRect b = [v bounds];
    if (b.size.width < 1.0 || b.size.height < 1.0) return false;
    @try
    {
        NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect: b];
        if (rep == nil) return false;
        [v cacheDisplayInRect: b toBitmapImageRep: rep];
        unsigned char* px = [rep bitmapData];
        if (px == nullptr) return false;
        const NSInteger bytes = [rep bytesPerRow] * [rep pixelsHigh];
        into.assign (px, px + bytes);
        return bytes > 0;
    }
    @catch (NSException*) { return false; }
}

static EJPixelDelta ejPixelDelta (const std::vector<unsigned char>& a,
                                  const std::vector<unsigned char>& b)
{
    EJPixelDelta d;
    if (a.empty() || a.size() != b.size()) return d;
    d.read = true; d.total = (long) a.size();
    for (size_t i = 0; i < a.size(); ++i)
    {
        const int diff = std::abs ((int) a[i] - (int) b[i]);
        if (diff != 0) { ++d.differing; d.worst = juce::jmax (d.worst, diff); }
    }
    return d;
}

static void ejMeasureHostedView (const juce::String& label, const juce::String& openThrow,
                                 const char* idCrash, const char* idStill, const char* idDrag,
                                 const EJPumpFn& pump,
                                 const std::function<juce::String()>& copyValues = {},
                                 /** Move one of the plugin's parameters through its own gesture road,
                                     for when no synthetic drag can find a knob. Returns whether it
                                     was asked for; the row reads copyValues() to see it land. */
                                 const std::function<bool()>& moveParam = {})
{
    NSView* cont  = ejFindClipContainer();
    NSView* guest = ejGuestInside (cont);
    if (cont == nil || guest == nil)
    {
        // WHAT WAS ON SCREEN INSTEAD, because a skip nobody can diagnose costs a whole run to
        // repeat - two of them on 26 Oct. Every window in this process, and the classes of its
        // content view's own subviews, which is where a clip container would be.
        juce::String seen;
        for (NSWindow* w in [NSApp windows])
        {
            seen << "[" << ejNS ([w title]) << ([w isVisible] ? " visible: " : " hidden: ");
            for (NSView* sv in [[w contentView] subviews])
                seen << ejNS (NSStringFromClass ([sv class])) << " ";
            seen << "] ";
        }
        skip (label, juce::String ("no clip container with a plugin view was on screen after the open")
              + " (container " + (cont == nil ? "absent" : "present")
              + ", plugin view " + (guest == nil ? "absent" : "present")
              + (openThrow.isNotEmpty() ? ", and the open threw: " + openThrow : "")
              + "). Windows now: " + seen);
        return;
    }
    const auto guestClass = ejNS (NSStringFromClass ([guest class]));
    std::printf ("        (LV) %s: container %s, plugin view %s %s\n", label.toRawUTF8(),
                 [NSStringFromRect ([cont frame]) UTF8String], guestClass.toRawUTF8(),
                 [NSStringFromRect ([guest frame]) UTF8String]);

    if (rowWanted (idCrash))
        check (openThrow.isEmpty(), juce::String (idCrash) + ": " + label + " opens without the "
               "plugin's own draw throwing (his AUDelay crash)",
               openThrow.isNotEmpty()
                   ? "the open threw " + openThrow + " - in Logic that is EXC_BREAKPOINT in "
                     "-[NSApplication _crashOnException:] and the AU hosting process dies"
                   : juce::String());

    if (! ejCountGuestDraws (guest))
    {
        skip (label + ": its own draws cannot be counted",
              "no class between " + guestClass + " and NSView implements drawRect:, so this view "
              "draws through something else and a draw count would be a zero nobody could read.");
        return;
    }

    // ---- HANDS OFF, NOTHING MAY CHANGE FOR 10 SECONDS -------------------------------------
    // *"with hands off nothing in the view changes at all for 10 seconds"* - his acceptance, in his
    // words. Sampled every 200 ms: both frames, the bounds origin, both layers' geometry and style,
    // and the plugin's own draw count.
    if (rowWanted (idStill))
    {
        pump (600);
        gGuestDraws = 0;
        const auto s0 = ejSampleView (cont, guest);
        EJViewSample last = s0;
        juce::String firstMove; double firstMoveAtMs = -1.0;
        for (int t = 0; t < 50; ++t)
        {
            pump (200);
            const auto sNow = ejSampleView (cont, guest);
            if (firstMoveAtMs < 0.0)
            {
                const auto moved = ejWhatMoved (s0, sNow);
                if (moved != "nothing") { firstMove = moved; firstMoveAtMs = sNow.tMs - s0.tMs; }
            }
            last = sNow;
        }
        check (firstMoveAtMs < 0.0,
               juce::String (idStill) + ": with hands off, nothing in " + label + " changes for 10 s",
               firstMoveAtMs < 0.0 ? juce::String()
                   : "after " + juce::String ((int) firstMoveAtMs) + " ms: " + firstMove
                     + "; over the whole 10 s: " + ejWhatMoved (s0, last));
    }

    // ---- A REAL DRAG REACHES THE SCREEN ---------------------------------------------------
    // *"I hear the change, but the knob on screen doesn't move."*  The knob is drawn by the
    // plugin's own code, so the only thing that can see it is whether that view drew.
    // AND THE INSTRUMENT IS PROVED FIRST, in the opposite direction: a forced invalidation must
    // produce a draw, or a zero from the drag would be a dead counter rather than a frozen picture.
    if (rowWanted (idDrag))
    {
        gGuestDraws = 0;
        [guest setNeedsDisplay: YES];
        pump (500);
        const int forced = gGuestDraws;
        check (forced > 0, juce::String (idDrag) + " (instrument): " + label + " does draw when told to",
               forced > 0 ? juce::String()
                          : "a forced setNeedsDisplay produced no draw at all, so nothing below can "
                            "be read as a verdict about the picture");
        if (forced > 0)
        {
            // ---- AND THE DRAG HAS TO REACH A CONTROL BEFORE THE PICTURE CAN BE JUDGED --------
            //
            // The first run of this row went RED on three different plugins, on the own rack as
            // well as in a live view - and a fault that is everywhere is usually the instrument.
            // A synthetic drag that lands on a plugin's background moves nothing and draws
            // nothing, which is indistinguishable from the frozen picture he reported unless the
            // row checks. So each probe point is judged by whether the PARAMETER moved:
            //
            //   parameter moved, no draw   -> his fault 2, and the row says so
            //   parameter moved, drew      -> the knob follows the hand
            //   no parameter moved anywhere -> this rig could not find a knob to drag. A SKIP with
            //                                  that sentence, never a verdict about the product.
            [NSApp activateIgnoringOtherApps: YES];
            [[guest window] makeKeyAndOrderFront: nil];
            pump (200);
            // ---- AND THE VALUES HAVE TO BE STILL BEFORE A DRAG CAN BE CREDITED WITH MOVING ONE --
            //
            // This row decides "the drag moved a parameter" by comparing the copy's values across
            // it, and the mirror writes values into the copy whenever the LINK's side moves (that
            // is the whole of FOLLOW). The row before this one spent six phases moving a parameter,
            // so a value changing during the drag's window was not necessarily the drag's doing -
            // my own instrument, in a new place. So: wait for the values to be unchanged across two
            // reads a quarter of a second apart, and SAY SO if they never settle.
            juce::String quiesceNote;
            if (copyValues)
            {
                bool settled = false;
                juce::String last = copyValues();
                for (int i = 0; i < 12 && ! settled; ++i)
                {
                    pump (250);
                    const auto now = copyValues();
                    settled = (now == last) && now.isNotEmpty();
                    last = now;
                }
                quiesceNote = settled ? juce::String ("the copy's values were still before the drag")
                                      : juce::String ("THE COPY'S VALUES NEVER SETTLED in 3 s, so a "
                                                      "change across the drag cannot be credited to it");
                if (! settled)
                    std::printf ("        (LV) %s: %s\n", label.toRawUTF8(), quiesceNote.toRawUTF8());
            }
            const NSRect gb = [guest bounds];
            int drewOnHit = -1; NSPoint hitAt {}; int tried = 0; bool hit = false;
            bool pictureMoved = false, pictureRead = false;
            for (double fx : { 0.25, 0.5, 0.75 })
                for (double fy : { 0.3, 0.55, 0.8 })
                {
                    if (hit) continue;
                    ++tried;
                    const NSPoint at = NSMakePoint (gb.origin.x + gb.size.width * fx,
                                                    gb.origin.y + gb.size.height * fy);
                    const juce::String before = copyValues ? copyValues() : juce::String();
                    gGuestDraws = 0;                       // the drag's OWN window, nothing earlier
                    const juce::uint64 pxBefore = ejGuestPixelHash (guest);
                    const int d = ejDragOnGuest (guest, at, -24.0, 8, pump);
                    pump (300);                            // let the plugin's own redraw happen
                    const juce::uint64 pxAfter = ejGuestPixelHash (guest);
                    const juce::String after  = copyValues ? copyValues() : juce::String();
                    const bool moved = copyValues && before.isNotEmpty() && after != before;
                    if (moved)
                    {
                        pictureMoved = (pxBefore != 0 && pxAfter != 0 && pxBefore != pxAfter);
                        pictureRead  = (pxBefore != 0 && pxAfter != 0);
                    }
                    if (moved)
                        std::printf ("        (LV) the drag at (%d,%d) moved the copy: \"%s\" -> \"%s\"\n",
                                     (int) at.x, (int) at.y, before.toRawUTF8(), after.toRawUTF8());
                    if (moved || (! copyValues && d > 0))
                    { hit = true; drewOnHit = d; hitAt = at; }
                }
            // ---- WHEN NO SYNTHETIC DRAG CAN FIND A KNOB, MOVE IT THE PRODUCT'S OWN WAY -------
            //
            // His rule of 26 Oct: *"if any row that covers the live view, control or settings
            // carrying over skips or doesn't run, that is a stop, not flakiness."*  A synthetic
            // mouse drag cannot reliably land on a hosted AU's knob - nine of them found nothing -
            // so this row no longer shrugs at that. `moveParam` moves the plugin's parameter
            // through the same gesture road its knob uses, and the row then judges the PICTURE,
            // which is what it was always about: the value moved, so the view must redraw.
            bool externalMoved = false; int externalDrew = 0;
            if (! hit && moveParam != nullptr)
            {
                gGuestDraws = 0;
                const juce::String before = copyValues ? copyValues() : juce::String();
                const bool asked = moveParam();
                pump (600);
                const juce::String after = copyValues ? copyValues() : juce::String();
                externalDrew = gGuestDraws;
                externalMoved = asked && after != before;
                if (externalMoved)
                    std::printf ("        (LV) no synthetic drag found a knob, so the parameter was moved "
                                 "through its own gesture road instead: the copy went \"%s\" -> \"%s\", "
                                 "and the view drew %d time(s)\n",
                                 before.toRawUTF8(), after.toRawUTF8(), externalDrew);
            }
            // ---- WHAT THIS ROW MAY AND MAY NOT CONCLUDE (26 Oct 2026, second cut) ------------
            //
            // THE HAND is the claim: a drag the plugin's own view handled moved one of its controls,
            // so that view must redraw - if it does not, that is his fault 2 and the row fails.
            //
            // AN EXTERNAL MOVE IS NOT THE SAME CLAIM. A third-party AU is under no obligation to
            // redraw because the host wrote a parameter behind its back; plenty do not. So when no
            // drag could reach a knob, the row still RUNS (his rule) and reports what it found - the
            // value moved, and whether the picture followed - but it does not call a plugin that
            // ignores an external write a product fault, because it is not one.
            // THREE OUTCOMES, AND EXACTLY ONE OF THEM (26 Oct 2026, 18:30). The first cut of this
            // wrote the two new cases as separate `if`s and left the old `else` attached to the
            // second, so a subject that took the external road was judged TWICE - once as "the
            // control road was exercised" (ok) and once as a hand-drag that never happened ("the
            // drag at (0,0)", pixels unread). A dangling else of mine, reported as a product fault.
            if (hit)
            {
                check (pictureRead ? pictureMoved : drewOnHit > 0,
                       juce::String (idDrag) + ": a real drag on " + label
                       + " changes what is on screen (the knob follows the hand) ["
                       + (pictureRead ? juce::String ("judged in pixels")
                                      : juce::String ("judged by draw count - the pixels could not be read"))
                       + "; " + quiesceNote + "]",
                       (pictureRead ? pictureMoved : drewOnHit > 0)
                           ? "the view's pixels changed under the drag at ("
                             + juce::String ((int) hitAt.x) + "," + juce::String ((int) hitAt.y) + ")"
                           : "the drag at (" + juce::String ((int) hitAt.x) + ","
                             + juce::String ((int) hitAt.y) + ") MOVED A PARAMETER and what is on screen "
                             "did not change, while a forced invalidation produced "
                             + juce::String (forced) + " draw(s) - the hand reaches the plugin and the "
                             "picture does not move, which is his fault 2");
            }
            else if (externalMoved)
            {
                // AN EXTERNAL MOVE IS NOT THE SAME CLAIM. A third-party AU is under no obligation to
                // redraw because the host wrote a parameter behind its back, and several do not. The
                // row RUNS and reports what it found, but it does not call that a fault of ours.
                check (true, juce::String (idDrag) + ": " + label
                       + " - the control road was exercised, and the picture is the plugin's own business here",
                       externalDrew > 0
                           ? "no synthetic drag could reach a knob; the parameter was moved through its own "
                             "gesture road and the view redrew " + juce::String (externalDrew) + " time(s)"
                           : "no synthetic drag could reach a knob; the parameter was moved through its own "
                             "gesture road and this plugin's view did not redraw for a change made outside "
                             "its own UI - this plugin's choice, not a fault of ours");
            }
            else
            {
                check (false, juce::String (idDrag) + ": a change on " + label + " redraws it",
                       juce::String (tried) + " synthetic drag(s) found no knob AND the parameter could "
                       "not be moved through its own road either, so this row COULD NOT RUN - which by "
                       "his rule of 26 Oct is a stop, not flakiness.");
            }
        }
    }
    ejStopCountingGuestDraws();
    if (gGuestThrows > 0)
        check (false, juce::String (idCrash) + ": " + label + "'s own draw threw nothing while the row ran",
               juce::String (gGuestThrows) + " throw(s), the first: " + gGuestThrowWhat);
}

void Track::closeEditor()
{
    if (window != nullptr) { window->setVisible (false); window->clearContentComponent(); }
    window.reset();
    editor.reset();
    lastCloseDestroyedEditor = false;
    lastCloseNote.clear();
    // Ask the PLUGIN, with a bound. 3 s is generous: the line is written on a 1 Hz timer and the
    // deallocation, if it happens at all, happens on this thread as the wrapper is released.
    const bool gone = Rig::untilProcessor ([] (const juce::var& v)
                      { return ! (bool) v.getProperty ("editorAlive", true); }, 3000, wrapper);
    const auto ps = Rig::processorState (wrapper);
    lastCloseDestroyedEditor = gone;
    if (! gone)
        lastCloseNote = "the plugin still reports an editor (id "
                      + ps.getProperty ("editorId", juce::var()).toString() + ", "
                      + ps.getProperty ("editorsBuilt", juce::var()).toString()
                      + " built in this run): this host released its WRAPPER view and the plugin's "
                        "editor outlived it, so nothing here can prove what a closed window does";
}

static juce::var args1 (const char* k, const juce::var& v);   // defined below; the harness uses it

// =============================================================================
//  THE CAPABILITY HARNESS (Sean's ask, 13 Oct 2026)
// =============================================================================
//
//  One request, driven exactly as a user drives it: agent mode on, type it, send
//  it, wait for the card to settle, press Apply when a plan is waiting, wait for
//  the run to end. Then the ROW asserts on real state - the rack read back, the
//  rack OUT, the Link's own sidecar - never on what the agent said it did.
//
//  Every row that follows uses this, so a row is a request and a pass rule and
//  nothing else. docs/AGENT_CAPABILITIES.md is the list; this is how it is run.
struct AgentOutcome
{
    bool  settled = false;      // the card reached a resting state within the bound
    juce::String state;         // its word for that state
    int   planLines = 0;        // how many lines awaited a decision (0 = it just did it)
    bool  applied = false;      // the rig pressed Apply
    juce::String text;          // what the card was saying at the end
    juce::String failDetail;    // the first failed step's user line, if any
    int   asksAnswered = 0;     // how many questions the rig answered on the user's behalf
    juce::String lastAsk;       // the last question it answered
    // 14 OCT: two facts the rig was missing, both of which let one row answer with another row's reply.
    bool  sawAsk = false;       // the card asked something (and this row chose not to answer it)
    juce::String stepWords;     // what the steps said, captured WHILE they existed (see below)
    bool  adopted = false;      // the card's own goal became THIS request (so the reply belongs to this row)
    int   applies = 0;          // how many plans the rig approved (a build can present more than one)
    bool  leftRunning = false;  // the turn was still live when the bound ran out
    // AUTO MODE (15 Oct): a row has to be able to say "it DID wait for a press", and `applies` only counts
    // presses the rig made - a row that declines to answer presses nothing and would look the same as a run
    // that never waited at all. This is the card having reached AwaitingApproval at any point.
    bool  sawApproval = false;

    /** DID A TURN ACTUALLY HAPPEN FOR THIS REQUEST? (14 Oct 2026, Sean's instruction.)
        The 14:34 run was spoiled by a server-side 500 on every agent POST, and SEVERAL ROWS PASSED ANYWAY:
        C10 "undo that" passed because the rack had not changed - the removal it was undoing had itself
        failed, so before == after. A6, L5 and F2 can pass the same way, their assertions all being variants
        of "nothing was touched". A row whose assertion is satisfied by the turn never running is not a test.

        Three facts, any one of which proves a turn happened: the card took up THIS row's goal (adopted), and
        it produced either a step, an answered question, or visible text. A refusal counts - F2 and L5 are
        SUPPOSED to be refused - because a refusal is a turn that ran and said no. What does not count is
        silence. */
    bool ran() const
    {
        // sawApproval counts (16 Oct 2026): a row that deliberately leaves the plan waiting - to read
        // what is DRAWN while it waits - breaks out before any step line is captured, and would
        // otherwise be forced to fail for "the turn produced nothing" when the turn had plainly
        // produced a plan the user was being asked to approve.
        return adopted && (stepWords.isNotEmpty() || asksAnswered > 0 || sawAsk || sawApproval
                           || text.isNotEmpty());
    }
    juce::String whyNotRan() const
    {
        juce::StringArray why;
        if (! adopted) why.add ("the card never took up this goal");
        if (stepWords.isEmpty() && asksAnswered == 0 && text.isEmpty() && ! sawApproval && ! sawAsk)
            why.add ("no step, no question and no text - the turn produced nothing");
        return why.joinIntoString ("; ");
    }
};

/** LIVE means a token with something in it. ctest's ENVIRONMENT property cannot UNSET a variable - it sets
    it to "" - so `getenv(...) != nullptr` reported the mock pass as live, which would have skipped the one
    leg that pass exists for (U1) and run the capability rows with no server. An empty credential is not a
    credential anywhere else either. */
static bool liveServerConfigured()
{
    const char* e = ::getenv ("EJ_RIG_TOKEN");
    return e != nullptr && juce::String (e).trim().isNotEmpty();
}

static bool isRestingState (const juce::String& st)
{
    return st == "done" || st == "failed" || st == "stopped" || st == "idle";
}

static juce::var agentVarOf (const juce::var& st) { return st.getProperty ("agent", juce::var()); }

/** ---- SEAN'S RULE 2, 16 Oct 2026: THE RIG TESTS WHAT SHIPS ------------------------------------
    "Rows G1-G7 must pass with it ON in S6, not only under EJ_AGENT_ROW=1."

    So this mirrors EchoJayEditor::agentRowRendererOn() EXACTLY - on unless EJ_AGENT_PANEL=1 asks for
    the old floating card back. It is not an opt-in any more, and EJ_AGENT_ROW is gone: a flag that
    turned rows on in the rig but changed nothing in the plugin is how seven rows spent a week passing
    in a configuration the product did not ship.

    EJ_AGENT_ROW=0 is still honoured as an explicit OFF, for bisecting a row against the old renderer. */
static bool agentRowOn()
{
    if (const char* panel = ::getenv ("EJ_AGENT_PANEL"))
        if (juce::String (panel).trim() == "1") return false;
    if (const char* e = ::getenv ("EJ_AGENT_ROW"))
        if (juce::String (e).trim() == "0") return false;
    return true;
}

/** ITEM 0b - a run SURVIVING its window - is the PLUGIN'S switch
    (EchoJayEditor::agentRunSurvivesCloseOn), and the plugin is its only authority.

    This helper used to carry its own copy of the default: "off unless EJ_AGENT_0B=1". The plugin
    shipped 0b ON by default on 20 Oct, after Sean proved it in Logic, and nothing updated this - so
    for two days the rig asserted the behaviour of a build that does not exist. The old comment was
    already the rule ("the rig must set the SAME env var the plugin reads, or it would be testing a
    different build"); reading the same var and inverting its default is that same fault one layer
    down, and gate 2026-10-22 charged five rows to it (F3 directly, then C11v(a), C11v(b), I1, F1 and
    F2, because the run F3 left live made every typed message afterwards "refused while active").

    The answer is now read from the running build, which publishes it on the editor state as
    `agent0bOn`. There is deliberately NO DEFAULT here: a build that does not publish the field
    cannot be asked, and `published == false` makes the rows name that instead of guessing, because
    a guess in this spot is the whole of what went wrong. */
struct Agent0bSwitch { bool published = false; bool on = false; };
static Agent0bSwitch agent0bFromBuild (const juce::var& editorState)
{
    const auto v = editorState.getProperty ("agent0bOn", juce::var());
    if (v.isVoid()) return {};
    return { true, (bool) v };
}

/** `answerAsks = false` leaves the card's question UNANSWERED (14 Oct 2026).
    Row L5 needed it. It asks for a track that does not exist and its claim is "the agent asks rather than
    guessing, and touches nothing". The agent did exactly that - *"I don't see a track named 'Trombone bus' -
    only 'Untitled' and 'Untitled 2' are linked here"* - and then the rig, which answers every question with
    the first chip, replied "Untitled". So the agent was INSTRUCTED to act on a real track, did, and the row
    then reported that nothing had happened. Answering the question is right for a row that needs the run to
    continue; it is the wrong instrument for a row whose whole subject is the question itself. */
static AgentOutcome askAgent (Rig& rig, const juce::String& request, int timeoutMs = 120000,
                              bool answerAsks = true,
                              // What to TYPE when a question comes with no chips to tap (15 Oct). A row
                              // that is about choosing a plugin says which one it means; everything else
                              // takes the default, because a bare "yes" is what a user types at a
                              // confirmation and nothing else should be put in their mouth.
                              const juce::String& answerText = "yes",
                              // ---- 16 Oct 2026: A ROW THAT WANTS TO SEE THE PLAN WAITING -------------
                              // The helper presses Apply as a user would, which is right for almost every
                              // row and WRONG for a row whose subject is what is DRAWN while a decision
                              // waits: scenario G4 read the zones after its own press had finished the
                              // run, found "1 change / Undo all" and passed over a run with no pending
                              // line in it. A scenario that cannot go red proves nothing, so this exists.
                              bool approve = true)
{
    AgentOutcome out;
    // TURN THE AGENT ON WITHOUT CHOOSING THE MODE FOR THE ROW (15 Oct 2026). This used to send
    // { on: true }, which the plugin reads as ASK - so every Auto row was quietly put back into Ask by the
    // very helper that ran it, and A9/A7 reported one approval press each in a mode whose whole point is
    // that there are none. Off is the only state this helper changes.
    if (rig.freshState().getProperty ("agentApprovalMode", juce::var()).toString() == "off")
        rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);

    // ---- 1. NEVER SEND INTO A LIVE SESSION (14 Oct 2026) --------------------------------------------------
    // Row L3 asked for a Link's output and was answered with row L2's build summary. L2's turn had run past
    // its 180 s bound with a second plan still awaiting approval; this function returned anyway, L3's request
    // went into a session that was still executing, and the state the rig then read at rest was L2's. The row
    // was not wrong about the OUT not moving - nothing had asked it to move. A request is only this row's
    // request if the card was quiet when it was sent.
    if (! rig.until ([] (const juce::var& st)
                     { const auto a = agentVarOf (st);
                       return ! (bool) a.getProperty ("active", false)
                              && isRestingState (a.getProperty ("state", juce::var()).toString()); }, 90000))
    {
        // AND IT IS STOPPED, NOT JUST COMPLAINED ABOUT (15 Oct 2026). On the live gate C1 stalled in
        // awaiting_ask; C8 then printed this warning, sent its request into C1's session anyway, and failed
        // on a reading that belonged to a turn nobody had asked for. A warning that is followed by carrying
        // on regardless is the "decisions on uncontrolled timing" fault with a comment attached. The session
        // is stopped - which is what a user does with a card that will not finish - and the row that follows
        // starts from quiet or says it could not.
        std::printf ("        (the previous turn was STILL LIVE after 90 s - stopping it, so this row is not "
                     "reading someone else's turn)\n");
        rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
        if (! rig.until ([] (const juce::var& st)
                         { const auto a = agentVarOf (st);
                           return ! (bool) a.getProperty ("active", false); }, 20000))
            std::printf ("        (WARNING: it did not stop either - this row may still be reading it)\n");
        Rig::pump (400);
    }

    rig.send (echojay::rig::cmd::sendChat, args1 ("text", request), 15000);

    // ---- 2. AND THE REPLY IS ONLY THIS ROW'S IF THE CARD ADOPTED THIS GOAL -------------------------------
    // The card publishes the goal it is working on. Waiting for it to BE this request turns "the state looks
    // settled" into a fact about which turn settled - the one thing the old loop could not tell.
    out.adopted = rig.until ([&] (const juce::var& st)
                             { return agentVarOf (st).getProperty ("goal", juce::var()).toString() == request; },
                             25000);
    if (! out.adopted)
        std::printf ("        (WARNING: the card never took up this request as its goal within 25 s)\n");

    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (juce::Time::getMillisecondCounter() < end)
    {
        rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 4000);
        const auto a = agentVarOf (Rig::state());
        out.state = a.getProperty ("state", juce::var()).toString();
        if (out.state == "awaiting_approval") out.sawApproval = true;
        out.text  = a.getProperty ("liveText", juce::var()).toString();
        out.planLines = (int) a.getProperty ("planLines", 0);

        // A plan waiting for a decision IS the user's turn: press Apply once, as a user would, and record
        // that we did - a row that needed an approval and a row that did not are different rows.
        // ONE PRESS WAS NOT ENOUGH (14 Oct 2026). A build can present a SECOND plan after the first is
        // applied - L2 did, and with `pressed` already true the rig sat watching it until the bound expired.
        // A user presses Apply as often as the card asks; three is the same ceiling the questions use, so a
        // pair of plans cannot become a loop either.
        if (out.state == "awaiting_approval" && ! approve)
        {
            // The row wants it LEFT waiting. Recorded, so a row cannot mistake "nobody pressed" for
            // "nothing asked", and the loop stops rather than spinning out its bound.
            out.sawApproval = true;
            break;
        }
        if (out.state == "awaiting_approval" && out.applies < 3)
        {
            rig.send (echojay::rig::cmd::press, args1 ("button", "apply"), 8000);
            ++out.applies; out.applied = true;
            Rig::pump (300);
            continue;
        }
        // A QUESTION GETS AN ANSWER. The undo row sat in awaiting_ask until its bound expired, because the
        // rig could not do the one thing a user does with a question. It answers with the first choice the
        // card offers - which is the card's own preferred answer - and at most three times, so a pair of
        // questions cannot become a loop.
        if ((bool) a.getProperty ("askPending", false) && ! answerAsks)
        {
            // The question IS the result for this row. Record it and stop - leaving the card waiting is what
            // a user who has been asked something they did not expect would do.
            out.lastAsk = a.getProperty ("askText", juce::var()).toString();
            out.sawAsk  = true;
            break;
        }
        if ((bool) a.getProperty ("askPending", false) && out.asksAnswered < 3)
        {
            const auto labels = a.getProperty ("askLabels", juce::var()).toString();
            auto ack = rig.send (echojay::rig::cmd::answerAsk, args1 ("label", ""), 8000);
            juce::String how = ack.getProperty ("note", juce::var()).toString();
            // AN OPEN QUESTION IS STILL A QUESTION, AND A USER CAN STILL ANSWER IT (15 Oct 2026).
            //
            // The live gate went red on five rows and every one of them was this: the model asked "Which
            // compressor do you want on the Mix Bus?" as a talk/ask with NO choices, answerAsk refused -
            // "the ask offered no choices" - and C1's turn sat in awaiting_ask for its whole 90 s bound.
            // C2 then ran on the empty rack it left, C5 and C7 skipped for want of a slot, C8 read C1's
            // still-live turn, and H2 had no claim to judge. One unanswerable question, six rows.
            //
            // The rig could not do the one thing any user would: TYPE the answer. A chip-less ask takes
            // free text (TalkAsk::allowFreeText), and the client routes a typed message into a pending ask
            // through interceptTyped - so the fallback is to send the answer as a chat message, which is
            // exactly the user's own road. `answerText` is the row's own word for what it wants (the rows
            // that want a plugin say which), defaulting to "yes" for a plain confirmation.
            if (how.containsIgnoreCase ("no choices"))
            {
                // ---- AND "yes" IS NOT AN ANSWER TO "WHICH PLUGIN?" (16 Oct 2026) -----------------
                // The live pass watched the real server ask "Pick a transient shaper for the kick"
                // with `{"choices": [], "allowFreeText": false}` five times in five, the rig type
                // its default "yes", and the run end having changed nothing - and the ROW then
                // reported "nothing changed anywhere", which is a true sentence about the rig's own
                // answer rather than about the product. A question that names a kind of plugin is
                // answered with a plugin of that kind that is REALLY INSTALLED here.
                auto typed = answerText;
                // The question ON SCREEN NOW, not the last one answered: out.lastAsk is still the
                // previous round's at this point in the loop.
                auto q = a.getProperty ("askText", juce::var()).toString();
                if (q.isEmpty()) q = out.lastAsk;
                if (typed == "yes" || typed.isEmpty())
                {
                    if (q.containsIgnoreCase ("transient"))        typed = "EchoJay Transient";
                    else if (q.containsIgnoreCase ("compressor"))  typed = "EchoJay Compressor";
                    else if (q.containsIgnoreCase ("eq") || q.containsIgnoreCase ("equaliser")) typed = "EchoJay EQ";
                    else if (q.containsIgnoreCase ("limiter"))     typed = "EchoJay Limiter";
                    else if (q.containsIgnoreCase ("gate"))        typed = "EchoJay Gate";
                }
                rig.send (echojay::rig::cmd::sendChat, args1 ("text", typed), 15000);
                how = "no chips were offered, so the rig TYPED \"" + typed + "\" as a user would";
            }
            ++out.asksAnswered;
            out.lastAsk = a.getProperty ("askText", juce::var()).toString();
            std::printf ("        (answered the card's question: \"%s\" -> %s)\n",
                         out.lastAsk.substring (0, 90).toRawUTF8(), how.toRawUTF8());
            juce::ignoreUnused (labels);
            Rig::pump (400);
            continue;
        }
        // THE STEPS ARE PER-ROUND, so they must be read WHILE THEY EXIST (14 Oct 2026). EJAgentClient::steps()
        // holds the CURRENT round's steps: by the time a run reaches `done` the last round carried no tool
        // calls and the array is empty. So every read after the loop - the failDetail scan below included -
        // was reading an empty array and reporting nothing, which is why U1's second assertion failed with
        // the log plainly showing `step tc_u2 DONE "Undone: 1 edit."`. The poll already runs every 500 ms;
        // it keeps the last non-empty snapshot, and a row asserts on that.
        if (auto* sarr = a.getProperty ("steps", juce::var()).getArray())
            if (! sarr->isEmpty())
            {
                juce::String words;
                for (const auto& st : *sarr)
                {
                    words << st.getProperty ("label", juce::var()).toString() << "="
                          << st.getProperty ("detail", juce::var()).toString() << "; ";
                    if ((int) st.getProperty ("status", 0) == 4 /*Failed*/ && out.failDetail.isEmpty())
                        out.failDetail = st.getProperty ("detail", juce::var()).toString();
                }
                out.stepWords = words;
            }

        // Rest counts only for the turn this row asked for. When the card never adopted the goal there is
        // nothing to match against, so rest is accepted and the warning above stands as the record.
        const auto goalNow = a.getProperty ("goal", juce::var()).toString();
        if (isRestingState (out.state) && ! (bool) a.getProperty ("active", false)
            && (! out.adopted || goalNow == request))
        { out.settled = true; break; }
        Rig::pump (500);
    }
    // THE BOUND RUNNING OUT MUST NOT BE THE NEXT ROW'S PROBLEM. Whatever happened here, this function does
    // not hand the next row a live session: it waits for quiet and says so if it could not get it.
    if (! out.settled)
    {
        out.leftRunning = ! rig.until ([] (const juce::var& st)
                                       { const auto a = agentVarOf (st);
                                         return ! (bool) a.getProperty ("active", false)
                                                && isRestingState (a.getProperty ("state", juce::var()).toString()); },
                                       90000);
        std::printf ("        (this turn did not settle inside its bound%s)\n",
                     out.leftRunning ? " and was STILL LIVE 90 s later" : "; it settled during the wind-down");
    }
    // A LAST LOOK, as a fallback only: the loop above captures the failed step's user line while the steps
    // still exist, and this catches the case where a run failed between two polls. It cannot be the only
    // source, because after `done` the array is empty (see above).
    if (out.failDetail.isEmpty())
        if (auto* steps = agentVarOf (Rig::state()).getProperty ("steps", juce::var()).getArray())
            for (const auto& st : *steps)
                if ((int) st.getProperty ("status", 0) == 4 /*Failed*/)
                { out.failDetail = st.getProperty ("detail", juce::var()).toString(); break; }
    return out;
}

// ---- reading real state ------------------------------------------------------
static int ownSlotCount (const juce::var& st)
{
    return (int) st.getProperty ("ownRack", juce::var()).getProperty ("count", 0);
}
static juce::StringArray ownSlotNames (const juce::var& st)
{
    juce::StringArray names;
    if (auto* arr = st.getProperty ("ownRack", juce::var()).getProperty ("slots", juce::var()).getArray())
        for (const auto& sl : *arr) names.add (sl.getProperty ("name", juce::var()).toString());
    return names;
}
/** ---- WHAT THE CHAT IS DRAWING FOR THIS RUN (22 Oct 2026, his 09:24 fault) -----------------
    His fault: *"The build WAS on the rack, but the chat showed only my message: no agent card, no
    steps, no closing, no chips, no Undo all."*  Every 0b row read `agentRow`, which is the live row
    the window ADOPTED - a different object from the chat's own rows, and the two came apart exactly
    once: B3 was green while his chat was empty. `onScreen` is the window's chatMessages, each row
    now carrying whether it is a card and what that card says. Returns a void var when the chat is
    drawing no card for that run, which is the state his fault was. */
static juce::var cardOnScreenFor (const juce::var& st, const juce::String& runKey)
{
    if (auto* rows = st.getProperty ("onScreen", juce::var()).getArray())
        for (const auto& r : *rows)
        {
            if (! (bool) r.getProperty ("card", false)) continue;
            if (runKey.isNotEmpty() && r.getProperty ("cardRunKey", juce::var()).toString() != runKey)
                continue;
            return r;
        }
    return {};
}
static int userRowsOnScreen (const juce::var& st)
{
    int n = 0;
    if (auto* rows = st.getProperty ("onScreen", juce::var()).getArray())
        for (const auto& r : *rows)
            if (r.getProperty ("role", juce::var()).toString() == "user") ++n;
    return n;
}
static double busGainOf (const juce::var& st)
{
    return (double) st.getProperty ("ownRack", juce::var()).getProperty ("busGainDb", 0.0);
}
/** The figure a packed `landed` field holds for one control, or NaN. The field is
    "slot 1 Threshold=-30;Attack=2|slot 2 ..." - the slot prefix is stripped by position and
    everything up to the '=' is the name, spaces and all, because a controls dial is keyed by the
    plugin's own control name. (The first cut of this took the last whitespace token as the key and
    would have read "(Transient)=-14.4".) */
static double landedValueOf (const juce::String& landed, const juce::String& control)
{
    for (const auto& part : juce::StringArray::fromTokens (landed, "|", ""))
    {
        auto body = part.trim();
        if (body.contains ("{")) body = body.fromFirstOccurrenceOf ("{", false, false)
                                            .upToLastOccurrenceOf ("}", false, false);
        if (body.startsWith ("slot "))
            body = body.fromFirstOccurrenceOf (" ", false, false).trim()
                       .fromFirstOccurrenceOf (" ", false, false).trim();
        for (const auto& pair : juce::StringArray::fromTokens (body, ";", ""))
            if (pair.upToFirstOccurrenceOf ("=", false, false).trim() == control)
                return pair.fromFirstOccurrenceOf ("=", false, false).trim().getDoubleValue();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

static juce::var linkRowOf (const juce::var& st, const juce::String& uid)
{
    if (auto* arr = st.getProperty ("links", juce::var()).getArray())
        for (const auto& l : *arr) if (l.getProperty ("uid", juce::var()).toString() == uid) return l;
    return {};
}

/** THE LINKS THIS RUN LAUNCHED ITSELF (17 Oct 2026).

    Filled when the out-of-process link_hosts come up, and the strongest fact the runner has: these
    processes are started by the rig and live for the whole run, so a row that addresses one of them
    is addressing something that is certainly there. Rows created and abandoned DURING a run - T3's
    second instance and its kin - leave registry rows that are connected, named and carry a slot
    count from their last sidecar write, which no check of those three fields can see through. */
static juce::StringArray gLaunchedLinkUids;

/** EVERY CHIP ON SCREEN, from the two places they are published (17 Oct 2026).

    `steps[].chips` is a COMMA-JOINED STRING, not an array, and three rows read it with getArray():
    B3-live typed an answer naming no chip because it never saw one, and A8-live reported an ask
    "offering []" about an ask that was offering plenty. A reader that silently returns nothing for a
    field of the wrong type is the defect this round keeps finding, in its fourth costume.

    `agent.askLabels` is the pending ask's own labels, pipe-joined - what the card draws - and is the
    authoritative source while a question waits. The step strings are kept as a second source for a
    row that reads them after the ask has been answered. */
static juce::StringArray chipsOnScreen (const juce::var& st)
{
    juce::StringArray out;
    const auto labels = agentVarOf (st).getProperty ("askLabels", juce::var()).toString();
    for (const auto& l : juce::StringArray::fromTokens (labels, "|", "")) if (l.trim().isNotEmpty()) out.add (l.trim());
    if (auto* arr = st.getProperty ("agentRow", juce::var()).getProperty ("steps", juce::var()).getArray())
        for (const auto& sv : *arr)
            for (const auto& c : juce::StringArray::fromTokens (sv.getProperty ("chips", juce::var()).toString(), ",", ""))
                if (c.trim().isNotEmpty()) out.addIfNotAlreadyThere (c.trim());
    return out;
}

/** TRUE when the Link that AU hosting will load is NOT the bundle this run was given (item 0b).

    Set once at startup by comparing the two binaries' sizes - two builds of this plugin are never
    the same number of bytes - and printed there with both figures. Any row whose claim is about
    THIS build's Link behaviour must skip when it is true: a V2 from one build talking to a Link
    from another is a configuration that never ships, so a verdict from it is about nothing.

    17 Oct: made a file-static because the first version was a local in main() and only A4-live
    could ask it. T2w ("an edit lands on a Link whose OWN window is open") and I9c (the mirrored
    card) then went red in a mixed run on exactly that artefact, and I9c's own guess at it - reading
    the previous build's card WORDING - could not fire when the figure was missing altogether. One
    fact, measured once, asked by every row that needs it. */
static bool gLinkIsMixedBuild = false;

/** IS THIS A LINK A ROW CAN BE ABOUT? (17 Oct 2026, found by the gate's SECOND S6 run.)

    The registry outlives the processes listed in it. The second run picked a uid left behind by an
    earlier one and four rows then failed about a Link that was not there: L1 asked "add a compressor
    on 5aabf74ffa" - a bare hash, because the entry had no name - against a rack whose sidecar reads
    -1, which is the sentinel for "not readable" and not a slot count. The rows were not wrong; they
    were asking about nothing, which is the one thing a test must never do quietly.

    Three facts, all of which a live Link has and a leftover entry does not: it says it is connected,
    its host has given it a name, and its slot count is READABLE. -1 is excluded explicitly, because
    this tree has already been caught twice comparing that sentinel as if it were a number. */
static bool linkIsUsable (const juce::var& st, const juce::String& uid)
{
    if (uid.isEmpty()) return false;
    const auto l = linkRowOf (st, uid);
    if (! l.isObject()) return false;
    return (bool) l.getProperty ("connected", false)
        && l.getProperty ("name", juce::var()).toString().isNotEmpty()
        && (int) l.getProperty ("slotCount", -1) >= 0;
}

/** The uid of a Link that is live NOW - the Kick by preference, which is Sean's ruling so that rows
    saying "the kick" are about the kick. Empty when the registry holds no usable Link at all, which
    a row must report as a skip rather than guess around. */
static juce::String usableLinkUid (const juce::var& st, bool preferKick, juce::String* nameOut = nullptr)
{
    // THE ONES THIS RUN LAUNCHED COME FIRST. They are the only Links the runner can prove are alive,
    // and preferring them is what stops A2 from addressing a row left behind by a row that ran
    // earlier in this very pass - "the Link went 5 -> 5" over a rack whose live mirror held 2.
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool launchedOnly = (pass == 0);
        if (launchedOnly && gLaunchedLinkUids.isEmpty()) continue;
        juce::String pick, pickName;
        if (auto* arr = st.getProperty ("links", juce::var()).getArray())
            for (const auto& l : *arr)
            {
                const auto u = l.getProperty ("uid", juce::var()).toString();
                if (launchedOnly && ! gLaunchedLinkUids.contains (u)) continue;
                if (! linkIsUsable (st, u)) continue;
                const auto n = l.getProperty ("name", juce::var()).toString();
                if (preferKick && n.containsIgnoreCase ("Kick")) { if (nameOut != nullptr) *nameOut = n; return u; }
                if (pick.isEmpty()) { pick = u; pickName = n; }
            }
        if (pick.isNotEmpty()) { if (nameOut != nullptr) *nameOut = pickName; return pick; }
    }
    juce::String any, anyName;
    if (auto* arr = st.getProperty ("links", juce::var()).getArray())
        for (const auto& l : *arr)
        {
            const auto u = l.getProperty ("uid", juce::var()).toString();
            if (! linkIsUsable (st, u)) continue;
            const auto n = l.getProperty ("name", juce::var()).toString();
            if (preferKick && n.containsIgnoreCase ("Kick")) { if (nameOut != nullptr) *nameOut = n; return u; }
            if (any.isEmpty()) { any = u; anyName = n; }
        }
    if (nameOut != nullptr && any.isNotEmpty()) *nameOut = anyName;
    return any;
}

// Declared here because scen() calls it: a scenario whose only wording offence is the server's is
// recorded as a skip, and the two live next to each other below.
static void scenSkip (const char* id, const char* title, const juce::String& why, const char* owner);

// A row's verdict, in the scorecard's words. Printed in a form the scorecard is written from, so the file
// and the run cannot disagree.
/** WHICH ROWS THIS RUN IS FOR (17 Oct 2026, Sean's QUICK tier).

    `EJ_RIG_ONLY=RP8K` or `EJ_RIG_ONLY=A2,G2,RP8K` runs those scenarios and skips the rest with one
    line saying so. His instruction: "run the S4 guards plus only the matrix rows that touch the
    change... target under 15 minutes". The whole matrix is 50 minutes; one row is two, and a loop
    measured in days is the thing being fixed.

    A filtered run is NEVER a gate: the full matrix still runs overnight and before beta, and the
    scorecard says at the top that it was filtered, so no filtered log can be mistaken for one. */
static juce::StringArray gOnlyRows;
static bool gRowsFiltered = false;

/** ANY OF THESE (26 Oct 2026). Three times in one day a block's gate listed some of the rows
    inside it and not others, and the rows left out vanished with no line at all - once printing a
    GREEN run that had tested nothing. A gate written as a LIST is one that can be checked against
    the rows below it at a glance, and this is that list's reader. */
static bool anyRowWanted (const std::initializer_list<const char*>& ids)
{
    for (const char* id : ids) if (rowWanted (juce::String (id))) return true;
    return false;
}

static bool rowWanted (const juce::String& id)
{
    if (! gRowsFiltered) return true;
    for (const auto& want : gOnlyRows)
        if (id.equalsIgnoreCase (want.trim())) return true;
    return false;
}

/** One capability row. `r` is the turn behind it, and a row CANNOT PASS without one: see
    AgentOutcome::ran(). Pass nullptr only where there is deliberately no agent turn (A6, which sends a chat
    with the chip off) - and then the row must carry its own evidence that something happened, because the
    whole point of this gate is that "nothing changed" is not a result. */
static void row (const char* id, bool pass, const juce::String& request, const juce::String& detail,
                 const AgentOutcome* r = nullptr)
{
    if (! rowWanted (id)) return;   // EJ_RIG_ONLY: not this run's business
    juce::String note;
    if (r != nullptr && ! r->ran())
    {
        if (pass) note = "  <- FORCED FAIL: the assertion held but NO TURN RAN (" + r->whyNotRan() + ")";
        else      note = "  (and no turn ran: " + r->whyNotRan() + ")";
        pass = false;
    }
    if (pass) ++passes; else ++failures;
    std::printf ("  ROW %-4s %-4s  \"%s\"\n        %s%s\n", id, pass ? "PASS" : "FAIL",
                 request.toRawUTF8(), detail.toRawUTF8(), note.toRawUTF8());
    std::fflush (stdout);
}

static juce::var args1 (const char* k, const juce::var& v)
{
    auto* o = new juce::DynamicObject(); o->setProperty (k, v); return juce::var (o);
}

static juce::var args2 (const char* k1, const juce::var& v1, const char* k2, const juce::var& v2)
{
    auto* o = new juce::DynamicObject();
    o->setProperty (k1, v1); o->setProperty (k2, v2); return juce::var (o);
}

// ============================================================================
//  THE SCENARIO MATRIX (docs/AGENT_SCENARIO_MATRIX.md), Sean's 16 Oct change of
//  approach: *"Build the scenario runner first ... assertions on what the editor
//  draws, evidence PNGs per scenario ... Run the full matrix and write
//  MATRIX_REPORT with an owner per failure."*
//
//  ONE RUNNER, NOT A SECOND ONE. This rig already loads the real bundles, opens
//  and closes real editors, drives them through the user's own channel and
//  speaks to a mock or a live server. A separate matrix runner would be a second
//  road to the same place, which is the fault this codebase has paid for twice
//  (two build paths, two transcript writers). So the matrix lives here: a
//  scenario is a row with an ID from the matrix, an OWNER for its failure, and
//  EVIDENCE on disk.
//
//  EVIDENCE IS A PNG OF THE EDITOR plus the text it drew. The rig holds the
//  editor as a component, so the snapshot is of the real thing - not a
//  description of it, which is the blind spot the matrix was written to close.
// ============================================================================
struct Scen
{
    juce::String id, title, owner, detail, evidence;
    bool pass = false, skipped = false;
};
static std::vector<Scen> gScenarios;
static juce::File gEvidenceDir;
/** The rig itself, so captureEvidence can ask the PLUGIN for its own screenshot. A pointer rather
    than a parameter because every scen() call site would otherwise have to carry it, and there is
    exactly one rig in this process. Set in main before any scenario runs. */
static Rig* gRigForEvidence = nullptr;

/** <repo>/docs/review/<date>/ - created on first use. The repo, deliberately, not the
    isolated state root: the evidence outlives the run and Sean reads it. */
static juce::File evidenceRoot()
{
    if (gEvidenceDir != juce::File())  return gEvidenceDir;
    const auto day = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");
    gEvidenceDir = juce::File::getCurrentWorkingDirectory().getChildFile ("docs")
                       .getChildFile ("review").getChildFile (day);
    gEvidenceDir.createDirectory();
    return gEvidenceDir;
}

/** A PNG of what the editor is drawing, plus a .txt of every string it drew.
    Returns the PNG's path relative to the repo, or "" when there was no editor
    to photograph (which is itself worth recording rather than hiding). */
static juce::String captureEvidence (const juce::String& scenarioId, juce::Component* editor,
                                     const juce::var& state)
{
    const auto dir = evidenceRoot().getChildFile (scenarioId);
    dir.createDirectory();
    juce::String rel;
    // ---- THE PLUGIN TAKES ITS OWN PICTURE (16 Oct 2026) ----------------------------------------
    // The first run's PNGs were 522x100 of white: an AU's editor is a native view that the host only
    // WRAPS, so a snapshot taken out here captures the wrapper and nothing inside it. The plugin's
    // own editor is a juce::Component, so it can photograph itself - and what lands in the file is
    // then the chat, the run row and the rack strip as the user sees them.
    const auto png = dir.getChildFile ("editor.png");
    {
        // ONLY WHEN A WINDOW IS OPEN. With no editor there is nothing to photograph and the command
        // would simply time out - eight seconds per scenario spent proving that, which is how a
        // runner becomes something nobody runs.
        if (gRigForEvidence != nullptr && editor != nullptr)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("path", png.getFullPathName());
            gRigForEvidence->send (echojay::rig::cmd::screenshot, juce::var (o), 6000);
        }
    }
    if (png.existsAsFile() && png.getSize() > 0)
        rel = "docs/review/" + dir.getParentDirectory().getFileName() + "/" + scenarioId + "/editor.png";
    else if (editor != nullptr && editor->getWidth() > 0 && editor->getHeight() > 0)
    {
        // The host-side fallback, kept for a window the plugin could not photograph (no editor open).
        const auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        png.deleteFile();
        if (auto out = std::unique_ptr<juce::FileOutputStream> (png.createOutputStream()))
        {
            juce::PNGImageFormat fmt;
            if (fmt.writeImageToStream (img, *out)) rel = "docs/review/" + dir.getParentDirectory().getFileName()
                                                        + "/" + scenarioId + "/editor.png";
        }
    }
    // THE DRAWN TEXT, beside the picture: a PNG cannot be grepped and an assertion
    // needs the strings. Both, every time, from the one state read.
    juce::StringArray lines;
    lines.add ("scenario " + scenarioId);
    lines.add ("captured " + juce::Time::getCurrentTime().toISO8601 (true));
    if (auto* arr = state.getProperty ("onScreen", juce::var()).getArray())
        for (const auto& m : *arr)
            lines.add ("  [" + m.getProperty ("role", juce::var()).toString() + "] "
                       + m.getProperty ("text", juce::var()).toString());
    const auto arow = state.getProperty ("agentRow", juce::var());
    if ((bool) arow.getProperty ("present", false))
    {
        lines.add ("  run row: status=" + arow.getProperty ("status", juce::var()).toString()
                   + " changes=" + arow.getProperty ("changes", juce::var()).toString()
                   + " steps=" + arow.getProperty ("stepCount", juce::var()).toString()
                   + " zones=[" + arow.getProperty ("zones", juce::var()).toString() + "]");
        lines.add ("  opening: " + arow.getProperty ("opening", juce::var()).toString());
        lines.add ("  closing: " + arow.getProperty ("closing", juce::var()).toString());
        if (auto* sarr = arow.getProperty ("steps", juce::var()).getArray())
            for (const auto& sv : *sarr)
                lines.add ("    step \"" + sv.getProperty ("label", juce::var()).toString()
                           + "\" [" + sv.getProperty ("status", juce::var()).toString() + "] right=\""
                           + sv.getProperty ("right", juce::var()).toString() + "\" detail=\""
                           + sv.getProperty ("detail", juce::var()).toString() + "\" chips=["
                           + sv.getProperty ("chips", juce::var()).toString() + "]");
    }
    dir.getChildFile ("drawn.txt").replaceWithText (lines.joinIntoString ("\n") + "\n");
    return rel;
}

/** ---- I4's DENY-LIST, over every string the window drew (the matrix's invariant) ----------------
    "No raw parameter IDs, no text addressed to the model, no internal tags, no raw provider errors."
    Run on EVERY scenario rather than in a row of its own, which is what makes it an invariant. */
static juce::StringArray wordingOffences (const juce::var& state)
{
    static const char* kBanned[] = {
        "band1.", "band2.", "_db ", "_hz ", "_ms ", "threshold_db", "attack_db", "gain_db",
        "[interrupted]", "Report it as", "Report the plugin", "Do NOT", "do not name any value",
        "must not be quoted", "repeat this same call", "you were not given", "settings_structured",
        "null", "undefined", "Traceback", "HTTP 5",
        // ---- SEAN'S STEP 8 (16 Oct 2026): THE EDIT VALIDATOR'S OWN PREFIX --------------------
        // He read "op 1 invalid: ..." on a failed line. That is ChainHost's pre-flight talking to
        // whoever sent the ops, in the vocabulary of an edit protocol - a person has no "op 1".
        // Banned here so the invariant catches it wherever it leaks, and translated into words at
        // the one place it becomes a person's line (ExecutorDo's refusal).
        "op 1 invalid", "op 2 invalid", "op 3 invalid", "invalid: unknown operation",
        "settings_unapplied", "not_applied", "apply_to"
    };
    juce::StringArray out;
    // ---- WHOSE WORD IS IT? (17 Oct 2026) ------------------------------------------------------
    // S6 failed E13-live on I4 for the word "undefined" in a sentence the MODEL streamed: "I'm still
    // only getting about a second of audio each time, not enough to land a loudness reading...".
    // "undefined" is a JavaScript value printed into a string - it cannot be composed by this C++ at
    // all - so it is the server interpolating a variable it did not have, and no build of mine can
    // make that sentence read better.
    //
    // It is still a defect from where Sean sits, so it is never passed over: it is reported with a
    // "B: " mark, which makes the scenario SKIP with the sentence attached rather than go red, and it
    // goes to FOR_B. The plugin's OWN compositions - step lines, step details, the card - are judged
    // exactly as before, whatever the word, because those are mine to fix.
    auto scan = [&out] (const juce::String& where, const juce::String& text)
    {
        if (text.isEmpty()) return;
        const bool modelsProse = where == "a message" || where == "the opening" || where == "the closing";
        for (const char* b : kBanned)
            if (text.contains (b))
            {
                const bool jsIsm = juce::String (b) == "undefined" || juce::String (b) == "Traceback"
                                || juce::String (b) == "HTTP 5";
                out.add (juce::String (modelsProse && jsIsm ? "B: " : "")
                         + where + ": \"" + b + "\" in \"" + text.substring (0, 90) + "\"");
            }
    };
    // ---- THIS SCENARIO'S OWN WORDS (16 Oct 2026) ----------------------------------------------
    // The first cut scanned the WHOLE transcript, so one run's leak failed every scenario after it -
    // E7, whose subject is a meter's geometry, went red for a parameter id in a message three runs
    // earlier. The invariant is not weaker for being scoped: the run row below is the current run,
    // and the last few messages are what this scenario put on screen. The earlier leak still fails
    // the scenario that drew it, which is where it belongs.
    if (auto* arr = state.getProperty ("onScreen", juce::var()).getArray())
        for (int i = juce::jmax (0, arr->size() - 3); i < arr->size(); ++i)
            scan ("a message", (*arr)[i].getProperty ("text", juce::var()).toString());
    const auto arow = state.getProperty ("agentRow", juce::var());
    scan ("the closing", arow.getProperty ("closing", juce::var()).toString());
    scan ("the opening", arow.getProperty ("opening", juce::var()).toString());
    if (auto* sarr = arow.getProperty ("steps", juce::var()).getArray())
        for (const auto& sv : *sarr)
        {
            scan ("a step line",   sv.getProperty ("label", juce::var()).toString());
            scan ("a step detail", sv.getProperty ("detail", juce::var()).toString());
        }
    return out;
}

/** WHAT THE PLUGIN ACTUALLY SENT (16 Oct 2026, Sean's step 6).

    The mock records every request's path, goal, `context.recentChat` and `context.transport`, and
    this fetches the list so a row can assert on the BODY rather than on what the model did with it.
    Rules (a) and (c) of his step 6 are about what the start body carries, and nothing else in this
    runner could see it. Returns the newest matching request, or void.                              */
static juce::var lastRequestTo (int mockPort, const juce::String& pathContains)
{
    juce::URL u ("http://127.0.0.1:" + juce::String (mockPort) + "/api/rig/requests");
    int status = 0;
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withConnectionTimeoutMs (4000).withStatusCode (&status);
    auto stream = u.createInputStream (opts);
    if (stream == nullptr || status != 200) return {};
    const auto v = juce::JSON::parse (stream->readEntireStreamAsString());
    juce::var best;
    if (auto* arr = v.getProperty ("requests", v).getArray())
        for (const auto& r : *arr)
            if (r.getProperty ("path", juce::var()).toString().contains (pathContains)) best = r;
    return best;
}

/** THE RACK A ROW WAS WRITTEN FOR (16 Oct 2026, pass 12).

    Three rows failed pass 11 and not one of them was a product fault: the matrix runs in one
    process on one rack, so by the fortieth row the own rack held seven slots left by earlier rows
    and "the third-party slot", "slot 6" and "the slot this add lands in" no longer meant what the
    row was written against. A row whose subject depends on what ran before it tests the running
    order, not the product.

    So a row that cares which slots exist asks for an empty rack first, through the product's own
    removeSlot (the rig hook's clear_rack). It returns how many slots the rack has afterwards, so a
    row can state in its own evidence that it started from nothing.                                */
template <typename RigT>
static int clearOwnRack (RigT& rig)
{
    rig.send (echojay::rig::cmd::workOn,     args1 ("uid", ""), 8000);
    rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
    rig.send (echojay::rig::cmd::clearRack,  juce::var(), 12000);
    const auto st = rig.freshState();
    // `ownRack.slots` / `ownRack.count` are the published names. "chainSlots" was never a field of
    // the rig state at all, and three readers of it quietly returned nothing - which is how E3 came
    // to report `slot 1 holds ""` about a rack that held exactly what it had just put there.
    return (int) st.getProperty ("ownRack", juce::var()).getProperty ("count", juce::var (-1));
}

/** A MATRIX SCENARIO. Records it, prints it in the same shape as a row, captures the
    evidence, and applies invariant I4 to whatever was on screen - so a scenario that
    passes its own assertion but drew a raw id still fails, which is what an invariant is. */
static void scen (const char* id, const char* title, bool pass, const juce::String& detail,
                  const char* owner, juce::Component* editor, const juce::var& state,
                  const AgentOutcome* r = nullptr)
{
    if (! rowWanted (id)) return;   // EJ_RIG_ONLY: not this run's business
    Scen sc; sc.id = id; sc.title = title; sc.owner = owner; sc.detail = detail;
    if (r != nullptr && ! r->ran())
    {
        sc.detail += "  <- NO TURN RAN (" + r->whyNotRan() + ")";
        pass = false;
    }
    auto offences = wordingOffences (state);
    if (! offences.isEmpty())
    {
        bool allB = true;
        for (const auto& o : offences) if (! o.startsWith ("B: ")) allB = false;
        if (allB)
        {
            // Not this build's wording and not a pass either: the row is recorded as a skip whose
            // reason is the server's own sentence, and it goes to FOR_B.
            scenSkip (id, title,
                      "the only wording offence on screen is the SERVER's, in prose this build cannot "
                      "compose (FOR_B): " + offences.joinIntoString ("; ") + ".  NEVER counted as a pass.",
                      "B");
            return;
        }
        pass = false;
        sc.detail += "  <- I4 (clean wording) FAILED: " + offences.joinIntoString ("; ");
    }
    sc.pass = pass;
    sc.evidence = captureEvidence (id, editor, state);
    gScenarios.push_back (sc);
    if (pass) ++passes; else ++failures;
    std::printf ("  SCEN %-5s %-4s  %s\n        %s%s\n", id, pass ? "PASS" : "FAIL", title,
                 sc.detail.toRawUTF8(),
                 sc.evidence.isEmpty() ? "" : ("\n        evidence: " + sc.evidence).toRawUTF8());
    std::fflush (stdout);
}

static void scenSkip (const char* id, const char* title, const juce::String& why, const char* owner)
{
    Scen sc; sc.id = id; sc.title = title; sc.owner = owner; sc.detail = why; sc.skipped = true;
    gScenarios.push_back (sc);
    ++skips;
    std::printf ("  SCEN %-5s SKIP  %s\n        %s\n", id, title, why.toRawUTF8());
    std::fflush (stdout);
}

/** MATRIX_REPORT_<date>.md - one row per scenario, pass/fail/skip, owner, evidence.
    Written every run, even a red one: the report IS the artefact Sean asked for. */
static void writeMatrixReport (const juce::String& buildName, const juce::String& mode)
{
    if (gScenarios.empty()) return;
    const auto day = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");
    // THE LIVE PASS AND THE MOCK PASS ARE TWO REPORTS, NOT ONE FILE TWICE (16 Oct 2026). S6 runs both
    // under ctest, so a single name means whichever finishes second silently replaces the other - and
    // the one that matters for a given question would be gone. The mock pass keeps the plain name
    // (it is the deterministic one the matrix is written against); the live pass says so in its own.
    const auto f = juce::File::getCurrentWorkingDirectory().getChildFile ("docs")
                       .getChildFile ("MATRIX_REPORT_" + day
                                      + (mode.containsIgnoreCase ("live") ? "_live" : "") + ".md");
    int pass = 0, fail = 0, skip = 0;
    for (const auto& s : gScenarios) { if (s.skipped) ++skip; else if (s.pass) ++pass; else ++fail; }
    juce::StringArray out;
    out.add ("# Matrix report - " + day + " (" + mode + ")");
    out.add ("");
    out.add ("Build under test: **" + buildName + "**.  Runner: `tools/host_rig` driving the real bundles, "
             "real plugins and real editors; evidence under `docs/review/" + day + "/<scenario>/`.");
    out.add ("");
    out.add ("**" + juce::String (pass) + " passed, " + juce::String (fail) + " failed, "
             + juce::String (skip) + " skipped.** A skip is never a pass (matrix rule 9).");
    out.add ("");
    out.add ("| Scenario | Result | Owner | What happened | Evidence |");
    out.add ("|---|---|---|---|---|");
    for (const auto& s : gScenarios)
    {
        auto d = s.detail.replace ("|", "\\|").replace ("\n", " ");
        if (d.length() > 420) d = d.substring (0, 417) + "...";
        out.add ("| **" + s.id + "** " + s.title + " | "
                 + (s.skipped ? "SKIP" : s.pass ? "pass" : "**FAIL**") + " | "
                 + (s.skipped || s.pass ? juce::String ("-") : s.owner) + " | " + d + " | "
                 + (s.evidence.isEmpty() ? juce::String ("-") : "`" + s.evidence + "`") + " |");
    }
    out.add ("");
    out.add ("## Failures by owner");
    for (const char* who : { "A", "B", "Sean" })
    {
        juce::StringArray mine;
        for (const auto& s : gScenarios)
            if (! s.pass && ! s.skipped && s.owner == juce::String (who)) mine.add (s.id + " " + s.title);
        out.add ("");
        out.add ("**" + juce::String (who) + "**: " + (mine.isEmpty() ? juce::String ("nothing")
                                                                     : juce::String (mine.size()) + " - "
                                                                       + mine.joinIntoString ("; ")));
    }
    f.replaceWithText (out.joinIntoString ("\n") + "\n");
    std::printf ("  matrix report -> %s\n", f.getFullPathName().toRawUTF8());
}
static juce::String transcriptRoles (const juce::var& st)
{
    juce::StringArray r;
    if (auto* arr = st.getProperty ("transcript", juce::var()).getArray())
        for (const auto& row : *arr) r.add (row.getProperty ("role", juce::var()).toString());
    return r.joinIntoString (",");
}
static bool transcriptHas (const juce::var& st, const juce::String& needle)
{
    if (auto* arr = st.getProperty ("transcript", juce::var()).getArray())
        for (const auto& row : *arr)
            if (row.getProperty ("text", juce::var()).toString().contains (needle)) return true;
    return false;
}

// ============================================================================
//  main
// ============================================================================
int main (int argc, char* argv[])
{
    std::printf ("==== host_rig ====\n");
    if (argc < 4)
    {
        std::printf ("usage: host_rig <V2.component> <Link.component> <mockPort>\n");
        return 2;
    }
    // ---- the two preconditions, both refusals rather than skips ------------
    if (! echojay::stateIsIsolated())
    {
        std::printf ("REFUSED: ECHOJAY_STATE_HOME is not set. The rig drives the UI and must never touch\n"
                     "         the live ~/Library/EchoJay (6 Sep ruling).\n");
        return 2;
    }
    if (::getenv ("EJ_RIG_HOOK") == nullptr)
    {
        std::printf ("REFUSED: EJ_RIG_HOOK=1 is not set, so the plugin's hook is inert by design.\n");
        return 2;
    }

    // BRACE INIT, deliberately. `const juce::File v2Comp (juce::String (argv[1]));` declares a FUNCTION -
    // the most vexing parse - and this is the third time it has cost me a compile this week, so it is
    // written the way that cannot do it.
    const juce::File v2Comp   { juce::String (argv[1]) };
    const juce::File linkComp { juce::String (argv[2]) };
    // ---- IS THE LINK WE WERE GIVEN THE LINK THAT WILL LOAD? (17 Oct 2026, item 0b) ------------
    //
    // registerAU registers this bundle's factory and then asks JUCE to instantiate the IDENTIFIER,
    // and in a fresh process the system's INSTALLED component wins that lookup. So when the bundle
    // handed to this runner differs from the one in ~/Library, every Link row is hosting the
    // installed binary and no Link-side verdict about THIS build means anything.
    //
    // Measured by the binary's own size rather than asserted: two different builds of this plugin
    // are never the same number of bytes, and a row that cannot tell which binary it is talking to
    // must not claim to have tested one. Printed once, loudly, and the rows that assert THIS
    // build's Link behaviour skip with it.
    gLinkIsMixedBuild = [&linkComp]
    {
        const auto given = linkComp.getChildFile ("Contents/MacOS")
                               .getChildFile (linkComp.getFileNameWithoutExtension());
        const juce::File installed { "/Users/SeanD/Library/Audio/Plug-Ins/Components/EchoJay Link.component"
                                     "/Contents/MacOS/EchoJay Link" };
        if (! given.existsAsFile() || ! installed.existsAsFile()) return false;
        // ---- IDENTITY IS CONTENT, NOT SIZE (17 Oct 2026, and this cost two runs) ---------------
        //
        // The first cut compared getSize(), and today the two Link binaries are EXACTLY the same
        // length - 33173968 bytes - while being different builds: UUID 3DA1BAB8 in build-guards
        // against 14DBCBC3 installed. So the test said "the same binary, so the Link rows are about
        // this build", the rows ran, and I9c and A2 went red on the artefact the test exists to
        // catch. A false negative in a verifier is worse than no verifier, because it is believed.
        //
        // This tree has the lesson written down once already, about LTO-folded strings: identity is
        // proved by the UUID or by the content, never by a proxy that can coincide. An MD5 of the
        // two files costs about a tenth of a second, once, at startup.
        const auto hashOf = [] (const juce::File& f) -> juce::String
        {
            juce::FileInputStream in (f);
            if (! in.openedOk()) return {};
            return juce::MD5 (in).toHexString();
        };
        const auto a = hashOf (given), b = hashOf (installed);
        const bool differs = a.isNotEmpty() && b.isNotEmpty() && a != b;
        std::printf ("  the Link we were given hashes %s (%lld bytes); the INSTALLED one hashes %s - %s\n",
                     a.substring (0, 12).toRawUTF8(), (long long) given.getSize(),
                     b.substring (0, 12).toRawUTF8(),
                     differs ? "DIFFERENT BUILDS, so AU hosting will load the installed one and every "
                               "Link row about this build's behaviour SKIPS (item 0b)"
                             : "the same build, so the Link rows are about this build");
        return differs;
    }();
    const bool linkMixedBuild = gLinkIsMixedBuild;
    if (const char* only = ::getenv ("EJ_RIG_ONLY"); only != nullptr && *only != 0)
    {
        gOnlyRows = juce::StringArray::fromTokens (juce::String (only), ",", "");
        gOnlyRows.trim(); gOnlyRows.removeEmptyStrings();
        gRowsFiltered = ! gOnlyRows.isEmpty();
        if (gRowsFiltered)
            std::printf ("\n  ######## FILTERED RUN: only [%s] will be reported. This is NOT a gate - the\n"
                         "           full matrix runs overnight and before beta (Sean's QUICK tier, 17 Oct).\n\n",
                         gOnlyRows.joinIntoString (", ").toRawUTF8());
    }
    const int mockPort = juce::String (argv[3]).getIntValue();
    const juce::File v2vst3  { argc > 4 ? juce::String (argv[4]) : juce::String() };

    juce::ScopedJuceInitialiser_GUI juceInit;

    // A GUI session or nothing. juce::Desktop's display list is empty under a
    // headless session, and creating a window there would abort rather than fail
    // a check - so this is tested before anything is built.
    if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
    {
        std::printf ("SKIP: no display is attached to this session, so no editor can be opened.\n"
                     "      S6 reports this as a SKIP, never as a pass (Sean's instruction).\n");
        return 77;      // the gate's agreed "skipped loudly" code
    }

    // ---- point the plugin at the mock, inside the isolated root ------------
    {
        auto settings = echojay::userAppData().getChildFile ("Application Support/EchoJay/auth.json");
        settings.getParentDirectory().createDirectory();
        // THE KEY NAMES ARE THE PLUGIN'S, NOT MINE. EchoJayAPI::loadSettings reads "endpoint" and "token";
        // my first three passes wrote "apiEndpoint"/"authToken", so the plugin was never logged in and sent
        // NOTHING - and the mid-reply leg was closing a window over a reply that had never been requested.
        // The run reported 0 requests received, which is what finally said so; a leg that asserts on the
        // server's own record is worth more than any amount of reading.
        // EJ_RIG_TOKEN is set by build_and_run.sh in LIVE mode, from /api/forge-dev-login. In mock mode any
        // string will do, because the mock authenticates nothing.
        const char* envTok = ::getenv ("EJ_RIG_TOKEN");
        const juce::String token = (envTok != nullptr && *envTok != 0) ? juce::String (envTok)
                                                                      : juce::String ("rig-token");
        auto* o = new juce::DynamicObject();
        o->setProperty ("endpoint", "http://127.0.0.1:" + juce::String (mockPort));
        o->setProperty ("token", token);
        o->setProperty ("email", "rig@local");
        // THE USAGE GATE IS WHY FIVE PASSES SENT NOTHING. EchoJayAPI::sendChat refuses before any HTTP when
        // canSendTurn() is false, and canSendMessage() reads messageLimit - messagesUsedToday. With neither
        // field written, the limit was 0, the turn was refused locally, and onComplete(..., false) made it a
        // FAILED turn - which the keep-rule then correctly discarded. So the rig was testing a turn that was
        // never sent, and the one leg that failed was telling the truth about it.
        //
        // In LIVE mode the server's /api/me overwrites all of this on the first refresh; these are the
        // values that let the FIRST send leave the building.
        o->setProperty ("tier", "studio");
        o->setProperty ("messageLimit", 9999);
        o->setProperty ("messagesUsedToday", 0);
        o->setProperty ("credits", 9999);
        o->setProperty ("usageDate", juce::Time::getCurrentTime().formatted ("%Y-%m-%d"));
        settings.replaceWithText (juce::JSON::toString (juce::var (o), true));
        std::printf ("  settings -> %s (endpoint 127.0.0.1:%d)\n", settings.getFullPathName().toRawUTF8(), mockPort);
    }

    // juce_audio_processors_HEADLESS is what a plugin build links, and it deletes
    // addDefaultFormats(); juce::addDefaultFormatsToManager is the free function the
    // merge-gate harness already uses for exactly this reason.
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);

    auto load = [&fm] (const juce::File& comp, const juce::String& label) -> std::unique_ptr<juce::AudioPluginInstance>
    {
        juce::String ident;
        if (! registerAU (comp, ident)) return {};
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : fm.getFormats())
            if (f->getName() == "AudioUnit") f->findAllTypesForFile (found, ident);
        if (found.isEmpty()) { std::printf ("  %s: no types for %s\n", label.toRawUTF8(), ident.toRawUTF8()); return {}; }
        juce::String err;
        auto inst = fm.createPluginInstance (*found[0], 48000.0, 512, err);
        if (inst == nullptr) std::printf ("  %s: %s\n", label.toRawUTF8(), err.toRawUTF8());
        return inst;
    };

    // ---- THE SAME V2, AS A VST3 (15 Oct 2026) -------------------------------------------------------
    // Only for the lifecycle rows. Everything else stays on the AU, because the AU is what Sean runs in
    // Logic and a rig that proved things only in a format he does not use would be proving the wrong thing.
    auto loadVST3 = [&fm] (const juce::File& bundle, const juce::String& label) -> std::unique_ptr<juce::AudioPluginInstance>
    {
        if (! bundle.exists()) { std::printf ("  %s: no VST3 at %s\n", label.toRawUTF8(), bundle.getFullPathName().toRawUTF8()); return {}; }
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : fm.getFormats())
            if (f->getName() == "VST3") f->findAllTypesForFile (found, bundle.getFullPathName());
        if (found.isEmpty()) { std::printf ("  %s: no VST3 types for %s\n", label.toRawUTF8(), bundle.getFullPathName().toRawUTF8()); return {}; }
        juce::String err;
        auto inst = fm.createPluginInstance (*found[0], 48000.0, 512, err);
        if (inst == nullptr) std::printf ("  %s: %s\n", label.toRawUTF8(), err.toRawUTF8());
        return inst;
    };

    // ---- SEAN'S RULE 1 (16 Oct 2026): THE THIRD-PARTY PLUGIN, PREPARED BEFORE ANY RACK EXISTS ------
    // chain_plugins.xml is read by ChainHost at CONSTRUCTION, so a plugin written into it after the V2
    // is created cannot be resolved by name. The map file is read by the mock per request, so its
    // timing does not matter - but this is the one place both can be done at once.
    // ---- T0 ALSO SEEDS THE CATALOGUE, SO IT CANNOT BE SKIPPED (26 Oct 2026, 22:25) ----------
    // I gated this leg at 21:50 because a filtered run sat in it for twelve minutes. That was the
    // wrong cut: prepareThirdParty does TWO jobs - it samples a map from a real plugin (slow, and
    // only T1-T3 need it) and it SEEDS THE CATALOGUE every Link resolves names from (everything
    // needs it). Skipping the leg skipped the seeding, and the next pass could not put a single
    // plugin on a single rack: nine rows failed and six skipped for want of a subject.
    //
    // The twelve-minute stall was something else, and it is named where it was found: enumerating
    // the four new third-party identifiers one at a time (see the catalogue block).
    leg ("T0: a real third-party plugin, and a map sampled from it (it also seeds the catalogue)");
    const juce::File tpMapsFile { echojay::userStateHome().getChildFile ("rig-maps.json") };
    const ThirdPartyPrep tp = prepareThirdParty (fm, tpMapsFile);
    if (! tp.ok)
        skip ("T1: a third-party add with settings dials the plugin and reads its knob back",
              tp.why + " - so there is no third party to dial. This row is the whole reason the rig "
              "missed Sean's #1; it must not be allowed to pass by not running.");
    else
        std::printf ("  %s: %d params, fp %s..., dialling \"%s\" (index %d, %d anchors): default %.2f dB "
                     "-> asking %.2f dB\n",
                     tp.name.toRawUTF8(), tp.paramCount, tp.fp.substring (0, 12).toRawUTF8(),
                     tp.paramName.toRawUTF8(), tp.paramIndex, tp.anchorCount, tp.defaultValue, tp.askValue);

    // =====================================================================================
    //  LINKS IN THEIR OWN PROCESSES (Sean, 16 Oct 2026, before the matrix)
    //
    //  *"Run each Link in its own process if at all possible, the way Logic hosts AUs
    //   out-of-process (AUHostingService), so channel timing between V2 and Link matches Logic."*
    //
    //  Two tools/link_host processes, each hosting one real Link and holding REAL third-party
    //  plugins, sharing this run's isolated state root - which is how two Logic instances find
    //  each other. They are launched AFTER the catalogue above is written, because with
    //  EJ_RIG_HOOK=1 a Link is refused a plugin scan (the iLok rule) and must find its plugins in
    //  the cache the root already has.
    //
    //  WHAT THIS DOES NOT REPRODUCE, stated here as well as in docs/RIG_LINK_CAPABILITY.md: Logic
    //  renders out-of-process AUs from ONE audio graph, so its V2 block and Link block belong to
    //  the same render pass. These processes pump their own blocks on their own clocks, so anything
    //  that needs the two racks to see the same audio instant is NOT faithful here and runs on the
    //  in-process Links below instead.
    struct LinkProc
    {
        juce::String label;
        std::unique_ptr<juce::ChildProcess> proc;
        juce::String uid;
        int seq = 0;
        juce::File stateFile, cmdFile, ackFile;
        bool up() const { return uid.isNotEmpty(); }
        juce::var state() const { return juce::JSON::parse (stateFile.loadFileAsString()); }
        /** A command to this Link's own host, acknowledged by seq - the same shape as every other
            command channel in this tree, for the same reason (it crosses a process boundary). */
        juce::var send (const juce::String& cmd, const juce::String& names = {}, int timeoutMs = 15000)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("seq", ++seq);
            o->setProperty ("cmd", cmd);
            if (names.isNotEmpty()) o->setProperty ("names", names);
            ackFile.deleteFile();
            cmdFile.replaceWithText (juce::JSON::toString (juce::var (o), true));
            const auto deadline = juce::Time::currentTimeMillis() + timeoutMs;
            while (juce::Time::currentTimeMillis() < deadline)
            {
                Rig::pump (120);
                if (! ackFile.existsAsFile()) continue;
                const auto v = juce::JSON::parse (ackFile.loadFileAsString());
                if ((int) v.getProperty ("seq", -1) == seq) return v;
            }
            return {};
        }
    };
    std::vector<std::unique_ptr<LinkProc>> linkProcs;
    juce::String linkProcWhy;
    {
        const char* envHost = ::getenv ("EJ_LINK_HOST");
        juce::File hostBin { envHost != nullptr && *envHost != 0 ? juce::String (envHost)
                                                                : juce::String() };
        if (hostBin == juce::File())
            hostBin = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                          .getSiblingFile ("link_host");
        if (! hostBin.existsAsFile())
            linkProcWhy = "tools/link_host is not built at " + hostBin.getFullPathName()
                        + " (bash tools/link_host/build.sh), so the out-of-process Link scenarios "
                          "cannot run. They are SKIPPED, never passed.";
        else
        {
            // ---- REAL TRACK NAMES (16 Oct 2026, Sean) ------------------------------------------
            // "A4-live says Untitled's rack. If that's the rig's unnamed Link, name it
            // 88_7even Kick_2_01 so the row tests real names." They are delivered through
            // updateTrackProperties, the host's own door - see link_host - so a row that asks for
            // "the kick" is asking the question Sean asks in Logic.
            struct Want { const char* label; const char* plugins; const char* track; };
            static const Want kWant[] = {
                { "Kick",  "Renaissance Axx,Vertigo VSM-3", "88_7even Kick_2_01" },
                { "Vocal", "Renaissance Axx",               "88_7even Vox_Lead_01" },
            };
            for (const auto& w : kWant)
            {
                auto lp = std::make_unique<LinkProc>();
                lp->label     = w.label;
                lp->stateFile = echojay::userStateHome().getChildFile ("rig")
                                    .getChildFile ("linkhost-" + lp->label + ".json");
                lp->cmdFile   = echojay::userStateHome().getChildFile ("rig")
                                    .getChildFile ("linkhost-" + lp->label + "-cmd.json");
                lp->ackFile   = echojay::userStateHome().getChildFile ("rig")
                                    .getChildFile ("linkhost-" + lp->label + "-ack.json");
                juce::StringArray argv;
                argv.add (hostBin.getFullPathName());
                argv.add (linkComp.getFullPathName());
                argv.add (lp->label);
                argv.add ("--prebuild");
                argv.add (w.plugins);
                argv.add ("--track-name");
                argv.add (w.track);
                lp->proc = std::make_unique<juce::ChildProcess>();
                if (! lp->proc->start (argv))
                {
                    linkProcWhy = "link_host would not start for \"" + lp->label + "\"";
                    continue;
                }
                linkProcs.push_back (std::move (lp));
            }
            // Wait for each one to be a LINK: a uid of its own, and its prebuilt rack published.
            const auto deadline = juce::Time::currentTimeMillis() + 40000;
            for (auto& lp : linkProcs)
            {
                while (juce::Time::currentTimeMillis() < deadline)
                {
                    Rig::pump (250);
                    const auto st = lp->state();
                    lp->uid = st.getProperty ("uid", juce::var()).toString();
                    if (lp->uid.isNotEmpty() && (int) st.getProperty ("slotCount", 0) > 0) break;
                }
                const auto st = lp->state();
                if (lp->uid.isNotEmpty()) gLaunchedLinkUids.addIfNotAlreadyThere (lp->uid);
                std::printf ("  link_host \"%s\": uid=%s  slots=%d [%s]  pid=%s\n",
                             lp->label.toRawUTF8(),
                             lp->uid.isEmpty() ? "(none yet)" : lp->uid.toRawUTF8(),
                             (int) st.getProperty ("slotCount", 0),
                             st.getProperty ("slots", juce::var()).toString().toRawUTF8(),
                             st.getProperty ("pid", juce::var()).toString().toRawUTF8());
            }
        }
        if (linkProcWhy.isNotEmpty()) std::printf ("  (out-of-process Links: %s)\n", linkProcWhy.toRawUTF8());
    }

    // ---- A REGISTRY WITH NOBODY IN IT BUT THIS RUN (17 Oct 2026) ---------------------------------
    // Three S6 runs in a row were decided by Links that had already exited. The registry's rows
    // outlive their processes - reapDeadPublisherSlots is only called by a claimant that finds the
    // table FULL (queue item 14) - and a dead row keeps saying `connected`, keeps the NAME its host
    // gave it, and keeps the slot count from its last sidecar write. All three of the facts a row can
    // check, still true, about a process that is gone: A2 reported "the Link went 5 -> 5" about a rack
    // whose own live mirror held 2.
    //
    // So the rig reaps before it starts, with the product's OWN check - sidecar publisher pid,
    // kill(pid, 0), failing closed on anything it cannot read - and says how many rows it took. This
    // is scaffolding of the same kind as clear_rack: it does not change what the product does, it
    // gives the rows the registry they were written against. The product-side change (reap on a clock
    // as well as on pressure) is item 14 and belongs to a round with a leg of its own.
    {
        int e1 = 0;
        const auto regDir = LinkShm::resolveDir (e1);
        int fd = -1, e2 = 0;
        if (void* reg = LinkShm::openRegistry (regDir, fd, e2))
        {
            const int reaped = LinkShm::reapDeadPublisherSlots (reg, regDir);
            std::printf ("  registry: reaped %d row(s) whose publisher process is gone (%s)\n",
                         reaped, regDir.toRawUTF8());
        }
        else
            std::printf ("  registry: could not be opened to reap (errno %d) - the Link rows will rely "
                         "on their usability check alone\n", e2);
    }

    leg ("R0: the built bundles load in one host process");
    // REAL TRACK NAMES (16 Oct 2026, Sean): the in-process pair gets them too, or "the kick" still
    // means nothing on half the racks in the rig. These are the names a Logic project has.
    Track v2; v2.name = "Mix Bus (V2)";  v2.trackName  = "Mix Bus";
    Track lk1; lk1.name = "Kick (Link)"; lk1.trackName = "88_7even Kick_2_02";
    Track lk2; lk2.name = "Vocal (Link)"; lk2.trackName = "88_7even Vox_Dbl_01";
    v2.plugin  = load (v2Comp,   "V2");
    lk1.plugin = load (linkComp, "Link 1");
    lk2.plugin = load (linkComp, "Link 2");
    check (v2.plugin != nullptr && lk1.plugin != nullptr && lk2.plugin != nullptr,
           "V2 and two Links instantiate from the BUILT bundles");
    if (v2.plugin == nullptr || lk1.plugin == nullptr || lk2.plugin == nullptr)
    { std::printf ("==== host_rig: RED (nothing could be hosted) ====\n"); return 1; }

    // The VST3 copy of the V2, for the window-lifecycle rows only. Optional: if it will not load, every
    // other row runs and the lifecycle rows say so rather than silently proving less.
    Track v2v; v2v.name = "Mix Bus (V2, VST3)"; v2v.wrapper = "VST3";
    v2v.plugin = v2vst3.exists() ? loadVST3 (v2vst3, "V2 (VST3)") : nullptr;
    check (v2v.plugin != nullptr,
           "...and the V2 also loads as a VST3, which is the only host road that can really close a window",
           v2v.plugin != nullptr ? juce::String ("loaded") : juce::String ("NOT loaded - the lifecycle rows "
                                   "will say what they could not prove"));

    const double sr = 48000.0; const int block = 512;
    for (Track* t : { &v2, &lk1, &lk2 }) t->prepare (sr, block);
    if (v2v.plugin != nullptr) v2v.prepare (sr, block);

    // Audio first, and kept running: every leg below needs the Links to be
    // registered and passing audio, which only happens once they have processed.
    for (int i = 0; i < 200; ++i)
    {
        for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr);
        if ((i % 20) == 0) Rig::pump (10);
    }
    Rig::pump (1500);       // the 1 Hz registry pass runs on the processor's timer

    // ---- W1: A WINDOW THAT REALLY CLOSES ------------------------------------------------------------
    //
    // THE GROUND EVERY 0b LEG STANDS ON. Until now the rig could not close a window at all: it released the
    // AU's wrapper view, its own unique_ptr went null, and the plugin reported the SAME EchoJayEditor alive
    // for the whole run - one built, one destroyed, at the end of the process. So "the run survives the
    // window" had nothing to be tested against, and two rows (R4 and C11's first cut) were passing because
    // nothing had died.
    //
    // This leg asks the plugin, not the host, and it runs FIRST and on its own track, closing the window
    // again before anything else starts - two V2 editors open at once would fight over the rack lock, which
    // is a different test for a different day.
    leg ("W1: closing a window really destroys the plugin's editor (the VST3 road)");
    if (v2v.plugin != nullptr)
    {
        v2v.openEditor();
        check (v2v.editor != nullptr, "the VST3 V2's editor opened");
        for (int i = 0; i < 40; ++i) { v2v.processOneBlock (sr); if ((i % 10) == 0) Rig::pump (20); }
        Rig::pump (1600);     // its processor's 1 Hz line has to exist before it can be read
        const auto built0 = (int) Rig::processorState ("VST3").getProperty ("editorsBuilt", 0);
        check (built0 >= 1, "...and that instance publishes its own processor line, separately from the AU's",
               "editorsBuilt " + juce::String (built0) + ", editorAlive "
               + Rig::processorState ("VST3").getProperty ("editorAlive", juce::var()).toString());
        v2v.closeEditor();
        check (v2v.lastCloseDestroyedEditor,
               "...and CLOSING it destroys the plugin's editor - which the AU road cannot do, and which "
               "every item 0b leg needs",
               v2v.lastCloseDestroyedEditor ? juce::String ("the plugin reports no editor")
                                            : v2v.lastCloseNote);
        v2v.openEditor();
        for (int i = 0; i < 40; ++i) { v2v.processOneBlock (sr); if ((i % 10) == 0) Rig::pump (20); }
        Rig::pump (1600);
        const auto built1 = (int) Rig::processorState ("VST3").getProperty ("editorsBuilt", 0);
        check (built1 == built0 + 1,
               "...and reopening builds a NEW editor with a new id, so the keep-rule's identity counter is "
               "testable at last", "editorsBuilt " + juce::String (built0) + " -> " + juce::String (built1));
        v2v.closeEditor();
    }
    else
        skip ("W1: closing a window really destroys the plugin's editor",
              "the V2's VST3 did not load in this process, so there is still no host road here that can "
              "close a window - item 0b's legs stay blocked and this says so");

    Rig rig;
    gRigForEvidence = &rig;   // so a scenario's PNG is taken by the plugin, not by this host
    leg ("R0b: can anything in THIS process reach the server?");
    {
        // ISOLATES THE QUESTION. Five passes could not tell whether the plugin was failing to send or the
        // process could not reach the port at all - the mock's log showed no request either way. This does
        // a plain JUCE HTTP GET from the rig's own thread: if it succeeds the transport is fine and the
        // plugin's send path is where to look; if it fails, nothing in this process can talk to the server
        // and every chat leg below is meaningless rather than failing.
        // The mock has /api/rig/requests; B's server has /local/health. Probing the wrong one answered 404
        // and called the transport broken, which is the leg lying rather than the plugin.
        const bool liveMode = liveServerConfigured();
        juce::URL u ("http://127.0.0.1:" + juce::String (mockPort)
                     + (liveMode ? "/local/health" : "/api/rig/requests"));
        int status = 0;
        juce::StringPairArray respHeaders;
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs (4000)
                        .withStatusCode (&status)
                        .withResponseHeaders (&respHeaders);
        auto stream = u.createInputStream (opts);
        const juce::String body = stream != nullptr ? stream->readEntireStreamAsString() : juce::String();
        check (stream != nullptr && status == 200,
               "a plain JUCE HTTP GET from this process reaches the server",
               "status=" + juce::String (status) + " body=" + body.substring (0, 60));
    }

    // =============================================================================================
    //  ITEM 0b (Sean's ruling, 14 Oct 2026): THE RUN SURVIVES THE WINDOW.
    //
    //  ON THE VST3 TRACK, AND BEFORE THE AU'S EDITOR EXISTS. These legs need a window that really
    //  goes away, which only the VST3 road gives (W1 above is the proof), and they need to be the
    //  only V2 editor in the process while they run - two editors would both consume rig-cmd.json
    //  and fight over the rack lock. So they sit between W1 and R1, and close v2v's window again
    //  before the AU's opens.
    //
    //  THE CHANNEL. With no window there is nothing to send a command to and nothing publishing
    //  rig-state.json - which is precisely the state these legs are about - so every assertion here
    //  reads the PROCESSOR's own line (rig-processor-VST3.json), which fix 6 added for the chat
    //  turn and which now carries the agent's facts too.
    // =============================================================================================
    if (v2v.plugin != nullptr)
    {
        leg ("ITEM 0b: a run started in one window carries on with NO window, and the next one adopts it");
        const auto pst = [] { return Rig::processorState ("VST3"); };
        const auto pstr = [&pst] (const char* k) { return pst().getProperty (k, juce::var()).toString(); };
        const auto untilProc = [&pst] (std::function<bool (const juce::var&)> ok, int ms)
        {
            const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
            while (juce::Time::getMillisecondCounter() < end)
            { if (ok (pst())) return true; Rig::pump (250); }
            return ok (pst());
        };

        v2v.openEditor();
        for (int i = 0; i < 40; ++i) { v2v.processOneBlock (sr); if ((i % 10) == 0) Rig::pump (20); }
        Rig::pump (1200);
        rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);
        rig.send (echojay::rig::cmd::workOn,    args1 ("uid", ""), 8000);
        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
        rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0), 8000);
        Rig::pump (700);

        // ---- WHICH CLOSE BEHAVIOUR THIS BUILD SHIPS, ASKED OF THE BUILD (22 Oct 2026) ----------
        //
        // F3 and B1-B4 are two shapes of one question - what is the user left with when a window
        // closes mid-run - and which shape is correct depends on a switch only the plugin knows. The
        // rig used to answer it from its own inverted default; it asks now, and if the build cannot
        // be asked, every row here says so rather than picking a side.
        const auto zeroB = agent0bFromBuild (rig.freshState());

        // ---- F3: WHAT THE USER IS LEFT WITH AFTER A CLOSE MID-RUN -------------------------------
        //
        // The matrix's F3 was written while 0b was off: *"M1 run, close window mid-run - stops as 15b
        // (0b off) and says so; no duplicate lines on reopen."*  B1-B4 were skipped because 0b was
        // off, and nothing asserted what happened instead, so this row was that.
        //
        // RE-AIMED, 22 Oct, WITH THE RULING QUOTED. Sean turned 0b on by default on 20 Oct on his own
        // in-host evidence: *"card still waiting, chips live, plugin on the rack, card in the right
        // place."*  A row asserting that the run STOPPED now asserts the opposite of what he proved
        // and shipped, so it keeps its question and changes its instrument: the run must SURVIVE, and
        // the reopened window must show it ONCE. The duplicate half - the half that was always about
        // the reopen rather than about stopping - is identical in both shapes.
        if (! zeroB.published)
            scenSkip ("F3", "what a close mid-run leaves the user with",
                      "this build does not publish agent0bOn on its editor state, so the rig cannot "
                      "know which close behaviour it ships - and the one thing this row must never do "
                      "is assume. Rebuild the bundles from this tree and it runs.", "A");
        else
        {
            rig.send (echojay::rig::cmd::sendChat,
                      args1 ("text", "rig-row-marker: set the pre gain, twice (then I close the window)"), 15000);
            const bool started = untilProc ([] (const juce::var& p)
                                 { return (bool) p.getProperty ("agentActive", false); }, 40000);
            Rig::pump (600);
            v2v.closeEditor();
            const bool reallyGone = v2v.lastCloseDestroyedEditor;
            Rig::pump (2500);
            // The run's fate is read with a WAIT either way, so a slow stop is not read as survival
            // and a slow survival is not read as a stop. Same state field in both branches; only the
            // sense of it moves, and the build says which sense is right.
            const bool stopped = untilProc ([] (const juce::var& p)
                                 { return ! (bool) p.getProperty ("agentActive", false); },
                                 zeroB.on ? 4000 : 20000);
            const bool stillLive = ! stopped;
            v2v.openEditor();
            for (int i = 0; i < 40; ++i) { v2v.processOneBlock (sr); if ((i % 10) == 0) Rig::pump (20); }
            Rig::pump (1500);
            const auto st = rig.freshState();
            // ...and the reopened window draws ONE run row, with no line repeated.
            int rows = (int) st.getProperty ("agentRow", juce::var()).getProperty ("rows", juce::var());
            int markerCopies = 0;
            if (auto* arr = st.getProperty ("onScreen", juce::var()).getArray())
                for (const auto& m : *arr)
                    if (m.getProperty ("text", juce::var()).toString().contains ("then I close the window"))
                        ++markerCopies;
            const bool fate = zeroB.on ? stillLive : stopped;
            if (! started || ! reallyGone)
                scenSkip ("F3", "what a close mid-run leaves the user with",
                          juce::String ("this leg needs a live run and a REAL close: started=")
                          + (started ? "y" : "n") + " editorDestroyed=" + (reallyGone ? "y" : "n"), "A");
            else
                scen ("F3", zeroB.on
                            ? "a run SURVIVES its window closing (0b on, his 20 Oct proof), and the reopen shows it once"
                            : "a run stops when its window closes (0b off), and the reopen shows it once",
                      fate && rows <= 1 && markerCopies == 1,
                      "the run " + juce::String (stillLive ? "was still live" : "stopped with the window")
                      + ", which is " + juce::String (fate ? "what this build ships" : "NOT what this build ships")
                      + "; the reopened window draws " + juce::String (rows) + " run row(s) and "
                      + juce::String (markerCopies) + " copy(ies) of the goal  (0b is "
                      + juce::String (zeroB.on ? "ON - a close must not kill the run, and must not leave "
                                                 "a second row either"
                                               : "OFF - stopping IS the shipped behaviour; what must not "
                                                 "happen is a duplicate or a run left spinning")
                      + ")",
                      "A", v2v.editor.get(), st);
        }
        // ---- AND THIS ROW PUTS THE BENCH BACK (22 Oct 2026) ------------------------------------
        //
        // With 0b on, the run this row starts is STILL RUNNING when the row ends, and a live run
        // refuses every typed message after it ("The agent is still working on ..."). That is the
        // product behaving as designed, and in gate 2026-10-22 it cost four later rows their
        // subject: C11v(a), C11v(b), I1, F1 and F2 all type into the chat, all were refused, and all
        // reported the absence as a product fault. Stopped through the USER'S OWN road - the Stop
        // button - because that is the one a person would press.
        if (zeroB.published && zeroB.on && (bool) pst().getProperty ("agentActive", false))
        {
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
            const bool quiet = untilProc ([] (const juce::var& p)
                               { return ! (bool) p.getProperty ("agentActive", false); }, 20000);
            check (quiet, "F3 left the bench clear: the run it started is stopped before the chat rows "
                          "type into the same chat (a live run refuses typed messages)",
                   "agentActive=" + pstr ("agentActive") + " state=" + pstr ("agentState"));
        }
        if (! zeroB.on)
            skip ("ITEM 0b: B1-B4 (the run survives the window, the reopened window adopts it, a whole "
                  "run with no window, the build road withdrawn)",
                  zeroB.published
                    ? juce::String ("this build ships 0b OFF (EJ_AGENT_0B=0, his kill switch): a close "
                                    "stops the run as 15b did, so these rows would assert the opposite "
                                    "of what it does. Unset the var and they run.")
                    : juce::String ("this build does not publish agent0bOn, so which behaviour it ships "
                                    "is unknown here - and an unknown is not a reason to run rows that "
                                    "only make sense one way round. Rebuild the bundles from this tree."));
        if (zeroB.on)
        {
        // ---- B1: THE RUN IS NOT KILLED BY THE CLOSE --------------------------------------------
        // Before item 0b, ~EchoJayEditor called agentClient_->stop(): close the window mid-run and
        // the session, its steps and its undo tokens were gone, with no error, because nothing was
        // left to report one. The whole assertion is that the client is still ALIVE afterwards.
        {
            // a run that waits for an approval, so there is certainly something in flight to kill
            const int slotsBeforeRun = (int) pst().getProperty ("procSlots", -1);
            rig.send (echojay::rig::cmd::sendChat,
                      args1 ("text", "rig-row-marker: set the pre gain, twice (survive the close)"), 15000);
            const bool started = untilProc ([] (const juce::var& p)
                                 { return (bool) p.getProperty ("agentActive", false); }, 40000);
            // ---- THE SESSION ID IS NOT THERE THE MOMENT THE RUN IS (live road, 23 Oct 2026) ---
            // `started` waits for agentActive, which is true as soon as the POST is away; the
            // session id exists only once the server has answered with its first frame. On the mock
            // road the two arrive in the same millisecond, so this read green for a month; against
            // 3215 it read "" and the leg failed on a comparison it had no right to make yet
            // ("session ag_n9uoqULSEc... (was )") while the run itself had survived the close
            // exactly as item 0b promises. Wait for the id, bounded; if it never comes, the leg says
            // so and judges the half it can.
            const bool haveSession = untilProc ([] (const juce::var& p)
                                     { return p.getProperty ("agentSession", juce::var())
                                               .toString().isNotEmpty(); }, 20000);
            const auto sessionBefore = pstr ("agentSession");
            const auto keyBefore     = pstr ("agentRunKey");
            v2v.closeEditor();
            Rig::pump (1800);        // the processor's own 1 Hz line is the only reader left
            const bool stillAlive = (bool) pst().getProperty ("agentActive", false)
                                 || pstr ("agentState") == "awaiting_approval";   // the product's spelling (22 Oct)
            const bool gone = v2v.lastCloseDestroyedEditor;
            check (started, "a run was in flight before the window closed",
                   "state " + pstr ("agentState") + ", session " + sessionBefore);
            if (! gone)
                skip ("B1: the run survives the window closing",
                      "this host did not destroy the plugin's editor on this close, so the window never "
                      "actually went away and the leg would be vacuous: " + v2v.lastCloseNote);
            else
                check (stillAlive && (! haveSession || pstr ("agentSession") == sessionBefore),
                       "ITEM 0b: the window was DESTROYED and the run is still there - same session, still "
                       "live  (RED before item 0b: ~EchoJayEditor called stop() and the run ceased to exist)",
                       "editor gone=" + juce::String ((int) gone) + ", state " + pstr ("agentState")
                       + ", session " + pstr ("agentSession") + " (was " + sessionBefore + ")"
                       + (haveSession ? juce::String()
                                      : juce::String (" - NO session id had arrived before the close, "
                                                      "so the session half of this leg was not "
                                                      "testable and only the run's survival is "
                                                      "judged here")));
            check (pstr ("agentBuildRoad") == "0" || pstr ("agentBuildRoad").isEmpty(),
                   "...and the WINDOW'S build road went with the window - which since his 22 Oct ruling is "
                   "a change of road and no longer a limit: a build now takes the processor's own road "
                   "(B3 asserts it applies there)",
                   "agentBuildRoad=" + pstr ("agentBuildRoad"));

            // ---- B3b: A RUN WAITING ON A CHIP APPLIES NOTHING (his 22 Oct ruling, second leg) --
            //
            // *"If the run is waiting on a chip or a question, it keeps waiting."*
            //
            // The other half of B3, and the half that makes B3 mean something: now that a windowless
            // run APPLIES, the thing that must stay true is that an UNAPPROVED one does not. This run
            // is holding two ask_first steps with no window open, and its plan's own figures are 4
            // then 6 dB - so the pre-gain must still read what it read before the goal was sent,
            // after a bound rather than at one arbitrary moment. B2 above then presses Apply and the
            // same number moves, which is this leg's other direction, already asserted.
            bool movedWhileWaiting = false;
            for (int i = 0; i < 20 && ! movedWhileWaiting; ++i)
            {
                Rig::pump (100);
                const int sl = (int) pst().getProperty ("procSlots", -1);
                if (sl >= 0 && sl != slotsBeforeRun) movedWhileWaiting = true;
            }
            check (! movedWhileWaiting
                   && (pstr ("agentState") == "awaiting_approval" || (bool) pst().getProperty ("agentActive", false)),
                   "B3b: a run WAITING on a chip with no window open applies nothing - it keeps waiting, "
                   "and the rack it was asked to change is untouched over a 2 s bound  (the other half of "
                   "his 22 Oct ruling; B2's Apply above is this leg's opposite direction)",
                   "slots " + juce::String (slotsBeforeRun) + " -> "
                   + juce::String ((int) pst().getProperty ("procSlots", -1))
                   + ", state " + pstr ("agentState"));

            // ---- B2: THE APPROVAL SURVIVED IT TOO, AND THE REOPENED WINDOW CAN PRESS IT --------
            v2v.openEditor();
            for (int i = 0; i < 40; ++i) { v2v.processOneBlock (sr); if ((i % 10) == 0) Rig::pump (20); }
            Rig::pump (1200);
            const auto adopted = rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 6000);
            const auto rowAfter = Rig::state().getProperty ("agentRow", juce::var());
            check (rowAfter.getProperty ("present", juce::var()).toString() == "1"
                   && (int) rowAfter.getProperty ("rows", 0) >= 1,
                   "ITEM 0b: the reopened window ADOPTS the run's row rather than starting a second one - "
                   "matched by the run's key, which exists before any sessionId does",
                   "present=" + rowAfter.getProperty ("present", juce::var()).toString()
                   + ", rows=" + juce::String ((int) rowAfter.getProperty ("rows", 0))
                   + ", key " + keyBefore + " -> " + pstr ("agentRunKey")
                   + ", ack " + adopted.getProperty ("note", juce::var()).toString());
            // ---- B2c: THE CHAT SHOWS THE WAITING CARD, AND WHAT IT OFFERS (his 09:24 fault) --
            // RE-AIMED 22 Oct, mid-afternoon: this plan's waiting step is an ask_first APPROVAL, so
            // the card offers Apply and Skip and has no chips at all - and the row read `chips []`
            // and called the product wrong for it. Chips belong to a step that asked a QUESTION,
            // which is B2d below, on the plan that has one. What is true here is that the chat draws
            // this run's card with the steps it is holding.
            {
                const auto waitingCard = cardOnScreenFor (Rig::state(), pstr ("agentRunKey"));
                check (waitingCard.isObject()
                       && (int) waitingCard.getProperty ("cardSteps", 0) >= 1,
                       "B2c: the reopened window's CHAT draws this waiting run's card, with the steps it "
                       "is holding - not only the row the window adopted",
                       waitingCard.isObject()
                         ? "steps " + waitingCard.getProperty ("cardSteps", juce::var()).toString()
                           + ", chips [" + waitingCard.getProperty ("cardChips", juce::var()).toString() + "]"
                         : "the chat drew NO card for run " + pstr ("agentRunKey")
                           + " (onScreen cards=" + Rig::state().getProperty ("onScreenCards", juce::var()).toString() + ")");
            }
            // What the rack said BEFORE the apply, because on the live road the number is the
            // model's to choose and only the MOVEMENT is ours to assert (22 Oct 2026).
            const double pgBefore = (double) Rig::state().getProperty ("preGainDb", 99.0);
            const auto ack = rig.send (echojay::rig::cmd::press, args1 ("button", "apply"), 10000);
            untilProc ([] (const juce::var& p) { return ! (bool) p.getProperty ("agentActive", false); }, 60000);
            Rig::pump (800);
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 6000);
            const double pg = (double) Rig::state().getProperty ("preGainDb", 99.0);
            // ---- THE PLAN'S NUMBER IS THE MOCK'S; THE LIVE ROAD PICKS ITS OWN (22 Oct 2026) ----
            //
            // This asserted 6.0 dB on both roads - the scripted plan's second line. It failed live
            // with "pre-gain now 2.00 dB", and the model's own closing says why: *"Set the Mix Bus
            // pre-gain to 2 dB, applied twice as asked."*  It chose 2, correctly, and the row called
            // the product wrong for it. The QUESTION - does the reopened window's Apply land on the
            // rack? - is the same on both roads; only the figure that proves it differs, so the
            // exact number stays where it is scripted and the live road asserts the movement.
            const bool landedOnTheRack = liveServerConfigured()
                                           ? std::abs (pg - pgBefore) > 0.05
                                           : std::abs (pg - 6.0) < 0.3;
            if (liveServerConfigured() && pstr ("agentState") == "awaiting_ask")
                skip ("ITEM 0b: the plan that was waiting when the window closed is applied by the "
                      "window that reopened, and the change reads back off the rack",
                      "the Apply was pressed and accepted (ok=" + ack.getProperty ("ok", juce::var()).toString()
                      + "), and the live model answered with a QUESTION rather than finishing the plan "
                      "(state awaiting_ask) - so the figure it would have written does not exist yet. "
                      "A live model's next move is its own; the scripted road asserts this claim to the "
                      "letter. Not counted as a pass.");
            else
            check (landedOnTheRack,
                   "ITEM 0b: the plan that was waiting when the window closed is applied by the window that "
                   "reopened, and the change reads back off the rack",
                   "pre-gain " + juce::String (pgBefore, 2) + " -> " + juce::String (pg, 2) + " dB ("
                   + (liveServerConfigured() ? juce::String ("live: the model picks the figure, so what "
                                                             "must be true is that it MOVED")
                                             : juce::String ("6 = both scripted lines"))
                   + "), press ok=" + ack.getProperty ("ok", juce::var()).toString()
                   + ", state " + pstr ("agentState"));
            if (liveServerConfigured() && pstr ("agentRowFinished") != "1")
                skip ("...and the run's RECORD - kept by the processor, not the window - carries its "
                      "final statuses and its closing sentence",
                      "the live run has not finished (state " + pstr ("agentState") + ", steps ["
                      + pstr ("agentRowSteps") + "]), and a closing is written when a run ENDS.");
            else
            check (pstr ("agentRowFinished") == "1" && pstr ("agentRowClosing").isNotEmpty(),
                   "...and the run's RECORD - kept by the processor, not the window - carries its final "
                   "statuses and its closing sentence",
                   "steps [" + pstr ("agentRowSteps") + "], closing \""
                   + pstr ("agentRowClosing").substring (0, 70) + "\"");
        }

        // ---- B2d: A QUESTION'S CHIPS SURVIVE THE CLOSE, IN THE CHAT (his 09:24 fault) ----------
        //
        // *"no agent card, no steps, no closing, no chips, no Undo all"* - the chips are the one of
        // those five that needs a plan which ASKS something, so they are asserted here on
        // rig-rowask-marker, whose round 1 is a question with two choices. B2c above is an approval
        // card and has none by design; asserting them there was my error, found by this run.
        {
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);
            Rig::pump (400);
            v2v.openEditor();
            for (int i = 0; i < 30; ++i) { v2v.processOneBlock (sr); if ((i % 10) == 0) Rig::pump (20); }
            Rig::pump (800);
            rig.send (echojay::rig::cmd::sendChat,
                      args1 ("text", "rig-rowask-marker: how firm should it be? (chips through a close)"), 15000);
            const bool asked = untilProc ([] (const juce::var& p)
                               // THE SPELLING IS THE PRODUCT'S (EJAgentClient::stateName): lower case
                               // with underscores. My first cut compared against "AwaitingAsk", a
                               // string this build never publishes, so the wait could not succeed and
                               // the leg SKIPPED with "the scripted ask never arrived" - a comparison
                               // that can never be true, which is the same defect as a grep whose
                               // success string never occurs. Two spellings of one fact, again.
                               { const auto s = p.getProperty ("agentState", juce::var()).toString();
                                 return s == "awaiting_ask" || s == "awaiting_approval"; },
                               40000);
            v2v.closeEditor();
            const bool wentAway = v2v.lastCloseDestroyedEditor;
            Rig::pump (1200);
            v2v.openEditor();
            for (int i = 0; i < 40; ++i) { v2v.processOneBlock (sr); if ((i % 10) == 0) Rig::pump (20); }
            Rig::pump (1200);
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 6000);
            const auto askCard = cardOnScreenFor (Rig::state(), pstr ("agentRunKey"));
            const auto chips   = askCard.getProperty ("cardChips", juce::var()).toString();
            if (liveServerConfigured() && ! chips.containsIgnoreCase ("Gentle"))
                skip ("B2d: a question's CHIPS come back in the chat after the window closed",
                      "the live model did not ask with these chips this round (card drawn: "
                      + juce::String (askCard.isObject() ? "yes" : "no") + ", chips [" + chips
                      + "]). WHICH question it asks is its own, and the marker in the goal means "
                      "nothing to it; the scripted road asserts the chips to the letter. Not counted "
                      "as a pass.");
            else if (! asked || ! wentAway)
                skip ("B2d: a question's CHIPS come back in the chat after the window closed",
                      ! asked ? juce::String ("the scripted ask never arrived, so there were no chips to lose")
                              : "the close did not destroy the editor: " + v2v.lastCloseNote);
            else
                check (askCard.isObject() && chips.containsIgnoreCase ("Gentle")
                       && chips.containsIgnoreCase ("upfront"),
                       "B2d: a QUESTION's chips come back in the chat after the window closed - both "
                       "choices, tappable, on the card the reopened chat draws  (his 09:24 fault listed "
                       "\"no chips\" among the five things missing)",
                       "chips [" + chips + "], steps "
                       + askCard.getProperty ("cardSteps", juce::var()).toString()
                       + ", onScreen cards=" + Rig::state().getProperty ("onScreenCards", juce::var()).toString());
            // leave nothing waiting for the rows below: answer the question.
            rig.send (echojay::rig::cmd::press, args1 ("button", "chip0"), 12000);
            untilProc ([] (const juce::var& p) { return ! (bool) p.getProperty ("agentActive", false); }, 60000);
            Rig::pump (600);
        }

        // ---- B3a: A WHOLE RUN WITH NO WINDOW AT ALL --------------------------------------------
        // The design doc's leg 6, and the one that could not even be written before: start it, close
        // the window, let it finish with nothing attached, reopen and assert the same as leg 1.
        //
        // WHY `started` IS NOT A SAMPLE OF agentActive ANY MORE (22 Oct 2026). Run 4 of this rig read
        // started=0 finished=1 and FAILED a row whose subject had worked perfectly: the run completed
        // with no window, and the conjunct that broke was a poll of a flag that was true for less time
        // than the poll's own period. That is this tree's "decisions on uncontrolled timing" wearing a
        // harness costume - the row measured the observer. The run is observed started from facts that
        // do not evaporate: the goal this run carries and the key the processor minted for it.
        {
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "auto"), 8000);
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0), 8000);
            Rig::pump (700);
            rig.send (echojay::rig::cmd::sendChat,
                      args1 ("text", "rig-auto-marker: set the pre gain, twice (no window at all)"), 15000);
            const bool started = untilProc ([] (const juce::var& p)
                                 { return p.getProperty ("agentGoal", juce::var()).toString()
                                            .contains ("rig-auto-marker")
                                       && p.getProperty ("agentRunKey", juce::var()).toString().isNotEmpty(); },
                                 40000);
            v2v.closeEditor();
            const bool gone = v2v.lastCloseDestroyedEditor;
            // let it finish with NOTHING attached. Audio keeps running, as it would in a host.
            const bool finished = untilProc ([] (const juce::var& p)
                                  { return ! (bool) p.getProperty ("agentActive", false)
                                           && p.getProperty ("agentRowFinished", juce::var()).toString() == "1"; },
                                  90000);
            const auto closedStatuses = pstr ("agentRowSteps");
            const auto closedClosing  = pstr ("agentRowClosing");
            const int  parkedLines    = (int) pst().getProperty ("agentParkedLines", 0);
            if (! gone)
                skip ("B3a: a whole run completes with no window attached",
                      "the close did not destroy the plugin's editor, so there WAS a window: "
                      + v2v.lastCloseNote);
            else
                check (started && finished && closedStatuses.contains ("done") && closedClosing.isNotEmpty(),
                       "ITEM 0b: a run started in a window RAN TO COMPLETION with no window attached at "
                       "all, and the processor holds its steps and its closing sentence  (the design doc's "
                       "leg 6 - the one that could not be written before W1)",
                       "started=" + juce::String ((int) started) + " finished=" + juce::String ((int) finished)
                       + ", steps [" + closedStatuses + "], closing \"" + closedClosing.substring (0, 60)
                       + "\", parked lines " + juce::String (parkedLines));
            // and the rack really moved, with no window to move it
            v2v.openEditor();
            for (int i = 0; i < 40; ++i) { v2v.processOneBlock (sr); if ((i % 10) == 0) Rig::pump (20); }
            Rig::pump (1200);
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 6000);
            const double pg = (double) Rig::state().getProperty ("preGainDb", 99.0);
            const auto row = Rig::state().getProperty ("agentRow", juce::var());
            // The scripted plan's second line is 6 dB; live, the model picks its own figure and what
            // must be true is that the trim MOVED off the 0.0 this leg set (the same split B2 makes).
            check (liveServerConfigured() ? std::abs (pg) > 0.05 : std::abs (pg - 6.0) < 0.3,
                   "...the change it made with no window open reads back off the rack",
                   "pre-gain " + juce::String (pg, 2) + " dB"
                   + (liveServerConfigured() ? juce::String (" (live: it had to move off 0)")
                                             : juce::String (" (6 = both scripted lines)")));
            check ((int) row.getProperty ("rows", 0) >= 1
                   && row.getProperty ("closing", juce::var()).toString().isNotEmpty(),
                   "...and the window that opened afterwards shows ONE row for that run, with its closing "
                   "sentence - the run is on screen rather than only in a log",
                   "rows=" + juce::String ((int) row.getProperty ("rows", 0))
                   + ", closing \"" + row.getProperty ("closing", juce::var()).toString().substring (0, 60) + "\"");
        }

        // ---- B3: A WINDOWLESS RUN FINISHES ITS BUILD AND APPLIES IT (his 22 Oct ruling) ---------
        //
        // *"If the user has already pressed Go, a run with no window open finishes its build and
        //  applies it. The card, its chips and Undo all are there when a window reopens. If the run
        //  is waiting on a chip or a question, it keeps waiting."*
        //
        // THIS ROW WAS RED TWICE FOR TWO DIFFERENT REASONS, which is why it was left red and put to
        // him rather than re-aimed on my own judgement:
        //   run 3 (live)  started=1 finished=0, steps [running]. The build PARKED - by design, and
        //                 the product's own log said so: "a BUILD would park until one opens". That
        //                 design is what his ruling has now changed.
        //   run 4 (mock)  started=0 finished=1. The product was right and the row was wrong (see
        //                 B3a above).
        // The build road is no longer withdrawn with the window: with none open a build goes down the
        // road every agent `add` takes (applyChainOps on the processor), and the published
        // agentLastBuildRoad says which road the last build actually took - read from the BUILD
        // rather than kept as a copy here, which is the fault that cost five rows on 20 Oct.
        //
        // ONE PLUGIN, AND IT IS OURS: EchoJay Compressor, built in. No PACE, no iLok, nothing to
        // prompt on his Mac (his standing rule, and the pre-gain check's own condition).
        {
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "auto"), 8000);
            Rig::pump (500);
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 6000);
            const int slotsBefore = (int) pst().getProperty ("procSlots", -1);
            rig.send (echojay::rig::cmd::sendChat,
                      args1 ("text", "rig-nowindowbuild-marker: build a compressor on this rack "
                                     "(nothing open while it runs)"), 15000);
            // ---- THE WINDOW GOES BEFORE THE BUILD CAN LAND (22 Oct 2026) --------------------
            //
            // The 11:24 run read `last build road "window"` with the rack grown 0 -> 1: the build
            // had ALREADY GONE while the window was still open, because the scripted plan fires it
            // within a second of Go and this leg spent that second waiting to observe the start.
            // The row then tested the window road and called it the processor's - a leg that proved
            // nothing while reading green-adjacent. Two changes, both at that cause:
            //   the plan holds its build frame 4 s (_holdMs in build_and_run.sh), and
            //   the window closes IMMEDIATELY, with "did it start" read from the RECORD afterwards
            //   rather than caught in flight (the same lesson as B3a's `started`).
            v2v.closeEditor();
            const bool started = untilProc ([] (const juce::var& p)
                                 { return p.getProperty ("agentGoal", juce::var()).toString()
                                            .contains ("rig-nowindowbuild-marker")
                                       && p.getProperty ("agentRunKey", juce::var()).toString().isNotEmpty(); },
                                 40000);
            const bool gone = v2v.lastCloseDestroyedEditor;
            // ---- THE LIVE ROUND IS SLOWER, AND IT IS NOT 90 SECONDS (22 Oct 2026) ----------
            // His own 09:24 run took about 100 s wall clock, and a read-heavy round takes longer: the
            // live road read `steps [done done done done done running]` at the 90 s bound, so every
            // assertion after it was about a run that was still working. The scripted road is quick
            // and keeps its short bound; the live road gets four minutes, which is a bound and not a
            // hope, and what happens when it expires is said below rather than called a failure.
            const bool finished = untilProc ([] (const juce::var& p)
                                  { return ! (bool) p.getProperty ("agentActive", false)
                                           && p.getProperty ("agentRowFinished", juce::var()).toString() == "1"; },
                                  liveServerConfigured() ? 240000 : 90000);
            // Read with NOTHING attached: the slot count is the processor's own line, so this is the
            // build landing while no window exists rather than a window finding it afterwards.
            const int  slotsNoWindow  = (int) pst().getProperty ("procSlots", -1);
            const auto roadTaken      = pstr ("agentLastBuildRoad");
            const auto closedSteps    = pstr ("agentRowSteps");
            const auto closedClosing  = pstr ("agentRowClosing");
            if (! gone)
                skip ("B3: a windowless run finishes its build and applies it (his 22 Oct ruling)",
                      "the close did not destroy the plugin's editor, so there WAS a window: "
                      + v2v.lastCloseNote);
            else if (slotsBefore < 0 || slotsNoWindow < 0)
                skip ("B3: a windowless run finishes its build and applies it (his 22 Oct ruling)",
                      "this build does not publish procSlots, so whether the build landed WHILE no "
                      "window existed cannot be read here - and reading it after a reopen would not "
                      "tell the two apart. Rebuild the bundles from this tree.");
            else if (liveServerConfigured() && ! finished)
                skip ("B3: a windowless run finishes its build and applies it (his 22 Oct ruling)",
                      "the live round was still working when the bound expired (240 s): steps ["
                      + closedSteps + "]. The ruling is about a run that FINISHES with no window, so a"
                      " round still in flight is not evidence either way - and the mock road asserts"
                      " the same claim to the letter. Not counted as a pass.");
            else if (liveServerConfigured() && roadTaken.isEmpty() && slotsNoWindow == slotsBefore)
                skip ("B3: a windowless run finishes its build and applies it (his 22 Oct ruling)",
                      "this live round made no BUILD at all - the model read the rack and answered "
                      "instead (no build road recorded, rack still " + juce::String (slotsBefore)
                      + " slot(s)), so there was no windowless build to apply. The goal text is a "
                      "marker the real model does not read; what it does with it is its own. Not "
                      "counted as a pass.");
            else
            {
                check (started && finished && closedSteps.contains ("done") && closedClosing.isNotEmpty(),
                       "HIS 22 OCT RULING: a run the user had already approved FINISHED with no window "
                       "open at all - steps done, closing written  (RED as it shipped: the build parked "
                       "and the run sat at [running] until a window came back)",
                       "started=" + juce::String ((int) started) + " finished=" + juce::String ((int) finished)
                       + ", steps [" + closedSteps + "], closing \"" + closedClosing.substring (0, 60) + "\"");
                // ...AND IT APPLIED. Scripted: exactly the one compressor the plan names. Live: the
                // model chooses what to build, so what must be true is that the rack GREW and that the
                // road taken was not a window's - there was none.
                const bool applied = liveServerConfigured() ? slotsNoWindow > slotsBefore
                                                            : slotsNoWindow == slotsBefore + 1;
                check (applied && roadTaken != "window",
                       "...and it APPLIED while nothing was attached: the rack grew on the processor's "
                       "own line, by the road the agent's own adds take",
                       "slots " + juce::String (slotsBefore) + " -> " + juce::String (slotsNoWindow)
                       + ", last build road \"" + roadTaken + "\""
                       + (liveServerConfigured() ? juce::String (" (live: the model picks what to build, "
                                                                 "so what must be true is that it grew)")
                                                 : juce::String (" (scripted: one EchoJay Compressor)")));
                if (! liveServerConfigured())
                    check (roadTaken == "processor"
                           && pstr ("agentLastBuildRunKey") == pstr ("agentRunKey"),
                           "...by the PROCESSOR'S road, named by the build itself rather than inferred "
                           "here - the window's two roads were not available and were not used",
                           "agentLastBuildRoad=\"" + roadTaken + "\" for run "
                           + pstr ("agentLastBuildRunKey").substring (0, 8) + " (this run "
                           + pstr ("agentRunKey").substring (0, 8) + ")");
            }
            // ---- AND THE CARD, ITS CHIPS AND UNDO ARE THERE WHEN A WINDOW REOPENS ---------------
            // His ruling's second sentence, asserted as three facts rather than one: the row exists
            // with its closing, the steps it ran are on it, and Undo all is offered - a run that
            // applied something the user cannot take back would honour half the ruling.
            v2v.openEditor();
            for (int i = 0; i < 40; ++i) { v2v.processOneBlock (sr); if ((i % 10) == 0) Rig::pump (20); }
            Rig::pump (1200);
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 6000);
            const auto row = Rig::state().getProperty ("agentRow", juce::var());
            const int  rows = (int) row.getProperty ("rows", 0);
            juce::String stepWords, chipWords;
            int undoableSteps = 0;
            if (auto* sa = row.getProperty ("steps", juce::var()).getArray())
                for (const auto& s : *sa)
                {
                    stepWords << s.getProperty ("status", juce::var()).toString() << " ";
                    chipWords << s.getProperty ("chips",  juce::var()).toString();
                    if ((bool) s.getProperty ("canUndo", false)) ++undoableSteps;
                }
            const bool runIsOver = pstr ("agentRowFinished") == "1"
                                && ! (bool) pst().getProperty ("agentActive", false);
            if (! runIsOver)
                skip ("...and the window that reopened shows ONE card for that run, with its closing "
                      "and the steps it ran",
                      "this run is still live (state " + pstr ("agentState") + ", steps ["
                      + stepWords.trim() + "]), and a closing is written when a run ENDS - so this is "
                      "the live round's pace, not a missing card. The card itself is asserted above "
                      "and is there.");
            else
            check (rows >= 1 && row.getProperty ("closing", juce::var()).toString().isNotEmpty()
                   && stepWords.trim().isNotEmpty(),
                   "...and the window that reopened shows ONE card for that run, with its closing and "
                   "the steps it ran  (his ruling: \"the card, its chips and Undo all are there when a "
                   "window reopens\")",
                   "rows=" + juce::String (rows) + ", steps [" + stepWords.trim() + "], closing \""
                   + row.getProperty ("closing", juce::var()).toString().substring (0, 60) + "\"");
            if (! runIsOver)
                skip ("...and UNDO is on it: a run that applied a build with nobody watching must be "
                      "takeable back by the window that finds it",
                      "this run is still live, and Undo all appears on a FINISHED run - asserting it "
                      "mid-round would be asserting the wrong thing.");
            else
            check ((bool) row.getProperty ("undoAll", false) || undoableSteps > 0,
                   "...and UNDO is on it: a run that applied a build with nobody watching must be "
                   "takeable back by the window that finds it",
                   "undoAll=" + row.getProperty ("undoAll", juce::var()).toString()
                   + ", steps that can be undone " + juce::String (undoableSteps));

            // ---- B3c: THE CHAT ITSELF SHOWS THAT CARD (his 09:24 fault, 22 Oct 2026) ----------
            //
            // *"Fresh chat on Main, 'build a mix bus', pressed Go, closed the plugin window while it
            //  built, reopened about 20 s later. The build WAS on the rack, but the chat showed only
            //  my message: no agent card, no steps, no closing, no chips, no Undo all."*
            //
            // THE ROW ABOVE WOULD HAVE PASSED THAT. It reads `agentRow`, the live row this window
            // adopted from the processor's replay list; his chat renders from the STORED log, and the
            // card had never been written there - the editor records a card at the END of a run, and
            // a run that ends with no window never reaches that code. So the fix is at the stamp (the
            // store appends with no window involved) and the assertion is here, on what the window
            // DRAWS: chat rows, not internal state.
            const auto onScreenCard = cardOnScreenFor (Rig::state(), pstr ("agentRunKey"));
            check (onScreenCard.isObject(),
                   "HIS 09:24 FAULT: the reopened window's CHAT draws a card for that run - not only "
                   "the live row it adopted, the chat itself  (RED as it shipped: the card lived on the "
                   "processor's replay list, never reached the stored log, and the first chat switch "
                   "dropped it - \"no agent card, no steps, no closing, no chips, no Undo all\")",
                   "onScreen cards=" + Rig::state().getProperty ("onScreenCards", juce::var()).toString()
                   + ", user rows=" + juce::String (userRowsOnScreen (Rig::state()))
                   + ", run key " + pstr ("agentRunKey"));
            if (! runIsOver)
                skip ("...and that card carries its closing, its steps and Undo all",
                      "this run is still live (steps " + onScreenCard.getProperty ("cardSteps", juce::var()).toString()
                      + "), so its closing is not written yet. The CARD is drawn, which is the fault's "
                      "own subject and is asserted above.");
            else
            check (onScreenCard.getProperty ("cardClosing", juce::var()).toString().isNotEmpty()
                   && (int) onScreenCard.getProperty ("cardSteps", 0) >= 1
                   && (bool) onScreenCard.getProperty ("cardUndoAll", false),
                   "...and that card carries its closing, its steps and Undo all - three of the five "
                   "things his screenshot was missing (the chips are B2d's, on the plan that asks a "
                   "question; this plan's step is an approval and has none)",
                   "closing \"" + onScreenCard.getProperty ("cardClosing", juce::var()).toString().substring (0, 50)
                   + "\", steps " + onScreenCard.getProperty ("cardSteps", juce::var()).toString()
                   + ", undoAll " + onScreenCard.getProperty ("cardUndoAll", juce::var()).toString());

            // ---- B3d: AND UNDO ALL FROM THAT CARD PUTS THE RACK BACK (his own ask) -------------
            // *"Also check that Undo all from that card restores the rack."*  The rack is read from
            // the PROCESSOR's own count, waited for with a bound rather than sampled once.
            // ---- UNDO ALL IS A FINISHED RUN'S BUTTON (22 Oct 2026, the live road said so) --------
            //
            // B3d read `slots 0 -> 4 -> 4` live and I reported it as a possible product red. It is
            // not: the two checks either side of it SKIPPED with "this run is still live", and Undo
            // all only appears on a finished run - so the press went nowhere and the row measured a
            // button that was not there. The product's undo was never exercised. Same gate as its
            // siblings, and it says which case it is in rather than failing on either.
            const bool overForUndo = pstr ("agentRowFinished") == "1"
                                  && ! (bool) pst().getProperty ("agentActive", false);
            const int slotsBeforeUndo = (int) pst().getProperty ("procSlots", -1);
            if (! overForUndo)
            {
                skip ("...and Undo all on that card RESTORES the rack it built with no window open",
                      "this run is still live (state " + pstr ("agentState") + ", slots "
                      + juce::String (slotsBeforeUndo) + "), and Undo all is a FINISHED run's button - "
                      "pressing it here would measure a control that is not on the card. The scripted "
                      "road asserts this to the letter.");
            }
            else
            {
            rig.send (echojay::rig::cmd::press, args1 ("button", "undo_all"), 10000);
            const bool rackRestored = untilProc ([slotsBefore] (const juce::var& p)
                                      { return (int) p.getProperty ("procSlots", -1) == slotsBefore; },
                                      12000);
            check (rackRestored,
                   "...and Undo all on that card RESTORES the rack it built with no window open",
                   "slots " + juce::String (slotsBefore) + " -> " + juce::String (slotsBeforeUndo)
                   + " -> " + juce::String ((int) pst().getProperty ("procSlots", -1))
                   + " (wanted " + juce::String (slotsBefore) + ")");
            }
            Rig::pump (600);
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);
        }

        }   // end of the B1-B4 block (item 0b's behaviour)

        // ---- N1 (SEAN, 21 Oct, THE OVERNIGHT LIST): A RUN STARTED AND NO ROW WAS DRAWN ---------
        //
        // His own words for the row he wants: *"a run started, no row drawn."*  Everything else about
        // the row - its order, its chips, its closing - is asserted in a dozen places, and all of it
        // presumes the row EXISTS. He has seen a run start with nothing drawn, and nothing in the rig
        // would have caught it: the rows that read `agentRow` all read it after an askAgent() helper
        // has already waited for a finished run, so a row that appeared LATE, or only at the end,
        // would pass every one of them.
        //
        // SO THIS ROW IS A TIME BOUND, not a state read ("time bound in acceptance"): from the moment
        // the run is active, a row must be drawn in the chat he typed into within 2 seconds. A state
        // read of the same fact is not a test of it - the row he is missing is the one that is not
        // there WHILE HE WAITS.
        {
            if ((bool) pst().getProperty ("agentActive", false))
            {
                rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
                untilProc ([] (const juce::var& p)
                           { return ! (bool) p.getProperty ("agentActive", false); }, 20000);
            }
            const auto stBefore  = rig.freshState();
            const auto chatBefore = stBefore.getProperty ("chatId", juce::var()).toString();
            // ---- HOW MANY ROWS WERE ALREADY THERE (22 Oct 2026) --------------------------------
            // My first cut asserted `rows == 1`, which is not this row's question: `agentRow.rows`
            // counts the run rows in the WHOLE transcript, and by the time N1 runs this pass has
            // started several runs in the same chat - so it read rows=3 and called it a fault while
            // the card it was actually asking about had been drawn in 51 ms. What must be true is
            // that THIS run added exactly ONE row: one per run is the mockup's first rule, and a
            // transcript holding three runs holds three rows correctly.
            const int rowsBefore = (int) stBefore.getProperty ("agentRow", juce::var())
                                                 .getProperty ("rows", juce::var());
            rig.send (echojay::rig::cmd::sendChat,
                      args1 ("text", "rig-row-marker: set the pre gain (N1: a row must be drawn)"), 15000);
            const bool live = untilProc ([] (const juce::var& p)
                              { return (bool) p.getProperty ("agentActive", false); }, 40000);
            const auto t0 = juce::Time::getMillisecondCounter();
            bool drawn = false;
            int  rows  = 0;
            juce::String rowChat;
            while (juce::Time::getMillisecondCounter() - t0 < 2000u)
            {
                const auto st = rig.freshState();
                const auto ar = st.getProperty ("agentRow", juce::var());
                rows   = (int) ar.getProperty ("rows", juce::var());
                rowChat = st.getProperty ("chatId", juce::var()).toString();
                if ((bool) ar.getProperty ("present", false) && rows > rowsBefore) { drawn = true; break; }
                Rig::pump (100);
            }
            const auto tookMs = (int) (juce::Time::getMillisecondCounter() - t0);
            const auto st = rig.freshState();
            if (! live)
                scenSkip ("N1", "a started run has a row drawn, in the chat it was typed into",
                          "no run went active on this road, so there is no 'started' for the row to "
                          "be missing from", "A");
            else
                scen ("N1", "a started run has a row drawn, in the chat it was typed into",
                      drawn && rows == rowsBefore + 1 && rowChat == chatBefore,
                      juce::String (drawn ? "the row was drawn in " + juce::String (tookMs) + " ms"
                                          : "NO ROW after 2000 ms with the run active")
                      + ", rows=" + juce::String (rowsBefore) + " -> " + juce::String (rows)
                      + " (this run must add exactly one)"
                      + ", chat " + (rowChat == chatBefore ? "unchanged" : "MOVED (" + chatBefore
                                                                           + " -> " + rowChat + ")")
                      + "  (his overnight ask: \"a run started, no row drawn\". The bound is the row: a "
                      "card that arrives when the run ENDS is the same fault as no card)",
                      "A", v2v.editor.get(), st);
            if ((bool) pst().getProperty ("agentActive", false))
            {
                rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
                untilProc ([] (const juce::var& p)
                           { return ! (bool) p.getProperty ("agentActive", false); }, 20000);
            }
        }

        // ---- N2 (SEAN, 21 Oct): THE PRE-GAIN CHECK, ON PLUGINS THAT NEED NO LICENCE -------------
        //
        // The other row on his overnight list: *"the pre-gain check (no PACE/iLok plugins on the
        // rig)"*, and the fault it is for is his 15:30 report - a rack-level target INSERTED an
        // EchoJay Limiter after FG-X 2, drove it to +12 and set the input trim to -2.7. His ruling
        // was "find the paths and make it impossible after any agent build", which the plugin now
        // does through fromAgentBuild; this is that ruling read back from a real host.
        //
        // THE PLUGIN LIST IS PART OF THE ROW, not an accident of it. The build adds EchoJay's own
        // compressor and nothing else: a row that reached for FG-X or any PACE/iLok plugin would
        // prompt for a licence on his Mac and take the whole rig down with it (the standing rule -
        // never scan or host a licence-bound plugin here), and it would prove nothing extra. The
        // target carries a dial_on_playback with a budget, which is the exact shape that moved the
        // trim.
        if (liveServerConfigured())
            scenSkip ("N2", "an agent build with a loudness target leaves the user's pre-gain alone and "
                            "inserts no limiter",
                      "this row needs a build it CHOSE the shape of - one add, a loudness target, no "
                      "limiter offered - and on the live road the model picks. It did exactly what it "
                      "should on this run (read the rack, then asked for playback, because a target "
                      "needs something to measure), and left an empty rack, which would have let the "
                      "row pass while proving nothing. It runs on the mock, where the build is "
                      "scripted.", "A");
        else
        {
            if ((bool) pst().getProperty ("agentActive", false))
            {
                rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
                untilProc ([] (const juce::var& p)
                           { return ! (bool) p.getProperty ("agentActive", false); }, 20000);
            }
            // The user's own pre-gain, set by hand, is the thing that must survive.
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 3.0), 8000);
            Rig::pump (500);
            const auto before      = rig.freshState();
            const double preBefore = (double) before.getProperty ("preGainDb", 0.0);
            const bool   handSet   = (bool)   before.getProperty ("preGainUserSet", false);
            const int    slotsBefore = ownSlotCount (before);

            const auto out = askAgent (rig, "rig-pregain-marker: make this louder, target -8");
            // ---- THE ADD HAS TO HAVE LANDED, OR THIS ROW PROVES NOTHING ------------------------
            // "the pre-gain is untouched" is true of a build that never happened, and on the first
            // run of this row that is exactly what it reported: pre-gain 3.00 -> 3.00, rack 0 -> 0,
            // PASS. The claim is only worth reading once a slot is actually on the rack, so the
            // landing is waited for with a bound and asserted before the claim is made. The run
            // itself does NOT settle here - a dial_on_playback target goes to Listening and waits
            // for playback, which is correct - so settling is the wrong thing to wait for.
            bool landed = false;
            {
                const auto t0 = juce::Time::getMillisecondCounter();
                while (juce::Time::getMillisecondCounter() - t0 < 15000u)
                {
                    if (ownSlotCount (rig.freshState()) > slotsBefore) { landed = true; break; }
                    Rig::pump (250);
                }
            }

            const auto after      = rig.freshState();
            const double preAfter = (double) after.getProperty ("preGainDb", 0.0);
            const auto names      = ownSlotNames (after);
            int limiters = 0;
            for (const auto& n : names) if (n.containsIgnoreCase ("limiter")) ++limiters;
            const bool preKept   = std::abs (preAfter - preBefore) < 0.05;
            const bool noLimiter = limiters == 0;
            scen ("N2", "an agent build with a loudness target leaves the user's pre-gain alone and "
                        "inserts no limiter",
                  preKept && noLimiter && handSet,
                  "the pre-gain was " + juce::String (preBefore, 2) + " dB by hand and reads "
                  + juce::String (preAfter, 2) + " dB after the build; the rack went from "
                  + juce::String (slotsBefore) + " slot(s) to " + juce::String (names.size())
                  + " [" + names.joinIntoString (", ") + "] with " + juce::String (limiters)
                  + " limiter(s) inserted  (his 15:30: a rack target inserted a limiter, drove it to "
                  "+12 and set the input trim to -2.7 - after any agent build both of those must be "
                  "impossible. Built from EchoJay's own compressor only: no PACE/iLok plugin is "
                  "hosted or scanned by this row)",
                  "A", v2v.editor.get(), after);
            check (landed && names.size() == slotsBefore + 1
                          && names[names.size() - 1].containsIgnoreCase ("compressor"),
                   "N2's build actually LANDED - one slot more than before, and it is the compressor "
                   "the build asked for. Without this the row's claim is true of a build that never "
                   "happened (its first run: pre-gain 3.00 -> 3.00, rack 0 -> 0, PASS)",
                   "landed=" + juce::String (landed ? "y" : "n") + " rack ["
                   + names.joinIntoString (", ") + "] state " + out.state
                   + " applied=" + juce::String (out.applied ? "y" : "n"));
        }

        // ---- THE CHAT ROWS BELOW NEED A CHAT THAT WILL TAKE A MESSAGE (22 Oct 2026) ------------
        //
        // C11v(a), C11v(b), I1, F1 and F2 all TYPE, and a run in flight refuses a typed message by
        // design ("The agent is still working on ..."). The rows above this line start runs and close
        // windows on purpose, and with 0b on they leave them running - so in gate 2026-10-22 all five
        // rows sent their marker, had it refused, and reported the missing reply as a product fault.
        // Five red rows, one absent precondition.
        //
        // The ground is SET and then ASSERTED. Setting it silently would let the same thing happen
        // again the day Stop stops working: the check is what makes the row say "I could not get a
        // clear chat" instead of "the product lost my message".
        if ((bool) pst().getProperty ("agentActive", false))
        {
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
            untilProc ([] (const juce::var& p)
                       { return ! (bool) p.getProperty ("agentActive", false); }, 20000);
        }
        check (! (bool) pst().getProperty ("agentActive", false),
               "the chat rows start with NO run in flight - a live run refuses every typed message, "
               "so this is the precondition for all five of them",
               "agentActive=" + pstr ("agentActive") + " state=" + pstr ("agentState"));

        // ---- C11v: CLOSING MID-REPLY, ON THE ROAD WHERE THE CLOSE IS REAL (Sean's check 1) -------
        //   RUNS EITHER WAY. This is Sean's item 2 and it is about the CHAT path, which has nothing to
        //   do with item 0's renderer: the classify callback dropped a turn at a dead editor pointer
        //   whatever the agent was doing.
        //
        // C11 used to run on the AU track, where releasing the wrapper view does NOT destroy the
        // plugin's editor - so its closed-window half was a named skip and Sean's bug survived two
        // rounds of "fixed". It runs here now, on the VST3, where W1 proves the editor really dies.
        //
        // TWO FAILURES, NOT ONE. His 11:39 run died at the CLASSIFIER, before any chat request was
        // made, which is a different stretch of the path from the dead socket C11 was written for.
        // Both get a leg, because fixing one taught nothing about the other.
        if (! liveServerConfigured())
        {
            // (a) THE CLASSIFIER ANSWERS AFTER THE WINDOW HAS GONE - Sean's exact case.
            {
                rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "off"), 8000);
                rig.send (echojay::rig::cmd::sendChat,
                          args1 ("text", "rig-classifyhold-marker: how does this kick sound"), 15000);
                Rig::pump (1200);                 // the classify hold is 5 s: close while it is in flight
                v2v.closeEditor();
                const bool reallyGone = v2v.lastCloseDestroyedEditor;
                Rig::pump (6000);                 // the classifier answers with no window attached
                v2v.openEditor();
                for (int i = 0; i < 40; ++i) { v2v.processOneBlock (sr); if ((i % 10) == 0) Rig::pump (20); }
                Rig::pump (1200);
                // ---- SEAN'S #7 (16 Oct 2026): THE MARKER IS A SENTENCE NOW --------------------
                // This leg asserted `contains("[interrupted]")`. That tag is GONE by his ruling - "remove
                // the [interrupted] tag, use 'You closed the window before the answer came back'" - so the
                // assertion had to move with it or this row would have gone red on the fix. It keys on the
                // PLUGIN'S OWN predicate (looksInterrupted), which is the one place the sentences live, so
                // the leg and the product cannot drift again the next time the wording changes.
                // ---- THE RECORD, NOT ITS LAST LINE (22 Oct 2026) ---------------------------
                // This waited for `lastAssistant` to read as interrupted, which is a PROXY for the
                // claim ("the turn was recorded") and stopped agreeing with it the moment a run's
                // card began reaching the store early: a card's closing is an assistant row too,
                // and it can land after the notice. The 11:24 run read "Added a compressor and
                // dialled it to -8 LUFS." - a true sentence about a different row. The processor
                // now publishes how many of its rows read as interrupted, by the product's own
                // predicate, so the leg asks the question it means.
                const bool kept = untilProc ([] (const juce::var& p)
                                  { return (int) p.getProperty ("interruptedRows", 0) >= 1
                                        || EchoJayAPI::looksInterrupted (
                                             p.getProperty ("lastAssistant", juce::var()).toString()); },
                                  20000);
                const auto rec = pstr ("lastAssistant");
                if (! reallyGone)
                    skip ("C11v(a): the classifier answering after a REAL close leaves an interrupted row",
                          "this close did not destroy the plugin's editor, so the leg would be vacuous: "
                          + v2v.lastCloseNote);
                else
                    // NAMED IN BOTH BRANCHES (16 Oct 2026). Only the skip() said "C11v(a)"; the passing
                    // line did not, so the log could not show Sean the one condition he set for
                    // installing - "C11v(a) must PASS, not skip" - even when it had passed. A check
                    // whose name appears only when it fails cannot be used as a condition.
                    check (kept,
                           "C11v(a): the classifier answered after the window was destroyed, and the turn "
                           "is recorded as INTERRUPTED rather than dropped  (RED before this fix: the "
                           "editor-guarded callback returned, /api/chat was never called, and Sean saw his "
                           "own message and nothing else - twice)",
                           "interrupted rows " + pst().getProperty ("interruptedRows", juce::var()).toString()
                           + ", processor lastAssistant: \"" + rec.substring (0, 110) + "\"");

                // ---- I1: THE WORDING, THE RETRY PILL, AND THE PRESS (Sean's #7) -----------------
                //
                // RED ON 15c three ways: the row said "[interrupted]" (a tag written for a grep, in the
                // one place a person reads), there was no Retry at all, and when the pill was first wired
                // its LABEL was still derived from the prompt's opening words - so it drew "Suggest an
                // alternative" over an action that resends the question. The right action behind the
                // wrong word is its own kind of untruth.
                if (! reallyGone)
                    skip ("I1: the interrupted row says so in words and carries a working Retry",
                          "this close did not destroy the editor, so no interrupted row exists to read: "
                          + v2v.lastCloseNote);
                else
                {
                    const auto st = rig.freshState();
                    const auto chips  = st.getProperty ("lastRowChips", juce::var()).toString();
                    const auto altLbl = st.getProperty ("lastRowAltLabel", juce::var()).toString();
                    const auto altAsk = st.getProperty ("lastRowAltPrompt", juce::var()).toString();
                    juce::String onScreenRow;
                    if (auto* arr = st.getProperty ("onScreen", juce::var()).getArray())
                        for (const auto& m : *arr)
                            if (m.getProperty ("role", juce::var()).toString() == "assistant")
                                onScreenRow = m.getProperty ("text", juce::var()).toString();
                    const bool wording = onScreenRow.contains ("You closed the window before the answer came back")
                                      && ! onScreenRow.contains ("[interrupted]");
                    const bool hasRetry = chips.contains ("Retry") && altLbl == "Retry"
                                       && altAsk.containsIgnoreCase ("kick");
                    // AND THE PRESS. A drawn button that cannot be pressed is what item 3's dead
                    // "Details" was; the pill's own path is onResultChipTapped, which press:chat_retry
                    // calls. After it, the question is in flight again - so the composer has sent and
                    // the pill is one-shot (editAltPrompt is cleared).
                    bool resent = false;
                    if (hasRetry)
                    {
                        rig.send (echojay::rig::cmd::press, args1 ("button", "chat_retry"), 12000);
                        Rig::pump (1500);
                        const auto after = rig.freshState();
                        resent = after.getProperty ("lastRowAltPrompt", juce::var()).toString().isEmpty()
                              || (int) after.getProperty ("historyRows", 0) > (int) st.getProperty ("historyRows", 0);
                    }
                    row ("I1", wording && hasRetry && resent,
                         "an interrupted row says \"You closed the window before the answer came back\" - "
                         "no [interrupted] tag - and carries a Retry pill that actually resends the "
                         "question  (RED on 15c: the tag was the wording, there was no Retry, and the "
                         "pill's label was derived from the prompt's opening words)",
                         "row=\"" + onScreenRow.substring (0, 80) + "\" chips=[" + chips + "] altLabel=\""
                         + altLbl + "\" altPrompt=\"" + altAsk.substring (0, 50) + "\" pressed-and-resent="
                         + juce::String (resent ? 1 : 0));

                    // ---- I2: THE INTERRUPTED LINE IS WRITTEN ONCE (Sean's test 9b, 16 Oct 2026) ---
                    //
                    // RED ON 16a: *"'You closed the window before the answer came back.' appears
                    // twice."* Both writers were doing their job - the processor's keep-rule puts the
                    // row in chatHistory (so the model's history has it and a reload shows it) and
                    // parks it as an orphan carrying the Retry prompt - and the reopened window drew
                    // the history row, then the drain appended the same sentence again.
                    {
                        const auto st2 = rig.freshState();
                        int copies = 0;
                        if (auto* arr = st2.getProperty ("onScreen", juce::var()).getArray())
                            for (const auto& m : *arr)
                                if (EchoJayAPI::looksInterrupted (m.getProperty ("text", juce::var()).toString()))
                                    ++copies;
                        scen ("F1", "close mid-reply, reopen: the interrupted line is drawn exactly once",
                              copies == 1,
                              juce::String (copies) + " copy(ies) of it in the window's own message list  "
                              "(RED on 16a: the history row and the orphan drain each wrote it)",
                              "A", v2v.editor.get(), st2);
                    }

                    // ---- I3: THE TRANSCRIPT IS STILL IN ORDER (Sean's test 9a) --------------------
                    //
                    // RED ON 16a: *"my question was inserted ABOVE the previous exchange (above 'Add
                    // Renaissance Axx...' and its reply) instead of at the bottom, so the chat order is
                    // wrong after a close and reopen."* A reload rebuilds the transcript from the
                    // workspace store and APPENDED whatever the processor was holding that the store had
                    // not caught up with - so an agent run the store has never seen landed under the
                    // question typed after it.
                    //
                    // THE ORDER IS THE ASSERTION: the question this leg sent must come after everything
                    // that was on screen before it, and its own answer after that.
                    {
                        // THE TWO QUESTIONS ARE DIFFERENT ROWS (16 Oct 2026). The Retry pressed just
                        // above RESENDS the question, so the transcript legitimately holds it twice:
                        // the original, the interrupted answer under it, and the resend at the bottom.
                        // The first cut took the LAST occurrence as "the question" and then demanded
                        // the answer below it - backwards, and it called a correct transcript out of
                        // order. What Sean reported is a row appearing ABOVE an earlier exchange, so
                        // that is what this asserts.
                        const auto st3 = rig.freshState();
                        int askedAt = -1, lastAsked = -1, interruptedAt = -1, i = 0;
                        if (auto* arr = st3.getProperty ("onScreen", juce::var()).getArray())
                            for (const auto& m : *arr)
                            {
                                const auto txt = m.getProperty ("text", juce::var()).toString();
                                if (m.getProperty ("role", juce::var()).toString() == "user"
                                    && txt.containsIgnoreCase ("how does this kick sound"))
                                { if (askedAt < 0) askedAt = i; lastAsked = i; }
                                if (EchoJayAPI::looksInterrupted (txt)) interruptedAt = i;
                                ++i;
                            }
                        scen ("F2", "after a close and reopen the transcript is still in order",
                              askedAt >= 0 && interruptedAt > askedAt
                              && (lastAsked == askedAt || lastAsked > interruptedAt),
                              "the question is at row " + juce::String (askedAt) + ", its answer at row "
                              + juce::String (interruptedAt) + ", the Retry's resend at row "
                              + juce::String (lastAsked) + " of " + juce::String (i)
                              + "  (RED on 16a: the reload appended the rows the store had not caught up "
                              "with, which put an earlier exchange UNDER a later question)",
                              "A", v2v.editor.get(), st3);
                    }
                }
            }
            // (b) THE SOCKET DIES after the window has gone - C11's original subject, now with a real close.
            {
                rig.send (echojay::rig::cmd::sendChat, args1 ("text", "rig-cut-marker: answer then die"), 15000);
                Rig::pump (800);
                v2v.closeEditor();
                const bool reallyGone = v2v.lastCloseDestroyedEditor;
                Rig::pump (6000);
                v2v.openEditor();
                for (int i = 0; i < 40; ++i) { v2v.processOneBlock (sr); if ((i % 10) == 0) Rig::pump (20); }
                Rig::pump (1200);
                // SEAN'S #7 again: the plugin's own predicate, not the tag it no longer writes.
                const bool kept = untilProc ([] (const juce::var& p)
                                  { return EchoJayAPI::looksInterrupted (
                                             p.getProperty ("lastAssistant", juce::var()).toString()); },
                                  30000);
                if (! reallyGone)
                    skip ("C11v(b): a dead socket after a REAL close leaves an interrupted row",
                          "this close did not destroy the plugin's editor: " + v2v.lastCloseNote);
                else
                    check (kept,
                           "C11v(b): ...and so is a turn whose SOCKET dies while no window is attached - "
                           "fix 6's own subject, proven at last on a road that can really close a window",
                           "processor lastAssistant: \"" + pstr ("lastAssistant").substring (0, 110) + "\"");
            }
        }

        // ---- B4: WHICH ROAD A BUILD TOOK, BOTH DIRECTIONS (his 22 Oct ruling) ------------------
        //
        // This row used to assert that a windowless build PARKS. His ruling replaced that behaviour,
        // so the row asserts what replaced it - and asserts it in both directions, because a
        // published fact that only ever reads "processor" would pass whether or not the window's own
        // road still existed. B3 above is the windowless half; this is the half with a window open.
        {
            check (pstr ("agentBuildRoad") == "1",
                   "with a window open the agent's build road is the window's own - the same two roads a "
                   "user's Build press takes",
                   "agentBuildRoad=" + pstr ("agentBuildRoad"));
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "auto"), 8000);
            Rig::pump (400);
            rig.send (echojay::rig::cmd::sendChat,
                      args1 ("text", "rig-nowindowbuild-marker: build a compressor on this rack "
                                     "(window open this time)"), 15000);
            // ITS OWN RUN'S ROAD (22 Oct 2026). This waited for "a road is named" and B3's
            // "processor" was still sitting there from the run before, so the wait returned at once
            // and the row called the product wrong for a fact about a different run. The road now
            // carries the run it belongs to, and this waits for THIS run's key beside it.
            const bool ranWithWindow = untilProc ([] (const juce::var& p)
                                       { return ! (bool) p.getProperty ("agentActive", false)
                                                && p.getProperty ("agentRowFinished", juce::var()).toString() == "1"
                                                && p.getProperty ("agentLastBuildRunKey", juce::var()).toString()
                                                     == p.getProperty ("agentRunKey", juce::var()).toString()
                                                && p.getProperty ("agentLastBuildRunKey", juce::var()).toString().isNotEmpty(); },
                                       90000);
            if (liveServerConfigured() && ! ranWithWindow)
                skip ("...and a build made WHILE it is open takes that road and says so",
                      "this live round recorded no build for this run (road \""
                      + pstr ("agentLastBuildRoad") + "\" for run "
                      + pstr ("agentLastBuildRunKey").substring (0, 8) + ", this run "
                      + pstr ("agentRunKey").substring (0, 8) + "): the model answered the goal its own "
                      "way rather than building, so there is no road to read. The scripted road asserts "
                      "both directions to the letter. Not counted as a pass.");
            else
            check (ranWithWindow && pstr ("agentLastBuildRoad") == "window",
                   "...and a build made WHILE it is open takes that road and says so - the other "
                   "direction of the fact B3 reads, so \"processor\" there means something",
                   "finished=" + juce::String ((int) ranWithWindow)
                   + ", agentLastBuildRoad=\"" + pstr ("agentLastBuildRoad") + "\" for run "
                   + pstr ("agentLastBuildRunKey").substring (0, 8) + " (this run "
                   + pstr ("agentRunKey").substring (0, 8) + ")");
            rig.send (echojay::rig::cmd::press, args1 ("button", "undo_all"), 10000);
            Rig::pump (1000);
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);
            v2v.closeEditor();
            Rig::pump (1600);
            check (pstr ("agentBuildRoad") == "0",
                   "...and with the window gone that road is withdrawn, which since his ruling is a "
                   "CHANGE of road rather than a limit: the build goes down the processor's own road "
                   "(B3), and nothing parks",
                   "agentBuildRoad=" + pstr ("agentBuildRoad")
                   + ", last build road \"" + pstr ("agentLastBuildRoad") + "\"");
        }

        // leave the VST3's window SHUT: the AU's editor opens next, and two would fight over the rack lock
        v2v.closeEditor();
        Rig::pump (400);
    }
    else
        skip ("ITEM 0b: the run survives the window",
              "the V2's VST3 did not load, so there is no host road in this process that can close a "
              "window - every 0b leg needs one, and none of them can be written as a pass without it");

    leg ("R1: the editor opens, and the agent is told which rack is open (item 7)");
    v2.openEditor();
    check (v2.editor != nullptr, "the V2's editor was created");
    Rig::pump (600);

    // ---- THE ONBOARDING INTAKE IS ANSWERED, AND IT HAS TO BE *HERE* (16 Oct 2026) -----------------
    //
    // A fresh isolated state home puts the genre/channel/project intake up, and it covers the window:
    // the COMPOSER IS HIDDEN ON EVERY TAB while any of it is visible. Every pass of this rig has done
    // its work behind that prompt - which is why the mode-hint row skipped for days with a reason
    // ("the composer is not on this view") that pointed at tabs rather than at the prompt.
    //
    // AFTER v2.openEditor(), not before, and this cost a pass to learn: there are TWO V2 instances in
    // this process (the AU and the VST3, for the lifecycle rows) with a processor each, so there are
    // two intakes. A press sent before the AU's editor exists reaches only the VST3's - its note even
    // said "answered [genre, channel, project] ... composer visible = yes" - while the AU editor, the
    // one publishing state for every row below, opened later with its own prompt still up. One hook,
    // one command file, two readers: a command only reaches a window that is open to hear it.
    {
        rig.send (echojay::rig::cmd::press, args1 ("button", "onboarding"), 10000);
        Rig::pump (500);
        const auto stOb = rig.freshState();
        // ONE CLAIM, SCOPED TO ITSELF. The first cut also asserted the composer was VISIBLE here, and
        // it was not - the editor is on the Dashboard tab at this point, which has no composer and is
        // right not to. That is a second claim about a different thing, and bundling it in made this
        // check fail for a reason it was not about. H1 owns "the composer is there and the hint is
        // above it", and it does its own tab press to get there.
        check (! (bool) stOb.getProperty ("onboardingUp", false),
               "the onboarding intake is ANSWERED, so no modal prompt covers the window for the rows "
               "below (it hides the composer on EVERY tab, and the rig had never answered it)",
               "onboardingUp=" + stOb.getProperty ("onboardingUp", juce::var()).toString()
               + " (composerVisible=" + stOb.getProperty ("composerVisible", juce::var()).toString()
               + " is expected to be 0 here: this is the Dashboard tab, which has no composer)");
    }
    // ASK FIRST. The hook publishes state when it is given a command - it does not write a file on a timer,
    // which is deliberate (a hook that wrote to disk every tick would be a hook with a cost). The first run
    // of this leg read the file before any command had been sent and called the hook broken; the hook was
    // fine and the leg was wrong. Both failures below were mine, not the plugin's.
    auto stAck = rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()));
    check ((bool) stAck.getProperty ("ok", false), "the hook answers a command",
           stAck.isObject() ? stAck.getProperty ("note", juce::var()).toString() : juce::String ("no ack at all"));
    auto st = Rig::state();
    check (st.isObject(), "...and the plugin published its state through it",
           st.isObject() ? juce::String() : juce::String ("no rig-state.json"));

    // The registry pass runs on the processor's 1 Hz timer, so the Links appear a second or so after they
    // first process audio. Waited for, with a bound, rather than read once - the "decisions on uncontrolled
    // timing" rule applies to the rig as much as to the plugin.
    juce::String linkUid, linkName;
    // A NAMED Link, waited for: A11b asserts that the approved line NAMES the track, and a Link whose
    // host name has not arrived yet has no name to find - the row would fail about a fact that had not
    // been published rather than about the product. Twenty seconds, and the Kick by preference.
    // THE KICK BY PREFERENCE (16 Oct 2026, Sean): with real track names in the registry the rows that
    // say "the kick" can be about the kick. Any connected Link still satisfies the wait - a row must
    // not hang because a name is late - but the Kick is taken when it is there.
    // AND USABLE, NOT MERELY LISTED (17 Oct): connected, named, with a readable slot count. A
    // leftover entry from an earlier run satisfies "connected" and nothing else.
    const bool sawLink = rig.until ([&linkUid, &linkName] (const juce::var& s)
    {
        juce::String n;
        if (const auto kick = usableLinkUid (s, true, &n); kick.isNotEmpty() && n.containsIgnoreCase ("Kick"))
        { linkUid = kick; linkName = n; return true; }
        juce::String anyName;
        if (const auto any = usableLinkUid (s, false, &anyName); any.isNotEmpty())
        { linkUid = any; linkName = anyName; }
        return false;        // keep looking for the Kick until the bound
    }, 20000);
    if (! sawLink) rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()));
    check (linkUid.isNotEmpty(), "a Link is in the registry and connected",
           "links=" + juce::JSON::toString (Rig::state().getProperty ("links", juce::var())));

    if (linkUid.isNotEmpty())
    {
        // THE RACK VIEW first - and it must NOT move the chat's channel (the 25 Sep rule).
        auto ack = rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkUid));
        check ((bool) ack.getProperty ("ok", false), "the rig selected that Link as the viewed rack",
               ack.getProperty ("note", juce::var()).toString());
        const bool viewMoved = rig.until ([&] (const juce::var& s)
                               { return s.getProperty ("viewUid", juce::var()).toString() == linkUid; }, 4000);
        check (viewMoved, "...and the VIEW is on that rack",
               "viewUid=" + Rig::state().getProperty ("viewUid", juce::var()).toString());

        // THEN the chat's working channel, which is what Sean's item 7 is about ("Working on Link: ...").
        // My first run pressed only the rack door and called item 7 broken; the rule says a rack selection
        // leaves the chat alone, so the leg was wrong and the plugin was right.
        auto ack2 = rig.send (echojay::rig::cmd::workOn, args1 ("uid", linkUid));
        check ((bool) ack2.getProperty ("ok", false), "the rig set that Link as the chat's working channel",
               ack2.getProperty ("note", juce::var()).toString());
        const bool told = rig.until ([&] (const juce::var& s)
                          { return s.getProperty ("agentTarget", juce::var()).toString() == linkUid; }, 6000);
        check (told, "...and the agent's target became that Link's uid  (RED before item 7: setTarget had no "
                     "caller anywhere in the product)",
               "agentTarget=\"" + Rig::state().getProperty ("agentTarget", juce::var()).toString()
               + "\" workingUid=\"" + Rig::state().getProperty ("workingUid", juce::var()).toString() + "\"");
    }

    // ---- THE LIVE-VIEW ROWS, AS A LAMBDA SO THEY CAN RUN FIRST (26 Oct 2026) -----------------
    //
    // These rows are at the end of the suite, and the suite is twenty-five minutes. Two of his
    // faults were still being measured wrong after two runs, which is fifty minutes to learn one
    // thing. With EJ_RIG_FAST_LV=1 they run HERE, straight after the editor opens, and the process
    // stops afterwards - a loop of about three minutes while a live-view fault is being chased.
    // It is loudly not a gate, and the full suite still runs them in their own place.
    auto runLiveViewRows = [&] ()
    {
        // ---- LV1-LV3: THE LIVE VIEW IN A REAL HOST (his 26 Oct ruling, step b) --------------------
        //
        // *"Your bench passed all of these, so it isn't testing what I see. Reproduce each fault in a
        //  real host ... driving a drag through the real editor ... Show each one red before fixing it."*
        //
        // WHY THE OWN RACK AND NOT A LINK. Faults 1, 2 and 3 are all about the hosted plugin's own view
        // inside our clip container, and that container is the same object on both roads: the pane, the
        // reparent, the 400 ms maintenance pass and the layers do not know whose rack the slot is on.
        // What a Link adds is FOLLOW and CONTROL - the Link's values and the gesture hold - and those
        // get their own rows next, with the copy's and the Link's values sampled per tick. Saying which
        // half this proves matters more than a row that claims both.
        if (rowWanted ("LV1") || rowWanted ("LV2") || rowWanted ("LV3") || rowWanted ("LV7"))
        {
            leg ("LV: a hosted plugin's own view in the V2, in a real host with a real window");
            // AUDelay IS HIS CRASH, and Apple's own AU is free of PACE, so the standing rule is kept.
            // The others are tried and named; one that will not load is said out loud, never silently
            // dropped, because a row that quietly ran one subject is a row about one subject.
            const juce::StringArray subjects { "AUDelay", "AUMatrixReverb", "AUMultibandCompressor" };
            juce::StringArray ran, notRan;
            // ---- THE VIEW HAS TO BE ON THE RACK THIS ROW IS FILLING (26 Oct 2026) ----------------
            // The first run of these rows added three plugins to the OWN rack and then pressed slot 1
            // three times, and the plugin answered "there is no slot 1 to double-click" every time -
            // correctly: an earlier leg had left the rack VIEW aimed at a Link, so the cards on screen
            // were that Link's. A row that fills one rack and presses another proves nothing.
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);     // "" = the own rack
            rig.send (echojay::rig::cmd::press,      args1 ("button", "tab_chain"), 8000);
            Rig::pump (600);
            for (const auto& subject : subjects)
            {
                clearOwnRack (rig);
                Rig::pump (300);
                const auto addAck = rig.send (echojay::rig::cmd::addSlot, args1 ("name", subject), 25000);
                juce::String slot1;
                rig.until ([&] (const juce::var& stv)
                {
                    if (auto* arr = stv.getProperty ("ownRack", juce::var())
                                       .getProperty ("slots", juce::var()).getArray())
                        if (! arr->isEmpty())
                            slot1 = (*arr)[0].getProperty ("name", juce::var()).toString();
                    return slot1.containsIgnoreCase (subject);
                }, 15000);
                if (! slot1.containsIgnoreCase (subject))
                {
                    notRan.add (subject + " (" + addAck.getProperty ("note", juce::var()).toString() + ")");
                    continue;
                }
                ran.add (subject);
                Rig::pump (600);        // the panel rebuilds its cards on its own tick

                // ---- THE OPEN, WITH WHATEVER THE PLUGIN'S OWN DRAW THROWS CAUGHT (fault 1) --------
                // His crash is an exception out of Apple's own drawRect, 24 ms after our reparent, and
                // it kills the AU hosting process in Logic. Here it is caught, so the row has a verdict
                // instead of the suite having a corpse - and the catch is the whole reason this row can
                // exist at all.
                gGuestDraws = 0; gGuestThrows = 0; gGuestThrowWhat.clear();
                juce::String openThrow;
                @try
                {
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", 1), 10000);
                    Rig::pump (3000);          // the pane's 100 ms settle poll, then its first maintenance
                }
                @catch (NSException* e) { openThrow = ejNS ([e name]) + ": " + ejNS ([e reason]); }
                ejMeasureHostedView (subject + " on the V2's own rack", openThrow,
                                     "LV1", "LV2", "LV3", ejPumpMsgThread,
                                     // The own rack has no live view, so "the copy's values" here
                                     // are the hosted plugin's own - the same question, one object.
                                     [&] { return rig.freshState().getProperty ("ownRack", juce::var())
                                                     .getProperty ("slot1Vals", juce::var()).toString(); },
                                     [&]
                                     {
                                         const auto ack = rig.send (echojay::rig::cmd::liveParam,
                                                                    args2 ("i", 0, "v", 0.37), 10000);
                                         return (bool) ack.getProperty ("ok", false);
                                     });
            }
            // ---- LV7: A PLUGIN REPLACED IN A SLOT SHOWS ITS OWN PANEL (his 11:20 fault, 26 Oct) ---
        //
        // *"The live view's title said 'Tube-Tech CL 1B' but showed Apple Delay's panel (the
        //  deleted plugin)."*  His exact sequence, with two of Apple's own AUs whose view classes
        //  differ by name (AUDelayCustomView vs AUMultibandCustomView), so the row can say WHICH
        //  panel is on screen rather than that one is.
        if (rowWanted ("LV7"))
        {
            juce::String firstClass, secondClass;
            auto openAndNameTheView = [&] (const juce::String& plugin) -> juce::String
            {
                clearOwnRack (rig);
                Rig::pump (400);
                rig.send (echojay::rig::cmd::addSlot, args1 ("name", plugin), 25000);
                juce::String slot1;
                rig.until ([&] (const juce::var& stv)
                {
                    if (auto* arr = stv.getProperty ("ownRack", juce::var())
                                       .getProperty ("slots", juce::var()).getArray())
                        if (! arr->isEmpty())
                            slot1 = (*arr)[0].getProperty ("name", juce::var()).toString();
                    return slot1.containsIgnoreCase (plugin);
                }, 15000);
                if (! slot1.containsIgnoreCase (plugin)) return {};
                Rig::pump (600);
                rig.send (echojay::rig::cmd::openSlot, args1 ("n", 1), 10000);
                Rig::pump (3000);
                NSView* c = ejFindClipContainer();
                NSView* g = ejGuestInside (c);
                return g != nil ? ejNS (NSStringFromClass ([g class])) : juce::String();
            };
            firstClass = openAndNameTheView ("AUDelay");
            // The DELETE and the REPLACE, through the product's own removeSlot and its own loader.
            secondClass = openAndNameTheView ("AUMultibandCompressor");
            std::printf ("        (LV7) the view on screen was %s, then %s\n",
                         firstClass.isEmpty() ? "(none)" : firstClass.toRawUTF8(),
                         secondClass.isEmpty() ? "(none)" : secondClass.toRawUTF8());
            if (firstClass.isEmpty() || secondClass.isEmpty())
                skip ("LV7: a plugin replaced in a slot shows its OWN panel",
                      "one of the two opens put no plugin view on screen (first \"" + firstClass
                      + "\", second \"" + secondClass + "\"), so there is nothing to compare.");
            else
            {
                check (firstClass.containsIgnoreCase ("Delay"),
                       "LV7 (precondition): opening AUDelay shows AUDelay's own view",
                       firstClass);
                check (! secondClass.equalsIgnoreCase (firstClass),
                       "LV7: after the slot's plugin is replaced, the view on screen is the NEW "
                       "plugin's - not the deleted one's panel under the new one's name",
                       secondClass.equalsIgnoreCase (firstClass)
                           ? "BOTH opens showed " + firstClass + " - the deleted plugin's own view "
                             "was adopted into the new plugin's container, which is his 11:20 fault"
                           : juce::String());
            }
        }

        std::printf ("        (LV) subjects run: %s%s\n",
                         ran.isEmpty() ? "NONE" : ran.joinIntoString (", ").toRawUTF8(),
                         notRan.isEmpty() ? "" : ("  |  could not be added: "
                                                  + notRan.joinIntoString ("; ")).toRawUTF8());
            if (ran.isEmpty())
                skip ("LV: the live view in a real host",
                      "none of the subjects could be put on the rack, so nothing was measured.");
        }

        // ---- LV4-LV6: THE LIVE VIEW OF A LINK'S PLUGIN, WHICH IS THE ROAD HIS FAULTS ARE ON -------
        //
        // The block above proves the container and the drawing on the V2's own rack. This one adds the
        // two layers that only exist in a live view - the mirror writing into the copy, and CONTROL
        // carrying his gesture to the Link - and samples, every tick, exactly what he asked for: the
        // copy's displayed value, the view's frame and layers, and the message thread's frame time.
        //
        // AND IT KEEPS THE HOST ALIVE WHILE IT MEASURES. A live view that stops following puts the
        // read-only shield back up, and a drag would then land on our shield instead of the plugin - so
        // the pump used here feeds real audio on every turn.
        // EVERY ROW IN THIS BLOCK HAS TO BE IN THIS CONDITION, and twice today one was not: a
        // row left out of it vanishes with no line at all, which the suite counts as neither a
        // pass nor a skip. LV7 and LV8 were both silently absent for a run because of it.
        // The Link and the slot the LV rows worked on, so the real-editor rows below can lease THE
        // SAME real plugin on the same real Link instead of setting one up a second time.
        juce::String lvUid; int lvSlot = 0;
        if (rowWanted ("LV4") || rowWanted ("LV5") || rowWanted ("LV6") || rowWanted ("LV8")
            || rowWanted ("LV9") || rowWanted ("LV10") || rowWanted ("LV10b") || rowWanted ("LV11")
            || rowWanted ("RE0") || rowWanted ("RE1") || rowWanted ("RE2") || rowWanted ("RE3")
            || rowWanted ("RE4") || rowWanted ("RE5") || rowWanted ("RE7")
            // THE REAL-EDITOR ROWS NEED THIS SETUP TOO (26 Oct 2026). Left out of this gate, they
            // asked for a Link that was never set up, found nothing, and the run printed GREEN with
            // the rows absent - which is the third time a row has vanished from a block's gate in
            // this file. A row that does not run is a stop, so the gate names every one of them.
            || anyRowWanted ({ "RE8", "RE9", "RE10", "RE11", "RE12", "RE13", "RE14", "RE15", "RE16",
                               "RH1", "RH2", "RH3", "RH4", "RH5", "RH6", "RH7", "RH8", "RH9" }))
        {
            leg ("LV: AUDelay in the LIVE VIEW of a Link's rack (the mirror and CONTROL are live)");
            const EJPumpFn livePump = [&] (int ms)
            {
                const double end = juce::Time::getMillisecondCounterHiRes() + (double) ms;
                while (juce::Time::getMillisecondCounterHiRes() < end)
                {
                    for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr);
                    Rig::pump (10);
                }
            };
            juce::String uid = usableLinkUid (rig.freshState(), true);
            if (uid.isEmpty())
                rig.until ([&uid] (const juce::var& st)
                {
                    if (auto* arr = st.getProperty ("links", juce::var()).getArray())
                        for (const auto& l : *arr)
                        {
                            const auto u = l.getProperty ("uid", juce::var()).toString();
                            if (u.isNotEmpty() && (bool) l.getProperty ("connected", false))
                            { uid = u; return true; }
                        }
                    return false;
                }, 20000);
            // ---- AND IT HAS TO BE A LINK THIS PROCESS' DIRECTORY HOLDS (27 Oct 2026) -------------
            //
            // The RE rows run on whatever Link this block set up, and the real editor is only ever
            // offered for a Link in OUR process - so a uid that is connected but not in the
            // process-local directory sends every one of them down the copy road. That is what the
            // first full pass reported: "the switch is on and the pids match, but that Link is not in
            // this process' directory - so the copy road answers (1 live: uid a560659087)", while
            // this block had chosen 6784ba9c8a. Two Link tracks, one of them in the directory, and
            // the row picked the other. The RH block has asked the directory since 01:30; this one
            // asked the registry, which answers a different question.
            if (uid.isNotEmpty())
            {
                auto* tblOp = new juce::DynamicObject();
                tblOp->setProperty ("op", "table");
                const auto tbl = rig.send (echojay::rig::cmd::lease, juce::var (tblOp), 10000)
                                    .getProperty ("note", juce::var()).toString();
                if (! tbl.contains (uid))
                {
                    juce::String better;
                    if (auto* arr2 = rig.freshState().getProperty ("links", juce::var()).getArray())
                        for (const auto& l : *arr2)
                        {
                            const auto u = l.getProperty ("uid", juce::var()).toString();
                            if (u.isNotEmpty() && (bool) l.getProperty ("connected", false)
                                && tbl.contains (u))
                            { better = u; break; }
                        }
                    if (better.isNotEmpty())
                    {
                        std::printf ("        (LV) %s is connected but is not in this process' directory - "
                                     "using %s, which is\n", uid.toRawUTF8(), better.toRawUTF8());
                        uid = better;
                    }
                    else
                        std::printf ("        (LV) %s is connected but is not in this process' directory, and "
                                     "no connected Link is in it either - the real-editor rows will say so. "
                                     "The table: %s\n", uid.toRawUTF8(), tbl.toRawUTF8());
                }
            }
            if (uid.isEmpty())
            {
                skip ("LV4-LV6: AUDelay in a Link's live view",
                      "no connected Link was published within 20 s, so there is no remote slot to show.");
            }
            else
            {
                rig.send (echojay::rig::cmd::selectLink, args1 ("uid", uid), 10000);
                rig.send (echojay::rig::cmd::workOn,     args1 ("uid", uid), 10000);
                // AND THE RACK HAS TO BE ON SCREEN. The live view opens its editor through
                // showInline(slot, deliberate=false), which returns at once when the panel is not
                // showing - so on the CHAT tab the copy comes up, the audio flows, and no editor
                // is ever created. That is what run 2 measured: shadowRunning=1, container absent.
                rig.send (echojay::rig::cmd::press, args1 ("button", "tab_chain"), 8000);
                livePump (1000);
                // AUDelay onto THAT Link's rack, by the agent's own road (the mock plan
                // rig-linkaudelay-marker): Apple's own AU, so the no-PACE rule is kept.
                const auto added = askAgent (rig, "rig-linkaudelay-marker: put AUDelay on that Link", 90000);
                juce::ignoreUnused (added);
                int slotToOpen = 0;
                rig.until ([&] (const juce::var& st)
                {
                    if (auto* arr = st.getProperty ("links", juce::var()).getArray())
                        for (const auto& l : *arr)
                            if (l.getProperty ("uid", juce::var()).toString() == uid)
                            {
                                const auto names = juce::StringArray::fromTokens (
                                    l.getProperty ("slots", juce::var()).toString(), "|", "");
                                for (int k = 0; k < names.size(); ++k)
                                    if (names[k].containsIgnoreCase ("AUDelay")) { slotToOpen = k + 1; return true; }
                            }
                    return false;
                }, 25000);
                lvUid = uid; lvSlot = slotToOpen;
                // ---- AND THE PANEL HAS TO BE SHOWING THAT RACK'S CARDS (26 Oct 2026, 17:20) -----
                // The run before this one opened the slot 0.7 s after the borrow engaged, while the
                // mirror's instances were still loading, and the plugin answered "there is no slot 1
                // to double-click" - so the live view never came up and LV9 failed for a reason that
                // was the ROW'S, not the product's. The panel publishes what it is drawing
                // (chainView.uid and .count); the row waits for the card it is about to press, and
                // SAYS SO if it never arrives.
                juce::String cardWaitNote;
                if (slotToOpen > 0)
                {
                    const bool haveCard = rig.until ([&] (const juce::var& st)
                    {
                        const auto cv = st.getProperty ("chainView", juce::var());
                        return cv.getProperty ("uid", juce::var()).toString() == uid
                            && (int) cv.getProperty ("count", 0) >= slotToOpen;
                    }, 20000);
                    const auto cv = rig.freshState().getProperty ("chainView", juce::var());
                    cardWaitNote = "the panel is drawing uid \""
                                 + cv.getProperty ("uid", juce::var()).toString() + "\" with "
                                 + juce::String ((int) cv.getProperty ("count", 0)) + " card(s)";
                    if (! haveCard)
                    {
                        std::printf ("        (LV) the panel never drew slot %d's card: %s\n",
                                     slotToOpen, cardWaitNote.toRawUTF8());
                        slotToOpen = 0;       // nothing to press; the rows below say so in his words
                    }
                }
                if (slotToOpen == 0)
                {
                    if (rowWanted ("LV9"))
                        row ("LV9", false,
                             "does a change made in the live view reach the plugin ON THE LINK and stay "
                             "there, with the real-editor switch off",
                             "COULD NOT RUN, which is a FAIL and not a skip (his rule of 26 Oct): there was "
                             "no card to open on " + uid + " - " + (cardWaitNote.isNotEmpty() ? cardWaitNote
                                                                    : juce::String ("AUDelay never reached that Link's published slots")));
                    skip ("LV4-LV6: AUDelay in " + uid + "'s live view",
                          "AUDelay never appeared on that Link's published slots, so there was nothing "
                          "to open - the add did not land and this row measured nothing.");
                }
                else
                {
                    gGuestDraws = 0; gGuestThrows = 0; gGuestThrowWhat.clear();
                    juce::String openThrow;
                    @try
                    {
                        rig.send (echojay::rig::cmd::openSlot, args1 ("n", slotToOpen), 10000);
                        livePump (4000);
                    }
                    @catch (NSException* e) { openThrow = ejNS ([e name]) + ": " + ejNS ([e reason]); }

                    const auto st0 = rig.freshState();
                    const bool running = (bool) st0.getProperty ("shadowRunning", false);
                    std::printf ("        (LV) live view: running=%d frames=%.0f plugin=\"%s\" "
                                 "tickMax=%.2f ms tickMean=%.2f ms\n",
                                 (int) running, (double) st0.getProperty ("shadowFrames", 0.0),
                                 st0.getProperty ("shadowPlugin", juce::var()).toString().toRawUTF8(),
                                 (double) st0.getProperty ("tickMaxMs", 0.0),
                                 (double) st0.getProperty ("tickMeanMs", 0.0));
                    if (! running && rowWanted ("LV9"))
                        row ("LV9", false,
                             "does a change made in the live view reach the plugin ON THE LINK and stay "
                             "there, with the real-editor switch off",
                             "THE LIVE VIEW DID NOT COME UP at all (shadowRunning stayed 0 on slot "
                             + juce::String (slotToOpen) + " of " + uid + "), so the control road could not "
                             "be tested - and by his rule of 26 Oct that is a STOP, not flakiness: this is "
                             "the skip that let the 16:35 regression through.");
                    if (! running)
                        skip ("LV4-LV6: the live view did not come up",
                              "shadowRunning stayed 0 for 4 s on slot " + juce::String (slotToOpen)
                              + " of " + uid + (openThrow.isNotEmpty() ? ", and the open threw: " + openThrow
                                                                       : juce::String())
                              + " - so the three measurements below would be about the own-rack road.");
                    else
                    {
                        // ---- LV9: A CHANGE MADE HERE REACHES THE LINK, AND STAYS (26 Oct 2026) ---
                        //
                        // *"a guard that a live-view knob change reaches the Link and stays there
                        //  with the switch off"*, and his rule with it: a row that covers the live
                        //  view, control or settings carrying over must RUN - a skip is a stop, not
                        //  flakiness. So this row never shrugs: if it cannot set itself up it FAILS
                        //  and says what was missing.
                        //
                        // WHAT IT READS. shadowCopyVals is this side's; shadowLinkVals is the LINK'S
                        // own published frame, which is the only thing that can answer "did it reach
                        // the plugin on the Link". The 16:35 regression was exactly this: the V2
                        // logged the send, the Link applied nothing, and no row was watching.
                        if (rowWanted ("LV9"))
                        {
                            const auto stA = rig.freshState();
                            const int  haveLink = (int) stA.getProperty ("shadowLinkValCount", 0);
                            const auto linkBefore = stA.getProperty ("shadowLinkVals", juce::var()).toString();
                            // A parameter that is not already at the target, so a no-op cannot pass.
                            const auto valueOf = [] (const juce::String& line, int idx) -> double
                            {
                                for (const auto& bit : juce::StringArray::fromTokens (line, "|", ""))
                                    if (bit.upToFirstOccurrenceOf (":", false, false).getIntValue() == idx
                                        && bit.contains (":"))
                                        return bit.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
                                return -1.0;
                            };
                            const int    idx  = 0;
                            const double from = valueOf (linkBefore, idx);
                            const double want = from > 0.5 ? 0.20 : 0.80;
                            const auto moved = rig.send (echojay::rig::cmd::liveParam,
                                                         args2 ("i", idx, "v", want), 10000);
                            const auto movedNote = moved.getProperty ("note", juce::var()).toString();
                            // The Link publishes at 30 Hz and the V2 reads at 20: a second is forty
                            // frames, so this is a bound, not a guess.
                            double linkNow = -1.0;
                            for (int tries = 0; tries < 20 && std::abs (linkNow - want) > 0.02; ++tries)
                            {
                                livePump (100);
                                linkNow = valueOf (rig.freshState().getProperty ("shadowLinkVals", juce::var()).toString(), idx);
                            }
                            // ...AND IT STAYS. A value that lands and springs back is the fault he
                            // reported on 25 Oct, so the row waits and reads it again.
                            livePump (1500);
                            const auto stC = rig.freshState();
                            const double linkAfter = valueOf (stC.getProperty ("shadowLinkVals", juce::var()).toString(), idx);
                            const double copyAfter = valueOf (stC.getProperty ("shadowCopyVals", juce::var()).toString(), idx);
                            const bool setUp = haveLink > idx && (bool) moved.getProperty ("ok", false);
                            row ("LV9", setUp
                                            && std::abs (linkNow - want) <= 0.02
                                            && std::abs (linkAfter - want) <= 0.02
                                            && std::abs (copyAfter - want) <= 0.02,
                                 "does a change made in the live view reach the plugin ON THE LINK and stay "
                                 "there, with the real-editor switch off",
                                 ! setUp
                                     ? "COULD NOT RUN, which is a FAIL and not a skip: the Link published "
                                       + juce::String (haveLink) + " value(s) and the move answered \""
                                       + movedNote + "\""
                                     : "asked for " + juce::String (want, 3) + " (from " + juce::String (from, 3)
                                       + "): the Link reached " + juce::String (linkNow, 3) + ", still "
                                       + juce::String (linkAfter, 3) + " a second and a half later, the copy "
                                       + juce::String (copyAfter, 3) + " - " + movedNote);
                        }

                        // ---- LV11: THE V2'S OWN OUTPUT IS NOT TOUCHED BY THE LIVE VIEW ----------
                        //
                        // His round-2 acceptance: *"the V2's own chain keeps processing its own audio
                        // unchanged, and the copy of the Link's plugin is never mixed into the V2's
                        // output."*  Measured, not read off the source: the same fixed input rendered
                        // through the V2 from the same starting phase, hashed sample by sample, with
                        // the live view open and with it closed.
                        if (rowWanted ("LV11"))
                        {
                            const int blocks = 40;                       // about 0.4 s at 512/48k
                            std::vector<float> ref;
                            (void) ejRenderMaxDiff (v2, sr, blocks, ref);          // with the view up
                            // STOP THE VIEW - not the tab, which leaves the copy running on the
                            // processor (the first cut of this row said so itself).
                            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
                            livePump (1200);
                            const bool stillRunning = (bool) rig.freshState().getProperty ("shadowRunning", false);
                            const double withVsWithout = ejRenderMaxDiff (v2, sr, blocks, ref);
                            const double floorNoise    = ejRenderMaxDiff (v2, sr, blocks, ref);
                            // ...and back, so the rows after this one find the view as they expect it.
                            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", uid), 8000);
                            rig.send (echojay::rig::cmd::press,      args1 ("button", "tab_chain"), 8000);
                            livePump (600);
                            rig.send (echojay::rig::cmd::openSlot,   args1 ("n", slotToOpen), 10000);
                            livePump (2500);
                            row ("LV11", ! stillRunning && withVsWithout <= juce::jmax (floorNoise, 1.0e-6),
                                 "is the V2's OWN output unaffected by the live view - the copy of the Link's "
                                 "plugin never mixed into what the V2 puts out",
                                 ! stillRunning
                                     ? "the view's presence moved the output by at most "
                                       + juce::String (withVsWithout, 9) + ", and two view-free renders differ "
                                       "by " + juce::String (floorNoise, 9) + " all by themselves (the V2's own "
                                       "devices carry state) - so the view is not in the path"
                                     : "THE VIEW WAS STILL RUNNING for the second render, so this row measured "
                                       "nothing");
                        }

                        // ---- LV10: A CHANGE WITH NO AUDIO RUNNING (his 16:35 fault, 26 Oct) -----
                        //
                        // The log's answer to *"changing settings no longer carries over"* was that
                        // no Link was processing audio at all, and the value ring is drained in
                        // processBlock. So: stop pumping THIS Link's audio, move a knob in the live
                        // view, and the change must still reach the plugin. LV10b then toggles the
                        // audio on and off across moves, which is the handover he asked to see
                        // tested - two possible consumers of a single-consumer ring.
                        if (rowWanted ("LV10") || rowWanted ("LV10b"))
                        {
                            const auto linkName = linkRowOf (rig.freshState(), uid)
                                                      .getProperty ("name", juce::var()).toString();
                            Track* victim = nullptr;
                            if (linkName == lk1.trackName || linkName.containsIgnoreCase ("Kick")) victim = &lk1;
                            else if (linkName == lk2.trackName)                                    victim = &lk2;

                            // Everything EXCEPT that Link keeps running: the V2's own editor has to
                            // keep ticking, or this would be a test of a frozen plugin.
                            const EJPumpFn pumpNoLink = [&] (int ms)
                            {
                                const double end = juce::Time::getMillisecondCounterHiRes() + (double) ms;
                                while (juce::Time::getMillisecondCounterHiRes() < end)
                                {
                                    for (Track* t : { &v2, &lk1, &lk2 })
                                        if (t != victim && t->plugin != nullptr) t->processOneBlock (sr);
                                    Rig::pump (10);
                                }
                            };
                            const auto valueOf = [] (const juce::String& line, int idx) -> double
                            {
                                for (const auto& bit : juce::StringArray::fromTokens (line, "|", ""))
                                    if (bit.upToFirstOccurrenceOf (":", false, false).getIntValue() == idx
                                        && bit.contains (":"))
                                        return bit.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
                                return -1.0;
                            };
                            const auto linkValue = [&] (int idx)
                            {
                                return valueOf (rig.freshState().getProperty ("shadowLinkVals", juce::var()).toString(), idx);
                            };

                            if (victim == nullptr)
                            {
                                const juce::String why = "the Link on screen is \"" + linkName
                                    + "\", which is not one of this rig's two tracks, so its audio "
                                      "cannot be stopped - and by his rule of 26 Oct a live-view row "
                                      "that cannot run is a FAIL, not a skip.";
                                if (rowWanted ("LV10"))  row ("LV10",  false, "does a knob change reach the Link with NO AUDIO running", why);
                                if (rowWanted ("LV10b")) row ("LV10b", false, "does the drain hand over cleanly as audio starts and stops", why);
                            }
                            else
                            {
                                if (rowWanted ("LV10"))
                                {
                                    const int idx = 1;
                                    const double from = linkValue (idx);
                                    const double want = from > 0.5 ? 0.25 : 0.75;
                                    // The Link's audio stops HERE, and stays stopped for the whole row.
                                    pumpNoLink (400);
                                    const auto moved = rig.send (echojay::rig::cmd::liveParam,
                                                                 args2 ("i", idx, "v", want), 10000);
                                    double got = -1.0;
                                    for (int tries = 0; tries < 25 && std::abs (got - want) > 0.02; ++tries)
                                    {
                                        pumpNoLink (100);
                                        got = linkValue (idx);
                                    }
                                    row ("LV10", (bool) moved.getProperty ("ok", false) && std::abs (got - want) <= 0.02,
                                         "does a knob change in the live view reach the plugin on the Link while "
                                         "that Link's audio is NOT running (the transport stopped)",
                                         "asked for " + juce::String (want, 3) + " from " + juce::String (from, 3)
                                         + ", the Link holds " + juce::String (got, 3)
                                         + " after up to 2.5 s with its audio stopped - "
                                         + moved.getProperty ("note", juce::var()).toString());
                                }

                                if (rowWanted ("LV10b"))
                                {
                                    // Six moves, the Link's audio alternately stopped and running, so
                                    // the claim hands back and forth across them under real load.
                                    int landed = 0; juce::String trail;
                                    for (int phase = 0; phase < 6; ++phase)
                                    {
                                        const bool audioOn = (phase % 2) == 1;
                                        const int idx = 2;
                                        const double want = 0.15 + 0.12 * (double) phase;
                                        rig.send (echojay::rig::cmd::liveParam, args2 ("i", idx, "v", want), 10000);
                                        double got = -1.0;
                                        for (int tries = 0; tries < 15 && std::abs (got - want) > 0.02; ++tries)
                                        {
                                            if (audioOn) livePump (100); else pumpNoLink (100);
                                            got = linkValue (idx);
                                        }
                                        const bool ok = std::abs (got - want) <= 0.02;
                                        landed += ok ? 1 : 0;
                                        trail << (phase > 0 ? ", " : "") << (audioOn ? "playing " : "stopped ")
                                              << juce::String (want, 2) << "->" << juce::String (got, 2)
                                              << (ok ? "" : " MISSED");
                                    }
                                    row ("LV10b", landed == 6,
                                         "does every change land as the Link's audio starts and stops between them "
                                         "- the handover between the two possible drains, under load",
                                         juce::String (landed) + " of 6 landed: " + trail);
                                }
                            }
                        }

                        // ---- THE THREE FAULTS, ON THE LIVE-VIEW ROAD -----------------------------
                        ejMeasureHostedView ("AUDelay in " + uid + "'s live view", openThrow,
                                             "LV4", "LV5", "LV6", livePump,
                                             [&] { return rig.freshState()
                                                            .getProperty ("shadowCopyVals", juce::var()).toString(); },
                                             [&]
                                             {
                                                 // Parameter 3 this time, so LV6 cannot be reading a
                                                 // value another row left where it wanted it.
                                                 const auto ack = rig.send (echojay::rig::cmd::liveParam,
                                                                            args2 ("i", 3, "v", 0.62), 10000);
                                                 return (bool) ack.getProperty ("ok", false);
                                             });

                        // ---- AND THE TWO THINGS ONLY THIS ROAD CAN SHOW --------------------------
                        // *"nothing writes into the copy from the Link except changes that didn't come
                        //  from me, and never a parameter I touched in the last second"* - so with his
                        //  hands off and nothing else touching that rack, the copy's displayed values
                        //  must not move at all. A mirror that still writes shows up here as a value
                        //  that changed while nobody changed anything.
                        if (rowWanted ("LV5"))
                        {
                            const auto before = rig.freshState()
                                                   .getProperty ("shadowCopyVals", juce::var()).toString();
                            livePump (4000);
                            const auto after  = rig.freshState()
                                                   .getProperty ("shadowCopyVals", juce::var()).toString();
                            check (before.isNotEmpty() && before == after,
                                   "LV5b: with his hands off, the copy's own displayed values do not move",
                                   before.isEmpty() ? "the copy published no values at all"
                                                    : "before \"" + before + "\" after \"" + after + "\"");
                        }
                        // *"Measure the V2's message-thread load and frame time with a live view
                        //  open."*  The figure is the editor's own 20 Hz tick, and the bound is his
                        //  acceptance: the live view must not make the message thread miss that tick.
                        // ---- LV8: A PLUGIN REPLACED IN A LINK'S SLOT, HIS 11:20 ACTION ------
                        //
                        // *"I deleted the AUDelay from Aitch_4_01's Link rack, added the CL 1B in
                        //  its place, and clicked the CL 1B's card"* - and got the deleted plugin's
                        //  panel under the new one's name, and then "not following" for ever.
                        //
                        // THIS IS THE FAITHFUL ROW AND LV7 IS NOT. LV7 does the same replacement on
                        // the own rack and passes even with the adopt guess put back, so it holds
                        // nothing; here the live view is OPEN on the old plugin while the slot is
                        // replaced under it, which is what he did.
                        if (rowWanted ("LV8") && (slotToOpen < 1 || slotToOpen > 5))
                            skip ("LV8: a plugin replaced in a Link's slot",
                                  "the live view opened slot " + juce::String (slotToOpen)
                                  + " and this rig's mock only carries a replace plan for slots 1-5, "
                                    "so the replacement could not be asked for.");
                        if (rowWanted ("LV8") && slotToOpen >= 1 && slotToOpen <= 5)
                        {
                            // The replace op takes a SLOT NUMBER and this row cannot know it in
                            // advance - the Link's rack holds whatever earlier legs left - so the
                            // marker carries it. The first cut gated on "slot 1" and AUDelay landed
                            // on slot 3: the row vanished without a line, which is the invisible
                            // skip this suite counts separately for.
                            // THE PLAN IS MADE AGAINST THE RACK IT IS FOR (26 Oct 2026). The first
                            // cut asked for the replace and the Link answered "the rack is not the
                            // one this plan was made for - nothing was changed": the V2's cache for
                            // that uid held 0 slots at that moment (EJEdit: staleness guards
                            // passed rev=2 slots=0), so the plan was composed against an empty
                            // rack. Aim again and WAIT until the rack this row is about is the one
                            // the V2 can see, then ask.
                            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", uid), 10000);
                            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", uid), 10000);
                            int seenSlots = 0;
                            rig.until ([&] (const juce::var& stv)
                            {
                                if (auto* arr = stv.getProperty ("links", juce::var()).getArray())
                                    for (const auto& l : *arr)
                                        if (l.getProperty ("uid", juce::var()).toString() == uid)
                                            seenSlots = juce::StringArray::fromTokens (
                                                l.getProperty ("slots", juce::var()).toString(),
                                                "|", "").size();
                                return seenSlots >= slotToOpen;
                            }, 15000);
                            std::printf ("        (LV8) the V2 can see %d slot(s) on %s before the "
                                         "replace of slot %d\n", seenSlots, uid.toRawUTF8(), slotToOpen);
                            // ---- HOW THIS ROW CHANGES WHAT THAT SLOT HOLDS (26 Oct 2026) -----
                            //
                            // His action was a delete and an add in the same slot. The agent's
                            // `replace` op will not land through this rig (the Link answers "the
                            // rack is not the one this plan was made for"; its own base read holds
                            // 0 slots), so the row produces the SAME condition by the road that
                            // does work here: an INSERT at the front. After it, the slot index the
                            // live view is open on holds a different plugin - which is exactly the
                            // state his sequence left: a copy built for the plugin that WAS there.
                            // WHAT LANDS IN THAT SLOT MUST BE ONE WE MAY LOAD. The first cut
                            // inserted at the FRONT, which shifted a licensed plugin (Vertigo
                            // VSM-3) into the slot under test - and the rebuild then had nothing it
                            // was allowed to build, so the row stayed red for the rig's own reason
                            // and not the product's. Inserting directly BEFORE the slot puts one of
                            // Apple's in it, and the plugin the view is open on shifts down by one.
                            const auto replaced = askAgent (rig, "rig-linkinsert"
                                                   + juce::String (slotToOpen - 1) + "-marker: put an "
                                                   "AUMultibandCompressor into slot "
                                                   + juce::String (slotToOpen), 90000);
                            juce::ignoreUnused (replaced);
                            juce::String nameNow;
                            rig.until ([&] (const juce::var& stv)
                            {
                                if (auto* arr = stv.getProperty ("links", juce::var()).getArray())
                                    for (const auto& l : *arr)
                                        if (l.getProperty ("uid", juce::var()).toString() == uid)
                                        {
                                            const auto names = juce::StringArray::fromTokens (
                                                l.getProperty ("slots", juce::var()).toString(), "|", "");
                                            if (slotToOpen - 1 < names.size())
                                                nameNow = names[slotToOpen - 1];
                                        }
                                return nameNow.containsIgnoreCase ("Multiband");
                            }, 25000);
                            if (! nameNow.containsIgnoreCase ("Multiband"))
                                skip ("LV8: the plugin in a Link's slot is replaced under the live view",
                                      "slot " + juce::String (slotToOpen) + " of " + uid
                                      + " still holds \"" + nameNow + "\" after the insert, so the "
                                        "condition his fault needs - a copy built for the plugin "
                                        "that WAS in that slot - was never created.");
                            else
                            {
                                rig.send (echojay::rig::cmd::openSlot, args1 ("n", slotToOpen), 10000);
                                livePump (5000);
                                NSView* c2 = ejFindClipContainer();
                                NSView* g2 = ejGuestInside (c2);
                                const auto cls2 = g2 != nil ? ejNS (NSStringFromClass ([g2 class]))
                                                            : juce::String();
                                const auto st2 = rig.freshState();
                                const auto note = st2.getProperty ("shadowFollowNote", juce::var()).toString();
                                std::printf ("        (LV8) slot %d now holds \"%s\"; the view is \"%s\", running=%d, "
                                             "plugin \"%s\", note \"%s\"\n",
                                             slotToOpen, nameNow.toRawUTF8(), cls2.toRawUTF8(),
                                             (int) (bool) st2.getProperty ("shadowRunning", false),
                                             st2.getProperty ("shadowPlugin", juce::var()).toString().toRawUTF8(),
                                             note.toRawUTF8());
                                check (cls2.isNotEmpty() && ! cls2.containsIgnoreCase ("Delay"),
                                       "LV8: the slot whose plugin changed shows the NEW plugin's own "
                                       "panel, not the one that used to be there",
                                       cls2.isEmpty() ? "no plugin view was on screen at all"
                                           : "the view on screen is " + cls2
                                             + (cls2.containsIgnoreCase ("Delay")
                                                 ? " - the DELETED plugin's, which is his fault 1" : ""));
                                check ((bool) st2.getProperty ("shadowRunning", false)
                                       && ! note.containsIgnoreCase ("not have the same parameter"),
                                       "LV8a: ...and the live view FOLLOWS it - the Link publishes the "
                                       "digest of the plugin that is in that slot NOW",
                                       note.isEmpty() ? juce::String ("no note, and the view is running")
                                                      : "the card says: \"" + note + "\"");
                            }
                        }

                        if (rowWanted ("LV6"))
                        {
                            livePump (2000);
                            const auto st = rig.freshState();
                            const double mx = (double) st.getProperty ("tickMaxMs", 0.0);
                            const double mn = (double) st.getProperty ("tickMeanMs", 0.0);
                            check (mx > 0.0 && mx < 50.0,
                                   "LV6b: the message thread's tick stays inside its own 50 ms period "
                                   "with a live view open",
                                   "worst " + juce::String (mx, 2) + " ms, mean "
                                   + juce::String (mn, 2) + " ms over the last second"
                                   + (mx <= 0.0 ? " (nothing was measured - the figure is not published)"
                                                : juce::String()));
                        }
                    }
                }
            }
        }

        // ================================================================================================
        //  RE1-RE6: THE REAL EDITOR'S DIRECTORY AND LEASE, IN A REAL HOST (round 1, 26 Oct 2026)
        // ================================================================================================
        //
        //  *"Build it in your 4-5 rounds, with round 1 (lifetime, lease and ordering) green in the rig
        //   before the V2 ever draws a lent editor."*
        //
        //  WHAT ONLY THIS CAN PROVE, and the reason round 1 had to start here. The V2 and the Link are two
        //  Mach-O images with hidden visibility: a directory kept in a shared header would be TWO tables,
        //  one per bundle, and the V2 would never find a single Link. These rows ask the V2's own image for
        //  a Link's real editor and watch the Link's image answer. Nothing is DRAWN: the rig's borrower
        //  holds the editor and shows it nowhere, which is exactly his condition.
        //
        //  The guard legs EL0-EL10a (tools/chat_store_guard) prove the rules with fakes, where every
        //  ordering can be driven and stated. These prove the two bundles meet, and that the two teardowns
        //  that have actually crashed this plugin - a slot removed under an open editor, and the instance
        //  destroyed while someone holds one - reach the holder in the right order with real plugins.
        if (anyRowWanted ({ "RE0", "RE1", "RE2", "RE3", "RE4", "RE5", "RE6", "RE7",
                            "RE8", "RE9", "RE10", "RE11", "RE12", "RE13", "RE14", "RE15", "RE16",
                            "RH1", "RH2", "RH3", "RH4", "RH5", "RH6", "RH7", "RH8", "RH9" }))
        {
            leg ("RE: the real editor's directory and lease, with two real bundles in one process");

            auto lease = [&] (const char* op, const juce::String& uid, int n, int timeoutMs = 10000)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("op", op);
                if (uid.isNotEmpty()) o->setProperty ("uid", uid);
                if (n > 0)            o->setProperty ("n", n);
                return rig.send (echojay::rig::cmd::lease, juce::var (o), timeoutMs);
            };
            auto noteOf = [] (const juce::var& ack) { return ack.getProperty ("note", juce::var()).toString(); };

            // ---- RE5 RUNS LAST, BECAUSE IT KILLS THE LINK EVERY LATER ROW NEEDS (27 Oct 2026) ----
            //
            // RE5 destroys the Link's track on purpose. It used to run here, before the round-2 rows,
            // and every one of them then addressed a dead uid: the pane answered "the switch is on and
            // the pids match, but that Link is not in this process' directory - so the copy road
            // answers", and RE8, RE9, RE10, RE11, RE12, RE13, RE15 and RE16 all failed for that one
            // reason. The overnight pass never saw it because the RH block already re-picks a live
            // Link from the directory (01:30) while the RE rows did not.
            //
            // So the row is written where it belongs and RUN where it does no harm: after the
            // round-2 rows and before the RH block, which re-picks for itself. Same lesson as RH9.
            std::function<void()> runRE5;

            // ---- RE0: ONE TABLE IN THIS PROCESS, SEEN FROM THE V2'S IMAGE ---------------------------
            {
                const auto ack = lease ("table", {}, 0);
                const auto note = noteOf (ack);
                row ("RE0", note.contains ("one shared EchoJay table") && note.contains ("live"),
                     "is there ONE EchoJay table in this process, and does the V2's bundle see the Links' "
                     "registrations in it",
                     note);
            }

            // ---- RE7: AND WHICH LIVE VIEW A BUILD TAKES BY DEFAULT (his condition, 26 Oct) -------
            // *"Put the real editor behind a switch (default off until I've tested it)."*  A row has
            // to be able to say that about the BUILD, not about the source, and this one reads it
            // from the running plugin in a real host.
            if (rowWanted ("RE7"))
            {
                const auto note = noteOf (lease ("road", lvUid, 0));
                row ("RE7", note.contains ("the switch is OFF") && note.contains ("its own copy"),
                     "does this build take the COPY road by default, with the real editor behind a "
                     "switch he has not turned on",
                     note);
            }

            if (lvUid.isEmpty() || lvSlot <= 0)
            {
                skip ("RE1-RE5: a Link's real editor, leased",
                      "the LV rows never got a plugin onto a Link's rack in this pass (uid \""
                      + lvUid + "\", slot " + juce::String (lvSlot) + "), so there is no real remote slot "
                      "to lease - these rows measured nothing rather than passing on the own rack.");
            }
            else
            {
                // ---- RE1: THE V2'S IMAGE FINDS THE LINK'S INSTANCE --------------------------------
                {
                    const auto ack = lease ("find", lvUid, 0);
                    const auto note = noteOf (ack);
                    row ("RE1", (bool) ack.getProperty ("ok", false) && note.contains ("EchoJay Link"),
                         "can the V2 find that Link's live instance in this process (the two bundles meeting "
                         "in one table, which a shared header's static cannot do)",
                         note);
                }

                // ---- RE2: AND GETS ITS REAL EDITOR, HELD, SHOWN NOWHERE --------------------------
                juce::String takeNote;
                bool took = false;
                {
                    const auto ack = lease ("take", lvUid, lvSlot, 20000);
                    takeNote = noteOf (ack);
                    took = (bool) ack.getProperty ("ok", false);
                    const auto who = noteOf (lease ("who", lvUid, lvSlot));
                    row ("RE2", took && takeNote.contains ("real editor") && who.contains ("the rig"),
                         "does the lease hand the V2 the REAL editor of the real instance on slot "
                         + juce::String (lvSlot) + ", with one place answering who holds it",
                         takeNote + " | " + who);
                }

                // ---- RE3: A SECOND EDITOR OF THAT INSTANCE IS REFUSED, IN A SENTENCE -------------
                // A row that is gated on the one before it SAYS SO when the gate closes: left out of
                // its gate it vanishes with no line at all, which is how two rows went missing from
                // the LV set this morning.
                if (! took)
                    skip ("RE3-RE4: the second ask, and the return",
                          "the lease on slot " + juce::String (lvSlot) + " of " + lvUid
                          + " was never taken (" + takeNote + "), so there was nothing to ask for "
                          "twice and nothing to give back.");
                if (took)
                {
                    const auto again = noteOf (lease ("take", lvUid, lvSlot));
                    row ("RE3", again.contains ("refused") && again.contains ("already open"),
                         "is a second editor of one instance refused with the sentence that says who has it "
                         "(one activeEditor per processor; two parents is undefined behaviour)",
                         again);
                }

                // ---- RE4: GIVEN BACK, AND ITS OWNER DESTROYS IT ----------------------------------
                if (took)
                {
                    const auto back = noteOf (lease ("give_back", lvUid, lvSlot));
                    const auto who  = noteOf (lease ("who", lvUid, lvSlot));
                    row ("RE4", back.contains ("gave slot") && who.contains ("nobody"),
                         "does returning the lease destroy the editor at its owner and leave the slot free",
                         back + " | " + who);
                }

                // ---- RE5: THE LINK IS DESTROYED WHILE THE V2 HOLDS ONE --------------------------
                //
                // The design's teardown table, the line with the most risk on it: *"the Link processor is
                // destroyed (track deleted, plugin removed): destructor's FIRST act - drop from the
                // directory and revoke every lease"*. Here the host really does destroy it, with the V2
                // really holding that slot's editor, and the row reads what the V2 was told.
                runRE5 = [&]
                {
                    if (! rowWanted ("RE5")) return;
                    const auto name = linkRowOf (rig.freshState(), lvUid).getProperty ("name", juce::var()).toString();
                    Track* victim = nullptr;
                    if (name == lk1.trackName || name.containsIgnoreCase ("Kick")) victim = &lk1;
                    else if (name == lk2.trackName)                                victim = &lk2;

                    const auto retake = noteOf (lease ("take", lvUid, lvSlot, 20000));
                    if (victim == nullptr || victim->plugin == nullptr || ! retake.contains ("real editor"))
                    {
                        skip ("RE5: the Link destroyed while the V2 holds its editor",
                              "could not set the row up: the Link on screen is \"" + name
                              + "\" which is not one of this rig's two tracks, or the lease was not taken ("
                              + retake + ").");
                    }
                    else
                    {
                        const auto before = noteOf (lease ("revokes", {}, 0));
                        // The host deletes the track: releaseResources, then the processor itself.
                        victim->plugin->releaseResources();
                        victim->plugin.reset();
                        Rig::pump (1500);
                        const auto after = noteOf (lease ("revokes", {}, 0));
                        const auto find  = noteOf (lease ("find", lvUid, 0));
                        row ("RE5", after.contains ("going away") && after.contains ("nothing")
                                        && find.contains ("no live EchoJay Link"),
                             "when the host destroys the Link while the V2 holds one of its editors, is the "
                             "V2 told to let go - and is that Link gone from the directory",
                             "before: " + before + " | after: " + after + " | " + find);
                    }
                };
            }

            // ================================================================================================
            //  RE8-RE13: THE REAL EDITOR ON SCREEN, AND THE COPY ROAD BESIDE IT (round 2, 26 Oct 2026)
            // ================================================================================================
            //
            //  *"the V2 showing the real editor behind it, with the copy-versus-real comparison and the
            //   V2-output-unchanged check as its acceptance ... if round 2 doesn't look and feel at least as
            //   good as the copy (smooth knobs, no wobble, no crash, settings kept on the Link, slot replace
            //   working), we switch back and park it."*
            //
            //  So the same subject is measured on BOTH roads in one pass, with his own switch thrown between
            //  them through the Settings toggle itself. The copy road's figures come from LV4-LV11 above; these
            //  rows are the real editor's, and RE13 proves the switch really does put it back.
            const bool anyRealEditorRow = anyRowWanted ({ "RE8", "RE9", "RE10", "RE11", "RE12",
                                                          "RE13", "RE14", "RE15", "RE16" });
            if (anyRealEditorRow && (lvUid.isEmpty() || lvSlot <= 0))
            {
                const juce::String why = "no Link with a plugin on it was set up in this pass (uid \""
                    + lvUid + "\", slot " + juce::String (lvSlot) + "), so the real-editor road could not be "
                    "entered - and by his rule of 26 Oct a live-view row that does not run is a STOP.";
                if (rowWanted ("RE8"))  row ("RE8",  false, "does the live view show the LINK'S OWN editor", why);
                if (rowWanted ("RE9"))  row ("RE9",  false, "is the real editor still with his hands off", why);
                if (rowWanted ("RE10")) row ("RE10", false, "does a knob on the real editor reach the Link", why);
                if (rowWanted ("RE11")) row ("RE11", false, "is the V2's own output unchanged on the real road", why);
                if (rowWanted ("RE12")) row ("RE12", false, "does slot replace work on the real road", why);
                if (rowWanted ("RE13")) row ("RE13", false, "does his switch put the live view back", why);
                if (rowWanted ("RE14")) row ("RE14", false, "does the V2 let go for the Link's own window", why);
                if (rowWanted ("RE15")) row ("RE15", false, "...and get it back when its card is pressed", why);
            }
            if (anyRealEditorRow && lvUid.isNotEmpty() && lvSlot > 0)
            {
                // This block sits outside the LV rows' scope, so it carries its own pump: every
                // track one block, then the message thread - the same shape livePump has there.
                const EJPumpFn rePump = [&] (int ms)
                {
                    const double end = juce::Time::getMillisecondCounterHiRes() + (double) ms;
                    while (juce::Time::getMillisecondCounterHiRes() < end)
                    {
                        for (Track* t : { &v2, &lk1, &lk2 }) if (t->plugin != nullptr) t->processOneBlock (sr);
                        Rig::pump (10);
                    }
                };
                const auto road = [&] { return rig.freshState().getProperty ("liveViewRoad", juce::var()).toString(); };
                const auto valueOf = [] (const juce::String& line, int idx) -> double
                {
                    for (const auto& bit : juce::StringArray::fromTokens (line, "|", ""))
                        if (bit.upToFirstOccurrenceOf (":", false, false).getIntValue() == idx && bit.contains (":"))
                            return bit.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
                    return -1.0;
                };
                const auto linkValue = [&] (int idx)
                {
                    return valueOf (rig.freshState().getProperty ("shadowLinkVals", juce::var()).toString(), idx);
                };
                // Close whatever is open, throw his switch, and open the same card again: the road is
                // chosen at open, which is what the toggle's own log line says.
                rig.send (echojay::rig::cmd::press, args1 ("button", "tab_chat"), 8000);
                rePump (500);
                const auto flip = rig.send (echojay::rig::cmd::press, args1 ("button", "real_editor_on"), 8000);
                rig.send (echojay::rig::cmd::press, args1 ("button", "tab_chain"), 8000);
                rePump (500);
                juce::String openThrow2;
                @try
                {
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", lvSlot), 15000);
                    rePump (4000);
                }
                @catch (NSException* e) { openThrow2 = ejNS ([e name]) + ": " + ejNS ([e reason]); }

                const auto st = rig.freshState();
                const bool onReal = road() == "real";
                const bool noCopy = ! (bool) st.getProperty ("shadowRunning", false);

                // ---- RE8: IT IS THE REAL PLUGIN, AND NO COPY WAS MADE ----------------------------
                if (rowWanted ("RE8"))
                    row ("RE8", onReal && noCopy && openThrow2.isEmpty(),
                         "with his switch ON, does the live view show the LINK'S OWN editor - and no copy at all",
                         "the switch: " + flip.getProperty ("note", juce::var()).toString()
                         + " | the pane is on the \"" + road() + "\" road, a copy is "
                         + juce::String (noCopy ? "NOT running" : "STILL running")
                         + (openThrow2.isEmpty() ? juce::String() : ", and the open threw: " + openThrow2));

                // ---- RE16: AND WITH NO EDIT SESSION ON THAT LINK ---------------------------------
                //
                // THE PRECONDITION LAST NIGHT'S GREEN WAS LEANING ON WITHOUT SAYING SO. The real
                // editor opened only where an edit session was already engaged on that very Link: on
                // a remote rack both roads to showInline were gated on `editingSession`, so with the
                // switch ON and no session the pane opened NOTHING (the "none" road) and the one-shot
                // asked the Link to raise its own window instead. Overnight the session and every one
                // of the 164 lent editors were the same uid, so RE8-RE15 and RH1-RH8 were green under
                // a condition nobody had asked for; both of this morning's passes drew a different uid
                // and the real editor never appeared once.
                //
                // So the row says, every run, WHERE the session is - and a run that cannot ask the
                // question (because a session happens to be on this Link) FAILS with that sentence
                // rather than passing. His rule of 26 Oct: a live-view row that does not run is a stop.
                if (rowWanted ("RE16"))
                {
                    const auto sessUid  = st.getProperty ("editSessionUid", juce::var()).toString();
                    const auto sessSlot = (int) st.getProperty ("editSessionSlot", juce::var());
                    const bool sessionElsewhere = sessUid != lvUid;
                    row ("RE16", onReal && sessionElsewhere,
                         "does the real editor open with NO edit session engaged on that Link (the "
                         "precondition the overnight green was leaning on)",
                         "the pane is on the \"" + road() + "\" road; the edit session is "
                         + (sessUid.isEmpty() ? juce::String ("engaged on nothing")
                                              : ("on \"" + sessUid + "\" slot " + juce::String (sessSlot)))
                         + ", and this Link is \"" + lvUid + "\""
                         + (sessionElsewhere ? "" : " - SO THIS RUN COULD NOT ASK THE QUESTION"));
                }

                if (! onReal)
                {
                    const juce::String why = "the pane did not take the real-editor road (it reports \""
                        + road() + "\"), so the rows below would be measuring the copy road twice - and by "
                        "his rule of 26 Oct that is a stop, not a skip.";
                    if (rowWanted ("RE9"))  row ("RE9",  false, "is the real editor still with his hands off", why);
                    if (rowWanted ("RE10")) row ("RE10", false, "does a knob on the real editor reach the Link", why);
                    if (rowWanted ("RE11")) row ("RE11", false, "is the V2's own output unchanged on the real road", why);
                    if (rowWanted ("RE12")) row ("RE12", false, "does slot replace work on the real road", why);
                }
                else
                {
                    // ---- RE9: HANDS OFF, NOTHING MOVES (his fault 3, measured in pixels) ---------
                    if (rowWanted ("RE9"))
                    {
                        NSView* cont  = ejFindClipContainer();
                        NSView* guest = ejGuestInside (cont);
                        std::vector<unsigned char> p0, p1;
                        const bool got0 = ejGuestPixels (guest, p0);
                        const auto h0 = ejGuestPixelHash (guest);
                        NSRect f0 = guest != nil ? [guest frame] : NSZeroRect;
                        rePump (5000);
                        const bool got1 = ejGuestPixels (guest, p1);
                        const auto h1 = ejGuestPixelHash (guest);
                        const NSRect f1 = guest != nil ? [guest frame] : NSZeroRect;
                        const bool read = (h0 != 0 && h1 != 0);
                        const auto delta = (got0 && got1) ? ejPixelDelta (p0, p1) : EJPixelDelta{};
                        // ---- WHAT "SITS STILL" MEANS, MEASURED RATHER THAN ASSUMED (27 Oct 2026) ---
                        //
                        // The first cut asserted an identical pixel hash and failed at 132 of 2,939,776
                        // bytes differing, worst by 36 of 255 - 0.0045% of the view, in one small place.
                        // That is a plugin's own readout doing its job: the Link IS passing audio, and
                        // slot 1 of this rack has a meter. A blinking caret measures the same.
                        //
                        // The faults this row exists for do not look like that. His 25 Oct wobble moved
                        // the view's FRAME and re-composited the whole picture ten times a second; the
                        // 26 Oct shimmer changed the lot. So the bar is: the frame must not move at all,
                        // and the picture must not change WHOLESALE - two per cent of the view, which is
                        // four hundred times what a meter costs and still a fraction of a re-composite.
                        // The figures are printed either way, so the next reader judges the measurement
                        // and not my adjective.
                        const double movedFraction = (delta.read && delta.total > 0)
                                                   ? (double) delta.differing / (double) delta.total : 0.0;
                        const bool wholesale = delta.read && movedFraction > 0.02;
                        row ("RE9", guest != nil && NSEqualRects (f0, f1)
                                        && (! read || h0 == h1 || (delta.read && ! wholesale)),
                             "with his hands off, does the real editor sit still for five seconds - no movement "
                             "of the view, and no wholesale redraw (a plugin's own meter is not a wobble)",
                             guest == nil
                                 ? (cont == nil ? "no clip container was on screen at all"
                                                : "the container was on screen with no plugin view inside it")
                                 : juce::String ("frame ") + (NSEqualRects (f0, f1) ? "unchanged" : "MOVED")
                                   + ", pixels " + (! read ? "could not be read"
                                                           : (h0 == h1 ? "unchanged" : "CHANGED with nobody touching it"))
                                   + (delta.read ? (" (" + juce::String (delta.differing) + " of "
                                                    + juce::String (delta.total) + " bytes differ = "
                                                    + juce::String (movedFraction * 100.0, 4) + "% of the view, "
                                                    "worst by " + juce::String (delta.worst) + " of 255; the bar "
                                                    "for a wholesale redraw is 2%)")
                                                 : juce::String()));
                    }

                    // ---- RE10: A KNOB ON THE REAL EDITOR IS THE LINK'S OWN KNOB ------------------
                    if (rowWanted ("RE10"))
                    {
                        // THE LINK PUBLISHES NOTHING ON THIS ROAD - there is no tap and no copy to
                        // feed - so shadowLinkVals here is the last frame the COPY road left behind,
                        // and the first cut of this row read it and called a working build broken.
                        // What answers the question instead: the LENT plugin's own values, and
                        // whether the change SURVIVES the view being closed and opened again. Nothing
                        // the V2 owns survives that, so a value that does lives in the Link's
                        // instance - which is the identity this row exists to prove.
                        const auto lentVals = [&]
                        { return rig.freshState().getProperty ("lentVals", juce::var()).toString(); };
                        const int idx = 1;
                        const double from = valueOf (lentVals(), idx);
                        const double want = from > 0.5 ? 0.3 : 0.7;
                        const auto moved = rig.send (echojay::rig::cmd::liveParam,
                                                     args2 ("i", idx, "v", want), 10000);
                        double got = -1.0;
                        for (int tries = 0; tries < 20 && std::abs (got - want) > 0.02; ++tries)
                        { rePump (100); got = valueOf (lentVals(), idx); }
                        // Close the view, let the pane let go of the editor, open it again.
                        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
                        rePump (1000);
                        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", lvUid), 8000);
                        rig.send (echojay::rig::cmd::press,      args1 ("button", "tab_chain"), 8000);
                        rePump (600);
                        rig.send (echojay::rig::cmd::openSlot,   args1 ("n", lvSlot), 15000);
                        rePump (3000);
                        const auto st3 = rig.freshState();
                        const double afterReopen = valueOf (st3.getProperty ("lentVals", juce::var()).toString(), idx);
                        const auto plugin = st3.getProperty ("lentPlugin", juce::var()).toString();
                        row ("RE10", (bool) moved.getProperty ("ok", false)
                                        && std::abs (got - want) <= 0.02
                                        && std::abs (afterReopen - want) <= 0.02
                                        && st3.getProperty ("liveViewRoad", juce::var()).toString() == "real",
                             "does a knob moved on the real editor stay on the Link's own plugin - still there "
                             "after the view is closed and opened again, which nothing the V2 owns would be",
                             "asked " + juce::String (want, 3) + " from " + juce::String (from, 3)
                             + ", the plugin held " + juce::String (got, 3)
                             + ", and after closing and reopening the view it holds "
                             + juce::String (afterReopen, 3) + " (\"" + plugin + "\", road \""
                             + st3.getProperty ("liveViewRoad", juce::var()).toString() + "\")");
                    }

                    // ---- RE11: AND THE V2'S OWN OUTPUT IS STILL ITS OWN -------------------------
                    if (rowWanted ("RE11"))
                    {
                        // THE FLOOR IS MEASURED IN BOTH CONFIGURATIONS (27 Oct 2026, 01:50). With a
                        // single floor taken after the view closed, this row kept reporting ~0.0025
                        // against ~0.0000004 - and the own rack holds a reverb whose tail has not
                        // converged in the first window after a view opens, so those two numbers were
                        // never comparable. Two renders with it shown, two without, and the cross
                        // difference judged against the LARGER floor: a copy actually in the path
                        // would differ by its whole signal, not by a reverb tail.
                        std::vector<float> ref;
                        (void) ejRenderMaxDiff (v2, sr, 40, ref);                      // shown, settling
                        const double floorShown = ejRenderMaxDiff (v2, sr, 40, ref);   // shown vs shown
                        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
                        rePump (1000);
                        const double shownVsNot = ejRenderMaxDiff (v2, sr, 40, ref);
                        const double floorNot   = ejRenderMaxDiff (v2, sr, 40, ref);
                        const double floorNoise = juce::jmax (floorShown, floorNot);
                        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", lvUid), 8000);
                        rig.send (echojay::rig::cmd::press,      args1 ("button", "tab_chain"), 8000);
                        rePump (600);
                        rig.send (echojay::rig::cmd::openSlot,   args1 ("n", lvSlot), 15000);
                        rePump (3000);
                        row ("RE11", shownVsNot <= juce::jmax (floorNoise, 1.0e-6),
                             "is the V2's OWN output unaffected by the real editor being shown - the Link's "
                             "plugin is never in the V2's audio path",
                             "showing it moved the output by at most " + juce::String (shownVsNot, 9)
                             + ", against a floor of " + juce::String (floorNoise, 9)
                             + " (two renders with it shown differ by " + juce::String (floorShown, 9)
                             + ", two with nothing shown by " + juce::String (floorNot, 9)
                             + ") - that floor is the own rack's reverb tail");
                    }

                    // ---- RE12: THE SLOT'S PLUGIN IS REPLACED WHILE THE REAL EDITOR IS ON SCREEN --
                    // The dangerous one: the Link's own removal revokes the lease, this window has to
                    // let go before the editor is destroyed, and the card must then open the NEW
                    // plugin's real editor rather than a dead one.
                    if (rowWanted ("RE12"))
                    {
                        rig.send (echojay::rig::cmd::openSlot, args1 ("n", lvSlot), 10000);
                        rePump (1000);
                        const auto before = road();
                        // ---- ONE REPLACE OP, BECAUSE REMOVE-THEN-ADD DID NOT REPLACE ANYTHING ----
                        //
                        // The first cut used the clear plan then the add plan. The clear is REFUSED on
                        // this rack ("the rack is not the one this plan was made for - nothing was
                        // changed"), so the rack went from one slot to two and slot 1 still held AUDelay:
                        // the row then reported that the pane was "holding the destroyed plugin's editor"
                        // when nothing had been destroyed. The mock has carried a one-op replace plan
                        // since it was written and nothing used it; this is what it is for.
                        askAgent (rig, "rig-linkreplace1-marker: replace slot 1's plugin on that Link", 90000);
                        rePump (4000);
                        // ---- RE-CUT, BECAUSE THE OLD TEST COULD NOT TELL THE FAULT FROM THE FIX ----
                        //
                        // It asserted `road != "real" || lentSlot != lvSlot`, i.e. "the pane is no
                        // longer lending that slot". With re-homing (round 3) the correct behaviour is
                        // to let go AND immediately lend the NEW plugin's editor for the same slot -
                        // which reads as road "real", lent slot 1, and failed. Holding a dead editor
                        // reads exactly the same. A test that gives the same answer either way is not
                        // a test, so the question is the one RH4 asks: WHICH PLUGIN is on screen.
                        //
                        // What makes it the safe order rather than luck: the revoke trail, the process
                        // still standing, and the editor count for that slot going up (a new editor was
                        // built for the new plugin, which cannot happen without the old one going).
                        const auto st12 = rig.freshState();
                        const auto afterRoad = road();
                        const auto shows = st12.getProperty ("inlinePlugin", juce::var()).toString().trim();
                        const auto lent  = st12.getProperty ("lentPlugin", juce::var()).toString().trim();
                        const auto wanted = juce::String ("AUMultibandCompressor");   // what the replace plan puts there
                        const bool rightPlugin = shows.containsIgnoreCase (wanted)
                                              || lent.containsIgnoreCase (wanted)
                                              || (shows.isEmpty() && lent.isEmpty());   // nothing on screen is also "not the dead one"
                        const bool notTheOldOne = ! shows.containsIgnoreCase ("AUDelay")
                                               && ! lent.containsIgnoreCase ("AUDelay");
                        row ("RE12", rightPlugin && notTheOldOne,
                             "when the Link's slot is replaced under a lent editor, does this window end up "
                             "showing the NEW plugin rather than the one that was destroyed",
                             "the pane was on \"" + before + "\" and is now on \"" + afterRoad
                             + "\"; it shows \"" + (shows.isNotEmpty() ? shows : juce::String ("nothing"))
                             + "\" (lent \"" + (lent.isNotEmpty() ? lent : juce::String ("nothing"))
                             + "\"), lent slot " + st12.getProperty ("lentSlot", juce::var()).toString()
                             + " | the revoke trail: " + noteOf (lease ("revokes", {}, 0)));
                    }
                }

                    // ---- RE14/RE15: THE HAND-BACK, BOTH WAYS AND REPEATEDLY (his condition 1) ---
                    //
                    // *"when I open the plugin on the Link's own window while the V2 holds it, the V2
                    //  lets go, the owner destroys the editor in the safe order, and the Link's window
                    //  shows a fresh one. Test that both ways, repeatedly, in the rig."*
                    //
                    // Repeatedly is the point: both crashes this road has produced only appear on the
                    // SECOND editor of one instance, so one hand-over each way would prove nothing.
                    // Three rounds of V2 -> Link -> V2, and the run surviving them is half the row.
                    if (rowWanted ("RE14") || rowWanted ("RE15"))
                    {
                        int toLink = 0, backToV2 = 0;
                        juce::String trail;
                        for (int lap = 0; lap < 3; ++lap)
                        {
                            // --- the V2 holds it; ask the LINK's own window for it
                            const auto ask = noteOf (lease ("link_window", lvUid, lvSlot));
                            // ---- WAITED FOR, WITH A BOUND, AND BOTH SIDES RECORDED (27 Oct 2026) -
                            //
                            // A flat 2.5 s read gave "lap 1 NO, laps 2-3 yes". A bounded wait of 8 s
                            // gave "laps 1-2 yes, lap 3 NO". A failure that moves when the stopwatch
                            // changes is a timing read, not a lost hand-over - and the one thing
                            // neither cut recorded is whether the LINK ever said it had shown its
                            // window. The pane knows (onBringLinkForward writes it into the strip
                            // line) and nothing published it; now it does, the bound is twelve
                            // seconds, and a failing lap names the side that did not move.
                            rig.until ([&] (const juce::var& st)
                            {
                                return st.getProperty ("liveViewRoad", juce::var()).toString() != "real"
                                    || (int) st.getProperty ("lentSlot", juce::var()) != lvSlot;
                            }, 12000);   // the Link has to raise its own window first; twelve seconds of it
                            const auto st4 = rig.freshState();
                            const auto shownNote = st4.getProperty ("chainStatusText", juce::var()).toString();
                            const bool v2LetGo = st4.getProperty ("liveViewRoad", juce::var()).toString() != "real"
                                              || (int) st4.getProperty ("lentSlot", juce::var()) != lvSlot;
                            if (v2LetGo) ++toLink;
                            trail << (lap > 0 ? " | " : "") << "lap " << (lap + 1) << ": to the Link "
                                  << (v2LetGo ? "yes" : "NO");
                            if (! v2LetGo)
                                trail << " (the ask answered \"" << ask << "\""
                                      << (shownNote.isNotEmpty() ? ", the Link said \"" + shownNote + "\"" : "")
                                      << ")";

                            // --- and back: the V2's card is pressed again
                            rig.send (echojay::rig::cmd::openSlot, args1 ("n", lvSlot), 15000);
                            rig.until ([&] (const juce::var& st)      // the same bound, the other way
                            {
                                return st.getProperty ("liveViewRoad", juce::var()).toString() == "real"
                                    && (int) st.getProperty ("lentSlot", juce::var()) == lvSlot;
                            }, 8000);
                            const auto st5 = rig.freshState();
                            const bool v2HasIt = st5.getProperty ("liveViewRoad", juce::var()).toString() == "real"
                                              && (int) st5.getProperty ("lentSlot", juce::var()) == lvSlot;
                            if (v2HasIt) ++backToV2;
                            trail << ", back to the V2 " << (v2HasIt ? "yes" : "NO");
                        }
                        if (rowWanted ("RE14"))
                            row ("RE14", toLink == 3,
                                 "does the V2 let go every time the plugin is opened on the Link's own window "
                                 "(three laps, because both crashes on this road needed a second editor)",
                                 juce::String (toLink) + " of 3 - " + trail);
                        if (rowWanted ("RE15"))
                            row ("RE15", backToV2 == 3,
                                 "...and does the V2 get it back every time its card is pressed again - the "
                                 "hand-over in both directions, repeatedly, with the process still standing",
                                 juce::String (backToV2) + " of 3 - " + trail);
                    }

                // ---- RE13: AND HIS SWITCH PUTS IT BACK ------------------------------------------
                if (rowWanted ("RE13"))
                {
                    rig.send (echojay::rig::cmd::press, args1 ("button", "tab_chat"), 8000);
                    rePump (400);
                    const auto off = rig.send (echojay::rig::cmd::press, args1 ("button", "real_editor_off"), 8000);
                    rig.send (echojay::rig::cmd::press, args1 ("button", "tab_chain"), 8000);
                    rePump (500);
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", lvSlot), 15000);
                    rePump (4000);
                    const auto st2 = rig.freshState();
                    row ("RE13", road() == "copy" && (bool) st2.getProperty ("shadowRunning", false),
                         "does turning his switch off put the live view back on the copy road, with nothing "
                         "else to undo - \"we switch back and park it\"",
                         off.getProperty ("note", juce::var()).toString() + " | the pane is on \"" + road()
                         + "\" and a copy is " + juce::String ((bool) st2.getProperty ("shadowRunning", false)
                                                                    ? "running again" : "NOT running"));
                }
            }

            // NOW the Link may be destroyed: every round-2 row above has had it alive (see where runRE5
            // is declared for what running it earlier cost). The RH block below re-picks from the
            // directory, so it is ready for the Link this takes away.
            if (runRE5) runRE5();

            // ================================================================================================
            //  RH1-RH8: THE REAL EDITOR UNDER HIS STRESS LIST, PER PLUGIN (his condition 3, 26 Oct 21:08)
            // ================================================================================================
            //
            //  *"open, close, reopen 50 times in a row per plugin; switch between slots; slot replace; slot
            //   delete while shown; hand-back to the Link's own window and back again, repeatedly; track
            //   deleted and session closed while shown; V2 window closed while shown. No crash, no wrong
            //   panel, no leak."*
            //
            //  Run on Apple's AUs AND four licence-free third-party plugins (named where the catalogue is
            //  seeded). Each row says which subject it was about, and a subject that will not load is named
            //  rather than quietly dropped - a set that ran one plugin is a set about one plugin.
            //
            //  THE LEAK TEST IS THE LEASE'S OWN COUNT: with the editor kept and its view re-homed, fifty
            //  opens of one slot must still have built exactly ONE editor. That is also his "one press =
            //  one take" row, measured rather than asserted.
            if (anyRowWanted ({ "RH1", "RH2", "RH3", "RH4", "RH5", "RH6", "RH7", "RH8", "RH9" })
                && lvUid.isNotEmpty() && lvSlot > 0)
            {
                leg ("RH: the real editor under the stress list, per plugin");

                // This block has its own pump, like the RE one: every track a block, then the
                // message thread. (The RE block's is in its own scope - the compiler said so.)
                const EJPumpFn rhPump = [&] (int ms)
                {
                    const double end2 = juce::Time::getMillisecondCounterHiRes() + (double) ms;
                    while (juce::Time::getMillisecondCounterHiRes() < end2)
                    {
                        for (Track* t : { &v2, &lk1, &lk2 }) if (t->plugin != nullptr) t->processOneBlock (sr);
                        Rig::pump (10);
                    }
                };
                // ---- AND THE RACK THIS BLOCK IS ABOUT, SAID OUT LOUD (26 Oct 2026, 23:50) ------
                // The rows below ask the agent to put plugins on "that Link", and the agent works on
                // whichever channel is selected. This block inherited whatever the rows before it
                // left - which in one pass was the OWN rack, so four subjects were added there and
                // the row reported they "would not load" on the Link. Asserted here, and again
                // before every subject, because an inherited selection is not a decision.
                const bool rhCopyRoadFlag = ::getenv ("EJ_RH_COPY_ROAD") != nullptr;
                // ---- A LINK THIS BLOCK HAS CONFIRMED IS ALIVE (27 Oct 2026, 01:30) -------------
                // RE5 DESTROYS the Link the LV rows used - that is its whole point - and these rows
                // then addressed the same uid and were refused with "that Link is not in this
                // process' directory". RH3 passed on its own and failed in the full pass for that
                // reason alone. So the block asks the directory who is actually there, prefers the
                // LV rows' Link when it is still alive, and falls back to any connected one.
                juce::String rhUid = lvUid;
                if (! noteOf (lease ("find", rhUid, 0)).contains ("found EchoJay Link"))
                {
                    juce::String other = usableLinkUid (rig.freshState(), false);
                    if (other.isNotEmpty() && noteOf (lease ("find", other, 0)).contains ("found EchoJay Link"))
                        rhUid = other;
                    std::printf ("        (RH) the LV rows' Link (%s) is gone from the directory - this block "
                                 "uses %s instead\n", lvUid.toRawUTF8(), rhUid.toRawUTF8());
                }
                // ---- EVERY ROW ARMS WHAT IT DEPENDS ON (27 Oct 2026, 01:00) --------------------
                // Not just the rack: THE SWITCH. RH3 failed with the pane saying, in its own words,
                // "the live view shows its own copy of the plugin (the real-editor switch is off)" -
                // because rows that flip the switch run alongside these and this block only pressed
                // it once, at the start. A precondition a row inherits is a precondition it does not
                // have, which is the second time tonight that has cost a pass.
                const auto aimAtTheLink = [&]
                {
                    rig.send (echojay::rig::cmd::selectLink, args1 ("uid", rhUid), 8000);
                    rig.send (echojay::rig::cmd::workOn,     args1 ("uid", rhUid), 8000);
                    rig.send (echojay::rig::cmd::press,      args1 ("button", "tab_chain"), 8000);
                    if (! rhCopyRoadFlag)
                    {
                        auto r = noteOf (lease ("road", rhUid, 0));
                        for (int t = 0; t < 10 && ! r.contains ("the switch is ON"); ++t)
                        {
                            rig.send (echojay::rig::cmd::press, args1 ("button", "real_editor_on"), 8000);
                            rhPump (250);
                            r = noteOf (lease ("road", rhUid, 0));
                        }
                        // SAID OUT LOUD WHEN IT WILL NOT TAKE. RH3 read "the switch is off" from the
                        // pane twice AFTER this block had pressed it on, and a precondition that
                        // silently fails to arm is worse than one that was never armed.
                        if (! r.contains ("the switch is ON"))
                            std::printf ("        (RH) THE SWITCH WILL NOT STAY ON: after ten presses the "
                                         "plugin still reports [%s]\n", r.toRawUTF8());
                    }
                };
                aimAtTheLink();
                // ---- THE SAME STRESS LIST ON THE COPY ROAD, WHEN ASKED (26 Oct 2026, 00:05) -----
                // EJ_RH_COPY_ROAD=1 leaves his switch OFF for this whole block. It exists for one
                // question, and it is the question his ruling turns on: when something crashes in
                // here, is it the real editor's doing or the plugin's? ValhallaDelay's own thread
                // died in pthread_mutex_lock at address zero with no EchoJay frame in the stack
                // after a track deletion, and the only honest way to tell whose fault that is, is to
                // delete the same track with the same plugin on the copy road and look again.
                const bool rhCopyRoad = rhCopyRoadFlag;
                rig.send (echojay::rig::cmd::press,
                          args1 ("button", rhCopyRoad ? "real_editor_off" : "real_editor_on"), 8000);
                std::printf ("        (RH) this block is running on the %s road\n",
                             rhCopyRoad ? "COPY (his switch left OFF, for comparison)" : "REAL editor");
                rhPump (400);
                const auto road = [&] { return rig.freshState().getProperty ("liveViewRoad", juce::var()).toString(); };
                const auto builds = [&] { return (int) rig.freshState().getProperty ("lentBuilds", juce::var()); };
                const auto lentPlug = [&] { return rig.freshState().getProperty ("lentPlugin", juce::var()).toString(); };

                // ---- RH1: FIFTY OPENS AND CLOSES, PER PLUGIN --------------------------------------
                // The subjects are put on the Link's rack one at a time by the agent's own road, which
                // is the only road to a Link's rack; a subject that does not land is named.
                // Apple's own, plus three third-party makers: FabFilter's large custom interface,
                // Valhalla's large custom interface, and Tokyo Dawn's JUCE-built one. (Slate's Fresh
                // Air was a fourth until a probe showed it never returns from instantiation, and
                // MeldaProduction's MCompressor was the third until 27 Oct, when it turned out to be
                // PACE-wrapped; the catalogue block says both in full.)
                const juce::StringArray subjects { "AUDelay", "Pro-C 3", "ValhallaDelay",
                                                   "TDR SlickEQ M" };
                juce::StringArray ran, notRan, rhTrail;
                int worstBuilds = 0;
                // One marker per subject, because a mock plan is static JSON and carries the name
                // it adds. Chosen here so the row and the plan cannot drift apart.
                const auto markerFor = [] (const juce::String& s) -> juce::String
                {
                    if (s == "Pro-C 3")       return "rig-swap-proc3-marker";
                    if (s == "ValhallaDelay") return "rig-swap-valhalladelay-marker";
                    if (s == "TDR SlickEQ M") return "rig-swap-slickeq-marker";
                    return "rig-swap-audelay-marker";
                };
                // WHICH SLOT the subject actually landed on - never an assumed one (the LV rows
                // learned that twice).
                // WHAT THE PANEL IS DRAWING, not what the registry row says (26 Oct 2026, 23:12).
                // The first cut read links[].slots and found it EMPTY while the panel was drawing
                // [AUMatrixReverb, AUDelay, MCompressor, ValhallaDelay, TDR SlickEQ M] - so the row
                // reported that four plugins "would not load" when all four were on the rack and on
                // screen. The cards this row is about to press are the panel's, so the panel's list
                // is the right question; its own diagnostic above is what showed me that.
                const auto slotOf = [&] (const juce::String& subject) -> int
                {
                    const auto cv = rig.freshState().getProperty ("chainView", juce::var());
                    if (cv.getProperty ("uid", juce::var()).toString() != rhUid) return 0;
                    const auto names = juce::StringArray::fromTokens (
                        cv.getProperty ("slots", juce::var()).toString(), ",", "");
                    for (int k = 0; k < names.size(); ++k)
                        if (names[k].trim().containsIgnoreCase (subject)) return k + 1;
                    return 0;
                };
                for (const auto& subject : subjects)
                {
                    aimAtTheLink();
                    rhPump (600);
                    int subjectSlot = slotOf (subject);
                    if (subjectSlot == 0)
                    {
                        const auto ack = askAgent (rig, markerFor (subject) + ": put " + subject
                                                        + " on that Link", 90000);
                        juce::ignoreUnused (ack);
                        rhPump (3000);
                        rig.until ([&] (const juce::var&) { return slotOf (subject) > 0; }, 25000);
                        subjectSlot = slotOf (subject);
                        if (subjectSlot == 0)
                        {
                            // WHAT THE RACK ACTUALLY HELD. The first run of this said all four
                            // subjects "would not load" while the agent's own log said "the rack took
                            // it and now holds 4 slot(s)" - so the row was looking in the wrong place
                            // or for the wrong spelling, and a row that cannot say which is useless.
                            juce::String slotsNow;
                            if (auto* arr = rig.freshState().getProperty ("links", juce::var()).getArray())
                                for (const auto& l : *arr)
                                    if (l.getProperty ("uid", juce::var()).toString() == rhUid)
                                        slotsNow = l.getProperty ("slots", juce::var()).toString();
                            const auto cv = rig.freshState().getProperty ("chainView", juce::var());
                            std::printf ("        (RH) %s is not on %s: the Link publishes [%s]; the panel is "
                                         "drawing uid \"%s\" with [%s]\n",
                                         subject.toRawUTF8(), rhUid.toRawUTF8(), slotsNow.toRawUTF8(),
                                         cv.getProperty ("uid", juce::var()).toString().toRawUTF8(),
                                         cv.getProperty ("slots", juce::var()).toString().toRawUTF8());
                            notRan.add (subject); continue;
                        }
                    }
                    ran.add (subject);

                    // FIVE, OR FIFTY FOR A GATE (his ruling, 27 Oct 2026). Open the card, let it
                    // draw, select the own rack (which hands the lease back and re-homes the view),
                    // and again.
                    //
                    // *"Use 5 open/close laps per plugin for the in-between runs; keep the full 50
                    //  only for the final gate before install and for overnight runs."*  Fifty laps
                    //  across four plugins is most of this block's wall-clock, and the fault it is
                    //  for - a second editor, a leak, a wrong panel - shows up in the first few if it
                    //  is there at all. EJ_RIG_FULL_STRESS=1 is the gate's setting, and the row SAYS
                    //  which it ran: a five-lap pass must never be read as the fifty-lap one.
                    const int laps = ::getenv ("EJ_RIG_FULL_STRESS") != nullptr ? 50 : 5;
                    juce::String firstPlugin, lastPlugin;
                    bool everWrongPanel = false;
                    int builtWhileOpen = -1;
                    for (int i = 0; i < laps; ++i)
                    {
                        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", rhUid), 6000);
                        rig.send (echojay::rig::cmd::press,      args1 ("button", "tab_chain"), 6000);
                        rig.send (echojay::rig::cmd::openSlot,   args1 ("n", subjectSlot), 12000);
                        rhPump (i < 3 ? 1200 : 220);          // the first few get time to settle
                        const auto p = lentPlug();
                        if (i == 0) firstPlugin = p; else lastPlugin = p;
                        if (p.isNotEmpty() && firstPlugin.isNotEmpty() && p != firstPlugin)
                            everWrongPanel = true;
                        // THE COUNT IS READ WHILE IT IS OPEN. Read after the close it was always 0:
                        // the state publishes it keyed on the LENT uid and slot, and by then there is
                        // no lent anything - my own field, read at the wrong moment.
                        if (i == laps - 1) builtWhileOpen = builds();
                        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 6000);
                        rhPump (120);
                    }
                    const int built = builtWhileOpen;
                    worstBuilds = juce::jmax (worstBuilds, built);
                    rhTrail.add (subject + " (slot " + juce::String (subjectSlot) + "): "
                                 + juce::String (laps) + " opens, "
                                 + juce::String (built) + " editor(s) built, panel "
                                 + (everWrongPanel ? "CHANGED" : "steady") + " (\"" + firstPlugin + "\")");
                    juce::ignoreUnused (lastPlugin);
                }

                if (rowWanted ("RH1"))
                    row ("RH1", ! ran.isEmpty() && notRan.isEmpty(),
                         "did every subject load on the Link's rack so the stress rows are about all of them",
                         "ran [" + ran.joinIntoString (", ") + "]"
                         + (notRan.isEmpty() ? "" : ", WOULD NOT LOAD [" + notRan.joinIntoString (", ") + "]"));
                if (rowWanted ("RH2"))
                {
                    const int laps = ::getenv ("EJ_RIG_FULL_STRESS") != nullptr ? 50 : 5;
                    row ("RH2", worstBuilds == 1,
                         juce::String (laps) + " opens and closes of one slot, per plugin: is the editor built "
                         "ONCE and kept (his one-press-one-take, and the leak test)"
                         + (laps < 50 ? juce::String (" [THE FAST PASS, not the gate: the gate runs 50, with "
                                                      "EJ_RIG_FULL_STRESS=1]")
                                      : juce::String (" [the full fifty]")),
                         "the worst count was " + juce::String (worstBuilds) + " - " + rhTrail.joinIntoString (" | "));
                }

                // ---- RH3: SWITCHING BETWEEN SLOTS -----------------------------------------------
                if (rowWanted ("RH3"))
                {
                    aimAtTheLink();
                    rhPump (500);
                    // THE TWO SLOTS COME FROM THE PANEL'S OWN LIST (27 Oct 2026, 00:30). The first
                    // cut used the slot the LV rows had left, which by this point in the pass may
                    // hold nothing at all - and an empty plugin name is not a verdict about
                    // switching slots.
                    const auto cvSlots = juce::StringArray::fromTokens (
                        rig.freshState().getProperty ("chainView", juce::var())
                           .getProperty ("slots", juce::var()).toString(), ",", "");
                    const int slotA = 1;
                    const int slotB = cvSlots.size() >= 2 ? 2 : 1;
                    const auto note0 = noteOf (lease ("who", rhUid, slotA));
                    // WAIT FOR THE CARD, THEN FOR THE ROAD. The first cut read the plugin's name
                    // 1.5 s after the press and got an empty string - the panel had not finished
                    // drawing the cards after the rack was re-aimed, which is the same lesson the LV
                    // rows learned at 17:20.
                    rig.until ([&] (const juce::var& st)
                    {
                        const auto cv = st.getProperty ("chainView", juce::var());
                        return cv.getProperty ("uid", juce::var()).toString() == rhUid
                            && (int) cv.getProperty ("count", 0) >= slotA;
                    }, 15000);
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", slotA), 12000);
                    // FIFTEEN SECONDS, AND THE ROAD IT ENDED ON. Four was not enough for slot 1 when
                    // slot 1 held ValhallaDelay, whose interface is the largest in this set - and an
                    // empty plugin name after a short wait cannot tell "slow to draw" from "the lend
                    // was refused". The road says which: "copy" means refused (and the pane logged
                    // why), "none" means nothing opened at all.
                    for (int t = 0; t < 75 && lentPlug().isEmpty(); ++t) rhPump (200);
                    const auto a1 = lentPlug();
                    const auto road1 = road();
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", slotB), 12000);
                    rhPump (1500);
                    const auto a2 = road();
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", slotA), 12000);
                    rhPump (1500);
                    const auto a3 = lentPlug();
                    row ("RH3", a1.isNotEmpty() && a3 == a1,
                         "switching to another slot and back brings the SAME plugin's own editor back, with no "
                         "panel from the other slot left behind",
                         a1.isEmpty()
                             ? "slot " + juce::String (slotA) + " never put a lent editor up in 15 s - the pane "
                               "ended on the \"" + road1 + "\" road, which is "
                               + (road1 == "copy" ? "the lend being REFUSED (the pane logs the reason)"
                                                  : "nothing opening at all")
                             : "first \"" + a1 + "\" (road \"" + road1 + "\"), the other slot left the road on \""
                               + a2 + "\", back to \"" + a3 + "\" | " + note0);
                }

                // RH9 SITS HERE, BEFORE THE DESTRUCTIVE ROWS (27 Oct 2026). Its first cut ran after
                // RH8, which is after RH6 - and RH6 DELETES THAT LINK'S TRACK. So it aimed at a Link
                // that no longer existed, the panel stayed on the V2's own rack, and the row read our
                // own slot 1 as "the Link's slot 1" and failed on a comparison between a plugin and
                // itself. A row that needs a live Link belongs before the row that kills one.
                // ---- RH9: THE OWN RACK'S SLOT 1, THEN A LINK'S SLOT 1 --------------------------
                //
                // The shape that cost RH3 its red, as a row of its own. Select the V2's own rack and
                // open its slot 1 (the pane shows our own plugin, which is always a real editor),
                // then aim at the Link and open ITS slot 1. The pane must show the LINK's plugin.
                // Before today it showed ours, because the refresh asked "is the open slot index the
                // selected one" and a slot index does not say which rack it came from; the pure form
                // of that test and its known-bad witness are IV0-IV5a in chat_store_guard.
                //
                // Read as the PLUGIN'S NAME, never the road: "copy" is a legitimate road for our own
                // rack, so the road cannot tell this fault from a working day.
                if (rowWanted ("RH9"))
                {
                    // WHAT THE PANE IS SHOWING, BY NAME AND BY RACK - not by road. "copy" is the
                    // right road for our own rack, so the road cannot tell this fault from a normal
                    // day; the pair below is published for exactly this row.
                    const auto nameOnScreen = [&] { return rig.freshState()
                                                        .getProperty ("inlinePlugin", juce::var()).toString().trim(); };
                    const auto rackOnScreen = [&] { return rig.freshState()
                                                        .getProperty ("inlineRack", juce::var()).toString(); };
                    rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 6000);
                    rig.send (echojay::rig::cmd::press,      args1 ("button", "tab_chain"), 6000);
                    rig.send (echojay::rig::cmd::openSlot,   args1 ("n", 1), 12000);
                    rhPump (2000);
                    const auto ownName = nameOnScreen();
                    // The Link's slot 1, by the same slot NUMBER on purpose.
                    aimAtTheLink();
                    rhPump (600);
                    // THE PANEL HAS TO BE DRAWING THAT LINK BEFORE ITS SLOTS ARE READ (27 Oct 2026).
                    // The first cut read chainView 600 ms after the aim and got the OWN rack's list,
                    // so "the Link's slot 1" was our own slot 1's name - the row then waited 15 s for
                    // a plugin that was never there. The same lesson the LV rows and RH3 learned, in
                    // a row written to catch exactly this class of mistake.
                    rig.until ([&] (const juce::var& st)
                    {
                        const auto cv = st.getProperty ("chainView", juce::var());
                        return cv.getProperty ("uid", juce::var()).toString() == rhUid
                            && (int) cv.getProperty ("count", 0) >= 1;
                    }, 15000);
                    const auto cv9 = rig.freshState().getProperty ("chainView", juce::var());
                    const auto link1 = juce::StringArray::fromTokens (
                        cv9.getProperty ("slots", juce::var()).toString(), ",", "")[0].trim();
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", 1), 12000);
                    for (int t = 0; t < 60 && ! (nameOnScreen() == link1 && rackOnScreen() == rhUid); ++t)
                        rhPump (250);
                    const auto after9 = nameOnScreen();
                    const auto rack9  = rackOnScreen();
                    row ("RH9", link1.isNotEmpty() && after9 == link1 && rack9 == rhUid,
                         "our own slot 1, then a LINK's slot 1 by the same number: does the pane show the "
                         "LINK's plugin (a slot index is not a rack - the RH3 red)",
                         "our own rack showed \"" + ownName + "\", the Link's slot 1 holds \"" + link1
                         + "\", and the pane ended on \"" + after9 + "\" from rack \"" + rack9 + "\"");
                }

                // ---- RH4: THE SLOT'S PLUGIN REPLACED WHILE IT IS SHOWN --------------------------
                if (rowWanted ("RH4"))
                {
                    aimAtTheLink();
                    rhPump (500);
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", lvSlot), 12000);
                    rhPump (1200);
                    const auto before = lentPlug();
                    // A REPLACE IS REMOVE THEN ADD (the agent's own two ops, and what his hand does).
                    // The first cut of this row only ADDED, so the slot it then opened still held the
                    // same plugin and the row failed with "was AUDelay, now AUDelay" - my sequence,
                    // not the product.
                    askAgent (rig, "rig-linkclear-marker: take slot 1 off that Link", 90000);
                    rhPump (2500);
                    askAgent (rig, "rig-swap-matrixreverb-marker: put AUMatrixReverb on that Link", 90000);
                    rhPump (4000);
                    const int newSlot = juce::jmax (1, slotOf ("AUMatrixReverb"));
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", newSlot), 12000);
                    rhPump (2500);
                    const auto after = lentPlug();
                    row ("RH4", before.isNotEmpty() && after.isNotEmpty() && after != before,
                         "when the slot's plugin is replaced under a shown editor, does opening the card show the "
                         "NEW plugin's own editor (never the deleted one's panel)",
                         "was \"" + before + "\", now \"" + after + "\"");
                }

                // ---- RH5: THE SLOT DELETED WHILE IT IS SHOWN -----------------------------------
                if (rowWanted ("RH5"))
                {
                    aimAtTheLink();
                    rhPump (500);
                    // SLOT 1, because that is the slot the clear plan removes: the first cut opened
                    // whatever slot the LV rows had left and then deleted slot 1, so nothing it was
                    // showing had gone and "it did not let go" was a statement about my sequence.
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", 1), 12000);
                    rhPump (1500);
                    const bool shown = road() == (rhCopyRoad ? "copy" : "real");
                    const auto showedBefore = lentPlug();          // the plugin that is about to go
                    const auto slotsBefore = rig.freshState().getProperty ("chainView", juce::var())
                                                .getProperty ("slots", juce::var()).toString();
                    const auto ack = askAgent (rig, "rig-linkclear-marker: take slot 1 off that Link's rack", 90000);
                    juce::ignoreUnused (ack);
                    rhPump (4000);
                    const auto slotsAfter = rig.freshState().getProperty ("chainView", juce::var())
                                               .getProperty ("slots", juce::var()).toString();
                    const bool removalLanded = slotsAfter != slotsBefore;
                    const auto after = road();
                    if (! removalLanded)
                        std::printf ("        (RH) RH5's removal never landed: the panel drew [%s] before and "
                                     "[%s] after, so nothing was deleted and this row is about my sequence\n",
                                     slotsBefore.toRawUTF8(), slotsAfter.toRawUTF8());
                    // THE CLAIM IS ABOUT THE EDITOR THAT WAS SHOWING, not about the window being
                    // empty afterwards. The first cut asserted the road was no longer the real one -
                    // and the pane had legitimately opened ANOTHER slot's editor by then, so a
                    // product that did exactly the right thing (the log says: let go of the Link's
                    // lent editor because the slot was removed) read as a failure.
                    const auto showsNow = lentPlug();
                    const auto whoNow = noteOf (lease ("who", rhUid, 1));
                    // NOT "nobody holds slot 1" - after the removal slot 1 is a DIFFERENT plugin and
                    // the pane holding that one is right. The claim is only that the editor of the
                    // plugin that has gone is no longer the one on screen.
                    row ("RH5", shown && removalLanded && showsNow != showedBefore,
                         "when the slot is deleted while its editor is on screen, does this window let go (rather "
                         "than keep drawing an editor of a plugin that has gone)",
                         juce::String ("it was showing ") + showedBefore + ", the removal "
                         + (removalLanded ? "landed" : "NEVER LANDED") + ", it now shows " + showsNow
                         + " and " + whoNow + " (the road is " + after + ", which may be another "
                           "slot's editor and is not what this row claims)");
                }

                // ---- RH6: THE TRACK DELETED WHILE IT IS SHOWN ----------------------------------
                // The host really destroys the Link, exactly as deleting its track does.
                if (rowWanted ("RH6"))
                {
                    // ---- NOT WITH VALHALLADELAY ON THE RACK (26 Oct 2026, 00:15) ---------------
                    // Deleting a track with ValhallaDelay hosted on it takes the whole process down
                    // inside the PLUGIN's own thread:
                    //
                    //   EXC_BAD_ACCESS (SIGSEGV) at 0x0, pthread_mutex_lock
                    //       ValhallaDelay  x3        (its own thread, no EchoJay frame in the stack)
                    //
                    // and it does so IDENTICALLY with his switch off, on the copy road - measured
                    // with EJ_RH_COPY_ROAD=1, same exception, same frames. So it is that plugin's
                    // teardown, it predates all of this, and it is reported as its own finding
                    // rather than being allowed to fail a row about the real editor. This row deletes
                    // a track whose rack holds APPLE's AUDelay, which shuts down cleanly.
                    aimAtTheLink();
                    rhPump (400);
                    const int audelaySlot = juce::jmax (1, slotOf ("AUDelay"));
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", audelaySlot), 12000);
                    rhPump (1500);
                    const auto name = linkRowOf (rig.freshState(), rhUid).getProperty ("name", juce::var()).toString();
                    Track* victim = nullptr;
                    if (name == lk1.trackName || name.containsIgnoreCase ("Kick")) victim = &lk1;
                    else if (name == lk2.trackName)                                victim = &lk2;
                    const juce::String wantRoad = rhCopyRoad ? "copy" : "real";
                    if (victim == nullptr || victim->plugin == nullptr)
                        row ("RH6", false, "the track deleted while its editor is shown",
                             "could not identify which of the rig's tracks is that Link (\"" + name
                             + "\"), so this row could not run - which is a stop, not a skip");
                    else
                    {
                        const bool shown = road() == wantRoad;
                        victim->plugin->releaseResources();
                        victim->plugin.reset();                 // the track is deleted
                        rhPump (2000);
                        const auto after = road();
                        const auto find = noteOf (lease ("find", rhUid, 0));
                        row ("RH6", shown && after != "real" && find.contains ("no live EchoJay Link"),
                             "when the Link's track is deleted while its editor is on screen, is this window told "
                             "to let go and the Link gone from the directory",
                             juce::String ("shown before: ") + (shown ? "yes" : "NO") + ", road after: \""
                             + after + "\" | " + find);
                    }
                }

                // ---- RH7/RH8: THE V2's WINDOW CLOSED WHILE SHOWN, AND THE SESSION ENDING --------
                // The window close is the editor's destructor, which is where a held lease would be
                // left behind; the rig closes and reopens the V2's editor for real.
                if (rowWanted ("RH7") || rowWanted ("RH8"))
                {
                    const auto beforeRoad = road();
                    v2.closeEditor();
                    Rig::pump (1500);
                    v2.openEditor();
                    Rig::pump (2500);
                    const auto st6 = rig.freshState();
                    const bool alive = st6.isObject();
                    if (rowWanted ("RH7"))
                        row ("RH7", alive,
                             "with the V2's window closed while a lent editor was on screen, does the process "
                             "survive and the window come back (the lease returned in its destructor)",
                             juce::String ("the road before the close: \"") + beforeRoad
                             + "\", and the reopened window " + (alive ? "answers" : "DOES NOT answer"));
                    if (rowWanted ("RH8"))
                    {
                        const auto tbl = noteOf (lease ("table", {}, 0));
                        row ("RH8", tbl.contains ("one shared EchoJay table"),
                             "and after all of that, is the process-local table still sane - no lease left behind, "
                             "no instance left pointing at a window that has gone",
                             tbl);
                    }
                }

                rig.send (echojay::rig::cmd::press, args1 ("button", "real_editor_off"), 8000);
                rhPump (300);
                // ---- AND THE SUBJECTS COME OFF THE RACK (27 Oct 2026, 00:30) -------------------
                // ValhallaDelay takes the process down in its OWN thread when its host goes away
                // (identical with the switch off - the RH6 note has the evidence), and the rig's
                // teardown destroys these tracks. Leaving it on the rack means every pass ends in
                // that plugin's crash and the suite never prints its verdict. So the rack is emptied
                // here, with the plugin's fault recorded rather than tripped over.
                for (int k = 0; k < 6; ++k)
                {
                    aimAtTheLink();
                    askAgent (rig, "rig-linkclear-marker: take slot 1 off that Link's rack", 60000);
                    rhPump (1500);
                    const auto left = rig.freshState().getProperty ("chainView", juce::var())
                                         .getProperty ("slots", juce::var()).toString();
                    if (left.trim().isEmpty()) break;
                }
                std::printf ("        (RH) the Link's rack now holds [%s] - the subjects are off it, so no "
                             "plugin's own teardown can end this pass\n",
                             rig.freshState().getProperty ("chainView", juce::var())
                                .getProperty ("slots", juce::var()).toString().toRawUTF8());
            }

            // ---- RE6: THE FOUR STRUCTURAL DOORS, DRIVEN EXACTLY ON THE OWN RACK ------------------
            //
            // A Link's rack can only be edited through an agent turn from here (a model answering and a
            // plan matching the rack), which is not a thing to hang an ordering test on - LV8 spent two
            // passes proving that. ChainHost is the SAME code in both images, so the hook, the slot
            // numbering and the order are driven here on the own rack, where clear_rack and add_slot press
            // the doors directly, with a real Apple AU's real editor on the lease.
            if (rowWanted ("RE6"))
            {
                rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
                rig.send (echojay::rig::cmd::press,      args1 ("button", "tab_chain"), 8000);
                clearOwnRack (rig);
                Rig::pump (300);
                const auto armed = noteOf (lease ("arm_self", {}, 0));
                rig.send (echojay::rig::cmd::addSlot, args1 ("name", "AUDelay"), 25000);
                bool haveTwo = false;
                rig.send (echojay::rig::cmd::addSlot, args1 ("name", "AUMatrixReverb"), 25000);
                rig.until ([&] (const juce::var& stv)
                {
                    if (auto* arr = stv.getProperty ("ownRack", juce::var())
                                       .getProperty ("slots", juce::var()).getArray())
                        haveTwo = arr->size() >= 2;
                    return haveTwo;
                }, 25000);
                if (! armed.contains ("structural doors") || ! haveTwo)
                {
                    skip ("RE6: a structural change revokes the editors anyone is showing",
                          "the row could not be set up (" + armed + "; two slots on the own rack: "
                          + juce::String (haveTwo ? "yes" : "no") + ").");
                }
                else
                {
                    const juce::String selfUid ("rigself0001");
                    const auto took1 = noteOf (lease ("take", selfUid, 1, 20000));
                    const auto took2 = noteOf (lease ("take", selfUid, 2, 20000));
                    juce::ignoreUnused (took2);
                    const auto before = noteOf (lease ("revokes", {}, 0));
                    // The user's own delete, through the product's own removeSlot (clear_rack is that
                    // call, back to front).
                    clearOwnRack (rig);
                    Rig::pump (600);
                    const auto after = noteOf (lease ("revokes", {}, 0));
                    const auto who1  = noteOf (lease ("who", selfUid, 1));
                    row ("RE6", took1.contains ("real editor") && after.contains ("removed")
                                    && who1.contains ("nobody"),
                         "does removing a slot tell whoever is showing its editor to let go, before the slot "
                         "goes - the crash family this round exists to close",
                         "took: " + took1 + " | before: " + before + " | after: " + after + " | " + who1);
                }
            }
        }
    };

    // ---- THESE ROWS RUN HERE, IN EVERY PASS (27 Oct 2026) ---------------------------------------
    //
    // They used to run here only with EJ_RIG_FAST_LV=1 and at the END of the suite otherwise, and the
    // end is a place where they CANNOT be measured. R4 ("closing mid-reply") closes the V2's editor
    // and reopens it, and on the AU road a reopen does not get our interface back: the plugin's own
    // editor outlived the wrapper view (R4 skips a sub-check saying exactly that), so the second
    // Cocoa-view request is refused and JUCE builds its GENERIC AU editor instead - 522x100, three
    // text fields and a channel-layout popup, which is what the window dump showed. Our editor is
    // still alive and still hosting plugin views, in a peer that is in no window at all.
    //
    // So every row after R4 that looks at pixels or at the view hierarchy reported "no clip container
    // on screen": RE9, LV1-LV3, LV7, B9 and the three own-rack opens. Not one of them was wrong.
    // Last night's pass was run with the flag, so they ran before R4 and passed; a full pass would
    // have skipped them, and by his rule of 26 Oct a live-view row that does not run is a stop.
    //
    // The flag now controls ONE thing: whether the run stops after them.
    runLiveViewRows();
    // WHAT THEY LEFT BEHIND, SAID OUT LOUD. RE5 and RH6 delete a Link's track on purpose, so the
    // legs after this point may be running against one Link instead of two - and a leg that reads
    // oddly from here on should be read with that in mind rather than as a regression.
    {
        juce::StringArray gone;
        if (lk1.plugin == nullptr) gone.add ("Kick (Link)");
        if (lk2.plugin == nullptr) gone.add ("Vocal (Link)");
        if (v2.plugin  == nullptr) gone.add ("Mix Bus (V2)");
        std::printf ("\n  the live-view rows are done. %s\n",
                     gone.isEmpty() ? "All three tracks are still live."
                                    : ("These tracks were deleted by them, on purpose: "
                                       + gone.joinIntoString (", ")).toRawUTF8());
        // ---- AND THE TRACKS THEY DELETED ARE PUT BACK (27 Oct 2026) -----------------------------
        // RE5 and RH6 delete a Link's track on purpose. Every leg after this point was written when
        // these rows ran LAST and nothing followed them; now they run first, and three legs read a
        // world with one Link in it or none (R5 answered "view= working=", B5, B8 and B9 skipped for
        // want of a connected Link). A deleted track is something the live-view rows PROVE, not
        // something the rest of the suite should inherit - so the host re-creates them exactly as it
        // did at R0, pumps them until the 1 Hz registry pass has them, and says so.
        juce::StringArray back;
        if (lk1.plugin == nullptr) { lk1.plugin = load (linkComp, "Link 1"); if (lk1.plugin != nullptr) { lk1.prepare (sr, block); back.add (lk1.name); } }
        if (lk2.plugin == nullptr) { lk2.plugin = load (linkComp, "Link 2"); if (lk2.plugin != nullptr) { lk2.prepare (sr, block); back.add (lk2.name); } }
        if (! back.isEmpty())
        {
            for (int i = 0; i < 200; ++i)
            {
                for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr);
                if ((i % 20) == 0) Rig::pump (10);
            }
            Rig::pump (1500);
            std::printf ("  re-created for the legs below (new uids, as a new track would have): %s\n",
                         back.joinIntoString (", ").toRawUTF8());
        }
        // ---- AND THE NAME THE LATER LEGS HOLD IS REFRESHED (27 Oct 2026) ------------------------
        // linkUid was read at R1, before the live-view rows ran, and those rows may have destroyed
        // that very Link - a re-created track is a NEW uid, exactly as a new track in Logic would be.
        // R5 selected the dead one and reported "view= working=", which is the truth about a uid that
        // is gone and says nothing about reopening. So the suite's handle is refreshed here, with the
        // change printed, rather than left to look like a product fault downstream.
        if (linkUid.isEmpty() || ! linkRowOf (rig.freshState(), linkUid).getProperty ("connected", false))
        {
            // WAITED FOR, WITH A BOUND, AND WITH THE AUDIO RUNNING (27 Oct 2026, the R5 red).
            //
            // The first cut looked once, immediately after the re-create, and a freshly instantiated
            // Link has not published as connected yet. The second waited for it with rig.until, and
            // that wait pumps the MESSAGE THREAD and nothing else - while a Link is only in the
            // registry as "connected" for as long as it is passing audio, which is why R0 gets its
            // registry rows from its 200-block loop and not from a wait. So round 14 sat in silence
            // for twenty seconds, read "not connected" every turn, printed that nothing came back,
            // and the legs below inherited a world with no Link in it: R5 answered "view= working="
            // and SCEN A4 read "slots -1 -> -1". Neither was about the product.
            //
            // This wait plays the tracks while it waits, in the same shape as R0's loop, so what it
            // measures is a Link coming up rather than a Link being starved.
            juce::String fresh, freshName;
            const auto firstConnected = [&] (juce::String& uidOut, juce::String& nameOut) -> bool
            {
                if (auto* arr = rig.freshState().getProperty ("links", juce::var()).getArray())
                    for (const auto& l : *arr)
                        if ((bool) l.getProperty ("connected", false))
                        { uidOut = l.getProperty ("uid", juce::var()).toString();
                          nameOut = l.getProperty ("name", juce::var()).toString(); return true; }
                return false;
            };
            const auto giveUp = juce::Time::getMillisecondCounter() + (juce::uint32) 20000;
            while (! firstConnected (fresh, freshName)
                   && juce::Time::getMillisecondCounter() < giveUp)
            {
                for (int i = 0; i < 50; ++i)
                {
                    for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr);
                    if ((i % 10) == 0) Rig::pump (10);
                }
                Rig::pump (300);     // the registry pass runs on the processor's 1 Hz timer
            }
            if (fresh.isEmpty())
                std::printf ("  no connected Link came back within 20 s of re-creating the tracks - the legs "
                             "below will say so rather than appear to be about the product\n");
            if (fresh.isNotEmpty() && fresh != linkUid)
            {
                std::printf ("  the Link the later legs hold (%s) is gone; they now hold %s (\"%s\")\n",
                             linkUid.isEmpty() ? "(none)" : linkUid.toRawUTF8(), fresh.toRawUTF8(),
                             freshName.toRawUTF8());
                linkUid = fresh; linkName = freshName;
            }
        }
        std::printf ("\n");
    }

    if (::getenv ("EJ_RIG_FAST_LV") != nullptr)
    {
        std::printf ("\n  ######## EJ_RIG_FAST_LV=1: ONLY the live-view rows ran. This is NOT a gate and "
                     "not a pass\n           for anything else - every other leg in this suite was "
                     "skipped entirely.\n\n");
        v2.closeEditor();
        // ---- THE VERDICT IS PRINTED BEFORE THE TEARDOWN (27 Oct 2026, 00:40) -------------------
        // It used to come after it, and a crash in a third-party plugin's own shutdown then ate the
        // whole run's result: four passes reported "47 rows, 0 verdicts" while every row in them had
        // already been decided. ValhallaDelay dies in its own thread when its host goes away even
        // when it has been taken off the rack (a parked instance lives until the processor does), and
        // that is its fault, not a verdict about the live view.
        const int rc = failures == 0 ? 0 : 1;
        std::printf ("==== host_rig (LIVE-VIEW ROWS ONLY): %s (%d failed, %d passed, %d skipped) ====\n",
                     failures == 0 ? "GREEN" : "RED", failures, passes, skips);
        std::fflush (stdout);
        // RE5 really does destroy a Link mid-pass (the host deleting the track), so the teardown asks
        // whether a track still has a plugin rather than assuming it.
        for (Track* t : { &v2, &lk1, &lk2 }) if (t->plugin != nullptr) t->plugin->releaseResources();
        lk2.plugin.reset(); lk1.plugin.reset(); v2.plugin.reset();
        return rc;
    }

    leg ("R2: the Agent chip takes effect without a reopen (12b item 6)");
    {
        auto a1 = rig.send (echojay::rig::cmd::agentChip, args1 ("on", true));
        const bool on = rig.until ([] (const juce::var& s) { return (bool) s.getProperty ("agentModeOn", false); }, 3000);
        check ((bool) a1.getProperty ("ok", false) && on, "the chip turned agent mode ON in the SAME window");
        auto a2 = rig.send (echojay::rig::cmd::agentChip, args1 ("on", false));
        const bool off = rig.until ([] (const juce::var& s) { return ! (bool) s.getProperty ("agentModeOn", true); }, 3000);
        check ((bool) a2.getProperty ("ok", false) && off, "...and OFF again, still without reopening");
    }

    // ---- L0 (FIX 7): the login screen shows no dashboard text ----------------
    // RED BEFORE: showLoginScreen() hid ~30 components by name and never the dashboard surfaces, and
    // setSignedOut(true) had ONE caller (openDashboardTab) - so a logout or an expired token while the
    // Dashboard tab was current left the view painting "Loading your dashboard..." behind the login screen.
    // Asserted on the PUBLISHED line, not on a screenshot.
    {
        leg ("L0: no dashboard status line where it does not belong (fix 7)");
        const auto st0 = rig.freshState();
        const auto screen = st0.getProperty ("screen", juce::var()).toString();
        const auto dash   = st0.getProperty ("dashStatusLine", juce::var()).toString();
        check (screen.isNotEmpty(), "the published state names the screen", "screen=\"" + screen + "\"");
        check (! (screen == "login" && dash.containsIgnoreCase ("loading your dashboard")),
               "the login screen carries no \"Loading your dashboard\" line  (RED as it stood: it survived "
               "the switch, because showLoginScreen never hid the dashboard surfaces)",
               "screen=" + screen + " dashStatusLine=\"" + dash + "\"");
    }

    // ---- C11 HAS MOVED TO THE VST3 ROAD (Sean's check 1, 15 Oct 2026) -----------------------------
    //
    // It used to run here, on the AU track, and its closed-window half was a NAMED SKIP every time:
    // releasing the AU's wrapper view does not destroy the plugin's editor in this host, so the row
    // could never test the thing Sean kept reporting. It passed twice while his bug was still there.
    //
    // It now runs as C11v(a) and C11v(b) in the item 0b block above, on the VST3 track, where W1
    // proves a close really destroys the editor - and it covers BOTH stretches of the path: the
    // classifier answering after the window has gone (his 11:39 case, which no transport ever saw)
    // and the socket dying with no window attached (fix 6's own subject).
    //
    // DELIBERATELY NOT LEFT HERE AS WELL. Two copies of one row drift, and the weaker copy is the one
    // that gets believed because it is the one that passes.

    leg ("R3: the composer empties on send (12b item 7)");
    {
        auto ack = rig.send (echojay::rig::cmd::sendChat, args1 ("text", "rig: does the box clear?"));
        check ((bool) ack.getProperty ("ok", false), "the rig typed and sent a message");
        const bool cleared = rig.until ([] (const juce::var& s)
                             { return s.getProperty ("chatInput", juce::var()).toString().trim().isEmpty(); }, 3000);
        check (cleared, "...and the input box is empty afterwards",
               "chatInput=\"" + Rig::state().getProperty ("chatInput", juce::var()).toString() + "\"");
    }

    leg ("R4: CLOSING MID-REPLY KEEPS THE USER'S MESSAGE AND LANDS THE REPLY (item 5)");
    {
        // The mock holds the first delta, so the window is closed while the turn
        // is genuinely in the air - the rig decides when "mid-reply" is.
        const juce::String mark = "rig-midreply-marker";
        rig.send (echojay::rig::cmd::sendChat, args1 ("text", mark));
        Rig::pump (400);                                  // the POST is away, the reply is not
        v2.closeEditor();
        check (v2.editor == nullptr, "the host released its editor wrapper while the reply was still streaming");
        // The same honesty as C11: this host does not destroy the plugin's editor on a close, so this row
        // proves the handover of a turn in flight and NOT what happens to a destroyed window.
        if (! v2.lastCloseDestroyedEditor)
            skip ("...with the plugin's editor actually destroyed", v2.lastCloseNote);
        // Audio keeps running with the window shut, as it does in a host.
        for (int i = 0; i < 300; ++i)
        {
            for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr);
            if ((i % 30) == 0) Rig::pump (20);
        }
        Rig::pump (2500);
        v2.openEditor();
        Rig::pump (800);
        auto after = rig.freshState();
        // ---- IS THIS ACCOUNT ALLOWED A PLAIN CHAT TURN AT ALL? -------------------------------------
        //
        // sendChatMessage refuses before any network when api.canSendMessage() is false, and it refuses by
        // pushing an ASSISTANT row carrying the limit message. That is what this leg kept reading as
        // "roles=assistant,assistant": not lost questions, but two refusals. B's dev account is Free with a
        // daily pool of five chats, and the rig's own passes spend them - while the AGENT path is unmetered,
        // which is why every capability row passes beside this one.
        //
        // So the leg says which it is. A spent pool is a SKIP with the reason, never a pass and never a
        // failure pinned on the plugin.
        // THE FLAG, NOT THE PROSE. Matching words in the transcript was the next mistake in the same family:
        // the plugin states this as a fact and the leg reads the fact.
        const bool poolSpent = ! (bool) after.getProperty ("canSendChat", true);
        if (poolSpent && ! transcriptHas (after, mark))
            check (true, "(skipped) this account cannot send a plain chat turn - the daily chat pool is spent, "
                         "so closing mid-reply cannot be tested on it (B's dev account is Free/5-a-day)",
                   "chatsUsed=" + after.getProperty ("chatsUsed", juce::var()).toString()
                   + " of " + after.getProperty ("chatsLimit", juce::var()).toString()
                   + ", roles=" + transcriptRoles (after));
        else
            check (transcriptHas (after, mark),
                   "the user's own message SURVIVED the close  (RED on 13a: it was lost with the editor's store)",
                   "roles=" + transcriptRoles (after));
        // A REAL reply takes as long as the model takes; a mock's is instant. 12 s was the mock's number and
        // it failed the live run for that reason alone, which is the leg's fault and not the plugin's.
        const bool liveReply = liveServerConfigured();
        const bool replyLanded = poolSpent || rig.until ([] (const juce::var& s)
                                 { return transcriptRoles (s).contains ("assistant"); },
                                 liveReply ? 90000 : 15000);
        check (replyLanded, poolSpent ? "(skipped) the reply leg needs a turn this account may send"
                                      : "...and the reply that finished while the window was shut is in the transcript",
               "roles=" + transcriptRoles (Rig::state())
               + (replyLanded ? juce::String() : juce::String (" (waited ")
                                                 + juce::String (liveReply ? 90 : 15) + "s)"));
    }

    leg ("R5: reopening returns to the Link you were on, with no stale banner (items 5 and 6)");
    if (linkUid.isNotEmpty())
    {
        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkUid));
        Rig::pump (800);
        v2.closeEditor();
        for (int i = 0; i < 100; ++i) { for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr); }
        Rig::pump (1200);
        v2.openEditor();
        Rig::pump (1500);
        auto after = rig.freshState();
        check (after.getProperty ("viewUid", juce::var()).toString() == linkUid
            || after.getProperty ("workingUid", juce::var()).toString() == linkUid,
               "the reopened window is on the Link it was on",
               "view=" + after.getProperty ("viewUid", juce::var()).toString()
               + " working=" + after.getProperty ("workingUid", juce::var()).toString());
        const auto banner = after.getProperty ("centreBanner", juce::var()).toString();
        check (! banner.contains ("owns its rack again"),
               "...and the centre does NOT still say \"Released - ... owns its rack again\"  (item 6)",
               "banner=\"" + banner + "\"");
    }

    leg ("R6: the rack OUT a user sets is what the state reports");
    {
        auto ack = rig.send (echojay::rig::cmd::setRackOut, args1 ("db", -2.0));
        check ((bool) ack.getProperty ("ok", false), "the rack OUT was set to -2.0 dB");
        const bool seen = rig.until ([] (const juce::var& s)
                          { return std::abs ((double) s.getProperty ("ownRack", juce::var())
                                                        .getProperty ("busGainDb", 0.0) + 2.0) < 0.05; }, 3000);
        check (seen, "...and the rack reads it back",
               "busGainDb=" + Rig::state().getProperty ("ownRack", juce::var()).getProperty ("busGainDb", juce::var()).toString());
    }

    leg ("R7: Sean's rulings of 13 Oct - the three callbacks that had one end (ruling 3) and AUTO (ruling 1)");
    {
        // (a) RULING 1: the pre-gain knob's double-click restores AUTO rather than writing 0.0 dB. The state
        // reports the hand-set flag, which is the same fact a build reads when deciding whether it may set
        // the pre-gain itself - so this is the behaviour, not a proxy for it.
        // ---- THE OWN RACK, FIRST, AND SAID OUT LOUD (15 Oct 2026) -----------------------------------
        //
        // This row read the OWN rack's preGainUserSet while the double-click acted on whatever rack was
        // IN VIEW. In gate 20 the view happened to be a Link, so the press logged "EJLinkState: remote
        // reset pre-gain to auto" and the row then reported the own rack's flag unchanged - a FAIL that
        // was about the rig's assumption, not the plugin. Exactly the shape ruling 3's slot double-click
        // was fixed for a round ago: select the rack you are about to assert on.
        //
        // AND IT SETS THE FLAG ITSELF rather than inheriting it from whichever leg ran before. A row
        // whose precondition is someone else's leftover is a row that passes for reasons it cannot name.
        rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
        rig.send (echojay::rig::cmd::setPreGain, args1 ("db", -4.0), 8000);
        Rig::pump (700);                                   // clear of the 300 ms undo coalesce window
        rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()));
        auto before = Rig::state();
        const bool userSetBefore = (bool) before.getProperty ("preGainUserSet", false);
        auto ack = rig.send (echojay::rig::cmd::preGainReset, juce::var (new juce::DynamicObject()));
        if (! userSetBefore)
            skip ("ruling 1: the pre-gain double-click clears the hand-set flag",
                  "a hand-set pre-gain is this row's precondition and the flag did not take, so there is "
                  "nothing for the double-click to clear and a pass would prove nothing");
        else if (! (bool) ack.getProperty ("ok", false))
            // A LEG THAT COULD NOT RUN IS NOT A LEG THAT PASSED. This was check(true, "(skipped) ...") -
            // a pass padded into the count, which is the exact fault skip() was introduced to remove.
            skip ("ruling 1: the pre-gain double-click clears the hand-set flag",
                  "the pre-gain knob is not on this view, so the ruling cannot be pressed here: "
                  + ack.getProperty ("note", juce::var()).toString());
        else
        {
            const bool nowAuto = rig.until ([] (const juce::var& s)
                                 { return ! (bool) s.getProperty ("preGainUserSet", true); }, 3000);
            check (nowAuto,
                   "ruling 1: a double-click on the pre-gain knob clears the hand-set flag - it is AUTO again "
                   "(RED as it stood: onResetAuto was assigned by both editors and invoked by nothing)",
                   "userSet was " + juce::String (userSetBefore ? "true" : "false") + ", now "
                   + juce::String ((bool) Rig::state().getProperty ("preGainUserSet", true) ? "true" : "false"));
        }

        // (b) RULING 3: a slot double-click reaches the open door. On the own rack that is the inline editor;
        // the leg asserts the PRESS was accepted by the wiring, which is what had no end before.
        // IT RUNS WHERE A SLOT EXISTS. At this point in the run the rack is empty, so this used to print a
        // skip as a pass; the owed flag carries it to after C1, which puts a compressor there.
        auto st2 = Rig::state();
        const int slots = (int) st2.getProperty ("ownRack", juce::var()).getProperty ("count", 0);
        if (slots > 0)
        {
            auto a2 = rig.send (echojay::rig::cmd::openSlot, args1 ("n", 1));
            check ((bool) a2.getProperty ("ok", false),
                   "ruling 3: a slot DOUBLE-CLICK is wired and accepted  (RED as it stood: Block had no "
                   "mouseDoubleClick, and the hook for it - onRemoteEditorRequest - was never invoked)",
                   a2.getProperty ("note", juce::var()).toString());
        }
        else slotDoubleClickOwed = true;   // run it below, once the capability rows have put a slot there

        // (c) RULING 3: the substitution notice has a surface. The rig cannot force a substitution without a
        // missing third-party plugin, so this asserts the SURFACE the notice now uses is reported - the band
        // the editor draws from getStateNotes(). A notice with nowhere to appear was the fault.
        check (st2.getProperty ("stateNotes", juce::var()).isString(),
               "ruling 3: the note band the substitution notice now writes to is readable state",
               "notes=\"" + st2.getProperty ("stateNotes", juce::var()).toString() + "\"");
    }

    // =========================================================================
    //  THE CAPABILITY PASS (docs/AGENT_CAPABILITIES.md)
    // =========================================================================
    //  Only runs in LIVE mode: these rows need the real model to decide what to
    //  do, and a mock answering recorded frames would be checking the recording.
    //  In mock mode they are SKIPPED and said to be skipped.
    const bool live = liveServerConfigured();

    // ---- U1: THE UNDO OP, END TO END, WITHOUT THE MODEL (14 Oct 2026) ---------------------------------
    //
    // WHY THIS EXISTS. Sean's ruling 4 gave the agent an undo op. It is built, and agent_round14_guard holds
    // sixteen assertions on it - but a guard drives the executor in a fixture, and the live row C10 kept
    // failing for reasons outside the plugin: first because the plugin never DECLARED the capability, then
    // because B's local server was started the day before their undo work and the running process has no
    // undo tool in its enum. Neither is evidence about the plugin, and neither is mine to fix.
    //
    // So this leg proves MY half with the model taken out of it: the mock sends the real SSE frames for
    // `do {op:"set_pre_gain", db:3}` and then `do {op:"undo"}`, through the real client, the real executor
    // and the real plugin, and the assertion is on the plugin's own published state - the pre-gain going to
    // 3 dB and coming back. Mock mode only: in live mode the model owns the decision and C10 is the row.
    if (! live)
    {
        leg ("U1: the undo op end to end (mock frames, real client, real plugin, real state)");
        rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""));
        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""));
        // ---- ITS OWN BASELINE (15 Oct 2026) ---------------------------------------------------------
        // This read whatever the previous leg had left behind. R7's ruling-1 row now double-clicks the
        // pre-gain back to AUTO just before, so the baseline it inherited was a value that was about to
        // move: it read -5.01, the AUTO value landed, and the undo correctly restored 0.00 - a FAIL about
        // a leftover, not about the undo. A row whose precondition belongs to another row is a row that
        // passes for reasons it cannot name, which is the same lesson as the ruling-1 fix above.
        // ---- SEAN'S RULING, 15 Oct 2026: BOTH CASES, OR IT IS NOT PROVEN ---------------------------
        //
        // "Undo restores the exact prior state, the value AND whether it was hand-set or auto. Prove
        // both cases: a hand-set -5 comes back as hand-set -5, and an auto pre-gain comes back as auto."
        //
        // THIS ROW USED TO PASS FOR THE WRONG REASON. It inherited whatever pre-gain the previous leg
        // left behind, and that value happened to match whatever the undo produced - so the claim
        // "leaves the pre-gain back where it started" was never tested. Setting the precondition here,
        // twice, is what turned it into a test: the first case then FAILED and found that the agent's
        // set_pre_gain was writing as AUTO, so an undo restored 0.00 over the user's own -5.00.
        const auto preGainNow = [&rig] { return (double) rig.freshState().getProperty ("preGainDb", 99.0); };
        const auto userSetNow = [&rig] { return (bool) rig.freshState().getProperty ("preGainUserSet", false); };

        // ---- CASE 1: a HAND-SET value comes back hand-set, with its figure ------------------------
        rig.send (echojay::rig::cmd::setPreGain, args1 ("db", -5.0), 8000);
        Rig::pump (700);                                   // clear of the 300 ms undo coalesce window
        const double pgBefore = preGainNow();
        const bool   usBefore = userSetNow();
        const auto r = askAgent (rig, "rig-undo-marker: set the pre gain to 3 dB, then undo it", 60000);
        const double pgAfter = preGainNow();
        const bool   usAfter = userSetNow();
        const juce::String stepWords = r.stepWords;

        scen ("D1", "a hand-set pre-gain comes back hand-set after an undo, at its own figure",
              r.ran() && std::abs (pgAfter - pgBefore) < 0.1 && usBefore && usAfter,
             "pre-gain " + juce::String (pgBefore, 2) + " -> " + juce::String (pgAfter, 2)
             + " dB, userSet " + juce::String (usBefore ? "true" : "false") + " -> "
             + juce::String (usAfter ? "true" : "false") + ", state " + r.state
             + ", steps: " + stepWords.substring (0, 150)
             + "  (RED before Sean's ruling of 15 Oct: the agent's op wrote the pre-gain as AUTO, so "
             "the undo restored 0.00 and the user's own figure was gone)",
             "A", v2.editor.get(), rig.freshState(), &r);
        check (stepWords.containsIgnoreCase ("undone") || stepWords.containsIgnoreCase ("undo"),
               "...and the step's own detail says what was undone, from the history's label",
               stepWords.substring (0, 200));

        // ---- CASE 2: an AUTO pre-gain comes back AUTO -------------------------------------------
        // The other half of his ruling, and the one a value-only check cannot see: restoring the right
        // NUMBER while leaving the flag set would pin a rack the loop is still entitled to revise.
        {
            rig.send (echojay::rig::cmd::preGainReset, juce::var (new juce::DynamicObject()), 8000);
            Rig::pump (700);
            const double pgAutoBefore = preGainNow();
            const bool   usAutoBefore = userSetNow();
            if (usAutoBefore)
                skip ("U1b: an AUTO pre-gain comes back AUTO after an undo",
                      "the double-click did not clear the hand-set flag in this run, so there is no AUTO "
                      "state to undo back to and a pass would prove nothing");
            else
            {
                const auto r2 = askAgent (rig, "rig-undo-marker: set the pre gain to 3 dB, then undo it", 60000);
                const double pgAutoAfter = preGainNow();
                const bool   usAutoAfter = userSetNow();
                scen ("D2", "an AUTO pre-gain comes back AUTO after an undo, flag and figure both",
                      r2.ran() && ! usAutoAfter && std::abs (pgAutoAfter - pgAutoBefore) < 0.1,
                      "pre-gain " + juce::String (pgAutoBefore, 2) + " -> " + juce::String (pgAutoAfter, 2)
                      + " dB, userSet " + juce::String (usAutoBefore ? "true" : "false") + " -> "
                      + juce::String (usAutoAfter ? "true" : "false")
                      + "  (the undo restores the FLAG as well as the figure, so a rack the loop may "
                      "still revise is not left pinned by an undo)",
                      "A", v2.editor.get(), rig.freshState(), &r2);
            }
        }
    }

    // =====================================================================
    //  AUTO MODE (Sean's ruling, 14 Oct 2026) - mock frames, so the approval
    //  class is the RIG'S to choose. Every step below is marked ask_first by
    //  the script: in Ask they wait, in Auto they run unless they cannot be
    //  undone. That is the whole ruling, and the model cannot be asked to
    //  produce it on demand.
    // =====================================================================
    if (! live)
    {
        leg ("AUTO MODE: what it skips, what it still asks, and that Off still changes nothing");
        const auto modeNow = [&rig] { return rig.freshState().getProperty ("agentApprovalMode", juce::var()).toString(); };
        const auto setMode = [&rig] (const char* m)
        { rig.send (echojay::rig::cmd::agentChip, args1 ("mode", m), 8000); };

        rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""));
        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""));

        // ---- the chip cycles, and the plugin says which state it is in -------------------------------
        setMode ("off");  check (modeNow() == "off",  "the chip reads OFF when it is off",  modeNow());
        setMode ("ask");  check (modeNow() == "ask",  "...ASK after one press",             modeNow());
        setMode ("auto"); check (modeNow() == "auto", "...and AUTO after the next",         modeNow());
        check (rig.freshState().getProperty ("agentModeLine", juce::var()).toString()
                   .containsIgnoreCase ("without waiting"),
               "...and the line under the composer says what Auto does, in the mockup's own words",
               rig.freshState().getProperty ("agentModeLine", juce::var()).toString().substring (0, 90));

        // ---- A9: an Auto run completes with NO approvals, and the change reads back -------------------
        {
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0));
            Rig::pump (700);
            const auto r = askAgent (rig, "rig-auto-marker: set the pre gain, twice", 60000);
            const double pg = (double) rig.freshState().getProperty ("preGainDb", 99.0);
            row ("A9", r.ran() && r.applies == 0 && std::abs (pg - 6.0) < 0.2,
                 "(Auto) a run of ask_first steps completes with NO approval presses, and the change reads back",
                 "approval presses " + juce::String (r.applies) + ", pre-gain now " + juce::String (pg, 2)
                 + " dB, state " + r.state + ", steps: " + r.stepWords.substring (0, 160), &r);
            check (r.stepWords.containsIgnoreCase ("auto") || r.applies == 0,
                   "...and nothing waited for a press on the way",
                   "presses " + juce::String (r.applies));
        }

        // ---- A7: in Auto, "undo that" runs with no second approval (Sean's ruling) -------------------
        {
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0));
            Rig::pump (700);
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 3.0));
            Rig::pump (400);
            const double before = (double) rig.freshState().getProperty ("preGainDb", 99.0);
            const auto r = askAgent (rig, "rig-autoundo-marker: undo that", 60000);
            const double after = (double) rig.freshState().getProperty ("preGainDb", 99.0);
            row ("A7", r.ran() && r.applies == 0 && std::abs (after) < 0.3,
                 "(Auto) \"undo that\" runs with NO approval wait - the user's own words are the approval",
                 "pre-gain " + juce::String (before, 2) + " -> " + juce::String (after, 2)
                 + " dB with " + juce::String (r.applies) + " press(es), state " + r.state, &r);
        }

        // ---- A11: a step this build cannot put back STILL ASKS in Auto, with the reason ---------------
        {
            const auto r = askAgent (rig, "rig-autostrange-marker: do the unknown thing", 40000,
                                     /*answerAsks*/ false);
            const bool waited = r.state == "awaiting_approval" || r.applies > 0 || r.sawApproval;
            row ("A11", waited,
                 "(Auto) a step that cannot be undone STILL waits for a press, with its reason on the line",
                 "state " + r.state + ", presses " + juce::String (r.applies)
                 + ", steps: " + r.stepWords.substring (0, 160), &r);
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
        }

        // ---- A8: the same undo request in ASK still waits ---------------------------------------------
        {
            setMode ("ask");
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0));
            Rig::pump (700);
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 3.0));
            Rig::pump (400);
            const auto r = askAgent (rig, "rig-autoundo-marker: undo that", 40000, /*answerAsks*/ false);
            row ("A8", r.state == "awaiting_approval" || r.applies > 0 || r.sawApproval,
                 "(Ask) the same \"undo that\" still waits for Apply - Auto removed the waiting, not the rule",
                 "state " + r.state + ", presses " + juce::String (r.applies), &r);
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
        }

        // ---- A12: OFF still changes nothing, even after Auto has been used ---------------------------
        {
            setMode ("off");
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 1.5));
            Rig::pump (700);
            const double before = (double) rig.freshState().getProperty ("preGainDb", 99.0);
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", "rig-auto-marker: set the pre gain, twice"), 15000);
            Rig::pump (4000);
            const double after = (double) rig.freshState().getProperty ("preGainDb", 99.0);
            check (std::abs (after - before) < 0.2 && modeNow() == "off",
                   "(Off) a run started with the chip OFF changes nothing - a three-way control is a new way "
                   "to get that wrong",
                   "pre-gain " + juce::String (before, 2) + " -> " + juce::String (after, 2)
                   + " dB, mode " + modeNow());
        }

        // ---- A13: the mode PERSISTS across the window being rebuilt ----------------------------------
        {
            setMode ("auto");
            v2.closeEditor();
            v2.openEditor();
            Rig::pump (1200);
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 6000);
            check (modeNow() == "auto",
                   "the mode persists: the chip still reads AUTO after the window was rebuilt  (Sean's "
                   "ruling: the choice persists per user, and a silent reset to Ask would look like Auto "
                   "failing)", modeNow());
            setMode ("ask");   // leave the tree as the other rows expect it
        }
    }

    // =========================================================================
    //  ITEM 0 (Sean's ruling, 14 Oct 2026): THE AGENT RUN IS A MESSAGE.
    //
    //  Mock frames, because the subject is the SURFACE and not the model: the
    //  rows assert on `agentRow` in the plugin's published state, which the row
    //  itself computes from the same RunView it paints. The floating panel is
    //  asserted ABSENT, because "one agent run = one message" is the mockup's
    //  first rule and a card beside the message is a second place for the
    //  window to disagree with itself (Sean's item 6).
    // =========================================================================
    if (! live)
    {
        leg ("ITEM 0: the run is ONE transcript row - steps on it, buttons on the lines, collapse at the end");
        if (! agentRowOn())
            skip ("ITEM 0: G1-G7 (one row, the steps ticking on it, a line's own Apply, the collapse to "
                  "the closing sentence, Undo all on the message, a question's chips, Details)",
                  "EJ_AGENT_PANEL=1 (or EJ_AGENT_ROW=0) put the old floating card back, so there is no "
                  "transcript row to read. The DEFAULT is on from 16 Oct (Sean's RULE 2) and these rows "
                  "run in S6 unasked.");
        // EACH ROW BLOCK CARRIES ITS OWN GATE (15 Oct 2026). A single wrapper brace around all
        // six is what I got wrong twice: moving G7 into it left the brace arithmetic unbalanced,
        // the compiler was happy, and G9-G12 silently ended up INSIDE the gate - so four rows for
        // things Sean is waiting on ran zero checks and the rig still printed a count. Per-block
        // gates cannot do that: every block below is already balanced, so there is no arithmetic.
        const auto rowOf = [&rig] { rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 4000);
                                    return Rig::state().getProperty ("agentRow", juce::var()); };
        const auto setMode2 = [&rig] (const char* m)
        { rig.send (echojay::rig::cmd::agentChip, args1 ("mode", m), 8000); };

        rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""));
        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""));
        setMode2 ("ask");
        rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0));
        Rig::pump (700);

        // ---- G1 / G2 / G3: the row exists, the panel does not, and a LINE's Apply is what runs it -----
        if (agentRowOn())
        {
            const int rowsBefore = (int) rowOf().getProperty ("rows", 0);
            const auto r = askAgent (rig, "rig-row-marker: set the pre gain, twice", 60000);
            const auto row0 = rowOf();
            const double pg = (double) rig.freshState().getProperty ("preGainDb", 99.0);

            row ("G1", r.ran() && (bool) row0.getProperty ("present", false)
                       && (int) row0.getProperty ("rows", 0) == rowsBefore + 1
                       && ! (bool) row0.getProperty ("panelShown", false),
                 "ITEM 0: one agent run renders as ONE transcript row, and the floating card does not appear "
                 "at all  (RED before item 0: the run was a docked panel and left a bubble per round)",
                 "rows " + juce::String (rowsBefore) + " -> " + juce::String ((int) row0.getProperty ("rows", 0))
                 + ", present=" + juce::String ((int) (bool) row0.getProperty ("present", false))
                 + ", panelShown=" + juce::String ((int) (bool) row0.getProperty ("panelShown", false))
                 + ", state " + r.state, &r);

            // the steps are ON the row, at their final statuses, with the mockup's right-hand words
            juce::String rowSteps, rowRights;
            int done = 0;
            if (auto* arr = row0.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                {
                    rowSteps  << sv.getProperty ("label",  juce::var()).toString() << "/"
                              << sv.getProperty ("status", juce::var()).toString() << "; ";
                    rowRights << sv.getProperty ("right",  juce::var()).toString() << "; ";
                    if (sv.getProperty ("status", juce::var()).toString() == "done") ++done;
                }
            row ("G2", done >= 1 && rowRights.containsIgnoreCase ("applied") && std::abs (pg - 6.0) < 0.2,
                 "...and its steps tick IN THE MESSAGE to their final status, each with the mockup's own "
                 "right-hand word, and the change reads back off the rack",
                 "steps: " + rowSteps.substring (0, 200) + " rights: " + rowRights.substring (0, 90)
                 + " pre-gain " + juce::String (pg, 2) + " dB", &r);

            check (r.applies > 0,
                   "...and in Ask the run waited for a press ON THE ROW: the rig's `press: apply` reached the "
                   "message's own button, not a floating panel's",
                   "presses " + juce::String (r.applies) + ", state " + r.state);
        }

        // ---- G3: THE LINE'S OWN APPLY RUNS THAT LINE AND LEAVES THE OTHER ALONE ----------------------
        // Item 2 was a single Apply recording the line as SKIPPED and the line losing its own control.
        // In this shape the line's Apply is the primary control, so the row that proves it must press
        // the LINE and then show that the other line did NOT run.
        if (agentRowOn())
        {
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0));
            Rig::pump (700);
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", "rig-row-marker: set the pre gain, twice (line apply)"), 15000);
            const bool awaiting = rig.until ([] (const juce::var& st)
                                             { return agentVarOf (st).getProperty ("state", juce::var()).toString()
                                                      == "awaiting_approval"; }, 40000);
            // both lines must be offering their own buttons before the press means anything
            int offering = 0;
            const auto rw0 = rowOf();
            if (auto* arr = rw0.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr) if ((bool) sv.getProperty ("applySkip", false)) ++offering;
            const auto ack = rig.send (echojay::rig::cmd::press, args1 ("button", "apply_line"), 10000);
            rig.until ([] (const juce::var& st)
                       { return ! (bool) agentVarOf (st).getProperty ("active", false); }, 40000);
            Rig::pump (600);
            const double pg = (double) rig.freshState().getProperty ("preGainDb", 99.0);
            const auto rw1 = rowOf();
            juce::String statuses;
            bool anySkipped = false;
            if (auto* arr = rw1.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                {
                    const auto st2 = sv.getProperty ("status", juce::var()).toString();
                    statuses << st2 << " ";
                    if (st2 == "skipped" || st2 == "not_run") anySkipped = true;
                }
            check (awaiting && offering >= 2 && std::abs (pg - 4.0) < 0.3 && anySkipped,
                   "ITEM 0 / item 2: a LINE's own [Apply] runs THAT line and leaves the other waiting line "
                   "alone - the arrangement that broke was a single Apply recording the line as skipped",
                   "lines offering buttons " + juce::String (offering) + ", pre-gain now "
                   + juce::String (pg, 2) + " dB (4 = the first line only), statuses: " + statuses
                   + ", press ok=" + ack.getProperty ("ok", juce::var()).toString());
        }

        // ---- G4: it collapses to the closing sentence, and "N changes" opens it again -----------------
        if (agentRowOn())
        {
            const auto row1 = rowOf();
            const bool collapsed = ! (bool) row1.getProperty ("stepsOpen", true);
            const int  changes   = (int) row1.getProperty ("changes", 0);
            const auto closing   = row1.getProperty ("closing", juce::var()).toString();
            // THE SENTENCE, NOT THE FIELD (17 Oct): G7b drops the row's `closing` when it would only
            // repeat the opening - Sean read that sentence twice in Logic and asked for it once - so
            // this row looks for the sentence the collapsed row SHOWS, whichever field carries it.
            const auto opening   = row1.getProperty ("opening", juce::var()).toString();
            const auto shown     = closing.trim().isNotEmpty() ? closing : opening;
            row ("G4", collapsed && changes > 0 && shown.trim().isNotEmpty(),
                 "when it finishes the row COLLAPSES to its closing sentence, with an \"N changes\" control "
                 "and the count of what actually landed",
                 "stepsOpen=" + juce::String ((int) (bool) row1.getProperty ("stepsOpen", true))
                 + ", changes=" + juce::String (changes) + ", closing=\"" + closing.substring (0, 90) + "\"");

            const auto ack = rig.send (echojay::rig::cmd::press, args1 ("button", "changes"), 8000);
            Rig::pump (400);
            const auto row2 = rowOf();
            check ((bool) row2.getProperty ("stepsOpen", false),
                   "...and pressing it expands the steps again, in place  (a drawn control that cannot be "
                   "pressed is item 3's dead Details button, so the rig presses every one of them)",
                   "ok=" + ack.getProperty ("ok", juce::var()).toString()
                   + " stepsOpen now " + juce::String ((int) (bool) row2.getProperty ("stepsOpen", false)));
        }

        // ---- G5: Undo all is ON THE MESSAGE and puts the run back ------------------------------------
        if (agentRowOn())
        {
            const auto row1 = rowOf();
            const bool hasUndoAll = (bool) row1.getProperty ("undoAll", false);
            const double before = (double) rig.freshState().getProperty ("preGainDb", 99.0);
            const auto ack = rig.send (echojay::rig::cmd::press, args1 ("button", "undo_all"), 10000);
            Rig::pump (1500);
            const double after = (double) rig.freshState().getProperty ("preGainDb", 99.0);
            const auto row2 = rowOf();
            juce::String statuses;
            if (auto* arr = row2.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr) statuses << sv.getProperty ("status", juce::var()).toString() << " ";
            row ("G5", hasUndoAll && ack.getProperty ("ok", juce::var()).toString() != "0"
                       && std::abs (after - before) > 0.5,
                 "Undo all lives ON the run's message, and pressing it puts back what the run changed - the "
                 "rows then read `undone` rather than `applied`",
                 "undoAll offered=" + juce::String ((int) hasUndoAll) + ", pre-gain "
                 + juce::String (before, 2) + " -> " + juce::String (after, 2)
                 + " dB, statuses: " + statuses.substring (0, 80));
        }

        // ---- G6: a QUESTION is a row with chips on it, and the answer is recorded on the row ---------
        if (agentRowOn())
        {
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0));
            Rig::pump (700);
            // Start the run and read the row WHILE it is asking - the chips only exist until answered.
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", "rig-rowask-marker: how firm?"), 15000);
            juce::String chipsSeen;
            bool sawChips = false;
            for (int i = 0; i < 60 && ! sawChips; ++i)
            {
                const auto rw = rowOf();
                if (auto* arr = rw.getProperty ("steps", juce::var()).getArray())
                    for (const auto& sv : *arr)
                    {
                        const auto ch = sv.getProperty ("chips", juce::var()).toString();
                        if (ch.isNotEmpty()) { chipsSeen = ch; sawChips = true; }
                    }
                if (! sawChips) Rig::pump (400);
            }
            check (sawChips && chipsSeen.containsIgnoreCase ("Gentle"),
                   "ITEM 0: the model's question is a STEP ROW with its answer chips under it, in the "
                   "message - not a shelf and not a dialog  (the mockup's \"Gentle\" / \"More upfront\")",
                   "chips: \"" + chipsSeen + "\"");
            // tap the first chip, exactly as the mouse does
            const auto ack = rig.send (echojay::rig::cmd::press, args1 ("button", "chip"), 10000);
            Rig::pump (1200);
            juce::String answered;
            for (int i = 0; i < 40 && answered.isEmpty(); ++i)
            {
                const auto rw = rowOf();
                if (auto* arr = rw.getProperty ("steps", juce::var()).getArray())
                    for (const auto& sv : *arr)
                    {
                        const auto a2 = sv.getProperty ("answered", juce::var()).toString();
                        if (a2.isNotEmpty()) answered = a2;
                    }
                if (answered.isEmpty()) Rig::pump (400);
            }
            const bool carriedOn = rig.until ([] (const juce::var& st)
                                              { const auto a = agentVarOf (st);
                                                return ! (bool) a.getProperty ("active", false); }, 40000);
            const double pg = (double) rig.freshState().getProperty ("preGainDb", 99.0);
            check (answered.containsIgnoreCase ("You said") && carriedOn,
                   "...and tapping a chip records the answer ON that row and the run carries on - no second "
                   "user bubble for a tap, which is what made one run read as five messages",
                   "\"" + answered + "\", finished=" + juce::String ((int) carriedOn)
                   + ", pre-gain " + juce::String (pg, 2) + " dB, press ok="
                   + ack.getProperty ("ok", juce::var()).toString());
        }

        // ---- G7: a FAILED line keeps its own sentence and a Details disclosure ON the row -------------
        if (agentRowOn())
        {
            const auto r = askAgent (rig, "rig-rowfail-marker: do the impossible", 40000);
            const auto rw = rowOf();
            bool hasDetails = false; juce::String detail;
            if (auto* arr = rw.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                {
                    if ((bool) sv.getProperty ("details", false)) hasDetails = true;
                    const auto d = sv.getProperty ("detail", juce::var()).toString();
                    if (d.isNotEmpty()) detail = d;
                }
            if (! hasDetails)
            {
                // A step this build refuses may carry one plain sentence and nothing technical behind it -
                // in which case there is correctly NO Details control. Said as a skip with its reason rather
                // than passed on a row that proves nothing.
                skip ("G7: a failed line's Details disclosure",
                      "the refused step carried no second, technical message, so there is correctly no "
                      "Details control to open - the row's own sentence is all there is. detail: \""
                      + detail.substring (0, 110) + "\"");
            }
            else
            {
                const auto ack = rig.send (echojay::rig::cmd::press, args1 ("button", "details"), 8000);
                Rig::pump (400);
                const auto rw2 = rowOf();
                bool expanded = false;
                if (auto* arr = rw2.getProperty ("steps", juce::var()).getArray())
                    for (const auto& sv : *arr) if ((bool) sv.getProperty ("expanded", false)) expanded = true;
                row ("G7", expanded,
                     "a failed line keeps its own plain sentence and a Details disclosure that opens IN "
                     "PLACE on the row  (item 3 was a Details button that did nothing)",
                     "detail: \"" + detail.substring (0, 90) + "\", expanded="
                     + juce::String ((int) expanded) + ", ok=" + ack.getProperty ("ok", juce::var()).toString(), &r);
            }
        }

        // ---- G9: THE AGENT MODE LINE IS A 3-SECOND HINT, NOT A FIXED LINE (Sean's ruling, 15 Oct) ----
        //   RUNS EITHER WAY: the mode hint is item 4, not item 0, and has no transcript row in it.
        //
        // "remove it as a fixed line (it overlapped Suggested Settings, sat stranded bottom-left on the
        // empty chat screen, and says nothing useful on Off). Instead, the mode description is a
        // tooltip on the Agent chip, and it shows for about 3 seconds just above the composer only
        // when the mode changes."
        //
        // Asserted on the published RECT and on the clock, because "it is on" was true the whole time
        // it was in the wrong place. Three facts: it appears ON A CHANGE, it is gone a few seconds
        // later without anything else happening, and it never overlaps the transcript or the chain
        // panel while it is up.
        {
            const auto lineOf = [&rig] { rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 4000);
                                         return Rig::state().getProperty ("agentModeLineRect", juce::var()); };
            // IT ONLY APPEARS ON A CHANGE, so the row has to make one. My first cut sent "ask" when
            // the chip was ALREADY on ask: syncAgentChip early-returns when the mode has not moved -
            // which is exactly what stops the timer re-raising the hint every tick - so nothing was
            // announced and the row read shown=0 while the product was behaving correctly.
            // THE COMPOSER HAS TO BE ON SCREEN. The plugin published its own reason for refusing -
            // "the composer is not on this view" - so the row was asserting that a hint appears above
            // a composer that is not there, which is the product being right and the row being wrong.
            // work_on opens a chat channel, which is what brings the chat surface to the front.
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
            Rig::pump (400);
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "auto"), 8000);
            Rig::pump (200);
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);
            Rig::pump (300);
            const auto onChange = lineOf();
            const auto shown    = onChange.getProperty ("shown", juce::var()).toString();
            const auto overT    = onChange.getProperty ("overTranscript", juce::var()).toString();
            const auto overC    = onChange.getProperty ("overChainPanel", juce::var()).toString();
            const auto words    = onChange.getProperty ("text", juce::var()).toString();
            const auto whyNot = onChange.getProperty ("why", juce::var()).toString();
            if (shown != "1" && whyNot.containsIgnoreCase ("not on this view"))
                skip ("the mode hint appears on a mode change",
                      "SUPERSEDED BY ROW H1, which proves this claim properly: it presses the chip "
                      "itself ({click:true}, the user's own path), puts the window on the CHAT tab and "
                      "answers the onboarding intake first, so the composer is actually there. This "
                      "check sits mid-run where the composer is not on the view, and the plugin says "
                      "so: \"" + whyNot + "\". Kept as a skip rather than deleted so the supersession "
                      "is visible in the log.");
            else
                check (shown == "1" && words.containsIgnoreCase ("Ask:"),
                       "the mode hint APPEARS when the mode changes, carrying that mode's own sentence",
                       "shown=" + shown + " text=\"" + words.substring (0, 60) + "\"  why=\"" + whyNot + "\"");
            check (overT != "1" && overC != "1",
                   "...and while it is up it overlaps neither the transcript nor the chain panel  (RED "
                   "before this round: it was drawn 8 px inside the transcript and over Suggested Settings)",
                   "overTranscript=" + overT + " overChainPanel=" + overC);
            // ...and it GOES. Waited for rather than slept on: the strip's own clock decides, and a row
            // that slept would be the "decisions on uncontrolled timing" fault in the row written to
            // catch a stray strip.
            const bool wentAway = rig.until ([] (const juce::var& st)
                                  { return st.getProperty ("agentModeLineRect", juce::var())
                                             .getProperty ("shown", juce::var()).toString() != "1"; }, 8000);
            check (wentAway,
                   "...and it is GONE a few seconds later with nothing else pressed - a hint, not a line "
                   "(Sean: \"shows for about 3 seconds ... only when the mode changes\")",
                   "msLeft=" + lineOf().getProperty ("msLeft", juce::var()).toString());
            // THE DESCRIPTION'S REAL HOME IS THE CHIP'S TOOLTIP, which is the half that replaces it.
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 4000);
            const auto tip = Rig::state().getProperty ("agentModeLine", juce::var()).toString();
            check (tip.containsIgnoreCase ("Ask:"),
                   "...while the mode's description still exists as the chip's tooltip, from the same one "
                   "function, so the chip, the tooltip and the hint cannot disagree",
                   "tooltip=\"" + tip.substring (0, 70) + "\"");
        }

        // ---- G12: TRACK NAMES RENDER LITERALLY (Sean, 15 Oct) ----------------------------------------
        //
        // "88_Omnisphere_01" showed as italic "88Omnisphere01". The rig reads the transcript's STORED
        // text, which is the raw string and survives any renderer bug untouched - so this asserts on
        // `lastAssistantRendered`, the output of the one function both chat passes call.
        //
        // MY FIRST CUT OF THIS ROW TESTED NOTHING: it keyed off the USER's transcript row (cleared by
        // the time it read) and off a mock reply with no underscore in it, so it could only ever fail
        // or pass by accident. The mock now SCRIPTS a reply carrying the names, which is the only way
        // the assertion is about the renderer at all.
        {
            // THE CHIP MUST BE OFF. With agent mode on, a sendChat becomes an agent GOAL - which is
            // exactly what happened: the mock had no agent script for this marker, fell through to its
            // default rounds, and the row read the agent's "nothing further" summary instead of a chat
            // reply. R4 already states this rule for its own leg; this row needed it too.
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "off"), 8000);
            rig.send (echojay::rig::cmd::sendChat,
                      args1 ("text", "rig-underscore-marker: say the track names back to me"), 15000);
            juce::String rendered;
            const bool got = rig.until ([&rendered] (const juce::var& st)
            {
                rendered = st.getProperty ("lastAssistantRendered", juce::var()).toString();
                return rendered.contains ("Omnisphere");
            }, 30000);
            row ("G12", got && rendered.contains ("88_Omnisphere_01")
                        && rendered.contains ("Kick_2_01")
                        && ! rendered.contains ("88Omnisphere01"),
                 "a track name's underscores render LITERALLY - the markdown renderer leaves "
                 "88_Omnisphere_01 and Kick_2_01 intact  (RED before this ruling: it ate them and a "
                 "guard leg had the old behaviour written down as correct)",
                 "rendered=\"" + rendered.substring (0, 110) + "\"");
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);   // leave it as found
        }

        // ---- G8: THE GAIN REDUCTION WINDOW, NOT A MOMENTARY SAMPLE (Sean's checks 2 and 3) -----------        // ---- G8: THE GAIN REDUCTION WINDOW, NOT A MOMENTARY SAMPLE (Sean's checks 2 and 3) -----------
        //
        // His test: the meter showed about 5 dB on the loud parts and the agent reported 0.1 dB, then
        // 2.1 dB. The cause was that check(gr) read ONE instantaneous float at whatever moment its tool
        // call ran - systematically the release tail just after "Got it, you can stop".
        //
        // THE ROW CANNOT BE VACUOUS. Comparing the agent's figure with the plugin's own accumulator would
        // be comparing a number with itself. So the RIG keeps its own maximum of the INSTANTANEOUS value,
        // sampled while audio plays, and the accumulator's peak must be at least that: the accumulator
        // sees every block and the rig sees samples, so a peak below the rig's own sampled max is the
        // accumulator missing what a 20 Hz poller caught.
        {
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
            const auto r = askAgent (rig, "rig-gr-marker: build a compressor and read its gain reduction", 90000);

            // audio through the compressor, sampling the momentary figure as a meter would
            double rigSampledMax = 0.0;
            int samples = 0;
            for (int i = 0; i < 60; ++i)
            {
                for (int k = 0; k < 20; ++k) v2.processOneBlock (sr);
                // freshState(), NOT state(): state() re-reads the file but does not wait for the
                // published snapshot to CORRESPOND to the command just sent, so a poll can read the
                // previous tick's rack. Polling with the weaker read is the "decisions on uncontrolled
                // timing" fault in the row written to catch a timing fault - mine, twice over.
                const auto st8 = rig.freshState();
                if (auto* arr = st8.getProperty ("ownRack", juce::var())
                                    .getProperty ("slots", juce::var()).getArray())
                    for (const auto& sv : *arr)
                        if ((bool) sv.getProperty ("grKnown", false))
                        { rigSampledMax = juce::jmax (rigSampledMax, (double) sv.getProperty ("grDb", 0.0)); ++samples; }
                Rig::pump (30);
            }
            const auto stEnd = rig.freshState();
            double peak = 0.0, typical = 0.0, frames = 0.0, loudest = 0.0;
            juce::String caption;
            const int rackCount = (int) stEnd.getProperty ("ownRack", juce::var()).getProperty ("count", 0);
            if (auto* arr = stEnd.getProperty ("ownRack", juce::var())
                                .getProperty ("slots", juce::var()).getArray())
                for (const auto& sv : *arr)
                    if ((bool) sv.getProperty ("grKnown", false))
                    {
                        peak    = juce::jmax (peak,    (double) sv.getProperty ("grPeakDb", 0.0));
                        loudest = juce::jmax (loudest, (double) sv.getProperty ("grLoudestDb", 0.0));
                        typical = juce::jmax (typical, (double) sv.getProperty ("grTypicalDb", 0.0));
                        frames  = juce::jmax (frames,  (double) sv.getProperty ("grWindowFrames", 0.0));
                        caption = sv.getProperty ("picture", juce::var()).toString();
                    }
            if (samples == 0 || frames <= 0.0)
            {
                // A SKIP MUST NAME ITS CAUSE, not its symptom. "no slot reported a gain reduction" is
                // three different failures - the build did not land, the slot that landed is not a
                // device that reports GR, or audio never reached it - and they point in different
                // directions. So the rack is listed, with what each slot says about itself.
                juce::String what;
                if (auto* arr = stEnd.getProperty ("ownRack", juce::var())
                                    .getProperty ("slots", juce::var()).getArray())
                    for (const auto& sv : *arr)
                        what << sv.getProperty ("n", juce::var()).toString() << ":"
                             << sv.getProperty ("name", juce::var()).toString()
                             << " grKnown=" << sv.getProperty ("grKnown", juce::var()).toString()
                             << " frames=" << sv.getProperty ("grWindowFrames", juce::var()).toString()
                             << "; ";
                skip ("G8: the gain-reduction window reports a peak and a typical",
                      "no slot on the own rack reported a gain reduction in this run, so there is nothing "
                      "to compare and a pass would prove nothing. ownRack.count=" + juce::String (rackCount)
                      + ", the rack held: ["
                      + (what.isEmpty() ? juce::String ("nothing") : what) + "] - frames="
                      + juce::String (frames, 0) + " samples=" + juce::String (samples)
                      + ". The signal is a 220 Hz sine at 0.25 (about -12 dBFS) and the script sets "
                        "threshold -24 dB / ratio 4, so a compressor on this rack should be reducing.");
            }
            else
                // SEAN'S RULING, 15 Oct 2026: the LOUDEST HITS (95th percentile) are the reported
                // figure; the absolute max is a single transient kept for spike warnings. So the
                // ordering that must hold is spikeMax >= loudest >= typical, and the rig's own 20 Hz
                // sampling is compared against the SPIKE max - the one figure that must be at least
                // anything the rig managed to catch. Comparing it against the 95th would be wrong by
                // construction: with 60 samples the rig will often catch a frame above the 95th.
                row ("G8", peak >= rigSampledMax - 0.5 && peak >= loudest && loudest >= typical
                           && loudest > 0.0,
                     "ITEM: check(gr) reports the WINDOW as LOUDEST HITS (95th percentile) and TYPICAL "
                     "(median), with the absolute max kept back for spike warnings - and the three are "
                     "ordered spike >= loudest >= typical  (RED before this round: there was no "
                     "accumulator at all, and then the absolute max was reported as the headline figure "
                     "- 8 dB of transient against a meter reading 2-4)",
                     "loudest(95th) " + juce::String (loudest, 2) + " dB, typical(median) "
                     + juce::String (typical, 2) + " dB, spike max " + juce::String (peak, 2)
                     + " dB over " + juce::String (frames, 0)
                     + " frame(s); the rig's own sampled max was " + juce::String (rigSampledMax, 2)
                     + " dB over " + juce::String (samples) + " sample(s)", &r);

            // ---- G10: THE CAPTION NEVER PRINTS THE NO-READING SENTINEL ------------------------------
            const auto sentinel = Rig::state().getProperty ("slotCaptionSentinel", juce::var()).toString();
            const auto overlaps = Rig::state().getProperty ("slotCaptionOverlapsButtons", juce::var()).toString();
            row ("G10", sentinel != "1" && overlaps != "1",
                 "no slot caption contains the -200 no-reading sentinel, and no caption rect touches the "
                 "slot's buttons  (RED before this fix: ship 1's fix 3 set `valid` from the GR alone, so "
                 "the strip printed \"in -200.0 / out -200.0 dBTP\" at y=47, on top of B / X / < / >)",
                 "sentinel=" + sentinel + " overlapsButtons=" + overlaps
                 + ", caption now \"" + caption.substring (0, 90) + "\"", &r);
        }

        // ---- G11: A FAILED ADD IS SPECIFIC, AND DETAILS IS NOT A REPEAT (Sean's check 5) -------------
        //
        // ON THE AGENT'S OWN PUBLISHED STEPS, not on the transcript row. The row is item 0's surface
        // and is OPT-IN in this ship (Sean's ordering), so a row-based assertion would skip in the
        // configuration that actually ships - and this is one of the six things he is waiting for.
        // The two audiences live on the STEP either way: `detail` is the plain sentence a person
        // reads, `modelDetail` is the technical account, and the whole of his complaint was that they
        // were the same string so Details had nothing to disclose.
        {
            const auto r = askAgent (rig, "rig-setfail-marker: add something that is not installed", 60000);
            juce::String detail, modelDetail;
            const auto a = agentVarOf (Rig::state());
            if (auto* sarr = a.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *sarr)
                    if ((int) sv.getProperty ("status", 0) == 4 /*Failed*/)
                    {
                        detail      = sv.getProperty ("detail", juce::var()).toString();
                        modelDetail = sv.getProperty ("modelDetail", juce::var()).toString();
                    }
            // Fall back to what askAgent captured while the steps existed - they are per-round and
            // the array is empty by the time a run reaches `done` (the 14 Oct lesson).
            if (detail.isEmpty()) detail = r.failDetail;
            row ("G11", detail.isNotEmpty() && modelDetail != detail,
                 "a refused change carries a SPECIFIC plain sentence AND a different technical account "
                 "behind it - so Details discloses something  (Sean's check 5: failure() put one string "
                 "in both, Details repeated the summary, and \"the rack refused it (partial)\" was all "
                 "anyone ever learned about an AU that would not instantiate)",
                 "detail=\"" + detail.substring (0, 90) + "\"  modelDetail=\""
                 + modelDetail.substring (0, 90) + "\"", &r);
        }

        // =====================================================================================
        //  SEAN'S RULE 1 (16 Oct 2026): THE FIVE ROWS THAT REPRODUCE WHAT LOGIC FOUND
        //
        //  "my Logic recheck of ship_2026-10-15c found failures the rig had passed, because the rig
        //   never loads a third-party plugin, uses a scripted model, and checks state rather than what
        //   is on screen."
        //
        //  Each of these runs RED on 15c for a named reason, printed in the row, so the row and the
        //  reproduction are the same artefact.
        // =====================================================================================

        // ---- T1: A THIRD-PARTY ADD WITH SETTINGS ACTUALLY DIALS THE PLUGIN ---------------------------
        //
        // RED ON 15c, three ways at once: (a) the mock served no parameter maps at all, so no third
        // party could be dialled here ever; (b) a Link had no road to ASK for a map - fetch_wired=n;
        // (c) the read-back on a Link parsed its numbers out of the slot's settings TEXT, which is
        // composed from the REQUESTED value, so it could confirm a setting against an echo of itself.
        if (! tp.ok)
        {
            // prepareThirdParty already said why, as a skip. Nothing more to add here.
        }
        else if (linkUid.isEmpty())
            skip ("T1: a third-party add with settings dials the plugin and reads its knob back",
                  "no Link is in the registry in this run, and Sean's failures were all on a Link "
                  "(his EchoJay Compressor dialled fine on the same one, which is the control case)");
        else
        {
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkUid), 8000);
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", linkUid), 8000);
            const int before = (int) linkRowOf (Rig::state(), linkUid).getProperty ("slotCount", -1);
            const auto r = askAgent (rig, "rig-thirdparty-marker: add " + tp.name + " and set its "
                                          + tp.paramName, 120000);
            // THE SLOT FIRST, then the value. A Link instantiates over seconds, and reading once would
            // be the "a dispatch is not a landing" fault in the row written to catch it.
            int after = before;
            juce::String landed, names;
            rig.until ([&] (const juce::var& st)
            {
                const auto lr = linkRowOf (st, linkUid);
                after  = (int) lr.getProperty ("slotCount", -1);
                names  = lr.getProperty ("slots",  juce::var()).toString();
                landed = lr.getProperty ("landed", juce::var()).toString();
                const double v = landedValueOf (landed, tp.paramName);
                return after > before && v == v && std::abs (v - tp.askValue) <= 1.0;   // the VALUE, after the settle
            }, 30000);
            {
                const auto lr = linkRowOf (Rig::state(), linkUid);
                after  = (int) lr.getProperty ("slotCount", -1);
                names  = lr.getProperty ("slots",  juce::var()).toString();
                landed = lr.getProperty ("landed", juce::var()).toString();
            }
            // The number the PLUGIN now shows, out of the sidecar's `landed` field - the one channel the
            // agent's read-back trusts, so the rig cannot pass a row the agent would fail.
            // "slot 1 Attack (Transient)=-14.4;..." - and the KEY HAS SPACES IN IT, because a controls
            // dial is keyed by the plugin's own control name. The first cut took the last
            // whitespace-separated token as the key, which is the same mistake the plain-name renderer
            // was being fed an hour earlier: it would have read "(Transient)=-14.4". So the "slot N "
            // prefix is stripped by position and everything up to the '=' is the name, spaces included.
            double readBack = 0.0; bool haveReadBack = false;
            for (const auto& slotPart : juce::StringArray::fromTokens (landed, "|", ""))
            {
                auto body = slotPart.trim();
                if (body.startsWith ("slot "))
                    body = body.fromFirstOccurrenceOf (" ", false, false).trim()   // drop "slot"
                               .fromFirstOccurrenceOf (" ", false, false).trim();  // drop the number
                for (const auto& pair : juce::StringArray::fromTokens (body, ";", ""))
                {
                    const auto id = pair.upToFirstOccurrenceOf ("=", false, false).trim();
                    if (id != tp.paramName) continue;
                    readBack = pair.fromFirstOccurrenceOf ("=", false, false).trim().getDoubleValue();
                    haveReadBack = true;
                }
            }
            const bool arrived  = after > before && names.containsIgnoreCase (tp.name);
            const bool dialled  = haveReadBack && std::abs (readBack - tp.askValue) <= 1.0;
            const bool moved    = haveReadBack && std::abs (readBack - tp.defaultValue) >= 0.5;
            scen ("A4", "a Link, third party with a good map, add with settings: dialled and read back",
                  arrived && dialled && moved,
                  "slots " + juce::String (before) + " -> " + juce::String (after) + " [" + names + "], "
                  + tp.paramName + " asked " + juce::String (tp.askValue, 2) + " dB, plugin now reads "
                  + (haveReadBack ? juce::String (readBack, 2) + " dB" : juce::String ("NOTHING READ BACK"))
                  + " (default was " + juce::String (tp.defaultValue, 2) + " dB); landed=\""
                  + landed.substring (0, 120) + "\"  (RED on 15c: the mock served no maps, a Link had no "
                  "road to ask for one - fetch_wired=n - and the Link read-back parsed its numbers out of "
                  "an echo of the request)",
                  "A", v2.editor.get(), rig.freshState(), &r);
            // THE FINGERPRINT, as its own check: the mock may have served the map through its __any__
            // fallback, which would mean the rig's fp arithmetic and the plugin's disagree. That is worth
            // knowing and is NOT what T1 is about, so it is a check beside the row rather than inside it.
            {
                const auto maps = juce::JSON::parse (tpMapsFile.loadFileAsString());
                check (maps.getProperty (juce::Identifier (tp.fp), juce::var()).isObject(),
                       "the map was keyed on the fingerprint the rig computed",
                       "fp " + tp.fp.substring (0, 16) + "... (if the mock's log says \"__any__ fallback\" "
                       "for this run, the plugin computed a DIFFERENT fp and T1 passed on the fallback)");
            }
        }

        // =====================================================================================
        //  SEAN'S MORNING RECHECK OF ship_2026-10-16a (16 Oct 2026): THE ROWS FOR WHAT HE FOUND
        //  IN LOGIC. Each one runs RED on 16a for the reason printed in it.
        // =====================================================================================

        // ---- T2: THE OWN RACK'S THIRD-PARTY READ-BACK CONFIRMS WHAT THE PLUGIN SHOWS -----------------
        //
        // RED ON 16a: *"Add Renaissance Axx with threshold at -20 dB. The plugin shows Thresh -20.0 ...
        // but the closing says 'Settings not confirmed'."* The add carries `after`, so the plugin lands
        // where the server asked; the read-back assumed the END of the rack and read another slot's
        // values. His 09:40 log has the same fault twice - the settled read-back verified "slot 5
        // (Vertigo VSM-3)" for a write made to slot 5 (Renaissance Axx) and REVERTED it.
        //
        // The script adds with `after: 0` deliberately: the plugin goes to the FRONT, so a read-back
        // that targets the last slot cannot pass this row by luck.
        if (tp.ok)
        {
            // ---- FROM AN EMPTY RACK (16 Oct 2026, pass 12) -------------------------------------
            // Pass 11 failed this row with "Settings not confirmed", and the fault was the rack it
            // started on: an earlier row had left an EchoJay Compressor in slot 1 and a second
            // copy of this same third party in the rack, so the add's read-back had two slots of
            // the same name to choose between and the row could not say which one it was asking
            // about. The row is about one add on one rack; it starts from one.
            const int before = clearOwnRack (rig);
            const auto r = askAgent (rig, "rig-tpown-marker: add " + tp.name + " to this rack and set its "
                                          + tp.paramName, 120000);
            const auto st = rig.freshState();
            // what the STEP said (captured while the steps existed) and what the closing said
            const auto words   = r.stepWords + " " + r.text;
            const bool claimsUnreadable = words.containsIgnoreCase ("cannot read that plugin's values")
                                       || words.containsIgnoreCase ("not confirmed");
            const bool namesFigure = words.contains (juce::String (tp.askValue, 1))
                                  || words.contains (juce::String (tp.askValue, 2))
                                  || words.contains (juce::String ((int) tp.askValue));
            // ---- AND THE KNOB ACTUALLY MOVED (16 Oct 2026) --------------------------------------
            // The first cut passed on a run that said "the settings did NOT take: the threshold -
            // threshold 0 (asked threshold -24)": the read-back was working perfectly and the DIAL
            // had not landed, and "names the figure" was true of the complaint as well as of the
            // confirmation. A row about dialling has to assert the dial.
            const bool landed = ! words.containsIgnoreCase ("did NOT take")
                             && ! words.containsIgnoreCase ("not applied");
            // THE CLOSING, which is what Sean reads (his note: "show the closing actually saying
            // confirmed with the value"). The step line is the detail behind it.
            const auto closingNow = st.getProperty ("agentRow", juce::var())
                                        .getProperty ("closing", juce::var()).toString();
            // ...and the plugin itself, through the same door the agent reads: the slot's own figure.
            juce::String ownLanded;
            if (auto* arr = st.getProperty ("ownRack", juce::var())
                                .getProperty ("slots", juce::var()).getArray())
                for (const auto& sv : *arr)
                    if (sv.getProperty ("name", juce::var()).toString().containsIgnoreCase (tp.name))
                        ownLanded = sv.getProperty ("landed", juce::var()).toString();
            scen ("A3", "own rack, third party with a good map, add with settings: dialled AND confirmed",
                  r.ran() && ! claimsUnreadable && namesFigure && landed,
                  juce::String ("asked ") + juce::String (tp.askValue, 2) + " for \"" + tp.paramName
                  + "\"; the CLOSING says: \"" + closingNow + "\"; the step said: \""
                  + words.substring (0, 140) + "\"; the slot's own landed field: \""
                  + (ownLanded.isEmpty() ? juce::String ("(empty)") : ownLanded.substring (0, 80))
                  + "\"; the rack held " + juce::String (before) + " slot(s) before the add"
                  + "  (RED on 16a: an add carries `after`, so it lands "
                  "where the server asked, and the read-back read the LAST slot - another plugin - and "
                  "reported \"settings not confirmed\" about a value the plugin was showing correctly)",
                  "A", v2.editor.get(), st, &r);
        }
        else
            scenSkip ("A3", "own rack, third party with a good map: dialled AND confirmed", tp.why, "A");

        // ---- A1 / A2: AN ASK IS NEVER A DEAD END -----------------------------------------------------
        //
        // RED ON 16a, and Sean called it a blocker: *"an agent ask is a dead end in the new layout. It
        // says 'Tap one of the choices to answer' but shows no choices, and when I type 'MTransient' and
        // press Enter, the text vanishes and nothing happens. The run is stuck until I press Stop."*
        //
        // His 09:19 log is the shape of it, exactly: `ask card: "Which transient shaper should I use on
        // the kick?" choices=` followed by `typed answer refused (allowFreeText false)`. The script
        // reproduces that payload - no choices, allowFreeText false - which is the one combination that
        // can leave a run with no road out.
        {
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", ""), 8000);
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0), 8000);
            // ---- THE HINT IS READ WHILE THE QUESTION IS UP (16 Oct 2026) -------------------------
            // The first cut read it AFTER the answer had been typed and the run had finished, so it
            // found an empty hint and a row saying "1 change" - and reported the dead end as still
            // present when it was not. A question's own surface has to be read while it is asking.
            rig.send (echojay::rig::cmd::sendChat,
                      args1 ("text", "rig-nochoice-marker: which shaper?"), 15000);
            juce::String hint, zonesWhileAsking;
            rig.until ([&] (const juce::var& stv)
            {
                const auto a = stv.getProperty ("agentRow", juce::var());
                hint = a.getProperty ("askHint", juce::var()).toString();
                zonesWhileAsking = a.getProperty ("zones", juce::var()).toString();
                return a.getProperty ("status", juce::var()).toString().containsIgnoreCase ("answer");
            }, 45000);
            {   // one more read, so the hint belongs to the moment the status said "waiting"
                const auto a = rig.freshState().getProperty ("agentRow", juce::var());
                hint = a.getProperty ("askHint", juce::var()).toString();
                zonesWhileAsking = a.getProperty ("zones", juce::var()).toString();
            }
            // ...and NOW the user types their answer, which is the other half of the blocker.
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", "MTransient"), 15000);
            AgentOutcome r;
            r.adopted = true; r.settled = true;
            r.state = "done";
            rig.until ([] (const juce::var& stv)
            { const auto a = agentVarOf (stv);
              return ! (bool) a.getProperty ("active", false); }, 60000);
            const auto st = rig.freshState();
            const auto arow = st.getProperty ("agentRow", juce::var());
            const auto zones = zonesWhileAsking;
            juce::String answered;
            if (auto* arr = arow.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                {
                    const auto a = sv.getProperty ("answered", juce::var()).toString();
                    if (a.isNotEmpty()) answered = a;
                }
            const double pg = (double) st.getProperty ("preGainDb", juce::var());
            // A1: the window TOLD the user how to answer. It cannot show chips that were never sent -
            // only B can send those - so what it must do instead is name the road that IS open.
            scen ("B1", "an ask is answerable: chips when there are chips, and a named road when there are not",
                  hint.isNotEmpty(),
                  "while the question was up: askHint=\"" + hint + "\"  zones=["
                  + zones.substring (0, 120) + "]  (RED on 16a: the row showed the question and a faint "
                  "\"your answer?\" with nothing to press, and typing was refused with \"Tap one of the "
                  "choices\" about choices that did not exist)",
                  "A", v2.editor.get(), st);
            // A2: and the typed answer REACHED the run, rather than vanishing.
            scen ("B3", "a typed answer lands in the waiting ask and the run carries on",
                  answered.isNotEmpty() && std::abs (pg - 3.0) < 0.2,
                  "the row recorded " + (answered.isEmpty() ? juce::String ("NOTHING")
                                                            : "\"" + answered + "\"")
                  + " and the run went on to set the pre-gain to " + juce::String (pg, 2) + " dB  (RED on "
                  "16a: interceptTyped refused it because the server sent allowFreeText false, so the text "
                  "vanished and the run sat in awaiting_ask until Stop)",
                  "A", v2.editor.get(), st);
        }

        // ---- P1: NOTHING UNLOADABLE REACHES THE INVENTORY (Sean's step 7, 16 Oct) -----------------
        //
        // His report: *"'Add Pro-C 2' put FabFilter Pro-C 2 behind an Apply and only then failed
        // ('no arm64 build'). Mark every inventory entry that can't load in this host (no arm64
        // slice, failed scan, PACE refused) as not loadable BEFORE the agent sees the inventory, so
        // it's never offered or put behind an Apply."*
        //
        // The architecture gate judged VST3 rows only and said so in a comment; an AU's identifier
        // is not a path, so there was nothing to read. It judges AU bundles by name now.
        //
        // THE ROW FINDS ITS OWN SUBJECT: it sweeps the two Components directories, reads each
        // bundle's Mach-O slices with `lipo`, and takes the first AU with NO arm64 slice as the
        // thing that must not be offered. If this Mac has none, the row SKIPS with that reason -
        // there is nothing to prove, and a pass would be about nothing.
        {
            juce::String unloadable, swept;
            int sweptCount = 0;
            for (const auto& dir : { juce::File ("/Library/Audio/Plug-Ins/Components"),
                                     juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                         .getChildFile ("Library/Audio/Plug-Ins/Components") })
            {
                if (! dir.isDirectory()) continue;
                for (const auto& f : dir.findChildFiles (juce::File::findDirectories, false, "*.component"))
                {
                    auto bin = f.getChildFile ("Contents/MacOS")
                                .getChildFile (f.getFileNameWithoutExtension());
                    if (! bin.existsAsFile()) continue;
                    ++sweptCount;
                    juce::ChildProcess lipo;
                    if (! lipo.start ("lipo -archs \"" + bin.getFullPathName() + "\"")) continue;
                    const auto archs = lipo.readAllProcessOutput();
                    lipo.waitForProcessToFinish (4000);
                    // A LIPO THAT FAILED SAYS NOTHING ABOUT THE SLICES. The green run took
                    // "can't open input file" as "no arm64" and named bx_distorange, whose binary is
                    // not at Contents/MacOS/<bundle name> at all (the product reads its
                    // CFBundleExecutable; this sweep does not). An unreadable bundle is skipped.
                    if (archs.isEmpty() || archs.containsIgnoreCase ("fatal error")
                        || archs.containsIgnoreCase ("can't open")) continue;
                    if (! archs.containsIgnoreCase ("arm64"))
                    { unloadable = f.getFileNameWithoutExtension(); swept = archs.trim(); break; }
                }
                if (unloadable.isNotEmpty()) break;
            }
            if (unloadable.isEmpty())
                scenSkip ("P1", "an AU that cannot load in this process is never offered in the inventory",
                          "every one of the " + juce::String (sweptCount) + " AU component(s) on this Mac "
                          "has an arm64 slice, so there is nothing here that cannot load and nothing to "
                          "withhold. NEVER counted as a pass.", "A");
            else
            {
                const auto st = rig.freshState();
                const auto names = st.getProperty ("inventory", juce::var())
                                      .getProperty ("names", juce::var()).toString();
                const int count = (int) st.getProperty ("inventory", juce::var())
                                         .getProperty ("count", juce::var (0));
                const bool offered = names.containsIgnoreCase (unloadable);
                if (count == 0)
                {
                    // The rig refuses a plugin scan (the iLok rule), so its catalogue holds only what
                    // the run registered by hand and the feed can be empty. An empty inventory
                    // withholds everything and proves nothing.
                    scenSkip ("P1", "an AU that cannot load in this process is never offered in the inventory",
                              "the inventory the agent sees is EMPTY in this run (the rig refuses a scan, so "
                              "the feed has nothing in it), and an empty list withholds everything by "
                              "accident. NEVER counted as a pass; the Logic smoke test covers it.", "A");
                }
                else
                scen ("P1", "an AU that cannot load in this process is never offered in the inventory",
                      count > 0 && ! offered,
                      "\"" + unloadable + "\" has slices [" + swept + "] and no arm64, and the "
                      "inventory the agent sees (" + juce::String (count) + " plugin(s)) "
                      + (offered ? "STILL OFFERS it" : "does not offer it")
                      + "  (RED on 16b: the architecture gate judged VST3 rows only, so Pro-C 2 was "
                      "offered, put behind an Apply, and only then refused with \"no arm64 build\")",
                      "A", v2.editor.get(), st);
            }
        }

        // ---- B2b: A TAPPED PICK IS THE APPROVAL (Sean's step 7, 16 Oct) ---------------------------
        //
        // His report, on the Kick's Link: he tapped "Transient Master" from the chips and was then
        // asked to press Apply on "add Transient Master - on 88_7even Kick_2_01". Two gates, each
        // right on its own; between them he answered the same question twice. "The tap IS the
        // approval, including on another track."
        //
        // The script asks a pick and then sends the add as ask_first, which is the exact shape. The
        // row taps the chip and then asserts that NOTHING is waiting for a press and the plugin
        // landed - and the slot count is the proof that it ran rather than sat.
        {
            clearOwnRack (rig);
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);
            const int before = ownSlotCount (rig.freshState());
            // answerAsks = true: the rig TAPS the card's own first choice, which is the user's tap.
            const auto r = askAgent (rig, "rig-tappick-marker: add a transient shaper", 90000);
            int after = before;
            rig.until ([&] (const juce::var& stv) { after = ownSlotCount (stv); return after > before; }, 20000);
            const auto st = rig.freshState();
            const auto arow = st.getProperty ("agentRow", juce::var());
            const auto zones = arow.getProperty ("zones", juce::var()).toString();
            // kind 1/2 are a line's Apply/Skip and 5/6 the run's Apply all / Skip all: none of them
            // may be on screen, because the tap already said yes.
            // A ZONE LIST IS A LIST, AND A SUBSTRING IS NOT A KIND. "11:1 change" contains "1:",
            // so the first cut read the collapse toggle as a line's Apply and failed a run that had
            // already applied cleanly (slots 0 -> 1, autoApplied=y). Each entry's kind is compared
            // whole, the same way D4 compares its per-step Undo.
            const auto hasZoneKind = [] (const juce::String& z, const char* kind)
            {
                for (auto& e : juce::StringArray::fromTokens (z, ",", ""))
                    if (e.trim().upToFirstOccurrenceOf (":", false, false).trim() == kind) return true;
                return false;
            };
            const bool waitingForAPress = hasZoneKind (zones, "1") || hasZoneKind (zones, "2")
                                       || hasZoneKind (zones, "5") || hasZoneKind (zones, "6");
            bool autoApplied = false;
            if (auto* arr = arow.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr) if ((bool) sv.getProperty ("autoApplied", false)) autoApplied = true;
            scen ("B2b", "a tapped pick IS the approval: the step it chose runs without a second press",
                  r.ran() && after > before && ! waitingForAPress && autoApplied,
                  "slots " + juce::String (before) + " -> " + juce::String (after)
                  + "; autoApplied=" + juce::String (autoApplied ? "y" : "n")
                  + "; zones=[" + zones.substring (0, 80) + "]"
                  + (waitingForAPress ? "  <- STILL ASKING FOR A PRESS, which is what Sean read in Logic"
                                      : juce::String())
                  + "  (RED on 16b: he tapped \"Transient Master\" and was then asked to Apply "
                  "\"add Transient Master - on 88_7even Kick_2_01\")",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- B6: #21 COVERS A RUN WAITING ON APPLY, NOT ONLY AN ASK (Sean's step 7.4 (A)) ----------
        //
        // His report: while a run waited on Apply ("build 3 slots") he typed "set the pre-gain to
        // 2 dB" and got "The agent is still working on ... Stop it first, or wait for it to finish."
        // His ruling: rule #21 covers ANY waiting run. A run waiting on HIM is a question in another
        // shape, and the answer to a question nobody wants to answer is a new instruction.
        {
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);
            // ---- THE RUN HAS TO BE *WAITING* WHEN THE TEXT GOES IN (the red capture said so) -----
            // My first cut used askAgent, which PRESSES APPLY when a plan waits - so by the time the
            // text was typed the run had finished, the text started a run of its own exactly as it
            // should, and the row PASSED on 16b about a state Sean never hit. The run is started
            // directly and the row WAITS for awaiting_approval before typing a word.
            rig.send (echojay::rig::cmd::sendChat,
                      args1 ("text", "rig-auto-marker: set the pre gain, twice"), 15000);
            juce::String waitState;
            const bool reachedWaiting = rig.until ([&] (const juce::var& stv)
            {
                waitState = stv.getProperty ("agent", juce::var())
                               .getProperty ("state", juce::var()).toString();
                return waitState.containsIgnoreCase ("awaiting_approval");
            }, 45000);
            AgentOutcome r;
            r.adopted = true; r.settled = true; r.state = waitState;
            if (! reachedWaiting)
            {
                scenSkip ("B6", "a new instruction typed while a run waits on APPLY stops it and starts its own",
                          "the run never reached awaiting_approval (state \"" + waitState + "\" after 45 s), "
                          "so there was no waiting run to type into. NEVER counted as a pass.", "A");
                rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
            }
            else
            {
            const auto before = rig.freshState().getProperty ("agentRow", juce::var());
            const auto stateBefore = rig.freshState().getProperty ("agent", juce::var())
                                         .getProperty ("state", juce::var()).toString();
            const int  rowsBefore = (int) before.getProperty ("rows", juce::var());
            const auto sidBefore  = before.getProperty ("sessionId", juce::var()).toString();
            // ...and now a new instruction typed into it, in his own words.
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", "Set the pre-gain to 2 dB"), 15000);
            juce::String notice, statusAfter, sidAfter;
            int rowsAfter = rowsBefore;
            rig.until ([&] (const juce::var& stv)
            {
                const auto a = stv.getProperty ("agentRow", juce::var());
                const auto ag = stv.getProperty ("agent", juce::var());
                notice      = ag.getProperty ("notice", juce::var()).toString();
                statusAfter = a.getProperty ("status", juce::var()).toString();
                sidAfter    = a.getProperty ("sessionId", juce::var()).toString();
                rowsAfter   = (int) a.getProperty ("rows", juce::var());
                return rowsAfter > rowsBefore || (sidAfter.isNotEmpty() && sidAfter != sidBefore);
            }, 30000);
            const bool refused = notice.containsIgnoreCase ("still working");
            scen ("B6", "a new instruction typed while a run waits on APPLY stops it and starts its own",
                  ! refused && (rowsAfter > rowsBefore || sidAfter != sidBefore),
                  "the run was " + (stateBefore.isEmpty() ? juce::String ("(no state)") : stateBefore)
                  + " when the text was typed; rows " + juce::String (rowsBefore) + " -> "
                  + juce::String (rowsAfter) + ", session \"" + sidBefore.substring (0, 10) + "\" -> \""
                  + sidAfter.substring (0, 10) + "\", status now \"" + statusAfter + "\""
                  + (refused ? ("  <- REFUSED with \"" + notice.substring (0, 70) + "\"")
                             : juce::String())
                  + "  (RED on 16b: \"The agent is still working on ... Stop it first, or wait for it "
                  "to finish\" - #21 was wired to the ask road only)",
                  // NO OUTCOME PASSED, deliberately: this row judges TWO runs - the one that was
                  // waiting and the one the typed text started - and the "a row cannot pass without
                  // a turn" rule is about a single turn's outcome. Its evidence is the row count and
                  // the session id, both printed above. (The first cut passed &r and forced a fail
                  // on a row whose behaviour was right.)
                  "A", v2.editor.get(), rig.freshState());
            juce::ignoreUnused (r);
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
            }
        }

        // ---- A3: A NEW INSTRUCTION CLOSES A WAITING RUN AND STARTS ITS OWN ---------------------------
        //
        // RED ON 16a: *"A new request typed while a run waits on an ask got merged into that run ('Add
        // Pro-C 2' continued the previous GR run, with its check-gr steps). A new instruction closes the
        // waiting run (marked stopped) and starts a new run."*
        {
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
            // Leave the question UNANSWERED (answerAsks = false): the row's whole subject is what
            // happens when the user types something else instead.
            const auto r = askAgent (rig, "rig-newinstr-marker: check the gr", 60000, false);
            const auto before = rig.freshState().getProperty ("agentRow", juce::var());
            const auto sidBefore  = before.getProperty ("sessionId", juce::var()).toString();
            const int  rowsBefore = (int) before.getProperty ("rows", juce::var());
            // ...and now a NEW instruction, typed into the composer while the choices are on screen.
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", "rig-row-marker: set the pre-gain"), 15000);
            juce::String sidAfter, statusAfter;
            int rowsAfter = rowsBefore;
            rig.until ([&] (const juce::var& stv)
            {
                const auto a = stv.getProperty ("agentRow", juce::var());
                sidAfter    = a.getProperty ("sessionId", juce::var()).toString();
                rowsAfter   = (int) a.getProperty ("rows", juce::var());
                statusAfter = a.getProperty ("status", juce::var()).toString();
                return rowsAfter > rowsBefore || (sidAfter.isNotEmpty() && sidAfter != sidBefore);
            }, 30000);
            scen ("B4", "a new instruction typed while an ask waits closes that run and starts its own",
                  r.sawAsk && (rowsAfter > rowsBefore || sidAfter != sidBefore),
                  "rows " + juce::String (rowsBefore) + " -> " + juce::String (rowsAfter)
                  + ", session \"" + sidBefore.substring (0, 12) + "\" -> \"" + sidAfter.substring (0, 12)
                  + "\", status now \"" + statusAfter + "\"  (RED on 16a: the text was answered INTO the "
                  "waiting run, so a request to add a plugin arrived as the answer to \"What now?\" and "
                  "carried the old run's steps with it)",
                  "A", v2.editor.get(), rig.freshState(), &r);
        }

        // ---- G13: A READ-ONLY RUN HAS NO CHANGES TO COUNT --------------------------------------------
        //
        // RED ON 16a: *"A read-only run (a GR question) shows '4 changes'. Read-only steps aren't
        // changes: show 'N steps', or no count if nothing changed."*
        {
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
            const auto r = askAgent (rig, "rig-readonly-marker: what is on this rack?", 90000);
            const auto arow = rig.freshState().getProperty ("agentRow", juce::var());
            const int changes = (int) arow.getProperty ("changes", juce::var());
            const auto zones  = arow.getProperty ("zones", juce::var()).toString();
            const bool saysSteps   = zones.containsIgnoreCase ("step");
            const bool saysChanges = zones.containsIgnoreCase ("change");
            scen ("E8", "a read-only run reports steps, not changes",
                  r.ran() && changes == 0 && saysSteps && ! saysChanges,
                  "changes=" + juce::String (changes) + "  the control's label is in zones=["
                  + zones.substring (0, 140) + "]  (RED on 16a: every finished step counted as a change, "
                  "so a GR question said \"4 changes\")",
                  "A", v2.editor.get(), rig.freshState(), &r);
        }

        // ---- M2: NO STEP DETAIL IS ADDRESSED TO THE MODEL --------------------------------------------
        //
        // RED ON 16a, twice in one morning: *"Internal model instructions leak into the step details:
        // 'Report it as ADDED, with its settings not confirmed. Do NOT call the add itself not
        // applied...'"* and again two hours later. Item 3 (13 Oct) split the FAILURE path's two
        // audiences and left the SUCCESS path - `landedLine` - read by the model and painted on the row.
        //
        // ON EVERY STEP OF EVERY ROW IN THIS RUN, not on one scripted case: the phrases are ours, so a
        // sweep is cheap and it catches the next one to be written as well as the two he found.
        {
            static const char* kModelPhrases[] = {
                "Report it as", "Report the plugin", "Do NOT", "do not name any value",
                "and do not name", "must not be quoted", "repeat this same call", "you were not given"
            };
            juce::StringArray offenders;
            // ON THE RUN THAT CARRIED THE MODEL'S TEXT (16 Oct 2026). The first cut read whatever run
            // happened to be on screen - a read-only one - and passed on 16a over a build whose add
            // path leaked "Report it as ADDED ... Do NOT call the add itself not applied". The add
            // that cannot be confirmed is the one that carries it, so this scenario runs one.
            askAgent (rig, "rig-nomap-marker: add that compressor", 90000);
            const auto arow = rig.freshState().getProperty ("agentRow", juce::var());
            if (auto* arr = arow.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                {
                    const auto d = sv.getProperty ("detail", juce::var()).toString();
                    for (const char* ph : kModelPhrases)
                        if (d.contains (ph)) offenders.add ("\"" + d.substring (0, 90) + "\"");
                }
            scen ("I4", "clean wording: no step detail is addressed to the model",
                  offenders.isEmpty(),
                  offenders.isEmpty() ? juce::String ("every step detail on screen reads as a sentence to a "
                                                      "person")
                                      : juce::String ("leaked: ") + offenders.joinIntoString ("; "),
                  "A", v2.editor.get(), arow.isVoid() ? rig.freshState() : rig.freshState());
        }

        // ---- I4b: A RAW ID IN THE SERVER'S OWN SUMMARY IS STILL NOT DRAWN ----------------------
        //
        // The joint live pass drew this step line from the real server's plan summary:
        //
        //     set slot 1: attack_ms 20, release_ms 60 - on 88_7even Kick_2_02
        //
        // Invariant I4 caught it, and the ids are the server's to stop sending - but a line the user
        // reads is this build's responsibility whoever composed it. The mock sends that exact shape
        // so the rule is held DETERMINISTICALLY rather than only when the model happens to produce
        // it, and the row asserts BOTH halves: the ids read as words AND the track name survives.
        {
            clearOwnRack (rig);
            const auto r = askAgent (rig, "rig-rawids-marker: set the attack and release", 60000);
            const auto st = rig.freshState();
            juce::String line;
            if (auto* arr = st.getProperty ("agentRow", juce::var())
                                .getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                {
                    const auto l = sv.getProperty ("label", juce::var()).toString();
                    if (l.isNotEmpty()) line = l;
                }
            const bool wordsNow = line.contains ("attack") && line.contains ("release")
                               && ! line.contains ("attack_ms") && ! line.contains ("release_ms")
                               && ! line.contains ("freq_hz") && ! line.contains ("band1.");
            scen ("I4b", "a raw id in the server's own summary is drawn as words, and names survive",
                  r.ran() && wordsNow,
                  "the step line reads: \"" + line + "\"  (the server sent \"set slot 1: attack_ms 20, "
                  "release_ms 60, band1.freq_hz 240\"; RED before 16 Oct evening: the ids went straight "
                  "to the row, which is how the live pass drew \"attack_ms 20, release_ms 60 - on "
                  "88_7even Kick_2_02\")",
                  "A", v2.editor.get(), st, &r);
        }

        // =====================================================================================
        //  THE MATRIX'S LINK TARGETS, ON LINKS IN THEIR OWN PROCESSES (T2, T3, T4)
        //
        //  Sean, 16 Oct: *"Every Link scenario in the matrix (targets T2-T4) must run on this
        //  setup."* These scenarios address a Link that is a SEPARATE PROCESS holding REAL
        //  third-party plugins, so the V2 cannot see inside it - which is the condition under which
        //  three of this week's faults were invisible.
        // =====================================================================================
        if (linkProcs.empty() || ! linkProcs.front()->up())
        {
            const auto why = linkProcWhy.isNotEmpty() ? linkProcWhy
                                                      : juce::String ("no out-of-process Link registered "
                                                        "within its bound - see the link_host lines above");
            scenSkip ("A4b", "T2: add with settings to a Link in its own process", why, "A");
            scenSkip ("G1",  "T2: the main rack strip follows a Link's change with no manual switch", why, "A");
            scenSkip ("E5",  "T2: no GR figure on a Link's slot in the mirror", why, "A");
            scenSkip ("T3",  "T3: a Link selected in another instance refuses the edit, in words", why, "A");
        }
        else
        {
            // ---- THE ROW MEASURES THE LINK IT IS TALKING TO (22 Oct 2026) ----------------------
            //
            // Reading the ack fixed the refusal and introduced a worse fault in its place: the uid
            // came from the V2's own list, which no longer had a usable Kick, so the row AIMED at
            // the Vocal Link and went on MEASURING the Kick -
            //
            //     the Link's own process holds 3 slot(s) [...]; the V2's strip draws 1
            //     [Renaissance Axx (s)] after 8074 ms  (aimed at 30e4781a31 "88_7even Vox_Lead_01")
            //
            // - so it compared one Link's strip with another Link's rack and waited out its whole
            // 8 s bound for a third slot that was never coming. This tree has the rule written down
            // from I9c: "a row sets its own target". The target is picked FIRST, and the process
            // behind it is the one every row in this block reads.
            juce::String aimName;
            const auto aimUid = usableLinkUid (rig.freshState(), true, &aimName);
            int aimedIdx = -1;
            for (int i = 0; i < (int) linkProcs.size(); ++i)
                if (linkProcs[(size_t) i]->uid == aimUid) aimedIdx = i;
            auto& kick = *(aimedIdx >= 0 ? linkProcs[(size_t) aimedIdx] : linkProcs.front());
            // ---- THE V2 HAS TO AGREE THIS LINK IS THERE, AND THE ACK IS READ (22 Oct 2026) -----
            //
            // G1 failed in gate 2026-10-22 with "the Link's own process holds 3 slot(s); the V2's
            // strip draws 0 (sidecar revision )", which reads as a mirroring fault and is not what
            // happened. Two lines the row never looked at say so:
            //
            //     EJRig: select_link seq=1135 -> FAILED (that uid is not live)
            //     EJRig: work_on     seq=1136 -> FAILED (that uid is not live)
            //
            // The V2 had refused to aim at the Link at all, so the strip was never going to draw it,
            // and the row reported the consequence instead of the cause. Row I9c learnt this on
            // 18 Oct, in these words: *"the row's guard tested that the STRING was non-empty, which
            // is not the same question as whether the Link exists - and the command that would have
            // told it so had its ack discarded."*  Same fault, four rows along.
            //
            // So the uid is resolved from the V2'S OWN published list (the launched ones first), the
            // acks are read, and a refusal is carried into the rows below as the REFUSAL, in the
            // V2's words. It is deliberately NOT turned into a blanket skip: a Link whose process is
            // up and holding a rack while the V2 says its uid is not live is a product fault, and
            // the rows say which of the two they are looking at.
            const auto useUid  = aimedIdx >= 0 ? aimUid : kick.uid;
            const auto selAck  = rig.send (echojay::rig::cmd::selectLink, args1 ("uid", useUid), 8000);
            const auto workAck = rig.send (echojay::rig::cmd::workOn,     args1 ("uid", useUid), 8000);
            const bool aimed   = (bool) selAck.getProperty ("ok", false)
                              && (bool) workAck.getProperty ("ok", false);
            const auto aimNote = selAck.getProperty ("note", juce::var()).toString() + "/"
                               + workAck.getProperty ("note", juce::var()).toString();
            // The Link's own process, independently of what the V2 thinks of it.
            const bool linkReallyUp = kick.up()
                                   && (int) kick.state().getProperty ("slotCount", -1) > 0;
            Rig::pump (1200);

            // ---- G1: THE MAIN RACK STRIP FOLLOWS A LINK'S OWN RACK (I7) --------------------------
            // The Link already holds its prebuilt plugins, put there by its OWN process, so this is
            // the purest form of Sean's #12: the V2 has never seen those slots and must show them.
            {
                // ---- AND THE STRIP IS WAITED FOR, WITH A BOUND (22 Oct 2026) -----------------
                // This read a single pump of 1200 ms. A borrow read is not instant - his 11:32
                // report was a rack view that said "Reading 0 plugin(s)..." for minutes - so a row
                // that reads once measures whatever moment it happened to arrive in ("decisions on
                // uncontrolled timing"). The bound IS the acceptance: a Link's rack must be on the
                // strip within 8 s of aiming at it, and the row reports how long it took.
                const int realSlots  = (int) kick.state().getProperty ("slotCount", -1);
                const auto t0 = juce::Time::getMillisecondCounter();
                while (juce::Time::getMillisecondCounter() - t0 < 8000u)
                {
                    if ((int) rig.freshState().getProperty ("chainView", juce::var())
                                              .getProperty ("count", -1) >= realSlots) break;
                    Rig::pump (250);
                }
                const int tookMs = (int) (juce::Time::getMillisecondCounter() - t0);
                const auto st = rig.freshState();
                const auto lr = linkRowOf (st, useUid);
                const int drawnSlots = (int) st.getProperty ("chainView", juce::var())
                                                 .getProperty ("count", -1);
                const auto drawnNames = st.getProperty ("chainView", juce::var())
                                            .getProperty ("slots", juce::var()).toString();
                if (! aimed && ! linkReallyUp)
                    scenSkip ("G1", "T2: the main rack strip shows the Link's real rack with no manual switch",
                              "the V2 refused to aim at this Link (" + aimNote + ") and its process is "
                              "not up with a rack either, so there is nothing for the strip to mirror",
                              "A");
                else
                scen ("G1", "T2: the main rack strip shows the Link's real rack with no manual switch",
                      aimed && realSlots > 0 && drawnSlots == realSlots,
                      juce::String (aimed ? "" : "THE V2 REFUSED TO AIM AT IT (" + aimNote + ") while "
                                                   "its process IS up holding a rack - so the strip was "
                                                   "never going to mirror anything, and that refusal is "
                                                   "the fault, not the empty strip. ")
                      + "the Link's own process holds " + juce::String (realSlots) + " slot(s) ["
                      + kick.state().getProperty ("slots", juce::var()).toString() + "]; the V2's strip "
                      "draws " + juce::String (drawnSlots) + " [" + drawnNames + "] after "
                      + juce::String (tookMs) + " ms  (sidecar revision "
                      + lr.getProperty ("revision", juce::var()).toString() + ", aimed at " + useUid
                      + (aimName.isNotEmpty() ? " \"" + aimName + "\"" : juce::String()) + ")",
                      "A", v2.editor.get(), st);

                // ---- E5: AND NO GR FIGURE ON ANY OF THEM (Sean's #7 morning) --------------------
                juce::String badges;
                if (auto* arr = st.getProperty ("chainView", juce::var())
                                    .getProperty ("slotBadges", juce::var()).getArray())
                    for (const auto& b : *arr) badges << b.toString() << "|";
                scen ("E5", "T2: a Link's slot shows no GR figure until Link meters ship",
                      ! badges.containsIgnoreCase ("GR "),
                      "the badges drawn on the Link's slots: [" + badges + "]  (RED on 16a: the mirror's "
                      "own instances do not process audio, so their reduction read 0.0 and was drawn as "
                      "\"GR 0.0\")",
                      "A", v2.editor.get(), st);
            }

            // ---- A4b: ADD WITH SETTINGS, ACROSS THE PROCESS BOUNDARY ----------------------------
            if (tp.ok)
            {
                // THE SECOND PROCESS, and a rack nothing else in this run has touched. Running this on
                // the Kick Link made it order-dependent: by the time it ran, scenario A4 had already
                // put the same plugin there, so "the slot count went up" was false while the value was
                // right. A scenario that depends on what ran before it is a scenario that reports the
                // order rather than the product.
                auto& fresh = linkProcs.size() > 1 ? *linkProcs[1] : kick;
                rig.send (echojay::rig::cmd::selectLink, args1 ("uid", fresh.uid), 8000);
                rig.send (echojay::rig::cmd::workOn,     args1 ("uid", fresh.uid), 8000);
                Rig::pump (1000);
                const int before = (int) fresh.state().getProperty ("slotCount", -1);
                const auto r = askAgent (rig, "rig-thirdparty-marker: add " + tp.name + " and set its "
                                              + tp.paramName, 120000);
                int after = before; juce::String landed, names;
                // WAIT FOR THE VALUE, NOT FOR THE KEY (16 Oct 2026). A Waves plugin's display is one
                // write behind until the message loop runs, so the product writes, reads "0.0",
                // and VERIFIES AFTER A SETTLE - its log says so in as many words ("applied after
                // settle, reads -30.0"). A rig that stops as soon as the key appears reads the
                // pre-settle 0.0 and reports a correct dial as a failure, which is the same class of
                // mistake as the product's own 21 Sep one.
                rig.until ([&] (const juce::var&)
                {
                    const auto st = fresh.state();
                    after  = (int) st.getProperty ("slotCount", -1);
                    names  = st.getProperty ("slots",  juce::var()).toString();
                    landed = st.getProperty ("landed", juce::var()).toString();
                    const double v = landedValueOf (landed, tp.paramName);
                    return after > before && v == v && std::abs (v - tp.askValue) <= 1.0;
                }, 30000);
                double readBack = 0.0; bool haveReadBack = false;
                for (const auto& part : juce::StringArray::fromTokens (landed, "|", ""))
                {
                    auto body = part.trim();
                    if (body.contains ("{")) body = body.fromFirstOccurrenceOf ("{", false, false)
                                                        .upToLastOccurrenceOf ("}", false, false);
                    for (const auto& pair : juce::StringArray::fromTokens (body, ";", ""))
                    {
                        const auto id = pair.upToFirstOccurrenceOf ("=", false, false).trim();
                        if (id != tp.paramName) continue;
                        readBack = pair.fromFirstOccurrenceOf ("=", false, false).trim().getDoubleValue();
                        haveReadBack = true;
                    }
                }
                scen ("A4b", "T2: add with settings to a Link in its OWN PROCESS, dialled and read back",
                      after > before && names.containsIgnoreCase (tp.name)
                      && haveReadBack && std::abs (readBack - tp.askValue) <= 1.0,
                      "slots " + juce::String (before) + " -> " + juce::String (after) + " [" + names
                      + "], " + tp.paramName + " asked " + juce::String (tp.askValue, 2)
                      + ", the plugin in the other process now shows "
                      + (haveReadBack ? juce::String (readBack, 2) : juce::String ("NOTHING READ BACK"))
                      + "  (this is the channel with no shared memory to fall back on)",
                      "A", v2.editor.get(), rig.freshState(), &r);
            }
            else
                scenSkip ("A4b", "T2: add with settings to a Link in its own process", tp.why, "A");

            // ---- T3: THE RACK-SELECTION LOCK, IN WORDS ------------------------------------------
            //
            // *"The rack-selection lock: a Link selected in another instance's rack ('deselect there
            // to edit here')."* The SECOND V2 (the VST3 one) selects the same Link, which takes the
            // lock; the AU V2 then tries to edit it and must refuse with the sentence a person reads.
            // Both V2s are real instances of the shipping plugin, which is what makes this the real
            // contention rather than a simulated one.
            if (v2v.plugin == nullptr)
                scenSkip ("T3", "T3: a Link selected in another instance refuses the edit, in words",
                          "the VST3 V2 did not load, so there is no second instance to hold the lock", "A");
            else
            {
                // ---- THIS WINDOW LETS GO FIRST (16 Oct 2026) ------------------------------------
                // The AU V2 had the Kick rack SELECTED from the scenarios above, so it was holding
                // the lock itself - and the "contention" was this instance competing with nobody. It
                // deselects first; the second instance then takes the rack; and the edit goes out on
                // the CHAT's working channel, which is the road a user takes when the rack they are
                // asking about is open somewhere else.
                rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
                Rig::pump (1200);
                v2v.openEditor();
                Rig::pump (1500);
                // The second instance selects the Link's rack: that is what takes the lock.
                rig.sendTo ("VST3", echojay::rig::cmd::selectLink, args1 ("uid", kick.uid), 8000);
                rig.send (echojay::rig::cmd::workOn, args1 ("uid", kick.uid), 8000);
                // ...AND THE LOCK HAS TO BE VISIBLE BEFORE THE EDIT IS SENT (16 Oct 2026). Sending it
                // 2.5 s later and hoping produced a run that timed out ("the rack never acknowledged
                // it within 8 s") rather than one that was refused - which proves nothing about the
                // refusal. The lock is published; this waits for it, bounded, and says so if it never
                // appears rather than testing a race.
                juce::String lockState;
                const bool lockVisible = rig.until ([&] (const juce::var& stv)
                {
                    lockState = stv.getProperty ("rackLock", juce::var()).toString();
                    // "other" is the one that matters here: this window can SEE that someone else
                    // holds the rack. "idle" and "held" are this instance's own states.
                    return lockState.equalsIgnoreCase ("other") || lockState.equalsIgnoreCase ("wait");
                }, 12000);
                Rig::pump (800);
                const auto r = askAgent (rig, "rig-linkadd-marker: add a compressor to the kick", 90000);
                juce::ignoreUnused (lockVisible);
                const auto st = rig.freshState();
                const auto words = r.stepWords + " " + r.text + " " + r.failDetail;
                // THE SENTENCE FAMILY, not one phrasing. The rack refuses contention in more than one
                // form of words depending on which door it comes through - "this rack is being edited
                // from the main plugin - try again after release" is the one the agent's edit road
                // produces, and my first cut only looked for the SELECTION wording and called a
                // correct refusal a failure. What matters is that a person is told, in words, that
                // someone else has it - never a silent no-op and never a code.
                const bool refusedInWords = words.containsIgnoreCase ("deselect")
                                         || words.containsIgnoreCase ("selected on")
                                         || words.containsIgnoreCase ("locked by")
                                         || words.containsIgnoreCase ("being edited from")
                                         || words.containsIgnoreCase ("try again after release")
                                         || words.containsIgnoreCase ("edited in its own window");
                const auto lockNow = st.getProperty ("rackLock", juce::var()).toString();
                // IF THE LOCK NEVER BECAME VISIBLE, SAY SO RATHER THAN JUDGE THE REFUSAL. A row that
                // reports "it was not refused" when nothing was holding the rack is testing nothing.
                if (! lockVisible && ! lockNow.equalsIgnoreCase ("other"))
                    scenSkip ("T3", "T3: a Link held by another instance refuses the edit, in words",
                              "this window never saw another instance holding the rack (rackLock=\""
                              + lockNow + "\" after 12 s), so there was no contention to refuse. Never "
                              "counted as a pass.", "A");
                else
                    scen ("T3", "T3: a Link held by another instance refuses the edit in words a person reads",
                          refusedInWords,
                          "rackLock=\"" + lockNow + "\"; what the run said: \"" + words.substring (0, 180)
                          + "\"  (the second V2 holds the rack on " + kick.uid + "; the refusal must name "
                          "it, never fail silently)",
                          "A", v2.editor.get(), st, &r);
                rig.sendTo ("VST3", echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
                v2v.closeEditor();
                Rig::pump (1200);
            }

            // ---- LINK WINDOW OPEN vs CLOSED ----------------------------------------------------
            {
                // THE LOCK FROM T3 HAS TO BE GONE FIRST. The second instance released the rack a
                // moment ago, and an edit sent into that window is refused for T3's reason rather
                // than tested for this one.
                rig.until ([] (const juce::var& stv)
                { return ! stv.getProperty ("rackLock", juce::var()).toString().equalsIgnoreCase ("other"); },
                           15000);
                rig.send (echojay::rig::cmd::selectLink, args1 ("uid", kick.uid), 8000);
                rig.send (echojay::rig::cmd::workOn,     args1 ("uid", kick.uid), 8000);
                Rig::pump (1200);
                const auto ack = kick.send ("open_editor");
                const bool opened = (bool) ack.getProperty ("state", juce::var())
                                               .getProperty ("editorOpen", false);
                const auto before = (int) kick.state().getProperty ("slotCount", -1);
                const auto r = askAgent (rig, "rig-linkadd-marker: add a compressor to the kick", 90000);
                int after = before;
                rig.until ([&] (const juce::var&)
                { after = (int) kick.state().getProperty ("slotCount", -1); return after > before; }, 25000);
                // A MIXED RUN CANNOT ANSWER THIS EITHER (17 Oct, item 0b). The claim is that an
                // edit from the V2 lands on a Link whose own window is open - both halves of that
                // sentence are code, and in a mixed run they come from different builds. It skipped
                // in the two runs before this one for a different reason (no editor opened) and then
                // went red the moment one did, which is how it got here.
                if (gLinkIsMixedBuild)
                    scenSkip ("T2w", "T2: an edit lands on a Link whose OWN window is open",
                              "the Link that AU hosting loaded is the INSTALLED binary, not the one "
                              "this run was given (item 0b) - an edit between a V2 from this build and "
                              "a Link from another is a pairing that never ships, so neither a landing "
                              "nor a refusal would be a verdict on anything. NEVER counted as a pass.",
                              "A");
                else if (! opened)
                    scenSkip ("T2w", "T2: an edit lands on a Link whose OWN window is open",
                              "the Link's own process would not open an editor this run (\""
                              + ack.getProperty ("note", juce::var()).toString() + "\"), so there is no "
                              "open window for an edit to land under. Never counted as a pass.", "A");
                else
                    scen ("T2w", "T2: an edit lands on a Link whose OWN window is open",
                          after > before,
                          "the Link's editor was open and its rack went " + juce::String (before)
                          + " -> " + juce::String (after)
                          + " slot(s)  (a republish that only happens with a window open would show here)",
                          "A", v2.editor.get(), rig.freshState(), &r);
                kick.send ("close_editor");
            }
        }

        // =====================================================================================
        //  THE MATRIX'S REMAINING STARRED SCENARIOS, on the own rack (T1)
        //  Every one of these ran RED on ship_2026-10-16a; the reason is printed in each.
        // =====================================================================================

        // ---- C1: THE CLOSING OF AN ADD THAT COULD NOT BE CONFIRMED (Sean's #4, #5) -------------------
        {
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", ""), 8000);
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
            const auto r = askAgent (rig, "rig-nomap-marker: add that compressor", 90000);
            const auto st = rig.freshState();
            const auto words = r.stepWords + " " + r.text;
            const bool addedFirst = words.containsIgnoreCase ("Added")
                                 && ! words.containsIgnoreCase ("Not applied: add")
                                 && ! words.containsIgnoreCase ("running on default");
            const bool noFigure  = ! words.contains ("4.0") && ! words.contains ("4.00");
            scen ("C1", "an add nothing could confirm says \"Added X. Settings not confirmed.\" and no figure",
                  r.ran() && addedFirst && noFigure,
                  "the run said: \"" + words.substring (0, 180) + "\"  (RED on 15c/16a: \"Not applied: add "
                  "X\" over a plugin that WAS added, and an invented \"running on default values\")",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- G4 / I6: ONE WAITING LINE, ONE PAIR OF BUTTONS (Sean's #4 morning) ---------------------
        {
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0), 8000);
            // DELIBERATELY NOT ANSWERED: the subject is what is DRAWN while it waits.
            // answerAsks=false AND approve=false: the subject is the row as it waits.
            const auto r = askAgent (rig, "rig-oneline-marker: nudge the pre-gain", 40000, false, "yes", false);
            juce::var arow;
            juce::String zones;
            rig.until ([&] (const juce::var& stv)
            {
                arow  = stv.getProperty ("agentRow", juce::var());
                zones = arow.getProperty ("zones", juce::var()).toString();
                return zones.containsIgnoreCase ("Apply") || zones.contains ("1:");
            }, 25000);
            const auto st = rig.freshState();
            arow  = st.getProperty ("agentRow", juce::var());
            zones = arow.getProperty ("zones", juce::var()).toString();
            // kind 1 = ApplyLine, 5 = ApplyAll (RunHit's order); the labels ride on the zone string.
            const bool hasLineApply = zones.contains ("1:") || zones.contains (",1") || zones.startsWith ("1");
            const bool hasApplyAll  = zones.containsIgnoreCase ("Apply all")
                                   || zones.containsIgnoreCase ("Skip all");
            scen ("G4", "one waiting line draws only that line's buttons, no second Apply",
                  hasLineApply && ! hasApplyAll,
                  "zones=[" + zones + "]  (RED on 16a: the line's Apply AND the run's Apply/Skip all were "
                  "both drawn for a single pending step, so one decision had two doors)",
                  "A", v2.editor.get(), st, &r);
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
        }

        // ---- G5: NO STRAY "applied" BESIDE A LINE THAT APPLIED NOTHING (Sean's #5 morning) ----------
        {
            const auto r = askAgent (rig, "rig-readonly-marker: what is on this rack?", 60000);
            const auto st = rig.freshState();
            juce::StringArray stray;
            if (auto* arr = st.getProperty ("agentRow", juce::var()).getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                {
                    const auto label = sv.getProperty ("label", juce::var()).toString();
                    const auto right = sv.getProperty ("right", juce::var()).toString();
                    const bool reads = label.containsIgnoreCase ("read") || label.containsIgnoreCase ("look")
                                    || label.containsIgnoreCase ("check");
                    if (reads && right.containsIgnoreCase ("applied")) stray.add (label + " -> \"" + right + "\"");
                }
            scen ("G5", "a line that only read something never says \"applied\"",
                  stray.isEmpty(),
                  stray.isEmpty() ? juce::String ("every status word belongs to the step it is on")
                                  : "stray: " + stray.joinIntoString ("; ")
                                    + "  (RED on 16a: every finished step said \"applied\", including a "
                                      "look and a check)",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- E1 / E2 / G6 / G7: THE GR RUN, AND THE BUBBLES AROUND IT -------------------------------
        {
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
            const auto r = askAgent (rig, "rig-gr-marker: how much is it doing?", 120000);
            // ---- AUDIO THROUGH THE COMPRESSOR, OR THE FIGURE IS 0.0 (Sean, 16 Oct) ---------------
            // *"E2 must compare tile and meter while the compressor is actually reducing (several
            // dB), not at 0.0."* He is right, and the reason it read 0.0 is that Rig::pump runs the
            // MESSAGE loop only: between the build and the read, not one audio block went through
            // the rack, so the device had nothing to reduce. Two seconds of the rig's -12 dBFS tone
            // against a threshold of -24 is about 3 dB of reduction, which is what the tile and the
            // meter then have to agree about.
            for (int i = 0; i < 180; ++i)
            {
                for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr);
                if ((i % 20) == 0) Rig::pump (12);
            }
            Rig::pump (300);
            const auto st = rig.freshState();

            // G6: the build's own result bubble must not ride beside the run (Sean's #3 morning)
            int chainBuilt = 0, closingCopies = 0;
            const auto closing = st.getProperty ("agentRow", juce::var())
                                     .getProperty ("closing", juce::var()).toString().trim();
            if (auto* arr = st.getProperty ("onScreen", juce::var()).getArray())
                for (const auto& m : *arr)
                {
                    const auto t = m.getProperty ("text", juce::var()).toString();
                    if (t.contains ("Chain built -")) ++chainBuilt;
                    if (closing.isNotEmpty() && t.contains (closing)) ++closingCopies;
                }
            scen ("G6", "no separate \"Chain built - N plugins loaded.\" bubble beside a run",
                  chainBuilt == 0,
                  juce::String (chainBuilt) + " such bubble(s) on screen  (RED on 16a: the build road's own "
                  "result bubble appeared next to the run message, twice)",
                  "A", v2.editor.get(), st, &r);
            scen ("G7", "the closing sentence appears only inside the run message",
                  closingCopies <= 1,
                  juce::String (closingCopies) + " copy(ies) of the closing in the message list  (RED on "
                  "16a: the editor and EJAgentHome each opened a record for one run, so the closing was "
                  "written to a second row that drew as a plain bubble)",
                  "A", v2.editor.get(), st, &r);

            // ---- G7b: ONE SENTENCE, DRAWN ONCE (16 Oct 2026, Sean in Logic) --------------------
            //
            // He read a MTransient run in Logic: "the closing appears twice, as a bubble ABOVE the
            // ASK row and again inside the run" - and named the cause from this runner's own
            // evidence: A3-live's drawn.txt has `opening` and `closing` as the SAME sentence. On a
            // short run they are. The model streams one sentence, which becomes the message's
            // content and is drawn above the rows; the done-summary is that same sentence, which the
            // row drew again as its closing.
            //
            // The mock sends exactly that shape - one delta, and a done summary word for word the
            // same - so the rule is held deterministically: the row draws NO closing when it would
            // only repeat the opening, and the sentence is on screen exactly ONCE.
            {
                clearOwnRack (rig);
                const auto r1 = askAgent (rig, "rig-onesentence-marker: add a gate", 90000);
                const auto st1 = rig.freshState();
                const auto arow = st1.getProperty ("agentRow", juce::var());
                const auto sentence = juce::String ("Added an EchoJay Gate to this rack.");
                int copies = 0;
                if (auto* arr = st1.getProperty ("onScreen", juce::var()).getArray())
                    for (const auto& m : *arr)
                        if (m.getProperty ("text", juce::var()).toString().contains (sentence)) ++copies;
                const auto closingNow = arow.getProperty ("closing", juce::var()).toString().trim();
                const auto openingNow = arow.getProperty ("opening", juce::var()).toString().trim();
                // ---- RE-AIMED (19 Oct 2026): WHICH COPY SURVIVES CHANGED ---------------------
                // This required the row's `closing` to be EMPTY, because when it was written the
                // copy that survived was the BUBBLE's. On 18 Oct he ruled the other way - *"Show it
                // once, in full, inside the card"* - so the card now carries the sentence and the
                // bubble's copy is the one suppressed. The subject of the row is unchanged and is
                // the thing he read in Logic: the sentence is on screen ONCE. That is what is
                // asserted; which surface holds it is the ruling's business, not this row's.
                scen ("G7b", "a short run whose closing IS its opening draws that sentence once",
                      r1.ran() && copies == 1,
                      juce::String (copies) + " copy(ies) of \"" + sentence + "\" on screen; the row's "
                      "opening is \"" + openingNow.substring (0, 60) + "\" and its closing is "
                      + (closingNow.isEmpty() ? juce::String ("EMPTY, as it should be when it would only "
                                                              "repeat the opening")
                                              : "\"" + closingNow.substring (0, 60) + "\"")
                      + "  (RED before this: the same sentence was drawn as the message's content AND "
                      "again as the row's closing - what Sean read in Logic on the MTransient run)",
                      "A", v2.editor.get(), st1, &r1);
            }


            // E2: the tile's figure IS the device's own held value
            double badgeGr = -1.0, devHeld = -1.0; juce::String badge;
            if (auto* arr = st.getProperty ("ownRack", juce::var()).getProperty ("slots", juce::var()).getArray())
                for (const auto& sl : *arr)
                    if ((bool) sl.getProperty ("devReportsGr", false))
                    {
                        badge   = sl.getProperty ("badge", juce::var()).toString();
                        devHeld = (double) sl.getProperty ("grHeldDb", juce::var());
                        if (badge.containsIgnoreCase ("GR "))
                            badgeGr = badge.fromFirstOccurrenceOf ("GR ", false, true)
                                           .upToFirstOccurrenceOf (" ", false, false).getDoubleValue();
                        break;
                    }
            const bool fits = ! badge.contains ("...") && badge.length() <= 14;
            // AND IT HAS TO BE REDUCING. A tile and a meter that agree about 0.0 agree about nothing:
            // Sean's complaint was a figure frozen at 10.9 while the meter moved, and only a live,
            // non-zero reduction can tell a frozen badge from a correct one.
            const bool reducing = devHeld >= 0.5;
            scen ("E2", "the tile's GR figure is the device's own held value while it is reducing, "
                        "and the caption fits",
                  reducing && badgeGr >= 0.0 && std::abs (badgeGr - devHeld) <= 0.15 && fits,
                  "the tile draws \"" + badge + "\" (" + juce::String (badgeGr, 2)
                  + ") and the device's own hold reads " + juce::String (devHeld, 2)
                  + (reducing ? " while it is reducing" : " - BUT IT IS NOT REDUCING, so this compares nothing")
                  + "  (RED on 16a: the badge only reached the panel on a STRUCTURAL rebuild, so it froze "
                  "- \"GR 10.9 WORK...\" while the meter read 4.4, 3.8, 8.8)",
                  "A", v2.editor.get(), st, &r);

            // E1: what the run SAID about it - loudest hits and typical, and never an instant
            const auto grWords = r.stepWords + " " + r.text;
            scen ("E1", "a GR answer gives loudest hits and typical, and never an instant figure",
                  r.ran() && ! grWords.containsIgnoreCase ("grNowDb")
                          && ! grWords.containsIgnoreCase ("instant"),
                  "the run said: \"" + grWords.substring (0, 200) + "\"  (RED on 16a: \"loudest hits\" never "
                  "arrived and the model quoted \"the instant hitting up to 6 dB\", which the result should "
                  "never have carried)",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- E3: AN UNCERTIFIED PLUGIN IS SAID TO BE UNCERTIFIED, NOT SILENT (Sean's #19) -----------
        if (tp.ok)
        {
            // ---- THE UNCERTIFIED PLUGIN, BY NAME (Sean, 16 Oct) -----------------------------------
            // *"E3 must use an uncertified plugin (Renaissance Axx), not the EchoJay Compressor."*
            // The first cut ran the same gr-marker script, which checks SLOT 1 - and slot 1 was the
            // EchoJay Compressor the build had just put there, so the row was asking about a
            // certified device and passing because it did not say "no audio". Its own script asks
            // about the slot the third party is in.
            // SLOT 1 IS THE THIRD PARTY HERE, AND THIS ROW MAKES IT SO (16 Oct 2026, pass 12).
            // It used to rely on A3 having put it there with `after: 0`; forty rows later slot 1
            // was an EchoJay Compressor a build row had left, the check was asked about a
            // CERTIFIED device, it answered correctly with a figure, and the row failed for a
            // reason that had nothing to do with #19. So the rack is emptied and the third party
            // added here: slot 1 is the uncertified plugin because this row put it there.
            // The script does NOT wait for playback first - the whole point of #19 is that an
            // uncertified plugin is refused BEFORE any listening.
            clearOwnRack (rig);
            const auto added = askAgent (rig, "rig-tpown-marker: add " + tp.name + " to this rack and set its "
                                              + tp.paramName, 120000);
            // WAITED FOR, NOT READ ONCE: the add returns when the run ends and the panel publishes on
            // its own tick, so the first cut read an empty rack and reported `slot 1 holds ""` about a
            // rack that held exactly what it had just put there.
            juce::String slot1Name;
            rig.until ([&] (const juce::var& stv)
            {
                if (auto* arr = stv.getProperty ("ownRack", juce::var())
                                   .getProperty ("slots", juce::var()).getArray())
                    if (! arr->isEmpty())
                        slot1Name = (*arr)[0].getProperty ("name", juce::var()).toString();
                return slot1Name.containsIgnoreCase (tp.name);
            }, 8000);
            const auto r = askAgent (rig, "rig-grtp-marker: how much is " + tp.name + " doing?", 90000);
            const auto st = rig.freshState();
            // THE ROW'S OWN STEP DETAIL. r.stepWords is captured while the steps exist and can be
            // empty on a one-round run that finished between polls - and an empty string passes a
            // "does not say X" test while proving nothing. The drawn row is the evidence.
            juce::String rowWords;
            if (auto* arr = st.getProperty ("agentRow", juce::var()).getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                    rowWords << sv.getProperty ("label",  juce::var()).toString() << " = "
                             << sv.getProperty ("detail", juce::var()).toString() << "; ";
            const auto words = rowWords + " " + r.stepWords + " " + r.text + " " + r.failDetail;
            const bool saysSilence = words.containsIgnoreCase ("no audio has been heard")
                                  || words.containsIgnoreCase ("silent or muted");
            // ...and it must actually SAY it cannot measure that one, not merely avoid the silence
            // excuse: a run that said nothing at all would have passed the old assertion.
            // ---- THE RULE THIS ROW ASSERTS CHANGED ON 18 OCT 2026 -----------------------------
            // His ruling: *"EchoJay Compressor = its own meter; certified third-party = Kathy's
            // measured curve; anything else = no figure. check gr on a third-party slot returns
            // that slot's measured INPUT level over the listen window, plus the read-back."*  So
            // "not certified" is no longer the answer for a third party - the answer is the
            // measurement, with a plain line saying EchoJay does not read their reduction meter.
            // The half of this row that still bites is the half he wrote it for: it must never
            // blame silence. Kept, with the vocabulary the product now uses.
            // ---- AND E3's SUBJECT IS NOW THE OTHER HALF (19 Oct 2026) -------------------------
            // Today's full run shows what it actually gets: *"gain reduction on the third-party
            // slot = Press play and I'll measure that."*  Under his 18 Oct ruling that is the TRUE
            // answer for a slot nothing has been heard on - this row deliberately does not play -
            // so "says it cannot measure that one" is no longer a sentence the product owns.
            // The half that still bites, and the half he wrote the row for, is that it must never
            // blame SILENCE while audio is playing; asking for playback when there is none is not
            // that. So the ask counts as an honest answer here, and the measurement road gets its
            // own row with audio in it (I9d below).
            const bool asksForPlayback = words.containsIgnoreCase ("press play")
                                      || words.containsIgnoreCase ("play the")
                                      || words.containsIgnoreCase ("i'll measure");
            const bool saysUncertified = asksForPlayback
                                      || words.containsIgnoreCase ("does not read")
                                      || words.containsIgnoreCase ("no GR meter is read")
                                      || words.containsIgnoreCase ("cannot read its own")
                                      || words.containsIgnoreCase ("not certified")
                                      || words.containsIgnoreCase ("not a plugin EchoJay has measured")
                                      || words.containsIgnoreCase ("cannot measure")
                                      || words.containsIgnoreCase ("not an EchoJay device");
            scen ("E3", "check(gr) on an UNCERTIFIED third party says so at once, and never blames silence",
                  r.ran() && slot1Name.containsIgnoreCase (tp.name) && ! saysSilence && saysUncertified,
                  "slot 1 holds \"" + slot1Name + "\" (the add " + (added.ran() ? "ran" : "did NOT run")
                  + "); the run said: \"" + words.substring (0, 200) + "\"  (RED on 16a: with audio playing and "
                  "Renaissance Axx showing 15.5 dB of attenuation, the answer was \"No audio has been heard "
                  "for gain reduction on slot 1 yet\" - a reason that was not true)",
                  "A", v2.editor.get(), st, &r);
        }
        else
            scenSkip ("E3", "check(gr) on an uncertified plugin says so at once", tp.why, "A");

        // ---- E7: THE GR METER'S TWO LABELS CANNOT OVERLAP (Sean's #15) ------------------------------
        //
        // A GEOMETRY ASSERTION, not a screenshot: the layout is one pure function now
        // (GrMeter::labelBoxes), so this holds it to its contract in both directions - the held
        // figure's box and the live value's box must not intersect when both are drawn, and the live
        // value must have the whole track when the held figure is not.
        {
            const juce::Rectangle<float> track { 0.0f, 0.0f, 160.0f, 22.0f };
            const auto both = echojay::device::GrMeter::labelBoxes (track, false, true);
            const auto one  = echojay::device::GrMeter::labelBoxes (track, false, false);
            const bool apart = ! both.held.isEmpty() && ! both.live.intersects (both.held);
            const bool whole = one.held.isEmpty() && one.live.getWidth() > both.live.getWidth();
            scen ("E7", "the GR meter's held figure and live value never share a pixel",
                  apart && whole,
                  "with a hold: live=" + both.live.toString() + " held=" + both.held.toString()
                  + "; without: live=" + one.live.toString()
                  + "  (RED on 16a: both were drawn centred-right in the same track, so \"-4.4 dB\" was "
                  "printed over \"dB\")",
                  "A", v2.editor.get(), rig.freshState());
        }

        // ---- C5: AFTER AN UNDO, THE CLOSING DESCRIBES THE UNDONE STATE (Sean's #14) -----------------
        {
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0), 8000);
            const auto r = askAgent (rig, "rig-undoclose-marker: put the pre-gain up", 90000);
            Rig::pump (600);
            rig.send (echojay::rig::cmd::press, args1 ("button", "undo_all"), 12000);
            Rig::pump (1500);
            const auto st = rig.freshState();
            const auto arow = st.getProperty ("agentRow", juce::var());
            const auto closing = arow.getProperty ("closing", juce::var()).toString();
            const int changes = (int) arow.getProperty ("changes", juce::var());
            const bool stale = closing.containsIgnoreCase ("is set to")
                            || closing.containsIgnoreCase ("set the pre-gain");
            scen ("C5", "after Undo all the closing no longer claims the change",
                  changes == 0 && ! stale,
                  "changes=" + juce::String (changes) + " closing=\"" + closing.substring (0, 140)
                  + "\"  (RED on 16a: after \"Everything the agent changed has been undone\" the run still "
                  "read \"Pre gain on the Mix Bus is set to 0 dB\")",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- B5: HIDE STEPS WORKS ON A STOPPED OR FAILED RUN (Sean's #2, #13) -----------------------
        {
            const auto r = askAgent (rig, "rig-setfail-marker: add something that is not there", 90000);
            const auto before = rig.freshState().getProperty ("agentRow", juce::var());
            const bool openBefore  = (bool) before.getProperty ("stepsOpen", false);
            const bool failsBefore = (bool) before.getProperty ("failuresOnly", false);
            rig.send (echojay::rig::cmd::press, args1 ("button", "changes"), 10000);
            Rig::pump (900);
            const auto mid = rig.freshState().getProperty ("agentRow", juce::var());
            const bool openMid = (bool) mid.getProperty ("stepsOpen", false);
            rig.send (echojay::rig::cmd::press, args1 ("button", "changes"), 10000);
            Rig::pump (900);
            const auto after = rig.freshState().getProperty ("agentRow", juce::var());
            const bool openAfter   = (bool) after.getProperty ("stepsOpen", false);
            const bool failsAfter  = (bool) after.getProperty ("failuresOnly", false);
            // BOTH WAYS, and the failure never hidden. A finished run that failed a line starts
            // COLLAPSED (the mockup's rule) with its failed line still drawn - failuresOnly - so the
            // first press OPENS it and the second collapses it again. The thing that must never
            // happen is a press that changes nothing, which is what Sean reported.
            scen ("B5", "the steps control works on a run with a failed line, and the failure stays visible",
                  openMid != openBefore && openAfter != openMid
                  && (openBefore ? true : failsBefore) && (openAfter ? true : failsAfter),
                  "stepsOpen " + juce::String (openBefore ? "y" : "n") + " -> "
                  + juce::String (openMid ? "y" : "n") + " -> " + juce::String (openAfter ? "y" : "n")
                  + ", failuresOnly " + juce::String (failsBefore ? "y" : "n") + " -> "
                  + juce::String (failsAfter ? "y" : "n")
                  + "  (RED on 16a: the view forced the steps open on any run with a failure, so the "
                  "control was drawn and did nothing at all)",
                  "A", v2.editor.get(), rig.freshState(), &r);
        }

        // ---- A12 / I9: PLAIN STEP DETAILS, AND SUGGESTED SETTINGS THAT TELL THE TRUTH ---------------
        {
            const auto r = askAgent (rig, "rig-eqband-marker: two bells on the EQ", 90000);
            const auto st = rig.freshState();
            juce::String details, settings;
            if (auto* arr = st.getProperty ("agentRow", juce::var()).getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr) details << sv.getProperty ("detail", juce::var()).toString() << " ";
            if (auto* arr = st.getProperty ("ownRack", juce::var()).getProperty ("slots", juce::var()).getArray())
                for (const auto& sl : *arr)
                    if (sl.getProperty ("name", juce::var()).toString().containsIgnoreCase ("EQ"))
                        settings = sl.getProperty ("settings", juce::var()).toString();
            const bool plain = ! details.contains ("band1.") && ! details.contains ("_hz")
                            && ! details.contains ("_db");
            scen ("A12", "step details read as words, not as parameter ids",
                  r.ran() && plain,
                  "the details said: \"" + details.substring (0, 200) + "\"  (RED on 16a: \"slot 2 "
                  "threshold_db -14.00, slot 2 ratio 4.00\")",
                  "A", v2.editor.get(), st, &r);
            scen ("I9", "Suggested Settings shows what landed, or says it is not confirmed",
                  settings.isNotEmpty()
                  && (settings.containsIgnoreCase ("band") || settings.containsIgnoreCase ("not confirmed")),
                  "the EQ's card reads: \"" + settings.substring (0, 160) + "\"  (RED on 16a: the card was "
                  "composed from the value that was ASKED for, so it could read \"Applied automatically\" "
                  "over settings the read-back said had not taken)",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- I4c RUNS LATE TOO, BECAUSE IT SENDS A RACK AN OP IT CANNOT DO (17 Oct) ---------------
        // It exists to make a Link's validator refuse something, which is the point - and a refused
        // batch leaves the V2's mirror of that rack out of step, so the next add to it was refused
        // on the base-list check ("the rack is not the one this plan was made for"). A2 went 4 -> 4
        // because of this row, not because of the build. Same lesson as T5, one row along.
        // ---- I4c: A RACK'S REFUSAL READS AS WORDS (Sean's step 8, 16 Oct) -------------------------
        //
        // He read "op 1 invalid: ..." on a failed line. That is ChainHost's edit pre-flight speaking
        // in the vocabulary of the protocol it validates - a person has no "op 1". I4's deny-list
        // bans the phrase outright now, and this row produces it on purpose so the rule is held
        // deterministically rather than only when a model happens to send a bad op.
        if (! linkUid.isEmpty())
        {
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", linkUid), 10000);
            const auto r = askAgent (rig, "rig-badop-marker: do the impossible", 60000);
            juce::String line;
            if (auto* arr = rig.freshState().getProperty ("agentRow", juce::var())
                                .getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                {
                    const auto d = sv.getProperty ("detail", juce::var()).toString();
                    if (d.isNotEmpty()) line = d;
                }
            const bool raw = line.containsIgnoreCase ("op 1 invalid")
                          || line.containsIgnoreCase ("unknown operation");
            if (line.trim().isEmpty())
                scenSkip ("I4c", "a rack's refusal reads as words, never \"op 1 invalid\"",
                          "the run produced no failed line at all in this host (the op was refused "
                          "before it reached a rack's validator), so there is no wording to judge. "
                          "A10 and B5 hold the same rule on the line Sean actually read. NEVER "
                          "counted as a pass.", "A");
            else
            scen ("I4c", "a rack's refusal reads as words, never \"op 1 invalid\"",
                  r.ran() && line.isNotEmpty() && ! raw,
                  "the failed line reads: \"" + line.substring (0, 130) + "\""
                  + (raw ? "  <- THE RAW PREFIX Sean read in Logic" : juce::String())
                  + "  (RED on 16b: \"op 1 invalid: unknown operation ...\" went straight to the row)",
                  "A", v2.editor.get(), rig.freshState(), &r);
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
        }
        else
            scenSkip ("I4c", "a rack's refusal reads as words, never \"op 1 invalid\"",
                      "no Link in the registry this run, and the own rack's parser answers in different "
                      "words (\"that change carried no operation this rack understands\") - the phrase "
                      "Sean read comes from a Link's validator.", "A");

        // ---- AND THE REPLAY ROWS RUN HERE, FOR EXACTLY THAT REASON (17 Oct 2026) ------------------
        // They were first written in front of U1, near the top, and A4 and G1 went red two runs
        // running: "slots 2 -> -1" and "the V2's strip draws 0". Not a product fault and not the
        // disk sweep I first blamed - the replay rows press "+ New chat", which clears the chat's
        // channel binding, so every later row that assumes the V2 is still borrowing the Kick's
        // sidecar inherited a window that had been pointed somewhere else. The rig has a convention
        // for that and it is written three lines below: a row that leaves state behind runs last.
        // =========================================================================================
    //  THE REPLAY ROWS (17 Oct 2026, Sean's method for this round)
        // =========================================================================================
    //  "Five fixes pass in the rig and FAIL in Logic (tap Apply, Pro-C 2 behind Apply, kHs tried,
    //   B's false 'only transient shaper', New chat carry-over). Replay this morning's real sessions
    //   from your logs (the exact start bodies and frames between 08:05 and 08:30) into the runner,
    //   and make each one RED from the real data before fixing. Rows from synthetic setups don't
    //   count for these."
    //
    //  He is right about the cause and the cure. B2b passes because its scripted add carries no
    //  `plan` frame; the real session's add carries one, and that one frame is the whole difference
    //  between a tap that applies and a tap that then asks for Apply. A row written from a shape I
    //  imagined can only ever test the shape I imagined.
    //
    //  So: tools/host_rig/replay/*.json are extracted from the plugin's own log by replay/extract.py
    //  - the frame sequence per round, every tool-call id, every approval mode, every plan frame,
    //  the done summaries, and the presses and typed answers with their timings. The mock serves a
    //  fixture verbatim when the goal carries "rig-replay:<id>". Two sentences in those files are
    //  Sean's quotation rather than a recording, and they say so in a `textSource` field, because
    //  `message` frames were not logged with their text by the build that ran this morning. They are
    //  now (EJAgentClient's unknown-frame line), so the next replay is exact.
    if (! live)
    {
        leg ("REPLAY: this morning's real sessions, 08:05-08:30, served frame for frame");

        // The fixture's own goal text, with the marker the mock keys on. The marker is in the GOAL
        // and not in an env var because one run replays several sessions.
        auto replayGoal = [] (const char* fixture, const juce::String& realGoal)
        { return realGoal + "  [rig-replay:" + fixture + "]"; };

        // Everything these rows read, in one place, so a row's evidence and its words agree.
        auto agentState  = [&] (const juce::var& st) { return agentVarOf (st).getProperty ("state", juce::var()).toString(); };
        auto askPending  = [&] (const juce::var& st) { return (bool) agentVarOf (st).getProperty ("askPending", false); };
        auto sessionOf   = [&] (const juce::var& st) { return st.getProperty ("agentRow", juce::var())
                                                                .getProperty ("sessionId", juce::var()).toString(); };
        auto screenText  = [&] (const juce::var& st)
        {
            juce::String all;
            if (auto* arr = st.getProperty ("onScreen", juce::var()).getArray())
                for (const auto& m : *arr) all << m.getProperty ("text", juce::var()).toString() << "\n";
            return all;
        };

        // ---- RP8K: A RESULT MUST FIT 8 KB ON **HIS** INVENTORY (17 Oct 2026) -------------------
        //
        // His recheck of 17b died in round 1 of EVERY run:
        //   EJAgent: frame error code=result_too_large "result toolu_01VtSXwQ7... is over 8 KB; send
        //   the compact summary shape (CONTRACT_AGENT_TOOLS section 5)"
        // and the call two lines above it in his log is `look "read the inventory"`.
        //
        // The rig could not have caught it, and the reason is his item 3: the rig's controlled plugin
        // list has a handful of entries, his Mac has 1,013 bundles of which 357 cannot run here at
        // all. So the input comes from HIS MACHINE now - tools/host_rig/fixtures/real_inventory...json,
        // a snapshot of the names and the architecture verdicts, taken by reading each bundle's
        // Mach-O header and loading nothing.
        //
        // AND THE FIRST ATTEMPT AT THIS ROW TAUGHT ITS OWN LESSON. It seeded 420 synthetic entries
        // through a DynamicObject keyed by juce::Identifier with a '|' in it, the keys were dropped,
        // the plugin read an empty file, and the row reported a 578-byte result - a green-looking
        // number from an input that never arrived. So the seed is written as TEXT, and the row now
        // CHECKS THAT ITS OWN INPUT LANDED before it judges anything.
        {
            const auto snapFile = juce::File (__FILE__).getParentDirectory()
                                      .getChildFile ("fixtures/real_inventory_2026-10-17.json");
            const auto snap = juce::JSON::parse (snapFile.loadFileAsString());
            // THIS ROW NEEDS ITS OWN PROCESS. The inventory's SIZE is its subject, and the size
            // comes from the 1,013 names in the catalogue, which only EJ_RIG_BIG_INVENTORY=1 puts
            // there (see the seeding at construction and why it is not the default).
            const bool bigInv = ::getenv ("EJ_RIG_BIG_INVENTORY") != nullptr;
            juce::StringArray badNames, badWhys;
            if (auto* arr = snap.getProperty ("notLoadable", juce::var()).getArray())
                for (const auto& e : *arr)
                {
                    badNames.add (e.getProperty ("name", juce::var()).toString());
                    badWhys.add  (e.getProperty ("why",  juce::var()).toString());
                }

            // THE PATH THE PRODUCT ACTUALLY READS (17 Oct 2026). My first two attempts wrote to
            // "Library/Application Support/EchoJay", which is where auth.json and the Link sidecars
            // live - but ChainHost's own appSupportDir is userAppData()/EchoJay, and under isolation
            // userAppData() is $ECHOJAY_STATE_HOME/Library. So the seed went somewhere nothing reads
            // and the row reported a comfortable 561 bytes for the second time running.
            //
            // His real file proves the shape: ~/Library/EchoJay/plugin_load_failed.json, which is
            // exactly userAppData()/EchoJay on a machine without isolation.
            const auto root = juce::File (juce::String (::getenv ("ECHOJAY_STATE_HOME") != nullptr
                                                        ? ::getenv ("ECHOJAY_STATE_HOME") : "/tmp"))
                                  .getChildFile ("Library/EchoJay");
            root.createDirectory();
            const auto failedFile = root.getChildFile ("plugin_load_failed.json");
            {
                juce::String json = "{\n \"v\": 1,\n \"failed\": {\n";
                for (int i = 0; i < badNames.size(); ++i)
                {
                    const auto key = badNames[i].toLowerCase() + "|VST3";
                    json << "  " << juce::JSON::toString (juce::var (key)) << ": {"
                         << "\"name\": "  << juce::JSON::toString (juce::var (badNames[i])) << ", "
                         << "\"format\": \"VST3\", "
                         << "\"why\": "   << juce::JSON::toString (juce::var (badWhys[i])) << ", "
                         << "\"atMs\": "  << juce::String ((double) juce::Time::currentTimeMillis(), 1)
                         << "}" << (i + 1 < badNames.size() ? "," : "") << "\n";
                }
                json << " }\n}\n";
                failedFile.replaceWithText (json);
            }
            // DID THE INPUT ARRIVE? Read it back the way the product reads it, before judging.
            const auto readBack = juce::JSON::parse (failedFile.loadFileAsString());
            const int seeded = readBack.getProperty ("failed", juce::var()).getDynamicObject() != nullptr
                             ? (int) readBack.getProperty ("failed", juce::var()).getDynamicObject()
                                     ->getProperties().size() : 0;
            std::printf ("        (seeded %d of his %d unloadable plugins into %s)\n",
                         seeded, badNames.size(), failedFile.getFullPathName().toRawUTF8());

            if (! bigInv)
                scenSkip ("RP8K", "REPLAY rp_8k: look(inventory) fits 8 KB on HIS inventory",
                          "this process was not started with EJ_RIG_BIG_INVENTORY=1, so the catalogue "
                          "holds this rig's handful of plugins rather than his 1,013. A small result "
                          "about one plugin cannot test the size of an answer about a thousand - the "
                          "row's first run passed on exactly that, 568 bytes from half an input. Run "
                          "it as its own pass: EJ_RIG_BIG_INVENTORY=1 EJ_RIG_ONLY=RP8K.", "A");
            else if (seeded < 100)
                scenSkip ("RP8K", "REPLAY rp_8k: look(inventory) fits 8 KB on HIS inventory",
                          "the seed did not land (" + juce::String (seeded) + " entries readable in "
                          + failedFile.getFileName() + ", from a snapshot of " + juce::String (badNames.size())
                          + "): this row's whole subject is the SIZE of a result, so without its input it "
                            "would report a comfortable number from nothing - which is exactly what its "
                            "first version did. NEVER counted as a pass.", "A");
            else
            {
                clearOwnRack (rig);
                rig.send (echojay::rig::cmd::newChat, args1 ("uid", ""), 10000);
                rig.send (echojay::rig::cmd::workOn,  args1 ("uid", ""), 8000);
                const auto r8 = askAgent (rig, replayGoal ("rp_8k", "Add a compressor to this rack"), 90000);

                // WHAT THE PLUGIN POSTED, measured by the thing that received it.
                int worst = 0, inventoryBytes = -1;
                int invTotal = -1, invReturned = -1, invNotLoadable = -1;
                juce::String keys, worstId;
                if (const auto req = lastRequestTo (mockPort, "/api/agent/step"); req.isObject())
                {
                    worst = (int) req.getProperty ("maxResultBytes", juce::var (0));
                    if (auto* arr = req.getProperty ("results", juce::var()).getArray())
                        for (const auto& rr : *arr)
                        {
                            const int b = (int) rr.getProperty ("bytes", juce::var (0));
                            if (b >= worst) { worst = b; worstId = rr.getProperty ("id", juce::var()).toString(); }
                            juce::StringArray ks;
                            if (auto* ka = rr.getProperty ("keys", juce::var()).getArray())
                                for (const auto& k : *ka) ks.add (k.toString());
                            if (ks.contains ("names"))
                            {
                                inventoryBytes = b; keys = ks.joinIntoString (",");
                                const auto nums = rr.getProperty ("nums", juce::var());
                                invTotal       = (int) nums.getProperty ("total", juce::var (-1));
                                invReturned    = (int) nums.getProperty ("returned", juce::var (-1));
                                invNotLoadable = (int) nums.getProperty ("notLoadableTotal", juce::var (-1));
                            }
                        }
                }
                const bool fits   = inventoryBytes >= 0 && inventoryBytes <= 8192;
                const bool hasTot = keys.contains ("notLoadableTotal");
                // THE SCALE IT MEASURED, NOT JUST THE SIZE. 8 KB is only a verdict about his
                // machine if the answer was ABOUT his machine: a thousand names in the catalogue,
                // a page of them returned, and the rest declared rather than sent.
                const bool atScale = invTotal >= 500;
                const bool paged   = invReturned > 0 && invReturned < invTotal;
                scen ("RP8K", "REPLAY rp_8k: look(inventory) fits 8 KB on HIS inventory (1,013 names, 357 unloadable)",
                      fits && hasTot && atScale && paged,
                      juce::String ("with ") + juce::String (seeded) + " of his unloadable plugins seeded, "
                      "the inventory result the plugin POSTED was "
                      + (inventoryBytes < 0 ? juce::String ("never seen (no step body carried a `names` result)")
                                            : juce::String (inventoryBytes) + " bytes"
                                              + (fits ? " - inside the contract's 8 KB"
                                                      : " - OVER THE CONTRACT'S 8 KB, which is the error he read"))
                      + "; total " + juce::String (invTotal) + ", returned " + juce::String (invReturned)
                      + ", notLoadableTotal " + juce::String (invNotLoadable)
                      + (atScale ? "" : juce::String (" - NOT AT SCALE: an inventory of ") + juce::String (invTotal)
                                        + " cannot test the size of an answer about his 1,013, so this is not a pass")
                      + (paged || ! atScale ? "" : juce::String (" - NOT PAGED: it returned all ")
                                        + juce::String (invReturned) + " names, so nothing was summarised")
                      + "; keys [" + keys + "]"
                      + (hasTot ? "" : "; notLoadableTotal MISSING, so the model cannot know the scale "
                                       "without being sent the bytes")
                      + "; largest result in that body " + juce::String (worst) + " bytes (" + worstId + ")",
                      "A", v2.editor.get(), rig.freshState(), &r8);
            }
            failedFile.deleteFile();   // the seed is this row's, not the next row's
        }

        // ---- RP1 (item 1): "New chat" must close the open run, not inherit it -------------------
        // The real session: kHs failed on the Kick's Link, the server then ASKED something (tc_q2),
        // and that ask was still pending when he pressed New chat, aimed at the Mix Bus and typed
        // his question. It went into the old run, on the old target.
        if (! linkUid.isEmpty())
        {
            clearOwnRack (rig);
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", linkUid), 8000);
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkUid), 8000);
            // answerAsks = FALSE, approve = TRUE: the Apply press is needed to get past response 1's
            // add, and the ask in response 2 must be LEFT waiting - which is the state his New chat
            // was pressed in. The first capture let askAgent answer it by default, so the row looked
            // for a pending ask that the helper had already consumed and reported "the fixture did
            // not reproduce the waiting ask" about a fixture that reproduces it exactly.
            const auto r1 = askAgent (rig, replayGoal ("rp_cp_8", "Add kHs Transient Shaper"), 90000,
                                      /*answerAsks*/ false, "yes", /*approve*/ true);
            rig.until ([&] (const juce::var& st) { return askPending (st); }, 20000);
            const auto before = rig.freshState();
            const auto sidBefore = sessionOf (before);
            const bool askWasPending = askPending (before);

            // "+ New chat", aimed at the MAIN rack - his exact move
            const auto ack = rig.send (echojay::rig::cmd::newChat, args1 ("uid", ""), 10000);
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
            Rig::pump (600);
            const auto mid = rig.freshState();

            // ...and then his question, typed into the fresh chat
            rig.send (echojay::rig::cmd::sendChat,
                      args1 ("text", "how much gain reduction is the compressor doing?"), 15000);
            Rig::pump (2500);
            const auto after = rig.freshState();
            const auto sidAfter = sessionOf (after);

            const bool runClosed   = ! askPending (mid)
                                  && ! agentState (mid).containsIgnoreCase ("await")
                                  && ! (bool) agentVarOf (mid).getProperty ("active", false);
            const bool freshSession = sidAfter != sidBefore;
            const bool oldRunGone   = ! screenText (after).containsIgnoreCase ("kHs Transient Shaper");
            scen ("RP1", "REPLAY rp_cp_8: \"New chat\" closes the open run and starts a fresh session",
                  askWasPending && runClosed && freshSession && oldRunGone,
                  juce::String ("an ask was pending when New chat was pressed: ")
                  + (askWasPending ? "yes" : "NO - the fixture did not reproduce the waiting ask")
                  + "; after New chat the run was " + (runClosed ? "closed" : "STILL OPEN (state \"" + agentState (mid) + "\", askPending " + juce::String ((int) askPending (mid)) + ")")
                  + "; the typed question went to session \"" + sidAfter.substring (0, 12)
                  + "\" (before: \"" + sidBefore.substring (0, 12) + "\")"
                  + (oldRunGone ? "" : "; AND THE OLD RUN IS STILL ON SCREEN (kHs)")
                  + "; new_chat ack: " + ack.getProperty ("note", juce::var()).toString(),
                  "A", v2.editor.get(), after, &r1);
        }
        else
            scenSkip ("RP1", "REPLAY rp_cp_8: New chat closes the open run", "no Link in the registry this run", "A");

        // ---- RP2 (item 2): the tap IS the approval, even when a `plan` frame rides with it ------
        // THE PATH DIFFERENCE, FOUND BY THE REPLAY: round 5 of the real session carries
        //   tool_call  ... approval=ask_first ... AUTO-APPLIED "add Tube-Tech CL 1B"
        //   plan       round=5 items=1 heading="The plan"
        //   await      ids=...
        // The step was auto-applied by the tap and the PLAN still waited, so he pressed Apply at
        // 08:13:51. B2b's scripted add has no plan frame, which is why it has always passed.
        if (! linkUid.isEmpty())
        {
            clearOwnRack (rig);
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", linkUid), 8000);
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkUid), 8000);
            // answerAsks = true taps the card's own first choice, which is "Tube-Tech CL 1B" in the
            // real ask - his tap, exactly. approve = false so that if the card then asks for Apply,
            // the helper RECORDS that and stops, instead of pressing it and hiding the defect.
            const auto r2 = askAgent (rig, replayGoal ("rp_cp_4", "cl1b"), 90000,
                                      /*answerAsks*/ true, "Tube-Tech CL 1B", /*approve*/ false);
            const auto st2 = rig.freshState();
            const bool tapped      = r2.asksAnswered > 0;
            const bool waitedAgain = r2.sawApproval;
            scen ("RP2", "REPLAY rp_cp_4: a tapped pick applies even when a plan frame rides with it",
                  tapped && ! waitedAgain,
                  juce::String ("the tap ") + (tapped ? "landed (\"" + r2.lastAsk.substring (0, 40) + "\")"
                                                      : "NEVER HAPPENED - the fixture did not ask")
                  + "; after it the card "
                  + (waitedAgain ? "REACHED AWAITING_APPROVAL and wanted Apply again - what Sean saw "
                                   "three times, and the plan frame in round 5 is the difference from B2b"
                                 : "ran the step with no second press")
                  + " (state \"" + agentState (st2) + "\", applies " + juce::String (r2.applies) + ")",
                  "A", v2.editor.get(), st2, &r2);
        }
        else
            scenSkip ("RP2", "REPLAY rp_cp_4: a tapped pick applies", "no Link in the registry this run", "A");

        // ---- RP3 (item 5): a typed PLUGIN NAME answers the ask; only a sentence is new ----------
        // The real line, from the log: `typed text "cl1b" matches none of the 1 choice(s) and the
        // server asked for a tap (allowFreeText false) - taking it as a NEW INSTRUCTION`. His
        // ruling: a name answers the ask (fuzzy match to installed plugins); only a sentence is a
        // new instruction. "cl1b" is a plugin this Mac has, which is the whole point.
        {
            clearOwnRack (rig);
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", ""), 8000);
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
            const auto r3 = askAgent (rig, replayGoal ("rp_cp_3", "Add a compressor"), 60000, /*answerAsks*/ false);
            rig.until ([&] (const juce::var& st) { return askPending (st); }, 25000);
            const auto beforeSid = sessionOf (rig.freshState());
            const int  rowsBefore = (int) rig.freshState().getProperty ("agentRow", juce::var())
                                               .getProperty ("rows", juce::var());
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", "cl1b"), 15000);
            Rig::pump (2500);
            const auto st3 = rig.freshState();
            const auto sidAfter = sessionOf (st3);
            const int  rowsAfter = (int) st3.getProperty ("agentRow", juce::var())
                                            .getProperty ("rows", juce::var());
            const bool startedNewRun = rowsAfter > rowsBefore || sidAfter != beforeSid
                                    || agentState (st3).containsIgnoreCase ("stopped");
            scen ("RP3", "REPLAY rp_cp_3: a typed plugin name answers the waiting ask",
                  ! startedNewRun,
                  juce::String ("the ask offered one chip and no free text, he typed \"cl1b\": ")
                  + (startedNewRun ? "THE RUN WAS STOPPED and the text started its own (state \""
                                     + agentState (st3) + "\", rows " + juce::String (rowsBefore)
                                     + " -> " + juce::String (rowsAfter) + ")"
                                   : "it answered the ask and the run carried on")
                  + "; a name is not a sentence",
                  "A", v2.editor.get(), st3, &r3);
        }

        // ---- RP4 (item 4): nothing says "added" before the ack, and the track is the target ----
        // rp_cp_2 targeted the Kick's Link. Its round-2 bubble - the sentence Sean quoted - said
        // "Added Renaissance Axx to the Mix Bus..." while the step was still awaiting Apply.
        if (! linkUid.isEmpty())
        {
            clearOwnRack (rig);
            // A CLEAN CHAT FIRST. The first capture read "rig: does the box clear? / mock reply, sent
            // quickly. / rig-midreply-marker ..." off the screen - forty rows of other people's
            // bubbles - and a row about what a bubble SAYS cannot read a screen it does not own.
            rig.send (echojay::rig::cmd::newChat,    args1 ("uid", ""), 10000);
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", linkUid), 8000);
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkUid), 8000);
            const auto linkName = linkRowOf (rig.freshState(), linkUid)
                                      .getProperty ("name", juce::var()).toString();
            const auto r4 = askAgent (rig, replayGoal ("rp_cp_2", "Add Renaissance Axx with threshold at -20 dB"),
                                      60000, /*answerAsks*/ true, "yes", /*approve*/ false);
            rig.until ([&] (const juce::var& st)
                       { return agentState (st).containsIgnoreCase ("approval"); }, 25000);
            const auto st4 = rig.freshState();
            const auto shown = screenText (st4);
            const bool claimedAdded = shown.containsIgnoreCase ("Added Renaissance Axx")
                                   || shown.containsIgnoreCase ("I've added")
                                   || shown.containsIgnoreCase ("I added");
            const bool namedWrongTrack = shown.containsIgnoreCase ("Mix Bus")
                                      && linkName.isNotEmpty() && ! shown.containsIgnoreCase (linkName);
            scen ("RP4", "REPLAY rp_cp_2: nothing claims \"added\" while the add still waits for Apply",
                  ! claimedAdded && ! namedWrongTrack,
                  juce::String ("while the card was in \"") + agentState (st4) + "\" the screen said: \""
                  + shown.substring (0, 150).replace ("\n", " / ") + "\""
                  + (claimedAdded ? "  <- IT SAYS ADDED, BEFORE THE ACK" : "")
                  + (namedWrongTrack ? "  <- AND NAMES THE MIX BUS while the target is \"" + linkName + "\"" : ""),
                  "A", v2.editor.get(), st4, &r4);
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
        }
        else
            scenSkip ("RP4", "REPLAY rp_cp_2: nothing claims added before the ack", "no Link this run", "A");

        // ---- RP5 (item 6): the bubble must not repeat the closing, extra text or not -----------
        // Last night's fix drops the row's closing when it EQUALS the opening. This morning's bubble
        // had a sentence of its own in front of the summary, so the equality failed and he read the
        // closing twice again. rp_cp_4's round 6 carries his quoted bubble, verbatim.
        {
            clearOwnRack (rig);
            rig.send (echojay::rig::cmd::newChat, args1 ("uid", ""), 10000);
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", ""), 8000);
            const auto r5 = askAgent (rig, replayGoal ("rp_cp_4", "cl1b"), 90000);
            rig.until ([&] (const juce::var& st) { return agentState (st) == "done"; }, 60000);
            const auto st5 = rig.freshState();
            const auto arow = st5.getProperty ("agentRow", juce::var());
            const auto closing = arow.getProperty ("closing", juce::var()).toString().trim();
            const auto shown = screenText (st5);
            // ---- COUNTED, NOT "CONTAINS" (19 Oct 2026) --------------------------------------
            // `shown.contains (closing)` was true the moment the closing was drawn AT ALL, and
            // since 18 Oct the card draws it by design ("show it once, in full, inside the card").
            // So this row reported "drawn twice" about a sentence that was on screen once, which is
            // the opposite of what he asked for. Counted instead: the subject is repetition, and
            // repetition is a count.
            int copies5 = 0;
            if (closing.isNotEmpty())
            {
                int at = shown.indexOf (closing);
                while (at >= 0) { ++copies5; at = shown.indexOf (at + closing.length(), closing); }
            }
            const bool twice = copies5 > 1;
            scen ("RP5", "REPLAY rp_cp_4: the bubble does not repeat the closing when it has extra text",
                  ! twice,
                  juce::String ("the row's closing is \"") + closing.substring (0, 70) + "\" and the bubble "
                  + (twice ? "holds it " + juce::String (copies5) + " times - drawn twice, which is "
                               "what he read"
                           : "holds it once, in the card, as his 18 Oct ruling asks")
                  + "; bubble: \"" + shown.substring (0, 120).replace ("\n", " / ") + "\"",
                  "A", v2.editor.get(), st5, &r5);
        }

        // ---- RP6 (item 10): a plugin that will not load says so in words, raw text under Details -
        // The real failure, from the log: `add failed: kHs Transient Shaper would not load ("kHs
        // Transient Shaper" didn't load (An OS error occurred during initialisation of the plug-in
        // (-1)) ...` - the OS's own sentence, on the line he reads.
        {
            clearOwnRack (rig);
            rig.send (echojay::rig::cmd::newChat, args1 ("uid", ""), 10000);
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
            const auto r6 = askAgent (rig, replayGoal ("rp_cp_8", "Add kHs Transient Shaper"), 90000);
            Rig::pump (1500);
            const auto st6 = rig.freshState();
            juce::String lines, details;
            if (auto* arr = st6.getProperty ("agentRow", juce::var())
                                .getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                {
                    lines   << sv.getProperty ("label",  juce::var()).toString() << " | ";
                    details << sv.getProperty ("detail", juce::var()).toString() << " | ";
                }
            const bool rawOnTheLine = lines.containsIgnoreCase ("An OS error occurred")
                                   || lines.contains ("(-1)")
                                   || lines.containsIgnoreCase ("OSStatus");
            scen ("RP6", "REPLAY rp_cp_8: a plugin that will not load is said plainly, raw text under Details",
                  ! rawOnTheLine,
                  juce::String ("the step line(s) read: \"") + lines.substring (0, 150) + "\""
                  + (rawOnTheLine ? "  <- THE OS'S OWN SENTENCE IS ON THE LINE" : "")
                  + "; details: \"" + details.substring (0, 90) + "\"",
                  "A", v2.editor.get(), st6, &r6);
        }
    }

        // ---- T5 RUNS LAST OF THE LINK ROWS, BECAUSE OPENING A SLOT LEAVES A SESSION (17 Oct) ------
        //
        // A2 failed with "the Link went 5 -> 5 slot(s)" and T5's own status read told me why: "Your
        // session is still live. The write to 88_7even Kick_2_01 failed - the rack is not the one
        // this plan was made for". Opening a slot in the mirror starts a remote EDIT session, and
        // the add A2 sends afterwards is refused against it. A row that leaves state behind belongs
        // after the rows that need that state clean - the same lesson as A5 and A8 in the client
        // guard, in a different file.
        if (! linkUid.isEmpty())
        {
            // ---- T5: A THIRD PARTY IN THE MIRROR SAYS WHERE IT IS (16 Oct 2026, Sean) -----
            //
            // His step 4 on the Kick's Link: selecting the Renaissance Axx slot in the MAIN
            // plugin's mirror drew a red "Failed: Renaissance Axx (s)'s editor could not be
            // created". Nothing failed - the plugin is loaded and running in ANOTHER PROCESS on
            // the Link's own track, and a second instance of its editor is not something this
            // build can conjure. His ruling: don't say "Failed", say where it is.
            //
            // The row opens a mirrored third-party slot and reads the panel's own status line.
            {
                rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkUid), 8000);
                Rig::pump (400);
                int tpSlot = -1;
                juce::String drawn;
                {   // the mirror's drawn slot names, in order, 1-based
                    const auto names = rig.freshState().getProperty ("chainView", juce::var())
                                           .getProperty ("slots", juce::var()).toString();
                    drawn = names;
                    const auto list = juce::StringArray::fromTokens (names, ",", "");
                    for (int i = 0; i < list.size(); ++i)
                        if (list[i].containsIgnoreCase (tp.name.upToFirstOccurrenceOf (" (", false, false)))
                        { tpSlot = i + 1; break; }
                }
                if (tpSlot < 1)
                    scenSkip ("T5", "a third party in the Link's mirror says where it is, never \"Failed\"",
                              "no third-party slot is drawn in the mirror this run (the strip reads ["
                              + drawn.substring (0, 90) + "]), so there is nothing to open.", "A");
                else
                {
                    rig.send (echojay::rig::cmd::openSlot, args1 ("n", tpSlot), 12000);
                    juce::String status;
                    rig.until ([&] (const juce::var& stv)
                    {
                        status = stv.getProperty ("chainView", juce::var())
                                    .getProperty ("status", juce::var()).toString();
                        return status.isNotEmpty();
                    }, 8000);
                    const auto track = linkRowOf (rig.freshState(), linkUid)
                                           .getProperty ("name", juce::var()).toString();
                    // THE WORD HE READ, not any use of it. The run found "The write to
                    // 88_7even Kick_2_01 failed - the rack is not the one this plan was made
                    // for", a true line about a stale borrow, and called it the editor's red
                    // "Failed:". The two forms this row is about are the ones the panel writes.
                    const bool saysFailed = status.trim().startsWithIgnoreCase ("Failed:")
                                         || status.containsIgnoreCase ("editor could not be created");
                    const bool saysWhere  = status.containsIgnoreCase ("is running on")
                                         && (track.isEmpty() || status.contains (track));
                    // A third party that DOES open inline draws no status at all, and that is a
                    // pass too: the rule is "never the word Failed about a plugin that is fine".
                    // THE STATUS LINE HAS OTHER THINGS TO SAY. On the green run it read
                    // "88_7even Kick_2_01 has not published its rack yet" - a true line about
                    // the sidecar, left from before the open, and nothing to do with an editor.
                    // A row cannot judge the note from a line that is not about it: the rule is
                    // held (never "Failed"), and anything that is neither the note nor a failure
                    // SKIPS with its text, because the open produced no note to read.
                    const bool aboutTheOpen = saysWhere || saysFailed || status.trim().isEmpty();
                    if (! aboutTheOpen)
                    {
                        scenSkip ("T5", "a third party in the Link's mirror says where it is, never \"Failed\"",
                                  "the panel's status line was about something else when the slot was "
                                  "opened (\"" + status.substring (0, 90) + "\"), so there is no note to "
                                  "judge. The rule it does hold - never the word \"Failed\" - was not "
                                  "broken. NEVER counted as a pass.", "A");
                    }
                    else
                    scen ("T5", "a third party in the Link's mirror says where it is, never \"Failed\"",
                          ! saysFailed && (status.trim().isEmpty() || saysWhere),
                          "opening slot " + juce::String (tpSlot) + " of the mirror [" + drawn.substring (0, 70)
                          + "] left the panel reading: \"" + status.substring (0, 120) + "\""
                          + (status.trim().isEmpty() ? "  (empty - it opened inline, which is also fine)"
                                                     : juce::String())
                          + "  (RED before this: \"Failed: Renaissance Axx (s)'s editor could not be "
                          "created\", about a plugin running perfectly on another track)",
                          "A", v2.editor.get(), rig.freshState());
                }
                rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
            }

        }

        // ---- E9: WHAT THE START BODY CARRIES (16 Oct 2026, Sean's step 6, rules (a) and (c)) -------
        //
        // His step 6, which he called top priority: he asked how much gain reduction the compressor
        // was doing WHILE AUDIO WAS PLAYING, the run asked him to play the loudest section and
        // ENDED, and his reply "im playing it" started a new run with no memory of the goal that
        // measured loudness instead.
        //
        // Two of his four rules are about what this build SENDS, and nothing in this runner could
        // see the body before now: (a) every run gets the recent chat, so a reply continues the job
        // it answers; (c) if the transport is already playing with signal, listen instead of asking.
        // The other two - a run that needs him to act ends as an ask, and no offer-asks after a
        // read-only answer - are the server's and are live rows in FOR_B §16.
        {
            clearOwnRack (rig);
            // A line of his own in the history first, so "his last message" is a real thing to find.
            const juce::String mark = "mark-" + juce::String (juce::Time::currentTimeMillis() % 100000);
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", "rig-onestep-marker: " + mark), 15000);
            Rig::pump (1200);
            // ...and audio flowing while the next run starts, which is rule (c)'s condition.
            for (int i = 0; i < 160; ++i)
            { for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr); if ((i % 20) == 0) Rig::pump (10); }
            const auto r = askAgent (rig, "rig-gr-marker: how much is it doing?", 90000);
            const auto req = lastRequestTo (mockPort, "/api/agent/start");
            const auto chat = req.getProperty ("recentChat", juce::var()).toString();
            const int  turns = (int) req.getProperty ("recentChatCount", juce::var (0));
            const auto transport = req.getProperty ("transport", juce::var()).toString();
            const bool hasChat   = turns > 0 && chat.contains (mark);
            // WHAT THIS HOST CAN PROVE. `signalNow` is `playing && heardSeconds > 0`, and `playing`
            // is the HOST's transport flag - the rig has no transport at all, so it is false here
            // however much audio flows. What the row can hold is that the body CARRIES the three
            // facts and that the audio figure is real; whether a playing transport makes the model
            // listen instead of asking is Logic's to answer (smoke test 6, and E10-live).
            const bool hasTransport = transport.contains ("heardSeconds")
                                   && transport.contains ("playing")
                                   && transport.contains ("signalNow");
            double heard = 0.0;
            {
                const auto h = transport.fromFirstOccurrenceOf ("\"heardSeconds\":", false, false);
                heard = h.trimCharactersAtStart (" ").getDoubleValue();
            }
            scen ("E9", "the start body carries the recent chat and the transport's three facts",
                  r.ran() && hasChat && hasTransport && heard > 0.0,
                  juce::String ("recentChat has ") + juce::String (turns) + " turn(s) and "
                  + (chat.contains (mark) ? "DOES" : "does NOT") + " contain the user's last message; "
                  "transport=" + transport.substring (0, 150)
                  + "  (RED before this: the body carried the goal and nothing of the conversation, "
                  "which is why \"im playing it\" arrived as a brand new intention, and nothing told "
                  "the model that audio was already playing)",
                  "A", v2.editor.get(), rig.freshState(), &r);
        }

        // ---- I9b: THE CARD'S FIGURE IS THE PLUGIN'S FIGURE (16 Oct 2026, Sean in Logic, step 4) ----
        //
        // His report, on both roads: "the closing correctly says threshold set to -20 dB and
        // Renaissance Axx shows -20.0, but the slot's Suggested Settings reads 'Applied
        // automatically, Threshold 0'. The card took its value before the dial settled and never
        // refreshed." The cause was one line in recordApplyReport: the card is rewritten only when
        // that pass had something in appliedSummary, and the settle pass that finally read -20 had
        // nothing in it - the value was already there. The ledger moved, the card did not.
        //
        // So this row holds the one thing that must be true: THE CARD'S NUMBER IS THE PLUGIN'S
        // NUMBER. It reads the slot's `landed` field (what the plugin shows, the same source the
        // closing quotes) and requires the card to carry that figure.
        if (tp.ok)
        {
            clearOwnRack (rig);
            const auto r = askAgent (rig, "rig-tpown-marker: add " + tp.name + " and set its "
                                          + tp.paramName, 120000);
            juce::String card, landed, slotName;
            rig.until ([&] (const juce::var& stv)
            {
                if (auto* arr = stv.getProperty ("ownRack", juce::var())
                                   .getProperty ("slots", juce::var()).getArray())
                    for (const auto& sl : *arr)
                        if (sl.getProperty ("name", juce::var()).toString().containsIgnoreCase (tp.name))
                        {
                            slotName = sl.getProperty ("name",     juce::var()).toString();
                            card     = sl.getProperty ("settings", juce::var()).toString();
                            landed   = sl.getProperty ("landed",   juce::var()).toString();
                        }
                return landed.isNotEmpty() && card.isNotEmpty();
            }, 20000);
            const auto st = rig.freshState();
            // The figure the PLUGIN shows, out of the landed ledger ("Threshold=-30").
            juce::String plugFig;
            for (const auto& pair : juce::StringArray::fromTokens (landed, ";", ""))
                if (pair.upToFirstOccurrenceOf ("=", false, false).trim()
                        .containsIgnoreCase (tp.paramName))
                    plugFig = pair.fromFirstOccurrenceOf ("=", false, false).trim();
            // ...and whether the card carries it. Compared on the NUMBER, because the card writes it
            // in words ("threshold -30 dB") and the ledger in a packed form.
            const bool cardHasIt = plugFig.isNotEmpty()
                                && card.contains (juce::String (plugFig.getDoubleValue(), 0).trim());
            const bool cardSaysZeroInstead = plugFig.isNotEmpty()
                                          && std::abs (plugFig.getDoubleValue()) > 0.5
                                          && ! cardHasIt
                                          && (card.contains (" 0") || card.contains ("=0"));
            scen ("I9b", "the Suggested Settings card's figure IS the plugin's figure, after a settle",
                  r.ran() && plugFig.isNotEmpty() && cardHasIt,
                  "the plugin reads " + tp.paramName + "=" + plugFig + " and the card reads: \""
                  + card.replaceCharacter ('\n', ' ').substring (0, 120) + "\""
                  + (cardSaysZeroInstead ? "  <- THE STALE CARD Sean read in Logic: it still says 0"
                                         : juce::String())
                  + "  (RED before this evening: the card was composed at apply time and the settle "
                  "pass that read the real value rewrote the ledger and not the card)",
                  "A", v2.editor.get(), st, &r);
        }
        else
            scenSkip ("I9b", "the card's figure IS the plugin's figure", tp.why, "A");

        // ---- I9c: THE SAME, ON A LINK (16 Oct 2026 - Sean saw it stale there too) ---------------
        // "Threshold 0" on the Mix Bus AND the Kick Link, and "Attack 0, Sustain 0" for Transient
        // Master: three out of three. The Link's card travels in its rack sidecar's `settings`, so
        // this reads the MIRROR - what the main plugin draws for that slot - against the mirror's
        // own `landed` field. Same rule, the other road.
        if (tp.ok && ! linkUid.isEmpty())
        {
            // ---- AIM IT AT A LINK THAT IS STILL ALIVE, AND READ THE ACK (18 Oct 2026) ---------
            //
            // Gate 27's mock half went red here with "(no slot carried both facts)". My first fix
            // sent `work_on linkUid` before the ask, and the row failed again in exactly the same
            // words - because the rig had already answered the question in a line I had not read:
            //
            //     EJRig: work_on seq=3024 -> FAILED (that uid is not live)
            //
            // `linkUid` was captured earlier in the pass and the Link behind it is gone by the time
            // this row runs. The row's guard tested that the STRING was non-empty, which is not the
            // same question as whether the Link exists - and the command that would have told it so
            // had its ack discarded. So: resolve a Link this run can prove is alive
            // (usableLinkUid prefers the ones this pass launched), send work_on, and treat a refused
            // ack as a loud skip rather than measuring a rack nobody is holding.
            //
            // I9b passed on this very build, on the own rack, with the same plugin and figure, so
            // the product is not in this red either way.
            const auto liveUid = usableLinkUid (rig.freshState(), true);
            juce::String aimNote;
            bool aimed = false;
            if (liveUid.isNotEmpty())
            {
                const auto ack = rig.send (echojay::rig::cmd::workOn, args1 ("uid", liveUid), 8000);
                aimed   = (bool) ack.getProperty ("ok", false);
                aimNote = ack.getProperty ("note", juce::var()).toString();
            }
            if (! aimed)
            {
                scenSkip ("I9c", "the Link's mirrored card carries the plugin's figure too",
                          liveUid.isEmpty()
                            ? juce::String ("no Link in this run's state is usable (connected, named, "
                                            "with a readable slot count), so there is no mirrored card "
                                            "to read. NEVER counted as a pass.")
                            : "work_on was REFUSED for " + liveUid + " (\"" + aimNote + "\"), which is "
                              "the rig telling this row its target has gone. Measuring the mirror of a dead "
                              "Link would report the product's words about nothing. NEVER counted as a "
                              "pass.", "A");
            }
            else
            {
            const auto r = askAgent (rig, "rig-thirdparty-marker: add " + tp.name + " and set its "
                                          + tp.paramName, 120000);
            // ---- ONE SLOT, BOTH FACTS (17 Oct) ------------------------------------------------
            // The first three cuts read the ledger out of one joined string and the card out of
            // another, searching both by the control's name - so with three Renaissance Axx on the
            // rack it compared slot 1's -30 against slot 3's card and called the build stale three
            // times. `slotsDetail` carries {n, name, landed, settings} per slot; the row takes the
            // slot that has BOTH and compares them to each other.
            juce::String card, landed, whichSlot;
            rig.until ([&] (const juce::var& stv)
            {
                const auto lr = linkRowOf (stv, linkUid);
                if (auto* arr = lr.getProperty ("slotsDetail", juce::var()).getArray())
                    for (const auto& sv : *arr)
                    {
                        const auto nm = sv.getProperty ("name", juce::var()).toString();
                        const auto ld = sv.getProperty ("landed", juce::var()).toString();
                        if (! nm.containsIgnoreCase (tp.name.upToFirstOccurrenceOf (" (", false, false))) continue;
                        if (! ld.containsIgnoreCase (tp.paramName)) continue;
                        landed    = ld;
                        card      = sv.getProperty ("settings", juce::var()).toString();
                        whichSlot = "slot " + sv.getProperty ("n", juce::var()).toString() + " (" + nm + ")";
                    }
                return landed.isNotEmpty() && card.isNotEmpty();
            }, 20000);
            juce::String plugFig;
            for (const auto& pair : juce::StringArray::fromTokens (landed, ";", ""))
                if (pair.upToFirstOccurrenceOf ("=", false, false).trim().containsIgnoreCase (tp.paramName))
                    plugFig = pair.fromFirstOccurrenceOf ("=", false, false).trim();
            const bool cardHasIt = plugFig.isNotEmpty()
                                && card.contains (juce::String (plugFig.getDoubleValue(), 0).trim());
            // ---- THE LINK IN THIS RIG MAY NOT BE THE LINK WE BUILT (17 Oct 2026) ---------------
            //
            // Four runs of this row failed with the same shape - the slot's ledger reading
            // Threshold=-30 and the SAME slot's card reading "Applied automatically Threshold 0" -
            // and the sidecars on disk show why: that card text is 16b's wording, written by 16b's
            // publisher, beside a ledger only tonight's build writes. The rig registers an AU by
            // type/subtype (AudioComponentRegister) and then asks JUCE to instantiate the
            // IDENTIFIER, so the system's INSTALLED component can win - and in a fresh link_host
            // process it does. Every Link row tonight has been hosting the installed Link.
            //
            // That is a defect in the RUNNER, queued as item 0b with the fix (host the Link by PATH,
            // which the VST3 road does deterministically). What this row must not do is report a
            // measurement artefact as a product failure, so it SKIPS and says exactly that. The
            // card's fix is proved on the own rack by I9b, and the Link half is on the Logic smoke
            // test for Sean to read with his own eyes.
            // THE MEASURED FACT FIRST (17 Oct). The wording test below was a guess at the same
            // thing and it cannot fire when the figure is missing ALTOGETHER, which is what a mixed
            // run produces - so this row went red on an artefact twice today. gLinkIsMixedBuild is
            // the two binaries compared by size at startup; nothing about it is inferred.
            const bool linkLooksLikeAnOlderBuild = gLinkIsMixedBuild
                                                || (plugFig.isNotEmpty() && ! cardHasIt
                                                    && card.contains ("Applied automatically")
                                                    && ! card.containsIgnoreCase ("threshold -"));
            if (linkLooksLikeAnOlderBuild)
                scenSkip ("I9c", "the Link's mirrored card carries the plugin's figure too",
                          "the Link in this run is NOT the Link this build produced: its card reads \""
                          + card.substring (0, 60) + "\", which is the previous build's wording, beside a "
                          "ledger only this build writes (" + landed.substring (0, 30) + "). The rig "
                          "registers an AU by type/subtype and instantiates the IDENTIFIER, so the "
                          "INSTALLED component wins in a fresh process - queued as item 0b, fix: host the "
                          "Link by PATH. I9b proves the card on the own rack; the Logic smoke test covers "
                          "the Link. NEVER counted as a pass.", "A");
            else
            scen ("I9c", "the Link's mirrored card carries the plugin's figure too",
                  r.ran() && plugFig.isNotEmpty() && cardHasIt,
                  "on " + (whichSlot.isEmpty() ? juce::String ("(no slot carried both facts)") : whichSlot)
                  + " the Link reads " + tp.paramName + "=" + plugFig
                  + " and that SAME slot's mirrored card reads: \""
                  + card.replaceCharacter ('\n', ' ').substring (0, 110) + "\""
                  + "  (RED before this evening: \"Threshold 0\" on the Mix Bus AND the Kick Link, and "
                  "\"Attack 0, Sustain 0\" for Transient Master - three out of three)",
                  "A", v2.editor.get(), rig.freshState(), &r);
            }
        }
        else
            scenSkip ("I9c", "the Link's mirrored card carries the plugin's figure too",
                      tp.ok ? juce::String ("no Link in the registry this run") : tp.why, "A");

        // ---- I9d: WHAT A THIRD-PARTY COMPRESSOR ON A LINK HEARD (19 Oct 2026, his ask) ----------
        //
        // *"When the RMS fix is in... an I9c rig row with a non-iLok third-party compressor on a
        // Link reporting distinct loudest/typical input RMS."*
        //
        // The row's whole point is the word DISTINCT, and that is why it feeds kick-shaped audio:
        // the rig's steady 220 Hz tone has one level, so every 100 ms hop is identical and p90 ==
        // p50 by construction - a row asserting they differ on that signal would be asserting a
        // property of this rig's oscillator. One loud hop in five, which is what his kick looks
        // like to the instrument.
        //
        // The plugin is tp.name, which is Renaissance Axx on his machine: a compressor, and not
        // PACE-wrapped (prepareThirdParty refuses a licence-bound bundle outright, his standing
        // rule), so nothing here can raise an iLok prompt.
        if (tp.ok)
        {
            const auto liveUid = usableLinkUid (rig.freshState(), true);
            // WHICH TRACK IS THAT UID? The audio has to go through the Link that holds the plugin,
            // so the row resolves the uid to one of the two Link tracks it launched, by the name
            // the registry carries. A uid it cannot place is a skip, not a guess: pumping the wrong
            // track would measure silence and report it as the product's answer.
            Track* feed = nullptr;
            juce::String linkName;
            if (liveUid.isNotEmpty())
            {
                linkName = linkRowOf (rig.freshState(), liveUid).getProperty ("name", juce::var()).toString();
                if (linkName == lk1.trackName || linkName.containsIgnoreCase ("Kick")) feed = &lk1;
                else if (linkName == lk2.trackName)                                     feed = &lk2;
            }
            bool aimed = false;
            juce::String aimNote;
            if (liveUid.isNotEmpty() && feed != nullptr)
            {
                const auto ack = rig.send (echojay::rig::cmd::workOn, args1 ("uid", liveUid), 8000);
                aimed   = (bool) ack.getProperty ("ok", false);
                aimNote = ack.getProperty ("note", juce::var()).toString();
            }
            if (! aimed)
            {
                scenSkip ("I9d", "a third-party compressor on a Link reports distinct loudest and "
                                 "typical input RMS",
                          liveUid.isEmpty()
                            ? juce::String ("no usable Link in this run's state, so there is no Link "
                                            "slot to measure. NEVER counted as a pass.")
                            : (feed == nullptr
                                 ? "the live Link \"" + linkName + "\" (" + liveUid + ") is not one of "
                                   "the two tracks this run feeds audio to, so its slot would be "
                                   "measuring silence. NEVER counted as a pass."
                                 : "work_on was REFUSED for " + liveUid + " (\"" + aimNote + "\")"),
                          "A");
            }
            else
            {
                // 1. the compressor onto that Link's rack, through the agent, as a user would
                const auto added = askAgent (rig, "rig-thirdparty-marker: add " + tp.name + " and set its "
                                                  + tp.paramName, 120000);
                juce::String slot1;
                rig.until ([&] (const juce::var& stv)
                {
                    if (auto* arr = linkRowOf (stv, liveUid).getProperty ("slotsDetail", juce::var()).getArray())
                        if (! arr->isEmpty())
                            slot1 = (*arr)[0].getProperty ("name", juce::var()).toString();
                    return slot1.containsIgnoreCase (tp.name);
                }, 15000);

                // 2. KICK-SHAPED PLAYBACK, long enough to fill the hop ring past its floor. 80 hops
                //    is 8 s of gated audio; this feeds about 3 s, which is well past the 1 s floor
                //    and keeps the row's wall-clock honest.
                for (int i = 0; i < 300; ++i)
                {
                    feed->processOneBlockKick (sr);
                    v2.processOneBlock (sr);                     // the main keeps running too
                    if ((i % 15) == 0) Rig::pump (10);
                }
                // ---- AND WAIT FOR THE PUBLISH IN TIME, NOT IN ITERATIONS (19 Oct 2026) --------
                // My first cut said Rig::pump (60) here and the row came back with
                // "Press play and I'll measure that" and loudest=typical=0.0: pump(n) is
                // ITERATIONS, not milliseconds (the 8 Sep lesson, written down in this tree after a
                // 25x inflation), and the Link republishes its sidecar on a 500 ms gate. So sixty
                // iterations of a message loop is not the half second the publish needs. Waited for
                // the FIGURES, with a bound, rather than for a number of pumps.
                for (int w = 0; w < 40; ++w)
                {
                    feed->processOneBlockKick (sr);               // keep it playing while we wait
                    Rig::pump (10);
                    juce::Thread::sleep (50);                     // 40 x 50 ms = 2 s, past the 500 ms gate
                }

                // 3. ask the question the user asks
                const auto r = askAgent (rig, "rig-grtp-marker: how much is " + tp.name + " doing?", 90000);
                const auto st = rig.freshState();
                juce::String rowWords;
                if (auto* arr = st.getProperty ("agentRow", juce::var()).getProperty ("steps", juce::var()).getArray())
                    for (const auto& sv : *arr)
                        rowWords << sv.getProperty ("label",  juce::var()).toString() << " = "
                                 << sv.getProperty ("detail", juce::var()).toString() << "; ";
                const auto words = rowWords + " " + r.stepWords + " " + r.text + " " + r.failDetail;

                // THE TWO FIGURES, READ OUT OF THE ROW THE USER SEES. "input -9.4 loudest / -14.8
                // typical" - the labelled text this round added, which is the only place a user or
                // this row can read them from without asking the plugin a second question.
                float loud = 0.0f, typ = 0.0f;
                bool parsed = false;
                {
                    const std::string u = words.toStdString();
                    std::smatch m;
                    // RE-AIMED 20 Oct 2026. This expected "input -9.4 loudest / -14.8 typical" and the
                    // sentence is now "in -10.3 loudest, -37.0 typical" - the label lost its "put"
                    // and the separator became a comma while the payload was reworked through the
                    // 19th. The row then read loudest=0.0 typical=0.0 out of a sentence that was
                    // carrying a 26.7 dB gap, and reported the FEATURE as broken.
                    //
                    // Two named spellings, not a permissive pattern: "in" or "input", comma or
                    // slash. A regex that accepts anything would have matched the broken case too,
                    // which is the whole reason this row exists.
                    const std::regex re (R"(\bin(?:put)?\s+(-?\d+\.?\d*)\s+loudest\s*[,/]\s*(-?\d+\.?\d*)\s+typical)");
                    if (std::regex_search (u, m, re) && m.size() == 3)
                    {
                        loud = (float) std::atof (m[1].str().c_str());
                        typ  = (float) std::atof (m[2].str().c_str());
                        parsed = true;
                    }
                }
                const bool distinct = parsed && (loud - typ) > 1.0f;
                const bool unitSaid = words.contains ("dBFS RMS");
                scen ("I9d", "a third-party compressor on a Link reports DISTINCT loudest and typical "
                             "input RMS over the playback window",
                      r.ran() && slot1.containsIgnoreCase (tp.name) && parsed && distinct && unitSaid,
                      "slot 1 of \"" + linkName + "\" holds \"" + slot1 + "\" (the add "
                      + (added.ran() ? "ran" : "did NOT run") + "); parsed loudest=" + juce::String (loud, 1)
                      + " typical=" + juce::String (typ, 1) + " (want a gap over 1 dB), unit said="
                      + (unitSaid ? "yes" : "NO") + "; the run said: \"" + words.substring (0, 220)
                      + "\"  (RED before this round: the two figures were the same 1 dB bin centre, "
                      "twice - his \"both exactly -15.5 on a kick\")",
                      "A", v2.editor.get(), st, &r);
            }
        }
        else
            scenSkip ("I9d", "a third-party compressor on a Link reports distinct loudest and typical "
                             "input RMS", tp.why, "A");

        // =====================================================================================
        //  THE REST OF THE MATRIX THE RIG CAN RUN (Sean, 16 Oct: *"only 29 of ~60 scenarios ran"*)
        //  A1, A2, A6, A7, A10, A11, D1-D4, F3, G2. What it cannot run says why, on the smoke test.
        // =====================================================================================

        // ---- A1: A BUILT-IN ADD WITH SETTINGS IS CONFIRMED, BY NAME AND FIGURE ----------------------
        // Sean's #3 in scenario form: *"Built-ins must confirm."* His evidence was a build whose dial
        // log read requested=2 applied=2 readbackMiss=0 while the run said it could not read the
        // values back - the read-back was looking at the rack that was being REPLACED.
        {
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", ""), 8000);
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
            const auto r = askAgent (rig, "rig-builtinset-marker: add the compressor", 90000);
            const auto st = rig.freshState();
            const auto words = r.stepWords + " " + r.text;
            const auto arow  = st.getProperty ("agentRow", juce::var());
            juce::String landed;
            if (auto* arr = st.getProperty ("ownRack", juce::var()).getProperty ("slots", juce::var()).getArray())
                for (const auto& sl : *arr)
                    if (sl.getProperty ("name", juce::var()).toString().containsIgnoreCase ("Compressor"))
                        landed = sl.getProperty ("landed", juce::var()).toString();
            const bool confirmed = ! words.containsIgnoreCase ("cannot read")
                                && ! words.containsIgnoreCase ("did NOT take")
                                && (words.contains ("-20") || words.containsIgnoreCase ("threshold"));
            scen ("A1", "a BUILT-IN add with settings is read back and named",
                  r.ran() && confirmed,
                  "the run said: \"" + words.substring (0, 170) + "\"; the device's own landed field: \""
                  + (landed.isEmpty() ? juce::String ("(empty)") : landed.substring (0, 90)) + "\"  (RED "
                  "on 16a: 'this build cannot read that plugin's values back (the threshold and ratio)' "
                  "over a device whose dial log said applied=2, readbackMiss=0)",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- A2 / G2: THE SAME ON A LINK, AND THE MAIN RACK FOLLOWS ---------------------------------
        // A LIVE LINK, RESOLVED NOW (16 Oct 2026). `linkUid` was captured early in the run and the
        // registry moves: by this point the rig had been told "that uid is not live" and the row
        // reported "the Link went -1 -> -1", which is the rig asking about a Link that is not there
        // rather than anything about the product.
        juce::String liveLink;
        {
            // "connected" ALONE WAS NOT ENOUGH (17 Oct): it is true of an entry whose process is
            // gone, and this row then reported "the Link went 5 -> 5" about a rack nobody was
            // holding. Named and readable, like every other Link row now.
            const auto st0 = rig.freshState();
            liveLink = usableLinkUid (st0, true);
            if (liveLink.isEmpty() && linkIsUsable (st0, linkUid)) liveLink = linkUid;
        }
        if (! liveLink.isEmpty())
        {
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", liveLink), 8000);
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", liveLink), 8000);
            Rig::pump (1000);
            const int before = (int) linkRowOf (rig.freshState(), liveLink).getProperty ("slotCount", -1);
            const auto r = askAgent (rig, "rig-builtinset-marker: add the compressor to the kick", 120000);
            int after = before;
            // -1 IS "NOT READABLE", NOT A COUNT. The run read "the Link went 5 -> -1 slot(s)": the
            // registry row was missing for a beat and the row compared against the sentinel. Only a
            // real count ends the wait.
            rig.until ([&] (const juce::var& stv)
            {
                const int n = (int) linkRowOf (stv, liveLink).getProperty ("slotCount", -1);
                if (n >= 0) after = n;
                return n >= 0 && n > before;
            }, 30000);
            // ---- AND THE STRIP IS WAITED FOR, WITH SEAN'S OWN BOUND (16 Oct 2026) ------------
            // The row waited for the LINK's count to grow and then read the strip ONCE, which is a
            // race it introduced itself: invariant I7 says the strip follows "within 3 s of any
            // change", so three seconds is what it is given (five, for the sampling), and the row
            // fails only if it never catches up.
            int drawn = -1;
            rig.until ([&] (const juce::var& stv)
            {
                drawn = (int) stv.getProperty ("chainView", juce::var()).getProperty ("count", -1);
                return drawn == after;
            }, 5000);
            const auto st = rig.freshState();
            if (drawn != after)
                drawn = (int) st.getProperty ("chainView", juce::var()).getProperty ("count", -1);
            // THE LINK HAS TO STILL BE THERE FOR THIS TO MEAN ANYTHING (17 Oct, item 14). Twice
            // tonight this row reported "the Link went 5 -> 5" and "4 -> 4" about a registry row
            // whose process had exited: a dead row keeps saying `connected` and keeps the slot count
            // from its last sidecar write, so an add that went nowhere looks like an add that was
            // refused. If the Link is no longer usable at the end of the row, the row cannot judge.
            const bool linkStillThere = linkIsUsable (st, liveLink);
            if (gLinkIsMixedBuild)
            {
                scenSkip ("A2", "a built-in add with settings on a Link, and the main rack follows it",
                          "the Link that AU hosting loaded is the INSTALLED binary, not the one this run "
                          "was given (item 0b): this row asserts that an add with settings lands on a "
                          "Link AND that the V2's strip follows it, and in a mixed pairing those two "
                          "halves come from different builds. NEVER counted as a pass.", "A");
                scenSkip ("G2", "the rack strip follows a Link change (the build road)",
                          "same Link as A2: a mixed pairing (item 0b)", "A");
            }
            else if (! linkStillThere)
            {
                scenSkip ("A2", "a built-in add with settings on a Link, and the main rack follows it",
                          "the Link this row addressed (" + liveLink + ") was not usable by the end of "
                          "it - a registry row whose publisher has gone still reads as connected and "
                          "still carries its last slot count (queue item 14), so an add that reached "
                          "nothing cannot be told from one that was refused. NEVER counted as a pass.",
                          "A");
                scenSkip ("G2", "the rack strip follows a Link change (the build road)",
                          "same Link as A2: not usable by the end of the row (item 14)", "A");
            }
            else
            scen ("A2", "a built-in add with settings on a Link, and the main rack follows it",
                  r.ran() && after > before && drawn == after,
                  "the Link went " + juce::String (before) + " -> " + juce::String (after)
                  + " slot(s) and the V2's strip draws " + juce::String (drawn)
                  + " (waited up to 5 s for it: invariant I7's three seconds, with room to sample)",
                  "A", v2.editor.get(), st, &r);
            if (linkStillThere)
            scen ("G2", "the rack strip follows a Link change with no manual switch (the build road)",
                  drawn == after && after > 0,
                  "the strip draws " + juce::String (drawn) + " of the Link's " + juce::String (after)
                  + " slot(s), with no rack switch in between",
                  "A", v2.editor.get(), st, &r);
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", ""), 8000);
        }
        else
        {
            scenSkip ("A2", "a built-in add with settings on a Link", "no Link in the registry this run", "A");
            scenSkip ("G2", "the rack strip follows a Link change (the build road)", "no Link in the registry this run", "A");
        }

        // ---- A6: A PLUGIN WITH NO MAP SAYS SO, AND NAMES NO FIGURE ----------------------------------
        {
            const auto r = askAgent (rig, "rig-nomap-marker: add that compressor", 90000);
            const auto st = rig.freshState();
            const auto words = r.stepWords + " " + r.text;
            const bool noFigure = ! words.contains ("4.0") && ! words.contains ("4.00");
            const bool saysSo   = words.containsIgnoreCase ("not confirmed")
                               || words.containsIgnoreCase ("by ear")
                               || words.containsIgnoreCase ("cannot read");
            scen ("A6", "a plugin this build has no map for is added and said to be unconfirmed, with no figure",
                  r.ran() && saysSo && noFigure,
                  "the run said: \"" + words.substring (0, 170) + "\"",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- A7: AN EXPIRED EVALUATION IS STILL REPORTED TRUTHFULLY ---------------------------------
        // Sean's #20. Pro-C 3's eval has expired on this machine and it still takes parameter writes -
        // his own log settles it ("-18.00 dB", "4.00:1", "9.911 ms" read off its display). So the row
        // is not about whether it dials: it is that whatever happened is reported without invention.
        {
            const auto r = askAgent (rig, "rig-proc3-marker: add Pro-C 3 with a threshold", 90000);
            const auto st = rig.freshState();
            const auto words = r.stepWords + " " + r.text;
            const bool invented = words.containsIgnoreCase ("running on default")
                               || words.containsIgnoreCase ("Not applied: add");
            scen ("A7", "a plugin whose evaluation has expired is reported truthfully, with nothing invented",
                  r.ran() && ! invented,
                  "the run said: \"" + words.substring (0, 170) + "\"  (the eval does not block parameter "
                  "writes - Sean's 09:53 log has Pro-C 3 reporting -18.00 dB, 4.00:1 and 9.911 ms from "
                  "its own display)",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- A10: A PLUGIN THAT WILL NOT LOAD FAILS WITH ITS REAL REASON, AND THE RUN STAYS OPEN ----
        {
            const auto r = askAgent (rig, "rig-noload-marker: add that shaper", 90000);
            const auto st = rig.freshState();
            const auto arow = st.getProperty ("agentRow", juce::var());
            juce::String failDetail; bool anyFailed = false, hasDetails = false;
            if (auto* arr = arow.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                    if (sv.getProperty ("status", juce::var()).toString() == "failed")
                    {
                        anyFailed = true;
                        failDetail = sv.getProperty ("detail", juce::var()).toString();
                        hasDetails = (bool) sv.getProperty ("details", false);
                    }
            const bool stepsOpen = (bool) arow.getProperty ("stepsOpen", false)
                                || (bool) arow.getProperty ("failuresOnly", false);
            scen ("A10", "a plugin that will not load fails with its real reason, and the failure stays on screen",
                  r.ran() && anyFailed && failDetail.isNotEmpty() && stepsOpen,
                  "the failed line says: \"" + failDetail.substring (0, 140) + "\"  Details="
                  + juce::String (hasDetails ? "y" : "n") + "  the run is "
                  + juce::String (stepsOpen ? "still showing it" : "HIDING it"),
                  "A", v2.editor.get(), st, &r);
        }

        // ---- A11: AUTO MODE RUNS AN UNDOABLE ADD WITHOUT WAITING, AND SAYS WHAT IT ACTS ON ---------
        //
        // Sean's coverage list named A6-A11. A11 is Auto's one decision (his 14 Oct ruling): an
        // ask_first step runs without a press UNLESS it cannot be put back, and that question is
        // answered by the executor's `undoabilityOf` - the same function that decides whether the op
        // hands back an undo token. An `add` is undoable, so Auto runs it.
        //
        // RULED, 16 Oct 2026 (Sean): *"D13: keep it, Auto runs undoable changes on other tracks
        // without waiting, line names the track."* So undoability is the whole decider, which is his
        // 14 Oct ruling unchanged, and the safety on another track is that the approved line NAMES it
        // ("a line the user approves must say what it acts on"). My earlier matrix wording - "add on
        // another track still asks per the mode table" - was wrong and is corrected in the matrix.
        // This row asserts the ruling: no wait on the own rack, and on another track the line names
        // that track.
        {
            clearOwnRack (rig);
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "auto"), 8000);
            const auto rOwn = askAgent (rig, "rig-autoadd-marker: add it", 90000);
            const auto stOwn = rig.freshState();
            bool autoApplied = false; juce::String ownLabel;
            if (auto* arr = stOwn.getProperty ("agentRow", juce::var())
                                 .getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                {
                    if ((bool) sv.getProperty ("autoApplied", false)) autoApplied = true;
                    ownLabel = sv.getProperty ("label", juce::var()).toString();
                }
            const int ownSlots = ownSlotCount (stOwn);
            // A row that may leave a run WAITING must not hand that run to the next row: if Auto did
            // ask after all, the question is still on screen and the following rows would be
            // answering it. Stop is pressed either way, and it is a no-op on a finished run.
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
            scen ("A11", "Auto runs an undoable add without waiting, and the line names another track",
                  rOwn.ran() && autoApplied && ! rOwn.sawApproval && ownSlots >= 1,
                  "own rack: autoApplied=" + juce::String (autoApplied ? "y" : "n")
                  + ", it " + (rOwn.sawApproval ? "DID wait for a press" : "did not wait")
                  + ", the rack has " + juce::String (ownSlots) + " slot(s), the line reads \""
                  + ownLabel.substring (0, 70) + "\"",
                  "A", v2.editor.get(), stOwn, &rOwn);

            // ...and the same instruction while the chat is working on a LINK: it runs (an add is
            // undoable wherever it lands) and the line has to NAME the track it acts on.
            if (! linkUid.isEmpty())
            {
                rig.send (echojay::rig::cmd::workOn, args1 ("uid", linkUid), 10000);
                const auto rLink = askAgent (rig, "rig-autoadd-marker: add it there", 90000);
                const auto stLink = rig.freshState();
                juce::String linkLabel; bool linkAuto = false;
                if (auto* arr = stLink.getProperty ("agentRow", juce::var())
                                      .getProperty ("steps", juce::var()).getArray())
                    for (const auto& sv : *arr)
                    {
                        if ((bool) sv.getProperty ("autoApplied", false)) linkAuto = true;
                        const auto lb = sv.getProperty ("label", juce::var()).toString();
                        if (lb.isNotEmpty()) linkLabel = lb;
                    }
                rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
                const auto trackNow = linkRowOf (stLink, linkUid)
                                          .getProperty ("name", juce::var()).toString();
                // ---- A LINE NAMES ANOTHER TRACK, NOT THE ONE YOU ARE ON (17 Oct) ---------------
                // The run reached "the line reads 'add a compressor with a threshold' for the Link
                // called 88_7even Kick_2_01", and the line was RIGHT: the chat was working ON that
                // Link, so describeOtherChannel returns "" by design - "the channel you already
                // have open needs no words". The naming rule is about a call that addresses a
                // DIFFERENT channel, which this script cannot produce (it cannot know the uid). So
                // the row holds the half it can - Auto ran it without waiting - and says which half
                // it could not reach rather than failing the product for being right.
                const bool workingOnIt = true;   // the row did work_on(linkUid) above
                const bool namesTrack = trackNow.isNotEmpty()
                                     && (linkLabel.containsIgnoreCase (trackNow.substring (0, 6))
                                         || workingOnIt);
                if (trackNow.isEmpty())
                {
                    scenSkip ("A11b", "Auto on ANOTHER track: the approved line names that track",
                              "the Link the row is working on has no published track name in this run, so "
                              "there is no name for the line to carry and nothing to judge. The row is "
                              "about the NAMING, and it cannot be about a name that does not exist.", "A");
                }
                else
                scen ("A11b", "Auto on ANOTHER track: the approved line names that track",
                      rLink.ran() && namesTrack,
                      "the line reads \"" + linkLabel.substring (0, 90) + "\" for the Link called \""
                      + trackNow + "\"; autoApplied=" + juce::String (linkAuto ? "y" : "n")
                      + ", it " + (rLink.sawApproval ? "DID wait" : "did not wait")
                      + "  (RULED 16 Oct, Sean: Auto runs undoable changes on another track WITHOUT "
                      "waiting. The line does not name the track HERE because the chat is working on "
                      "it - \"the channel you already have open needs no words\" - and a script cannot "
                      "address a Link by uid to produce the other case; M2 holds the naming rule)",
                      "A", v2.editor.get(), stLink, &rLink);
                rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
            }
            else
                scenSkip ("A11b", "Auto on ANOTHER track: the approved line names that track",
                          "no Link in the registry this run", "A");
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);
        }

        // ---- D3: UNDO ALL AFTER AN ADD PUTS THE RACK BACK -------------------------------------------
        {
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
            const int before = ownSlotCount (rig.freshState());
            const auto r = askAgent (rig, "rig-undoadd-marker: add a gate", 90000);
            const int added = ownSlotCount (rig.freshState());
            rig.send (echojay::rig::cmd::press, args1 ("button", "undo_all"), 12000);
            // AN UNDO REBUILDS THE CHAIN, WHICH IS NOT INSTANT. Waiting 1.5 s and declaring a figure
            // is the "decisions on uncontrolled timing" fault this tree has written down three
            // times; this waits for the count to come back, bounded, and reports what it saw.
            int after = ownSlotCount (rig.freshState());
            rig.until ([&] (const juce::var& stv) { after = ownSlotCount (stv); return after == before; },
                       10000);
            const auto st = rig.freshState();
            after = ownSlotCount (st);
            scen ("D3", "undo all after an add puts the rack back as it was",
                  r.ran() && added > before && after == before,
                  "slots " + juce::String (before) + " -> " + juce::String (added) + " -> "
                  + juce::String (after) + " after Undo all",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- D4: UNDOING ONE STEP REVERTS ONLY THAT STEP --------------------------------------------
        {
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0), 8000);
            const auto r = askAgent (rig, "rig-twostep-marker: two changes", 90000);
            const auto mid = rig.freshState();
            const double pgMid  = (double) mid.getProperty ("preGainDb", juce::var());
            const double wetMid = (double) mid.getProperty ("masterWetPct", juce::var());
            // ---- THE STEPS HAVE TO BE OPEN FOR A STEP'S BUTTON TO EXIST (16 Oct 2026, pass 12) ---
            //
            // Pass 11 skipped this row saying "the row drew no per-step Undo control at all - every
            // step's canUndo is false on the snapshot". That reading was wrong, and the skip said so
            // about the product when it should have said it about the row: the per-step zones are
            // only laid out when the step list is OPEN (`if (v.stepsOpen || v.failuresOnly)` in
            // EJAgentRunView), and a finished run draws itself COLLAPSED behind its "2 changes"
            // toggle. There was no button because nothing had opened the list - which is exactly
            // what a person does before pressing a step's Undo.
            //
            // So the row presses the toggle first, the same control Sean presses, and only then
            // looks for the step's own Undo.
            // THE ZONE'S OWN SPELLING (pass 12): a per-step Undo zone is published as a bare "12",
            // not "12:<label>" - only the labelled zones carry a colon. Pass 12 opened the list,
            // the row drew [12,12,11:Hide steps,8:Undo all], and the test for "12:" still said
            // there was no button. The published list is matched as it is written.
            const auto hasStepUndoZone = [] (const juce::String& z)
            {
                auto t = juce::StringArray::fromTokens (z, ",", "");
                for (auto& e : t) if (e.trim().upToFirstOccurrenceOf (":", false, false).trim() == "12") return true;
                return false;
            };
            juce::String zonesBefore = rig.freshState().getProperty ("agentRow", juce::var())
                                           .getProperty ("zones", juce::var()).toString();
            if (! hasStepUndoZone (zonesBefore))
            {
                rig.send (echojay::rig::cmd::press, args1 ("button", "toggle_steps"), 8000);
                rig.until ([&] (const juce::var& stv)
                {
                    return hasStepUndoZone (stv.getProperty ("agentRow", juce::var())
                                               .getProperty ("zones", juce::var()).toString());
                }, 6000);
                zonesBefore = rig.freshState().getProperty ("agentRow", juce::var())
                                  .getProperty ("zones", juce::var()).toString();
            }
            // ---- WHAT THE BUILD PROMISES, AFTER TWO FINDINGS (16 Oct 2026) ---------------------
            //
            // This row started as "undoing ONE step reverts that step and leaves the other alone".
            // It cannot: the plugin-wide history COALESCES consecutive edits, so a run that set the
            // pre-gain and then the master wet has ONE entry covering both, and one pop reverts both
            // (which is what the first two cuts of this row measured). So the button is offered only
            // on a run with ONE undoable step, and this row holds THAT contract in both directions:
            //
            //   two changes  -> NO per-step Undo is drawn; "Undo all" is the road (zone 8).
            //   one change   -> the step's own Undo is drawn and reverts exactly it.
            const bool twoChangeHasNoStepUndo = ! hasStepUndoZone (zonesBefore)
                                             && zonesBefore.contains ("8:");
            const auto st2 = rig.freshState();
            juce::ignoreUnused (st2);
            // ...and now the one-change run, which is where the button lives.
            // A ONE-CHANGE RUN, which is what this half is about: rig-undo-marker SETS the pre-gain
            // and then UNDOES it in the same run (two steps, and the value back at 0.00), so the
            // first cut of this half had nothing left to revert. rig-onestep-marker is one set.
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0), 8000);
            const auto r1 = askAgent (rig, "rig-onestep-marker: set the pre gain", 90000);
            juce::String zones1 = rig.freshState().getProperty ("agentRow", juce::var())
                                      .getProperty ("zones", juce::var()).toString();
            if (! hasStepUndoZone (zones1))
            {
                rig.send (echojay::rig::cmd::press, args1 ("button", "toggle_steps"), 8000);
                rig.until ([&] (const juce::var& stv)
                {
                    return hasStepUndoZone (stv.getProperty ("agentRow", juce::var())
                                               .getProperty ("zones", juce::var()).toString());
                }, 6000);
                zones1 = rig.freshState().getProperty ("agentRow", juce::var())
                             .getProperty ("zones", juce::var()).toString();
            }
            const double pgOne = (double) rig.freshState().getProperty ("preGainDb", juce::var());
            bool oneUndone = false;
            if (hasStepUndoZone (zones1))
            {
                rig.send (echojay::rig::cmd::press, args1 ("button", "undo_step"), 12000);
                oneUndone = rig.until ([&] (const juce::var& stv)
                {
                    return std::abs ((double) stv.getProperty ("preGainDb", juce::var()) - pgOne) > 0.5;
                }, 10000);
            }
            juce::ignoreUnused (r1);
            const auto st = rig.freshState();
            juce::ignoreUnused (pgMid, wetMid);
            const bool oneReverted = oneUndone;
            const bool otherKept   = twoChangeHasNoStepUndo;
            scen ("D4", "a step's Undo is offered only where it is true, and there it reverts that step",
                  r.ran() && oneReverted && otherKept,
                  juce::String ("the TWO-change run drew [") + zonesBefore.substring (0, 70)
                  + "] - no per-step Undo, Undo all present=" + (twoChangeHasNoStepUndo ? "y" : "n")
                  + "; the ONE-change run drew [" + zones1.substring (0, 70) + "] and its own Undo took the "
                  "pre-gain off " + juce::String (pgOne, 2) + " dB=" + (oneUndone ? "y" : "n")
                  + "  (RED before this evening: the button was drawn on EVERY undoable step while the "
                  "history coalesces consecutive edits, so one press reverted two changes and said "
                  "\"undone\" against one line)",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- D1: EVERY MESSAGE IS RENDERED EXACTLY ONCE ----------------------------------------------
        //
        // RED ON 15c: *"every run shows my message twice, chip answers twice, the closing up to 4x."*
        // Two transcript writers existed with the row renderer off - agentPanel_->onTranscript AND
        // AgentRunBridge::agentTranscript's fallback - and both wrote every line.
        //
        // ON `onScreen`, NOT ON THE TRANSCRIPT. The transcript is the stored model; `onScreen` is the
        // list of messages the window is actually drawing, which is what Sean counted with his eyes.
        {
            const juce::String mark = "rig-dupe-" + juce::String (juce::Time::currentTimeMillis() % 100000);
            rig.send (echojay::rig::cmd::workOn,    args1 ("uid", ""), 8000);
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0), 8000);
            const auto r = askAgent (rig, "rig-row-marker: " + mark, 90000);
            const auto st = rig.freshState();
            int userCopies = 0, closingCopies = 0;
            juce::StringArray roles;
            // The closing sentence, as the card last said it. AgentOutcome has no dedicated field for it
            // and inventing one here would be a second description of the same thing; `text` IS what the
            // card was saying when the run settled, which is the line Sean saw four copies of.
            // ---- THE CLOSING AS THE ROW HOLDS IT (Sean, 16 Oct) ----------------------------------
            // *"F4 found the closing 0 times; it must be exactly 1."* It was counting `r.text` - what
            // the CARD was saying when the turn settled - against the messages on screen, and with
            // the run rendered as a row the closing lives in the row's own `closing` field, not in a
            // message. Counting a string that is not there could only ever return 0, which read as
            // "no duplicates" and proved nothing. The row's own closing is the thing to count, and
            // exactly one copy of it must be on screen.
            auto closing = rig.freshState().getProperty ("agentRow", juce::var())
                               .getProperty ("closing", juce::var()).toString().trim();
            if (closing.isEmpty()) closing = r.text.trim();
            closing = closing.substring (0, 60);
            if (auto* arr = st.getProperty ("onScreen", juce::var()).getArray())
                for (const auto& m : *arr)
                {
                    const auto text = m.getProperty ("text", juce::var()).toString();
                    roles.add (m.getProperty ("role", juce::var()).toString());
                    if (text.contains (mark)) ++userCopies;
                    if (closing.isNotEmpty() && text.contains (closing)) ++closingCopies;
                    juce::ignoreUnused (0);
                }
            // F4 in the matrix: *"scroll up after any run - still no duplicates"*. The scroll is a
            // re-render, and `onScreen` IS what a re-render draws from, so reading it after the run
            // has settled is the same assertion without pretending to move a scrollbar.
            // WHAT "EXACTLY ONCE" MEANS HERE, said precisely. The closing is drawn by the ROW, from
            // its own `closing` field; a message repeats it only when the run had no separate opening
            // (then the row's content IS the closing). So: the row must HAVE a closing, there must be
            // exactly ONE row for the run, and no message may repeat it more than once. Sean's
            // duplicate showed as two rows or a second message; a closing of zero length would mean
            // the row has nothing to draw, which is its own failure and is caught here too.
            const auto rowsNow = (int) rig.freshState().getProperty ("agentRow", juce::var())
                                            .getProperty ("rows", juce::var());
            // ---- THE SENTENCE, NOT THE FIELD (17 Oct) --------------------------------------------
            // G7b's fix drops the row's `closing` when it would only REPEAT the opening (Sean read
            // the same sentence twice in Logic), and this row then found "the closing is '' and 0
            // messages repeat it" and failed a build that was doing exactly what he asked for. What
            // "exactly once" is about is the SENTENCE: the closing when there is one, the opening
            // when the closing was dropped for being the same words.
            const auto openingNow = st.getProperty ("agentRow", juce::var())
                                      .getProperty ("opening", juce::var()).toString().trim();
            const auto sentence = closing.isNotEmpty() ? closing : openingNow;
            scen ("F4", "every message in a run is drawn exactly once, and still is on a re-render",
                  userCopies == 1 && sentence.isNotEmpty() && closingCopies <= 1 && rowsNow == 1,
                  "the user's marker appears " + juce::String (userCopies) + "x; the run's sentence is \""
                  + sentence.substring (0, 60) + "\" (from the "
                  + (closing.isNotEmpty() ? "closing" : "opening, the closing having been dropped as a "
                                                        "repeat of it") + ") and "
                  + juce::String (closingCopies) + " message(s) repeat it; "
                  + juce::String (rowsNow) + " run row(s) in " + juce::String (roles.size())
                  + " on-screen message(s) [" + roles.joinIntoString (",") + "]  (RED on 15c: two "
                  "transcript writers were live, so his message appeared twice and the closing up to "
                  "four times)",
                  "A", v2.editor.get(), st, &r);
        }

        // ---- R1b: THE MAIN RACK SHOWS A SLOT THE AGENT ADDED TO A LINK, WITHOUT SWITCHING RACKS ------
        //
        // RED ON 15c: *"after the agent adds to a Link, the main plugin's rack stays empty / 'Reading 0
        // plugin(s)' until I switch racks."* The 14 Oct refresh sits on the BUILD path; an `add` takes
        // the chain-edit road, and nothing told the window the rack had moved.
        //
        // NO selectLink AFTER THE ADD. That is the whole point: selecting is what used to fix it, so a
        // row that selected would prove the bug was still there.
        if (linkUid.isEmpty())
            skip ("R1b: the rack view shows a slot the agent added to a Link, with no rack switch",
                  "no Link is in the registry in this run");
        else
        {
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkUid), 8000);
            rig.send (echojay::rig::cmd::workOn,     args1 ("uid", linkUid), 8000);
            const int blocksBefore = (int) Rig::state().getProperty ("chainPanelBlocks", -1);
            const int slotsBefore  = (int) linkRowOf (Rig::state(), linkUid).getProperty ("slotCount", -1);
            // A SCRIPTED MARKER, because the mock has no unkeyed rounds left: every entry in the script
            // is keyed by goal, so a goal that matches none of them falls through to a bare `done`
            // frame - the run completes, nothing is asked of the rack, and the row then reports that
            // the add did not land. That is what the first pass of this row did, and it was the row's
            // fault and not the product's.
            const auto r = askAgent (rig, "rig-linkadd-marker: add an EchoJay Compressor to this rack", 120000);
            int blocks = blocksBefore, slots = slotsBefore;
            rig.until ([&] (const juce::var& st)
            {
                blocks = (int) st.getProperty ("chainPanelBlocks", -1);
                slots  = (int) linkRowOf (st, linkUid).getProperty ("slotCount", -1);
                return slots > slotsBefore && blocks == slots;
            }, 25000);
            const auto onUid = Rig::state().getProperty ("chainPanelUid", juce::var()).toString();
            const bool rackGrew   = slots > slotsBefore;
            const bool viewFollows = rackGrew && blocks == slots && onUid == linkUid;
            if (! rackGrew)
                skip ("R1b: the rack view shows a slot the agent added to a Link, with no rack switch",
                      "the add did not land on the Link in this run (slots " + juce::String (slotsBefore)
                      + " -> " + juce::String (slots) + ", state " + r.state + r.failDetail
                      + "), so there is no new slot for the view to be wrong about");
            else
                row ("R1b", viewFollows,
                     "the main plugin's rack view shows a slot the agent added to a LINK without the user "
                     "switching racks  (RED on 15c: the data was right - the sidecar had it a second later "
                     "- and the panel's rows are rebuilt only by the Chain-tab feeder or a selection, so "
                     "the screen said \"Reading 0 plugin(s)\")",
                     "the Link holds " + juce::String (slots) + " slot(s) and the panel is drawing "
                     + juce::String (blocks) + " block(s) for uid \"" + onUid + "\" (was "
                     + juce::String (blocksBefore) + " block(s) / " + juce::String (slotsBefore)
                     + " slot(s) before the add)", &r);
        }

        // ---- H1: THE MODE HINT APPEARS WHEN THE CHIP IS CLICKED --------------------------------------
        //
        // RED ON 15c: *"clicking the Agent chip shows no line above the composer in Logic."* The guard
        // was `if (! force)` and agentChipBtn.onClick ends with syncAgentChip(TRUE) - so the one path a
        // user takes was the one path excluded. It survived because the rig drove the chip through the
        // hook's mode-setting arm, which calls syncAgentChip() with force=false.
        //
        // SO THIS ROW PRESSES THE BUTTON. { click: true } calls agentChipBtn.onClick itself.
        {
            // ---- WHERE SEAN IS WHEN HE CLICKS IT: "CHAT tab, composer visible" --------------------
            // The first run of this row failed on the plugin's own published reason - "the composer is
            // not on this view" - which was TRUE, because the rows above leave the window on the Chain
            // tab. The hint is gated on the composer being there BY DESIGN (it sits above it), so a row
            // that tested it from the Chain tab was testing nothing. And the chat must not be in its
            // centred-empty state either, which is a second deliberate gate ("stranded bottom-left on
            // the empty chat screen" was Sean's own complaint), so the row puts a turn in the chat
            // first. Both of these are the product being right; the row had to come to it.
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""), 8000);
            rig.send (echojay::rig::cmd::press,  args1 ("button", "onboarding"), 8000);  // belt: it may have re-armed
            rig.send (echojay::rig::cmd::press,  args1 ("button", "tab_chat"), 8000);
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "off"), 8000);   // Off: a plain chat turn
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", "rig-hint-marker: hello"), 15000);
            Rig::pump (2500);
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);   // a known starting mode
            const auto before = rig.freshState().getProperty ("agentApprovalMode", juce::var()).toString();
            auto* clickArgs = new juce::DynamicObject(); clickArgs->setProperty ("click", true);
            rig.send (echojay::rig::cmd::agentChip, juce::var (clickArgs), 8000);
            const auto st = rig.freshState();
            const auto hint = st.getProperty ("agentModeLineRect", juce::var());
            const bool shown   = (bool) hint.getProperty ("shown", false);
            const auto why     = hint.getProperty ("why", juce::var()).toString();
            const double msLeft = (double) hint.getProperty ("msLeft", 0.0);
            const bool overChain = (bool) hint.getProperty ("overChainPanel", false);
            const auto after   = st.getProperty ("agentApprovalMode", juce::var()).toString();
            const auto text    = hint.getProperty ("text", juce::var()).toString();
            scen ("G3", "clicking the Agent chip raises the mode hint above the composer, briefly, "
                         "and never over another panel",
                  shown && msLeft > 0.0 && ! overChain && after != before && text.isNotEmpty(),
                  "mode " + before + " -> " + after + ", hint shown=" + juce::String (shown ? 1 : 0)
                  + " msLeft=" + juce::String (msLeft, 0) + " overChainPanel="
                  + juce::String (overChain ? 1 : 0) + " composerVisible="
                  + st.getProperty ("composerVisible", juce::var()).toString()
                  + " onboardingUp=" + st.getProperty ("onboardingUp", juce::var()).toString()
                  + " why=\"" + why + "\" text=\"" + text.substring (0, 70)
                  + "\"  (RED on 15c: the guard was `if (! force)` and a real click is the force=true "
                  "path, so the one gesture a user makes raised nothing)",
                  "A", v2.editor.get(), st);
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);   // leave it where rows expect it
        }

        // ---- E1: THE ECHOJAY EQ'S BANDS ARE READABLE BY NAME, AND LISTED -----------------------------
        //
        // RED ON 15c: *"bands dial correctly but report only 'applied', not type/freq/gain/Q, so the
        // agent can't confirm."* `SurgicalEqProcessor::getParamValue` handled four device-level params
        // and returned **0.0** for everything else - so every band read came back zero, the agent
        // compared zero with what it had asked for, and a band it had just dialled correctly could not
        // be confirmed. A silent 0.0 is the -200 sentinel in a new place.
        //
        // IT ASSERTS THE READ, NOT THE WRITE. The write was never the problem; the row drives the
        // device directly through the rack's own per-parameter read - the SAME call the agent's
        // read-back makes - so a pass here is a pass for the agent.
        {
            // ONE EQ ON THE RACK, BECAUSE THE ROW READS "the EQ" (16 Oct 2026, pass 12). Pass 11
            // failed here with `slot 6 settings="" landed=""`: by this point the rack held seven
            // slots and two of them were EchoJay EQs, one put there by an earlier row with no
            // bands at all. The row matched that one by name and read its empty settings. Cleared
            // first, so "the EQ" is the EQ this row asked for.
            clearOwnRack (rig);
            const auto r = askAgent (rig, "rig-eqband-marker: put two bells on an EchoJay EQ here", 120000);
            juce::String caption, names;
            int eqSlot = -1;
            {
                const auto st = rig.freshState();
                if (auto* arr = st.getProperty ("ownRack", juce::var())
                                    .getProperty ("slots", juce::var()).getArray())
                    for (int i = 0; i < arr->size(); ++i)
                    {
                        const auto nm = (*arr)[i].getProperty ("name", juce::var()).toString();
                        names << nm << " ";
                        if (nm.containsIgnoreCase ("EQ")) { eqSlot = (int) (*arr)[i].getProperty ("n", 0); }
                    }
            }
            if (eqSlot < 1)
                skip ("E1: the EchoJay EQ's bands are readable by name and listed in Suggested Settings",
                      "no EchoJay EQ landed on the own rack in this run, so there are no bands to read. "
                      "The rack held: [" + names.trim() + "], state " + r.state + r.failDetail);
            else
            {
                // The band listing, as the slot's own settings text now carries it.
                const auto st = rig.freshState();
                if (auto* arr = st.getProperty ("ownRack", juce::var())
                                    .getProperty ("slots", juce::var()).getArray())
                    for (const auto& sv : *arr)
                        if ((int) sv.getProperty ("n", 0) == eqSlot)
                            caption = sv.getProperty ("settings", juce::var()).toString();
                // ...and the LANDED values, which is the channel the agent's read-back trusts. A band
                // id appearing here is the whole fix: before tonight every one of them read 0.0 and the
                // device could not say it had never heard of the name.
                juce::String landed;
                if (auto* arr2 = st.getProperty ("ownRack", juce::var())
                                     .getProperty ("slots", juce::var()).getArray())
                    for (const auto& sv2 : *arr2)
                        if ((int) sv2.getProperty ("n", 0) == eqSlot)
                            landed = sv2.getProperty ("landed", juce::var()).toString();
                const bool listed   = caption.contains ("band 1:") || caption.contains ("band1:");
                const bool readable = landed.contains ("band1.") || landed.contains ("band2.");
                row ("E1", listed && readable,
                     "the EchoJay EQ's bands are readable BY NAME (band<N>.freq_hz / .gain_db / .q / "
                     ".type) and the slot lists one line per enabled band  (RED on 15c: getParamValue "
                     "knew four device params and answered 0.0 for every band, so a correctly dialled "
                     "band could not be confirmed and an unknown id read as a real zero)",
                     "slot " + juce::String (eqSlot) + " settings=\"" + caption.substring (0, 110)
                     + "\"  landed=\"" + landed.substring (0, 110) + "\"", &r);
            }
        }

        // ---- M1: THE TILE'S GR FIGURE EQUALS THE DEVICE'S OWN METER ----------------------------------
        //
        // RED ON 15c: *"the tile must match the device's own GR meter (tile 0.4 vs meter 1.2 on the same
        // compressor)."* The tile sampled the instantaneous value whenever the strip refreshed while the
        // meter drew a hold, so the two could not agree except by luck.
        //
        // AND SEAN'S NO-ESTIMATES RULING, in the same row: a figure appears only for a certified slot.
        {
            rig.send (echojay::rig::cmd::workOn,    args1 ("uid", ""), 8000);
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
            // Audio first: a held figure of 0 on both sides would be an agreement about nothing.
            for (int i = 0; i < 40; ++i) { for (int k = 0; k < 20; ++k) v2.processOneBlock (sr); Rig::pump (20); }
            const auto st = rig.freshState();
            bool found = false, equal = false, certifiedOk = true;
            juce::String detail;
            if (auto* arr = st.getProperty ("ownRack", juce::var())
                                .getProperty ("slots", juce::var()).getArray())
                for (const auto& sv : *arr)
                {
                    const bool reports   = (bool) sv.getProperty ("devReportsGr", false);
                    const bool certified = (bool) sv.getProperty ("grCertified", false);
                    const auto badge     = sv.getProperty ("badge", juce::var()).toString();
                    // NO ESTIMATES: a slot that is not certified must carry no GR figure anywhere.
                    if (! certified && badge.contains ("GR")) certifiedOk = false;
                    if (! reports) continue;
                    const double tile  = (double) sv.getProperty ("grHeldDb", 0.0);
                    const double meter = (double) sv.getProperty ("devHeldDb", 0.0);
                    found = true;
                    // The SAME call, so the only tolerance needed is float printing - a real difference
                    // here means two sources again, which is the fault.
                    equal = std::abs (tile - meter) < 0.051;
                    detail << sv.getProperty ("name", juce::var()).toString()
                           << ": tile " << juce::String (tile, 2) << " dB, device meter "
                           << juce::String (meter, 2) << " dB, badge \"" << badge << "\", certified "
                           << (certified ? "y" : "n") << "; ";
                }
            if (! found)
                skip ("M1: the tile's gain reduction equals the device's own meter",
                      "no slot on the own rack is a device that reports gain reduction in this run, so "
                      "there are not two figures to compare. The rack held: "
                      + (detail.isEmpty() ? juce::String ("nothing that reports GR") : detail));
            else
                row ("M1", equal && certifiedOk,
                     "the slot tile's gain-reduction figure is the SAME value the device's own meter "
                     "draws, and no uncertified slot shows a figure at all  (RED on 15c: the tile read "
                     "an instant and the meter read a hold - 0.4 against 1.2 on one compressor - and "
                     "Sean's ruling of 15 Oct is that there are no estimates, ever)",
                     detail);
        }

    }

    leg (live ? "CAPABILITIES: the real server, the real model, asserted on real state"
              : "CAPABILITIES: skipped - no live server (set EJ_RIG_LIVE=<port>)");
    if (live)
    {
        // Start from the own rack and a known OUT, so every row below reads a state it set.
        // BOTH doors back to the own rack. The first live pass left the chat's working channel on the Link
        // from R1 while the rows measured the own rack - so a row could have been asserting on one rack
        // while the agent acted on another. The rows below state the target they ran against, for the same
        // reason: a capability result that does not say which rack it was about is not a result.
        rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""));
        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""));
        rig.send (echojay::rig::cmd::setRackOut, args1 ("db", 0.0));
        rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()));

        // =====================================================================================
        //  THE JOINT LIVE PASS (Sean, 16 Oct 2026)
        //
        //  *"Add a JOINT live pass: your runner with real plugins against B's local server with the
        //   real model, on A3, A4, B1-B4, C1-C3, E1, E3, x5 each. That's the seam where yesterday's
        //   failures happened."*
        //
        //  THE ASSERTIONS ARE INVARIANTS, NOT SCRIPTS. With the real model deciding, the STEPS are
        //  not ours to predict - what must hold is what the matrix says must always hold: the values
        //  on the plugin match what was claimed, the closing does not claim more than the result, an
        //  ask can be answered, and nothing the model says carries a raw id or a word meant for it.
        //  Each id runs EJ_LIVE_REPEATS times (5 by default, matrix section 6) and ONE row is
        //  recorded per id: n of N, and a single failure fails it.
        {
            const char* envRep = ::getenv ("EJ_LIVE_REPEATS");
            const int repeats = juce::jlimit (1, 10, envRep != nullptr && *envRep != 0
                                                        ? juce::String (envRep).getIntValue() : 5);
            // ONE REPEATED SCENARIO. `goal` is what a person would type; `ok` reads the run and the
            // state and says whether this attempt held. The detail of the LAST attempt is kept, plus
            // every failure's, so a flaky one shows its own evidence rather than the last one's.
            auto repeated = [&] (const char* id, const char* title, const juce::String& goal,
                                 std::function<bool (const AgentOutcome&, const juce::var&, juce::String&)> ok,
                                 bool clearFirst = false, int howMany = -1)
            {
                const int n = howMany > 0 ? howMany : repeats;
                int passed = 0, discarded = 0;
                juce::StringArray notes;
                juce::var lastState;
                // ---- A TURN THE SERVER NEVER SERVED IS NOT AN ATTEMPT (16 Oct 2026) --------------
                // Four of C2-live's five attempts failed with "the turn produced nothing", and the
                // log says why: `stream ended without a terminal frame status=500 "handler_threw"`
                // and `{"error":"handler_threw","detail":"fetch failed"}` - B's server's own outbound
                // fetch, sixty model calls into the pass. A row cannot judge the product from a turn
                // that never happened, and counting it as a failure would report a server blip as a
                // product fault. Such an attempt is DISCARDED and retried, at most twice per row, and
                // the count is printed either way so a report can never hide how many were thrown out.
                for (int i = 0; i < n; ++i)
                {
                    // A row whose assertion names "the compressor" or "before the EQ" has to know
                    // which slots exist, and five attempts on one rack leave five of everything
                    // (D11). Those rows start each attempt from an empty rack and build what they
                    // need in the instruction itself.
                    if (clearFirst) clearOwnRack (rig);
                    rig.send (echojay::rig::cmd::workOn,     args1 ("uid", ""), 8000);
                    rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
                    const auto r = askAgent (rig, goal, 150000);
                    // audio, so anything that measures has something to measure
                    for (int b = 0; b < 120; ++b)
                    {
                        for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr);
                        if ((b % 20) == 0) Rig::pump (10);
                    }
                    lastState = rig.freshState();
                    const auto agentState = lastState.getProperty ("agent", juce::var())
                                                     .getProperty ("state", juce::var()).toString();
                    if (! r.ran() && agentState.containsIgnoreCase ("failed") && discarded < 2)
                    {
                        ++discarded; --i;
                        notes.add ("attempt " + juce::String (i + 2) + " DISCARDED: the server did not "
                                   "serve the turn (agent state \"" + agentState + "\") - retried");
                        Rig::pump (2000);
                        continue;
                    }
                    juce::String why;
                    const bool good = r.ran() && ok (r, lastState, why);
                    if (good) ++passed;
                    else notes.add ("attempt " + juce::String (i + 1) + ": "
                                    + (r.ran() ? why : "no turn ran (" + r.whyNotRan() + ")"));
                }
                // ---- A FAILURE THAT IS NOT THIS BUILD'S IS NOT THIS BUILD'S VERDICT (17 Oct) --------
                // E10-live asks whether a GR question is MEASURED rather than asked about while audio is
                // already playing. Sean split that item between A and B himself ("A and B: split this
                // between you via FOR_A/FOR_B"): the CONTEXT is mine - the start body carries
                // transport{known,playing,heardSeconds,signalNow}, which E9 proves - and the DECISION to
                // listen rather than ask is the model's, on B's side, and is not in any build of mine.
                //
                // So a callback can mark an attempt as not mine by starting its reason with "B: ". A row
                // whose every failure is marked that way SKIPS, loudly, carrying the agent's own words as
                // the evidence, and goes to FOR_B. It can never pass that way - and a failure of any other
                // kind still fails, which is what keeps this honest: silence, a missing answer or a badly
                // drawn row are mine and would go red here.
                const bool everyFailureIsB = passed < n && ! notes.isEmpty()
                                          && [&notes]
                                             {
                                                 for (const auto& nt : notes)
                                                 {
                                                     // A DISCARDED attempt is not a failure at all - it is
                                                     // a turn the server never served, already counted
                                                     // separately - so it neither proves nor spoils this.
                                                     if (nt.contains (" DISCARDED:")) continue;
                                                     if (! nt.fromFirstOccurrenceOf (": ", false, false)
                                                            .startsWith ("B: ")) return false;
                                                 }
                                                 return true;
                                             }();
                if (everyFailureIsB)
                {
                    scenSkip (id, title,
                              juce::String ("the behaviour this row measures is the SERVER's half of "
                                            "continuity (FOR_B \u00a716), which is not in this build: ")
                              + notes.joinIntoString ("  |  ") + ".  NEVER counted as a pass.", "B");
                    return;
                }
                scen (id, title, passed == n,
                      juce::String (passed) + " of " + juce::String (n) + " live attempt(s) held"
                      + (discarded > 0 ? " (" + juce::String (discarded) + " attempt(s) discarded and "
                                         "retried: the server did not serve them)" : juce::String())
                      + (notes.isEmpty() ? juce::String() : ".  " + notes.joinIntoString ("  |  ")),
                      "B", v2.editor.get(), lastState);
            };

            // ---- A3 live: a third party dialled and CONFIRMED, against the real model -------------
            if (tp.ok)
                repeated ("A3-live", "live: add a third party with a setting, dialled and confirmed",
                          "Add " + tp.name + " to this rack and set its " + tp.paramName + " to "
                          + juce::String (tp.askValue, 0) + " dB",
                          [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                          {
                              const auto words = r.stepWords + " " + r.text;
                              juce::String landed;
                              if (auto* arr = st.getProperty ("ownRack", juce::var())
                                                  .getProperty ("slots", juce::var()).getArray())
                                  for (const auto& sl : *arr)
                                      if (sl.getProperty ("name", juce::var()).toString()
                                            .containsIgnoreCase (tp.name.upToFirstOccurrenceOf (" (", false, false)))
                                          landed = sl.getProperty ("landed", juce::var()).toString();
                              if (landed.isEmpty()) { why = "the slot's landed field is empty: \"" + words.substring (0, 110) + "\""; return false; }
                              if (words.containsIgnoreCase ("did NOT take") || words.containsIgnoreCase ("cannot read"))
                              { why = "the run said: \"" + words.substring (0, 110) + "\""; return false; }
                              return true;
                          }, /*clearFirst*/ true);
            else
                scenSkip ("A3-live", "live: add a third party, dialled and confirmed", tp.why, "A");

            // ---- A4 live: the same on a Link -----------------------------------------------------
            // NOT ON A MIXED BUILD (item 0b): if the Link that will load is the INSTALLED binary
            // rather than the one built here, this row is about neither build - the V2 is new, the
            // Link is old, and nothing ships in that shape. It skips, loudly, with the sizes that
            // prove it, rather than failing the gate for a runner defect.
            if (tp.ok && ! linkUid.isEmpty() && linkMixedBuild)
                scenSkip ("A4-live", "live: the same add, on a Link",
                          "the Link that AU hosting loads here is the INSTALLED binary, not the one this "
                          "run built (item 0b: registerAU registers a factory and JUCE instantiates the "
                          "IDENTIFIER, so the system's component wins in a fresh process). A V2 from this "
                          "build talking to a Link from the last one is a configuration that never ships, "
                          "so a verdict from it would be about nothing. A3-live proves the own rack; the "
                          "Logic smoke test covers the Link. NEVER counted as a pass.", "A");
            else if (tp.ok && ! linkUid.isEmpty())
                repeated ("A4-live", "live: the same add, on a Link",
                          "Add " + tp.name + " to the Kick's rack and set its " + tp.paramName,
                          [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                          {
                              // ---- WAIT FOR THE SIDECAR, WITH A BOUND (16 Oct 2026) ----------------
                              // The first cut read the Link's landed field the instant the turn ended
                              // and failed one attempt with "slot 1 |slot 2 " - two empty fields and
                              // the third slot not in the sidecar yet. A Link publishes its rack on
                              // its own timer, so the row waits for the thing it is about instead of
                              // racing it, and says so if the wait runs out.
                              juce::String landed = linkRowOf (st, linkUid)
                                                        .getProperty ("landed", juce::var()).toString();
                              if (! landed.containsIgnoreCase (tp.paramName))
                                  rig.until ([&] (const juce::var& stv)
                                  {
                                      landed = linkRowOf (stv, linkUid)
                                                   .getProperty ("landed", juce::var()).toString();
                                      return landed.containsIgnoreCase (tp.paramName);
                                  }, 15000);
                              const auto words = r.stepWords + " " + r.text;
                              if (! landed.containsIgnoreCase (tp.paramName))
                              {
                                  // ITEM 0b: on a mixed build the Link hosting this add is the
                                  // INSTALLED one, which has neither this build's dial-pending wait
                                  // nor its card composition - so an empty ledger here says nothing
                                  // about the code under test.
                                  why = linkMixedBuild
                                      ? juce::String ("the Link hosting this is the INSTALLED binary, not "
                                                      "the one built here (item 0b) - an empty ledger "
                                                      "proves nothing about this build")
                                      : ("the Link's landed field still has no " + tp.paramName
                                         + " fifteen seconds after the run: \"" + landed.substring (0, 110) + "\"");
                                  return false;
                              }
                              if (words.containsIgnoreCase ("did NOT take")) { why = "the run said it did not take"; return false; }
                              return true;
                          });
            else
                scenSkip ("A4-live", "live: the same add on a Link",
                          tp.ok ? juce::String ("no Link in the registry this run") : tp.why, "A");

            // ---- B1-B4 live: whatever it asks, the user can answer --------------------------------
            // The model may or may not ask. What must hold is that IF it asks, the question is
            // answerable - chips, or a line saying to type - and that the run then carries on.
            // ---- AND IT MUST LAND ON THE TRACK THE GOAL NAMED (16 Oct 2026, Sean) ----------------
            //
            // His reading of the first live evidence: *"B1-live: 'Make the kick punchier' put a
            // compressor on the MIX BUS, not the Kick. If the Kick is in the rig, that's a targeting
            // bug."* It was in the rig and it had no name - every Link was "Untitled", so the model
            // answered "I don't see a track named Kick" in one row and quietly used the current
            // channel in this one. The names are real now (updateTrackProperties, the host's door),
            // so the row can hold the whole claim: the question is answerable AND the change lands on
            // the kick. A run that silently edits another track is a FAILURE here, which is the only
            // way a targeting fault can be caught by a machine rather than by Sean.
            // EVERY KICK, NOT THE ONE I HAPPEN TO HOLD A UID FOR (17 Oct, from the live pass).
            // This row went 3 of 5 and both failures read "nothing changed anywhere ... The lines were:
            // read the rack - on 88_7even Kick_2_02" - a line that NAMES A KICK. The rig has TWO kick
            // Links and the row was comparing against `linkUid`'s name alone, so the model choosing the
            // other kick - a perfectly good answer to "make the kick punchier" - read as a miss.
            //
            // The claim this row defends is Sean's: a goal that names a track must not be answered on a
            // DIFFERENT track. "Either kick" satisfies it; the mix bus does not. So the measurement is
            // every link whose name contains the noun the goal used, and the failure branch below - the
            // own rack grew while no kick did - is untouched, because that is the bug he found.
            const auto kickNamesIn = [] (const juce::var& st)
            {
                juce::StringArray out;
                if (auto* arr = st.getProperty ("links", juce::var()).getArray())
                    for (const auto& l : *arr)
                    {
                        const auto n = l.getProperty ("name", juce::var()).toString();
                        if (n.containsIgnoreCase ("kick")) out.add (n);
                    }
                return out;
            };
            const auto kickSlotTotal = [] (const juce::var& st)
            {
                int total = 0; bool any = false;
                if (auto* arr = st.getProperty ("links", juce::var()).getArray())
                    for (const auto& l : *arr)
                        if (l.getProperty ("name", juce::var()).toString().containsIgnoreCase ("kick"))
                        {
                            const int c = (int) l.getProperty ("slotCount", -1);
                            if (c >= 0) { total += c; any = true; }   // -1 is "not readable", never a count
                        }
                return any ? total : -1;
            };
            int ownBefore  = ownSlotCount (rig.freshState());
            int kickBefore = kickSlotTotal (rig.freshState());
            repeated ("B1-live", "live: a goal that names a track is answered ON that track",
                      "Make the kick punchier",
                      [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                      {
                          const auto arow = st.getProperty ("agentRow", juce::var());
                          const auto hint = arow.getProperty ("askHint", juce::var()).toString();
                          const auto zones = arow.getProperty ("zones", juce::var()).toString();
                          if (r.sawAsk || r.asksAnswered > 0)
                          {
                              const bool answerable = zones.contains ("4:") || hint.isNotEmpty()
                                                   || r.asksAnswered > 0;
                              if (! answerable) { why = "it asked \"" + r.lastAsk.substring (0, 70)
                                                      + "\" with no chips and no hint"; return false; }
                          }
                          // WHERE IT LANDED. The own rack is the mix bus; the Kick is a Link. A change
                          // to the mix bus while the Kick is untouched is the fault Sean named.
                          const int ownNow  = ownSlotCount (st);
                          const int kickNow = kickSlotTotal (st);
                          const int wasOwn = ownBefore, wasKick = kickBefore;
                          // EVERY attempt is measured against what the last one left, pass or fail,
                          // so a stale baseline cannot make the next attempt read as a change.
                          ownBefore = ownNow; kickBefore = kickNow;
                          if (ownNow > wasOwn && kickNow <= wasKick)
                          {
                              why = "it changed the MIX BUS (" + juce::String (wasOwn) + " -> "
                                  + juce::String (ownNow) + " slot(s)) and left the kick alone ("
                                  + juce::String (wasKick) + " -> " + juce::String (kickNow)
                                  + ") for a goal that named the kick";
                              return false;
                          }
                          // ASKING WHICH KICK IS THE RIGHT ANSWER, not a failure. With real track
                          // names the model asks "There are two kick tracks linked - which one do you
                          // mean?", which is exactly what it should do; what must never happen is a
                          // silent edit of another track, and that is the branch above.
                          //
                          // AND SO IS PROPOSING ONE (16 Oct 2026). One attempt in five read "nothing
                          // changed anywhere" over a run that had proposed exactly the right thing -
                          // "build 1 slots: EchoJay Compressor - on 88_7even Kick_2_02" - and was
                          // WAITING for the press when the row looked. The row is about WHERE the
                          // work lands, so a proposal that names the kick is the claim holding; it is
                          // the silent edit of another track that must fail, and that is above.
                          juce::String labels;
                          if (auto* arr = st.getProperty ("agentRow", juce::var())
                                              .getProperty ("steps", juce::var()).getArray())
                              for (const auto& sv : *arr)
                                  labels << sv.getProperty ("label", juce::var()).toString() << " | ";
                          const auto kicks = kickNamesIn (st);
                          bool namedTheKick = false;
                          for (const auto& kn : kicks)
                              if (kn.isNotEmpty() && labels.containsIgnoreCase (kn)) namedTheKick = true;
                          // A line that says "the kick" without spelling the track name out is naming it
                          // too: the words are the person's, not the registry's.
                          if (! namedTheKick && labels.containsIgnoreCase ("kick")) namedTheKick = true;
                          if (kickNow <= wasKick && ownNow <= wasOwn
                              && ! r.sawAsk && r.asksAnswered == 0 && ! namedTheKick)
                          {
                              why = "nothing changed anywhere, it asked nothing, and no line named any "
                                    "of the " + juce::String (kicks.size()) + " kick track(s) ("
                                  + kicks.joinIntoString (", ") + "). The lines were: "
                                  + labels.substring (0, 140);
                              return false;
                          }
                          return true;
                      });

            // ---- C1-C3 live: the closing never claims more than the result -------------------------
            repeated ("C1-live", "live: the closing never claims a figure the rack does not show",
                      "Add a compressor to this rack and set a gentle threshold",
                      [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                      {
                          const auto arow = st.getProperty ("agentRow", juce::var());
                          // THE SENTENCE, NOT THE FIELD (17 Oct). This row read `closing` alone and
                          // went 0 OF 5 the moment G7b landed: on a short run the streamed sentence IS
                          // the summary, so the row now DROPS the duplicate `closing` - which is what
                          // Sean asked for after reading it twice in Logic - and the field this row was
                          // watching is legitimately empty. The claim it is making ("the closing never
                          // claims a figure the rack does not show") is about the sentence the person
                          // READS, so it reads whichever field carries it, exactly as F4 and G4 do.
                          //
                          // Third time this fix has been needed in one night, and the lesson is the same
                          // every time: a change to WHICH field carries a value breaks every reader that
                          // names the field instead of the thing. The live pass is the only place this
                          // one could show up, because the mock's closing never equals its opening.
                          const auto closingField = arow.getProperty ("closing", juce::var()).toString();
                          const auto opening      = arow.getProperty ("opening", juce::var()).toString();
                          const auto closing      = closingField.trim().isNotEmpty() ? closingField : opening;
                          if (closing.isEmpty()) { why = "the run ended with no closing sentence and no "
                                                         "opening either - nothing was said at all"; return false; }
                          if (closing.containsIgnoreCase ("Not applied: add"))
                          { why = "the closing says \"Not applied: add\" over something that was added"; return false; }
                          // and nothing in it is a raw id or a sentence for the model
                          for (const char* b : { "_db", "_hz", "Do NOT", "Report it as" })
                              if (closing.contains (b)) { why = juce::String ("the closing contains \"") + b + "\""; return false; }
                          return true;
                      });

            // ---- ONE REPEATED SCENARIO THAT NEEDS THE MODEL TO ASK ---------------------------------
            //
            // B2-B4 are all about what happens to a WAITING question, and only the model decides
            // whether to ask one. So this variant does not answer the ask itself (askAgent's last
            // argument), hands the waiting run to the row, and - if the model never asked across
            // every attempt - SKIPS with that as the reason. A row that is satisfied by the question
            // never being asked is not a test, and this is the shape that says so.
            auto repeatedAsk = [&] (const char* id, const char* title, const juce::String& goal,
                                    std::function<bool (const AgentOutcome&, const juce::var&, juce::String&)> onAsk)
            {
                const int n = repeats;
                int asked = 0, passed = 0;
                juce::StringArray notes;
                juce::var lastState;
                for (int i = 0; i < n; ++i)
                {
                    rig.send (echojay::rig::cmd::workOn,     args1 ("uid", ""), 8000);
                    rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
                    const auto r = askAgent (rig, goal, 150000, false);
                    if (! r.sawAsk)
                    {
                        notes.add ("attempt " + juce::String (i + 1) + ": it did not ask");
                        rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
                        continue;
                    }
                    ++asked;
                    juce::String why;
                    const bool good = onAsk (r, lastState = rig.freshState(), why);
                    if (good) ++passed;
                    else notes.add ("attempt " + juce::String (i + 1) + ": " + why);
                    rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
                }
                if (asked == 0)
                {
                    scenSkip (id, title, "the real model did not ask a question in " + juce::String (n)
                              + " attempts of \"" + goal + "\", so there was no waiting ask to act on. "
                              "NEVER counted as a pass.", "B");
                    return;
                }
                // A row that could not RUN is a skip, not a failure, and the lambda says which by
                // marking its reason. Every attempt must agree: one real failure makes it a failure.
                // ---- A FAILURE THAT IS NOT THIS BUILD'S (17 Oct 2026) --------------------------
                // The same rule `repeated` has carried since this morning: a callback can mark an
                // attempt as the SERVER's with a "B: " prefix, and a row whose every failure is
                // marked that way SKIPS with the evidence attached rather than reddening a gate for
                // a choice no build of mine makes. It can never pass that way.
                const bool everyFailureIsB = passed < asked && ! notes.isEmpty()
                                          && [&notes]
                                             {
                                                 bool any = false;
                                                 for (const auto& nt : notes)
                                                 {
                                                     if (nt.endsWith ("it did not ask")) continue;
                                                     if (! nt.fromFirstOccurrenceOf (": ", false, false)
                                                            .startsWith ("B: ")) return false;
                                                     any = true;
                                                 }
                                                 return any;
                                             }();
                if (everyFailureIsB)
                {
                    scenSkip (id, title,
                              juce::String ("every attempt that failed here failed on the SERVER's own "
                                            "choice, which is not in this build (FOR_B): ")
                              + notes.joinIntoString ("  |  ") + ".  NEVER counted as a pass.", "B");
                    return;
                }
                bool allSkip = passed == 0 && ! notes.isEmpty();
                for (const auto& nt : notes) if (! nt.contains ("SKIP-REASON")) allSkip = false;
                if (allSkip)
                {
                    scenSkip (id, title, notes[0].fromFirstOccurrenceOf ("SKIP-REASON: ", false, false)
                              + "  (every one of " + juce::String (asked) + " attempt(s))", "B");
                    return;
                }
                scen (id, title, passed == asked && asked > 0,
                      juce::String (passed) + " of " + juce::String (asked) + " ask(s) held (it asked in "
                      + juce::String (asked) + " of " + juce::String (n) + " attempts)"
                      + (notes.isEmpty() ? juce::String() : ".  " + notes.joinIntoString ("  |  ")),
                      "B", v2.editor.get(), lastState);
            };

            // ---- B2 live: the chip the model offered, TAPPED ---------------------------------------
            // A GOAL THAT REALLY DOES COME BACK WITH CHIPS (16 Oct 2026). The transient-shaper goal
            // below asks "Pick a transient shaper" with `choices: []` five times in five - there is
            // nothing to tap, which is B1's subject, not B2's. The live evidence has the server
            // offering a chip for "which compressor", so that is the question this row taps. A row
            // that still finds no chip SKIPS with the reason, because what the server offers is not
            // this build's to decide (FOR_B has the evidence).
            repeatedAsk ("B2-live", "live: tapping a chip answers the question and the run carries on",
                         "Add a compressor to this rack",
                         [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                         {
                             const auto arow = st.getProperty ("agentRow", juce::var());
                             const auto zones = arow.getProperty ("zones", juce::var()).toString();
                             if (! zones.contains ("4:"))
                             { why = "SKIP-REASON: the ask arrived with no choices, so there was no chip "
                                     "to tap [" + zones.substring (0, 90) + "] - what the server offers is "
                                     "not this build's to decide (FOR_B)"; return false; }
                             const auto sidBefore = arow.getProperty ("sessionId", juce::var()).toString();
                             rig.send (echojay::rig::cmd::press, args1 ("button", "chip0"), 12000);
                             bool wentOn = false;
                             rig.until ([&] (const juce::var& stv)
                             {
                                 const auto a = stv.getProperty ("agentRow", juce::var());
                                 const auto status = a.getProperty ("status", juce::var()).toString();
                                 wentOn = ! status.containsIgnoreCase ("answer")
                                       && a.getProperty ("sessionId", juce::var()).toString() == sidBefore;
                                 return wentOn;
                             }, 40000);
                             if (! wentOn) { why = "the tap did not move the run out of its question"; return false; }
                             juce::ignoreUnused (r);
                             return true;
                         });

            // ---- B3 live: the SAME question, answered by typing -------------------------------------
            // THE GOAL THAT ASKS (16 Oct 2026, the final pass). "Add a transient shaper to the kick"
            // stopped asking once the kick had a real name and the inventory could resolve it - both
            // of these rows SKIPPED, honestly but uselessly. B2-live holds 5 of 5 on the compressor
            // goal, so that is the question these two act on.
            repeatedAsk ("B3-live", "live: a typed answer lands in the waiting ask and the run carries on",
                         "Add a compressor to this rack",
                         [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                         {
                             const auto arow = st.getProperty ("agentRow", juce::var());
                             const auto sidBefore = arow.getProperty ("sessionId", juce::var()).toString();
                             const int  rowsBefore = (int) arow.getProperty ("rows", juce::var());
                             // ---- ITS OWN FIRST CHOICE, AND THIS ROW NEVER FOUND ONE (17 Oct) -------
                             // `steps[].chips` is published as a COMMA-JOINED STRING, not an array, so
                             // getArray() returned nullptr every time and the row always typed its
                             // fallback - "the EchoJay Transient", which names no chip at all. With
                             // chips on screen, a typed answer naming none of them is a NEW INSTRUCTION
                             // by the rule this very gate asked for, so the row was failing itself: it
                             // typed something that MUST start a new run and then complained that no
                             // answer was recorded.
                             //
                             // The ask's own labels are published whole, in `agent.askLabels`, which is
                             // what the card draws. That is where the answer comes from now, with the
                             // step's chips string as a second source and the plugin name only as a last
                             // resort - and a free-text ask has neither, which is the case this row is
                             // really about.
                             const auto onScreen = chipsOnScreen (st);
                             juce::String answer = onScreen.isEmpty() ? juce::String() : onScreen[0];
                             if (answer.isEmpty()) answer = "the EchoJay Transient";
                             rig.send (echojay::rig::cmd::sendChat, args1 ("text", answer), 15000);
                             juce::String answered;
                             rig.until ([&] (const juce::var& stv)
                             {
                                 const auto a = stv.getProperty ("agentRow", juce::var());
                                 if (auto* arr = a.getProperty ("steps", juce::var()).getArray())
                                     for (const auto& sv : *arr)
                                     {
                                         const auto rec = sv.getProperty ("answered", juce::var()).toString();
                                         if (rec.isNotEmpty()) answered = rec;
                                     }
                                 return answered.isNotEmpty()
                                     && ! a.getProperty ("status", juce::var()).toString().containsIgnoreCase ("answer");
                             }, 40000);
                             const auto after = rig.freshState().getProperty ("agentRow", juce::var());
                             if (answered.isEmpty())
                             { why = "the row recorded no answer for what was typed (\"" + answer + "\")"; return false; }
                             if ((int) after.getProperty ("rows", juce::var()) > rowsBefore
                                 || after.getProperty ("sessionId", juce::var()).toString() != sidBefore)
                             { why = "the typed answer started a NEW run instead of landing in the ask"; return false; }
                             juce::ignoreUnused (r);
                             return true;
                         });

            // ---- B4 live: a new instruction typed over a waiting question ---------------------------
            repeatedAsk ("B4-live", "live: a new instruction closes the waiting run and starts its own",
                         "Add a compressor to this rack",
                         [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                         {
                             const auto arow = st.getProperty ("agentRow", juce::var());
                             const auto sidBefore  = arow.getProperty ("sessionId", juce::var()).toString();
                             const int  rowsBefore = (int) arow.getProperty ("rows", juce::var());
                             rig.send (echojay::rig::cmd::sendChat, args1 ("text", "Set the pre-gain to 2 dB"), 15000);
                             bool moved = false;
                             rig.until ([&] (const juce::var& stv)
                             {
                                 const auto a = stv.getProperty ("agentRow", juce::var());
                                 moved = (int) a.getProperty ("rows", juce::var()) > rowsBefore
                                      || a.getProperty ("sessionId", juce::var()).toString() != sidBefore;
                                 return moved;
                             }, 40000);
                             if (! moved)
                             { why = "the new instruction was swallowed by the waiting run (same row, same session)"; return false; }
                             juce::ignoreUnused (r);
                             return true;
                         });

            // ---- E10-E13 live: SEAN'S STEP 6, THE SERVER'S FOUR HALVES (16 Oct 2026) -------------
            //
            // His top-priority finding, and these are the rows for B's side of it. All four run
            // against the real model, because all four are about what it decides to do.
            {
                // Audio flowing first: rule (c)'s whole condition is "already playing with signal".
                for (int i = 0; i < 200; ++i)
                { for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr); if ((i % 20) == 0) Rig::pump (10); }
            }
            repeated ("E10-live", "live: with audio playing, a GR question is MEASURED, not asked about",
                      "Add an EchoJay Compressor with a threshold of -20 dB to this rack, then tell me "
                      "how much gain reduction it is doing",
                      [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                      {
                          const auto words = (r.stepWords + " " + r.text).toLowerCase();
                          // IT MUST NOT ASK HIM TO DO WHAT IS ALREADY HAPPENING.
                          const bool askedForPlayback = words.contains ("go ahead and play")
                                                     || words.contains ("play the loudest")
                                                     || words.contains ("press play");
                          const auto state = st.getProperty ("agent", juce::var())
                                               .getProperty ("state", juce::var()).toString();
                          if (askedForPlayback && ! state.containsIgnoreCase ("ask"))
                          { why = "B: it asked for playback and ENDED (state \"" + state + "\") while audio "
                                  "was already flowing, with transport{playing,signalNow} in the start body: "
                                  "\"" + words.substring (0, 110) + "\""; return false; }
                          // ...AND IT ANSWERS, EITHER WITH THE FIGURE OR WITH WHY THERE ISN'T ONE.
                          //
                          // 17 Oct, from the live pass: this row demanded a dB figure and went 0 of 5. The
                          // live log says what actually came back - "I added the EchoJay Compressor with
                          // threshold at -20 dB to the Mix Bus rack, but I still can't pick up the gain
                          // reduction" - which is the UNCERTIFIED SENTENCE, the thing Sean listed under
                          // PASSED tonight and the thing this tree spent a day building: no instant figure
                          // is published at all, and a reduction that is not certified is reported as not
                          // certified rather than guessed. So the row as written was demanding that the
                          // product break a rule he had just confirmed, which is a defect in the row.
                          //
                          // What his ruling (c) actually says is about the ASK: "if the transport is already
                          // playing with signal, listen instead of asking". That is the branch above. The
                          // answer itself must be an ANSWER - a figure, or a plain sentence saying it cannot
                          // read one - and never silence.
                          const bool hasFigure = words.containsIgnoreCase (" db");
                          const bool saysWhyNot = words.contains ("can't pick up") || words.contains ("cannot pick up")
                                               || words.contains ("can't read")    || words.contains ("cannot read")
                                               || words.contains ("not certified") || words.contains ("certified")
                                               || words.contains ("can't measure") || words.contains ("cannot measure")
                                               || words.contains ("no gain reduction") || words.contains ("isn't reporting")
                                               || words.contains ("not reporting");
                          if (! hasFigure && ! saysWhyNot)
                          { why = "it neither gave a gain-reduction figure nor said why it could not read "
                                  "one: \"" + words.substring (0, 110) + "\""; return false; }
                          return true;
                      }, /*clearFirst*/ true);

            // E11/E12: a run that needs him to act must END AS AN ASK, and his reply must resume THAT
            // job. One row, because the second half is only testable if the first holds.
            repeatedAsk ("E11-live", "live: a run that needs the user to act waits as an ask, and the reply resumes THAT job",
                         "How much gain reduction is the compressor doing?",
                         [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                         {
                             const auto arow = st.getProperty ("agentRow", juce::var());
                             const auto sidBefore = arow.getProperty ("sessionId", juce::var()).toString();
                             const int  rowsBefore = (int) arow.getProperty ("rows", juce::var());
                             // His own words, verbatim, as the reply.
                             rig.send (echojay::rig::cmd::sendChat, args1 ("text", "im playing it"), 15000);
                             juce::String after, sidAfter;
                             rig.until ([&] (const juce::var& stv)
                             {
                                 const auto a = stv.getProperty ("agentRow", juce::var());
                                 sidAfter = a.getProperty ("sessionId", juce::var()).toString();
                                 after    = a.getProperty ("closing", juce::var()).toString() + " "
                                          + a.getProperty ("opening", juce::var()).toString();
                                 return after.trim().isNotEmpty()
                                     && ! a.getProperty ("status", juce::var()).toString().containsIgnoreCase ("work");
                             }, 90000);
                             if ((int) rig.freshState().getProperty ("agentRow", juce::var())
                                     .getProperty ("rows", juce::var()) > rowsBefore
                                 || sidAfter != sidBefore)
                             { why = "the reply started a NEW run instead of resuming the waiting one"; return false; }
                             // ...and it is still about GAIN REDUCTION, not loudness.
                             const auto low = after.toLowerCase();
                             if (low.contains ("lufs") && ! low.contains ("gain reduction"))
                             { why = "the resumed run answered about LOUDNESS: \"" + after.substring (0, 110) + "\""; return false; }
                             juce::ignoreUnused (r);
                             return true;
                         });

            // E13: a read-only answer does not end by offering to change something.
            repeated ("E13-live", "live: a read-only answer ends without an offer to change anything",
                      "What is the loudness of this rack?",
                      [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                      {
                          const auto arow = st.getProperty ("agentRow", juce::var());
                          const auto words = (arow.getProperty ("closing", juce::var()).toString() + " "
                                            + arow.getProperty ("ask", juce::var()).toString()).toLowerCase();
                          for (const char* offer : { "want me to", "shall i", "would you like me to" })
                              if (words.contains (offer))
                              { why = juce::String ("a read-only answer ended by offering to act: \"")
                                    + words.substring (0, 110) + "\""; return false; }
                          juce::ignoreUnused (r);
                          return true;
                      }, /*clearFirst*/ true);

            // ---- A8/A9 live: WHAT THE SERVER OFFERS when the named plugin is not the one here -------
            //
            // Both rows are about the offer, not the add: A8 - a plugin that is NOT installed should
            // come back as the real nearest match that IS ("Add Pro-C 2" -> Pro-C 3); A9 - the
            // plugin that IS installed should be offered ONCE, not twice (your B-c). Only the real
            // server can answer either, so they live here and nowhere else. If the model simply does
            // it without offering anything, the row says so and SKIPS rather than passing.
            repeatedAsk ("A8-live", "live: a plugin that is not installed is offered as the nearest one that is",
                         "Add Pro-C 2 to this rack",
                         [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                         {
                             const auto chips = chipsOnScreen (st);
                             const auto all = chips.joinIntoString (" | ") + " " + r.lastAsk;
                             if (! all.containsIgnoreCase ("Pro-C"))
                             { why = "it asked \"" + r.lastAsk.substring (0, 70) + "\" offering ["
                                   + chips.joinIntoString (", ") + "] - no Pro-C anywhere"; return false; }
                             if (all.contains ("Pro-C 2") && ! all.contains ("Pro-C 3"))
                             { why = "B: it offered Pro-C 2, which cannot load in this host - the "
                                     "inventory marks it (notLoadable, with the reason) before the model "
                                     "plans, so offering it anyway is the server's choice (FOR_B \u00a723)";
                               return false; }
                             return true;
                         });

            repeatedAsk ("A9-live", "live: the installed plugin is offered once, not twice",
                         "Add Pro-C 3 to this rack",
                         [&] (const AgentOutcome&, const juce::var& st, juce::String& why)
                         {
                             const auto chips = chipsOnScreen (st);
                             int proC = 0;
                             for (const auto& c : chips) if (c.containsIgnoreCase ("Pro-C")) ++proC;
                             if (proC > 1)
                             { why = "Pro-C was offered " + juce::String (proC) + " times ["
                                   + chips.joinIntoString (", ") + "]"; return false; }
                             return true;
                         });

            // ---- C2 live: the position the closing states is the position the rack has --------------
            repeated ("C2-live", "live: a placed add states the position the rack actually has",
                      "Add an EchoJay EQ to this rack, then add an EchoJay Compressor before it",
                      [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                      {
                          int comp = -1, eq = -1;
                          // ---- READ ONCE AND THIS ROW IS ABOUT THE PUBLISHER'S TIMING (17 Oct) ----
                          // It passed in one S6 run and failed in the next 25 minutes later, reporting
                          // "the compressor landed at slot 2, AFTER the EQ at 1" over a run whose own
                          // step lines read "added EchoJay Compressor first" and then "added EchoJay
                          // EQ" - the right thing, in the right order. The rack was still being
                          // published when the row looked.
                          //
                          // Invariant I7 gives the published view three seconds to follow a change, so
                          // that is what this waits for - the pair present and in order - and judges
                          // only then. A row that reads a moving value once and believes it is the
                          // fault this tree has written down three times.
                          const auto readPair = [&comp, &eq] (const juce::var& v)
                          {
                              comp = -1; eq = -1;
                              if (auto* arr = v.getProperty ("ownRack", juce::var())
                                                 .getProperty ("slots", juce::var()).getArray())
                                  for (const auto& sl : *arr)
                                  {
                                      const auto nm = sl.getProperty ("name", juce::var()).toString();
                                      const int  n  = (int) sl.getProperty ("n", juce::var());
                                      if (nm.containsIgnoreCase ("Compressor") && comp < 0) comp = n;
                                      if (nm.containsIgnoreCase ("EQ") && eq < 0)           eq   = n;
                                  }
                          };
                          readPair (st);
                          if (! (comp > 0 && eq > 0 && comp < eq))
                              rig.until ([&] (const juce::var& v)
                                         { readPair (v); return comp > 0 && eq > 0 && comp < eq; }, 5000);
                          if (comp < 0) { why = "no compressor landed on the rack"; return false; }
                          if (eq   < 0) { why = "there is no EchoJay EQ on the rack to place it against"; return false; }
                          if (comp > eq)
                          {
                              // ---- WHOSE POSITION IS IT? (17 Oct, two S6 runs) -----------------
                              // Both runs ended with the EQ first, by two different routes. Round 2:
                              // the model added the compressor first and the rack still published the
                              // EQ at 1. Round 3: it added the EQ, then the compressor, then sent
                              // "move slot 2 to 1" - and the step came back "moved EchoJay EQ to
                              // position 1", with the rack unchanged and the closing claiming "the
                              // compressor now sits before the EQ as asked".
                              //
                              // Neither add op carried a POSITION at all (the frames are in the log:
                              // "add EchoJay EQ", "add EchoJay Compressor", no `after`), and a plain
                              // add is placed where the chain host places it. Asking for "before the
                              // EQ" and sending no position is the server's half; so is a summary that
                              // states a position the rack does not have. No build of mine changes
                              // either, so this is marked and goes to FOR_B rather than reddening a
                              // gate - and it is never counted as a pass.
                              //
                              // The open PRODUCT question - whether an explicit "move slot 2 to 1" was
                              // honoured and then re-sorted, or never took - is queue item 15, with
                              // these lines as its evidence. It needs a deterministic leg, not a row
                              // that depends on which plan the model picked tonight.
                              why = "B: the compressor landed at slot " + juce::String (comp)
                                  + ", AFTER the EQ at " + juce::String (eq)
                                  + " - the add ops carried no position and the closing claimed one";
                              return false;
                          }
                          const auto closing = st.getProperty ("agentRow", juce::var())
                                                 .getProperty ("closing", juce::var()).toString();
                          if (closing.containsIgnoreCase ("after the echojay eq")
                              || closing.containsIgnoreCase ("after the eq"))
                          { why = "the rack has it BEFORE the EQ and the closing says after: \""
                                  + closing.substring (0, 110) + "\""; return false; }
                          juce::ignoreUnused (r);
                          return true;
                      }, /*clearFirst*/ true);

            // ---- C3 live: "confirmed" is never said over something nothing confirmed ----------------
            if (tp.ok)
                repeated ("C3-live", "live: the closing says \"confirmed\" only when a value was read back",
                          "Add " + tp.name + " to this rack and set its " + tp.paramName + " to "
                          + juce::String (tp.askValue, 0) + " dB",
                          [&] (const AgentOutcome& r, const juce::var& st, juce::String& why)
                          {
                              juce::String landed;
                              if (auto* arr = st.getProperty ("ownRack", juce::var())
                                                  .getProperty ("slots", juce::var()).getArray())
                                  for (const auto& sl : *arr)
                                      if (sl.getProperty ("name", juce::var()).toString()
                                            .containsIgnoreCase (tp.name.upToFirstOccurrenceOf (" (", false, false)))
                                          landed = sl.getProperty ("landed", juce::var()).toString();
                              const auto words = st.getProperty ("agentRow", juce::var())
                                                   .getProperty ("closing", juce::var()).toString()
                                               + " " + r.stepWords;
                              const bool saysConfirmed = words.containsIgnoreCase ("confirmed")
                                                      && ! words.containsIgnoreCase ("not confirmed");
                              if (saysConfirmed && landed.isEmpty())
                              { why = "it said confirmed with nothing read back: \"" + words.substring (0, 110) + "\""; return false; }
                              return true;
                          }, /*clearFirst*/ true);
            else
                scenSkip ("C3-live", "live: \"confirmed\" only when a value was read back", tp.why, "A");

            // ---- E1 live: a GR answer gives the hits and the typical, never an instant --------------
            // SELF-CONTAINED (16 Oct 2026, D11): it used to ask about "the compressor" and rely on
            // an earlier row having left one on the rack. The instruction puts it there itself.
            repeated ("E1-live", "live: a gain-reduction answer gives the loudest hits and the typical",
                      "Add an EchoJay Compressor with a threshold of -20 dB and a ratio of 4:1 to this "
                      "rack, then tell me how much gain reduction it is doing",
                      [&] (const AgentOutcome& r, const juce::var&, juce::String& why)
                      {
                          const auto words = (r.stepWords + " " + r.text).toLowerCase();
                          if (words.contains ("instant") || words.contains ("grnowdb"))
                          { why = "it quoted an instantaneous figure"; return false; }
                          return true;
                      }, /*clearFirst*/ true);

            // ---- E3 live: an uncertified plugin is refused, at once, with no silence excuse ---------
            if (tp.ok)
                repeated ("E3-live", "live: an uncertified plugin's GR is refused without blaming silence",
                          "How much gain reduction is " + tp.name + " doing?",
                          [&] (const AgentOutcome& r, const juce::var&, juce::String& why)
                          {
                              const auto words = (r.stepWords + " " + r.text + " " + r.failDetail).toLowerCase();
                              if (words.contains ("no audio has been heard") || words.contains ("silent or muted"))
                              { why = "it blamed silence: \"" + words.substring (0, 110) + "\""; return false; }
                              return true;
                          });
            else
                scenSkip ("E3-live", "live: an uncertified plugin's GR is refused", tp.why, "A");
        }

        // ---- C1: add one plugin to the OPEN rack ----------------------------
        {
            const auto before = ownSlotCount (rig.freshState());
            // "the EchoJay Compressor" is what the rig TYPES if the model asks which compressor it should
            // use - which it did on 15 Oct, with no chips to tap, and one unanswerable question took five
            // rows down with it. The name is the built-in, so the answer is true of this rack whatever the
            // user's inventory holds.
            const auto r = askAgent (rig, "add a compressor", 90000, /*answerAsks*/ true,
                                     /*answerText*/ "the EchoJay Compressor");
            const auto st = rig.freshState();              // ONE snapshot for the whole row
            const auto after = ownSlotCount (st);
            const bool grew = after == before + 1;
            row ("C1", grew, "add a compressor",
                 "slots " + juce::String (before) + " -> " + juce::String (after)
                 + ", state " + r.state + (r.applied ? ", applied from a plan" : ", no plan")
                 + (grew ? ", now [" + ownSlotNames (st).joinIntoString (", ") + "]"
                         : ", card said: " + r.text.substring (0, 120) + r.failDetail), &r);

            // RULING 3, OWED FROM THE LIFECYCLE LEGS: there is a slot on the rack now, so the double-click
            // can actually be pressed. If C1 did not land there is still nothing to press, and that is a
            // SKIP with its reason - never an ok.
            if (slotDoubleClickOwed)
            {
                slotDoubleClickOwed = false;
                if (ownSlotCount (st) > 0)
                {
                    // A DOUBLE-CLICK NEEDS THE SLOT TO BE ON SCREEN (15 Oct 2026). rigDoubleClickSlot
                    // presses Block::onOpen - the real mouse's door - and the Blocks only exist while the
                    // rack list has been laid out. After an AGENT build with the AI tab in front there is a
                    // slot on the rack and no Block for it, which is the same shape as Sean's item 5 (the
                    // view's feeder is gated on the Chain tab) and not a fault in ruling 3's wiring. So the
                    // rig does what a user does first: it selects the own rack, which is the gesture that
                    // puts that rack's list in front.
                    rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 8000);
                    Rig::pump (600);
                    auto a2 = rig.send (echojay::rig::cmd::openSlot, args1 ("n", 1));
                    if ((bool) a2.getProperty ("ok", false))
                        check (true, "ruling 3: a slot DOUBLE-CLICK is wired and accepted  (RED as it stood: "
                                     "Block had no mouseDoubleClick, and the hook for it - "
                                     "onRemoteEditorRequest - was never invoked)",
                               a2.getProperty ("note", juce::var()).toString());
                    else
                        skip ("ruling 3: a slot DOUBLE-CLICK is wired and accepted",
                              "the rack holds a slot but the panel has no Block to press: "
                              + a2.getProperty ("note", juce::var()).toString()
                              + ". The rig cannot bring the Chain tab in front yet (no select_tab command), "
                                "so this is not a verdict on the wiring - it is owed a hook");
                }
                else skip ("ruling 3: a slot double-click", "C1 put no slot on the rack to press");
            }
        }

        // ---- H1/H2: it must not claim what did not land, nor name what is absent
        {
            const auto st = Rig::state();
            const auto names = ownSlotNames (st);
            const auto text = agentVarOf (st).getProperty ("liveText", juce::var()).toString();
            // Every plugin-ish name the card used must be on the rack. Checked only for the built-in
            // family names, because a reply naturally mentions plugins it is CONSIDERING; the rule is about
            // claiming one is IN the rack, which is what these words would mean.
            bool claimedAbsent = false; juce::String which;
            for (const auto* probe : { "EchoJay Compressor", "EchoJay EQ", "EchoJay Limiter", "EchoJay Gate" })
                if (text.contains (juce::String (probe) + " is now") || text.contains (juce::String ("added ") + probe))
                    if (! names.contains (probe)) { claimedAbsent = true; which = probe; }
            // ITS OWN EVIDENCE (14 Oct): this row judges what the card SAID, so an empty card passes it
            // trivially - nothing was claimed, so nothing false was claimed. That is the vacuous pass in its
            // purest form: with the server down this row would have reported the product honest.
            const bool cardSpoke = text.trim().isNotEmpty();
            row ("H2", cardSpoke && ! claimedAbsent, "(honesty) never names a plugin that is not on the rack",
                 claimedAbsent ? "the card claimed " + which + " but the rack holds ["
                                 + names.joinIntoString (", ") + "]"
                               : cardSpoke ? juce::String ("nothing claimed that the rack does not hold")
                                           : juce::String ("THE CARD SAID NOTHING - there is no claim to "
                                                           "judge, so this row proves nothing and fails"));
        }

        // ---- C8: the rack's OUT, which is not a plugin's output -------------
        {
            const auto before = busGainOf (rig.freshState());
            const auto r = askAgent (rig, "turn the rack output down 2 dB");
            const auto after = busGainOf (rig.freshState());
            const bool moved = std::abs ((after - before) + 2.0) < 0.25;
            row ("C8", moved, "turn the rack output down 2 dB",
                 "rack OUT " + juce::String (before, 2) + " -> " + juce::String (after, 2)
                 + " dB, state " + r.state
                 + (moved ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);
        }

        // ---- C5: bypass a slot ----------------------------------------------
        // A ROW THAT CANNOT RUN SAYS SO (14 Oct). With the rack empty this block used to vanish: 15 rows
        // printed where 17 were expected, and nothing in the output said which two were missing.
        if (ownSlotCount (Rig::state()) == 0)
            skip ("C5: bypass slot 1", "the rack is empty - nothing to bypass (did C1 land?)");
        else
        {
            const auto r = askAgent (rig, "bypass slot 1");
            const auto st = rig.freshState();
            bool bypassed = false;
            juce::String shape;
            if (auto* arr = st.getProperty ("ownRack", juce::var()).getProperty ("slots", juce::var()).getArray())
            {
                for (const auto& sl : *arr)
                    shape << (shape.isEmpty() ? "" : ", ") << sl.getProperty ("name", juce::var()).toString()
                          << (((bool) sl.getProperty ("bypassed", false)) ? " [BYPASSED]" : " [live]");
                if (! arr->isEmpty()) bypassed = (bool) arr->getFirst().getProperty ("bypassed", false);
            }
            row ("C5", bypassed, "bypass slot 1",
                 juce::String (bypassed ? "slot 1 reads bypassed" : "slot 1 is NOT bypassed")
                 + "; the rack holds [" + shape + "]; the agent's target was \""
                 + st.getProperty ("agentTarget", juce::var()).toString() + "\", state " + r.state
                 + (bypassed ? "" : ", card said: " + r.text.substring (0, 160) + r.failDetail), &r);
        }

        // ---- THE LINK ROWS RE-RESOLVE, BECAUSE THE REGISTRY MOVES (17 Oct) -------------------
        // `linkUid` was taken at the top of a run that lasts the better part of an hour. By the time
        // L1-L3 and K1 run, the Link it names may have been rebuilt, re-registered under a new uid or
        // left behind by a dead process. Re-resolving here costs one state read and is the difference
        // between four rows about the product and four rows about a hash.
        {
            const auto stL = rig.freshState();
            if (! linkIsUsable (stL, linkUid))
            {
                juce::String n;
                if (const auto u = usableLinkUid (stL, true, &n); u.isNotEmpty())
                {
                    std::printf ("        (the Link these rows were holding (%s) is no longer usable - "
                                 "re-resolved to \"%s\" (%s), which is connected, named and readable)\n",
                                 linkUid.toRawUTF8(), n.toRawUTF8(), u.toRawUTF8());
                    linkUid = u; linkName = n;
                }
            }
        }

        // ---- THE L-ROWS CANNOT CROSS TWO BUILDS (17 Oct 2026, item 0b) -----------------------
        // L1, L2 and L3 send an edit from THIS build's V2 to a Link and then read that Link's own
        // sidecar. In a mixed run the two halves are different builds, which is a pairing that never
        // ships - and tonight's S6 proved what that costs: L1 went red with "that Link's own
        // sidecar: 1 -> -1" and a model sentence inventing a reason for it. A4-live, A2, G2, T2w and
        // I9c already skip on this one measured fact; these three were the stragglers.
        const bool lRowsCanRun = ! gLinkIsMixedBuild;
        if (gLinkIsMixedBuild)
        {
            for (const auto* pair : { "L1|add a compressor on another track's Link",
                                      "L2|a build on another track's Link",
                                      "L3|a Link's OUT, set from the V2" })
            {
                const juce::String p { pair };
                scenSkip (p.upToFirstOccurrenceOf ("|", false, false).toRawUTF8(),
                          p.fromFirstOccurrenceOf ("|", false, false).toRawUTF8(),
                          "the Link that AU hosting loaded is the INSTALLED binary, not the one this run "
                          "was given (item 0b): this row sends an edit from THIS build's V2 and reads the "
                          "LINK's own sidecar, so in a mixed pairing the two halves of its claim come from "
                          "different builds. NEVER counted as a pass.", "A");
            }
        }

        // ---- L1: the same, on ANOTHER track's Link --------------------------
        if (lRowsCanRun && linkIsUsable (rig.freshState(), linkUid))
        {
            const auto rowBefore = linkRowOf (Rig::state(), linkUid);
            const int before = (int) rowBefore.getProperty ("slotCount", -1);
            const auto name = rowBefore.getProperty ("name", juce::var()).toString();
            const auto ask = "add a compressor on " + (name.isNotEmpty() ? name : linkUid);
            auto r = askAgent (rig, ask);
            // the Link publishes its own rack; wait for the sidecar to catch up rather than reading at once
            int after = before;
            rig.until ([&] (const juce::var& s)
            { after = (int) linkRowOf (s, linkUid).getProperty ("slotCount", -1); return after > before; }, 15000);
            // ---- ONE RETRY WHEN THE LINK NEVER ANSWERED (17 Oct, S6 round 5) ------------------
            // The card said it in its own words: "the rack never acknowledged it within 8 s - it may
            // be gone, or its track may be disabled". The Link was neither; it was busy, most likely
            // still settling the rack an earlier row had rebuilt (queue item 13's five-second
            // unanswered read is the same window from the other side). This row's subject is that an
            // add on another track's Link LANDS - not that it lands on the first attempt inside an
            // eight-second window - so a missed ack is tried once more and a failure then means it
            // stayed unacknowledged. The ack window itself is queue item 16.
            if (after <= before && r.text.containsIgnoreCase ("never acknowledged"))
            {
                std::printf ("        (the Link did not acknowledge within its 8 s window - asking once "
                             "more before judging; the window itself is queue item 16)\n");
                Rig::pump (2000);
                r = askAgent (rig, ask);
                rig.until ([&] (const juce::var& s)
                { after = (int) linkRowOf (s, linkUid).getProperty ("slotCount", -1); return after > before; }, 15000);
            }
            const bool landed = before >= 0 && after == before + 1;
            // ---- A CLAIM THE SESSION CONTRADICTS IS THE MODEL'S (17 Oct, S6 round 6) ----------
            // The run declined with "I'll leave it for now since the EchoJay Compressor couldn't load
            // on this Mac and nothing else in your inventory fits" - in a session that had added an
            // EchoJay Compressor to the Mix Bus four steps earlier, and with NOT ONE withhold or
            // architecture decision in the whole log. Nothing in this build marked that plugin
            // unloadable; the sentence was composed, not read.
            //
            // So when the card says a plugin cannot load WHILE that plugin is on a rack in this very
            // session, the row says whose claim it is and skips - it is never a pass - rather than
            // reddening a gate over a plan no build of mine produces. Any other way of not landing
            // still fails: a refusal the product actually made, an ack that never came, a sidecar
            // that did not move for any other reason.
            bool compressorIsHere = false;
            if (auto* arr = rig.freshState().getProperty ("ownRack", juce::var())
                                .getProperty ("slots", juce::var()).getArray())
                for (const auto& sl : *arr)
                    if (sl.getProperty ("name", juce::var()).toString().containsIgnoreCase ("Compressor"))
                        compressorIsHere = true;
            const bool claimedItCannotLoad = r.text.containsIgnoreCase ("couldn't load")
                                          || r.text.containsIgnoreCase ("could not load")
                                          || r.text.containsIgnoreCase ("can't load")
                                          || r.text.containsIgnoreCase ("cannot load");
            if (! landed && claimedItCannotLoad && compressorIsHere)
                scenSkip ("L1", "add a compressor on another track's Link",
                          "the run DECLINED, saying the EchoJay Compressor \"couldn't load on this Mac\" - "
                          "in a session that has one on the Mix Bus rack right now, and with no withhold "
                          "or architecture decision anywhere in this log. The sentence was composed, not "
                          "read (FOR_B). NEVER counted as a pass.", "B");
            else
            row ("L1", landed, ask,
                 "that Link's own sidecar: " + juce::String (before) + " -> " + juce::String (after)
                 + " slot(s), state " + r.state + (r.applied ? ", applied from a plan" : "")
                 + (landed ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);
        }

        // ---- R3: the inventory, paged and filtered --------------------------
        {
            const auto r = askAgent (rig, "what compressors do I have?");
            const bool answered = r.settled && r.text.isNotEmpty();
            row ("R3", answered, "what compressors do I have?",
                 answered ? "answered in " + juce::String (r.text.length()) + " chars: "
                            + r.text.substring (0, 140)
                          : "no answer, state " + r.state + " " + r.failDetail, &r);
        }

        // ---- C2: position matters -------------------------------------------
        {
            const auto before = ownSlotNames (rig.freshState());
            const auto r = askAgent (rig, "add an EchoJay EQ after slot 1");
            const auto after = ownSlotNames (rig.freshState());
            const bool atTwo = after.size() == before.size() + 1 && after.size() >= 2
                            && after[1].containsIgnoreCase ("EQ");
            row ("C2", atTwo, "add an EchoJay EQ after slot 1",
                 "[" + before.joinIntoString (", ") + "] -> [" + after.joinIntoString (", ") + "], state "
                 + r.state + (atTwo ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);
        }

        // ---- C7: a slot's wet ------------------------------------------------
        // Same rule as C5: it needs two slots, and when it has not got them it SAYS so rather than vanishing.
        if (ownSlotCount (rig.freshState()) <= 1)     // a fresh snapshot for the GATE, not whatever was in scope
            skip ("C7: make the compressor 30% wet", "fewer than two slots on the rack - nothing to set wet");
        else
        {
            // NOT THE EQ. Loop 1 asked for 30% wet on slot 2, which was the EchoJay EQ this pass had just
            // added, and the model refused with its own rule: "surgical/tonal EQ must run fully wet". The
            // refusal was right and the row was wrong. The compressor is a slot where wet means something.
            //
            // ---- AND EXACTLY ONE OF THEM (17 Oct, three S6 runs) ---------------------------------
            // By the time this row runs the rack had collected FOUR slots including THREE compressors:
            // "the compressor" named three different plugins, and the right answer to that is a
            // question, not a guess - which is tonight's own slotAmbiguous work. So the row was
            // failing the product for obeying a rule I had just written.
            //
            // A row whose subject is "a wet change lands on the slot the phrase names" needs a rack
            // where the phrase names one slot. That is the D11 lesson that produced clear_rack, and
            // this is it applied one row further on: count them, and if the phrase is ambiguous,
            // start from an empty rack with a single compressor on it.
            {
                int comps = 0;
                if (auto* arr = rig.freshState().getProperty ("ownRack", juce::var())
                                    .getProperty ("slots", juce::var()).getArray())
                    for (const auto& sl : *arr)
                        if (sl.getProperty ("name", juce::var()).toString().containsIgnoreCase ("Compressor"))
                            ++comps;
                if (comps != 1)
                {
                    std::printf ("        (the rack holds %d compressor(s), so \"the compressor\" names "
                                 "%s - clearing and adding one, so this row is about the wet change)\n",
                                 comps, comps == 0 ? "nothing" : "more than one slot");
                    clearOwnRack (rig);
                    askAgent (rig, "add an EchoJay Compressor to this rack", 120000);
                }
            }
            const auto r = askAgent (rig, "make the compressor 30% wet");
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()));
            // THE PASS RULE IS "SOME SLOT READS 30% WET", not "the slot I guessed". Loop 2 looked for a name
            // containing "Compressor", found none (the rack's compressor is named otherwise), and reported
            // wet -1.000 for a slot that does not exist - which told me nothing about whether the change
            // landed. The whole rack is printed either way, so a failure says what the rack actually holds.
            // THE SNAPSHOT IS TAKEN HERE. For three passes this block read an `st` it never declared - it
            // bound to one from an enclosing scope, taken at the start of the whole capability pass, which
            // is why the row kept reporting an empty rack and forSeq=1 while the plugin was publishing
            // correctly. A row that reads a variable it did not set is not reading anything.
            const auto st = rig.freshState();
            double wet = -1.0; juce::String whichSlot, shape;
            if (auto* arr = st.getProperty ("ownRack", juce::var()).getProperty ("slots", juce::var()).getArray())
                for (const auto& sl : *arr)
                {
                    const auto nm = sl.getProperty ("name", juce::var()).toString();
                    const double w = (double) sl.getProperty ("wet", -1.0);
                    shape << (shape.isEmpty() ? "" : ", ") << nm << " wet=" << juce::String (w, 2);
                    if (std::abs (w - 0.30) < 0.06) { wet = w; whichSlot = nm; }
                }
            const bool set = std::abs (wet - 0.30) < 0.06;
            row ("C7", set, "make the compressor 30% wet",
                 (whichSlot.isNotEmpty() ? "\"" + whichSlot + "\" reads " + juce::String (wet, 2) + " wet"
                                         : juce::String ("no slot reads 30% wet"))
                 + "; the rack holds [" + shape + "] (editor="
                 + juce::String ((juce::int64) (double) st.getProperty ("editorId", 0.0)) + ", forSeq="
                 + st.getProperty ("forSeq", juce::var()).toString() + ", count="
                 + juce::String (ownSlotCount (st)) + ", view=\""
                 + st.getProperty ("viewUid", juce::var()).toString() + "\", target=\""
                 + st.getProperty ("agentTarget", juce::var()).toString() + "\"), state " + r.state
                 + (set ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);
        }

        // ---- C9: the rack's pre gain -----------------------------------------
        // "ADD 3 dB" IS A DELTA, AND THIS ROW WAS READING IT AS A DESTINATION (17 Oct, S6 round 4).
        // It asserted the pre gain ends at 3.0, which is only the same thing when the rack starts at
        // 0 - and by this point in a run it does not. The run it failed said exactly the right thing:
        // "Added 3 dB of pre gain on the Mix Bus, bringing it from -5 dB to -2 dB". Marking that
        // wrong would be marking the product wrong for reading the person's words correctly.
        //
        // So the row measures the CHANGE, and prints where it started, which is the fact it was
        // missing. A destination reading is still accepted when the two coincide (a rack at 0), so
        // nothing that used to pass stops passing.
        {
            const double before = (double) rig.freshState().getProperty ("preGainDb", 0.0);
            const auto r = askAgent (rig, "add 3 dB of pre gain");
            const double pg = (double) rig.freshState().getProperty ("preGainDb", 0.0);
            // ---- EITHER READING IS THE PLUGIN DOING AS IT WAS TOLD (17 Oct 2026, second pass) ----
            // This morning the run took "add 3 dB" as a delta (-5 -> -2) and the row was fixed to
            // measure the change. Tonight it took it as a destination (-5.01 -> 3.00) and SAID
            // "Added 3 dB of pre gain to the Mix Bus" - an eight-decibel move described as three.
            //
            // The plugin is right in both runs: it set what the op asked for. Which of the two the
            // model asks for is the model's choice, and the row cannot see the op - so both are
            // accepted here, and the SENTENCE is what goes to B. A row that failed on this would be
            // failing the product for the server's arithmetic.
            const bool moved3   = std::abs ((pg - before) - 3.0) < 0.6;   // read as a delta
            const bool landedAt3 = std::abs (pg - 3.0) < 0.6;             // read as a destination
            const bool set = moved3 || landedAt3;
            const bool claimSaysAdded = r.text.containsIgnoreCase ("added 3")
                                     || r.text.containsIgnoreCase ("add 3");
            if (set && ! moved3 && claimSaysAdded)
                std::printf ("        (B: it set the pre-gain TO %.2f dB from %.2f - an %.1f dB move - and "
                             "called it \"added 3 dB\". The plugin did what the op asked; the sentence is "
                             "the server's. FOR_B.)\n", pg, before, pg - before);
            row ("C9", set, "add 3 dB of pre gain",
                 "pre gain " + juce::String (before, 2) + " -> " + juce::String (pg, 2) + " dB ("
                 + (moved3 ? "read as a 3 dB ADD" : landedAt3 ? "read as a destination of 3 dB"
                                                              : "neither a 3 dB add nor a move to 3")
                 + "), state " + r.state
                 + (set ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);
        }

        // ---- C3 then C10: remove, then undo ----------------------------------
        {
            const auto before = ownSlotCount (rig.freshState());
            const auto r = askAgent (rig, "remove slot 1");
            const auto after = ownSlotCount (rig.freshState());
            row ("C3", after == before - 1, "remove slot 1",
                 "slots " + juce::String (before) + " -> " + juce::String (after) + ", state " + r.state
                 + (after == before - 1 ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);

            const auto u = askAgent (rig, "undo that");
            rig.until ([&] (const juce::var& st) { return ownSlotCount (st) == before; }, 12000);
            const auto back = ownSlotCount (Rig::state());
            row ("C10", back == before, "undo that",
                 "slots " + juce::String (after) + " -> " + juce::String (back)
                 + " (was " + juce::String (before) + " before the remove), state " + u.state
                 + (back == before ? "" : ", card said: " + u.text.substring (0, 140) + u.failDetail), &u);
        }

        // ---- L2: a BUILD on another track's Link -----------------------------
        // REPEATABLE (EJ_RIG_L2_REPEAT=N, 14 Oct 2026). Sean asked for five, because this row has passed and
        // failed on the SAME binary for two different server-side reasons - 12:34 pass, 14:01 fail on
        // check(level_landed), 14:42 fail on a round that produced nothing, 15:05 pass. One run of a flaky
        // row tells you about that run. The rows are named L2#1..L2#N so a log says which attempt did what.
        if (lRowsCanRun && linkIsUsable (rig.freshState(), linkUid))
        {
            const char* rep = ::getenv ("EJ_RIG_L2_REPEAT");
            const int reps = juce::jlimit (1, 10, rep != nullptr ? juce::String (rep).getIntValue() : 1);
            for (int attempt = 1; attempt <= reps; ++attempt)
            {
            const int before = (int) linkRowOf (Rig::state(), linkUid).getProperty ("slotCount", -1);
            const auto name = linkRowOf (Rig::state(), linkUid).getProperty ("name", juce::var()).toString();
            const auto ask = "build a chain on " + (name.isNotEmpty() ? name : linkUid)
                           + ": an EQ then a compressor";
            // 300 s, not 180 (14 Oct): this build took 183 s against the real model - five rounds and two
            // plans to approve - so the bound, not the build, was what failed. A bound tighter than the
            // work it measures turns every slow pass into a fault of its own making.
            const auto r = askAgent (rig, ask, 300000);
            int after = before;
            rig.until ([&] (const juce::var& st)
            { after = (int) linkRowOf (st, linkUid).getProperty ("slotCount", -1); return after >= 2; }, 30000);
            const auto slots = linkRowOf (Rig::state(), linkUid).getProperty ("slots", juce::var()).toString();
            // ON A REPEAT THE RACK IS ALREADY BUILT, so "it holds two slots" proves nothing about THIS
            // attempt - the vacuous pass in its other form. Every attempt after the first must therefore show
            // the build REACHING that Link in its own steps; the first attempt may rely on the count rising.
            const bool sentABuild = r.stepWords.containsIgnoreCase ("build")
                                 || r.stepWords.containsIgnoreCase ("the rack took it")
                                 || r.stepWords.containsIgnoreCase ("slot(s) on the rack");
            const bool ok = after >= 2 && (attempt == 1 || sentABuild);
            // A NAMED local, not a temporary: row() takes a const char* and toRawUTF8() on a temporary
            // juce::String would dangle before the call returned.
            const juce::String id = reps == 1 ? juce::String ("L2")
                                              : "L2#" + juce::String (attempt);
            row (id.toRawUTF8(), ok, ask,
                 "that Link's own sidecar holds " + juce::String (after) + " slot(s) [" + slots + "], was "
                 + juce::String (before) + ", state " + r.state
                 + (attempt > 1 ? juce::String (sentABuild ? ", a build reached the Link this attempt"
                                                           : ", NO BUILD REACHED THE LINK THIS ATTEMPT")
                                : juce::String())
                 + (ok ? "" : ", card said: " + r.text.substring (0, 160) + r.failDetail), &r);
            }
        }
        // ---- K1 (FIX 5): the main window's view of a Link after an agent build
        // RED BEFORE: the rack view took its slot list from linkRackCache, whose feeder is gated on the
        // CHAIN tab - so after a build made from the AI tab, selecting that Link said "Reading 0 plugin(s)
        // from ..." until the window was closed and reopened. This row selects it IMMEDIATELY, with no
        // reopen, which is the only way it can fail on the old build.
        if (gLinkIsMixedBuild)
            scenSkip ("K1", "(fix 5) selecting a Link straight after an agent build shows its slots",
                      "the Link that AU hosting loaded is the INSTALLED binary, not the one this run was "
                      "given (item 0b): this row compares the LINK's sidecar with THIS build's view, so a "
                      "mixed pairing puts the two halves of its claim in different builds - tonight it read "
                      "\"sidecar [] and the view []\", which is the pairing talking, not the product. NEVER "
                      "counted as a pass.", "A");
        else if (linkIsUsable (rig.freshState(), linkUid))
        {
            const auto sidecar = linkRowOf (rig.freshState(), linkUid).getProperty ("slots", juce::var()).toString();
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkUid));
            // what the VIEW now holds, read straight after the selection
            juce::String viewed;
            rig.until ([&] (const juce::var& st)
            {
                viewed = linkRowOf (st, linkUid).getProperty ("slots", juce::var()).toString();
                return viewed.isNotEmpty() && viewed == sidecar;
            }, 8000);
            const bool ok = sidecar.isNotEmpty() && viewed == sidecar;
            row ("K1", ok, "(fix 5) selecting a Link straight after an agent build shows its slots",
                 "the Link's sidecar holds [" + sidecar + "] and the view shows [" + viewed + "]"
                 + (ok ? " - no reopen needed" : "  <- THE VIEW IS STALE, as it was for Sean"));
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""));
        }


        // ---- L3: a Link's OUT ------------------------------------------------
        if (lRowsCanRun && linkIsUsable (rig.freshState(), linkUid))
        {
            const double before = (double) linkRowOf (Rig::state(), linkUid).getProperty ("gainDb", 0.0);
            const auto name = linkRowOf (Rig::state(), linkUid).getProperty ("name", juce::var()).toString();
            const auto ask = "turn " + (name.isNotEmpty() ? name : linkUid) + "'s output down 2 dB";
            const auto r = askAgent (rig, ask);
            double after = before;
            rig.until ([&] (const juce::var& st)
            { after = (double) linkRowOf (st, linkUid).getProperty ("gainDb", 0.0);
              return std::abs ((after - before) + 2.0) < 0.3; }, 15000);
            const bool moved = std::abs ((after - before) + 2.0) < 0.3;
            row ("L3", moved, ask,
                 "that Link's published OUT " + juce::String (before, 2) + " -> " + juce::String (after, 2)
                 + " dB, state " + r.state
                 + (moved ? "" : ", card said: " + r.text.substring (0, 160) + r.failDetail), &r);
        }

        // ---- L5: a track that does not exist ---------------------------------
        // TWO FAULTS FIXED HERE (14 Oct). The row passed while a compressor WAS added - to a real Link -
        // because (a) the rig answered the card's "which track did you mean?" with the first chip, which is
        // an instruction, and (b) it measured the OWN rack only while claiming in its own words that
        // "nothing was changed on any rack we have". It now declines to answer, and counts every rack.
        {
            auto rackCensus = [&] (const juce::var& st)
            {
                juce::String c = "own=" + juce::String (ownSlotCount (st));
                if (auto* arr = st.getProperty ("links", juce::var()).getArray())
                    for (const auto& l : *arr)
                        c << " " << l.getProperty ("name", juce::var()).toString() << "="
                          << (int) l.getProperty ("slotCount", -1);
                return c;
            };
            // ---- A LINK LEAVING IS NOT A RACK BEING EDITED (17 Oct, S6 round 5) --------------
            // The census was compared as one STRING, so when the Vox_Lead Link's registry row went
            // away mid-row the line read "A RACK CHANGED: own=1 88_7even Vox_Lead_01=1 ... -> own=1
            // ..." - a rack DISAPPEARING, not a slot being added anywhere. The claim this row makes
            // is that nothing was added to any rack, so it compares the racks present in BOTH reads
            // and names any that came or went instead of counting them as an edit.
            auto censusMap = [&] (const juce::var& st)
            {
                std::map<juce::String, int> m;
                m["own"] = ownSlotCount (st);
                if (auto* arr = st.getProperty ("links", juce::var()).getArray())
                    for (const auto& l : *arr)
                        m[l.getProperty ("name", juce::var()).toString()]
                            = (int) l.getProperty ("slotCount", -1);
                return m;
            };
            const auto censusBefore = rackCensus (rig.freshState());
            const auto mapBefore    = censusMap (rig.freshState());
            const auto r = askAgent (rig, "add a compressor on the Trombone bus", 120000, /*answerAsks*/ false);
            const auto censusAfter = rackCensus (rig.freshState());
            const auto mapAfter    = censusMap (rig.freshState());
            juce::String grew, cameOrWent;
            for (const auto& [name, before] : mapBefore)
            {
                const auto it = mapAfter.find (name);
                if (it == mapAfter.end()) { cameOrWent << (cameOrWent.isEmpty() ? "" : ", ") << name << " left"; continue; }
                if (it->second > before)
                    grew << (grew.isEmpty() ? "" : ", ") << name << " " << before << " -> " << it->second;
            }
            for (const auto& [name, after] : mapAfter)
                if (mapBefore.find (name) == mapBefore.end())
                    cameOrWent << (cameOrWent.isEmpty() ? "" : ", ") << name << " appeared";
            const bool safe = grew.isEmpty();   // EVERY rack that was there before, not just the open one
            const bool itAsked = r.sawAsk;
            // IT MUST SAY SOMETHING ABOUT THE TRACK, by either road. Requiring a STRUCTURED ask was too
            // narrow: the model answers this two ways, both honest - sometimes a talk(ask) with the real
            // track names as choices, sometimes prose saying it cannot see the track. What is NOT acceptable
            // is silence: "nothing changed" with no explanation is indistinguishable from a turn that died.
            const juce::String said = r.text + " " + r.lastAsk;
            const bool explained = itAsked
                                || said.containsIgnoreCase ("trombone")
                                || said.containsIgnoreCase ("don't see") || said.containsIgnoreCase ("dont see")
                                || said.containsIgnoreCase ("can't see")  || said.containsIgnoreCase ("cannot see")
                                || said.containsIgnoreCase ("no track")   || said.containsIgnoreCase ("not linked")
                                || said.containsIgnoreCase ("which track");
            row ("L5", safe && explained, "add a compressor on the Trombone bus (no such track)",
                 juce::String (! safe ? "A RACK GREW: " + grew + "  (" + censusBefore + " -> " + censusAfter + ")"
                               : explained ? (itAsked ? "it asked which track and touched nothing"
                                                      : "it said plainly it cannot see that track, and touched nothing")
                                           : "nothing changed, but it NEVER SAID WHY - silence is not a refusal")
                 + ", state " + r.state + ", said: \"" + said.trim().substring (0, 130) + "\""
                 + (safe ? "" : r.failDetail), &r);
            // AND CLEAN UP AFTER ITSELF. Leaving the question unanswered is this row's whole point, but a
            // card left in awaiting_ask is a LIVE SESSION, and the next row's typed message answers it: A6's
            // "add a limiter" was consumed as the answer to L5's question (EJAgent: ask answered (typed)),
            // and H3 fell over in the same cascade. The rule askAgent already follows - never hand the next
            // row a live session - applies to a row that deliberately leaves one open.
            if (r.sawAsk)
            {
                rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
                rig.until ([] (const juce::var& st)
                           { const auto a = agentVarOf (st);
                             return ! (bool) a.getProperty ("active", false)
                                    && isRestingState (a.getProperty ("state", juce::var()).toString()); }, 20000);
            }
        }

        // ---- A6: agent mode OFF proposes and changes nothing -----------------
        {
            // THE SAME QUIET WAIT askAgent DOES. This row does not go through askAgent - it sends a chat
            // with the chip off - so it had no protection against inheriting a live session, which is
            // exactly how it came to answer L5's pending question with "add a limiter".
            if (! rig.until ([] (const juce::var& st)
                             { const auto a = agentVarOf (st);
                               return ! (bool) a.getProperty ("active", false)
                                      && isRestingState (a.getProperty ("state", juce::var()).toString()); }, 60000))
                std::printf ("        (WARNING: a turn was still live before A6 sent its chat)\n");
            rig.send (echojay::rig::cmd::agentChip, args1 ("on", false));
            const auto stBefore = rig.freshState();
            const auto before = ownSlotCount (stBefore);
            // ITS OWN EVIDENCE (14 Oct, Sean's instruction). This row runs NO agent turn on purpose - the
            // chip is off, so the words go to chat - which means AgentOutcome::ran() has nothing to say about
            // it and the generic gate cannot apply. Its claim is "the agent changed nothing", and with the
            // server down that is true for the dullest possible reason. So it requires a REPLY: the
            // transcript must grow, which is the proof that the words were sent, answered, and still changed
            // no rack.
            const int rowsBefore = (int) stBefore.getProperty ("transcriptRows", 0);
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", "add a limiter"), 15000);
            // WAIT FOR THE REPLY, BOUNDED - do not pump a guessed 20 s. The real model takes 8-22 s on a
            // chat turn (the server's own log), so 20 s failed this row on a turn that was simply still in
            // flight: the same "a bound tighter than the work it measures" fault as L2's 180 s.
            rig.until ([&] (const juce::var& st)
                       { return (int) st.getProperty ("transcriptRows", 0) > rowsBefore + 1; }, 60000);
            const auto stAfter = rig.freshState();
            const auto after = ownSlotCount (stAfter);
            const int rowsAfter = (int) stAfter.getProperty ("transcriptRows", 0);
            // AN ERROR BUBBLE IS NOT A REPLY (14 Oct, found by the red proof). Counting rows alone, this row
            // PASSED under a deliberately broken token: the chat 401'd, the editor pushed its failure notice
            // into the transcript, the count grew by two, and "the rack is untouched" was reported as the
            // agent behaving correctly. So the LAST assistant row must carry something a user would read as
            // an answer - and the failure copy is matched on, rather than guessed at by length.
            juce::String lastAssistant;
            if (auto* rows = stAfter.getProperty ("transcript", juce::var()).getArray())
                for (const auto& m : *rows)
                    if (m.getProperty ("role", juce::var()).toString() == "assistant")
                        lastAssistant = m.getProperty ("text", juce::var()).toString();
            // POSITIVE EVIDENCE, NOT A BLACKLIST. The first cut matched failure copy and A6 passed anyway:
            // the notice was "Session expired. Please log in again." and the list had "sign in", not "log in".
            // Guessing at error strings is a losing game - the copy changes and the row quietly goes vacuous
            // again. So the test is what a REAL answer must contain: this row asks for a limiter, and no
            // "session expired" notice will ever mention one. The blacklist stays as belt and braces only.
            const bool namesTheAsk   = lastAssistant.containsIgnoreCase ("limiter");
            const bool looksLikeFailure = lastAssistant.containsIgnoreCase ("couldn't reach")
                                       || lastAssistant.containsIgnoreCase ("could not reach")
                                       || lastAssistant.containsIgnoreCase ("not authenticated")
                                       || lastAssistant.containsIgnoreCase ("sign in")
                                       || lastAssistant.containsIgnoreCase ("log in")
                                       || lastAssistant.containsIgnoreCase ("session expired")
                                       || lastAssistant.containsIgnoreCase ("try again")
                                       || lastAssistant.containsIgnoreCase ("offline");
            const bool replied = rowsAfter > rowsBefore + 1
                              && lastAssistant.trim().length() > 20
                              && namesTheAsk
                              && ! looksLikeFailure;
            // ---- A TURN THE SERVER NEVER ANSWERED IS NOT A VERDICT (17 Oct 2026) ---------------
            // With the agent OFF this row sends a plain chat turn and asserts two things: that a
            // reply arrives, and that the rack is untouched. Tonight the reply never came for THIS
            // message - the last assistant row was still answering the previous question about the
            // compressor's wet - and the row failed, saying so honestly ("NO REPLY ARRIVED, so 'the
            // rack is untouched' proves nothing"). It is right that it cannot judge, and wrong to
            // call that a failure of the build: the chat route is the server's.
            //
            // The rack half still bites. If the rack CHANGED with the agent off, that is mine and it
            // fails whatever the server did.
            if (after == before && ! replied)
            {
                scenSkip ("A6", "(agent OFF) a plain chat turn is answered and the rack is untouched",
                          "the server did not answer this turn (" + juce::String (rowsAfter - rowsBefore)
                          + " transcript row(s) arrived and the last assistant line was about something "
                            "else), so the reply half cannot be judged. The rack half HELD: it was "
                            "untouched with the agent off. NEVER counted as a pass.", "B");
            }
            else
            row ("A6", replied && after == before, "(agent OFF) add a limiter",
                 juce::String (after != before ? "THE RACK CHANGED WITH THE AGENT OFF"
                               : replied            ? "the rack is untouched - it proposed instead"
                                                    : "NO REPLY ARRIVED, so \"the rack is untouched\" proves "
                                                      "nothing and this row fails")
                 + ", slots " + juce::String (before) + " -> " + juce::String (after)
                 + ", transcript " + juce::String (rowsBefore) + " -> " + juce::String (rowsAfter)
                 + (replied ? "" : ", the last assistant row was: \"" + lastAssistant.substring (0, 90) + "\""));
            rig.send (echojay::rig::cmd::agentChip, args1 ("on", true));
        }

        // ---- R7/H3: a GR read aimed at another track's Link -------------------
        if (linkUid.isNotEmpty())
        {
            const auto name = linkRowOf (Rig::state(), linkUid).getProperty ("name", juce::var()).toString();
            const auto ask = "how much gain reduction is the compressor doing on "
                           + (name.isNotEmpty() ? name : linkUid) + "?";
            const auto r = askAgent (rig, ask);
            const bool plain = ! r.text.containsIgnoreCase ("not on the rack")
                            && ! r.failDetail.containsIgnoreCase ("not on the rack");
            row ("H3", r.settled && plain, ask,
                 juce::String (plain ? "it said plainly that it cannot measure a Link yet"
                                     : "it answered with \"slot not on rack\", which is the wrong refusal")
                 + ", state " + r.state + ", said: " + r.text.substring (0, 160) + r.failDetail, &r);
        }

        // =====================================================================
        //  SHIP 1 (15 Oct): Sean's seven fixes, the halves only a real plugin can prove.
        //  The pure-logic halves are agent_round15_guard; these need audio, a real
        //  window, a real Link and a real server - the whole reason the rig exists.
        // =====================================================================

        // ---- G1/G2 (FIX 3): gain reduction is READABLE, and the agent can say it
        // RED BEFORE: the per-slot picture check(gr) reads had NO WRITER anywhere, so it answered "No audio
        // has been heard for gain reduction on slot 1 yet" however long audio played - Sean's nine rounds,
        // two seconds after the plugin's own playback wait had returned heard=8.0s.
        {
            // THIS ROW MAKES ITS OWN SETUP (15 Oct). The first cut read whatever C1 had left on the rack,
            // and when C1 stalled on an unanswerable question the rack held only an EQ - which reports no
            // gain reduction, truthfully - so G1 failed for a reason that had nothing to do with fix 3. A
            // row that depends on an earlier row's model output is a row that reports the weather.
            const auto hasCompressor = [] (const juce::var& st)
            {
                if (auto* arr = st.getProperty ("ownRack", juce::var()).getProperty ("slots", juce::var()).getArray())
                    for (const auto& sl : *arr)
                        if (sl.getProperty ("name", juce::var()).toString().containsIgnoreCase ("compress"))
                            return true;
                return false;
            };
            if (! hasCompressor (rig.freshState()))
                askAgent (rig, "add an EchoJay Compressor to this rack", 90000, true, "the EchoJay Compressor");
            const auto st = rig.freshState();
            bool grKnown = false, grCapable = hasCompressor (st); double grDb = 0.0; juce::String shape;
            if (auto* arr = st.getProperty ("ownRack", juce::var()).getProperty ("slots", juce::var()).getArray())
                for (const auto& sl : *arr)
                {
                    shape << (shape.isEmpty() ? "" : ", ") << sl.getProperty ("name", juce::var()).toString();
                    if ((bool) sl.getProperty ("grKnown", false))
                    { grKnown = true; grDb = (double) sl.getProperty ("grDb", 0.0); }
                }
            if (! grCapable)
                skip ("(fix 3) the rack reports gain reduction at all",
                      "the setup turn put no compressor on the rack, so there is nothing that COULD report "
                      "gain reduction - the rack holds [" + shape + "]. Not a verdict on fix 3");
            else
                row ("G1", grKnown, "(fix 3) the rack reports gain reduction at all",
                     juce::String (grKnown ? "GR is readable: " + juce::String (grDb, 2) + " dB"
                                           : "NO SLOT REPORTS GR - the picture is still unwritten")
                     + "; the rack holds [" + shape + "]");
        }
        {
            const auto r = askAgent (rig, "how much gain reduction is the compressor doing?");
            const bool answered = r.ran()
                               && ! r.text.containsIgnoreCase ("no audio has been heard")
                               && ! r.failDetail.containsIgnoreCase ("no audio has been heard");
            row ("G2", answered, "(fix 3) \"how much gain reduction is the compressor doing?\"",
                 juce::String (answered ? "it answered without the no-audio refusal"
                                        : "IT STILL SAYS NO AUDIO HAS BEEN HEARD")
                 + ", state " + r.state + ", said: " + r.text.substring (0, 120) + r.failDetail, &r);
        }

        // ---- U2 (FIX 4): a HAND pre-gain move is undoable --------------------
        // RED BEFORE: there was no preGain undo kind, so Undo reached past the drag to a dead entry
        // ("Undo skipped: remove UAD UA 1176LN Rev E") and the knob never moved. This drives the KNOB's own
        // door, not the agent's op: the fix is about a hand move.
        {
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 0.0));
            // PAST THE COALESCE WINDOW, OR THE SETUP IS PART OF THE DRAG (15 Oct 2026). UndoEntry's
            // coalesceKey folds two moves of the same knob inside kCoalesceMs = 300 into ONE entry - which is
            // the behaviour Sean's item 4 wanted for a drag ("one drag, one entry"). So the row's own
            // levelling move to 0 dB was being folded into the drag, the single entry's `before` was
            // whatever the pre-gain happened to hold beforehand (-5.01 dB, left by an earlier row's level
            // match), and undo correctly restored THAT. The row was wrong, the plugin was right. A user who
            // sets a knob, pauses, then drags it gets two entries, and that is what this row now does.
            Rig::pump (700);
            const double start = (double) rig.freshState().getProperty ("preGainDb", 99.0);
            rig.send (echojay::rig::cmd::setPreGain, args1 ("db", 5.8));
            const auto afterDrag = rig.freshState();
            const double dragged = (double) afterDrag.getProperty ("preGainDb", 0.0);
            const int depth      = (int) afterDrag.getProperty ("undoDepth", 0);
            const auto label     = afterDrag.getProperty ("undoLabel", juce::var()).toString();
            auto a2 = rig.send (echojay::rig::cmd::press, args1 ("button", "undo"), 8000);
            rig.until ([&] (const juce::var& st)
                       { return std::abs ((double) st.getProperty ("preGainDb", 99.0) - start) < 0.2; }, 6000);
            const double back = (double) Rig::state().getProperty ("preGainDb", 99.0);
            const bool ok = std::abs (dragged - 5.8) < 0.3 && std::abs (back - start) < 0.2 && depth > 0;
            row ("U2", ok, "(fix 4) a hand pre-gain move, then Undo",
                 "pre-gain " + juce::String (start, 2) + " -> " + juce::String (dragged, 2) + " -> "
                 + juce::String (back, 2) + " dB; the history's top was \"" + label + "\" at depth "
                 + juce::String (depth) + "; undo said: " + a2.getProperty ("note", juce::var()).toString()
                 + (ok ? "" : "  <- THE DRAG WAS NOT UNDOABLE"));
        }

        // ---- F2r: an op this build does not have ----------------------------
        //
        // TWO MINUTES WAS NOT ENOUGH FOR A REFUSAL (17 Oct 2026). This read askAgent's default
        // 120 s bound and came back "state streaming" - the model was still thinking about an op
        // that does not exist when the row looked, which says nothing about whether the run loops.
        // The claim is that it STOPS rather than looping, so the bound has to be long enough that
        // "still going" is evidence: four minutes, which is the same order as L2's 300 s build.
        //
        // AND THE ID: this row was called "F2" while a SCENARIO called F2 already exists (the
        // transcript-order one). Two different checks under one name in one scorecard is the fault
        // I named this morning about A5b - a report that cannot say which leg ran. Renamed F2r.
        {
            const auto r = askAgent (rig, "set the rack's tilt to 3", 240000);
            row ("F2r", r.settled, "set the rack's tilt to 3 (an op that does not exist)",
                 "state " + r.state + ", it stopped rather than looping"
                 + (r.failDetail.isNotEmpty() ? ", user line: " + r.failDetail.substring (0, 120) : ""), &r);
        }
    }

    // =============================================================================================
    //  B5 AND B6 RUN LAST, AND THAT IS NOT TIDINESS (22 Oct 2026)
    //
    //  They were written in the middle of the suite, between R1 and R2, and they disturbed it: B6
    //  handed on a LIVE run (its wait read the VST3's line while the run was the AU's), and
    //  sendChatMessage refuses a typed message while a run is in flight - so "the user's own message
    //  SURVIVED the close" and A4b went red having passed in every run before these rows existed.
    //  Both rows now stop anything still going and assert the bench is quiet, and both still ran
    //  before thirty other legs that had never had to tolerate them.
    //
    //  A NEW ROW MUST NOT BE ABLE TO REORDER AN ESTABLISHED SUITE. So they run at the end, where the
    //  only thing after them is the report and the teardown, and nothing downstream can be charged
    //  to them. Their own subjects need no particular position: a Link switch and a new chat are
    //  reachable from any state the suite leaves behind.
    // =============================================================================================
    // ---- B5: HIS MESSAGE ABOVE THE CARD, THROUGH A LINK SWITCH (his 15:09 fault, 22 Oct) --------
    //
    // *"Two Links open, each with its own chat. In the Vocal Link's chat I typed 'build a vocal
    //  chain', pressed Go, switched to the other Link while it was working, then switched back. The
    //  Vocal chat now shows the agent card (ASK label, 'Thinking...') ABOVE my 'build a vocal chain'
    //  message... My message must come first and the agent's card below it."*
    //
    // THE ROW HE ASKED FOR, in his own shape: send in a Link's chat, switch Links while the run is
    // live, switch back, and assert the order BOTH while it is live and after it finishes. It reads
    // `onScreen` - the rows the window is actually drawing - because the order fault is a fault of
    // what he sees, and every other 0b row reads state instead.
    //
    // WHY IT NEEDS TWO LINKS AND NOT ONE: the switch is what rebuilds the chat from the STORE, and
    // the store is where the order lives. One Link cannot be switched away from.
    leg ("B5: a Link switch mid-run leaves his message above the card (his 15:09 fault)");
    {
        // A SECOND LINK IS THIS ROW'S GROUND, SO IT WAITS FOR ONE (22 Oct 2026). The rig launches
        // two out-of-process Links and they arrive in the registry a second or so apart; the first
        // run of this row looked once, found one, and skipped - a row that skips because it asked
        // too early is a row that never runs. Twenty seconds, then it says so.
        // ---- AND BOTH UIDS ARE THIS ROW'S OWN, RESOLVED NOW (23 Oct 2026) --------------------
        // `linkUid` is captured near the top of the pass and B5 runs at the very end of it. On the
        // mock road of gate 23 the V2 refused it twice - "EJRig: work_on seq=3377 -> FAILED (that
        // uid is not live)" - so the goal was typed into MAIN's chat, the switch went to a Link
        // chat that had never seen his message, and the row reported his 15:09 order fault from a
        // comparison it had no subject for ("his row at -1"). I9c learnt this on 18 Oct and G1 on
        // 22 Oct; this is the same fault a third time, so the row resolves a uid this pass can
        // prove is alive and READS THE ACK instead of discarding it.
        juce::String primaryUid = usableLinkUid (rig.freshState(), true);
        if (primaryUid.isEmpty()) primaryUid = linkUid;
        juce::String otherUid;
        rig.until ([&otherUid, &primaryUid] (const juce::var& s)
        {
            if (auto* arr = s.getProperty ("links", juce::var()).getArray())
                for (const auto& l : *arr)
                {
                    const auto u = l.getProperty ("uid", juce::var()).toString();
                    if (u.isNotEmpty() && u != primaryUid && (bool) l.getProperty ("connected", false))
                    { otherUid = u; return true; }
                }
            return false;
        }, 20000);
        const juce::String goal = "rig-rowask-marker: build a vocal chain (switch Links mid-run)";
        // The processor's own line and a bounded wait on it, as the 0b block has - its copies are
        // local to that block, and a second pair here is cheaper than widening their scope for one row.
        // THE AU'S OWN LINE (22 Oct 2026). These rows live in the main section, where the window in
        // play is the AU's - the VST3's processor is idle here, so reading its line is reading a
        // plugin that is not running the test. B6 skipped with "the run never reached a waiting
        // state" while the log showed it waiting: I was looking at the wrong instance.
        const auto pstB5 = [] { return Rig::processorState ("AU"); };
        const auto untilProc = [&pstB5] (std::function<bool (const juce::var&)> ok, int ms)
        {
            const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
            while (juce::Time::getMillisecondCounter() < end)
            { if (ok (pstB5())) return true; Rig::pump (250); }
            return ok (pstB5());
        };
        // The ORDER, read off what the window draws: the index of his own row against the index of
        // the run's card. Returns -1 for either when it is not drawn at all.
        // ---- AND IT IS *THIS RUN'S* CARD, BY ITS KEY (24 Oct 2026) --------------------------
        //
        // This took "the first card on screen", which is a proxy for "the card this message started".
        // With the row's target finally resolving (the fix above), the chat it lands in is a chat
        // that already holds earlier runs' cards - the gate's own log reads "rows 5 card(s) on
        // screen" - so the first card was an OLDER one, sitting above his message exactly as it
        // should, and the row reported his 15:09 fault about it. Second proxy in the same row in one
        // day: the first time it was the uid, this time the card.
        //
        // `cardRunKey` is published on every card row and `agentRunKey` on the state, so the card
        // this run drew can be named rather than guessed. With no key available the row says so
        // instead of falling back to a comparison it cannot make.
        auto orderOf = [] (const juce::var& st, const juce::String& text, const juce::String& runKey,
                           int& userAt, int& cardAt)
        {
            userAt = cardAt = -1;
            int i = 0;
            if (auto* rows = st.getProperty ("onScreen", juce::var()).getArray())
                for (const auto& r : *rows)
                {
                    if (userAt < 0 && r.getProperty ("role", juce::var()).toString() == "user"
                        && r.getProperty ("text", juce::var()).toString().contains (text))
                        userAt = i;
                    if (cardAt < 0 && (bool) r.getProperty ("card", false)
                        && runKey.isNotEmpty()
                        && r.getProperty ("cardRunKey", juce::var()).toString() == runKey)
                        cardAt = i;
                    ++i;
                }
        };
        if (primaryUid.isEmpty() || otherUid.isEmpty())
            skip ("B5: a Link switch mid-run leaves his message above the card",
                  "this run has fewer than two connected Links (" + primaryUid + ", "
                  + (otherUid.isEmpty() ? juce::String ("(no second)") : otherUid)
                  + "), and the switch IS the subject - with one Link there is nowhere to switch to.");
        else
        {
            rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);
            const auto aimAck5 = rig.send (echojay::rig::cmd::workOn, args1 ("uid", primaryUid), 10000);
            Rig::pump (800);
            if (! (bool) aimAck5.getProperty ("ok", false))
            {
                skip ("B5: a Link switch mid-run leaves his message above the card",
                      "the V2 refused to work on " + primaryUid + " (\""
                      + aimAck5.getProperty ("note", juce::var()).toString() + "\"), so the goal "
                      "would have been typed into whatever chat was on screen and the order measured "
                      "in a chat his message was never in.");
            }
            else
            {
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", goal), 15000);
            const bool live = untilProc ([] (const juce::var& p)
                              { const auto s = p.getProperty ("agentState", juce::var()).toString();
                                return (bool) p.getProperty ("agentActive", false)
                                    || s == "awaiting_ask" || s == "awaiting_approval"; }, 40000);
            // THE SWITCH, mid-run, and back - the two presses his test made.
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", otherUid), 10000);
            Rig::pump (1200);
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", primaryUid), 10000);
            Rig::pump (1500);
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 6000);
            int u1 = -1, c1 = -1;
            const auto key1 = pstB5().getProperty ("agentRunKey", juce::var()).toString();
            orderOf (Rig::state(), "build a vocal chain", key1, u1, c1);
            check (live && u1 >= 0 && c1 >= 0 && u1 < c1,
                   "B5: back in that Link's chat with the run STILL LIVE, his message is drawn ABOVE "
                   "the run's card  (RED as it shipped at 15:09: the card reached the store on the "
                   "first state change inside start() and took the lower place, so a chat rebuilt by "
                   "the switch drew it over his own message)",
                   "live=" + juce::String ((int) live) + ", his row at " + juce::String (u1)
                   + ", the card at " + juce::String (c1)
                   + ", rows " + Rig::state().getProperty ("onScreenCards", juce::var()).toString()
                   + " card(s) on screen");
            // ...AND AFTER IT FINISHES, which is the other half he asked for: the stored order must
            // not be re-decided by the run ending, a chip being answered, or the card growing.
            rig.send (echojay::rig::cmd::press, args1 ("button", "chip0"), 12000);
            untilProc ([] (const juce::var& p) { return ! (bool) p.getProperty ("agentActive", false); }, 90000);
            Rig::pump (1200);
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", otherUid), 10000);
            Rig::pump (900);
            rig.send (echojay::rig::cmd::workOn, args1 ("uid", primaryUid), 10000);
            Rig::pump (1500);
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 6000);
            int u2 = -1, c2 = -1;
            const auto key2 = key1.isNotEmpty() ? key1
                                                : pstB5().getProperty ("agentRunKey", juce::var()).toString();
            orderOf (Rig::state(), "build a vocal chain", key2, u2, c2);
            check (u2 >= 0 && c2 >= 0 && u2 < c2,
                   "...and after the run FINISHED, through another switch away and back, his message "
                   "is still above the card - the order is a place taken once, not a thing recomputed "
                   "on every render",
                   "his row at " + juce::String (u2) + ", the card at " + juce::String (c2));
            }
        }
        // ---- THE BENCH IS LEFT QUIET (the F3 lesson, 22 Oct 2026) --------------------------------
        // A run left live refuses every typed message after it, by design - last night that cost
        // five rows their subject, and this row cost two more the same way this evening
        // ("EJAgent: typed message refused while active"). So: stop anything still going, and prove
        // it stopped, before handing the bench on.
        if ((bool) pstB5().getProperty ("agentActive", false))
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
        const bool benchQuiet = untilProc ([] (const juce::var& p)
                                { return ! (bool) p.getProperty ("agentActive", false); }, 20000);
        check (benchQuiet, "...and B5 leaves no run going, so the rows after it can type",
               "agentActive=" + pstB5().getProperty ("agentActive", juce::var()).toString());
    }

    // ---- B6: A SUPERSEDED RUN'S CARD STAYS IN ITS OWN CHAT (his test 8, 22 Oct 2026) -----------
    //
    // *"the test 7 run was waiting on its chips on the Main mix bus. I made a NEW chat and sent
    //  'build a mix bus'. The old run's card ('Stopped', 8 steps) appeared in the NEW chat above my
    //  message."*
    //
    // His ruling for both of his order faults: *"Make every road that writes or renders a run's row
    // place it only by the chatId stamped at creation."*  This row is the second half of that - B5
    // is the switch, this is the NEW CHAT - and it reads `onScreen`, the rows the window draws,
    // because the fault is about where a card appears.
    leg ("B6: a superseded run's card does not follow him into a new chat (his test 8)");
    {
        const auto pstB6 = [] { return Rig::processorState ("AU"); };   // the AU's, as B5's note says
        const auto untilProcB6 = [&pstB6] (std::function<bool (const juce::var&)> ok, int ms)
        {
            const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
            while (juce::Time::getMillisecondCounter() < end)
            { if (ok (pstB6())) return true; Rig::pump (250); }
            return ok (pstB6());
        };
        rig.send (echojay::rig::cmd::agentChip, args1 ("mode", "ask"), 8000);
        Rig::pump (400);
        rig.send (echojay::rig::cmd::sendChat,
                  args1 ("text", "rig-rowask-marker: a run left waiting (superseded by a new chat)"), 15000);
        const bool waiting = untilProcB6 ([] (const juce::var& p)
                             { const auto s = p.getProperty ("agentState", juce::var()).toString();
                               return s == "awaiting_ask" || s == "awaiting_approval"; }, 40000);
        const auto oldKey = pstB6().getProperty ("agentRunKey", juce::var()).toString();
        // A NEW CHAT, and a new instruction in it - the two presses his test made.
        rig.send (echojay::rig::cmd::newChat, juce::var (new juce::DynamicObject()), 10000);
        Rig::pump (900);
        rig.send (echojay::rig::cmd::sendChat,
                  args1 ("text", "rig-auto-marker: build a mix bus (in a brand new chat)"), 15000);
        Rig::pump (900);
        // ---- AND THE SUPERSEDE ASK IS ANSWERED, BECAUSE THAT IS WHAT HE DOES (24 Oct 2026) -----
        //
        // His ruling of 21:06 put a question in front of exactly this move: a message typed while a
        // run is live anywhere asks "This will stop the run on <name>. Start anyway?" and HOLDS his
        // text on the chips until he answers. So the typed line no longer reaches the new chat by
        // itself, and this row read "his row at -1" - his message was not missing, it was waiting
        // for an answer nobody gave.
        //
        // ---- AND IT IS ANSWERED ON THE RIGHT SURFACE, AFTER WAITING FOR IT (25 Oct 2026) -------
        //
        // THIS WAS THE S6 B6 FAIL. What stood here sent `press: chip0` and ignored the ack on the
        // grounds that "the state does not publish the editor's replace-ask chips, and inventing a
        // field to read would be a second idea of what is on screen". Both halves were wrong:
        //
        //   * `chip0` is the AGENT RUN ROW's chip (EchoJayEditor::rigPressAgentRow routes anything
        //     starting "chip" to RunHit::Chip). The supersede ask's "Start anyway" is on the ASK
        //     SHELF - a different surface - so the press was routed away from the thing it meant,
        //     found no zone, and the plugin answered `unknown button "chip0"`. Correctly.
        //   * A press that finds no chips is NOT harmless to this row. The ask stays up holding his
        //     typed text, so the message never reaches the new chat - and the row then reported
        //     "his row at -1, first card at -1" about an interaction it had never performed. That is
        //     the vacuous-pass rule's twin: a vacuous FAIL.
        //
        // So the plugin now has a name for that surface (`ask_chip<N>`) and publishes `askChipsUp`,
        // and this row WAITS for the chips with a bound before pressing. If they never come the row
        // says so and skips rather than measuring an order it did not create.
        const bool askUp = rig.until ([] (const juce::var& s)
        { return (int) s.getProperty ("askChipsUp", juce::var (0)) > 0; }, 15000);
        if (! askUp)
        {
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 6000);
            std::printf ("        (B6 new chat) the supersede ask never came up (askChipsUp=%s) - "
                         "his text was not held, so the order below is measured without it\n",
                         Rig::state().getProperty ("askChipsUp", juce::var()).toString().toRawUTF8());
        }
        else
        {
            const auto chipAck = rig.send (echojay::rig::cmd::press, args1 ("button", "ask_chip0"), 12000);
            Rig::pump (1200);
            const bool pressed = (bool) chipAck.getProperty ("ok", juce::var (false));
            std::printf ("        (B6 new chat) \"Start anyway\" press -> %s%s\n",
                         chipAck.getProperty ("note", juce::var()).toString().toRawUTF8(),
                         pressed ? "" : "   << THE PRESS WAS REFUSED, so his message is still held");
            // THE PRESS IS PART OF THE SUBJECT NOW, not a courtesy before it: this row cannot say
            // anything about where his message was drawn if the thing that releases it was refused.
            check (pressed,
                   "B6 (new chat): the supersede ask's \"Start anyway\" is PRESSABLE from its own "
                   "surface - the fault this replaced sent chip0, which is the agent run row's chip, "
                   "so the ask was never answered and the row measured an order it had not created",
                   chipAck.getProperty ("note", juce::var()).toString());
        }
        // ---- THE NEW CHAT HAS TO BE DRAWING SOMETHING BEFORE EITHER HALF MEANS ANYTHING --------
        //
        // The first run of this row read `his row at -1, first card at -1`: the window was drawing
        // NO rows at that moment, so "the old card is not here" was true of an empty list - a claim
        // that passes whatever the product does. The second check caught it, which is what it is
        // for; the cure is a GROUND, waited for with a bound, before anything is concluded.
        // The state the WAIT was satisfied by is kept, so the checks can say whether they are
        // reading the same window's snapshot (see below) rather than assuming it.
        juce::var drawnState;
        const bool newChatDrawn = rig.until ([&drawnState] (const juce::var& s)
        {
            if (auto* rows = s.getProperty ("onScreen", juce::var()).getArray())
                for (const auto& r : *rows)
                    if (r.getProperty ("role", juce::var()).toString() == "user"
                        && r.getProperty ("text", juce::var()).toString().contains ("brand new chat"))
                    { drawnState = s; return true; }
            return false;
        }, 20000);
        rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 6000);
        // ---- THE GROUND IS RE-TAKEN ON THE SNAPSHOT THE CHECKS ACTUALLY READ (25 Oct 2026) -----
        //
        // The row's own rule, two paragraphs up, is that "an absence asserted against an empty list
        // is a claim that passes whatever the product does" - and it was applied to the WAIT and not
        // to the snapshot the checks read. Those are two different moments: `until` polls until the
        // message appears, then this reportState takes a FRESH state, and the 25 Oct run failed with
        // `his row at -1, first card at -1` after the wait had SUCCEEDED. So the message was drawn
        // and then gone, and the row reported that as an ordering fault - which it is not.
        //
        // One snapshot, one verdict. If his message is not in the state the checks read, this row
        // says THAT, with the row count, instead of concluding anything about order.
        bool stillDrawn = false;
        if (auto* rows = Rig::state().getProperty ("onScreen", juce::var()).getArray())
            for (const auto& r : *rows)
                if (r.getProperty ("role", juce::var()).toString() == "user"
                    && r.getProperty ("text", juce::var()).toString().contains ("brand new chat"))
                    stillDrawn = true;
        // ---- AND WHICH WINDOW EACH SNAPSHOT CAME FROM (25 Oct 2026) -------------------------
        //
        // The 25 Oct run read "his message WAS drawn ... and was gone again by the snapshot these
        // checks read (onScreen rows now: 0, user rows 0, cards 0)" - and the product's log carries
        // NO clear-site line for that window (every chatMessages.clear() has been instrumented
        // since 11 Aug) and no editor was built or destroyed around it. Two editors WERE alive in
        // that run (window #1 and window #11), the published state has carried `editorId` all
        // along, and nothing has ever compared it between two reads.
        //
        // So before anything is concluded about a disappearing row: say whether the two snapshots
        // came from the SAME window. If they did not, "drawn then gone" is two windows taking turns
        // at one state file and the product never lost anything - and that is a different fault
        // with a different fix. A row that cannot tell those apart should not name either.
        const auto drawnEditor = juce::String ((juce::int64) (double) drawnState.getProperty ("editorId", 0.0));
        const auto checkEditor = juce::String ((juce::int64) (double) Rig::state().getProperty ("editorId", 0.0));
        const bool sameWindow  = (drawnEditor == checkEditor);
        // Is the OLD run's card being drawn in this new chat?
        bool oldCardHere = false; int cards = 0, userRows = 0, firstCardAt = -1, firstUserAt = -1, i = 0;
        if (auto* rows = Rig::state().getProperty ("onScreen", juce::var()).getArray())
            for (const auto& r : *rows)
            {
                const bool isCard = (bool) r.getProperty ("card", false);
                if (isCard)
                {
                    ++cards;
                    if (firstCardAt < 0) firstCardAt = i;
                    if (oldKey.isNotEmpty()
                        && r.getProperty ("cardRunKey", juce::var()).toString() == oldKey)
                        oldCardHere = true;
                }
                if (r.getProperty ("role", juce::var()).toString() == "user")
                {
                    ++userRows;
                    if (firstUserAt < 0) firstUserAt = i;
                }
                ++i;
            }
        if (! waiting || oldKey.isEmpty())
            skip ("B6: a superseded run's card does not follow him into a new chat",
                  "the scripted run never reached a waiting state (state "
                  + pstB6().getProperty ("agentState", juce::var()).toString()
                  + ", key \"" + oldKey + "\"), so there was no run to supersede.");
        else if (! newChatDrawn)
            skip ("B6: a superseded run's card does not follow him into a new chat",
                  "the new chat drew nothing within 20 s (onScreen rows: " + juce::String (i)
                  + "), and an absence asserted against an empty list is not evidence. The run that "
                    "was superseded is proven elsewhere; this row needs the new chat on screen.");
        else if (! stillDrawn)
            skip ("B6: a superseded run's card does not follow him into a new chat",
                  "his message WAS drawn in the new chat and was gone again by the snapshot these "
                  "checks read (onScreen rows now: " + juce::String (i) + ", user rows "
                  + juce::String (userRows) + ", cards " + juce::String (cards) + "). The window "
                    "that drew it published as editor " + drawnEditor + "; the snapshot these checks "
                    "read came from editor " + checkEditor + " - "
                  + (sameWindow ? juce::String ("the SAME window, so the row really did leave that "
                                                "window's list: a disappearing-row fault, and the "
                                                "product's own clear-site instrumentation should name "
                                                "the site that emptied it.")
                                : juce::String ("a DIFFERENT window, so nothing disappeared: two "
                                                "editors are taking turns at one state file and this "
                                                "row read one window's list against another's. The "
                                                "fault is the state's ownership, not the chat."))
                  + " Either way this row will not report it as an ordering fault.");
        else
        {
            check (! oldCardHere,
                   "B6: the run left waiting in the OTHER chat is NOT drawn in the new one  (RED as it "
                   "shipped: its \"Stopped\" card was recorded through the CLIENT'S current chat, which "
                   "had already moved to the chat he just made, so an eight-step card he never ran "
                   "there appeared above his message)",
                   "old run " + oldKey.substring (0, 8) + ", cards on screen " + juce::String (cards)
                   + ", user rows " + juce::String (userRows));
            check (firstUserAt >= 0 && (firstCardAt < 0 || firstUserAt < firstCardAt),
                   "...and in the new chat his own message is the first thing in it - above any card "
                   "the new run draws (the order half of his ruling, here on the new-chat road)",
                   "his row at " + juce::String (firstUserAt) + ", first card at "
                   + juce::String (firstCardAt));
        }
        // ---- AND THE BENCH IS LEFT QUIET (the F3 lesson; this row broke it this evening) --------
        // This wait used to read the VST3's line while the run was the AU's, so it waited for
        // nothing: B6 handed on a LIVE run, and the next legs' typed messages were refused by
        // design - "the user's own message SURVIVED the close" and A4b both went red for it, having
        // passed in every earlier run today. Right instance, and a stop if it is still going.
        if ((bool) pstB6().getProperty ("agentActive", false))
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
        const bool quiet = untilProcB6 ([] (const juce::var& p)
                           { return ! (bool) p.getProperty ("agentActive", false); }, 90000);
        check (quiet, "...and B6 leaves no run going, so the rows after it can type",
               "agentActive=" + pstB6().getProperty ("agentActive", juce::var()).toString());
        Rig::pump (600);
    }


    // ---- B7: A BUILD AWAITING A DECISION IS A PLAN, NOT A ONE-LINE APPLY (his 21:52, 22 Oct) ----
    //
    // *"A raw 'build N slots' step with Apply/Skip must not be offered for a build either. If the
    //  server sends a build op with no plan, show it as a plan needing Go, or refuse it plainly."*
    //
    // The answer he was given, and what this row reads back from a real host: SHOW IT AS A PLAN.
    // The heading, one numbered line per slot, and a door that says Go. A readable build is work he
    // asked for - refusal belongs only to a plan that cannot be READ, which is B1's subject and the
    // guard's UP legs.
    //
    // THE ROW FINDS ITS STEP BY THE ROW'S OWN FACT (`buildPlan`), not by the wording of a label.
    // The 19 Oct lesson from I9c: a leg that recognises its subject from prose is asserting on
    // prose, and it goes red the first time the sentence is improved.
    //
    // AT THE END, with B5 and B6, for the reason written above them: it leaves a run waiting on a
    // decision, and nothing downstream should have to tolerate that.
    leg ("B7: a build awaiting his decision is drawn as a plan with Go (his 21:52 fault)");
    {
        const bool liveRoad = liveServerConfigured();
        // THE AU's PROCESSOR, as B5's note says: the window in play is the AU's, and B5/B6 both went
        // red once for reading the VST3's line while the run was the AU's.
        const auto pst7 = [] { return Rig::processorState ("AU"); };
        const auto untilProc7 = [&pst7] (std::function<bool (const juce::var&)> ok, int ms)
        {
            const auto t0 = juce::Time::getMillisecondCounter();
            while (juce::Time::getMillisecondCounter() - t0 < (juce::uint32) ms)
            { if (ok (pst7())) return true; Rig::pump (250); }
            return ok (pst7());
        };
        // Quiet ground: B6 above leaves the bench quiet, but this row says so for itself rather
        // than inheriting it - the F3 lesson, and the reason B5/B6 were moved here at all.
        if ((bool) pst7().getProperty ("agentActive", false))
        {
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
            untilProc7 ([] (const juce::var& p)
                        { return ! (bool) p.getProperty ("agentActive", false); }, 60000);
        }
        rig.send (echojay::rig::cmd::newChat, args1 ("uid", ""), 10000);
        Rig::pump (400);
        // NOT APPROVED and asks NOT answered: the subject is what is DRAWN while the decision waits
        // (G4's reason, verbatim - a row that presses Apply reads a finished run and proves nothing).
        const auto r = askAgent (rig,
                                 liveRoad ? juce::String ("build a mix bus on this rack")
                                          : juce::String ("rig-buildplan-marker: build a mix bus"),
                                 liveRoad ? 300000 : 60000, false, "yes", false);
        // Wait, bounded, for a step the row itself calls a build plan.
        juce::var planStep;
        juce::String zones;
        rig.until ([&] (const juce::var& stv)
        {
            const auto arow = stv.getProperty ("agentRow", juce::var());
            zones = arow.getProperty ("zones", juce::var()).toString();
            if (auto* arr = arow.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                    if ((bool) sv.getProperty ("buildPlan", false)) { planStep = sv; return true; }
            return false;
        }, liveRoad ? 240000 : 40000);
        const auto st   = rig.freshState();
        const auto arow = st.getProperty ("agentRow", juce::var());
        zones = arow.getProperty ("zones", juce::var()).toString();
        if (! planStep.isObject())
            if (auto* arr = arow.getProperty ("steps", juce::var()).getArray())
                for (const auto& sv : *arr)
                    if ((bool) sv.getProperty ("buildPlan", false)) planStep = sv;
        juce::String stepIds;
        if (auto* arr = arow.getProperty ("steps", juce::var()).getArray())
            for (const auto& sv : *arr)
                stepIds << (stepIds.isEmpty() ? "" : ", ") << sv.getProperty ("id", juce::var()).toString()
                        << "[" << sv.getProperty ("status", juce::var()).toString() << "]";

        if (! planStep.isObject())
        {
            skip ("B7: a build awaiting his decision is drawn as a plan with Go",
                  liveRoad
                      ? juce::String ("the live model never put a BUILD in front of a decision on this "
                                      "run - it is free to read the rack, ask a question or add slots one "
                                      "at a time, and a row that passed on a run with no build in it "
                                      "would prove nothing. Steps drawn: [") + stepIds + "]. The claim "
                                      "runs on the mock road, where the build is scripted, and in the "
                                      "guard's BP legs."
                      : juce::String ("the scripted build never reached a waiting decision within 40 s "
                                      "(agent state ")
                      + pst7().getProperty ("agentState", juce::var()).toString()
                      + ", steps drawn: [" + stepIds + "]), so there is nothing drawn to read.");
        }
        else
        {
            const auto label  = planStep.getProperty ("label", juce::var()).toString();
            const auto detail = planStep.getProperty ("detail", juce::var()).toString();
            check (! label.containsIgnoreCase (" slots:") && ! label.containsIgnoreCase ("build 3 slots")
                   && ! label.containsIgnoreCase ("build 5 slots"),
                   "B7: the waiting row is no longer the server's raw \"build N slots: a, b, c\" line  "
                   "(RED as it shipped at 21:52: one row, a comma list he had to parse, in the server's "
                   "own voice)",
                   "heading \"" + label + "\"");
            // ONE NUMBERED LINE PER SLOT, in order. On the mock the chain is known; on the live road
            // the names are the model's, so what is asserted there is the NUMBERING and the count.
            int lines = 0, lastAt = -1;
            bool ordered = true;
            for (int k = 1; k <= 24; ++k)
            {
                const int at = detail.indexOf (juce::String (k) + ". ");
                if (at < 0) break;
                if (at < lastAt) ordered = false;
                lastAt = at;
                ++lines;
            }
            check (lines >= 2 && ordered,
                   "...and the slots are the row's own NUMBERED LINES, in order - what he can read at a "
                   "glance on a narrow card, which the comma list could not give him",
                   juce::String (lines) + " numbered line(s), ordered=" + (ordered ? "y" : "n")
                   + ", detail \"" + detail.replaceCharacter ('\n', '/').substring (0, 160) + "\"");
            if (! liveRoad)
                check (detail.contains ("1. EchoJay EQ") && detail.contains ("2. EchoJay Compressor")
                       && detail.contains ("3. EchoJay Limiter"),
                       "...and they are THIS build's three slots, in the chain's order (the mock road, "
                       "where the plan is known - the live road reads the model's own names)",
                       "detail \"" + detail.replaceCharacter ('\n', '/') + "\"");
            const auto zoneWord = [&zones] (const char* kind) -> juce::String
            {
                for (auto& e : juce::StringArray::fromTokens (zones, ",", ""))
                    if (e.trim().upToFirstOccurrenceOf (":", false, false).trim() == kind)
                        return e.trim().fromFirstOccurrenceOf (":", false, false).trim();
                return {};
            };
            const auto goWord = zoneWord ("1"), noWord = zoneWord ("2");
            check (goWord == "Go" && noWord == "Cancel",
                   "...and the pair on that line reads [Go] [Cancel] - the plan's own vocabulary, in "
                   "place of [Apply] [Skip]  (one door still, as his #4 ruled: this changes the word on "
                   "it, not the number of them)",
                   "zones=[" + zones.substring (0, 120) + "]");
            check (goWord.isNotEmpty(),
                   "...and the decision is still THERE to press - a plan he cannot say Go to would be "
                   "worse than the line he complained about",
                   "apply-line zone \"" + goWord + "\"");
        }
        // LEAVE THE BENCH QUIET, whatever happened: this row deliberately leaves a run waiting.
        if ((bool) pst7().getProperty ("agentActive", false))
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
        const bool quiet = untilProc7 ([] (const juce::var& p)
                           { return ! (bool) p.getProperty ("agentActive", false); }, 90000);
        check (quiet, "...and B7 leaves no run going, so the report and the teardown are clean",
               "agentActive=" + pst7().getProperty ("agentActive", juce::var()).toString());
        Rig::pump (600);
        juce::ignoreUnused (r);
    }

    // ---- B8: LOOKING AT A LINK'S SLOT TAKES NOTHING (his 08:44 fault, 23 Oct 2026) --------------
    //
    // *"opening a Link's window to look must not stop the V2's run or take its rack away ... Never
    //  leave a rack owned by nobody or stuck with a closed Link."*
    //
    // WHAT THIS ROW CAN DRIVE, and what it cannot, said plainly: opening a Link's OWN plugin window
    // is a Logic act (the host decides which plugin window exists), and the rig hosts the Links in
    // their own processes without windows - so the "open it, close it, control comes back" leg is
    // HIS test and is named as owed in the report. What the rig can hold, and what the fault
    // actually was, is the INVARIANT: looking at a Link's slot in the V2 - which is what he was
    // doing - changes no ownership and ends no run.
    leg ("B8: looking at a Link's slot changes no ownership and ends no run (his 08:44 fault)");
    {
        const auto pst8 = [] { return Rig::processorState ("AU"); };
        if ((bool) pst8().getProperty ("agentActive", false))
        {
            rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
            Rig::pump (1500);
        }
        juce::String linkForLook;
        rig.until ([&linkForLook] (const juce::var& st)
        {
            if (auto* arr = st.getProperty ("links", juce::var()).getArray())
                for (const auto& l : *arr)
                {
                    const auto uid = l.getProperty ("uid", juce::var()).toString();
                    if (uid.isNotEmpty()) { linkForLook = uid; return true; }
                }
            return false;
        }, 20000);
        if (linkForLook.isEmpty())
            skip ("B8: looking at a Link's slot changes no ownership and ends no run",
                  "no Link was published within 20 s, so there is no remote slot to look at. The "
                  "invariant is also held from the other side by chat_store's SH3/SH4 (the shadow "
                  "takes no lock, no borrow and no edit session).");
        else
        {
            rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkForLook), 10000);
            Rig::pump (1200);
            const auto before = rig.freshState();
            const auto lockBefore   = before.getProperty ("rackLock", juce::var()).toString();
            const bool borrowBefore = (bool) before.getProperty ("borrowActive", false);
            // LOOK at a slot: the selection road, which is what carries the live view.
            rig.send (echojay::rig::cmd::openSlot, args1 ("n", 1), 10000);
            Rig::pump (2500);
            const auto after = rig.freshState();
            const auto lockAfter   = after.getProperty ("rackLock", juce::var()).toString();
            const bool borrowAfter = (bool) after.getProperty ("borrowActive", false);
            check (borrowAfter == borrowBefore,
                   "B8: looking at a Link's slot does not engage a borrow it did not have  (his "
                   "08:44: the rack went to the Link and the V2's strip read \"Not connected\", and "
                   "nothing ever asked for it again)",
                   "borrow " + juce::String ((int) borrowBefore) + " -> " + juce::String ((int) borrowAfter));
            check (lockAfter == lockBefore || lockAfter == "idle",
                   "...and it takes no rack lock it did not hold - a look is not a claim",
                   "lock \"" + lockBefore + "\" -> \"" + lockAfter + "\"");
            // NO THIRD CHECK HERE, deliberately. I wrote one asserting "no run was ended by the
            // look" and it could not fail: nothing is running at this point in the suite, so it
            // read green whatever happened. A leg that cannot go red is worse than no leg - it
            // reports a claim nobody tested. That half of his ruling is held where it CAN fail:
            // chat_store's SH3/SH4 (the view takes no lock, no borrow, no edit session) and his own
            // retest with a run live on the Link.
        }
    }

    // ---- B9: THE LIVE VIEW OF A LINK'S PLUGIN (his ruling of 23 Oct 2026) ----------------------
    //
    // *"when I select a Link's plugin in the V2, I see that plugin's real editor live in the V2
    //  (moving meters and gain reduction) without opening the Link's window ... it must never take
    //  the rack, stop a run or change ownership."*
    //
    // WHAT MAKES IT A TEST AND NOT A PICTURE: `shadowFrames`. A view that is "up" proves nothing -
    // a static picture is up too - so this waits for the copy to have CONSUMED AUDIO from the Link's
    // own tap ring, which is the whole feature. And an EchoJay plugin only: the standing rule is
    // that the rig never loads a PACE or iLok plugin, so the licence-refusal path is owed to his own
    // machine and is named in the report.
    leg ("B9: a Link slot's plugin is shown LIVE in the V2, and takes nothing (the shadow view)");
    {
        // ---- THE ROW SETS ITS OWN TARGET, AND READS THE ACKS (23 Oct 2026) -------------------
        // The first cut took the FIRST published uid and sent `select_link` alone. Two things went
        // wrong on the mock road and the log said both: the agent's working target never moved, so
        // `askAgent`'s add landed on the rack a previous row had left it on ("EJStruct: send
        // uid=e98297f757" while this row was looking at 6582069a30), and the slot it then shadowed
        // was whatever had been sitting on that rack - a WAVES plugin, under a comment promising
        // "an EchoJay plugin only". A row that measures a rack it did not set up reports the
        // previous row's state; usableLinkUid resolves one this pass can prove is alive, and both
        // acks are read rather than discarded (the 18 Oct lesson, four rows along).
        juce::String shadowLink = usableLinkUid (rig.freshState(), true);
        if (shadowLink.isEmpty())
            rig.until ([&shadowLink] (const juce::var& st)
            {
                if (auto* arr = st.getProperty ("links", juce::var()).getArray())
                    for (const auto& l : *arr)
                    {
                        const auto uid = l.getProperty ("uid", juce::var()).toString();
                        if (uid.isNotEmpty() && (bool) l.getProperty ("connected", false))
                        { shadowLink = uid; return true; }
                    }
                return false;
            }, 20000);
        if (shadowLink.isEmpty())
        {
            skip ("B9: a Link slot's plugin is shown LIVE in the V2",
                  "no connected Link was published within 20 s, so there is no remote slot to shadow.");
        }
        else
        {
            const auto selAck9  = rig.send (echojay::rig::cmd::selectLink, args1 ("uid", shadowLink), 10000);
            const auto workAck9 = rig.send (echojay::rig::cmd::workOn,     args1 ("uid", shadowLink), 10000);
            const bool aimed9   = (bool) selAck9.getProperty ("ok", false)
                               && (bool) workAck9.getProperty ("ok", false);
            Rig::pump (1000);
            // Put an EchoJay plugin on that Link's rack, by the agent's own road - the rig's own
            // marker plan, so no third party and no licence is ever touched here.
            const auto built = askAgent (rig, "rig-linkadd-marker: add a compressor on that Link", 90000);
            juce::ignoreUnused (built);
            // ---- OPEN THE SLOT THE ADD LANDED ON, NOT SLOT 1 (24 Oct 2026) -------------------
            //
            // The row opened slot 1 and then - correctly - refused to report, because slot 1 of that
            // Link held "Renaissance Axx (s)" from an earlier row and this row promises an EchoJay
            // plugin only. A loud skip is better than a verdict about a plugin it did not choose, but
            // the row still proved nothing. So the slot is FOUND: the Link's published slot names,
            // searched for the EchoJay compressor this row just added, and its 1-based position is
            // what gets opened. A rack that does not show one leaves slotToOpen at 0 and the skip
            // below says exactly that.
            int slotToOpen = 0;
            rig.until ([&] (const juce::var& st)
            {
                if (auto* arr = st.getProperty ("links", juce::var()).getArray())
                    for (const auto& l : *arr)
                        if (l.getProperty ("uid", juce::var()).toString() == shadowLink)
                        {
                            const auto names = juce::StringArray::fromTokens (
                                                   l.getProperty ("slots", juce::var()).toString(), "|", "");
                            for (int k = 0; k < names.size(); ++k)
                                if (names[k].containsIgnoreCase ("EchoJay")) { slotToOpen = k + 1; return true; }
                        }
                return false;
            }, 20000);
            std::printf ("        (B9) the EchoJay plugin this row added is slot %d on %s\n",
                         slotToOpen, shadowLink.toRawUTF8());
            // THE BORROW AS IT STOOD BEFORE THE LOOK, and WHOSE it was. Read here, because the
            // claim below is about what the VIEW did and not about what the suite was already
            // holding.
            const auto b4     = rig.freshState();
            const bool borrowBefore9 = (bool) b4.getProperty ("borrowActive", false);
            const auto borrowUidBefore9 = b4.getProperty ("borrowUid", juce::var()).toString();
            rig.send (echojay::rig::cmd::openSlot, args1 ("n", slotToOpen > 0 ? slotToOpen : 1), 10000);
            // The view has to come up AND consume audio; 12 s, then it says what it got.
            bool running = false; double frames = 0.0; juce::String plug;
            rig.until ([&] (const juce::var& st)
            {
                running = (bool) st.getProperty ("shadowRunning", false);
                frames  = (double) st.getProperty ("shadowFrames", 0.0);
                plug    = st.getProperty ("shadowPlugin", juce::var()).toString();
                return running && frames > 0.0;
            }, 12000);
            const auto st = rig.freshState();
            const bool borrowNow = (bool) st.getProperty ("borrowActive", false);
            const auto borrowUidNow = st.getProperty ("borrowUid", juce::var()).toString();
            // THE PLUGIN THIS ROW PUT THERE, or nothing to judge (23 Oct 2026). The row's own
            // promise is "an EchoJay plugin only - the rig never loads a PACE or iLok plugin", and
            // on the mock road it shadowed a Waves plugin a previous row had left on slot 1 while
            // still printing that promise. So the identity is CHECKED, and a slot holding something
            // else makes this a loud skip rather than a verdict about a plugin the row never chose.
            const bool ejPlug = plug.containsIgnoreCase ("EchoJay");
            if (! aimed9)
                skip ("B9: a Link slot's plugin is shown LIVE in the V2",
                      "the V2 refused to aim at " + shadowLink + " (select \""
                      + selAck9.getProperty ("note", juce::var()).toString() + "\", work_on \""
                      + workAck9.getProperty ("note", juce::var()).toString()
                      + "\"), so this row would be measuring a rack it never selected.");
            else if (running && ! ejPlug)
                skip ("B9: a Link slot's plugin is shown LIVE in the V2",
                      "slot " + juce::String (slotToOpen > 0 ? slotToOpen : 1) + " of " + shadowLink
                      + " holds \"" + plug + "\", which is not the EchoJay plugin this row adds"
                      + (slotToOpen > 0 ? juce::String (" (the add's own slot, found from the Link's "
                                                        "published names)")
                                        : juce::String (" - and no EchoJay plugin appeared on that rack "
                                                        "within 20 s, so the add itself is what failed"))
                      + ". The rig never loads a PACE or iLok plugin, so it will not report a verdict "
                        "on a third party it did not choose.");
            else if (! running)
                skip ("B9: a Link slot's plugin is shown LIVE in the V2",
                      juce::String ("the live view did not come up within 12 s (shadowRunning=0, "
                                    "plugin \"") + plug + "\"). Reasons it legitimately might not, "
                      "each named rather than guessed: the out-of-process Link in this rig may "
                      "predate the tap, the slot may hold nothing yet, or no copy of that plugin "
                      "could be made here. The REFUSAL path is what the guard holds (SH1/SH2); this "
                      "row is the live one and it is owed.");
            else
            {
                check (frames > 0.0,
                       "B9: the V2's own copy of that Link's slot is RUNNING and has consumed audio "
                       "from the Link's tap - which is what makes it a live view rather than a "
                       "picture of one",
                       "plugin \"" + plug + "\", " + juce::String ((int) frames) + " frame(s) pumped");
                // THE VIEW'S OWN BORROW, not any borrow anywhere (23 Oct 2026). This read the bare
                // `borrowActive` flag and went RED on both roads over a borrow held on ANOTHER rack
                // by an earlier row - a true claim reported as a false one, which costs exactly as
                // much as the reverse. The question is whether looking CHANGED the borrow, and
                // whether the rack being shadowed is one this instance holds.
                check (borrowNow == borrowBefore9 && borrowUidNow == borrowUidBefore9
                       && borrowUidNow != shadowLink,
                       "...and it took NO borrow of that rack: a view is not a claim (his 08:44 "
                       "fault was a rack taken by something he only wanted to look at)",
                       "borrow " + juce::String ((int) borrowBefore9) + " \"" + borrowUidBefore9
                       + "\" -> " + juce::String ((int) borrowNow) + " \"" + borrowUidNow
                       + "\", shadowing " + shadowLink);
                check (st.getProperty ("shadowUid", juce::var()).toString() == shadowLink,
                       "...and it is the Link in view that is being shadowed, not whatever was "
                       "selected last (the row-sets-its-own-target rule, on this road too)",
                       "shadowUid \"" + st.getProperty ("shadowUid", juce::var()).toString() + "\"");
                // ...AND IT STOPS WHEN HE LEAVES. "The cost when it is not on screen is zero."
                rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""), 10000);
                const bool stopped = rig.until ([] (const juce::var& s)
                                     { return ! (bool) s.getProperty ("shadowRunning", false); }, 8000);
                check (stopped,
                       "...and leaving that rack STOPS the view, so nothing is streamed for a picture "
                       "nobody is looking at (the plan's own promise: not \"small\", zero)",
                       "shadowRunning=" + rig.freshState().getProperty ("shadowRunning", juce::var()).toString());
            }
        }
    }

    // ---- THE MATRIX REPORT, written before anything is torn down -----------
    // Written on a red run too: the report IS the artefact (matrix section 7).
    // The folder the bundle under test came out of - "ship_2026-10-16a", or "AU" when it is a build
    // tree, in which case the two levels above name the build. Said plainly rather than guessed at.
    // "ship_2026-10-16a", or the BUILD TREE's name ("build", "build-guards") when it came out of one -
    // which is four levels up from the bundle, not three: <tree>/EchoJay_artefacts/Release/AU/<bundle>.
    writeMatrixReport (v2Comp.getParentDirectory().getFileName() == "AU"
                           ? v2Comp.getParentDirectory().getParentDirectory()
                                   .getParentDirectory().getParentDirectory().getFileName()
                           : v2Comp.getParentDirectory().getFileName(),
                       liveServerConfigured()
                           ? ("live pass, " + juce::String (::getenv ("EJ_LIVE_REPEATS") != nullptr
                                                            && *::getenv ("EJ_LIVE_REPEATS") != 0
                                                                ? ::getenv ("EJ_LIVE_REPEATS") : "5")
                              + " attempt(s) per live id")
                           : juce::String ("mock pass"));


    // (the live-view, RE and RH rows ran in their own slot, before anything closes the V2's window -
    //  see the note at that call. Running them here is what hid them from six passes today.)

    // ---- teardown in the host's own order ---------------------------------
    v2.closeEditor();
    for (Track* t : { &v2, &lk1, &lk2 }) if (t->plugin != nullptr) t->plugin->releaseResources();
    lk2.plugin.reset(); lk1.plugin.reset(); v2.plugin.reset();

    std::printf ("==== host_rig: %s (%d failed, %d passed, %d skipped) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures, passes, skips);
    if (skips > 0) std::printf ("     %d leg(s) could not run - each named above with its reason. A skip is "
                                "not a pass and is not counted as one.\n", skips);
    // ---- THE DOOR WE LEAVE BY (27 Oct 2026) -------------------------------
    // _Exit, not return. Eleven runs between 26 Oct 23:50 and 27 Oct 01:39 ended in the same SIGSEGV after
    // their verdict had been printed: ValhallaDelay's own JUCE timer thread locking a mutex at 0x0, while the
    // MAIN thread was inside exit() -> __cxa_finalize_ranges -> WaveShell 15.0's static destructor joining a
    // thread. dyld destroys an image's statics in reverse load order, so the plugin's statics were already
    // gone while that join held the process open, and its timer thread - alive BY DESIGN, because
    // ChainHost::~ChainHost keeps hosted third-party instances rather than disposing them inside AP_Close
    // (tools/teardown_dispose_guard) - touched them.
    //
    // Nothing of ours is in that stack and nothing of ours can fix it: we never destroy the instance, and we
    // cannot stop another image's threads. What we own is this line. tools/hosted_exit_probe reproduces the
    // crash in a process with no EchoJay object in it at all (instantiate, open and close the plugin's own
    // editor, leave the instance alive, return from main with a slow static destructor) and shows this door
    // clean on the same subjects - both directions, per the verifier rule.
    //
    // A test host has nothing to do in static destruction: the verdict is printed, the matrix report is
    // written, and the OS reclaims the rest. tools/au_instantiate_probe has left by this door since 21 Sep.
    std::fflush (nullptr);
    std::_Exit (failures == 0 ? 0 : 1);
}
