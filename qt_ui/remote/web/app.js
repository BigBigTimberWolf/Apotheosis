'use strict';
const $ = id => document.getElementById(id);
let token = /^[0-9a-f]{64}$/.test(location.hash.slice(1)) ? location.hash.slice(1) : '';
try {
  if (token) sessionStorage.setItem('apotheosisToken', token);
  else token = sessionStorage.getItem('apotheosisToken') || '';
} catch (_) { /* Fragment still supports browsers with storage disabled. */ }
if (token) history.replaceState(null, '', location.pathname);
let current = null, selected = 'global', selectedGroup = '*', busy = false, online = false;
const changes = new Map();
const controls = new Map();
function message(text, error = false) { $('message').textContent = text; $('message').classList.toggle('error', error); }
function status() {
  $('connection').textContent = online ? '已连接 · 局域网' : '未连接';
  $('connection').className = 'badge ' + (online ? 'online' : 'offline');
  $('dirty').textContent = changes.size ? `${changes.size} 项未应用的修改` : current ? '参数已同步' : '等待连接';
  $('apply').disabled = busy || !online || !changes.size;
  $('reload').disabled = busy;
  $('group').disabled = busy; $('scope').disabled = busy;
}
async function api(path, body) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 8000);
  try {
    const response = await fetch(path, {
      method: body ? 'POST' : 'GET', cache: 'no-store', signal: controller.signal,
      headers: { Authorization: 'Bearer ' + token, ...(body ? {'Content-Type': 'application/json'} : {}) },
      ...(body ? {body: JSON.stringify(body)} : {})
    });
    const data = await response.json();
    if (!response.ok) { const e = new Error(data.error || `请求失败 (${response.status})`); e.status = response.status; throw e; }
    return data;
  } finally { clearTimeout(timeout); }
}
function option(select, value, label) { const o = document.createElement('option'); o.value = value; o.textContent = label; select.append(o); }
function visibleScopes() { return current.scopes.filter(s => $('group').value === '*' || s.group === $('group').value || s.id === 'global'); }
function selectors() {
  const group = $('group').value || '*';
  $('group').replaceChildren(); option($('group'), '*', '全部分组');
  for (const name of [...new Set(current.scopes.filter(s => s.id !== 'global').map(s => s.group))]) option($('group'), name, name || '未命名分组');
  $('group').value = [...$('group').options].some(o => o.value === group) ? group : '*';
  selectedGroup = $('group').value;
  scopeOptions();
}
function scopeOptions() {
  $('scope').replaceChildren();
  const scopes = visibleScopes();
  for (const s of scopes) option($('scope'), s.id, s.id === 'global' ? s.name : `${s.group} / ${s.name}`);
  if (!scopes.some(s => s.id === selected)) selected = scopes[0]?.id || 'global';
  $('scope').value = selected;
}
function numeric(v) { return Number(Number(v).toPrecision(7)); }
function render() {
  controls.clear(); $('cards').replaceChildren(); $('sections').replaceChildren();
  const scope = current.scopes.find(s => s.id === selected);
  if (!scope) return;
  $('profile').textContent = current.profile || '当前配置';
  $('activeGroup').textContent = '生效分组：' + (current.activeGroup || '默认');
  $('title').textContent = scope.name; $('note').textContent = scope.note;
  const sections = new Map();
  for (const f of scope.fields) {
    let grid = sections.get(f.section);
    if (!grid) {
      const card = document.createElement('section'); card.className = 'card'; card.id = 'section-' + sections.size;
      const title = document.createElement('h3'); title.textContent = f.section; card.append(title);
      grid = document.createElement('div'); grid.className = 'grid'; card.append(grid); $('cards').append(card);
      const anchor = document.createElement('a'); anchor.href = '#' + card.id; anchor.textContent = f.section; $('sections').append(anchor);
      sections.set(f.section, grid);
    }
    const row = document.createElement('div'); row.className = 'field';
    row.dataset.search = (f.label + ' ' + f.section + ' ' + f.key).toLowerCase();
    const label = document.createElement('label'); label.className = 'field-label'; label.textContent = f.label;
    const id = 'field-' + controls.size; label.htmlFor = id; row.append(label);
    let input, initial = f.value;
    if (f.type === 'boolean') {
      const line = document.createElement('label'); line.className = 'toggle-line';
      input = document.createElement('input'); input.type = 'checkbox'; input.checked = f.value;
      const text = document.createElement('span'); text.textContent = input.checked ? '已开启' : '已关闭';
      input.addEventListener('change', () => { text.textContent = input.checked ? '已开启' : '已关闭'; });
      line.append(input, text); row.append(line);
    } else if (f.type === 'choice') {
      input = document.createElement('select');
      for (const v of f.options) option(input, v, v || '默认');
      if (!f.options.includes(f.value)) option(input, f.value, f.value || '默认');
      input.value = f.value; row.append(input);
    } else {
      const box = document.createElement('div'); box.className = 'number-box';
      input = document.createElement('input'); input.type = 'number'; input.min = f.min; input.max = f.max;
      input.step = f.integer ? '1' : 'any'; input.inputMode = 'decimal';
      initial = numeric(f.value); input.value = initial;
      box.append(input);
      for (const [symbol, sign] of [['−', -1], ['+', 1]]) {
        const button = document.createElement('button'); button.type = 'button'; button.textContent = symbol;
        button.setAttribute('aria-label', `${sign > 0 ? '增加' : '减少'}${f.label}`);
        button.addEventListener('click', () => {
          if (busy) return;
          const value = input.value === '' ? initial : Number(input.value);
          input.value = Number(Math.min(f.max, Math.max(f.min, value + sign*f.step)).toFixed(7));
          input.dispatchEvent(new Event('input'));
        }); box.append(button);
      }
      row.append(box);
      const hint = document.createElement('span'); hint.className = 'range-hint'; hint.textContent = `${f.min} — ${f.max}　步长 ${f.step}`; row.append(hint);
    }
    input.id = id;
    const update = () => {
      const value = f.type === 'boolean' ? input.checked : f.type === 'choice' ? input.value : input.value === '' ? null : Number(input.value);
      if (value === initial) changes.delete(f.key); else changes.set(f.key, value);
      row.classList.toggle('changed', changes.has(f.key));
      $('saveHint').textContent = '点击应用后生效并保存到当前配置文件'; status();
    };
    input.addEventListener(f.type === 'number' ? 'input' : 'change', update);
    controls.set(f.key, {input, row}); grid.append(row);
  }
  filter(); status();
}
function filter() {
  const query = $('search').value.trim().toLowerCase(); let count = 0;
  for (const card of $('cards').children) {
    let visible = false;
    for (const row of card.querySelectorAll('.field')) { row.hidden = !row.dataset.search.includes(query); visible ||= !row.hidden; }
    card.hidden = !visible; if (visible) count++;
  }
  $('empty').hidden = count !== 0;
}
async function load(manual = false) {
  if (busy) return;
  if (manual && changes.size && !confirm('重新读取将丢弃网页上尚未应用的修改，继续吗？')) return;
  if (!manual && (changes.size || $('form').contains(document.activeElement))) return;
  busy = true; status();
  if (manual) $('form').inert = true;
  try {
    const data = await api('/api/state'); online = true;
    // Typing may begin while a background read is in flight. Never replace
    // those new edits with the response captured before them.
    if (!manual && (changes.size || $('form').contains(document.activeElement))) return;
    if (!current || data.revision !== current.revision || manual) {
      current = data; changes.clear(); selectors(); render();
    }
    if (manual) message('已读取本机最新参数。');
    else if ($('message').classList.contains('error')) message('连接已恢复。');
  } catch (e) {
    online = false; message(e.message === 'Failed to fetch' || e.name === 'AbortError' ? '连接失败，请确认服务已开启、两台电脑在同一局域网，且防火墙允许访问。' : e.message, true);
  } finally { busy = false; if (manual) $('form').inert = false; status(); }
}
$('apply').addEventListener('click', async () => {
  if (busy || !changes.size) return;
  // Validate changed fields only; legacy, untouched values are never rewritten.
  for (const key of changes.keys()) {
    const {input, row} = controls.get(key);
    if (changes.get(key) === null || !input.checkValidity()) {
      $('search').value = ''; filter(); row.scrollIntoView({block: 'center'}); input.focus();
      message('请填写有效数值，并保持在标注范围内。', true); return;
    }
  }
  busy = true; status();
  $('form').inert = true;
  try {
    const data = await api('/api/apply', {scope: selected, revision: current.revision, changes: Object.fromEntries(changes)});
    current = data; online = true; changes.clear(); selectors(); render();
    message('已应用并保存，本机参数已同步。');
    $('saveHint').textContent = '最近保存：' + new Date().toLocaleTimeString();
  } catch (e) {
    if (e.status === 401) online = false;
    message(e.status ? e.message : '连接中断，保存结果尚未确认。请重新读取本机参数后再继续。', true);
  } finally { busy = false; $('form').inert = false; status(); }
});
$('reload').addEventListener('click', () => load(true));
$('scope').addEventListener('change', () => {
  const next = $('scope').value;
  if (changes.size && !confirm('切换会丢弃尚未应用的修改，继续吗？')) { $('scope').value = selected; return; }
  selected = next; changes.clear(); message(''); render();
});
$('group').addEventListener('change', () => {
  if (changes.size && !confirm('切换分组会丢弃尚未应用的修改，继续吗？')) { $('group').value = selectedGroup; return; }
  selectedGroup = $('group').value; scopeOptions(); changes.clear(); message(''); render();
});
$('search').addEventListener('input', filter);
$('form').addEventListener('submit', event => event.preventDefault());
window.addEventListener('beforeunload', event => { if (changes.size) { event.preventDefault(); event.returnValue = ''; } });
if (!token) { message('请使用 Apotheosis「局域网调参」页面复制的完整地址打开。', true); status(); }
else { load(); setInterval(() => { if (!document.hidden) load(); }, 2500); }
