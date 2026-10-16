/**
 * Plugin role review (Kathy, 16 Oct 2026): the human check for "roles not identified" (docs/TRUST_LEVELS_v0.md section 6).
 *
 * ONE MODULE, TWO HOSTS. The operator console tab (public/js/admin-plugin-review.js in echojay-saas, key 'plugin-review', mounted by
 * admin-shell.js like admin-community.js) and the local page today (tools/ejmap/review/index.html, served by review_server.py). Both
 * hand it a ctx with el() and api2(); the routes are the only difference:
 *     GET   <base>            -> { schema: "ejmap_role_review/1", generated_at, items: [...], answers: { <id>: answer } }
 *     POST  <base>  { id, verdict: "yes" | "no" | "clear" } -> { ok: true, answer | null, counts }
 * Local base: /api/review. Admin base: /admin/plugin-review (api2 prefixes /api/v2).
 *
 * WHAT A REVIEWER SEES: one card per plugin map; inside, the controls a person could settle - the name says a role the plugin's dial
 * needs, but the unit is missing / wrong, or there are several candidates. Each row: control, unit seen, suggested role, why, whether
 * the control is already read back, and Yes / No. "Counts now" items (yes_makes_mapped) come first: one yes moves that map to mapped.
 * Keyboard: y / n answer the focused row and move to the next; u clears.
 *
 * EVERY STRING IS textContent (the console's rule): el() never sets innerHTML.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else { root.EchoJayPluginReview = factory(); if (root.EchoJayAdmin && root.EchoJayAdmin.register) root.EchoJayAdmin.register(root.EchoJayPluginReview); }
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var LEDE = 'The human check for plugins EchoJay cannot dial yet because a control\'s job is not identified by its name and unit. ' +
             'Yes: this control does that job. No: it does not. Every answer saves at once and counts straight away.';
  var ROLE_WORDS = { threshold: 'Threshold', ratio: 'Ratio', attack: 'Attack', release: 'Release', makeup: 'Make-up', output: 'Output',
                     gain: 'Gain', freq: 'Frequency', q: 'Q', mix: 'Mix', delay_time: 'Delay time', decay: 'Decay', predelay: 'Pre-delay',
                     drive: 'Drive', ceiling: 'Ceiling' };

  // ---------------------------------------------------------------- pure --
  /** Cards: items grouped by map fp, "counts now" maps first, then by plugin name. */
  function group(items, onlyCountsNow) {
    var by = {}, order = [];
    items.forEach(function (it) {
      if (onlyCountsNow && !it.yes_makes_mapped) return;
      if (!by[it.fp]) { by[it.fp] = { fp: it.fp, plugin: it.plugin, vendor: it.vendor, version: it.version, category: it.category, items: [] }; order.push(it.fp); }
      by[it.fp].items.push(it);
    });
    var cards = order.map(function (fp) { var c = by[fp]; c.countsNow = c.items.some(function (i) { return i.yes_makes_mapped; }); return c; });
    cards.sort(function (a, b) { return (b.countsNow - a.countsNow) || String(a.plugin).localeCompare(String(b.plugin)); });
    return cards;
  }
  /** The counts the header shows, from the items and the answers alone (the server computes the same). */
  function counts(items, answers) {
    var answered = 0, mapsNow = {}, countsNowMaps = {}, countsNowItems = 0;
    items.forEach(function (it) {
      var a = answers[it.id];
      if (a && (a.verdict === 'yes' || a.verdict === 'no')) answered++;
      if (it.yes_makes_mapped) { countsNowItems++; countsNowMaps[it.fp] = true; if (a && a.verdict === 'yes') mapsNow[it.fp] = true; }
    });
    return { items: items.length, answered: answered, countsNowItems: countsNowItems, countsNowMaps: Object.keys(countsNowMaps).length,
             mapsMappedByAnswers: Object.keys(mapsNow).length, maps: Object.keys(items.reduce(function (m, i) { m[i.fp] = 1; return m; }, {})).length };
  }
  function roleWord(r) { return ROLE_WORDS[r] || r; }

  // ---------------------------------------------------------------- view --
  var el;
  function row(ctx, it, state, onAnswer) {
    var tr = el('tr', 'rv-row'); tr.tabIndex = 0; tr.dataset.id = it.id;
    var a = state.answers[it.id];
    if (a && a.verdict) tr.classList.add(a.verdict === 'yes' ? 'rv-yes' : 'rv-no');
    tr.appendChild(el('td', 'rv-control', it.control));
    tr.appendChild(el('td', 'rv-unit', it.unit));
    tr.appendChild(el('td', 'rv-role', roleWord(it.suggested_role)));
    var why = el('td', 'rv-why', it.why);
    if (it.yes_makes_mapped) why.appendChild(el('span', 'rv-tag', 'counts now'));
    if (!it.read_back) why.appendChild(el('span', 'rv-tag rv-tag-dim', 'not read back yet'));
    tr.appendChild(why);
    var td = el('td', 'rv-actions');
    var yes = el('button', 'adm-btn rv-btn-yes', 'Yes'), no = el('button', 'adm-btn rv-btn-no', 'No');
    yes.setAttribute('aria-pressed', String(!!(a && a.verdict === 'yes'))); no.setAttribute('aria-pressed', String(!!(a && a.verdict === 'no')));
    yes.disabled = no.disabled = !state.canWrite;
    yes.onclick = function () { onAnswer(it, 'yes', tr); };
    no.onclick = function () { onAnswer(it, 'no', tr); };
    td.appendChild(yes); td.appendChild(no); tr.appendChild(td);
    return tr;
  }
  function card(ctx, c, state, onAnswer) {
    var box = el('section', 'adm-card rv-card' + (c.countsNow ? ' rv-card-now' : ''));
    var h = el('h2', null, c.plugin);
    box.appendChild(h);
    box.appendChild(el('p', 'adm-empty rv-meta', [c.vendor, c.version, c.category].filter(Boolean).join(' · ')));
    var wrap = el('div', 'adm-table-wrap'), table = el('table', 'adm-table rv-table'), head = el('tr');
    ['Control', 'Unit', 'Suggested role', 'Why', ''].forEach(function (t) { head.appendChild(el('th', null, t)); });
    var thead = el('thead'); thead.appendChild(head); table.appendChild(thead);
    var tbody = el('tbody'); c.items.forEach(function (it) { tbody.appendChild(row(ctx, it, state, onAnswer)); }); table.appendChild(tbody);
    wrap.appendChild(table); box.appendChild(wrap);
    return box;
  }
  function header(ctx, state) {
    var k = counts(state.items, state.answers);
    var p = el('p', 'rv-counts');
    p.appendChild(el('strong', null, k.mapsMappedByAnswers + ' of ' + k.countsNowMaps));
    p.appendChild(el('span', null, ' maps moved to mapped by your answers · ' + k.answered + ' of ' + k.items + ' answered · ' + k.maps + ' plugins in the list'));
    return p;
  }

  function mount(ctx, host, opts) {
    el = ctx.el; opts = opts || {};
    var base = opts.base || '/admin/plugin-review';
    var state = { items: [], answers: {}, onlyNow: true, canWrite: ctx.canWrite ? ctx.canWrite() : true };
    host.appendChild(el('h1', 'adm-h1', 'Plugin role review'));
    host.appendChild(el('p', 'adm-lede', LEDE));
    var top = el('div', 'adm-section-head rv-top'), countsHost = el('div'), toggle = el('button', 'adm-btn', '');
    top.appendChild(countsHost); top.appendChild(toggle); host.appendChild(top);
    var list = el('div', 'rv-list'); host.appendChild(list);
    var status = el('p', 'adm-empty', 'Loading...'); host.appendChild(status);

    function render() {
      countsHost.textContent = ''; countsHost.appendChild(header(ctx, state));
      var k = counts(state.items, state.answers);
      toggle.textContent = state.onlyNow ? 'Show all ' + k.items + ' controls' : 'Show only the ' + k.countsNowItems + ' that count now';
      list.textContent = '';
      group(state.items, state.onlyNow).forEach(function (c) { list.appendChild(card(ctx, c, state, answer)); });
      status.textContent = state.items.length ? '' : 'Nothing to review.';
    }
    function next(tr) {
      var rows = Array.prototype.slice.call(list.querySelectorAll('tr.rv-row')), i = rows.indexOf(tr);
      for (var j = i + 1; j < rows.length; j++) if (!state.answers[rows[j].dataset.id]) { rows[j].focus(); return; }
    }
    function answer(it, verdict, tr) {
      return ctx.api2(base, { method: 'POST', body: JSON.stringify({ id: it.id, verdict: verdict }) }).then(function (d) {
        if (verdict === 'clear') delete state.answers[it.id]; else state.answers[it.id] = d.answer || { verdict: verdict };
        var keep = tr && tr.dataset.id; render();
        var now = keep && list.querySelector('tr.rv-row[data-id="' + CSS.escape(keep) + '"]'); if (now && verdict !== 'clear') next(now); else if (now) now.focus();
      }).catch(function (e) { status.textContent = (ctx.describeError ? ctx.describeError(e, 'The review') : 'The answer was not saved: ' + e.message); });
    }
    toggle.onclick = function () { state.onlyNow = !state.onlyNow; render(); };
    host.addEventListener('keydown', function (ev) {
      var tr = ev.target && ev.target.closest && ev.target.closest('tr.rv-row'); if (!tr || !state.canWrite) return;
      var it = state.items.filter(function (x) { return x.id === tr.dataset.id; })[0]; if (!it) return;
      if (ev.key === 'y') { ev.preventDefault(); answer(it, 'yes', tr); }
      if (ev.key === 'n') { ev.preventDefault(); answer(it, 'no', tr); }
      if (ev.key === 'u') { ev.preventDefault(); answer(it, 'clear', tr); }
    });
    return ctx.api2(base).then(function (d) {
      state.items = (d && d.items) || []; state.answers = (d && d.answers) || {};
      if (!state.items.some(function (i) { return i.yes_makes_mapped; })) state.onlyNow = false;
      render(); var first = list.querySelector('tr.rv-row'); if (first) first.focus();
    }).catch(function (e) { status.textContent = (ctx.describeError ? ctx.describeError(e, 'The review') : 'The review could not load: ' + e.message); });
  }

  return { key: 'plugin-review', label: 'Plugin review', mount: mount, LEDE: LEDE, __test: { group: group, counts: counts, roleWord: roleWord } };
}));
