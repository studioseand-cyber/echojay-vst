#!/usr/bin/env python3
"""Gate for semantic_source. Calls no API."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from semantic_source import SOURCES, VERIFYING, trust_for, is_independent_of_models

fails = checks = 0
def check(c, w):
    global fails, checks
    checks += 1
    if not c:
        fails += 1; print(f"FAIL: {w}")

# The four the mapper named, plus the two the pipeline needs.
for s in ("read-and-accepted", "typed-from-knowledge", "touched-the-control",
          "model-consulted", "model-proposed", "retrospective"):
    check(s in SOURCES, f"source '{s}' exists")

# TOUCH IS THE ONLY THING THAT VERIFIES, enforced not remembered.
check(trust_for("touched-the-control", "human-verified") == "human-verified",
      "touch may carry human-verified")
check(trust_for("read-and-accepted", "human-verified") == "llm-classified",
      "READING IS NOT VERIFYING -- a read-and-accepted answer is demoted even if "
      "the caller asks for human-verified")
check(trust_for("typed-from-knowledge", "human-verified") == "llm-classified",
      "nor does typing from knowledge verify: it is a better opinion, not a measurement")
check(trust_for("model-consulted", "human-verified") == "llm-classified",
      "and a model's answer with a human's assent certainly does not")
check(trust_for("read-and-accepted", "llm-classified") == "llm-classified",
      "a non-verifying trust passes through untouched")

# INDEPENDENCE is a different axis from trust, and that is the point.
check(not is_independent_of_models("model-consulted"),
      "model-consulted is NOT independent -- scoring the arms against it is scoring "
      "them against themselves, which is what the 98.7% turned out to partly be")
check(not is_independent_of_models("model-proposed"), "nor is the pipeline's own output")
check(not is_independent_of_models("read-and-accepted"),
      "nor is agreeing with a model over the model's own evidence -- excluded "
      "deliberately, to keep the claim strong rather than convenient")
check(is_independent_of_models("typed-from-knowledge"),
      "typed-from-knowledge IS independent: new information the models did not have")
check(is_independent_of_models("touched-the-control"),
      "and so is a measurement")

# The two axes must not be collapsed: a source can verify without being the only
# independent one, and be independent without verifying.
check(is_independent_of_models("typed-from-knowledge")
      and "typed-from-knowledge" not in VERIFYING,
      "INDEPENDENT AND NON-VERIFYING is a real combination -- the axes are separate")

print(f"{checks} checks, {fails} failures")
sys.exit(1 if fails else 0)
