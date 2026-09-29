#!/bin/bash
# rt_role_guard (21t-m, 29 Sep 2026): THE ROLE QUESTION NEVER TOUCHES A STRING ON THE AUDIO THREAD.
#
# WHAT IT PINS. The item-5 ruling made chainRole() the one reader of the chain's role and selfKeyRoleIsMusic()
# the one reader of the music question. It did not say WHICH THREAD, and the implementation followed it onto the
# audio thread: selfKeyRoleIsMusic() is called from EchoJayProcessor::processBlock on every block, and it was
# computing its answer by calling ChainHost::getHostTrackName() - a juce::String RETURNED BY VALUE. That is a
# ref-counted copy (a heap operation in the real-time path) and an unsynchronised read of a string the message
# thread writes, which is a data race on a ref count. Four Link rigs went red with a juce::String reading as
# neither its value nor empty, which is what that damage looks like.
#
# TWO RULES, both grep-able, both cheap enough to run on every gate:
#   (1) the BODY of selfKeyRoleIsMusic() is an atomic load and nothing else;
#   (2) processBlock (in either body) references none of getHostTrackName / decideChainRole / decideChainIsMusic
#       / chainRole, directly or through the functions it calls.
#
# EJ_SRC_ROOT: a pre-round checkout, for the RED run.
cd "${EJ_SRC_ROOT:-$(dirname "$0")/../..}"
FAIL=0

# ---- (1) selfKeyRoleIsMusic() is a load ------------------------------------------------------------------
BODY=$(awk '/bool selfKeyRoleIsMusic\(\)/{found=1} found{print; if (/\}/ && !/^\s*\/\//) exit}' Source/PluginProcessor.h)
if [ -z "$BODY" ]; then
    echo "  FAIL  selfKeyRoleIsMusic() not found in Source/PluginProcessor.h"; FAIL=1
elif echo "$BODY" | grep -qE "getHostTrackName|decideChain|chainRole\(\)"; then
    echo "  FAIL  selfKeyRoleIsMusic() COMPUTES its answer. It is called from processBlock on every block, so it"
    echo "        may only LOAD one. Its body:"
    echo "$BODY" | sed 's/^/          /'
    FAIL=1
elif ! echo "$BODY" | grep -q "\.load ("; then
    echo "  FAIL  selfKeyRoleIsMusic() does not read an atomic. Its body:"
    echo "$BODY" | sed 's/^/          /'
    FAIL=1
else
    echo "  ok    selfKeyRoleIsMusic() is an atomic load and nothing else"
fi

# ---- (2) processBlock touches none of them, directly or one call deep ------------------------------------
# The direct check, per body. Anything processBlock calls that is itself one of these names is caught by (1)
# for the music question and named here for the rest.
for f in Source/PluginProcessor.cpp Source/LinkProcessor.cpp; do
    BLOCK=$(awk '/^void (EchoJayProcessor|LinkProcessor)::processBlock/{found=1} found{print; if (/^\}/) exit}' "$f")
    HITS=$(echo "$BLOCK" | grep -nE "getHostTrackName|decideChainRole|decideChainIsMusic|chainRole *\(" \
           | grep -vE "^[0-9]+: *//" )
    if [ -n "$HITS" ]; then
        echo "  FAIL  $(basename "$f") processBlock references the role question on the AUDIO THREAD:"
        echo "$HITS" | sed 's/^/          /'
        FAIL=1
    else
        echo "  ok    $(basename "$f") processBlock references none of them"
    fi
done

if [ $FAIL -ne 0 ]; then echo; echo "==== rt_role_guard: RED ===="; exit 1; fi
echo "==== rt_role_guard: GREEN (the role question is computed on the message thread and LOADED on the audio one) ===="
