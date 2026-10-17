#!/usr/bin/env python3
"""python3 tools/ejmap/review/test_review_server.py - the local review server's counts (17 Oct: a pair needs both yeses). No network."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
import review_server as r
items = [{'id': 'A|Mix', 'fp': 'A', 'suggested_role': 'mix', 'yes_makes_mapped': True},
         {'id': 'E|F', 'fp': 'E', 'suggested_role': 'freq', 'counts_with': 'E|G'},
         {'id': 'E|G', 'fp': 'E', 'suggested_role': 'gain', 'counts_with': 'E|F'}]
one = r.counts(items, {'A|Mix': {'verdict': 'yes'}, 'E|F': {'verdict': 'yes'}})
both = r.counts(items, {'A|Mix': {'verdict': 'yes'}, 'E|F': {'verdict': 'yes'}, 'E|G': {'verdict': 'role', 'role': 'gain', 'section': ''}})
assert (one['countsNowMaps'], one['mapsMappedByAnswers'], both['mapsMappedByAnswers']) == (2, 1, 2), (one, both)
print('test_review_server: all checks passed')
