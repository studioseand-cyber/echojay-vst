#!/usr/bin/env python3
"""What an answer RESTS ON, as distinct from how much it is trusted.

`trust` says how much weight a dial-time decision may put on a semantic.
`semantic_source` says WHERE THE ANSWER CAME FROM. They are different questions
and collapsing them loses the ability to ask the second one later.

WHY THIS EXISTS, and it is not hypothetical. The semantic hold-out's headline --
two-model agreement matched the hand answer 98.7% of the time -- treats the hand
answers as an independent standard. On 7 Aug 2026 the mapper said that some of
the 266 review decisions came from pasting the card into a THIRD MODEL and using
its answer. Those answers are as accurate as they ever were; what changed is
what a future measurement of them would be measuring. "How good were the human
answers?" over that population is partly a model being compared with itself.

The fix is not to distrust them. It is to record the source at the moment the
answer is given, so the question stays askable.
"""

#: Ordered loosely by how much INDEPENDENT EVIDENCE the answer rests on.
SOURCES = {
    "model-proposed": (
        "two arms agreed and the gates passed; no human looked. The pipeline's "
        "own output."),

    "read-and-accepted": (
        "a human read the card -- names, ranges, units, both arms' answers -- and "
        "accepted it. This is a THIRD OPINION over the same evidence the models "
        "had, not new information, which is why it does not promote trust."),

    "typed-from-knowledge": (
        "a human supplied a semantic the card did not offer, from knowing the "
        "product. New information, and the only human source that is genuinely "
        "independent of the models."),

    "touched-the-control": (
        "a human moved the control and read the display. MEASUREMENT, and the "
        "only source that promotes trust to human-verified -- touch is the one "
        "thing that verifies."),

    "model-consulted": (
        "the answer came from asking another model, with a human's assent. "
        "Accurate or not, it is NOT independent of the model arms: it shares "
        "their training and their failure modes. Measured 7 Aug 2026: a third "
        "arm over the six hold-out disagreements formed a majority on 5-6 of 6 "
        "and was WRONG on 3-4 of them. Recording this apart from "
        "typed-from-knowledge is the whole point of the field."),

    "retrospective": (
        "assigned by a later process -- a re-derivation, a corpus sweep, a "
        "correction from a wrongness report. Not a contemporaneous judgement."),
}

#: The ONLY source that may carry trust "human-verified".
VERIFYING = {"touched-the-control"}


def trust_for(source: str, proposed_trust: str) -> str:
    """READING IS NOT VERIFYING, enforced rather than remembered.

    A card in an app makes answering feel more authoritative than it is, so the
    promotion is refused here instead of relying on whoever writes the next UI.
    """
    if proposed_trust == "human-verified" and source not in VERIFYING:
        return "llm-classified"
    return proposed_trust


def is_independent_of_models(source: str) -> bool:
    """Whether an answer may be used as a STANDARD for scoring the models.

    model-proposed and model-consulted are not: scoring the arms against either
    is scoring them against themselves. read-and-accepted is a judgement call --
    a human agreeing with a model over the model's own evidence is weak
    independence -- so it is excluded, deliberately, to keep the claim strong.
    """
    return source in {"typed-from-knowledge", "touched-the-control"}


if __name__ == "__main__":
    print(f"{'source':22s} independent?  verifies?")
    for s in SOURCES:
        print(f"  {s:20s} {'yes' if is_independent_of_models(s) else 'no ':>11s}"
              f"  {'yes' if s in VERIFYING else 'no'}")
    print()
    for s, why in SOURCES.items():
        print(f"{s}\n    {why}\n")
