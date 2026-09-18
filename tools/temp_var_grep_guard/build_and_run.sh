#!/bin/bash
# temp_var_grep_guard (18 Sep 2026 ruling, after the Pro Tools crash in EchoJayEditor::roleByNameFor): a raw pointer
# into a TEMPORARY parsed var - `JSON::parse(...).getProperty(...).getArray()` in one expression - is a use-after-free
# the moment the statement ends. RED on any such expression in Source/; GREEN when none. Also refuses the sibling
# shapes `JSON::parse(...).getArray()` and `JSON::parse(...).getDynamicObject()` kept past the expression.
cd "${EJ_SRC_ROOT:-$(dirname "$0")/../..}"   # EJ_SRC_ROOT: a pre-round checkout for the RED run
PAT='JSON::parse *\([^;]*\)\.getProperty *\([^;]*\)\.(getArray|getDynamicObject) *\(\)|JSON::parse *\([^;]*\)\.(getArray|getDynamicObject) *\(\)'
hits="$(grep -nE "$PAT" Source/*.cpp Source/*.h 2>/dev/null | grep -v '^\S*:[0-9]*:\s*//' )"
if [ -n "$hits" ]; then
  echo "$hits"
  echo; echo "==== temp_var_grep_guard: RED ($(echo "$hits" | wc -l | tr -d ' ') raw pointer(s) into a temporary parsed var - use ChainJsonView or hold the root in a named var) ===="
  exit 1
fi
echo "==== temp_var_grep_guard: GREEN (no pointer into a temporary parsed var in Source/) ===="
