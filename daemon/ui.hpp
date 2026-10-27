// The read-only web page of beliefd (/ui): domains and their counts, claims, hypotheses, sources and
// questions newest first, search by words and by meaning, and for any node its "why": sources, credence,
// the tree it rests on. One file, no build step, served by the daemon itself behind the UI password.
#pragma once

namespace beliefd {

inline const char* kUiHtml = R"HTML(<!doctype html>
<html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Память</title>
<style>
:root{--bg:#f6f4ef;--fg:#1d1c1a;--mute:#6b6760;--line:#dcd8cf;--card:#fffdf8;--acc:#8a4b1f;--acc2:#2f6b4f;--warn:#9a2f2f;--chip:#ece8df}
@media(prefers-color-scheme:dark){:root{--bg:#171614;--fg:#e8e4dc;--mute:#9a958c;--line:#2e2c28;--card:#1f1e1b;--acc:#e0a06a;--acc2:#7cc3a0;--warn:#e07b7b;--chip:#2a2824}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
header{display:flex;gap:12px;align-items:center;padding:10px 16px;border-bottom:1px solid var(--line);position:sticky;top:0;background:var(--bg);z-index:2;flex-wrap:wrap}
header h1{font-size:18px;margin:0 8px 0 0}header input{flex:1;min-width:200px;padding:8px 10px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--fg);font-size:15px}
#health{color:var(--mute);font-size:13px}
.wrap{display:grid;grid-template-columns:220px 1fr;min-height:calc(100vh - 56px)}
nav{border-right:1px solid var(--line);padding:10px 0}nav a{display:flex;justify-content:space-between;padding:7px 16px;color:var(--fg);text-decoration:none;cursor:pointer}
nav a.on{background:var(--chip);font-weight:600}nav a small{color:var(--mute)}
main{padding:12px 16px;min-width:0}
.tabs{display:flex;gap:6px;margin-bottom:10px;flex-wrap:wrap}.tabs button{padding:6px 12px;border:1px solid var(--line);background:var(--card);color:var(--fg);border-radius:999px;cursor:pointer}
.tabs button.on{background:var(--acc);color:#fff;border-color:var(--acc)}
.row{padding:10px 12px;border:1px solid var(--line);border-radius:10px;background:var(--card);margin-bottom:8px;cursor:pointer}
.row:hover{border-color:var(--acc)}.meta{display:flex;gap:10px;flex-wrap:wrap;color:var(--mute);font-size:12.5px;margin-top:4px}
.chip{background:var(--chip);border-radius:6px;padding:1px 6px}.cred{color:var(--acc2);font-weight:600}.st-retracted,.st-superseded{color:var(--warn)}
#more{margin:8px 0 24px;padding:8px 14px;border:1px solid var(--line);background:var(--card);color:var(--fg);border-radius:8px;cursor:pointer}
#detail{position:fixed;inset:0 0 0 auto;width:min(640px,100%);background:var(--card);border-left:1px solid var(--line);box-shadow:-8px 0 24px rgba(0,0,0,.15);overflow:auto;padding:16px;display:none;z-index:3}
#detail.on{display:block}#detail h2{font-size:17px;margin:0 32px 8px 0}#detail .x{position:absolute;right:12px;top:10px;font-size:22px;cursor:pointer;background:none;border:0;color:var(--fg)}
#detail h3{font-size:13px;text-transform:uppercase;letter-spacing:.04em;color:var(--mute);margin:16px 0 6px}
.why ul{list-style:none;padding-left:16px;margin:2px 0;border-left:1px dashed var(--line)}.why li{margin:3px 0}.why .id{color:var(--acc);cursor:pointer;font-size:12px}
.kv{display:grid;grid-template-columns:auto 1fr;gap:3px 12px;font-size:13.5px}.kv b{color:var(--mute);font-weight:500}
.empty{color:var(--mute);padding:20px 4px}pre{white-space:pre-wrap;font:13px/1.4 ui-monospace,Menlo,monospace;background:var(--chip);padding:8px;border-radius:8px}
@media(max-width:720px){.wrap{grid-template-columns:1fr}nav{display:flex;overflow-x:auto;border-right:0;border-bottom:1px solid var(--line);padding:4px 8px}nav a{white-space:nowrap;gap:6px}}
</style></head><body>
<header><h1>Память</h1><input id="q" placeholder="поиск по словам и по смыслу, Enter"><span id="health"></span></header>
<div class="wrap"><nav id="domains"></nav>
<main><div class="tabs" id="tabs">
<button data-t="claim" class="on">Утверждения</button><button data-t="hypothesis">Гипотезы</button><button data-t="source">Источники</button><button data-t="question">Вопросы</button><button data-t="conflicts">Противоречия</button></div>
<div id="list"></div><button id="more" style="display:none">ещё</button></main></div>
<aside id="detail"><button class="x" onclick="closeDetail()">×</button><div id="dbody"></div></aside>
<script>
const S={domain:'',tab:'claim',offset:0,total:0,q:''};
const $=s=>document.querySelector(s);
const esc=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const when=s=>s?new Date(s).toLocaleString('ru-RU',{day:'2-digit',month:'2-digit',year:'2-digit',hour:'2-digit',minute:'2-digit'}):'';
const pct=c=>c&&c.value!=null?Math.round(c.value*100)+'%':'';
async function api(path){const r=await fetch(path,{credentials:'same-origin'});if(!r.ok){throw new Error(r.status+' '+path)}return r.json()}
async function health(){try{const h=await api('/health');const k=h.counts;$('#health').textContent=`${k.claims} утверждений · ${k.hypotheses} гипотез · ${k.sources} источников`+(h.quiet_now?' · тихие часы':'')}catch(e){$('#health').textContent='beliefd недоступен'}}
async function domains(){const d=await api('/domains');const nav=$('#domains');let html=`<a data-d="" class="${S.domain===''?'on':''}">все <small>${d.entries.reduce((a,x)=>a+x.claims,0)}</small></a>`;
for(const x of d.entries)html+=`<a data-d="${esc(x.domain)}" class="${S.domain===x.domain?'on':''}" title="гипотез ${x.hypotheses}, источников ${x.sources}, последнее ${when(x.newest)}">${esc(x.domain)} <small>${x.claims}</small></a>`;
nav.innerHTML=html;nav.querySelectorAll('a').forEach(a=>a.onclick=()=>{S.domain=a.dataset.d;S.offset=0;S.q='';$('#q').value='';domains();load()})}
function credOf(n){return n.credence?pct(n.credence):''}
function rowNode(n){const p=n.payload||{};const attr=p.attribution?`<span class="chip">${esc(p.attribution)}</span>`:'';const sp=p.subject||p.predicate?`<span>${esc(p.subject||'')} ${esc(p.predicate||'')}</span>`:'';
return `<div class="row" data-id="${esc(n.id)}"><div>${esc(n.label||p.text||p.uri||n.id)}</div><div class="meta"><span class="cred">${credOf(n)}</span>${sp}${attr}<span class="chip">${esc(n.domain)}</span><span class="st-${esc(n.status)}">${esc(n.status)}</span><span>${when(n.created_at)}</span></div></div>`}
function rowHyp(h){return `<div class="row" data-id="${esc(h.id)}"><div>${esc(h.text)}</div><div class="meta"><span class="cred">${pct(h.credence)}</span><span class="chip">порядок ${h.order}</span><span>опор: ${(h.from||[]).length}</span><span class="chip">${esc(h.domain)}</span><span class="st-${esc(h.status)}">${esc(h.status)}${h.outcome?' · '+esc(h.outcome):''}</span><span>${when(h.created_at)}</span></div></div>`}
function rowQ(q){return `<div class="row" data-id="${esc(q.id)}"><div>${esc(q.text||q.question||'')}</div><div class="meta"><span class="chip">${esc(q.domain||S.domain)}</span><span class="st-${esc(q.status)}">${esc(q.status||'')}</span>${q.resolution?`<span>${esc(q.resolution)}</span>`:''}<span>${when(q.created_at)}</span></div></div>`}
function rowConflict(c){const a=c.a||{},b=c.b||{};return `<div class="row" data-id="${esc(a.id)}"><div>${esc(a.text)}</div><div class="meta"><span class="cred">${pct(a.credence)}</span><span>${esc(a.value||'')}</span></div><div style="margin:6px 0;color:var(--warn)">⇄ ${esc(c.kind||'противоречие')}${c.decided?' · '+esc(c.decided):''}</div><div data-id="${esc(b.id)}">${esc(b.text)}</div><div class="meta"><span class="cred">${pct(b.credence)}</span><span>${esc(b.value||'')}</span></div></div>`}
function rowHit(e){return `<div class="row" data-id="${esc(e.id)}"><div>${esc(e.text)}</div><div class="meta"><span class="chip">${esc(e.type)}</span>${e.cosine!=null?`<span>смысл ${Math.round(e.cosine*100)}%</span>`:''}${e.structural?'<span>та же тема</span>':''}<span class="chip">${esc(e.domain)}</span><span class="st-${esc(e.status)}">${esc(e.status)}</span><span>${when(e.created_at)}</span></div></div>`}
function bind(el){el.querySelectorAll('[data-id]').forEach(r=>r.onclick=ev=>{ev.stopPropagation();detail(r.dataset.id)})}
async function load(append){const list=$('#list');if(!append){list.innerHTML='';S.offset=0}const dom=S.domain?'&domain='+encodeURIComponent(S.domain):'';let html='';
try{if(S.q){const r=await api(`/search?q=${encodeURIComponent(S.q)}&limit=100${dom}`);html=r.entries.map(rowHit).join('')||'<div class="empty">ничего не нашлось</div>';S.total=0}
else if(S.tab==='hypothesis'){const r=await api(`/hypotheses?all=1${dom}`);const hs=r.entries.sort((a,b)=>(b.created_at||'').localeCompare(a.created_at||''));html=hs.map(rowHyp).join('')||'<div class="empty">гипотез нет</div>';S.total=0}
else if(S.tab==='question'){const r=await api(`/questions?all=1${dom}`);html=r.entries.map(rowQ).join('')||'<div class="empty">вопросов нет</div>';S.total=0}
else if(S.tab==='conflicts'){const r=await api(`/conflicts?${dom.slice(1)}`);const es=r.entries||r;html=(es||[]).map(rowConflict).join('')||'<div class="empty">противоречий нет</div>';S.total=0}
else{const r=await api(`/nodes?type=${S.tab}&offset=${S.offset}&limit=50${dom}`);html=r.entries.map(rowNode).join('')||(S.offset?'':'<div class="empty">пусто</div>');S.total=r.total;S.offset+=r.entries.length}}
catch(e){html=`<div class="empty">ошибка: ${esc(e.message)}</div>`}
list.insertAdjacentHTML('beforeend',html);bind(list);$('#more').style.display=S.total>S.offset?'':'none'}
function whyTree(w){const kids=(w.children||[]).map(c=>`<li><span class="id" data-id="${esc(c.id)}">${esc(c.type)}</span> ${esc(c.label)} <span class="cred">${pct(c.credence)}</span>${c.via?` <small>(${esc(c.via)}${c.rule?', '+esc(c.rule):''})</small>`:''}${c.notice?` <span class="st-retracted">${esc(c.notice)}</span>`:''}${c.truncated?' …':''}${whyTree(c)}</li>`).join('');return kids?`<ul>${kids}</ul>`:''}
async function detail(id){const d=$('#detail'),b=$('#dbody');d.classList.add('on');b.innerHTML='<div class="empty">загружаю…</div>';
try{const n=await api('/node/'+encodeURIComponent(id));const p=n.payload||{};let html=`<h2>${esc(p.text||((p.kind?p.kind+': ':'')+(p.uri||''))||p.name||id)}</h2>`;
html+=`<div class="kv"><b>id</b><span>${esc(n.id)}</span><b>тип</b><span>${esc(n.type)} · ${esc(n.status)} · ${esc(n.sensitivity)}</span><b>домен</b><span>${esc(n.domain)}</span><b>создано</b><span>${when(n.created_at)}</span>`;
if(n.credence)html+=`<b>уверенность</b><span class="cred">${pct(n.credence)} <small>(${esc(n.credence.method)}${n.credence.stale?', устарело':''})</small></span>`;
if(p.subject||p.predicate)html+=`<b>о чём</b><span>${esc(p.subject||'')} · ${esc(p.predicate||'')}${p.value&&p.value.value!=null?' = '+esc(JSON.stringify(p.value.value))+(p.value.unit?' '+esc(p.value.unit):''):''}</span>`;
if(p.attribution)html+=`<b>кто сказал</b><span>${esc(p.attribution)}</span>`;if(p.observed_at)html+=`<b>наблюдение</b><span>${when(p.observed_at)}</span>`;if(p.uri)html+=`<b>uri</b><span>${esc(p.uri)}</span>`;
if(p.retracted)html+=`<b>отозвано</b><span>${esc(p.retracted.reason)} (${when(p.retracted.at)})</span>`;html+='</div>';
if(p.content)html+=`<h3>Содержимое источника</h3><pre>${esc(p.content)}</pre>`;
if(n.type==='claim'||n.type==='hypothesis'){try{const w=await api('/why/'+encodeURIComponent(id));const nu=w.nutrition||{};
html+=`<h3>На чём стоит</h3><div class="kv"><b>источников</b><span>${nu.n_sources??'?'} (${(nu.attributions||[]).map(esc).join(', ')||'без атрибуции'})</span><b>вес</b><span>${nu.weighted!=null?Number(nu.weighted).toFixed(2):'?'}${nu.order>1?' · '+nu.order+'-й порядок, ×'+Number(nu.order_multiplier).toFixed(4):''}</span><b>глубина</b><span>${nu.depth??'?'}${nu.dilution!=null?' · разбавление '+Number(nu.dilution).toFixed(2):''}</span>${nu.oldest_observed_at?`<b>наблюдения</b><span>${when(nu.oldest_observed_at)} — ${when(nu.newest_observed_at)}</span>`:''}</div>`;
if(w.notice)html+=`<div class="st-retracted">${esc(w.notice)}</div>`;html+=`<div class="why">${whyTree(w)||'<div class="empty">опор нет</div>'}</div>`}catch(e){html+=`<div class="empty">почему: ${esc(e.message)}</div>`}}
const edges=(n.edges||[]).filter(e=>!(n.type==='claim'&&e.dir==='out'&&e.type==='supports'));
if(edges.length){html+='<h3>Связи</h3><div class="why"><ul>'+edges.map(e=>`<li>${e.dir==='out'?'→':'←'} ${esc(e.type)} <span class="id" data-id="${esc(e.dir==='out'?e.to:e.from)}">${esc(e.dir==='out'?e.to:e.from)}</span></li>`).join('')+'</ul></div>'}
b.innerHTML=html;bind(b)}catch(e){b.innerHTML=`<div class="empty">не открылось: ${esc(e.message)}</div>`}}
function closeDetail(){$('#detail').classList.remove('on')}
$('#tabs').querySelectorAll('button').forEach(b=>b.onclick=()=>{S.tab=b.dataset.t;S.q='';$('#q').value='';$('#tabs').querySelectorAll('button').forEach(x=>x.classList.toggle('on',x===b));load()});
$('#more').onclick=()=>load(true);$('#q').addEventListener('keydown',e=>{if(e.key==='Enter'){S.q=e.target.value.trim();load()}});
document.addEventListener('keydown',e=>{if(e.key==='Escape')closeDetail()});
health();domains();load();
</script></body></html>
)HTML";

}  // namespace beliefd
