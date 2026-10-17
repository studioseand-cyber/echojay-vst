// node tools/ejmap/review/test_review.cjs - the review module's pure parts and the admin route against a fake store (no network)
const assert = require('assert');
const path = require('path');
const Module = require('module');
const R = require('./admin-plugin-review.js');
const items = [
  { id: 'A|Mix', fp: 'A', plugin: 'Echo', category: 'delay', control: 'Mix', suggested_role: 'mix', yes_makes_mapped: true },
  { id: 'A|Time', fp: 'A', plugin: 'Echo', category: 'delay', control: 'Time', suggested_role: 'delay_time', yes_makes_mapped: true },
  { id: 'B|Thr', fp: 'B', plugin: 'Alpha', category: 'compressor', control: 'Thr', suggested_role: 'threshold', yes_makes_mapped: false },
];
const g = R.__test.group(items, true), all = R.__test.group(items, false);
const mixed = items.concat([{ id: 'C|Gain', fp: 'C', plugin: 'Zeta EQ', category: 'eq', control: 'Gain', suggested_role: 'gain', yes_makes_mapped: true }]);
assert.deepStrictEqual(R.__test.group(mixed, false, 'eq').map((c) => c.fp), ['C', 'A', 'B']);   // EQ counts-now first, then by name
assert.deepStrictEqual(R.__test.group(mixed, false).map((c) => c.fp), ['A', 'C', 'B']);         // no first_category: by name
assert.strictEqual(g.length, 1); assert.strictEqual(all.length, 2); assert.strictEqual(all[0].fp, 'A');   // counts-now first
const c = R.__test.counts(items, { 'A|Mix': { verdict: 'yes' }, 'B|Thr': { verdict: 'no' } });
assert.deepStrictEqual([c.answered, c.countsNowMaps, c.mapsMappedByAnswers, c.maps], [2, 1, 1, 2]);
assert.strictEqual(R.__test.roleWord('delay_time'), 'Delay time');
assert.strictEqual(R.__test.question({ section: 'Gate', suggested_role: 'threshold' }), 'Gate \u2192 Threshold');
assert.strictEqual(R.__test.question({ section: '', suggested_role: 'mix' }), 'Mix');
const opts = R.__test.options({ section: 'Gate', suggested_role: 'threshold', plugin_sections: ['Comp', 'Gate'] });
assert.strictEqual(opts[0].value, 'Gate|threshold'); assert.ok(opts.some((o) => o.value === 'Comp|makeup' && o.text === 'Comp \u2192 Make-up'));
assert.strictEqual(opts[opts.length - 1].value, '|other'); assert.strictEqual(new Set(opts.map((o) => o.value)).size, opts.length);
assert.strictEqual(R.__test.chosen({ section: 'Gate', suggested_role: 'threshold' }, { verdict: 'role', role: 'makeup', section: 'Comp' }), 'Comp|makeup');
const c2 = R.__test.counts(items, { 'A|Mix': { verdict: 'role', role: 'mix', section: '' }, 'A|Time': { verdict: 'unsure' } });
assert.deepStrictEqual([c2.answered, c2.unsure, c2.mapsMappedByAnswers], [1, 1, 1]);

// the route, with its requires answered by fakes
const store = { s: {}, h: {} };
const fakes = {
  '../../../lib/dash/adapters': { getSession: async (req) => req.session },
  '../../../lib/dash/admin': { adminSource: async (s) => (s && s.userId === 'admin1' ? 'env' : null), logAdminAction: () => {} },
  '../../../lib/env-scope': { scopedKey: (k) => 'dev:' + k },
  '@upstash/redis': { Redis: { fromEnv: () => ({
    get: async (k) => store.s[k] || null, set: async (k, v) => { store.s[k] = v; },
    hgetall: async (k) => store.h[k] || null, hset: async (k, o) => { store.h[k] = Object.assign(store.h[k] || {}, o); },
    hdel: async (k, f) => { if (store.h[k]) delete store.h[k][f]; } }) } },
};
const load = Module._load;
Module._load = function (req, parent, isMain) { return fakes[req] || load.apply(this, arguments); };
const route = require('./admin_handover/api/v2/admin/plugin-review.js');
Module._load = load;
store.s['dev:ejmap:review:items'] = JSON.stringify({ schema: 'ejmap_role_review/1', generated_at: 'T', items });
function call(method, body, session, query) {
  return new Promise((resolve) => { const res = { code: 0, status(c) { this.code = c; return this; }, json(b) { resolve({ code: this.code, body: b }); } };
    route({ method, body, session, query: query || {} }, res); });
}
(async () => {
  assert.strictEqual((await call('GET', null, null)).code, 401);
  assert.strictEqual((await call('GET', null, { userId: 'u2' })).code, 403);
  const admin = { userId: 'admin1' };
  assert.strictEqual((await call('POST', { id: 'nope', verdict: 'yes' }, admin)).code, 404);
  assert.strictEqual((await call('POST', { id: 'A|Mix', verdict: 'maybe' }, admin)).code, 400);
  const ok = await call('POST', { id: 'A|Mix', verdict: 'yes' }, admin);
  assert.strictEqual(ok.code, 200); assert.strictEqual(ok.body.counts.mapsMappedByAnswers, 1); assert.strictEqual(ok.body.answer.role, 'mix');
  assert.strictEqual((await call('POST', { id: 'B|Thr', verdict: 'role', role: 'nonsense' }, admin)).code, 400);
  const picked = await call('POST', { id: 'B|Thr', verdict: 'role', role: 'makeup', section: 'Comp' }, admin);
  assert.strictEqual(picked.body.answer.role, 'makeup'); assert.strictEqual(picked.body.answer.section, 'Comp'); assert.strictEqual(picked.body.answer.suggested_role, 'threshold');
  await call('POST', { id: 'B|Thr', verdict: 'clear' }, admin);
  const got = await call('GET', null, admin); assert.strictEqual(got.body.items.length, 3); assert.strictEqual(got.body.first_category, ''); assert.strictEqual(got.body.answers['A|Mix'].verdict, 'yes');
  const ex = await call('GET', null, admin, { export: '1' }); assert.strictEqual(ex.body.schema, 'ejmap_human_checks/1'); assert.strictEqual(ex.body.answers.length, 1);
  await call('POST', { id: 'A|Mix', verdict: 'clear' }, admin);
  assert.strictEqual(Object.keys((await call('GET', null, admin)).body.answers).length, 0);
  console.log('test_review: all checks passed');
})().catch((e) => { console.error('test_review: FAIL', e); process.exit(1); });
