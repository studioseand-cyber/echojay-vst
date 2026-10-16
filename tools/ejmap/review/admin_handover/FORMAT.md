# Formats (16 Oct 2026)

## ejmap_role_review/1 - the list (EJ Map writes it: `ejmap --trust-levels ... --out <dir>` -> `<dir>/review_items.json`)
```json
{
  "schema": "ejmap_role_review/1",
  "generated_at": "2026-10-16T17:36:44.786+01:00",
  "source": "ejmap --trust-levels",
  "items": [
    {
      "id": "<map fp>|<control name>",
      "fp": "2666dfd7...",
      "plugin": "EchoBoy", "vendor": "Soundtoys", "version": "5.4.3.17500", "category": "delay",
      "control": "Mix",
      "unit": "-",                       // the unit the display shows ("db", "hz", "ms", "s", "pct", "ratio", "q", "note"), "-" = none
      "suggested_role": "mix",           // threshold | ratio | attack | release | makeup | output | gain | freq | q | mix | delay_time | decay | predelay | drive | ceiling
      "why": "the name says mix; the display shows no unit",   // or "one of several <role> candidates"
      "read_back": true,                 // the control's writes already read back (a yes alone can then count)
      "yes_makes_mapped": true           // one yes on this row moves the map to "mapped"
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
      "role": "mix", "verdict": "yes", "at": "2026-10-16T17:39:57+0100", "by": "kathy (local review)" }
  ]
}
```
- **`verdict`:** "yes" identifies the control for that role on that map; "no" refutes it (the name rule never claims it back). A cleared answer is simply absent.
- **One answer per id; the latest wins.**
