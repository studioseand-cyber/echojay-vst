/*
  EjmapScanProgress.h

  Per-bundle scan progress, so a scan that dies resumes instead of repeating.

  THE PROBLEM. A VST3 probe opens the plugin's module for the 646 of 861 bundles
  with no moduleinfo.json, so a hang or a crash there takes the process down. The
  watchdog then stops the app BY DESIGN, and the whole 4.5 minute scan started
  again from bundle zero. Three hanging bundles were found on the development
  machine, which cost three full rescans and ~2600 ledger rows to discover.

  THE SHAPE. One line of JSON per probed bundle, appended and flushed as the scan
  walks, exactly like the ledger. Append-only, so a mid-write death loses at most
  the final line. On the next scan, a bundle with a usable entry is restored
  instead of re-probed.

  STALENESS. A bundle-list comparison catches additions and removals but not a
  bundle updated IN PLACE, which would silently restore a description of the old
  build. Every entry carries the disk stamp of the executable, and any bundle
  whose stamp has moved is re-probed.

  UNREADABLE OR OLD FORMAT: REFUSE AND RESCAN, never interpret. Same rule as the
  scan cache's version check. A progress file we cannot fully understand could
  restore descriptions under semantics that have since changed, and the cost of
  refusing is one scan, which is what would have happened anyway.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>

#include "EjmapDiskStamp.h"

namespace ejmap
{

class ScanProgress
{
public:
    /** Bump when the meaning of an entry changes: what a plugin id is, what is
        stamped, or what the descriptions are keyed by.
    */
    static constexpr int kFormatVersion = 1;

    struct Entry
    {
        DiskStamp stamp;
        juce::String outcome;                             // "ok" | "no_types"
        juce::Array<juce::PluginDescription> descriptions;
    };

    explicit ScanProgress (juce::File f) : file (std::move (f)) {}

    //==========================================================================
    /** Reads whatever is usable. Returns false and keeps nothing if the header
        is missing, unreadable, or from another format version.
    */
    bool load()
    {
        entries.clear();

        if (! file.existsAsFile())
            return false;

        juce::StringArray lines;
        lines.addLines (file.loadFileAsString());
        if (lines.isEmpty())
            return false;

        auto header = juce::JSON::parse (lines[0]);
        if (! header.isObject()
             || header.getProperty ("format", "").toString() != "ejmap-scan-progress"
             || (int) header.getProperty ("version", 0) != kFormatVersion)
        {
            // Refuse. Do not try to salvage lines from a format we do not know.
            entries.clear();
            return false;
        }

        for (int i = 1; i < lines.size(); ++i)
        {
            if (lines[i].trim().isEmpty())
                continue;

            auto v = juce::JSON::parse (lines[i]);
            if (! v.isObject())
                continue;   // truncated final line after a mid-write death

            const auto path = v.getProperty ("path", "").toString();
            if (path.isEmpty())
                continue;

            Entry e;
            e.stamp.modifiedMs = (juce::int64) (double) v.getProperty ("mtime", 0);
            e.stamp.size       = (juce::int64) (double) v.getProperty ("size", 0);
            e.stamp.valid      = (bool) v.getProperty ("stamped", false);
            e.outcome          = v.getProperty ("outcome", "").toString();

            if (auto* arr = v.getProperty ("plugins", juce::var()).getArray())
            {
                for (const auto& x : *arr)
                {
                    if (auto xml = juce::XmlDocument::parse (x.toString()))
                    {
                        juce::PluginDescription d;
                        if (d.loadFromXml (*xml))
                            e.descriptions.add (d);
                    }
                }
            }

            entries[path] = std::move (e);
        }

        return ! entries.empty();
    }

    /** The entry for this bundle, but ONLY if its stamp still matches what is on
        disk now. Returns nullptr when there is no entry, when the bundle has
        changed, or when either stamp could not be taken: "cannot tell" must
        re-probe, never restore.
    */
    const Entry* usableEntryFor (const juce::String& path) const
    {
        auto it = entries.find (path);
        if (it == entries.end())
            return nullptr;

        const auto now = diskStampFor (juce::File (path));
        if (! (now == it->second.stamp))
            return nullptr;

        return &it->second;
    }

    /** True when we have an entry at all, whether or not its stamp still
        matches. Lets a caller distinguish "never probed" from "probed, but the
        bundle has changed since".
    */
    bool hasEntryFor (const juce::String& path) const
    {
        return entries.find (path) != entries.end();
    }

    int size() const noexcept { return (int) entries.size(); }

    //==========================================================================
    /** Starts a fresh file. Called when a scan begins without a usable resume. */
    void begin (const juce::String& runId)
    {
        entries.clear();

        auto* o = new juce::DynamicObject();
        o->setProperty ("format", "ejmap-scan-progress");
        o->setProperty ("version", kFormatVersion);
        o->setProperty ("run_id", runId);
        o->setProperty ("at", juce::Time::getCurrentTime().toISO8601 (true));

        file.deleteFile();
        write (juce::JSON::toString (juce::var (o), true));
    }

    /** Appends one probed bundle. Flushed: the whole point is that it is on disk
        when the process stops existing.
    */
    void record (const juce::String& path,
                 const juce::String& outcome,
                 const juce::OwnedArray<juce::PluginDescription>& found)
    {
        const auto stamp = diskStampFor (juce::File (path));

        auto* o = new juce::DynamicObject();
        o->setProperty ("path", path);
        o->setProperty ("mtime", (double) stamp.modifiedMs);
        o->setProperty ("size", (double) stamp.size);
        o->setProperty ("stamped", stamp.valid);
        o->setProperty ("outcome", outcome);

        juce::Array<juce::var> xs;
        for (auto* d : found)
            if (auto xml = d->createXml())
                xs.add (xml->toString (juce::XmlElement::TextFormat().singleLine()));
        o->setProperty ("plugins", juce::var (xs));

        write (juce::JSON::toString (juce::var (o), true));
    }

    /** The scan completed, so the progress file has done its job. */
    void finish() { file.deleteFile(); }

private:
    void write (const juce::String& line) const
    {
        juce::FileOutputStream out (file);
        if (out.openedOk())
        {
            out.setPosition (file.getSize());
            out.writeText (line + "\n", false, false, nullptr);
            out.flush();
        }
    }

    juce::File file;
    std::map<juce::String, Entry> entries;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScanProgress)
};

} // namespace ejmap
