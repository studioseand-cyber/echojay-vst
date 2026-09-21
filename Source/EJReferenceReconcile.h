#pragma once

#include <JuceHeader.h>
#include <functional>
#include <vector>

#include "EJReferenceIndex.h"

// RECONCILING THE LIBRARY ON DISK WITH THE PATHS IN A PROJECT'S BLOB.
//
// The one decision this file makes: given what the index file said and what
// this project's state blob carries, WHAT IS THE LIBRARY FOR THIS SESSION, and
// which of its entries still need a decode.
//
// WHY IT IS NOT IN EJReferenceIndex.h. That header is the FORMAT: parse, write,
// merge, lock, epoch policy, and nineteen ri pins stand on it. This is the
// POLICY that calls it, it arrived two commits later, and keeping them apart
// means a change here cannot make a format pin red for a reason that has
// nothing to do with the format.
//
// Header-inline, the EJParamReads.h discipline: the gate links the PREVIOUS
// build's SharedCode lib, so anything a pin exercises must live in a header the
// test TU compiles directly, or the pin measures the last build and not this
// one. This one matters more than most, because the editor is the one thing the
// gate can never link (open list 158) and every decision below would otherwise
// be unpinnable.
//
// PURE, AND EVERY IMPURITY IS A PARAMETER. It reads no file, resolves no
// directory, asks nothing of the filesystem and holds no state. Whether a path
// exists arrives as a predicate, the clock arrives as a string, and the id
// source arrives as a function so a pin can mint predictably. That is what
// makes the six cases below testable at all: each one is a value in and a value
// out.
//
// WIRED TO NOTHING IN THIS COMMIT. Nothing in Source/ calls it yet. The same
// discipline the tombstones took: the format was parsed, written and merged for
// two commits before anything emitted one.
namespace echojay {

/** What a session gets back. The library is what the user sees; toAnalyse is
    the work the caller must queue, in the order it should be queued. */
struct RefReconciled
{
    ReferenceIndex            library;
    std::vector<juce::String> toAnalyse;

    /** False ONLY when the index file existed and could not be read. The
        session then runs on an empty library and NOTHING may be written back,
        because the file that could not be read is the user's library and the
        cause is usually recoverable. */
    bool mayWrite = true;
};

/** The reconciliation.

    @param loaded      what loadReferenceIndex returned, state included.
    @param blobPaths   this project's referencePaths, IN BLOB ORDER.
    @param pathExists  the caller's existsAsFile. A parameter because this
                       function must not touch a disk a pin does not own.
    @param nowIso      the timestamp to stamp. A parameter for the same reason.
    @param mintId      the id source. A parameter so a pin can assert an exact
                       library rather than "something beginning with r_".
    @param runningEpoch the epoch to judge entries against.

    THE ORDER OF THE RESULT IS THE INDEX FIRST, THEN THE BLOB'S UNKNOWN PATHS IN
    BLOB ORDER. It matters: with an absent index the result is the blob's own
    order, which is exactly the list today's startup builds, so a first run
    after this lands looks identical to a first run before it. */
inline RefReconciled refReconcile (const RefLoadResult& loaded,
                                   const std::vector<juce::String>& blobPaths,
                                   const std::function<bool (const juce::String&)>& pathExists,
                                   const juce::String& nowIso,
                                   const std::function<juce::String()>& mintId = newReferenceId,
                                   int runningEpoch = kRefMeasurementEpoch)
{
    RefReconciled out;

    // UNREADABLE OPENS EMPTY AND DOES NOT FALL BACK TO THE BLOB.
    //
    // The tempting alternative, rebuilding from the blob's paths so the user
    // sees something, is the dangerous one: this session would then hold a
    // library that is missing every reference the blob never knew about, and
    // the first commit would write THAT over the file that could not be read.
    // A damaged library would become a permanently shorter one. Empty says the
    // same thing honestly and destroys nothing, and refMayCommit already
    // refuses the write; this flag carries the same answer to a caller that
    // never gets as far as committing.
    if (loaded.state == RefLoadState::Unreadable)
    {
        out.mayWrite = false;
        return out;
    }

    // Absent leaves the default-constructed index, which is an empty library at
    // this build's schema and epoch. Absent is not damage: it is the first run.
    if (loaded.state == RefLoadState::Loaded)
        out.library = loaded.index;

    out.mayWrite = refIndexMayWrite (out.library);

    // THE ENTRIES THE LIBRARY ALREADY HAS. Availability is re-read every open,
    // because a volume mounts, a folder moves and neither event tells us.
    for (auto& e : out.library.entries)
    {
        const bool here = pathExists (e.path);

        // An entry that was Unreadable stays Unreadable when its file is still
        // there: this function cannot tell a permission problem from a healthy
        // file, and downgrading it to Present would offer a file we cannot
        // vouch for, which is the direction RefAvailability refuses to take.
        if (here)
        {
            if (e.availability != RefAvailability::Unreadable)
                e.availability = RefAvailability::Present;
            e.lastSeenAt = nowIso;
        }
        else
        {
            e.availability = RefAvailability::Missing;
            // lastSeenAt is NOT touched. It is the answer to "when did this
            // last work", and overwriting it with the moment we noticed it was
            // gone would destroy the only fact the user could act on.
        }

        e.checkedAt = nowIso;

        if (refNeedsReanalysis (e, runningEpoch))
            out.toAnalyse.push_back (e.path);
    }

    // THE BLOB'S PATHS. Anything the library already holds is left alone: the
    // index is the store of record and the blob is a weaker, older witness.
    for (const auto& p : blobPaths)
    {
        if (p.isEmpty()) continue;
        if (refFindByPath (out.library, p) >= 0) continue;

        RefEntry e;
        e.id       = mintId();
        e.name     = juce::File (p).getFileNameWithoutExtension();
        e.path     = p;
        e.addedAt  = nowIso;
        e.checkedAt = nowIso;
        e.measurementEpoch = runningEpoch;
        // measurements.valid stays false: refIsUnmeasured is the state, and it
        // is distinct from stale. Stale has numbers under an older definition;
        // this has none at all.

        if (pathExists (p))
        {
            e.availability = RefAvailability::Present;
            e.lastSeenAt   = nowIso;
            out.toAnalyse.push_back (p);
        }
        else
        {
            // THE SILENT DROP, GONE. PluginProcessor.cpp:4470 filters this path
            // out of the restore with no record anywhere, so the user's list is
            // simply shorter than they left it. Here it becomes an entry that
            // is LISTED, marked Missing and says so. A missing file is a state,
            // not a deletion.
            e.availability = RefAvailability::Missing;
        }

        out.library.entries.push_back (e);
    }

    // THE CAP IS NOT ENFORCED HERE, deliberately. REFERENCE_INDEX_SCHEMA.md
    // section 4 ships kRefIndexMaxEntries with the menu in commit 7, because a
    // library that accepts entries the menu cannot show is the worse defect.
    // Until then the library is unbounded, and a reconciliation that started
    // refusing would reintroduce exactly the silent shortening this function
    // exists to remove.

    return out;
}

} // namespace echojay
