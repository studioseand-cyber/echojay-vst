#pragma once
#include <JuceHeader.h>

// =============================================================================================================
//  21r item 3 (24 Sep 2026): IS THE SCAN STALE?
//
//  Sean's scan on this Mac is from 6 September. A plugin installed since then is invisible to EchoJay, and the
//  only surface saying so was a line in a dropdown nobody opens. The rule: on launch, if any plugin folder's
//  CONTENTS changed since the last scan, ask once - "Plugins changed since the last scan - Scan now?" - and
//  never scan without being told to.
//
//  The decision is a pure function so a guard can pin it against real files rather than against a description.
//  What counts as a change is the modification time of a folder's IMMEDIATE children (each .component/.vst3
//  bundle), not a recursive walk: a bundle's own directory time moves when it is installed, replaced or removed,
//  and a recursive walk over ~1,400 plugins costs seconds for nothing.
// =============================================================================================================
namespace echojay
{
struct FolderFreshness
{
    bool         changed   = false;
    int          newer     = 0;          // how many entries are newer than the scan
    juce::String firstName;              // one name, so the prompt can be specific rather than vague
    juce::int64  newestMs  = 0;
};

inline FolderFreshness foldersChangedSince (const juce::Array<juce::File>& folders, juce::int64 scannedAtMs)
{
    FolderFreshness f;
    if (scannedAtMs <= 0) return f;      // never scanned: that is not "stale", it is "not yet done"
    for (const auto& dir : folders)
    {
        if (! dir.isDirectory()) continue;
        for (const auto& child : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFilesAndDirectories))
        {
            const auto ms = child.getFile().getLastModificationTime().toMilliseconds();
            if (ms <= scannedAtMs) continue;
            ++f.newer;
            if (ms > f.newestMs) { f.newestMs = ms; f.firstName = child.getFile().getFileNameWithoutExtension(); }
        }
    }
    f.changed = f.newer > 0;
    return f;
}

// The folders a scan covers: the two system plugin folders plus whatever the user added.
inline juce::Array<juce::File> standardPluginFolders (const juce::StringArray& customFolders)
{
    juce::Array<juce::File> out;
    out.add (juce::File ("/Library/Audio/Plug-Ins/Components"));
    out.add (juce::File ("/Library/Audio/Plug-Ins/VST3"));
    out.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins/Components"));
    out.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins/VST3"));
    for (const auto& c : customFolders) if (c.isNotEmpty()) out.add (juce::File (c));
    return out;
}

inline juce::String rescanPromptText (const FolderFreshness& f)
{
    if (! f.changed) return {};
    return f.newer == 1 ? "Plugins changed since the last scan (" + f.firstName + ") - Scan now?"
                        : "Plugins changed since the last scan (" + juce::String (f.newer) + " items, newest "
                          + f.firstName + ") - Scan now?";
}
} // namespace echojay
