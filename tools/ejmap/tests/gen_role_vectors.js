// Regenerates tools/ejmap/tests/fixtures/name-token-vectors.json and role-classification-74.json
// from the SERVER'S OWN matcher, loaded unmodified from its file (no copy of server code lives here).
//
//   node tools/ejmap/tests/gen_role_vectors.js <echojay-saas>/lib/controls-note.js <dir of the 74 fixtures> tools/ejmap/tests/fixtures
//
// 29 Sep inputs: controls-note.js at echojay-saas origin/main a86dba8; the 74 fixtures as pushed at 2454c0a.
// The reference classifier below is the rule EjmapRoles.h implements, less its readout clause (none of the 74 carries a
// readout field); RoundTripTest asserts the C++ against its output.
// Change the matcher on the server first, then regenerate, then re-run both ends (EJMAP_CERT_DRIVER.md section 9).
const fs=require('fs'), vm=require('vm'), path=require('path');
const SERVER_FILE=process.argv[2], dir=process.argv[3], outDir=process.argv[4];
if (!SERVER_FILE || !dir || !outDir) { console.error('usage: gen_role_vectors.js <controls-note.js> <fixtures dir> <out dir>'); process.exit(2); }
const src=fs.readFileSync(SERVER_FILE,'utf8');
const mod={exports:{}}; vm.runInNewContext(src+'\n;module.exports.__controlNameTokens=controlNameTokens;module.exports.__BAND_ABBREV=BAND_ABBREV;', {module:mod, exports:mod.exports, console, require});
const tokens=mod.exports.__controlNameTokens, answers=mod.exports.controlAnswersTerm, BAND_ABBREV=mod.exports.__BAND_ABBREV;
const LEX={ threshold:['threshold','thresh','thr','peak reduction','peak reduct'], ratio:['ratio','rat'],
  attack:['attack','atk','att'], release:['release','recovery','recover','rel','rec','rcvr','rcv'],
  input:['input','input gain','in gain'], output:['output','out','output gain','out gain'], makeup:['makeup','make up'],
  mix:['mix','dry wet','wet','blend','parallel mix'] };
const TUNER={ key:['key','scale'], reference:['reference','ref'], strength:['strength','retune','speed','amount'] };
const VETO={ sidechain:['sc','sidechain','side chain','key','ext'], meter:['meter','vu','gr'],
  filter:['filter','hp','lp','hpf','lpf','freq','frequency'], preset:['preset','trigger'] };
const hit=(n,terms)=>terms.some(t=>answers(n,t));
function roleOf(name, category){
  const lex = category==='tuner' ? {...LEX,...TUNER} : LEX;
  const veto = Object.entries(VETO).filter(([f,t])=> !(category==='tuner' && f==='sidechain') && hit(name,t)).map(([f])=>f);
  if (veto.length) return {role:null, reason:'veto:'+veto.join('+')};
  const r=Object.keys(lex).filter(k=>hit(name,lex[k]));
  if (r.length>1) return {role:null, reason:'ambiguous:'+r.join('|')};
  const flags = hit(name,['dc']) ? ['dc'] : [];
  return {role:r[0]||null, flags};
}
function classify(controls, category){
  const per=controls.map(c=>({index:c.index,name:c.name,...roleOf(c.name,category)}));
  const thr=per.filter(p=>p.role==='threshold');
  let cls;
  if (thr.length===1) cls='single_threshold';
  else if (thr.length>1){
    const n=thr.map(t=>t.name.toLowerCase()).join(' ');
    const exp=thr.filter(t=>hit(t.name,['exp','expander','strap']));
    if (exp.length && thr.length-exp.length===1) cls='comp_over_expander';
    else if (/\blfe\b/.test(n)) cls='surround';
    else if (thr.every(t=>/^(l|r)\b/i.test(t.name) || /\b(l|r)$/i.test(t.name))) cls='channels_lr';
    else cls='bands_or_stages';
  } else if (per.some(p=>p.role==='input')) cls='input_as_threshold';
  else if (controls.some(c=>hit(c.name,['density','reduction','amount','compress','compression','drive','tension']))) cls='amount_only';
  else cls='none';
  return {cls, thresholds:thr.map(t=>t.name), per};
}

const fixtures=fs.readdirSync(dir).filter(f=>f.endsWith('.json')).sort().map(f=>JSON.parse(fs.readFileSync(path.join(dir,f),'utf8')));
// ---- FINAL per-control roles: the name roles, then the two flagged product-level rules ----
function finalRoles(controls){
  const c=classify(controls,'compressor');
  const per=c.per.map(p=>({index:p.index,name:p.name,role:p.role,flags:[...(p.flags||[])],reason:p.reason||''}));
  if (c.cls==='input_as_threshold')
    for (const p of per) if (p.role==='input'){ p.role='threshold'; p.flags.push('input_as_threshold'); }
  if (c.cls==='comp_over_expander')
    for (const p of per) if (p.role==='threshold'){
      if (['exp','expander','strap'].some(t=>answers(p.name,t))){ p.role=null; p.reason='expander_or_strap_threshold'; }
      else p.flags.push('comp_over_expander'); }
  return {cls:c.cls, per};
}
const products=fixtures.map(d=>{ const f=finalRoles(d.controls); return {product:d.product, cls:f.cls, controls:f.per}; });
const counts={}; products.forEach(p=>counts[p.cls]=(counts[p.cls]||0)+1);
fs.writeFileSync(path.join(outDir,'role-classification-74.json'), JSON.stringify({
  _:"PINNED role classification of the 74 compressor-profile fixtures (echojay-saas origin/main 2454c0a). Computed by a REFERENCE classifier running the SERVER'S OWN controlNameTokens/controlAnswersTerm (lib/controls-note.js @ a86dba8, unmodified, in node); EJ Map's C++ (EjmapRoles.h) must reproduce every product's class and every control's role, flags and reason. A lexicon change that moves a product between classes fails RoundTripTest.",
  counts, products},null,1));
// ---- TOKENISER / MATCHER VECTORS ----
const names=[...new Set(fixtures.flatMap(d=>d.controls.map(c=>c.name)))].sort();
const edge=["LDelayTime","Ch1LoGain","Gate Expander","Threshold","Calibration","Band 1 Threshold","CompThresh","Peak Reduct","LF Gain","HMF Freq","Low Cut","aBcD","a1b2","X2Y","","   ","Dry/Wet Mix","Comp. Amount","İnput","Key","Ärger Gain","Make-Up","Half Time","Shelf Gain","Selfmid","HPF Freq","Chpf","LF_Gain","lf2","Low-Mid","HMF Gain","Alpha Mix"];
const allNames=[...new Set([...names,...edge])];
const terms=[...new Set([...Object.values(LEX).flat(),...Object.values(TUNER).flat(),...Object.values(VETO).flat(),'dc','exp','expander','strap','density','reduction','amount','compress','compression','drive','tension','low','high','mid','low mid','high mid','low pass','high pass','low cut','high cut','pan','hold','mic','mf gain'])];
const BAND=BAND_ABBREV;
const sq=s=>String(s).toLowerCase().replace(/[^a-z0-9]/g,'');
const answerVectors=[];
for (const n of allNames) for (const t of terms){
  const r=answers(n,t);
  const abbrevInside = (BAND.get(t)||[]).some(a=>n.toLowerCase().includes(a));
  if (r || (sq(t) && sq(n).includes(sq(t))) || abbrevInside) answerVectors.push({name:n,term:t,result:r});   // every hit, every LETTER-match the rule must refuse, and every band abbreviation present without its word boundary
}
fs.writeFileSync(path.join(outDir,'name-token-vectors.json'), JSON.stringify({
  _:"controlNameTokens and controlAnswersTerm VECTORS, produced by running the SERVER'S OWN functions (echojay-saas lib/controls-note.js @ a86dba8, unmodified, in node). ASSERTED BY EJ Map (tools/ejmap/tests/RoundTripTest.cpp testNameTokenVectors) against its port, EjmapNameTokens.h. The server should assert the SAME file against its copy (a server-half item), so the two cannot drift. `answers` holds every hit plus every case where the term's letters occur inside the name but not as a contiguous token run - the matches the rule exists to refuse - and every name containing a band abbreviation's letters (lf, hmf, hpf...) for that band term, so the \\b boundary is pinned both ways.",
  tokens: allNames.map(n=>({name:n,tokens:tokens(n)})), answers:answerVectors},null,1));
console.log(JSON.stringify(counts)); console.log('token vectors', allNames.length, 'answer vectors', answerVectors.length, '(true', answerVectors.filter(a=>a.result).length, ', refusals', answerVectors.filter(a=>!a.result).length, ')');
