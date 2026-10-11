#!/bin/bash
# downgrade_keys_guard - a source-and-git check, so there is nothing to build. Run from the repo root
# (the ctest entry sets WORKING_DIRECTORY).
set -u
exec /usr/bin/python3 -I tools/downgrade_keys_guard/keys.py
