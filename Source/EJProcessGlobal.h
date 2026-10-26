#pragma once

#include <cstddef>

/** ONE BLOCK OF MEMORY, FOUND BY NAME, SHARED BY EVERY ECHOJAY BUNDLE IN THIS PROCESS.
    Round 1 of docs/LIVE_VIEW_REAL_EDITOR_2026-10-26.md §2, 26 Oct 2026.

    WHY THIS EXISTS AT ALL, which was the first thing round 1 had to find out. The design says the
    V2 keeps "a process-local directory of live EchoJay instances" and reaches the Link's own plugin
    through it. The obvious implementation - a static table in a shared header - CANNOT WORK: the V2
    and the Link are two Mach-O images (two .component bundles), each with its own copy of every
    static in every header it includes. Measured: both bundles are built with hidden visibility (the
    V2 exports 26 symbols, the Link 3, all of them AU factories), so they cannot even see each
    other's symbols to rendezvous on. Two tables, each side registering into its own, and the V2
    would never find a single Link.

    So the rendezvous is made where it is image-independent: in the Objective-C runtime, which is one
    per process and outlives any bundle.

      * The KEY is `sel_registerName(name)` - libobjc uniques selectors process-wide, so the same
        name computes to the same address in every image, without either image exporting anything.
      * The HOLDER is an associated object on an immortal Foundation class, so the block survives the
        bundle that created it being unloaded. Proved both ways by probe before this was written: one
        table across two dylibs, and the same table still readable after dlclose of the creator.
      * The LAYOUT is checked, not trusted: the block carries a header with its own size, and a
        caller asking for a different size gets nullptr rather than a differently-shaped table. Two
        EchoJay builds that disagree (which happens on this Mac - a V2 installed over an older Link)
        therefore do not share at all, and the caller takes the fallback road and says so.

    @param versionedKey  a name ending in a version, e.g. "echojayLocalInstances_v1". BUMP IT on any
                         layout change; the size check is the backstop, not the contract.
    @param bytes         the size of the block. Same key, different size = refused.
    @param createdOut    optional: true if THIS call allocated it (the first bundle in the process).
    @returns             the zeroed block, or nullptr if a different build got there first.

    Thread safety: the lookup and the allocation are done under one process-wide lock, so two
    bundles calling at once get one block. What the block CONTAINS is the caller's business; the
    typed layer above this (EJLocalInstances.h) does its own locking. */
namespace echojay
{
    void* processGlobal (const char* versionedKey, size_t bytes, bool* createdOut = nullptr) noexcept;
}
