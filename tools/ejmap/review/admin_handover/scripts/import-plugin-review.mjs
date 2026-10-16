#!/usr/bin/env node
/**
 * Import the plugin role review into the store (Kathy, 16 Oct 2026):
 *   node scripts/import-plugin-review.mjs --items <review_items.json> [--answers <human_checks.json>] [--apply]
 *
 * Without --apply it says what it would write and writes nothing. With --apply:
 *   - ejmap:review:items   <- the whole review_items.json (ejmap_role_review/1), replacing the previous list
 *   - ejmap:review:answers <- every answer from human_checks.json (ejmap_human_checks/1) whose id is in the items; an answer already in
 *                              the store with a later "at" is kept (the newer answer wins); an answer for an id not in the list is
 *                              reported and skipped
 * Keys are env-scoped exactly as the route reads them (lib/env-scope scopedKey), so run it with the environment the console uses.
 */
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const { scopedKey } = require('../lib/env-scope');

const args = process.argv.slice(2);
const arg = (k) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : null; };
const apply = args.includes('--apply');
const itemsPath = arg('--items'), answersPath = arg('--answers');
if (!itemsPath) { console.error('usage: node scripts/import-plugin-review.mjs --items <review_items.json> [--answers <human_checks.json>] [--apply]'); process.exit(2); }

const doc = JSON.parse(readFileSync(itemsPath, 'utf8'));
if (doc.schema !== 'ejmap_role_review/1' || !Array.isArray(doc.items)) { console.error('not an ejmap_role_review/1 document'); process.exit(2); }
const ids = new Set(doc.items.map((i) => i.id));
const given = answersPath ? JSON.parse(readFileSync(answersPath, 'utf8')) : { answers: [] };
if (answersPath && given.schema !== 'ejmap_human_checks/1') { console.error('not an ejmap_human_checks/1 document'); process.exit(2); }
const valid = (given.answers || []).filter((a) => ids.has(a.id) && ['yes', 'no', 'role', 'other', 'unsure'].includes(a.verdict));
const skipped = (given.answers || []).filter((a) => !ids.has(a.id));

console.log(`IMPORT: ${doc.items.length} item(s) (generated ${doc.generated_at}); ${valid.length} answer(s) to import, ${skipped.length} for ids not in this list (skipped)`);
for (const a of skipped.slice(0, 10)) console.log(`  skipped: ${a.plugin} / ${a.control}`);
if (!apply) { console.log('IMPORT: dry run - nothing written (add --apply)'); process.exit(0); }

const { Redis } = require('@upstash/redis');
const redis = Redis.fromEnv();
await redis.set(scopedKey('ejmap:review:items'), JSON.stringify(doc));
const have = (await redis.hgetall(scopedKey('ejmap:review:answers'))) || {};
let written = 0, kept = 0;
for (const a of valid) {
  const prev = have[a.id] ? (typeof have[a.id] === 'string' ? JSON.parse(have[a.id]) : have[a.id]) : null;
  if (prev && prev.at && a.at && Date.parse(prev.at) > Date.parse(a.at)) { kept++; continue; }
  await redis.hset(scopedKey('ejmap:review:answers'), { [a.id]: JSON.stringify(a) }); written++;
}
console.log(`IMPORT: items written; ${written} answer(s) written, ${kept} kept (the store's was newer)`);
