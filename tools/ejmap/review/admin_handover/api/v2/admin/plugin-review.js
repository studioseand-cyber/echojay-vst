/**
 * GET   /api/v2/admin/plugin-review
 *         -> { schema: "ejmap_role_review/1", generated_at, first_category, items: [...], answers: { <id>: answer } }
 * GET   /api/v2/admin/plugin-review?export=1
 *         -> { schema: "ejmap_human_checks/1", items_generated_at, answers: [answer...] }   (what EJ Map's --trust-levels --human reads)
 * POST  /api/v2/admin/plugin-review  { id, verdict, role?, section? }
 *         verdict: "yes" (the suggestion) | "role" (picked role + section) | "no" | "other" (not a main control) | "unsure" (counts neither
 *         way: the needs-measuring / ask-Sean list) | "clear"
 *         -> { ok: true, answer: answer | null, counts: { items, answered, countsNowMaps, mapsMappedByAnswers } }
 *
 * THE PLUGIN ROLE REVIEW (Kathy, 16 Oct 2026): the human check for controls EJ Map could not identify by name + unit. The items come
 * from EJ Map (`ejmap --trust-levels ... --out <dir>` writes review_items.json) and are put in the store by
 * scripts/import-plugin-review.mjs, with any answers Kathy already gave on the local page. This route only lists them and records
 * answers; it never edits a map. EchoJay does not read these keys yet (docs/TRUST_LEVELS_v0.md section 6.5).
 *
 * THE GATE, as misdials.js: lib/dash/admin.js alone - 401 without a session, 403 without an admin source.
 * THE REFUSAL ORDER: 405, 401, 503 (getSession threw), 403, 503 (no store), 400 (bad body / verdict), 404 (an id not in the items).
 * Every write logs [admin-action] BEFORE it lands.
 *
 * THE STORE (Upstash Redis, env-scoped like every other key - lib/env-scope scopedKey):
 *     ejmap:review:items     a string: the whole ejmap_role_review/1 document (one import replaces it)
 *     ejmap:review:answers   a hash: <id> -> JSON answer { id, fp, plugin, version, control, role, verdict, at, by }
 */
const { getSession } = require('../../../lib/dash/adapters');
const { adminSource, logAdminAction } = require('../../../lib/dash/admin');
const { scopedKey } = require('../../../lib/env-scope');

let redis = null;
try { redis = require('@upstash/redis').Redis.fromEnv(); } catch (e) { redis = null; }

const K_ITEMS = () => scopedKey('ejmap:review:items');
const K_ANSWERS = () => scopedKey('ejmap:review:answers');
const VERDICTS = new Set(['yes', 'no', 'role', 'other', 'unsure', 'clear']);
const ROLES = new Set(['threshold', 'ratio', 'attack', 'release', 'makeup', 'output', 'gain', 'freq', 'q', 'mix', 'delay_time', 'decay', 'predelay', 'drive', 'ceiling']);

function parse(x) { if (x && typeof x === 'object') return x; try { return JSON.parse(x); } catch (e) { return null; } }

function counts(items, answers) {
  const nowMaps = new Set(), mapped = new Set(); let answered = 0;
  for (const it of items) {
    const a = answers[it.id];
    if (a && a.verdict && a.verdict !== 'unsure') answered++;
    const isSuggestion = a && (a.verdict === 'yes' || (a.verdict === 'role' && a.role === it.suggested_role && (a.section || '') === (it.section || '')));
    if (it.yes_makes_mapped) { nowMaps.add(it.fp); if (isSuggestion) mapped.add(it.fp); }
  }
  return { items: items.length, answered, countsNowMaps: nowMaps.size, mapsMappedByAnswers: mapped.size };
}

async function readAll() {
  const doc = parse(await redis.get(K_ITEMS())) || { schema: 'ejmap_role_review/1', items: [] };
  const raw = (await redis.hgetall(K_ANSWERS())) || {};
  const answers = {};
  for (const [id, v] of Object.entries(raw)) { const a = parse(v); if (a) answers[id] = a; }
  return { doc, answers };
}

module.exports = async function handler(req, res) {
  if (req.method !== 'GET' && req.method !== 'POST') return res.status(405).json({ error: 'method_not_allowed' });
  let session;
  try { session = await getSession(req); } catch (e) { return res.status(503).json({ error: 'session_unavailable' }); }
  if (!session) return res.status(401).json({ error: 'not_signed_in' });
  const source = await adminSource(session);
  if (!source) return res.status(403).json({ error: 'not_admin' });
  if (!redis) return res.status(503).json({ error: 'store_unavailable' });

  try {
    if (req.method === 'GET') {
      const { doc, answers } = await readAll();
      if (req.query && req.query.export === '1')
        return res.status(200).json({ schema: 'ejmap_human_checks/1', items_generated_at: doc.generated_at || null, answers: Object.values(answers) });
      return res.status(200).json({ schema: doc.schema, generated_at: doc.generated_at || null, first_category: doc.first_category || '', items: doc.items || [], answers });
    }
    const body = typeof req.body === 'string' ? parse(req.body) : req.body;
    if (!body || typeof body.id !== 'string' || !VERDICTS.has(body.verdict)) return res.status(400).json({ error: 'bad_body' });
    if (body.verdict === 'role' && (!ROLES.has(body.role) || typeof (body.section || '') !== 'string')) return res.status(400).json({ error: 'bad_role' });
    const { doc, answers } = await readAll();
    const item = (doc.items || []).find((i) => i.id === body.id);
    if (!item) return res.status(404).json({ error: 'no_such_item' });
    logAdminAction('plugin_review_answer', session, source, { id: body.id, verdict: body.verdict });
    let answer = null;
    if (body.verdict === 'clear') { await redis.hdel(K_ANSWERS(), body.id); delete answers[body.id]; }
    else {
      let role = item.suggested_role, section = item.section || '';
      if (body.verdict === 'role') { role = body.role; section = body.section || ''; }
      if (body.verdict === 'other') { role = 'other'; section = ''; }
      answer = { id: item.id, fp: item.fp, plugin: item.plugin, version: item.version || null, control: item.control,
                 suggested_role: item.suggested_role, suggested_section: item.section || '', role, section, verdict: body.verdict, at: new Date().toISOString(), by: String(session.userId || 'admin') + ' (' + source + ')' };
      await redis.hset(K_ANSWERS(), { [item.id]: JSON.stringify(answer) });
      answers[item.id] = answer;
    }
    return res.status(200).json({ ok: true, answer, counts: counts(doc.items || [], answers) });
  } catch (e) {
    return res.status(503).json({ error: 'store_failed' });
  }
};
module.exports.__test = { counts, parse };
