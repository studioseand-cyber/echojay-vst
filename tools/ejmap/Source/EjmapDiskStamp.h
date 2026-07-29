/*
  EjmapDiskStamp.h

  "Has this plugin changed on disk since we last looked at it?"

  One mechanism, two jobs:

    - scan resume, so a restored progress entry for a bundle that has been
      updated in place is re-probed rather than trusted. A bundle-list match
      catches additions and removals; only a stamp catches an update.

    - scan-quarantine auto-release, because a plugin update is the usual reason
      a hang stops happening.

  WHY THE BINARY AND NOT THE BUNDLE. A .vst3 is a directory. On macOS
  File::getSize() on a directory is 0 and its modification time only tracks
  direct children, so a vendor replacing Contents/MacOS/Foo leaves the bundle's
  own stamp untouched. The executable inside is the thing that actually changes,
  and it is the same file the architecture census reads.
*/

#pragma once

#include <juce_core/juce_core.h>

namespace ejmap
{

struct DiskStamp
{
    juce::int64 modifiedMs = 0;
    juce::int64 size       = 0;
    bool        valid      = false;

    bool operator== (const DiskStamp& o) const noexcept
    {
        return valid && o.valid && modifiedMs == o.modifiedMs && size == o.size;
    }
    bool operator!= (const DiskStamp& o) const noexcept { return ! operator== (o); }
};

/** Stamps the executable inside a bundle, or the file itself if it is not a
    bundle. Returns valid=false when there is nothing to stamp, which callers
    must treat as "cannot tell" rather than as "unchanged".
*/
inline DiskStamp diskStampFor (const juce::File& f)
{
    DiskStamp s;

    juce::File target = f;

    if (f.isDirectory())
    {
        auto macos = f.getChildFile ("Contents").getChildFile ("MacOS");
        if (! macos.isDirectory())
            return s;

        auto kids = macos.findChildFiles (juce::File::findFiles, false);
        if (kids.isEmpty())
            return s;

        // Deterministic pick: a bundle with more than one executable would
        // otherwise stamp a different file depending on directory order.
        kids.sort();
        target = kids.getReference (0);
    }

    if (! target.existsAsFile())
        return s;

    s.modifiedMs = target.getLastModificationTime().toMilliseconds();
    s.size       = target.getSize();
    s.valid      = true;
    return s;
}

} // namespace ejmap
