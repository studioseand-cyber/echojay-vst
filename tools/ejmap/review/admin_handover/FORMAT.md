# Formats (16 Oct 2026)

## ejmap_role_review/1 - the list (EJ Map writes it: `ejmap --trust-levels ... --out <dir>` -> `<dir>/review_items.json`)
```json
{
  "schema": "ejmap_role_review/1",
  "generated_at": "2026-10-16T17:36:44.786+01:00",
  "source": "ejmap --trust-levels",
  "first_category": "eq",               // optional (17 Oct): this category's cards come first among the "counts now" ones (--review-first <category>)
  "items": [
    {
      "id": "<map fp>|<control name>",
      "fp": "2666dfd7...",
      "plugin": "EchoBoy", "vendor": "Soundtoys", "version": "5.4.3.17500", "category": "delay",
      "control": "Mix",
      "unit": "-",                       // the unit the display shows ("db", "hz", "ms", "s", "pct", "ratio", "q", "note"), "-" = none
      "section": "",                    // the section the name puts the role in: "Gate", "Comp", "EQ", "Band 2", "LF".."HF", "De-ess", "Limiter"; "" = the plugin's own
      "suggested_role": "mix",           // the question is "<section> -> <role>" (e.g. "Gate -> Threshold"), or the role alone           // threshold | ratio | attack | release | makeup | output | gain | freq | q | mix | delay_time | decay | predelay | drive | ceiling
      "why": "the name says mix; the display shows no unit",   // or "one of several <role> candidates"
      "read_back": true,                 // the control's writes already read back (a yes alone can then count)
      "yes_makes_mapped": true,          // one yes on this row moves the map to "mapped"
      "counts_with": "<id>"              // optional (17 Oct): the partner row; yes on BOTH moves the map to "mapped" (an EQ band's freq + gain)
    }
  ]
}
```

## ejmap_human_checks/1 - the answers (the local page writes it; the console exports it; EJ Map reads it with `--human`)
```json
{
  "schema": "ejmap_human_checks/1",
  "items_generated_at": "2026-10-16T17:36:44.786+01:00",
  "answers": [
    { "id": "<map fp>|<control name>", "fp": "...", "plugin": "EchoBoy", "version": "5.4.3.17500", "control": "Mix",
      "suggested_role": "mix", "suggested_section": "", "role": "mix", "section": "", "verdict": "yes", "at": "2026-10-16T17:39:57+0100", "by": "kathy (local review)" }
  ]
}
```
- **`verdict`:**
  - "yes": the suggested role, in the suggested section;
  - "role": the role and section the person picked in the dropdown (`role`, `section`);
  - "no": not the suggestion, and no role given - refutes it (the name rule never claims it back);
  - "other": not a main control - refutes it;
  - "unsure": counts neither way; the row goes to the "Needs measuring / ask Sean" list.
  - A cleared answer is simply absent.
- **`suggested_role` / `suggested_section`:** what the row asked, kept beside the answer.
- **`section`:** a section label ("Gate", "Comp", "EQ", "Band 2", "LF".."HF", "De-ess", "Limiter", or "" for the plugin's own). EJ Map reads it as the control's section, over what the name says.
- **One answer per id; the latest wins.**
