# [ECHOJAY FEATURES v1] — where each sentence comes from

The block the model is given about EchoJay itself (`EchoJayAPI.cpp:3963` onward) is allowed to say
only what the product actually does. Its own first line is the reason: *"THIS LIST IS THE WHOLE OF
WHAT YOU KNOW ABOUT ECHOJAY ... A confident wrong answer about this app is worse than no answer."*

So every section of it carries a citation here, and `mapfps_test`'s **ef PIN5** asserts that — adding
a feature sentence without citing it fails the gate rather than a review.

**This file is TRACKED, and that is the point (29 Oct 2026).** The pin used to read
`HANDOVER/commit-features-block.txt`, which is an untracked local scratch file: it could not pass on
a clean checkout, so it was red on this machine and red in Kathy's tree, for weeks, saying nothing
about the product either way. A rule that only holds on one person's disk is not a rule.

| section | claim | cited in |
|---|---|---|
| **TABS** | the eight tabs, in that order, and chat on all but two | the enum itself, `PluginEditor.h:369`, and `PluginEditor.cpp:13064` (`chatReplyControlsAllowed` — Dashboard and Settings are the two) |
| **CAPTURE** | a button in the header on every tab, a window averaged, against continuous live meters | `PluginEditor.h:774` (`captureBtn`, in the header) and `MeterEngine.h:508` (the 12-second runtime window the spectrum covers) |
| **NOTHING REACHES** | a chain arrives as a card with a Build button; a suggestion with no dialable values writes nothing | `PluginEditor.cpp:23817` ("Build this chain" on the card) and `EJDialWrites.h:54` (`dialWritesBlocked` — suggest everything, write none) |
| **CHAINS SAVE** | Save Chain and Save As in the Chain tab, loadable again by name | `PluginEditor.cpp:2289` (Save / Save As / Open) |
| **EACH SLOT** | bypass, remove, move, its own wet/dry, and a master wet for the rack | `ChainHost.h:1539` (`getMasterWet`, the rack's own) |
| **SETTINGS** | three toggles, off by default, on three different axes | `EchoJayAPI.h:902` — which plugins are offered (`autoDialMode`), whether they must be dialable, and whether values are written at all (`dialWritesBlocked`) |

A line here is evidence only while its citation resolves. If one of these moves, move the citation
with it in the same commit — that is cheaper than the hour it costs to find out later which of the
two is wrong.
