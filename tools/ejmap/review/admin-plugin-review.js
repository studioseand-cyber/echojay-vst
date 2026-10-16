/**
 * Plugin role review (Kathy, 16 Oct 2026): the human check for "roles not identified" (docs/TRUST_LEVELS_v0.md section 6).
 *
 * ONE MODULE, TWO HOSTS. The operator console tab (public/js/admin-plugin-review.js in echojay-saas, key 'plugin-review', mounted by
 * admin-shell.js like admin-community.js) and the local page today (tools/ejmap/review/index.html, served by review_server.py). Both
 * hand it a ctx with el() and api2(); the routes are the only difference:
 *     GET   <base>            -> { schema: "ejmap_role_review/1", generated_at, items: [...], answers: { <id>: answer } }
 *     POST  <base>  { id, verdict, role?, section? } -> { ok: true, answer | null, counts }
 *           verdict: "yes" (the suggestion) | "role" (the role + section picked in the dropdown) | "no" (not the suggestion, no role given) |
 *                    "other" (not a main control) | "unsure" (counts neither way: the needs-measuring / ask-Sean list) | "clear"
 * Local base: /api/review. Admin base: /admin/plugin-review (api2 prefixes /api/v2).
 *
 * WHAT A REVIEWER SEES: one card per plugin map; inside, the controls a person could settle - the name says a role the plugin's dial
 * needs, but the unit is missing / wrong, or there are several candidates. Each row: control, unit seen, suggested role, why, whether
 * the control is already read back, and Yes / No. "Counts now" items (yes_makes_mapped) come first: one yes moves that map to mapped.
 * Each row: a role dropdown (section + role, pre-set to the suggestion, plus "Other / not a main control"), Yes, No, Unsure.
 * Keyboard on a row: y / n / u answer and move on; r opens the dropdown; Backspace clears.
 *
 * EVERY STRING IS textContent (the console's rule): el() never sets innerHTML.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else { root.EchoJayPluginReview = factory(); if (root.EchoJayAdmin && root.EchoJayAdmin.register) root.EchoJayAdmin.register(root.EchoJayPluginReview); }
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var LEDE = 'The human check for plugins EchoJay cannot dial yet because a control\'s job is not identified by its name and unit. ' +
             'Each row asks one question: is this control the job shown - in that section? "Gate \u2192 Threshold" means the gate\'s threshold, ' +
             'not the compressor\'s. Yes: it is. A different job: pick it in the dropdown. No: it is not, and you are not saying what it is. ' +
             'Unsure: it goes to the Needs measuring / ask Sean list and counts neither way. Keys: y, n, u, r (dropdown). Every answer saves at once.';
  var ROLE_ORDER = ['threshold', 'ratio', 'attack', 'release', 'makeup', 'output', 'gain', 'freq', 'q', 'mix', 'delay_time', 'decay', 'predelay', 'drive', 'ceiling'];
  var SECTIONS = ['', 'Comp', 'Gate', 'EQ', 'LF', 'LMF', 'MF', 'HMF', 'HF', 'De-ess', 'Limiter'];
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
    var answered = 0, unsure = 0, mapsNow = {}, countsNowMaps = {}, countsNowItems = 0;
    items.forEach(function (it) {
      var a = answers[it.id];
      if (a && a.verdict && a.verdict !== 'unsure') answered++;
      if (a && a.verdict === 'unsure') unsure++;
      var isSuggestion = a && (a.verdict === 'yes' || (a.verdict === 'role' && a.role === it.suggested_role && (a.section || '') === (it.section || '')));
      if (it.yes_makes_mapped) { countsNowItems++; countsNowMaps[it.fp] = true; if (isSuggestion) mapsNow[it.fp] = true; }
    });
    return { items: items.length, answered: answered, unsure: unsure, countsNowItems: countsNowItems, countsNowMaps: Object.keys(countsNowMaps).length,
             mapsMappedByAnswers: Object.keys(mapsNow).length, maps: Object.keys(items.reduce(function (m, i) { m[i.fp] = 1; return m; }, {})).length };
  }
  function roleWord(r) { return ROLE_WORDS[r] || r; }
  /** The question a row asks (16 Oct): the section the control's name puts the role in, then the role - "Gate \u2192 Threshold";
   *  a control with no section prefix asks the plugin's own role - "Threshold". */
  function question(it) { return label(it.section, it.suggested_role); }
  function label(section, role) { return role === 'other' ? 'Other / not a main control' : (section ? section + ' \u2192 ' : '') + roleWord(role); }
  /** The dropdown's options: the suggestion first, then every section this plugin's controls name (and the plugin's own, and the
   *  standard ones) x every role, then Other. Values are "<section>|<role>". */
  function options(it) {
    var secs = [it.section || ''].concat(it.plugin_sections || [], SECTIONS), seen = {}, out = [{ value: (it.section || '') + '|' + it.suggested_role, text: question(it) + '  (suggested)' }];
    seen[out[0].value] = true;
    secs.forEach(function (sec) { ROLE_ORDER.forEach(function (r) { var v = sec + '|' + r; if (seen[v]) return; seen[v] = true; out.push({ value: v, text: label(sec, r), group: sec || 'This plugin' }); }); });
    out.push({ value: '|other', text: label('', 'other') });
    return out;
  }
  /** What the row's answer means for the dropdown: its picked role, or the suggestion. */
  function chosen(it, a) {
    if (a && a.verdict === 'role') return (a.section || '') + '|' + a.role;
    if (a && a.verdict === 'other') return '|other';
    return (it.section || '') + '|' + it.suggested_role;
  }

  // ---------------------------------------------------------------- view --
  var el;
  function row(ctx, it, state, onAnswer) {
    var tr = el('tr', 'rv-row'); tr.tabIndex = 0; tr.dataset.id = it.id;
    var a = state.answers[it.id];
    if (a && a.verdict) tr.classList.add('rv-' + a.verdict);
    tr.appendChild(el('td', 'rv-control', it.control));
    tr.appendChild(el('td', 'rv-unit', it.unit));
    var roleTd = el('td', 'rv-role'), sel = el('select', 'rv-select');
    sel.setAttribute('aria-label', 'The job of ' + it.control);
    var groups = {};
    options(it).forEach(function (o) {
      var opt = el('option', null, o.text); opt.value = o.value;
      if (!o.group) { sel.appendChild(opt); return; }
      if (!groups[o.group]) { groups[o.group] = el('optgroup'); groups[o.group].label = o.group; sel.appendChild(groups[o.group]); }
      groups[o.group].appendChild(opt);
    });
    sel.value = chosen(it, a); sel.disabled = !state.canWrite;
    sel.onchange = function () {
      var parts = sel.value.split('|'), sec = parts[0], role = parts[1];
      if (role === 'other') return onAnswer(it, { verdict: 'other', role: 'other', section: '' }, tr);
      if (role === it.suggested_role && sec === (it.section || '')) return onAnswer(it, { verdict: 'yes' }, tr);
      onAnswer(it, { verdict: 'role', role: role, section: sec }, tr);
    };
    roleTd.appendChild(sel); tr.appendChild(roleTd);
    var why = el('td', 'rv-why', it.why);
    if (it.yes_makes_mapped) why.appendChild(el('span', 'rv-tag', 'counts now'));
    if (!it.read_back) why.appendChild(el('span', 'rv-tag rv-tag-dim', 'not read back yet'));
    tr.appendChild(why);
    var td = el('td', 'rv-actions');
    [['yes', 'Yes', 'rv-btn-yes', 'Yes: ' + it.control + ' is the ' + question(it)],
     ['no', 'No', 'rv-btn-no', 'No: ' + it.control + ' is not the ' + question(it) + ' (no role given)'],
     ['unsure', 'Unsure', 'rv-btn-unsure', 'Unsure: needs measuring or a question for Sean - counts neither way']].forEach(function (b) {
      var btn = el('button', 'adm-btn ' + b[2], b[1]); btn.title = b[3];
      btn.setAttribute('aria-pressed', String(!!(a && a.verdict === b[0]))); btn.disabled = !state.canWrite;
      btn.onclick = function () { onAnswer(it, { verdict: b[0] }, tr); };
      td.appendChild(btn);
    });
    tr.appendChild(td);
    return tr;
  }
  function card(ctx, c, state, onAnswer) {
    var box = el('section', 'adm-card rv-card' + (c.countsNow ? ' rv-card-now' : ''));
    var h = el('h2', null, c.plugin);
    box.appendChild(h);
    box.appendChild(el('p', 'adm-empty rv-meta', [c.vendor, c.version, c.category].filter(Boolean).join(' · ')));
    var wrap = el('div', 'adm-table-wrap'), table = el('table', 'adm-table rv-table'), head = el('tr');
    ['Control', 'Unit', 'Is it the...', 'Why', ''].forEach(function (t) { head.appendChild(el('th', null, t)); });
    var thead = el('thead'); thead.appendChild(head); table.appendChild(thead);
    var tbody = el('tbody'); c.items.forEach(function (it) { tbody.appendChild(row(ctx, it, state, onAnswer)); }); table.appendChild(tbody);
    wrap.appendChild(table); box.appendChild(wrap);
    return box;
  }
  function header(ctx, state) {
    var k = counts(state.items, state.answers);
    var p = el('p', 'rv-counts');
    p.appendChild(el('strong', null, k.mapsMappedByAnswers + ' of ' + k.countsNowMaps));
    p.appendChild(el('span', null, ' maps moved to mapped by your answers · ' + k.answered + ' of ' + k.items + ' answered · ' + k.unsure + ' unsure · ' + k.maps + ' plugins in the list'));
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
      var unsureItems = state.items.filter(function (i) { var a = state.answers[i.id]; return a && a.verdict === 'unsure'; });
      if (unsureItems.length) {
        var u = el('section', 'adm-card rv-unsure'); u.appendChild(el('h2', null, 'Needs measuring / ask Sean (' + unsureItems.length + ')'));
        var w = el('div', 'adm-table-wrap'), t = el('table', 'adm-table rv-table'), tb = el('tbody');
        unsureItems.forEach(function (i) { var r = row(ctx, i, state, answer); r.insertBefore(el('td', 'rv-plugin', i.plugin), r.firstChild); tb.appendChild(r); });
        t.appendChild(tb); w.appendChild(t); u.appendChild(w); list.appendChild(u);
      }
      var rest = state.items.filter(function (i) { var a = state.answers[i.id]; return !(a && a.verdict === 'unsure'); });
      group(rest, state.onlyNow).forEach(function (c) { list.appendChild(card(ctx, c, state, answer)); });
      status.textContent = state.items.length ? '' : 'Nothing to review.';
    }
    function next(tr) {
      var rows = Array.prototype.slice.call(list.querySelectorAll('tr.rv-row')), i = rows.indexOf(tr);
      for (var j = i + 1; j < rows.length; j++) if (!state.answers[rows[j].dataset.id]) { rows[j].focus(); return; }
    }
    function answer(it, v, tr) {
      if (typeof v === 'string') v = { verdict: v };
      return ctx.api2(base, { method: 'POST', body: JSON.stringify(Object.assign({ id: it.id }, v)) }).then(function (d) {
        if (v.verdict === 'clear') delete state.answers[it.id]; else state.answers[it.id] = d.answer || v;
        var keep = tr && tr.dataset.id; render();
        var now = keep && list.querySelector('tr.rv-row[data-id="' + CSS.escape(keep) + '"]'); if (now && v.verdict !== 'clear') next(now); else if (now) now.focus();
      }).catch(function (e) { status.textContent = (ctx.describeError ? ctx.describeError(e, 'The review') : 'The answer was not saved: ' + e.message); });
    }
    toggle.onclick = function () { state.onlyNow = !state.onlyNow; render(); };
    host.addEventListener('keydown', function (ev) {
      var tr = ev.target && ev.target.closest && ev.target.closest('tr.rv-row'); if (!tr || !state.canWrite) return;
      var it = state.items.filter(function (x) { return x.id === tr.dataset.id; })[0]; if (!it) return;
      if (ev.target.tagName === 'SELECT') return;   // the dropdown keeps its own keys
      if (ev.key === 'y') { ev.preventDefault(); answer(it, 'yes', tr); }
      if (ev.key === 'n') { ev.preventDefault(); answer(it, 'no', tr); }
      if (ev.key === 'u') { ev.preventDefault(); answer(it, 'unsure', tr); }
      if (ev.key === 'Backspace') { ev.preventDefault(); answer(it, 'clear', tr); }
      if (ev.key === 'r') { ev.preventDefault(); var sel = tr.querySelector('select'); if (sel) { sel.focus(); try { sel.showPicker(); } catch (e) {} } }
    });
    return ctx.api2(base).then(function (d) {
      state.items = (d && d.items) || []; state.answers = (d && d.answers) || {};
      if (!state.items.some(function (i) { return i.yes_makes_mapped; })) state.onlyNow = false;
      render(); var first = list.querySelector('tr.rv-row'); if (first) first.focus();
    }).catch(function (e) { status.textContent = (ctx.describeError ? ctx.describeError(e, 'The review') : 'The review could not load: ' + e.message); });
  }

  return { key: 'plugin-review', label: 'Plugin review', mount: mount, LEDE: LEDE, __test: { group: group, counts: counts, roleWord: roleWord, question: question, options: options, chosen: chosen, label: label } };
}));
