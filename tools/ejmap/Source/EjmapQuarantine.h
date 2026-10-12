/*
  EjmapQuarantine.h - THE QUARANTINE FLAG (Kathy, 12 Oct). Sean's 11-12 Oct night did not start: the build came by email, the unzip
  left com.apple.quarantine on every file, and macOS killed the app at launch (the signature was fine; the build is not notarised).
  The pre-flight looks at the app's executable, its bundle and the probe beside it; a flag on any of them is said with the fix and the
  run refuses (a quarantined probe is killed at every load, so every row would fail). PURE judge pinned (PF-QUAR); the reader is getxattr.
  Seen 12 Oct on this Mac: once macOS has prompted for a copy (Gatekeeper's first-launch check), that copy gains com.apple.macl and
  `xattr -dr` from Terminal fails with "Operation not permitted"; a fresh unzip cleared before any launch clears (12 flags -> 0).
*/
#pragma once

#include <juce_core/juce_core.h>
#include <sys/xattr.h>
#include <vector>

namespace ejmap::quarantine
{
inline bool flagged (const juce::File& f)   // the extended attribute is present (its value - the source app and time - is not needed)
{
    return getxattr (f.getFullPathName().toRawUTF8(), "com.apple.quarantine", nullptr, 0, 0, XATTR_NOFOLLOW) >= 0;
}
struct Checked { juce::String what; juce::File file; bool flagged = false; };
struct Verdict { bool ok = true; juce::String line, fix; };
// the folder to clear: the folder the app sits in (the unzipped ejmap-dist folder), else the app itself
inline juce::File folderToClear (const juce::File& bundle) { return bundle.getParentDirectory(); }
inline Verdict judge (const std::vector<Checked>& checked, const juce::File& clearFolder)
{
    Verdict v; juce::StringArray hit, seen;
    for (const auto& c : checked) { seen.add (c.what); if (c.flagged) hit.add (c.what); }
    if (hit.isEmpty()) { v.line = "ok - no quarantine flag (" + seen.joinIntoString (", ") + ")"; return v; }
    v.ok = false;
    v.line = "FLAGGED - " + hit.joinIntoString (", ") + " still carr" + (hit.size() == 1 ? "ies" : "y") + " the download's quarantine flag (a build sent by email); macOS will stop it";
    v.fix = "xattr -dr com.apple.quarantine \"" + clearFolder.getFullPathName() + "\"   then check: xattr -lr \"" + clearFolder.getFullPathName()
          + "\" | grep quarantine   (prints nothing when it is clear). Builds sent by email need this once after unzipping."
            " If xattr says 'Operation not permitted' (macOS already tried to open this copy): delete the folder, unzip the email again, and run"
            " the command BEFORE opening anything.";
    return v;
}
} // namespace ejmap::quarantine
