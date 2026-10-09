/* WonderScope dashboard. Plain JS, no dependencies (works offline on the board's AP). */
'use strict';
(() => {

// =====================================================================
// Utilities
// =====================================================================
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const SVGNS = 'http://www.w3.org/2000/svg';

function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  setAttrs(el, attrs);
  append(el, kids);
  return el;
}
function sv(tag, attrs, ...kids) {
  const el = document.createElementNS(SVGNS, tag);
  setAttrs(el, attrs);
  append(el, kids);
  return el;
}
function setAttrs(el, attrs) {
  if (!attrs) return;
  for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
    else if (k === 'class') el.setAttribute('class', v);
    else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
    else if (k === 'value' && 'value' in el) el.value = v;
    else if (k === 'checked' || k === 'selected' || k === 'disabled') el[k] = !!v;
    else el.setAttribute(k, v === true ? '' : v);
  }
}
function append(el, kids) {
  for (const k of kids.flat(Infinity)) {
    if (k == null || k === false) continue;
    el.append(k instanceof Node ? k : document.createTextNode(String(k)));
  }
}
const hex2 = n => (n & 0xff).toString(16).toUpperCase().padStart(2, '0');
const hex4 = n => (n & 0xffff).toString(16).toUpperCase().padStart(4, '0');
const hex8 = n => (n >>> 0).toString(16).toUpperCase().padStart(8, '0');
const hexBytes = s => { const b = []; for (let i = 0; i + 1 < (s || '').length; i += 2) b.push(parseInt(s.substr(i, 2), 16)); return b; };
const spaced = s => (s || '').replace(/(..)(?=.)/g, '$1 ');
const fmtId = (id, ext) => ext ? hex8(id) : (id >>> 0).toString(16).toUpperCase().padStart(3, '0');
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
function parseNum(s) {
  s = String(s).trim();
  if (/^0x[0-9a-f]+$/i.test(s)) return parseInt(s, 16);
  if (/^-?\d+(\.\d+)?$/.test(s)) return Number(s);
  return NaN;
}
function fmtAge(ms) {
  if (ms == null || ms < 0) return '—';
  const s = Math.floor(ms / 1000);
  if (s < 2) return 'just now';
  if (s < 60) return s + 's ago';
  if (s < 3600) return Math.floor(s / 60) + 'm ago';
  if (s < 86400) return Math.floor(s / 3600) + 'h ago';
  return Math.floor(s / 86400) + 'd ago';
}
function fmtDur(ms) {
  const s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400), hh = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return d ? `${d}d ${hh}h` : hh ? `${hh}h ${m}m` : m ? `${m}m ${s % 60}s` : `${s}s`;
}
const fmtBytes = n => n > 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.round(n / 1024) + ' kB';
function kbit(bps) { return bps >= 1000000 ? (bps / 1000000) + 'M' : (bps / 1000) + 'k'; }
function debounce(fn, ms) { let t; return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); }; }
function store(k, v) { try { if (v === undefined) return JSON.parse(localStorage.getItem('bs.' + k)); localStorage.setItem('bs.' + k, JSON.stringify(v)); } catch (e) { return null; } }
function download(name, text, type = 'text/plain') {
  const a = h('a', { href: URL.createObjectURL(new Blob([text], { type })), download: name });
  document.body.append(a); a.click(); setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 500);
}

// ---------------------------------------------------------------- toasts & modals
function toast(msg, kind = '', ms = 3500) {
  const t = h('div', { class: 'toast ' + kind }, msg);
  $('#toasts').append(t);
  setTimeout(() => t.remove(), ms);
}
const fail = e => toast(e && e.message ? e.message : String(e), 'err', 6000);

function modal(title, body, buttons) {
  const dlg = $('#modal'), form = $('#modalForm');
  form.replaceChildren(
    h('div', { class: 'mh' }, title),
    h('div', { class: 'mb' }, body),
    h('div', { class: 'mf' }, buttons.map(b => h('button', { class: 'btn ' + (b.cls || ''), value: b.value, type: 'submit' }, b.label))));
  return new Promise(res => {
    // Resolve from submit/cancel directly; don't depend on the async 'close' event.
    let done = false;
    const finish = v => { if (!done) { done = true; res(v); } };
    form.onsubmit = e => {
      e.preventDefault();
      const v = (e.submitter && e.submitter.value) || 'ok';
      dlg.close(v);
      finish(v);
    };
    dlg.oncancel = () => finish('cancel');
    dlg.onclose = () => finish(dlg.returnValue || 'cancel');
    dlg.returnValue = '';
    dlg.showModal();
    const first = form.querySelector('input,select,textarea');
    if (first) first.focus();
  });
}
async function confirmBox(title, text, okLabel = 'OK', danger = false) {
  const r = await modal(title, typeof text === 'string' ? h('p', { style: { margin: 0 } }, text) : text,
    [{ label: 'Cancel', value: 'cancel' }, { label: okLabel, value: 'ok', cls: danger ? 'danger' : 'primary' }]);
  return r === 'ok';
}

// =====================================================================
// Protocol knowledge (decoding for display)
// =====================================================================
const MB_FN = { 1: 'Read Coils', 2: 'Read Discrete Inputs', 3: 'Read Holding Registers', 4: 'Read Input Registers', 5: 'Write Single Coil', 6: 'Write Single Register', 15: 'Write Multiple Coils', 16: 'Write Multiple Registers', 17: 'Report Server ID', 23: 'Read/Write Multiple Registers', 43: 'Encapsulated Interface' };
const MB_EXC = { 1: 'illegal function', 2: 'illegal data address', 3: 'illegal data value', 4: 'server device failure', 5: 'acknowledge', 6: 'server busy', 8: 'memory parity error', 10: 'gateway path unavailable', 11: 'gateway target failed to respond' };
const NMT_CS = { 1: 'start', 2: 'stop', 0x80: 'pre-operational', 0x81: 'reset node', 0x82: 'reset communication' };
const NMT_STATE = { 0: 'boot-up', 4: 'stopped', 5: 'operational', 127: 'pre-operational' };
const SDO_ABORT = {
  0x05030000: 'toggle bit not alternated', 0x05040000: 'SDO protocol timed out', 0x05040001: 'invalid command specifier',
  0x05040005: 'out of memory', 0x06010000: 'unsupported access', 0x06010001: 'write-only object', 0x06010002: 'read-only object',
  0x06020000: 'object does not exist', 0x06040041: 'cannot be mapped to PDO', 0x06040042: 'PDO length exceeded',
  0x06040043: 'general parameter incompatibility', 0x06040047: 'general internal incompatibility', 0x06060000: 'hardware error',
  0x06070010: 'data type length mismatch', 0x06070012: 'data too long', 0x06070013: 'data too short',
  0x06090011: 'sub-index does not exist', 0x06090030: 'value range exceeded', 0x06090031: 'value too high',
  0x06090032: 'value too low', 0x08000000: 'general error', 0x08000020: 'cannot transfer/store data',
  0x08000021: 'cannot store (local control)', 0x08000022: 'cannot store (device state)', 0x08000024: 'no data available',
};
const abortText = c => `abort 0x${hex8(c)}: ${SDO_ABORT[c] || 'unknown'}`;
const CO_PROFILES = { 401: 'CiA 401 generic I/O', 402: 'CiA 402 drive / motion', 404: 'CiA 404 measuring device', 406: 'CiA 406 encoder', 408: 'CiA 408 hydraulic', 410: 'CiA 410 inclinometer', 418: 'CiA 418 battery', 419: 'CiA 419 battery charger', 443: 'CiA 443 SIIS', 447: 'CiA 447 car add-on' };

const PGN_NAMES = {
  0: 'TSC1 Torque/Speed Control 1', 59392: 'Acknowledgment', 59904: 'Request', 60160: 'TP.DT Transport Data',
  60416: 'TP.CM Transport Connection', 60928: 'Address Claimed', 61440: 'ERC1 Retarder', 61441: 'EBC1 Brake Controller 1',
  61442: 'ETC1 Transmission 1', 61443: 'EEC2 Engine Controller 2', 61444: 'EEC1 Engine Controller 1', 61445: 'ETC2 Transmission 2',
  64965: 'ECUID ECU Identification', 65132: 'TCO1 Tachograph', 65134: 'HRW Wheel Speed', 65213: 'FD Fan Drive',
  65214: 'EEC4 Engine Controller 4', 65215: 'EBC2 Wheel Speed', 65217: 'VDHR Vehicle Distance (hi-res)',
  65226: 'DM1 Active DTCs', 65227: 'DM2 Previously Active DTCs', 65228: 'DM3 Clear DTCs', 65242: 'SOFT Software ID',
  65247: 'EEC3 Engine Controller 3', 65248: 'VD Vehicle Distance', 65253: 'HOURS Engine Hours', 65257: 'LFC Fuel Consumption',
  65259: 'CI Component ID', 65260: 'VI Vehicle ID', 65262: 'ET1 Engine Temperature 1', 65263: 'EFL/P1 Engine Fluids 1',
  65265: 'CCVS1 Cruise Control / Vehicle Speed', 65266: 'LFE1 Fuel Economy', 65269: 'AMB Ambient Conditions',
  65270: 'IC1 Inlet/Exhaust Conditions', 65271: 'VEP1 Vehicle Electrical Power', 65272: 'TRF1 Transmission Fluids',
  65276: 'DD Dash Display',
};
const J1939_FUNCTIONS = ['Engine', 'Auxiliary Power Unit', 'Electric Propulsion Control', 'Transmission', 'Battery Pack Monitor',
  'Shift Control/Console', 'Power TakeOff (Main)', 'Axle - Steering', 'Axle - Drive', 'Brakes - System Controller',
  'Brakes - Steer Axle', 'Brakes - Drive Axle', 'Retarder - Engine', 'Retarder - Driveline', 'Cruise Control', 'Fuel System',
  'Steering Controller', 'Suspension - Steer Axle', 'Suspension - Drive Axle', 'Instrument Cluster', 'Trip Recorder',
  'Cab Climate Control', 'Aerodynamic Control', 'Vehicle Navigation', 'Vehicle Security', 'Network Interconnect ECU',
  'Body Controller', 'Power TakeOff (Secondary)', 'Off Vehicle Gateway', 'Virtual Terminal (in cab)',
  'Management Computer', 'Propulsion Battery Charger', 'Headway Controller', 'System Monitor', 'Hydraulic Pump Controller',
  'Suspension - System Controller', 'Pneumatic - System Controller', 'Cab Controller', 'Tire Pressure Control',
  'Ignition Control Module', 'Seat Control'];
const INDUSTRY = ['Global', 'On-Highway', 'Agricultural & Forestry', 'Construction', 'Marine', 'Industrial/Process', 'Reserved', 'Reserved'];

function pgnOf(id) {
  const pf = (id >>> 16) & 0xff, ps = (id >>> 8) & 0xff, dp = (id >>> 24) & 3;
  return (dp << 16) | (pf << 8) | (pf >= 240 ? ps : 0);
}
function decodeJ1939Name(hex) {
  if (!hex) return null;
  const n = BigInt('0x' + hex), f = (sh, m) => Number((n >> BigInt(sh)) & BigInt(m));
  const fn = f(40, 0xff), ig = f(60, 7);
  return {
    'Identity number': f(0, 0x1fffff), 'Manufacturer code': f(21, 0x7ff), 'ECU instance': f(32, 7),
    'Function instance': f(35, 0x1f), 'Function': fn + (fn < J1939_FUNCTIONS.length ? ` (${J1939_FUNCTIONS[fn]})` : ''),
    'Vehicle system': f(49, 0x7f), 'Vehicle system instance': f(56, 0xf), 'Industry group': `${ig} (${INDUSTRY[ig]})`,
    'Arbitrary address capable': f(63, 1) ? 'yes' : 'no', _fn: fn,
  };
}
const NA = v => v === 0xff || v === 0xffff;
function decodePgnData(pgn, d) {
  const u16 = i => d[i] | (d[i + 1] << 8);
  try {
    switch (pgn) {
      case 61444: return NA(u16(3)) ? '' : `engine ${(u16(3) * 0.125).toFixed(0)} rpm, torque ${d[2] - 125}%`;
      case 65262: return `coolant ${NA(d[0]) ? 'n/a' : d[0] - 40 + ' °C'}, fuel ${NA(d[1]) ? 'n/a' : d[1] - 40 + ' °C'}`;
      case 65265: return NA(u16(1)) ? '' : `speed ${(u16(1) / 256).toFixed(1)} km/h`;
      case 65263: return NA(d[3]) ? '' : `oil pressure ${d[3] * 4} kPa`;
      case 65271: return NA(u16(6)) ? '' : `battery ${(u16(6) * 0.05).toFixed(2)} V`;
      case 65269: return NA(u16(3)) ? '' : `ambient ${(u16(3) * 0.03125 - 273).toFixed(1)} °C`;
      case 65253: { const v = d[0] | d[1] << 8 | d[2] << 16 | (d[3] << 24) >>> 0; return v === 0xffffffff ? '' : `${(v * 0.05).toFixed(1)} h`; }
      case 65266: return NA(u16(0)) ? '' : `fuel rate ${(u16(0) * 0.05).toFixed(2)} L/h`;
      case 65226: case 65227: {
        if (d.length < 6) return '';
        const spn = d[2] | d[3] << 8 | ((d[4] >> 5) << 16), fmi = d[4] & 0x1f;
        return spn ? `SPN ${spn} FMI ${fmi} (x${d[5] & 0x7f})` : 'no active DTCs';
      }
      case 59904: return `request PGN ${d[0] | d[1] << 8 | d[2] << 16}`;
      case 60928: return 'address claim';
    }
  } catch (e) { /* ignore */ }
  return '';
}

function decodeCan(id, ext, d, rtr) {
  if (ext) {
    const pgn = pgnOf(id), sa = id & 0xff, pf = (id >>> 16) & 0xff;
    const da = pf < 240 ? ((id >>> 8) & 0xff) : null;
    let s = `PGN ${pgn} ${PGN_NAMES[pgn] ? '(' + PGN_NAMES[pgn] + ') ' : ''}SA ${sa}${da != null && da !== 255 ? ' → DA ' + da : ''}`;
    const x = decodePgnData(pgn, d);
    return x ? s + ' · ' + x : s;
  }
  if (rtr) return 'remote request';
  if (id === 0) return d.length >= 2 ? `NMT ${NMT_CS[d[0]] || '0x' + hex2(d[0])} → ${d[1] ? 'node ' + d[1] : 'all nodes'}` : 'NMT';
  if (id === 0x80) return 'SYNC';
  if (id === 0x100) return 'TIME';
  const fc = id >> 7, node = id & 0x7f;
  if (!node) return '';
  switch (fc) {
    case 1: return `EMCY node ${node}` + (d.length >= 3 ? `: code 0x${hex4(d[0] | d[1] << 8)}, err reg 0x${hex2(d[2])}` : '');
    case 3: case 5: case 7: case 9: return `TPDO${(fc - 1) / 2} node ${node}`;
    case 4: case 6: case 8: case 10: return `RPDO${(fc - 2) / 2} node ${node}`;
    case 11: case 12: return sdoText(fc === 11, node, d);
    case 14: return d.length === 1 ? (d[0] === 0 ? `boot-up node ${node}` : `heartbeat node ${node}: ${NMT_STATE[d[0] & 0x7f] || d[0]}`) : '';
  }
  if (id === 0x7e4 || id === 0x7e5) return 'LSS';
  return '';
}
function sdoText(resp, node, d) {
  if (d.length < 4) return `SDO node ${node}`;
  const cs = d[0], idx = `${hex4(d[1] | d[2] << 8)}:${d[3]}`, top = cs >> 5;
  const val = () => { const n = (cs & 1) ? 4 - ((cs >> 2) & 3) : 4; let v = 0; for (let i = 0; i < n; i++) v += d[4 + i] * 2 ** (8 * i); return `0x${v.toString(16).toUpperCase()} (${v})`; };
  if (cs === 0x80) return `SDO ${resp ? '←' : '→'} node ${node} ${idx} ${abortText((d[4] | d[5] << 8 | d[6] << 16 | d[7] << 24) >>> 0)}`;
  if (!resp) {
    if (top === 2) return `SDO read ${idx} → node ${node}`;
    if (top === 1) return `SDO write ${idx} = ${(cs & 2) ? val() : 'segmented'} → node ${node}`;
    return `SDO segment → node ${node}`;
  }
  if (top === 2) return `SDO ← node ${node} ${idx} = ${(cs & 2) ? val() : 'segmented'}`;
  if (top === 3) return `SDO ← node ${node} ${idx} write OK`;
  return `SDO segment ← node ${node}`;
}
function decodeModbus(b, crcOk) {
  if (b.length < 4) return b.length ? 'short frame' : '';
  if (!crcOk) return 'no valid Modbus CRC (noise, wrong baud/parity, or not Modbus)';
  const a = b[0], fn = b[1], n = b.length, w = i => (b[i] << 8) | b[i + 1];
  const who = a === 0 ? 'broadcast' : 'addr ' + a;
  if (fn & 0x80) return `${who} EXCEPTION to ${MB_FN[fn & 0x7f] || 'fn ' + (fn & 0x7f)}: ${MB_EXC[b[2]] || b[2]}`;
  const name = MB_FN[fn] || 'fn ' + fn;
  if (fn >= 1 && fn <= 4) {
    if (n === 8) return `${who} ${name}: start ${w(2)}, qty ${w(4)}`;
    const bc = b[2];
    if (n === 5 + bc) {
      if (fn >= 3) {
        const regs = []; for (let i = 0; i < bc / 2; i++) regs.push(w(3 + 2 * i));
        return `${who} ← ${regs.length} reg${regs.length === 1 ? '' : 's'}: ${regs.slice(0, 10).join(', ')}${regs.length > 10 ? ', …' : ''}`;
      }
      return `${who} ← ${bc} byte(s) of ${fn === 1 ? 'coils' : 'inputs'}`;
    }
  }
  if (fn === 5) return `${who} ${name}: ${w(2)} = ${b[4] ? 'ON' : 'OFF'}`;
  if (fn === 6) return `${who} ${name}: ${w(2)} = ${w(4)}`;
  if (fn === 15 || fn === 16) return n === 8 ? `${who} ${name} OK: start ${w(2)}, qty ${w(4)}` : `${who} ${name}: start ${w(2)}, qty ${w(4)}`;
  return `${who} ${name}`;
}

// Value formats for watch items and register views
const MB_FMTS = { u16: 'Unsigned 16', s16: 'Signed 16', u32: 'Unsigned 32 (hi word first)', u32sw: 'Unsigned 32 (lo word first)', s32: 'Signed 32 (hi first)', s32sw: 'Signed 32 (lo first)', f32: 'Float 32 (hi first)', f32sw: 'Float 32 (lo first)', hex: 'Hex', str: 'ASCII', bool: 'Bit / boolean' };
const CO_FMTS = { u8: 'UNSIGNED8', u16: 'UNSIGNED16', u32: 'UNSIGNED32', i8: 'INTEGER8', i16: 'INTEGER16', i32: 'INTEGER32', f32: 'REAL32', str: 'VISIBLE_STRING', hex: 'Hex' };
const FMT_WORDS = { u16: 1, s16: 1, u32: 2, u32sw: 2, s32: 2, s32sw: 2, f32: 2, f32sw: 2, hex: 1, str: 4, bool: 1 };

function decodeValue(fmt, bytes, proto, fn) {
  if (!bytes || !bytes.length) return null;
  const dv = new DataView(new Uint8Array(bytes.concat([0, 0, 0, 0])).buffer);
  if (proto === 'modbus') {
    if (fn === 1 || fn === 2) return fmt === 'hex' ? bytes.map(hex2).join(' ') : (bytes[0] & 1);
    const sw = () => new DataView(new Uint8Array([bytes[2], bytes[3], bytes[0], bytes[1]]).buffer);
    switch (fmt) {
      case 'u16': return dv.getUint16(0);
      case 's16': return dv.getInt16(0);
      case 'u32': return dv.getUint32(0);
      case 's32': return dv.getInt32(0);
      case 'u32sw': return bytes.length >= 4 ? sw().getUint32(0) : null;
      case 's32sw': return bytes.length >= 4 ? sw().getInt32(0) : null;
      case 'f32': return dv.getFloat32(0);
      case 'f32sw': return bytes.length >= 4 ? sw().getFloat32(0) : null;
      case 'bool': return dv.getUint16(0) ? 1 : 0;
      case 'str': return String.fromCharCode(...bytes.filter(c => c >= 32 && c < 127));
      default: return bytes.map(hex2).join(' ');
    }
  }
  switch (fmt) {
    case 'u8': return dv.getUint8(0);
    case 'i8': return dv.getInt8(0);
    case 'u16': return dv.getUint16(0, true);
    case 'i16': return dv.getInt16(0, true);
    case 'u32': return dv.getUint32(0, true);
    case 'i32': return dv.getInt32(0, true);
    case 'f32': return dv.getFloat32(0, true);
    case 'str': return String.fromCharCode(...bytes.filter(c => c >= 32 && c < 127));
    default: return bytes.map(hex2).join(' ');
  }
}
function fmtValue(v, scale, unit) {
  if (v == null) return '—';
  if (typeof v === 'number') {
    let x = v * (scale || 1);
    const s = Number.isInteger(x) ? String(x) : (Math.abs(x) >= 100 ? x.toFixed(1) : x.toFixed(3).replace(/0+$/, '').replace(/\.$/, ''));
    return unit ? `${s} ${unit}` : s;
  }
  return String(v);
}

// =====================================================================
// State
// =====================================================================
const S = {
  connected: false, hello: null, status: null,
  devices: new Map(), serverNow: 0, serverNowAt: 0,
  rates: { rs485: 0, can: 0, rs485tx: 0, cantx: 0 }, lastCounters: null,
  view: 'map', selected: null, drawerTab: null,
  scan: { rs485: null, can: null }, scanned: { rs485: new Set(), canopen: new Set() },
  trace: [], traceMax: 5000, tracePaused: false, traceFollow: true, traceDrop: 0,
  traceFilter: { bus: 'all', text: '' },
  ids: [], idsPrev: new Map(), idsSort: store('idsSort') || { key: 'id', asc: true }, idsFilter: '',
  gridCan: store('gridCan') || 'canopen',
  watchHist: new Map(), lastRx: new Map(), pulse: new Set(),
};
const serverNow = () => S.serverNow + (performance.now() - S.serverNowAt);
const busCfg = bus => (S.status && S.status[bus]) || (S.hello && S.hello.settings[bus]) || {};
const canActive = () => busCfg('can').mode === 'normal';
const bootEpoch = () => (S.status && S.status.epoch) ? S.status.epoch - S.status.up : 0;

// =====================================================================
// Connection / RPC
// =====================================================================
const RPC = { ws: null, seq: 1, pending: new Map(), backoff: 500 };

function connect() {
  const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
  let ws;
  try { ws = new WebSocket(url); } catch (e) { setTimeout(connect, 2000); return; }
  RPC.ws = ws;
  ws.onopen = () => { S.connected = true; RPC.backoff = 500; renderConn(); };
  ws.onclose = () => {
    const was = S.connected;
    S.connected = false; renderConn();
    for (const p of RPC.pending.values()) p.reject(new Error('connection lost'));
    RPC.pending.clear();
    setTimeout(connect, was ? 300 : RPC.backoff);
    RPC.backoff = Math.min(RPC.backoff * 2, 5000);
  };
  ws.onmessage = e => { let m; try { m = JSON.parse(e.data); } catch (x) { return; } onMessage(m); };
}

function call(cmd, args = {}, timeoutMs = 30000) {
  return new Promise((resolve, reject) => {
    if (!S.connected) return reject(new Error('Not connected to the board'));
    const id = RPC.seq++;
    const t = setTimeout(() => { RPC.pending.delete(id); reject(new Error(`${cmd}: no reply`)); }, timeoutMs);
    RPC.pending.set(id, { resolve: r => { clearTimeout(t); resolve(r); }, reject: e => { clearTimeout(t); reject(e); } });
    RPC.ws.send(JSON.stringify(Object.assign({ cmd, id }, args)));
  });
}

function onMessage(m) {
  if (m.ev === undefined && m.id !== undefined && m.ok !== undefined) {
    const p = RPC.pending.get(m.id);
    if (!p) return;
    RPC.pending.delete(m.id);
    m.ok ? p.resolve(m.result || {}) : p.reject(new Error(m.error || 'error'));
    return;
  }
  switch (m.ev) {
    case 'hello': onHello(m.d); break;
    case 'status': onStatus(m.s); break;
    case 'dev': onDevices(m.d, m.now); break;
    case 'devdel': for (const k of m.keys) { S.devices.delete(k); if (S.selected === k) closeDrawer(); } scheduleRender(); break;
    case 'devclear': for (const [k, d] of S.devices) if (!m.bus || d.bus === m.bus) S.devices.delete(k); scheduleRender(); break;
    case 'devreload': loadDevices(); break;
    case 'tr': onTrace(m); break;
    case 'ids': onIds(m.ids); break;
    case 'scan': onScan(m); break;
    case 'autobaud': toast(`Listening at ${kbit(m.bitrate)}bit/s…`, '', 900); break;
    case 'cli': Console.receive(m); break;
  }
}

async function onHello(d) {
  S.hello = d;
  onStatus(d.status);
  renderSettings();
  try { await call('time.set', { epoch: Date.now() }); } catch (e) { /* not critical */ }
  await loadDevices();
  subscribe();
}
async function loadDevices() {
  try {
    const r = await call('dev.list');
    S.devices.clear();
    S.serverNow = r.now; S.serverNowAt = performance.now();
    for (const d of r.devices) S.devices.set(d.key, d);
    scheduleRender();
    if (S.selected && S.devices.has(S.selected)) refreshSelected();
  } catch (e) { fail(e); }
}
function subscribe() {
  if (!S.connected) return;
  call('sub', { topics: { trace: true, ids: S.view === 'ids' || S.view === 'map' } }).catch(() => {});
}

function onStatus(s) {
  const now = performance.now();
  if (S.lastCounters) {
    const dt = (now - S.lastCounters.t) / 1000;
    if (dt > 0.2) {
      S.rates.rs485 = Math.max(0, (s.rs485.rx - S.lastCounters.rr) / dt);
      S.rates.rs485tx = Math.max(0, (s.rs485.tx - S.lastCounters.rt) / dt);
      S.rates.can = Math.max(0, (s.can.rx - S.lastCounters.cr) / dt);
      S.rates.cantx = Math.max(0, (s.can.tx - S.lastCounters.ct) / dt);
      blink('rs485', s.rs485.rx + s.rs485.tx > S.lastCounters.rr + S.lastCounters.rt, s.rs485.err > S.lastCounters.re);
      blink('can', s.can.rx + s.can.tx > S.lastCounters.cr + S.lastCounters.ct, s.can.busErrors > S.lastCounters.ce);
    }
  }
  S.lastCounters = { t: now, rr: s.rs485.rx, rt: s.rs485.tx, re: s.rs485.err, cr: s.can.rx, ct: s.can.tx, ce: s.can.busErrors };
  const modeChanged = S.status && S.status.can.mode !== s.can.mode;
  S.status = s;
  if (modeChanged && S.selected) renderDrawer();
  S.serverNow = s.up; S.serverNowAt = now;
  renderHeader(); renderStatusbar(); renderBusHeads();
  if (S.view === 'grid') renderGrid();
  if (S.view === 'map') scheduleRender();
  if (S.selected) updateDrawerLive();
}
function blink(bus, rx, err) {
  const led = $('#led-' + bus);
  if (!led) return;
  led.className = 'led' + (err ? ' err' : rx ? ' rx' : '');
  if (rx || err) setTimeout(() => { led.className = 'led'; }, 350);
}

function onDevices(list, now) {
  if (now) { S.serverNow = now; S.serverNowAt = performance.now(); }
  for (const d of list) {
    const prev = S.devices.get(d.key);
    if (prev && d.rx > prev.rx) S.pulse.add(d.key);
    if (d.watch) recordWatchHistory(d);
    S.devices.set(d.key, Object.assign(prev || {}, d));
  }
  scheduleRender();
  if (S.selected && list.some(d => d.key === S.selected)) updateDrawerLive();
}
function recordWatchHistory(d) {
  d.watch.forEach((w, i) => {
    if (!w.ts || w.err) return;
    const k = d.key + '|' + i;
    const hist = S.watchHist.get(k) || [];
    if (hist.length && hist[hist.length - 1].ts === w.ts) return;
    const v = decodeValue(w.fmt, hexBytes(w.raw), d.proto, w.fn);
    if (typeof v !== 'number') return;
    hist.push({ ts: w.ts, v: v * (w.scale || 1) });
    if (hist.length > 120) hist.shift();
    S.watchHist.set(k, hist);
  });
}

// =====================================================================
// Device helpers
// =====================================================================
function devStatus(d) {
  if (d.consecErr >= 3) return 'err';
  if (!d.present) return 'offline';
  const age = serverNow() - d.lastSeen;
  let stale = 60000;
  if (d.pollMs) stale = Math.max(d.pollMs * 4, 5000);
  else if (d.bus === 'can') stale = 15000;
  return age > stale ? 'stale' : 'ok';
}
const STATUS_TEXT = { ok: 'online', stale: 'quiet', err: 'not responding', offline: 'not seen yet' };
function devTag(d) { return d.proto === 'modbus' ? 'MB ' + d.addr : d.proto === 'canopen' ? 'CO ' + d.addr : 'J ' + d.addr; }
function devName(d) {
  if (d.label) return d.label;
  const ident = [d.vendor, d.product].filter(Boolean).join(' ') || d.name;
  if (ident) return ident;
  if (d.proto === 'j1939' && d.j1939Name) { const n = decodeJ1939Name(d.j1939Name); if (n && n._fn < J1939_FUNCTIONS.length) return J1939_FUNCTIONS[n._fn]; }
  return d.proto === 'modbus' ? 'Modbus device' : d.proto === 'canopen' ? 'CANopen node' : 'J1939 ECU';
}
function devSub(d) {
  const st = devStatus(d);
  if (d.proto === 'canopen' && d.state != null && st !== 'err') return NMT_STATE[d.state] || 'state ' + d.state;
  if (st === 'ok') return d.passive ? 'online · passive' : 'online · ' + fmtAge(serverNow() - d.lastSeen);
  if (st === 'stale') return 'last seen ' + fmtAge(serverNow() - d.lastSeen);
  return STATUS_TEXT[st];
}
const devsOn = bus => [...S.devices.values()].filter(d => d.bus === bus).sort((a, b) => (a.proto > b.proto) - (a.proto < b.proto) || a.addr - b.addr);

// =====================================================================
// Header, status bar, theme
// =====================================================================
function renderConn() {
  const c = $('#conn');
  c.className = 'conn ' + (S.connected ? 'ok' : 'bad');
  $('#connText').textContent = S.connected ? (location.host || 'connected') : 'Disconnected';
  $('#offline').hidden = S.connected;
}
function renderHeader() {
  const r = busCfg('rs485'), c = busCfg('can');
  $('#pill-rs485 input').checked = !!r.enabled;
  $('#pill-can input').checked = !!c.enabled;
  $('#pill-rs485-sub').textContent = r.enabled ? `${r.baud} 8${r.parity}${r.stop}${r.busy ? ' · ' + r.busy : ''}` : 'off';
  let cs = 'off';
  if (c.enabled) {
    cs = `${kbit(c.bitrate)} · ${c.mode === 'normal' ? 'active' : 'listen-only'}`;
    if (c.state && c.state !== 'running') cs += ' · ' + c.state;
    if (c.busy) cs += ' · ' + c.busy;
  }
  $('#pill-can-sub').textContent = cs;
}
function renderStatusbar() {
  const s = S.status; if (!s) return;
  const sb = $('#statusbar');
  const item = (label, val, cls) => h('span', { class: cls || '' }, label + ' ', h('b', null, val));
  const w = s.wifi || {};
  sb.replaceChildren();
  append(sb, [
    item('RS485', `${Math.round(S.rates.rs485)}/s rx · ${Math.round(S.rates.rs485tx)}/s tx`),
    s.rs485.err ? item('RS485 errors', s.rs485.err, 'err') : null,
    item('CAN', `${Math.round(S.rates.can)}/s rx · ${Math.round(S.rates.cantx)}/s tx`),
    s.can.up ? item('CAN state', `${s.can.state} (TEC ${s.can.tec} / REC ${s.can.rec})`, s.can.state === 'running' ? '' : 'err') : null,
    item('Devices', s.devices),
    h('span', { class: 'hide-sm' }, 'Wi-Fi ', h('b', null, w.sta && w.sta.connected ? `${w.sta.ip} (${w.sta.rssi} dBm)` : `AP ${w.ap ? w.ap.clients : 0} client(s)`)),
    h('span', { class: 'hide-sm' }, 'Heap ', h('b', null, fmtBytes(s.heap))),
    h('span', { class: 'hide-sm' }, 'Up ', h('b', null, fmtDur(s.up))),
  ]);
}
const THEMES = ['auto', 'light', 'dark'];
function applyTheme(t) {
  if (t === 'light' || t === 'dark') document.documentElement.dataset.theme = t;
  else delete document.documentElement.dataset.theme;
  store('theme', t);
  try { localStorage.setItem('bs.theme', t); } catch (e) { /* ignore */ }
  $('#themeBtn').title = 'Theme: ' + t + ' (click to change)';
  const sel = $('#themeSelect'); if (sel) sel.value = t;
}
function currentTheme() { const t = document.documentElement.dataset.theme; return t || 'auto'; }

// =====================================================================
// Views / tabs
// =====================================================================
function setView(v) {
  S.view = v;
  $$('.tabs button').forEach(b => b.classList.toggle('active', b.dataset.view === v));
  $$('.view').forEach(s => s.classList.toggle('active', s.id === 'view-' + v));
  store('view', v);
  subscribe();
  if (v === 'map') renderMap();
  if (v === 'grid') renderGrid();
  if (v === 'traffic') renderTraffic(true);
  if (v === 'ids') { renderIds(); call('can.ids').then(r => onIds(r.ids)).catch(() => {}); }
  if (v === 'console') Console.focus();
  if (v === 'settings') renderSettings();
}
let renderQueued = false;
function scheduleRender() {
  if (renderQueued) return;
  renderQueued = true;
  requestAnimationFrame(() => {
    renderQueued = false;
    if (S.view === 'map') renderMap();
    if (S.view === 'grid') renderGrid();
  });
}

// =====================================================================
// Map view (topology)
// =====================================================================
const BUS_INFO = {
  rs485: { title: 'RS485', protos: 'Modbus RTU' },
  can: { title: 'CAN', protos: 'CANopen · J1939 · raw' },
};

function buildMapSkeleton() {
  const v = $('#view-map');
  if (v.dataset.built) return;
  v.dataset.built = '1';
  for (const bus of ['rs485', 'can']) {
    v.append(h('div', { class: 'card bus-card', 'data-bus': bus, id: 'buscard-' + bus },
      h('div', { class: 'card-head' },
        h('label', { class: 'switch', title: `Enable / disable ${BUS_INFO[bus].title}` },
          h('input', { type: 'checkbox', 'data-act': 'toggle-bus', 'data-bus': bus }), h('span')),
        h('h2', null, BUS_INFO[bus].title, h('span', { class: 'muted', style: { fontWeight: 400 } }, ' · ' + BUS_INFO[bus].protos)),
        h('span', { class: 'meta', id: 'busmeta-' + bus }),
        h('span', { class: 'spacer' }),
        h('button', { class: 'btn small', onclick: () => busConfigDialog(bus) }, 'Configure'),
        h('button', { class: 'btn small primary', onclick: () => scanDialog(bus) }, 'Scan…')),
      h('div', { class: 'scanbar', id: 'scanbar-' + bus, hidden: true }),
      h('div', { class: 'topo-wrap' }, sv('svg', { class: 'topo', id: 'topo-' + bus, role: 'img', 'aria-label': `${bus} topology` }))));
  }
  // Re-layout whenever the available width changes (window resize, drawer, tab shown).
  if (window.ResizeObserver) {
    let lastW = 0;
    new ResizeObserver(entries => {
      const w = Math.round(entries[0].contentRect.width);
      if (w && Math.abs(w - lastW) > 4) { lastW = w; if (S.view === 'map') renderMap(); }
    }).observe(v);
  }
  v.append(h('div', { class: 'legend' },
    h('span', null, h('i', { style: { background: 'var(--ok)' } }), 'online'),
    h('span', null, h('i', { style: { background: 'var(--warn)' } }), 'quiet (no recent traffic)'),
    h('span', null, h('i', { style: { background: 'var(--err)' } }), 'not responding'),
    h('span', null, h('i', { style: { background: 'var(--idle)' } }), 'not seen since boot'),
    h('span', { class: 'faint' }, 'Select a device to configure. Termination: 120 Ω jumpers H1 (CAN), H2 (RS485).')));
}

function renderBusHeads() {
  for (const bus of ['rs485', 'can']) {
    const meta = $('#busmeta-' + bus); if (!meta) continue;
    const c = busCfg(bus), n = devsOn(bus).length;
    $(`#buscard-${bus} .card-head input`).checked = !!c.enabled;
    $('#buscard-' + bus).classList.toggle('off', !c.enabled);
    const link = bus === 'rs485' ? `${c.baud} 8${c.parity}${c.stop}` : `${kbit(c.bitrate || 0)}bit/s · ${c.mode === 'normal' ? 'active' : 'listen-only'}`;
    const rate = Math.round(bus === 'rs485' ? S.rates.rs485 : S.rates.can);
    meta.textContent = c.enabled ? `${link} · ${n} device${n === 1 ? '' : 's'} · ${rate} frames/s` : 'disabled';
  }
}

function renderMap() {
  buildMapSkeleton();
  renderBusHeads();
  for (const bus of ['rs485', 'can']) renderTopology(bus);
  S.pulse.clear();
}

function truncate(s, n) { s = String(s); return s.length > n ? s.slice(0, n - 1) + '…' : s; }

function renderTopology(bus) {
  const svg = $('#topo-' + bus);
  const wrap = svg.parentElement;
  if (!wrap.clientWidth) return;  // not laid out yet; ResizeObserver will call us again
  const W = Math.max(640, wrap.clientWidth - 16);
  svg.style.width = W + 'px';
  const c = busCfg(bus);
  const devs = devsOn(bus);
  const items = devs.map(d => ({ d }));
  if (bus === 'can' && S.ids.length) items.push({ ids: S.ids.length });
  items.push({ ghost: true });

  const gwW = 150, gwH = 76, cardW = 172, cardH = 58, colW = 188, drop = 24, rowH = 2 * cardH + 2 * drop + 30;
  const x0 = 20 + gwW + 44, xr = W - 30, xl = x0 - 22;
  const cols = Math.max(1, Math.floor((xr - x0 + 16) / colW));
  const perRow = cols * 2;
  const rows = Math.max(1, Math.ceil(items.length / perRow));
  const H = rows * rowH + 16;
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  svg.setAttribute('height', H);
  const trunkY = r => 10 + r * rowH + cardH + drop + 14;

  const kids = [];
  // trunk (serpentine)
  let d = `M ${20 + gwW} ${trunkY(0)} H ${xr}`;
  for (let r = 1; r < rows; r++) {
    const side = r % 2 ? xr : xl;
    d += ` V ${trunkY(r)} H ${r % 2 ? xl : xr}`;
    void side;
  }
  kids.push(sv('path', { class: 'trunk', d }));
  const endX = rows % 2 ? xr : xl, endY = trunkY(rows - 1);
  // terminators
  const term = (x, y, label, anchor) => [
    sv('rect', { class: 'term', x: x - 7, y: y - 13, width: 14, height: 26, rx: 2 }),
    sv('text', { class: 'term-label', x: x + (anchor === 'end' ? -12 : 12), y: y + 4, 'text-anchor': anchor }, label)];
  kids.push(...term(endX, endY, '120 Ω', rows % 2 ? 'end' : 'start'));
  // gateway
  const gy = trunkY(0) - gwH / 2;
  const sub1 = bus === 'rs485' ? (c.enabled ? `${c.baud} 8${c.parity}${c.stop}` : 'disabled') : (c.enabled ? `${kbit(c.bitrate || 0)}bit/s` : 'disabled');
  const sub2 = bus === 'rs485' ? 'Modbus master / sniffer' : (c.enabled ? (c.mode === 'normal' ? 'active (ACK + TX)' : 'listen-only') : '');
  kids.push(sv('g', { class: 'gw', transform: `translate(20 ${gy})` },
    sv('rect', { width: gwW, height: gwH, rx: 10 }),
    sv('text', { x: 12, y: 22 }, 'WonderScope'),
    sv('text', { x: 12, y: 40, class: 'sub' }, sub1),
    sv('text', { x: 12, y: 56, class: 'sub' }, sub2),
    sv('text', { x: 12, y: 70, class: 'sub', style: 'font-size:9.5px' }, `jumper ${bus === 'rs485' ? 'H2' : 'H1'}: 120 Ω`)));

  items.forEach((it, i) => {
    const r = Math.floor(i / perRow), k = i % perRow, col = Math.floor(k / 2), above = k % 2 === 0;
    const x = x0 + col * colW;
    const ty = trunkY(r);
    const y = above ? ty - drop - cardH : ty + drop;
    const cx = x + cardW / 2;
    kids.push(sv('line', { class: 'drop', x1: cx, y1: above ? y + cardH : y, x2: cx, y2: ty }));
    kids.push(sv('circle', { cx, cy: ty, r: 3.5, class: 'trunk', style: 'fill: var(--surface); stroke-width: 2' }));
    if (it.ghost) {
      kids.push(sv('g', { class: 'ghost', transform: `translate(${x} ${y})`, tabindex: 0, role: 'button', onclick: () => scanDialog(bus) },
        sv('rect', { width: cardW, height: cardH, rx: 8 }),
        sv('text', { x: cardW / 2, y: cardH / 2 - 2, 'text-anchor': 'middle' }, devs.length ? '+ Scan' : 'No devices'),
        sv('text', { x: cardW / 2, y: cardH / 2 + 14, 'text-anchor': 'middle', style: 'font-size:11px' }, devs.length ? '' : 'Scan or await traffic')));
      return;
    }
    if (it.ids) {
      kids.push(sv('g', { class: 'ghost', transform: `translate(${x} ${y})`, tabindex: 0, role: 'button', onclick: () => setView('ids') },
        sv('rect', { width: cardW, height: cardH, rx: 8 }),
        sv('text', { x: cardW / 2, y: cardH / 2 - 2, 'text-anchor': 'middle', style: 'fill: var(--text)' }, `${it.ids} CAN ID${it.ids === 1 ? '' : 's'} seen`),
        sv('text', { x: cardW / 2, y: cardH / 2 + 14, 'text-anchor': 'middle', style: 'font-size:11px' }, 'View ID map')));
      return;
    }
    const dv = it.d, st = devStatus(dv), tag = devTag(dv);
    const tagW = tag.length * 7 + 10;
    const cls = `node ${st}${S.selected === dv.key ? ' selected' : ''}`;
    kids.push(sv('g', {
      class: cls, transform: `translate(${x} ${y})`, tabindex: 0, role: 'button', 'aria-label': `${devName(dv)} ${STATUS_TEXT[st]}`,
      onclick: () => openDevice(dv.key), onkeydown: e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); openDevice(dv.key); } },
    },
      sv('title', null, `${dv.key}\n${devName(dv)}\n${STATUS_TEXT[st]}`),
      sv('rect', { class: 'box', width: cardW, height: cardH, rx: 8 }),
      sv('rect', { class: 'strip', x: 0, y: 6, width: 4, height: cardH - 12, rx: 2 }),
      sv('rect', { class: 'tagbg', x: 12, y: 8, width: tagW, height: 17, rx: 4 }),
      sv('text', { class: 'tag', x: 17, y: 20.5 }, tag),
      sv('text', { class: 't1', x: 12, y: 40 }, truncate(devName(dv), 21)),
      sv('text', { class: 't2', x: 12, y: 53 }, truncate(devSub(dv), 26)),
      sv('circle', { class: 'act' + (S.pulse.has(dv.key) ? ' on' : ''), cx: cardW - 10, cy: cardH - 10, r: 3.5 }),
      dv.emcy && serverNow() - dv.emcy.ts < 60000 ? sv('text', { x: cardW - 10, y: 20, 'text-anchor': 'end', style: 'fill: var(--err); font-weight:700; font-size: 11px' }, 'EMCY') : null));
  });
  svg.replaceChildren(...kids);
}

// =====================================================================
// Address grid view
// =====================================================================
function renderGrid() {
  const v = $('#view-grid');
  if (!v.dataset.built) {
    v.dataset.built = '1';
    v.append(
      h('div', { class: 'card', 'data-bus': 'rs485' },
        h('div', { class: 'card-head' }, h('span', { class: 'badge rs485' }, 'RS485'), h('h3', null, 'Modbus addresses 1–247'),
          h('span', { class: 'spacer' }), h('span', { class: 'muted', id: 'gridscan-rs485' }),
          h('button', { class: 'btn small primary', onclick: () => scanDialog('rs485') }, 'Scan…')),
        h('div', { class: 'card-body' }, h('div', { class: 'addr-grid', id: 'grid-rs485' }))),
      h('div', { class: 'card', 'data-bus': 'can' },
        h('div', { class: 'card-head' }, h('span', { class: 'badge can' }, 'CAN'),
          h('div', { class: 'seg', id: 'gridSeg' },
            h('button', { 'data-p': 'canopen' }, 'CANopen nodes 1–127'),
            h('button', { 'data-p': 'j1939' }, 'J1939 addresses 0–253')),
          h('span', { class: 'spacer' }), h('span', { class: 'muted', id: 'gridscan-can' }),
          h('button', { class: 'btn small primary', onclick: () => scanDialog('can') }, 'Scan…')),
        h('div', { class: 'card-body' }, h('div', { class: 'addr-grid', id: 'grid-can' }))),
      h('div', { class: 'legend' },
        h('span', null, h('i', { style: { background: 'var(--ok-soft)', border: '1px solid var(--ok)' } }), 'device online'),
        h('span', null, h('i', { style: { background: 'var(--warn-soft)', border: '1px solid var(--warn)' } }), 'quiet'),
        h('span', null, h('i', { style: { background: 'var(--err-soft)', border: '1px solid var(--err)' } }), 'not responding'),
        h('span', null, h('i', { style: { background: 'var(--surface-2)', border: '1px dashed var(--text-3)' } }), 'known, not seen yet'),
        h('span', { class: 'faint' }, 'Select an empty address to probe it.')));
    $('#gridSeg').addEventListener('click', e => {
      const b = e.target.closest('button'); if (!b) return;
      S.gridCan = b.dataset.p; store('gridCan', S.gridCan); renderGrid();
    });
  }
  $$('#gridSeg button').forEach(b => b.classList.toggle('on', b.dataset.p === S.gridCan));
  fillGrid('grid-rs485', 'rs485', 'modbus', 1, 247);
  if (S.gridCan === 'canopen') fillGrid('grid-can', 'can', 'canopen', 1, 127);
  else fillGrid('grid-can', 'can', 'j1939', 0, 253);
  for (const bus of ['rs485', 'can']) {
    const sc = S.scan[bus];
    $('#gridscan-' + bus).textContent = sc && sc.state === 'running' ? `Scanning ${sc.proto} ${sc.cur}… found ${sc.found}` : '';
  }
}
function fillGrid(id, bus, proto, from, to) {
  const g = $('#' + id);
  if (g.dataset.proto !== proto) {
    g.dataset.proto = proto;
    g.replaceChildren();
    for (let a = from; a <= to; a++) {
      g.append(h('button', { class: 'cell', 'data-addr': a, title: proto === 'j1939' ? `SA ${a} (0x${hex2(a)})` : `address ${a}` }, String(a)));
    }
    g.onclick = e => {
      const c = e.target.closest('.cell'); if (!c) return;
      gridClick(bus, proto, +c.dataset.addr);
    };
  }
  const sc = S.scan[bus];
  const scanning = sc && sc.state === 'running' && (sc.proto === proto || (proto === 'modbus' && bus === 'rs485')) ? sc.cur : -1;
  const scanned = S.scanned[proto === 'modbus' ? 'rs485' : proto] || new Set();
  for (const c of g.children) {
    const a = +c.dataset.addr, key = `${bus}:${proto}:${a}`, d = S.devices.get(key);
    let cls = 'cell';
    if (d) { const st = devStatus(d); cls += ' found' + (st === 'ok' ? '' : st === 'offline' ? ' offline' : ' ' + st); }
    if (a === scanning) cls += ' scanning';
    if (scanned.has(a)) cls += ' scanned';
    if (S.selected === key) cls += ' selected';
    if (c.className !== cls) c.className = cls;
    const lbl = d ? (d.label || '') : '';
    const want = lbl ? [String(a), lbl] : [String(a)];
    if (c.dataset.lbl !== lbl) {
      c.dataset.lbl = lbl;
      c.replaceChildren(want[0], lbl ? h('span', { class: 'lbl' }, lbl) : '');
      c.title = d ? `${devName(d)} — ${STATUS_TEXT[devStatus(d)]}` : (proto === 'j1939' ? `SA ${a} (0x${hex2(a)})` : `address ${a}`);
    }
  }
}
async function gridClick(bus, proto, addr) {
  const key = `${bus}:${proto}:${addr}`;
  if (S.devices.has(key)) return openDevice(key);
  const what = proto === 'modbus' ? `Modbus address ${addr}` : proto === 'canopen' ? `CANopen node ${addr}` : `J1939 address ${addr}`;
  const needActive = bus === 'can' && !canActive();
  const r = await modal(what, h('div', { class: 'stack' },
    h('p', { style: { margin: 0 } }, 'No device registered at this address.'),
    needActive ? h('div', { class: 'warnbox' }, 'Requires CAN active mode (ACK and transmit).') : null),
  [{ label: 'Cancel', value: 'cancel' }, { label: 'Add manually', value: 'add' }, { label: needActive ? 'Switch to active & probe' : 'Probe now', value: 'probe', cls: 'primary' }]);
  if (r === 'add') {
    try { await call('dev.add', { key }); await loadDevices(); openDevice(key); } catch (e) { fail(e); }
  } else if (r === 'probe') {
    try {
      if (needActive) await setCanMode('normal');
      toast(`Probing ${what}…`, '', 1500);
      if (proto === 'modbus') {
        const res = await call('mb.ident', { addr });
        if (!res.present) return toast(`${what}: ${res.probe}`, 'err');
      } else if (proto === 'canopen') {
        await call('co.info', { node: addr });
      } else {
        const res = await call('j1939.request', { pgn: 60928, da: addr });
        if (!res.responses.length) return toast(`${what}: no address claim received`, 'err');
      }
      toast(`Found ${what}`, 'ok');
      await loadDevices();
      openDevice(key);
    } catch (e) { fail(e); }
  }
}

// =====================================================================
// Scanning
// =====================================================================
function onScan(m) {
  const bus = m.bus;
  S.scan[bus] = m;
  const setKey = bus === 'rs485' ? 'rs485' : m.proto;
  if (m.state === 'running') {
    if (!S.scanned[setKey] || m.cur === m.from) S.scanned[setKey] = new Set();
    for (let a = m.from; a < m.cur; a++) S.scanned[setKey].add(a);
  } else {
    S.scanned[setKey] = new Set();
  }
  const bar = $('#scanbar-' + bus);
  if (bar) {
    if (m.state === 'running') {
      const span = Math.max(1, m.to - m.from + 1);
      const pct = m.proto === 'j1939' ? 50 : clamp(((m.cur - m.from) / span) * 100, 0, 100);
      const pass = m.passes > 1 ? ` · pass ${m.pass}/${m.passes} @ ${m.baud} 8${m.parity}1` : '';
      bar.hidden = false;
      bar.replaceChildren(
        h('span', null, m.proto === 'j1939' ? 'Requesting J1939 address claims…' : `Scanning ${m.proto === 'modbus' ? 'address' : 'node'} ${m.cur} of ${m.from}–${m.to}${pass}`),
        h('div', { class: 'progress' }, h('div', { style: { width: pct + '%' } })),
        h('b', null, `${m.found} found`),
        h('button', { class: 'btn small', onclick: () => call('scan.cancel', { bus }).catch(fail) }, 'Stop'));
    } else {
      bar.hidden = true;
    }
  }
  if (S.view === 'grid') renderGrid();
}

async function scanDialog(bus) {
  const c = busCfg(bus);
  if (!c.enabled) {
    if (!await confirmBox(`${BUS_INFO[bus].title} disabled`, 'Enable the port?', 'Enable')) return;
    await toggleBus(bus, true);
  }
  if (bus === 'rs485') return scanDialogRs485();
  return scanDialogCan();
}

async function scanDialogRs485() {
  const c = busCfg('rs485');
  const bauds = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200];
  const est = h('p', { class: 'hint' });
  const from = h('input', { type: 'number', min: 1, max: 247, value: 1 });
  const to = h('input', { type: 'number', min: 1, max: 247, value: 247 });
  const thorough = h('input', { type: 'checkbox' });
  const tmo = h('input', { type: 'number', min: 10, max: 2000, value: c.scanTimeoutMs || 80 });
  const parity = h('select', null, ['N', 'E', 'O'].map(p => h('option', { value: p, selected: p === c.parity }, { N: 'None', E: 'Even', O: 'Odd' }[p])));
  const boxes = bauds.map(b => h('label', { class: 'check' }, h('input', { type: 'checkbox', value: b, checked: b === c.baud }), String(b)));
  const update = () => {
    const n = Math.max(0, +to.value - +from.value + 1);
    const nb = Math.max(1, boxes.filter(l => l.firstChild.checked).length);
    const secs = Math.ceil(n * nb * (+tmo.value + 12) * (thorough.checked ? 2.2 : 1) / 1000);
    est.textContent = `Estimated ${secs} s. Read-only probe (FC03, register 0); any reply, including an exception, registers a device.`;
  };
  [from, to, thorough, tmo, ...boxes.map(b => b.firstChild)].forEach(e => e.addEventListener('input', update));
  update();
  const r = await modal('Scan RS485 for Modbus devices', [
    h('div', { class: 'form-grid' },
      h('label', { class: 'field' }, h('span', null, 'From address'), from),
      h('label', { class: 'field' }, h('span', null, 'To address'), to),
      h('label', { class: 'field' }, h('span', null, 'Timeout per address (ms)'), tmo),
      h('label', { class: 'field' }, h('span', null, 'Parity'), parity)),
    h('div', null, h('div', { class: 'muted', style: { fontSize: '12.5px', marginBottom: '6px' } }, 'Baud rates (each selected rate is scanned)'), h('div', { class: 'row' }, boxes)),
    h('label', { class: 'check' }, thorough, 'Thorough: also probe with FC04 and FC01'),
    est,
  ], [{ label: 'Cancel', value: 'cancel' }, { label: 'Start scan', value: 'go', cls: 'primary' }]);
  if (r !== 'go') return;
  const links = boxes.filter(l => l.firstChild.checked).map(l => ({ baud: +l.firstChild.value, parity: parity.value }));
  runScan('rs485', { bus: 'rs485', from: +from.value, to: +to.value, thorough: thorough.checked, timeoutMs: +tmo.value, links: links.length ? links : undefined });
}

async function scanDialogCan() {
  const c = busCfg('can');
  const kind = h('select', null,
    h('option', { value: 'canopen' }, 'CANopen: SDO read of 0x1000 per node'),
    h('option', { value: 'j1939' }, 'J1939: request address claims'),
    h('option', { value: 'autobaud' }, 'Detect bitrate (listen-only)'));
  const from = h('input', { type: 'number', min: 1, max: 127, value: 1 });
  const to = h('input', { type: 'number', min: 1, max: 127, value: 127 });
  const identify = h('input', { type: 'checkbox', checked: true });
  const thorough = h('input', { type: 'checkbox' });
  const coOpts = h('div', { class: 'stack' }, h('div', { class: 'form-grid' },
    h('label', { class: 'field' }, h('span', null, 'From node'), from), h('label', { class: 'field' }, h('span', null, 'To node'), to)),
    h('label', { class: 'check' }, identify, 'Read identity objects (0x1008–0x1018)'));
  const jOpts = h('label', { class: 'check' }, thorough, 'Also request Component ID and Software ID');
  const warn = h('div', { class: 'warnbox' }, `Scanning switches CAN to active mode at ${kbit(c.bitrate)}bit/s. A bitrate mismatch in active mode generates error frames. Use Detect bitrate if the bus rate is unknown.`);
  const sync = () => {
    coOpts.hidden = kind.value !== 'canopen';
    jOpts.hidden = kind.value !== 'j1939';
    warn.hidden = kind.value === 'autobaud' || canActive();
  };
  kind.addEventListener('change', sync); sync();
  const r = await modal('Scan the CAN bus', [h('label', { class: 'field' }, h('span', null, 'Action'), kind), coOpts, jOpts, warn,
    h('p', { class: 'hint' }, 'Passive discovery (CANopen heartbeat, J1939 traffic) runs continuously.')],
  [{ label: 'Cancel', value: 'cancel' }, { label: 'Start', value: 'go', cls: 'primary' }]);
  if (r !== 'go') return;
  if (kind.value === 'autobaud') return autobaud();
  if (!canActive()) { try { await setCanMode('normal'); } catch (e) { return fail(e); } }
  if (kind.value === 'canopen') runScan('can', { bus: 'can', proto: 'canopen', from: +from.value, to: +to.value, identify: identify.checked });
  else runScan('can', { bus: 'can', proto: 'j1939', thorough: thorough.checked });
}

async function runScan(bus, args) {
  try {
    const res = await call('scan', args, 30 * 60 * 1000);
    const n = (res.found || []).length;
    toast(res.cancelled ? `Scan stopped — ${n} found` : `Scan complete — ${n} device${n === 1 ? '' : 's'} found`, n ? 'ok' : '');
    if (res.garbled && res.garbled.length) toast(`CRC errors from ${res.garbled.length} address(es): check baud rate, parity and termination`, 'err', 8000);
    loadDevices();
  } catch (e) { fail(e); }
}
async function autobaud() {
  toast('Detecting CAN bitrate (listen-only)…');
  try {
    const r = await call('can.autobaud', {}, 60000);
    if (r.detected) toast(`Bitrate detected: ${kbit(r.detected)}bit/s (listen-only)`, 'ok', 6000);
    else toast('No valid traffic at any bitrate. Check CAN H/L/GND wiring and termination.', 'err', 8000);
  } catch (e) { fail(e); }
}

// =====================================================================
// Bus control
// =====================================================================
async function toggleBus(bus, on) {
  try {
    applyBusStatus(bus, await call(bus + '.config', { enabled: on }));
    toast(`${BUS_INFO[bus].title} ${on ? 'enabled' : 'disabled'}`);
  } catch (e) { fail(e); }
}
async function setCanMode(mode) {
  if (mode === 'normal' && !canActive()) {
    const ok = await confirmBox('Switch CAN to active mode?',
      h('div', { class: 'stack' }, h('p', { style: { margin: 0 } }, 'Active mode acknowledges frames and permits transmission (scans, SDO, NMT, J1939 requests, raw frames).'),
        h('div', { class: 'warnbox' }, `Configured bitrate: ${kbit(busCfg('can').bitrate)}bit/s. A mismatch generates error frames on the bus.`)),
      'Switch to active');
    if (!ok) throw new Error('Cancelled');
  }
  const r = await call('can.config', { mode });
  applyBusStatus('can', r);
}
// Merge a bus status returned by rs485.config / can.config so the UI reacts
// immediately instead of waiting for the next status push.
function applyBusStatus(bus, r) {
  if (!S.status || !r || r.enabled === undefined) return;
  const modeChanged = bus === 'can' && S.status.can.mode !== r.mode;
  Object.assign(S.status[bus], r);
  renderHeader(); renderBusHeads(); scheduleRender();
  if (modeChanged && S.selected) renderDrawer();
}

async function busConfigDialog(bus) {
  const c = busCfg(bus);
  if (bus === 'rs485') {
    const baud = h('select', null, [1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200, 230400].map(b => h('option', { value: b, selected: b === c.baud }, b)));
    if (![...baud.options].some(o => +o.value === c.baud)) baud.append(h('option', { value: c.baud, selected: true }, c.baud));
    const parity = h('select', null, ['N', 'E', 'O'].map(p => h('option', { value: p, selected: p === c.parity }, { N: 'None', E: 'Even', O: 'Odd' }[p])));
    const stop = h('select', null, [1, 2].map(s => h('option', { value: s, selected: s === c.stop }, s)));
    const tmo = h('input', { type: 'number', value: c.timeoutMs, min: 10, max: 5000 });
    const r = await modal('RS485 port', h('div', { class: 'form-grid' },
      h('label', { class: 'field' }, h('span', null, 'Baud rate'), baud), h('label', { class: 'field' }, h('span', null, 'Parity'), parity),
      h('label', { class: 'field' }, h('span', null, 'Stop bits'), stop), h('label', { class: 'field' }, h('span', null, 'Response timeout (ms)'), tmo)),
    [{ label: 'Cancel', value: 'cancel' }, { label: 'Apply', value: 'ok', cls: 'primary' }]);
    if (r !== 'ok') return;
    try { await call('rs485.config', { baud: +baud.value, parity: parity.value, stop: +stop.value, timeoutMs: +tmo.value }); toast('RS485 settings applied', 'ok'); } catch (e) { fail(e); }
  } else {
    const rates = (S.hello && S.hello.canBitrates) || [125000, 250000, 500000, 1000000];
    const br = h('select', null, rates.map(b => h('option', { value: b, selected: b === c.bitrate }, kbit(b) + 'bit/s')));
    const mode = h('select', null, h('option', { value: 'listen', selected: c.mode !== 'normal' }, 'Listen-only (passive, safe)'), h('option', { value: 'normal', selected: c.mode === 'normal' }, 'Active (ACK + transmit)'));
    const r = await modal('CAN port', [h('div', { class: 'form-grid' },
      h('label', { class: 'field' }, h('span', null, 'Bitrate'), br), h('label', { class: 'field' }, h('span', null, 'Mode'), mode))],
    [{ label: 'Cancel', value: 'cancel' }, { label: 'Detect bitrate', value: 'auto' }, { label: 'Apply', value: 'ok', cls: 'primary' }]);
    if (r === 'auto') return autobaud();
    if (r !== 'ok') return;
    try {
      if (mode.value === 'normal' && c.mode !== 'normal') await setCanMode('normal');
      await call('can.config', { bitrate: +br.value, mode: mode.value });
      toast('CAN settings applied', 'ok');
    } catch (e) { if (e.message !== 'Cancelled') fail(e); }
  }
}

// =====================================================================
// Device drawer
// =====================================================================
const drawer = () => $('#drawer');
function closeDrawer() {
  S.selected = null;
  drawer().classList.remove('open'); drawer().setAttribute('aria-hidden', 'true');
  $('#scrim').classList.remove('open');
  scheduleRender();
}
async function openDevice(key) {
  const d = S.devices.get(key);
  if (!d) return;
  if (S.selected !== key) S.drawerTab = 'overview';
  S.selected = key;
  drawer().classList.add('open'); drawer().setAttribute('aria-hidden', 'false');
  $('#scrim').classList.add('open');
  renderDrawer();
  scheduleRender();
  refreshSelected();
}
async function refreshSelected() {
  const key = S.selected; if (!key) return;
  try {
    const full = await call('dev.get', { key });
    if (S.selected !== key) return;
    S.devices.set(key, Object.assign(S.devices.get(key) || {}, full));
    if (full.watch) recordWatchHistory(full);
    updateDrawerLive();
  } catch (e) { /* device may have been removed */ }
}

function renderDrawer() {
  const d = S.devices.get(S.selected); if (!d) return closeDrawer();
  const tabs = [['overview', 'Overview']];
  if (d.proto === 'modbus') tabs.push(['regs', 'Registers'], ['watch', 'Watch']);
  if (d.proto === 'canopen') tabs.push(['objects', 'Objects & NMT'], ['watch', 'Watch']);
  if (d.proto === 'j1939') tabs.push(['pgns', 'PGNs']);
  if (!tabs.some(t => t[0] === S.drawerTab)) S.drawerTab = 'overview';
  const label = h('input', { value: d.label || '', placeholder: devName(d), 'aria-label': 'Device label', title: 'Rename' });
  const saveLabel = async () => {
    if (label.value === (d.label || '')) return;
    try { await call('dev.update', { key: d.key, label: label.value }); d.label = label.value; scheduleRender(); toast('Saved', 'ok', 1200); } catch (e) { fail(e); }
  };
  label.addEventListener('change', saveLabel);
  label.addEventListener('keydown', e => { if (e.key === 'Enter') label.blur(); });
  const body = h('div', { class: 'dbody', id: 'dbody' });
  $('#drawerInner').replaceChildren(
    h('div', { class: 'dh' },
      h('div', { class: 'dh-top' },
        h('span', { class: 'badge ' + d.bus }, devTag(d)),
        h('div', { class: 'dh-title' }, label),
        h('span', { id: 'dStatusChip' }),
        h('button', { class: 'icon-btn', title: 'Close', 'aria-label': 'Close', onclick: closeDrawer }, '✕')),
      h('div', { class: 'dh-sub', id: 'dSub' }),
      h('div', { class: 'dtabs' }, tabs.map(([k, t]) => h('button', {
        class: S.drawerTab === k ? 'active' : '', onclick: () => { S.drawerTab = k; renderDrawer(); },
      }, t)))),
    body);
  const tab = S.drawerTab;
  const content = { overview: drawerOverview, regs: drawerRegisters, objects: drawerObjects, watch: drawerWatch, pgns: drawerPgns }[tab];
  append(body, [content(d)]);
  updateDrawerLive();
}

function updateDrawerLive() {
  const d = S.devices.get(S.selected); if (!d) return;
  const st = devStatus(d);
  const chip = $('#dStatusChip');
  if (chip) chip.replaceChildren(h('span', { class: 'chip ' + ({ ok: 'ok', stale: 'warn', err: 'err' }[st] || '') }, STATUS_TEXT[st]));
  const sub = $('#dSub');
  if (sub) sub.textContent = `${d.key} · ${devName(d)} · last seen ${d.lastSeen ? fmtAge(serverNow() - d.lastSeen) : 'never'} · ${d.rx} frames${d.errs ? ` · ${d.errs} errors` : ''}`;
  const live = $('#dLive');
  if (live) live.replaceChildren(...overviewFacts(d));
  const wv = $('#dWatchValues');
  if (wv) renderWatchValues(d, wv);
  const pg = $('#dPgnTable');
  if (pg) renderPgnTable(d, pg);
  const nmt = $('#dNmtState');
  if (nmt) nmt.textContent = d.state != null ? (NMT_STATE[d.state] || d.state) : 'unknown (no heartbeat seen)';
}

function overviewFacts(d) {
  const rows = [];
  const add = (k, v) => { if (v !== undefined && v !== null && v !== '') rows.push(h('dt', null, k), h('dd', null, v)); };
  add('Status', `${STATUS_TEXT[devStatus(d)]}${d.passive ? ' (discovered passively)' : ''}`);
  add('Last seen', d.lastSeen ? fmtAge(serverNow() - d.lastSeen) : 'never');
  add('Frames / errors', `${d.rx} / ${d.errs}${d.consecErr ? ` (${d.consecErr} in a row)` : ''}`);
  if (d.proto === 'canopen') add('NMT state', d.state != null ? NMT_STATE[d.state] || d.state : 'unknown');
  add('Vendor', d.vendor); add('Product', d.product); add('Revision', d.revision); add('Name', d.name); add('Serial', d.serial);
  if (d.co) {
    const prof = d.co.deviceType & 0xffff;
    add('Device type', `0x${hex8(d.co.deviceType)}${CO_PROFILES[prof] ? ' — ' + CO_PROFILES[prof] : prof ? ' — profile ' + prof : ''}`);
    add('Vendor ID', '0x' + hex8(d.co.vendorId)); add('Product code', '0x' + hex8(d.co.productCode));
    add('Revision no.', '0x' + hex8(d.co.revision)); add('Serial no.', d.co.serial);
  }
  if (d.emcy) add('Last EMCY', `code 0x${hex4(d.emcy.code)}, error register 0x${hex2(d.emcy.reg)} — ${fmtAge(serverNow() - d.emcy.ts)} (${d.emcy.count} total)`);
  if (d.j1939Name) {
    add('NAME', d.j1939Name);
    const n = decodeJ1939Name(d.j1939Name);
    for (const [k, v] of Object.entries(n)) if (!k.startsWith('_')) add(k, v);
  }
  return [h('dl', { class: 'kv' }, rows)];
}

function drawerOverview(d) {
  const out = [h('div', { class: 'dsec' }, h('h4', null, 'Device'), h('div', { id: 'dLive' }))];
  if (d.proto === 'modbus') {
    const c = busCfg('rs485');
    const baud = h('input', { type: 'number', value: d.baud || c.baud, class: 'w-md' });
    const parity = h('select', null, ['N', 'E', 'O'].map(p => h('option', { value: p, selected: p === (d.parity || c.parity) }, p)));
    const stop = h('select', null, [1, 2].map(s => h('option', { value: s, selected: s === (d.stop || c.stop) }, s)));
    out.push(h('div', { class: 'dsec' }, h('h4', null, 'Serial link'),
      h('div', { class: 'row' }, baud, parity, stop,
        h('button', { class: 'btn small', onclick: async () => { try { await call('dev.update', { key: d.key, baud: +baud.value, parity: parity.value, stop: +stop.value }); toast('Saved', 'ok', 1200); } catch (e) { fail(e); } } }, 'Save')),
      h('p', { class: 'hint' }, 'Link settings used for requests to this device. The device itself is not reconfigured.')));
  }
  const notes = h('textarea', { placeholder: 'Location, wiring, purpose…' }, d.notes || '');
  out.push(h('div', { class: 'dsec' }, h('h4', null, 'Notes'), notes,
    h('div', { class: 'row', style: { marginTop: '6px' } }, h('button', { class: 'btn small', onclick: async () => { try { await call('dev.update', { key: d.key, notes: notes.value }); toast('Notes saved', 'ok', 1200); } catch (e) { fail(e); } } }, 'Save notes'))));
  const actions = h('div', { class: 'row' });
  const ident = async () => {
    try {
      if (d.proto === 'modbus') await call('mb.ident', { addr: d.addr });
      else if (d.proto === 'canopen') { if (!canActive()) await setCanMode('normal'); await call('co.info', { node: d.addr }); }
      else { if (!canActive()) await setCanMode('normal'); await call('j1939.request', { pgn: 65259, da: d.addr }); }
      toast('Identification refreshed', 'ok'); refreshSelected();
    } catch (e) { if (e.message !== 'Cancelled') fail(e); }
  };
  actions.append(h('button', { class: 'btn small', onclick: ident }, 'Re-identify'),
    h('button', { class: 'btn small danger', onclick: async () => {
      if (!await confirmBox('Forget this device?', 'Removes the device with its label, notes and watch list. It reappears if rediscovered.', 'Remove', true)) return;
      try { await call('dev.remove', { key: d.key }); closeDrawer(); } catch (e) { fail(e); }
    } }, 'Remove device'));
  out.push(h('div', { class: 'dsec' }, h('h4', null, 'Actions'), actions));
  return out;
}

// ---------------------------------------------------------------- Modbus registers
function drawerRegisters(d) {
  const table = h('select', null, h('option', { value: 3 }, 'Holding registers (FC03, 4x)'), h('option', { value: 4 }, 'Input registers (FC04, 3x)'),
    h('option', { value: 1 }, 'Coils (FC01, 0x)'), h('option', { value: 2 }, 'Discrete inputs (FC02, 1x)'));
  const st = store('regs.' + d.key) || { fn: 3, start: 0, count: 10, fmt: 'u16' };
  table.value = st.fn;
  const start = h('input', { type: 'number', min: 0, max: 65535, value: st.start });
  const count = h('input', { type: 'number', min: 1, max: 125, value: st.count, class: 'w-sm' });
  const fmt = h('select', null, Object.entries(MB_FMTS).filter(([k]) => k !== 'bool').map(([k, v]) => h('option', { value: k }, v)));
  fmt.value = st.fmt;
  const result = h('div');
  let last = null;
  const show = () => {
    if (!last) { result.replaceChildren(h('p', { class: 'muted' }, 'Addresses are 0-based (40001 = holding register 0).')); return; }
    const fn = last.fn, writable = fn === 3 || fn === 1;
    const rows = [];
    if (fn <= 2) {
      last.values.forEach((v, i) => rows.push(h('tr', null, h('td', { class: 'mono' }, last.start + i),
        h('td', null, h('span', { class: 'chip ' + (v ? 'ok' : '') }, v ? 'ON' : 'OFF')),
        h('td', null, writable ? h('button', { class: 'btn small', onclick: () => writeRegs(d, 5, last.start + i, [v ? 0 : 1], doRead) }, v ? 'Turn OFF' : 'Turn ON') : null,
          addWatchBtn(d, fn, last.start + i, 1, 'bool')))));
    } else {
      const f = fmt.value, words = FMT_WORDS[f] || 1;
      const bytes = hexBytes(last.raw);
      for (let i = 0; i < last.values.length; i += (words > 1 && f !== 'hex' ? words : 1)) {
        const chunk = bytes.slice(i * 2, (i + words) * 2);
        const val = f === 'hex' ? '0x' + hex4(last.values[i]) : decodeValue(f, chunk, 'modbus', fn);
        const addr = last.start + i;
        const valCell = h('td', { class: 'mono' }, fmtValue(val));
        const editBtn = writable && words === 1 && f !== 'str' ? h('button', { class: 'btn small ghost', title: 'Write value', onclick: () => inlineEdit(valCell, last.values[i], async nv => writeRegs(d, 6, addr, [nv], doRead)) }, '✎') : null;
        rows.push(h('tr', null, h('td', { class: 'mono' }, addr), h('td', { class: 'mono faint' }, spaced(last.raw.substr(i * 4, Math.min(words, last.values.length - i) * 4))), valCell,
          h('td', null, editBtn, addWatchBtn(d, fn, addr, Math.max(1, words === 4 ? 4 : words), f === 'hex' ? 'hex' : f))));
      }
    }
    result.replaceChildren(h('div', { class: 'table-wrap', style: { maxHeight: '48vh' } }, h('table', { class: 'tbl' },
      h('thead', null, h('tr', null, h('th', null, 'Address'), fn <= 2 ? null : h('th', null, 'Raw'), h('th', null, 'Value'), h('th', null, ''))),
      h('tbody', null, rows))),
    h('p', { class: 'hint' }, `Read ${new Date().toLocaleTimeString()}`));
  };
  const doRead = async () => {
    store('regs.' + d.key, { fn: +table.value, start: +start.value, count: +count.value, fmt: fmt.value });
    try { last = await call('mb.read', { addr: d.addr, fn: +table.value, start: +start.value, count: +count.value }); show(); } catch (e) { fail(e); }
  };
  fmt.addEventListener('change', show);
  const multiStart = h('input', { type: 'number', min: 0, max: 65535, placeholder: 'start' });
  const multiVals = h('input', { placeholder: 'values: 100, 200, 0x10', style: { flex: 1 } });
  show();
  return [
    h('div', { class: 'dsec' }, h('h4', null, 'Read'),
      h('div', { class: 'stack' }, table, h('div', { class: 'row' },
        h('label', { class: 'field' }, h('span', null, 'Start'), start), h('label', { class: 'field' }, h('span', null, 'Count'), count),
        h('label', { class: 'field', style: { flex: 1 } }, h('span', null, 'Show as'), fmt),
        h('button', { class: 'btn primary', style: { alignSelf: 'flex-end' }, onclick: doRead }, 'Read')))),
    h('div', { class: 'dsec' }, result),
    h('div', { class: 'dsec' }, h('h4', null, 'Write multiple holding registers (FC16)'),
      h('div', { class: 'row' }, multiStart, multiVals, h('button', { class: 'btn', onclick: () => {
        const vals = multiVals.value.split(/[\s,;]+/).filter(Boolean).map(parseNum);
        if (!vals.length || vals.some(v => isNaN(v) || v < -32768 || v > 65535)) return toast('Values must be 16-bit, comma-separated', 'err');
        writeRegs(d, 16, +multiStart.value || 0, vals.map(v => v & 0xffff), doRead);
      } }, 'Write'))),
  ];
}
function addWatchBtn(d, fn, addr, count, fmt) {
  return h('button', { class: 'btn small ghost', title: 'Add to watch list', onclick: async () => {
    const full = S.devices.get(d.key);
    const watch = (full.watch || []).map(stripWatch);
    if (watch.length >= 16) return toast('Watch list is full (16 items)', 'err');
    watch.push({ name: `${fn <= 2 ? (fn === 1 ? 'coil' : 'input') : (fn === 3 ? 'hr' : 'ir')} ${addr}`, fn, addr, count, fmt, scale: 1, unit: '' });
    try { await call('dev.update', { key: d.key, watch, pollMs: full.pollMs || 1000 }); toast('Added to watch list (polling every ' + ((full.pollMs || 1000) / 1000) + ' s)', 'ok'); refreshSelected(); } catch (e) { fail(e); }
  } }, '+ watch');
}
function inlineEdit(cell, current, onSave) {
  const inp = h('input', { class: 'edit-in', value: current });
  const done = async ok => {
    if (ok) {
      const v = parseNum(inp.value);
      if (isNaN(v) || v < -32768 || v > 65535) { toast('Value must be 0..65535 (or -32768..32767)', 'err'); return; }
      await onSave(v & 0xffff);
    } else cell.textContent = current;
  };
  inp.addEventListener('keydown', e => { if (e.key === 'Enter') done(true); if (e.key === 'Escape') done(false); });
  cell.replaceChildren(inp, h('button', { class: 'btn small primary', onclick: () => done(true) }, 'Write'));
  inp.focus(); inp.select();
}
async function writeRegs(d, fn, start, values, after) {
  const what = fn === 5 || fn === 15 ? 'coil' : 'register';
  if (!await confirmBox(`Write to ${devName(d)}?`, `${what} ${start}${values.length > 1 ? '…' + (start + values.length - 1) : ''} ← ${values.join(', ')}  (FC${fn === 6 && values.length > 1 ? 16 : fn})`, 'Write')) return;
  try { await call('mb.write', { addr: d.addr, fn, start, values }); toast('Written', 'ok', 1500); if (after) after(); } catch (e) { fail(e); }
}

// ---------------------------------------------------------------- CANopen objects
const CO_OBJECTS = [
  [0x1000, 0, 'Device type', 'u32'], [0x1001, 0, 'Error register', 'u8'], [0x1008, 0, 'Device name', 'str'],
  [0x1009, 0, 'Hardware version', 'str'], [0x100A, 0, 'Software version', 'str'], [0x1017, 0, 'Producer heartbeat (ms)', 'u16', true],
  [0x1018, 1, 'Vendor ID', 'u32'], [0x1018, 2, 'Product code', 'u32'], [0x1018, 3, 'Revision number', 'u32'], [0x1018, 4, 'Serial number', 'u32'],
  [0x1003, 0, 'Error history count', 'u8'],
];
function sdoValueText(r, fmt) {
  if (r.abort != null) return abortText(r.abort);
  const bytes = hexBytes(r.hex);
  const v = decodeValue(fmt || (r.size === 1 ? 'u8' : r.size === 2 ? 'u16' : r.size <= 4 ? 'u32' : 'str'), bytes, 'canopen');
  return typeof v === 'number' ? `${v}  (0x${v.toString(16).toUpperCase()})` : `"${v}"`;
}
function drawerObjects(d) {
  const needActive = !canActive();
  const out = [];
  if (needActive) out.push(h('div', { class: 'warnbox', style: { marginBottom: '12px' } }, 'CAN listen-only: SDO and NMT unavailable. ',
    h('button', { class: 'btn small', onclick: async () => { try { await setCanMode('normal'); renderDrawer(); } catch (e) { if (e.message !== 'Cancelled') fail(e); } } }, 'Switch to active mode')));
  const nmt = cmd => async () => {
    if ((cmd === 'reset' || cmd === 'resetcomm') && !await confirmBox('Reset node?', `Send NMT ${cmd === 'reset' ? 'reset node' : 'reset communication'} to node ${d.addr}?`, 'Reset', true)) return;
    try { await call('co.nmt', { node: d.addr, cmd }); toast(`NMT ${cmd} sent`, 'ok', 1500); } catch (e) { fail(e); }
  };
  out.push(h('div', { class: 'dsec' }, h('h4', null, 'Network management'),
    h('p', { style: { margin: '0 0 8px' } }, 'State: ', h('b', { id: 'dNmtState' })),
    h('div', { class: 'row nmt-row' },
      h('button', { class: 'btn small', disabled: needActive, onclick: nmt('start') }, 'Start'),
      h('button', { class: 'btn small', disabled: needActive, onclick: nmt('preop') }, 'Pre-operational'),
      h('button', { class: 'btn small', disabled: needActive, onclick: nmt('stop') }, 'Stop'),
      h('button', { class: 'btn small danger', disabled: needActive, onclick: nmt('reset') }, 'Reset node'),
      h('button', { class: 'btn small', disabled: needActive, onclick: nmt('resetcomm') }, 'Reset comm'))));

  const rows = CO_OBJECTS.map(([idx, sub, name, fmt, editable]) => {
    const val = h('td', { class: 'mono' }, '—');
    const read = async () => {
      try { const r = await call('co.sdo.read', { node: d.addr, index: idx, sub }); val.textContent = sdoValueText(r, fmt); } catch (e) { val.textContent = e.message; }
    };
    const edit = editable ? h('button', { class: 'btn small ghost', disabled: needActive, onclick: async () => {
      const v = prompt(`New value for ${name}:`); if (v == null) return;
      const n = parseNum(v); if (isNaN(n)) return toast('Not a number', 'err');
      try { const r = await call('co.sdo.write', { node: d.addr, index: idx, sub, value: n, size: fmt === 'u8' ? 1 : fmt === 'u16' ? 2 : 4 }); if (r.abort != null) toast(abortText(r.abort), 'err'); else { toast('Written', 'ok', 1200); read(); } } catch (e) { fail(e); }
    } }, '✎') : null;
    return { tr: h('tr', null, h('td', { class: 'mono' }, `${hex4(idx)}:${sub}`), h('td', null, name), val, h('td', null, h('button', { class: 'btn small ghost', disabled: needActive, onclick: read }, 'Read'), edit)), read };
  });
  out.push(h('div', { class: 'dsec' }, h('div', { class: 'row' }, h('h4', { style: { margin: 0 } }, 'Standard objects'), h('span', { class: 'spacer' }),
    h('button', { class: 'btn small', disabled: needActive, onclick: async () => { for (const r of rows) await r.read(); } }, 'Read all')),
  h('table', { class: 'tbl', style: { marginTop: '6px' } }, h('tbody', null, rows.map(r => r.tr)))));

  const idx = h('input', { placeholder: 'index (hex)', class: 'w-md' });
  const sub = h('input', { type: 'number', min: 0, max: 255, value: 0, class: 'w-sm' });
  const type = h('select', null, Object.entries(CO_FMTS).map(([k, v]) => h('option', { value: k }, v)));
  type.value = 'u32';
  const val = h('input', { placeholder: 'value', class: 'w-md' });
  const res = h('div', { class: 'mono', style: { marginTop: '8px', minHeight: '1.4em' } });
  const getIdx = () => { const n = parseInt(idx.value.replace(/^0x/i, ''), 16); if (isNaN(n) || n < 0 || n > 0xffff) throw new Error('Index must be hex 0000..FFFF'); return n; };
  out.push(h('div', { class: 'dsec' }, h('h4', null, 'SDO access'),
    h('div', { class: 'row' }, idx, h('span', null, 'sub'), sub, type),
    h('div', { class: 'row', style: { marginTop: '6px' } },
      h('button', { class: 'btn', disabled: needActive, onclick: async () => {
        try { const r = await call('co.sdo.read', { node: d.addr, index: getIdx(), sub: +sub.value }); res.textContent = `${hex4(r.index)}:${r.sub} = ${sdoValueText(r, type.value)}${r.hex ? '   [' + spaced(r.hex) + ']' : ''}`; } catch (e) { res.textContent = e.message; }
      } }, 'Read'),
      val,
      h('button', { class: 'btn', disabled: needActive, onclick: async () => {
        try {
          const t = type.value, args = { node: d.addr, index: getIdx(), sub: +sub.value };
          if (t === 'hex') args.hex = val.value; else if (t === 'str' || t === 'f32') {
            const bytes = t === 'str' ? [...val.value].map(c => c.charCodeAt(0)) : [...new Uint8Array(new Float32Array([parseFloat(val.value)]).buffer)];
            args.hex = bytes.map(hex2).join('');
          } else { const n = parseNum(val.value); if (isNaN(n)) throw new Error('Not a number'); args.value = n; args.size = t.endsWith('8') ? 1 : t.endsWith('16') ? 2 : 4; }
          if (!await confirmBox('Write SDO?', `node ${d.addr} ${hex4(args.index)}:${args.sub} ← ${val.value}`, 'Write')) return;
          const r = await call('co.sdo.write', args);
          res.textContent = r.abort != null ? abortText(r.abort) : 'written OK';
        } catch (e) { res.textContent = e.message; }
      } }, 'Write')),
    res,
    h('p', { class: 'hint' }, 'Writes use expedited SDO (up to 4 bytes).')));
  out.push(h('div', { class: 'dsec' }, h('h4', null, 'Parameters in non-volatile memory'),
    h('div', { class: 'row' },
      h('button', { class: 'btn small', disabled: needActive, onclick: async () => {
        if (!await confirmBox('Store parameters?', 'Writes "save" to 0x1010:1. The node stores its configuration in non-volatile memory.', 'Store')) return;
        try { const r = await call('co.sdo.write', { node: d.addr, index: 0x1010, sub: 1, hex: '73617665' }); toast(r.abort != null ? abortText(r.abort) : 'Parameters stored', r.abort != null ? 'err' : 'ok'); } catch (e) { fail(e); }
      } }, 'Store (0x1010)'),
      h('button', { class: 'btn small danger', disabled: needActive, onclick: async () => {
        if (!await confirmBox('Restore factory defaults?', 'Writes "load" to 0x1011:1. Defaults apply after node reset.', 'Restore defaults', true)) return;
        try { const r = await call('co.sdo.write', { node: d.addr, index: 0x1011, sub: 1, hex: '6C6F6164' }); toast(r.abort != null ? abortText(r.abort) : 'Defaults restored (reset the node to apply)', r.abort != null ? 'err' : 'ok'); } catch (e) { fail(e); }
      } }, 'Restore defaults (0x1011)'))));
  return out;
}

// ---------------------------------------------------------------- watch list
const stripWatch = w => ({ name: w.name, fn: w.fn, addr: w.addr, sub: w.sub, count: w.count, fmt: w.fmt, scale: w.scale, unit: w.unit });
function sparkline(points) {
  if (!points || points.length < 2) return sv('svg', { class: 'spark', viewBox: '0 0 90 24' });
  const vs = points.map(p => p.v), mn = Math.min(...vs), mx = Math.max(...vs), rng = mx - mn || 1;
  const d = vs.map((v, i) => `${i ? 'L' : 'M'}${(i / (vs.length - 1) * 88 + 1).toFixed(1)} ${(22 - (v - mn) / rng * 20).toFixed(1)}`).join(' ');
  return sv('svg', { class: 'spark', viewBox: '0 0 90 24' }, sv('path', { d }));
}
function renderWatchValues(d, tbody) {
  const ws = d.watch || [];
  if (!ws.length) { tbody.replaceChildren(h('tr', null, h('td', { colspan: 5, class: 'muted' }, 'No watch items. Add below, or use "+ watch" in Registers.'))); return; }
  tbody.replaceChildren(...ws.map((w, i) => {
    let val = '—';
    if (w.ts) {
      if (w.err) val = d.proto === 'canopen' && w.abort ? abortText(w.abort) : (w.err === -1 ? 'timeout' : d.proto === 'modbus' && w.err > 0 ? 'exception ' + w.err : 'error');
      else val = fmtValue(decodeValue(w.fmt, hexBytes(w.raw), d.proto, w.fn), w.scale, w.unit);
    }
    const src = d.proto === 'modbus' ? `${({ 1: 'coil', 2: 'input', 3: 'hr', 4: 'ir' })[w.fn]} ${w.addr}${w.count > 1 ? '+' + (w.count - 1) : ''}` : `${hex4(w.addr)}:${w.sub}`;
    return h('tr', null, h('td', null, w.name || '(unnamed)'), h('td', { class: 'mono faint' }, src),
      h('td', { class: 'mono' }, h('span', { class: w.err ? 'faint' : 'val-big', style: { fontSize: '14px' } }, val)),
      h('td', null, sparkline(S.watchHist.get(d.key + '|' + i))),
      h('td', null, h('button', { class: 'btn small ghost', title: 'Remove', onclick: async () => {
        const watch = ws.map(stripWatch); watch.splice(i, 1);
        try { await call('dev.update', { key: d.key, watch }); refreshSelected(); } catch (e) { fail(e); }
      } }, '✕')));
  }));
}
function drawerWatch(d) {
  const poll = h('select', null, [[0, 'Off'], [250, '0.25 s'], [500, '0.5 s'], [1000, '1 s'], [2000, '2 s'], [5000, '5 s'], [10000, '10 s'], [30000, '30 s'], [60000, '1 min']]
    .map(([v, t]) => h('option', { value: v, selected: v === (d.pollMs || 0) }, t)));
  poll.addEventListener('change', async () => { try { await call('dev.update', { key: d.key, pollMs: +poll.value }); toast('Polling ' + (+poll.value ? 'every ' + poll.options[poll.selectedIndex].text : 'off'), 'ok', 1500); } catch (e) { fail(e); } });
  const name = h('input', { placeholder: 'Name', style: { flex: 1 } });
  const scale = h('input', { type: 'number', value: 1, step: 'any', class: 'w-sm', title: 'Scale factor' });
  const unit = h('input', { placeholder: 'unit', class: 'w-sm' });
  let src;
  const fmt = h('select');
  if (d.proto === 'modbus') {
    const fn = h('select', null, h('option', { value: 3 }, 'Holding reg'), h('option', { value: 4 }, 'Input reg'), h('option', { value: 1 }, 'Coil'), h('option', { value: 2 }, 'Discrete in'));
    const addr = h('input', { type: 'number', min: 0, max: 65535, placeholder: 'address', class: 'w-md' });
    const fillFmt = () => fmt.replaceChildren(...Object.entries(MB_FMTS).filter(([k]) => (+fn.value <= 2) === (k === 'bool' || k === 'hex')).map(([k, v]) => h('option', { value: k }, v)));
    fn.addEventListener('change', fillFmt); fillFmt();
    src = { nodes: [fn, addr], get: () => { const f = fmt.value; return { fn: +fn.value, addr: +addr.value, count: +fn.value <= 2 ? 1 : (FMT_WORDS[f] || 1), fmt: f }; } };
  } else {
    const idx = h('input', { placeholder: 'index hex', class: 'w-md' });
    const sub = h('input', { type: 'number', min: 0, max: 255, value: 0, class: 'w-sm' });
    fmt.replaceChildren(...Object.entries(CO_FMTS).map(([k, v]) => h('option', { value: k }, v)));
    src = { nodes: [idx, h('span', null, 'sub'), sub], get: () => { const n = parseInt(idx.value.replace(/^0x/i, ''), 16); if (isNaN(n)) throw new Error('Index must be hex'); return { fn: 0, addr: n, sub: +sub.value, count: 1, fmt: fmt.value }; } };
  }
  const add = async () => {
    try {
      const it = Object.assign({ name: name.value || 'item', scale: parseFloat(scale.value) || 1, unit: unit.value }, src.get());
      const watch = (S.devices.get(d.key).watch || []).map(stripWatch);
      if (watch.length >= 16) throw new Error('Watch list is full (16 items)');
      watch.push(it);
      await call('dev.update', { key: d.key, watch, pollMs: +poll.value || 1000 });
      if (!+poll.value) poll.value = 1000;
      name.value = ''; refreshSelected(); toast('Added', 'ok', 1200);
    } catch (e) { fail(e); }
  };
  const warn = d.proto === 'canopen' && !canActive() ? h('div', { class: 'warnbox', style: { marginBottom: '10px' } }, 'CAN listen-only: CANopen polling requires active mode.') : null;
  return [
    warn,
    h('div', { class: 'dsec' }, h('div', { class: 'row' }, h('h4', { style: { margin: 0 } }, 'Live values'), h('span', { class: 'spacer' }), h('span', { class: 'muted' }, 'Poll every'), poll),
      h('table', { class: 'tbl', style: { marginTop: '8px' } }, h('thead', null, h('tr', null, h('th', null, 'Name'), h('th', null, 'Source'), h('th', null, 'Value'), h('th', null, 'Trend'), h('th'))),
        h('tbody', { id: 'dWatchValues' }))),
    h('div', { class: 'dsec' }, h('h4', null, 'Add item'),
      h('div', { class: 'stack' }, h('div', { class: 'row' }, name), h('div', { class: 'row' }, ...src.nodes, fmt),
        h('div', { class: 'row' }, h('span', { class: 'muted' }, '×'), scale, unit, h('span', { class: 'spacer' }), h('button', { class: 'btn primary', onclick: add }, 'Add')))),
  ];
}

// ---------------------------------------------------------------- J1939 PGNs
function renderPgnTable(d, tbody) {
  const ps = (d.pgns || []).slice().sort((a, b) => a.pgn - b.pgn);
  if (!ps.length) { tbody.replaceChildren(h('tr', null, h('td', { colspan: 5, class: 'muted' }, 'No PGNs received from this address.'))); return; }
  tbody.replaceChildren(...ps.map(p => h('tr', null,
    h('td', { class: 'mono' }, `${p.pgn}`, h('div', { class: 'faint' }, '0x' + p.pgn.toString(16).toUpperCase())),
    h('td', null, PGN_NAMES[p.pgn] || '', h('div', { class: 'faint' }, decodePgnData(p.pgn, hexBytes(p.data)))),
    h('td', { class: 'mono' }, p.count), h('td', null, fmtAge(serverNow() - p.ts)), h('td', { class: 'mono' }, spaced(p.data)))));
}
function drawerPgns(d) {
  const presets = [[65259, 'Component ID'], [65242, 'Software ID'], [64965, 'ECU ID'], [65260, 'Vehicle ID'], [65226, 'DM1 active DTCs'], [65227, 'DM2 previous DTCs'], [60928, 'Address claim']];
  const pgn = h('input', { placeholder: 'PGN', class: 'w-md', value: 65259 });
  const sel = h('select', null, presets.map(([p, n]) => h('option', { value: p }, `${n} (${p})`)), h('option', { value: '' }, 'Custom…'));
  sel.addEventListener('change', () => { if (sel.value) pgn.value = sel.value; });
  const res = h('div', { class: 'mono', style: { marginTop: '8px', whiteSpace: 'pre-wrap' } });
  return [
    h('div', { class: 'dsec' }, h('h4', null, 'Parameter groups seen'),
      h('div', { class: 'table-wrap', style: { maxHeight: '45vh' } }, h('table', { class: 'tbl' },
        h('thead', null, h('tr', null, h('th', null, 'PGN'), h('th', null, 'Name / value'), h('th', null, 'Count'), h('th', null, 'Last'), h('th', null, 'Data'))),
        h('tbody', { id: 'dPgnTable' })))),
    h('div', { class: 'dsec' }, h('h4', null, 'Request a PGN from this ECU'),
      !canActive() ? h('div', { class: 'warnbox', style: { marginBottom: '8px' } }, 'Requires CAN active mode.') : null,
      h('div', { class: 'row' }, sel, pgn, h('button', { class: 'btn primary', onclick: async () => {
        try {
          if (!canActive()) await setCanMode('normal');
          const p = parseNum(pgn.value); if (isNaN(p)) throw new Error('PGN must be a number');
          const r = await call('j1939.request', { pgn: p, da: d.addr }, 10000);
          res.textContent = r.responses.length ? r.responses.map(x => `SA ${x.sa}: ${spaced(x.data)}${x.tp ? '  (multi-packet)' : ''}\n${asciiOf(x.data)}`).join('\n') : 'No response (PGN may be unsupported).';
          refreshSelected();
        } catch (e) { if (e.message !== 'Cancelled') res.textContent = e.message; }
      } }, 'Request')), res),
  ];
}
const asciiOf = hex => { const s = hexBytes(hex).map(c => c >= 32 && c < 127 ? String.fromCharCode(c) : '·').join(''); return /[A-Za-z0-9]{3}/.test(s) ? '"' + s + '"' : ''; };

// =====================================================================
// Traffic view
// =====================================================================
function onTrace(m) {
  if (m.drop) S.traceDrop += m.drop;
  const add = m.f.map(r => ({ seq: r[0], t: r[1], bus: r[2], dir: r[3], fl: r[4], id: r[5], hex: r[6] }));
  S.trace.push(...add);
  if (S.trace.length > S.traceMax) S.trace.splice(0, S.trace.length - S.traceMax);
  if (S.view === 'traffic' && !S.tracePaused) appendTraffic(add);
}
function frameText(f) {
  if (f._dec !== undefined) return f._dec;
  const bytes = hexBytes(f.hex);
  f._dec = f.bus === 0 ? decodeModbus(bytes, f.fl & 4) : decodeCan(f.id, f.fl & 1, bytes, f.fl & 2);
  return f._dec;
}
function frameMatches(f) {
  const flt = S.traceFilter;
  if (flt.bus === 'rs485' && f.bus !== 0) return false;
  if (flt.bus === 'can' && f.bus !== 1) return false;
  if (!flt.text) return true;
  const q = flt.text.toLowerCase().replace(/^0x/, '');
  const idStr = f.bus === 1 ? fmtId(f.id, f.fl & 1).toLowerCase() : String(f.id);
  if (idStr === q || idStr.replace(/^0+/, '') === q.replace(/^0+/, '')) return true;
  return (f.hex.toLowerCase().includes(q.replace(/\s/g, '')) || frameText(f).toLowerCase().includes(q));
}
function fmtFrameTime(f) {
  const be = bootEpoch();
  if (be) {
    const d = new Date(be + f.t);
    return d.toLocaleTimeString([], { hour12: false }) + '.' + String(d.getMilliseconds()).padStart(3, '0');
  }
  return (f.t / 1000).toFixed(3) + ' s';
}
function trafficRow(f) {
  const can = f.bus === 1;
  const cls = (f.dir ? 'tx' : '') + (!can && !(f.fl & 4) ? ' bad' : '');
  return h('tr', { class: cls },
    h('td', { class: 'mono' }, fmtFrameTime(f)),
    h('td', { class: can ? 'bus-can' : 'bus-rs485' }, can ? 'CAN' : 'RS485'),
    h('td', null, f.dir ? 'TX' : 'RX'),
    h('td', { class: 'mono' }, can ? fmtId(f.id, f.fl & 1) + (f.fl & 2 ? ' R' : '') : (f.hex ? String(f.id) : '')),
    h('td', { class: 'mono' }, f.hex.length / 2),
    h('td', { class: 'data' }, spaced(f.hex)),
    h('td', { class: 'dec' }, frameText(f)));
}
function buildTraffic() {
  const v = $('#view-traffic');
  if (v.dataset.built) return;
  v.dataset.built = '1';
  const bus = h('div', { class: 'seg' }, ['all', 'rs485', 'can'].map(b => h('button', { 'data-b': b }, b === 'all' ? 'All' : b === 'rs485' ? 'RS485' : 'CAN')));
  bus.addEventListener('click', e => { const b = e.target.closest('button'); if (!b) return; S.traceFilter.bus = b.dataset.b; renderTraffic(true); });
  const filter = h('input', { placeholder: 'Filter: ID, address, bytes or text', style: { width: '280px' }, id: 'trFilter' });
  filter.addEventListener('input', debounce(() => { S.traceFilter.text = filter.value.trim(); renderTraffic(true); }, 200));
  const pause = h('button', { class: 'btn', id: 'trPause', onclick: () => { S.tracePaused = !S.tracePaused; renderTraffic(true); } });
  v.append(h('div', { class: 'toolbar' }, bus, filter, pause,
    h('button', { class: 'btn', onclick: () => { S.trace = []; S.traceDrop = 0; call('trace.clear').catch(() => {}); renderTraffic(true); } }, 'Clear'),
    h('button', { class: 'btn', onclick: exportCsv }, 'Export CSV'),
    h('span', { class: 'spacer' }), h('span', { class: 'muted', id: 'trStats' })),
  h('div', { class: 'card' }, h('div', { class: 'table-wrap', id: 'trWrap', style: { maxHeight: 'calc(100vh - 230px)' } },
    h('table', { class: 'tbl traffic-table' }, h('thead', null, h('tr', null, ['Time', 'Bus', 'Dir', 'ID / Addr', 'Len', 'Data', 'Decoded'].map(t => h('th', null, t)))),
      h('tbody', { id: 'trBody' })))));
  $('#trWrap').addEventListener('scroll', () => { const w = $('#trWrap'); S.traceFollow = w.scrollTop + w.clientHeight >= w.scrollHeight - 30; });
}
const TRAFFIC_ROWS = 600;
function renderTraffic(full) {
  buildTraffic();
  $$('#view-traffic .seg button').forEach(b => b.classList.toggle('on', b.dataset.b === S.traceFilter.bus));
  $('#trPause').textContent = S.tracePaused ? '▶ Resume' : '⏸ Pause';
  if (!full) return;
  const rows = [];
  for (let i = S.trace.length - 1; i >= 0 && rows.length < TRAFFIC_ROWS; i--) if (frameMatches(S.trace[i])) rows.push(S.trace[i]);
  rows.reverse();
  const body = $('#trBody');
  body.replaceChildren(...rows.map(trafficRow));
  if (!rows.length) body.append(h('tr', null, h('td', { colspan: 7, class: 'empty' }, S.trace.length ? 'No frames match the filter.' : 'No frames received.')));
  trafficStats();
  if (S.traceFollow) { const w = $('#trWrap'); w.scrollTop = w.scrollHeight; }
}
function appendTraffic(frames) {
  const body = $('#trBody'); if (!body) return;
  const add = frames.filter(frameMatches);
  if (!add.length) return trafficStats();
  if (body.querySelector('.empty')) body.replaceChildren();
  body.append(...add.map(trafficRow));
  while (body.children.length > TRAFFIC_ROWS) body.firstChild.remove();
  trafficStats();
  if (S.traceFollow) { const w = $('#trWrap'); w.scrollTop = w.scrollHeight; }
}
function trafficStats() {
  const el = $('#trStats'); if (!el) return;
  el.textContent = `${S.trace.length} frames buffered${S.traceDrop ? ` · ${S.traceDrop} dropped (too fast)` : ''}${S.tracePaused ? ' · paused' : ''}`;
}
function exportCsv() {
  const rows = [['time', 'ms_since_boot', 'bus', 'dir', 'id', 'ext', 'len', 'data', 'decoded']];
  for (const f of S.trace) if (frameMatches(f)) rows.push([fmtFrameTime(f), f.t.toFixed(3), f.bus ? 'CAN' : 'RS485', f.dir ? 'TX' : 'RX', f.bus ? fmtId(f.id, f.fl & 1) : f.id, f.fl & 1 ? 1 : 0, f.hex.length / 2, f.hex, frameText(f)]);
  download(`wonderscope-trace-${new Date().toISOString().replace(/[:.]/g, '-')}.csv`, rows.map(r => r.map(c => /[",\n]/.test(String(c)) ? `"${String(c).replace(/"/g, '""')}"` : c).join(',')).join('\n'), 'text/csv');
}

// =====================================================================
// CAN ID map
// =====================================================================
function onIds(ids) {
  const prev = new Map(S.ids.map(r => [r[0] + (r[1] ? 'x' : ''), r[6]]));
  S.idsPrev = prev;
  S.ids = ids;
  if (S.view === 'ids') renderIds();
  else if (S.view === 'map') scheduleRender();
}
function renderIds() {
  const v = $('#view-ids');
  if (!v.dataset.built) {
    v.dataset.built = '1';
    const filter = h('input', { placeholder: 'Filter by ID or data', style: { width: '220px' } });
    filter.addEventListener('input', debounce(() => { S.idsFilter = filter.value.trim().toLowerCase().replace(/^0x/, ''); renderIds(); }, 200));
    v.append(h('div', { class: 'toolbar' }, filter,
      h('button', { class: 'btn', onclick: () => call('can.ids.clear').then(() => { S.ids = []; renderIds(); }).catch(fail) }, 'Clear'),
      h('span', { class: 'spacer' }), h('span', { class: 'muted', id: 'idsStats' })),
    h('div', { class: 'card' }, h('div', { class: 'table-wrap', style: { maxHeight: 'calc(100vh - 230px)' } },
      h('table', { class: 'tbl ids-table' }, h('thead', { id: 'idsHead' }), h('tbody', { id: 'idsBody' })))),
    h('p', { class: 'hint' }, 'All CAN identifiers received (passive, protocol-independent). Highlighted bytes changed since the previous update.'));
  }
  const cols = [['id', 'ID'], ['type', 'Frame'], ['dlc', 'DLC'], ['count', 'Count'], ['rate', 'Rate /s'], ['age', 'Last seen'], ['data', 'Data'], ['dec', 'Meaning']];
  const s = S.idsSort;
  $('#idsHead').replaceChildren(h('tr', null, cols.map(([k, t]) => h('th', {
    class: 'sortable' + (s.key === k ? ' sorted' + (s.asc ? ' asc' : '') : ''),
    onclick: () => { S.idsSort = { key: k, asc: s.key === k ? !s.asc : k === 'id' }; store('idsSort', S.idsSort); renderIds(); },
  }, t))));
  let rows = S.ids.map(r => ({ id: r[0], ext: r[1], dlc: r[2], count: r[3], rate: r[4], age: r[5], data: r[6], rtr: r[7] }));
  if (S.idsFilter) rows = rows.filter(r => fmtId(r.id, r.ext).toLowerCase().includes(S.idsFilter) || r.data.toLowerCase().includes(S.idsFilter.replace(/\s/g, '')));
  const key = { id: r => r.ext * 2 ** 32 + r.id, type: r => r.ext, dlc: r => r.dlc, count: r => r.count, rate: r => r.rate, age: r => r.age, data: r => r.data, dec: r => r.id }[s.key] || (r => r.id);
  rows.sort((a, b) => (key(a) > key(b) ? 1 : key(a) < key(b) ? -1 : 0) * (s.asc ? 1 : -1));
  $('#idsBody').replaceChildren(...(rows.length ? rows.map(r => {
    const prev = S.idsPrev.get(r.id + (r.ext ? 'x' : ''));
    const bytes = (r.data.match(/../g) || []).map((b, i) => h('span', { class: prev && prev.substr(i * 2, 2) !== b ? 'chg' : '' }, b));
    const dataCell = h('td', { class: 'mono' }); bytes.forEach((b, i) => { if (i) dataCell.append(' '); dataCell.append(b); });
    return h('tr', null, h('td', { class: 'mono' }, fmtId(r.id, r.ext)), h('td', null, (r.ext ? '29-bit' : '11-bit') + (r.rtr ? ' RTR' : '')),
      h('td', { class: 'mono' }, r.dlc), h('td', { class: 'mono' }, r.count), h('td', { class: 'mono' }, r.rate.toFixed(1)),
      h('td', null, fmtAge(r.age)), dataCell, h('td', { class: 'dec', style: { fontSize: '12px', color: 'var(--text-2)' } }, decodeCan(r.id, r.ext, hexBytes(r.data), r.rtr)));
  }) : [h('tr', null, h('td', { colspan: 8, class: 'empty' }, S.ids.length ? 'No IDs match the filter.' : 'No CAN traffic received. Verify the bitrate (Scan → Detect bitrate).'))]));
  $('#idsStats').textContent = `${S.ids.length} identifiers`;
}

// =====================================================================
// Console (same commands as the USB serial console)
// =====================================================================
const COMMANDS = ['help', 'clear', 'status', 'info', 'rs485', 'can', 'scan', 'devices', 'dev', 'ids', 'mb', 'rs485send', 'sdo', 'nmt', 'co', 'j1939', 'cansend', 'trace', 'wifi', 'auth', 'time', 'reboot', 'factory-reset'];
const Console = {
  built: false, hist: store('hist') || [], hIdx: -1, pending: new Map(), lines: 0, tracing: false,
  build() {
    if (this.built) return;
    this.built = true;
    const v = $('#view-console');
    this.out = h('div', { class: 'term', id: 'termOut', tabindex: 0 });
    this.input = h('input', { autocomplete: 'off', spellcheck: 'false', 'aria-label': 'Console command', placeholder: "command ('help' for list)" });
    this.traceBtn = h('button', { class: 'btn small', onclick: () => this.run(this.tracing ? 'trace off' : 'trace on') }, 'Live trace');
    v.append(
      h('div', { class: 'term-bar' },
        h('span', { class: 'muted' }, 'Command console. Also available on USB serial at 115200 baud.'),
        h('span', { class: 'spacer' }),
        ...['help', 'status', 'devices', 'ids'].map(c => h('button', { class: 'btn small', onclick: () => this.run(c) }, c)),
        this.traceBtn,
        h('button', { class: 'btn small', onclick: () => { navigator.clipboard && navigator.clipboard.writeText(this.out.innerText).then(() => toast('Copied', 'ok', 1000)); } }, 'Copy'),
        h('button', { class: 'btn small', onclick: () => this.clear() }, 'Clear')),
      this.out,
      h('div', { class: 'term-input' }, h('span', null, 'wonderscope>'), this.input));
    this.input.addEventListener('keydown', e => this.key(e));
    this.out.addEventListener('click', () => { if (!getSelection().toString()) this.input.focus(); });
    this.print("WonderScope console. 'help' lists commands; 'help <command>' shows details. Tab: complete · ↑/↓: history · Ctrl+L: clear.", 'note');
  },
  focus() { this.build(); setTimeout(() => this.input.focus(), 0); },
  clear() { this.out.replaceChildren(); this.lines = 0; },
  // Print a line; if 'into' is given (a command's block), the output goes under that command.
  print(text, cls, into) {
    text = String(text).replace(/\x1b\[[0-9;]*[A-Za-z]/g, '');
    const atBottom = this.out.scrollTop + this.out.clientHeight >= this.out.scrollHeight - 20;
    const line = h('div', { class: cls || '' }, text);
    (into && into.isConnected ? into : this.out).append(line);
    this.lines++;
    while (this.lines > 3000 && this.out.firstChild) { this.out.firstChild.remove(); this.lines--; }
    if (atBottom) this.out.scrollTop = this.out.scrollHeight;
    return line;
  },
  run(line) {
    this.build();
    line = line.trim();
    const block = h('div');
    this.out.append(block);
    this.print('wonderscope> ' + line, 'in', block);
    if (!line) return;
    if (this.hist[this.hist.length - 1] !== line) { this.hist.push(line); if (this.hist.length > 100) this.hist.shift(); store('hist', this.hist); }
    this.hIdx = -1;
    if (line === 'clear' || line === 'cls') return this.clear();
    if (/^trace\b/.test(line)) { this.tracing = !/\b(off|stop)\b/.test(line); this.traceBtn.classList.toggle('primary', this.tracing); }
    if (!S.connected) return this.print('Not connected to the board.', 'err', block);
    const id = RPC.seq++;
    this.pending.set(id, block);
    RPC.ws.send(JSON.stringify({ cmd: 'cli', line, id }));
  },
  receive(m) {
    this.build();
    const text = m.text || '';
    if (m.id != null && this.pending.has(m.id)) {
      const block = this.pending.get(m.id);
      this.pending.delete(m.id);
      if (text) this.print(text, /^error:|^Unknown command|^Usage:|^Invalid argument/.test(text) ? 'err' : '', block);
    } else if (text) {
      const isTrace = /^\s*[\d:.]+( s)? +(CAN|RS485)/.test(text) || text.startsWith('...');
      // Progress notes belong to the most recent command still awaiting its reply.
      const blocks = [...this.pending.values()];
      this.print(text, isTrace ? 'tr' : 'note', isTrace ? null : blocks[blocks.length - 1]);
    }
  },
  key(e) {
    const inp = this.input;
    if (e.key === 'Enter') { const l = inp.value; inp.value = ''; this.run(l); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); if (!this.hist.length) return; this.hIdx = this.hIdx < 0 ? this.hist.length - 1 : Math.max(0, this.hIdx - 1); inp.value = this.hist[this.hIdx]; }
    else if (e.key === 'ArrowDown') { e.preventDefault(); if (this.hIdx < 0) return; this.hIdx++; if (this.hIdx >= this.hist.length) { this.hIdx = -1; inp.value = ''; } else inp.value = this.hist[this.hIdx]; }
    else if (e.key === 'Tab') {
      e.preventDefault();
      const v = inp.value; if (/\s/.test(v)) return;
      const m = COMMANDS.filter(c => c.startsWith(v.toLowerCase()));
      if (m.length === 1) inp.value = m[0] + ' ';
      else if (m.length > 1) this.print(m.join('  '), 'note');
    } else if (e.key === 'l' && e.ctrlKey) { e.preventDefault(); this.clear(); }
    else if (e.key === 'c' && e.ctrlKey && !getSelection().toString()) { if (this.tracing) this.run('trace off'); }
  },
};

// =====================================================================
// Settings view
// =====================================================================
function renderSettings() {
  const v = $('#view-settings');
  if (!S.hello) { v.replaceChildren(h('p', { class: 'muted' }, 'Connecting…')); return; }
  if (S.view !== 'settings' && v.dataset.built) return;
  v.dataset.built = '1';
  const st = S.hello.settings, r = busCfg('rs485'), c = busCfg('can');
  const field = (label, input, hint) => h('label', { class: 'field' }, h('span', null, label), input, hint ? h('small', { class: 'faint' }, hint) : null);
  const save = (cmd, get, msg) => async () => { try { await call(cmd, get()); toast(msg || 'Saved', 'ok'); const s = await call('settings.get'); S.hello.settings = s; } catch (e) { fail(e); } };

  // RS485
  const rBaud = h('input', { type: 'number', value: r.baud, min: 300, max: 1000000 });
  const rPar = h('select', null, ['N', 'E', 'O'].map(p => h('option', { value: p, selected: p === r.parity }, { N: 'None', E: 'Even', O: 'Odd' }[p])));
  const rStop = h('select', null, [1, 2].map(s => h('option', { value: s, selected: s === r.stop }, s)));
  const rTmo = h('input', { type: 'number', value: r.timeoutMs, min: 10, max: 5000 });
  const rScan = h('input', { type: 'number', value: r.scanTimeoutMs, min: 10, max: 2000 });
  const rEn = h('input', { type: 'checkbox', checked: r.enabled });
  // CAN
  const rates = S.hello.canBitrates || [];
  const cBr = h('select', null, rates.map(b => h('option', { value: b, selected: b === c.bitrate }, kbit(b) + 'bit/s')));
  const cMode = h('select', null, h('option', { value: 'listen', selected: c.mode !== 'normal' }, 'Listen-only (passive)'), h('option', { value: 'normal', selected: c.mode === 'normal' }, 'Active (ACK + transmit)'));
  const cEn = h('input', { type: 'checkbox', checked: c.enabled });
  const cRec = h('input', { type: 'checkbox', checked: c.autoRecover });
  const cCo = h('input', { type: 'checkbox', checked: c.canopenPassive });
  const cJ = h('input', { type: 'checkbox', checked: c.j1939Passive });
  const cSa = h('input', { type: 'number', value: c.j1939Sa, min: 0, max: 253 });
  const cSdo = h('input', { type: 'number', value: c.sdoTimeoutMs, min: 10, max: 5000 });
  const cScan = h('input', { type: 'number', value: c.scanTimeoutMs, min: 5, max: 2000 });
  // Wi-Fi
  const w = st.wifi;
  const apSsid = h('input', { value: w.apSsid, maxlength: 32 });
  const apPass = h('input', { type: 'password', placeholder: w.apHasPass ? '(unchanged)' : '(open network)', maxlength: 63 });
  const staSsid = h('input', { value: w.staSsid, maxlength: 32, placeholder: 'not joined' });
  const staPass = h('input', { type: 'password', placeholder: w.staHasPass ? '(unchanged)' : '', maxlength: 63 });
  const host = h('input', { value: w.hostname, maxlength: 32 });
  // Auth
  const aUser = h('input', { value: st.auth.user, maxlength: 16 });
  const aPass = h('input', { type: 'password', placeholder: st.auth.enabled ? '(unchanged)' : 'none', maxlength: 32 });
  const theme = h('select', { id: 'themeSelect' }, THEMES.map(t => h('option', { value: t, selected: t === currentTheme() }, { auto: 'Automatic (follow system)', light: 'Light', dark: 'Dark' }[t])));
  theme.addEventListener('change', () => applyTheme(theme.value));
  const otaFile = h('input', { type: 'file', accept: '.bin' });
  const otaBar = h('div', { class: 'progress', hidden: true }, h('div'));
  const importFile = h('input', { type: 'file', accept: '.json,application/json', hidden: true });
  importFile.addEventListener('change', async () => {
    const f = importFile.files[0]; if (!f) return;
    try { const res = await fetch('/api/devices', { method: 'POST', body: await f.text() }); if (!res.ok) throw new Error(await res.text()); toast('Device list imported', 'ok'); loadDevices(); } catch (e) { fail(e); }
    importFile.value = '';
  });
  const s = S.status || {};
  const card = (title, ...body) => h('div', { class: 'card' }, h('div', { class: 'card-head' }, h('h3', null, title)), h('div', { class: 'card-body stack' }, body));

  v.replaceChildren(h('div', { class: 'settings-grid' },
    card('RS485 port',
      h('label', { class: 'check' }, rEn, 'Enabled'),
      h('div', { class: 'form-grid' }, field('Baud rate', rBaud), field('Parity', rPar), field('Stop bits', rStop),
        field('Response timeout (ms)', rTmo), field('Scan timeout per address (ms)', rScan)),
      h('div', { class: 'row' }, h('button', { class: 'btn primary', onclick: save('rs485.config', () => ({ enabled: rEn.checked, baud: +rBaud.value, parity: rPar.value, stop: +rStop.value, timeoutMs: +rTmo.value, scanTimeoutMs: +rScan.value }), 'RS485 settings applied') }, 'Apply')),
      h('p', { class: 'hint' }, 'Termination: fit jumper H2 (120 Ω) only at a line end.')),
    card('CAN port',
      h('label', { class: 'check' }, cEn, 'Enabled'),
      h('div', { class: 'form-grid' }, field('Bitrate', cBr), field('Mode', cMode), field('J1939 source address', cSa, 'Default 249 (service tool)'),
        field('SDO timeout (ms)', cSdo), field('Scan timeout per node (ms)', cScan)),
      h('label', { class: 'check' }, cRec, 'Recover automatically from bus-off'),
      h('label', { class: 'check' }, cCo, 'Discover CANopen nodes passively (heartbeat / EMCY)'),
      h('label', { class: 'check' }, cJ, 'Discover J1939 ECUs passively (29-bit traffic)'),
      h('div', { class: 'row' },
        h('button', { class: 'btn primary', onclick: async () => {
          try {
            if (cMode.value === 'normal' && !canActive()) await setCanMode('normal');
            await call('can.config', { enabled: cEn.checked, bitrate: +cBr.value, mode: cMode.value, autoRecover: cRec.checked, canopenPassive: cCo.checked, j1939Passive: cJ.checked, j1939Sa: +cSa.value, sdoTimeoutMs: +cSdo.value, scanTimeoutMs: +cScan.value });
            toast('CAN settings applied', 'ok');
          } catch (e) { if (e.message !== 'Cancelled') fail(e); }
        } }, 'Apply'),
        h('button', { class: 'btn', onclick: autobaud }, 'Detect bitrate'),
        h('button', { class: 'btn', onclick: () => call('can.recover').then(() => toast('CAN controller restarted', 'ok')).catch(fail) }, 'Restart controller'),
        h('button', { class: 'btn', onclick: async () => {
          if (!await confirmBox('Run CAN self-test?', 'Internal loopback test. Transmits one frame (ID 7FF) on the bus.', 'Run self-test')) return;
          try { const r = await call('can.selftest'); toast(r.pass ? `Self-test passed (${r.roundTripUs} µs)` : 'Self-test FAILED — ' + r.sendResult, r.pass ? 'ok' : 'err', 6000); } catch (e) { fail(e); }
        } }, 'Self-test')),
      h('p', { class: 'hint' }, 'Listen-only: no transmission or acknowledgement. Active: required for scans, SDO, NMT, J1939 requests and raw frames. Termination: jumper H1 (120 Ω).')),
    card('Wi-Fi',
      h('div', { class: 'form-grid' }, field('Access point name', apSsid), field('Access point password', apPass, '8–63 characters'),
        field('Join network (SSID)', staSsid, 'Leave empty to run as access point only'), field('Network password', staPass), field('Hostname', host, `http://${w.hostname}.local`)),
      h('div', { class: 'row' }, h('button', { class: 'btn primary', onclick: async () => {
        const args = { apSsid: apSsid.value, staSsid: staSsid.value, hostname: host.value };
        if (apPass.value) args.apPass = apPass.value;
        if (staPass.value || !staSsid.value) args.staPass = staPass.value;
        if (!await confirmBox('Apply Wi-Fi settings?', 'Wi-Fi restarts; this page may lose its connection.', 'Apply')) return;
        save('wifi.config', () => args, 'Wi-Fi settings saved — reconnecting')();
      } }, 'Save & reconnect')),
      s.wifi ? h('p', { class: 'hint' }, `AP ${s.wifi.ap.ip} · ${s.wifi.sta.connected ? `joined "${s.wifi.sta.ssid}" as ${s.wifi.sta.ip}` : s.wifi.sta.ssid ? `not connected to "${s.wifi.sta.ssid}"` : 'not joined to a network'}`) : null),
    card('Dashboard login',
      h('div', { class: 'form-grid' }, field('User', aUser), field('Password', aPass, 'Empty disables the login')),
      h('div', { class: 'row' },
        h('button', { class: 'btn primary', onclick: save('auth.config', () => ({ user: aUser.value, pass: aPass.value }), 'Login settings saved') }, 'Save'),
        st.auth.enabled ? h('button', { class: 'btn', onclick: save('auth.config', () => ({ pass: '' }), 'Login disabled') }, 'Disable login') : null),
      h('p', { class: 'hint' }, st.auth.enabled ? 'Login is enabled.' : 'No login required. Set a password on shared networks.')),
    card('Appearance', field('Theme', theme)),
    card('Device list',
      h('p', { class: 'muted', style: { margin: 0 } }, `${S.devices.size} devices. Labels, notes, link settings and watch lists are stored on the board.`),
      h('div', { class: 'row' },
        h('a', { class: 'btn', href: '/api/devices.json', download: 'wonderscope-devices.json' }, 'Export'),
        h('button', { class: 'btn', onclick: () => importFile.click() }, 'Import…'), importFile,
        h('button', { class: 'btn danger', onclick: async () => {
          if (!await confirmBox('Forget all devices?', 'Removes every device, label, note and watch list from the board.', 'Forget all', true)) return;
          call('dev.clear').then(r => toast(`Removed ${r.removed} devices`)).catch(fail);
        } }, 'Forget all'))),
    card('Firmware & system',
      h('dl', { class: 'kv' },
        h('dt', null, 'Firmware'), h('dd', null, `${S.hello.fw} ${S.hello.version} (${S.hello.build})`),
        h('dt', null, 'Board'), h('dd', null, S.hello.board),
        h('dt', null, 'MAC'), h('dd', { class: 'mono' }, S.hello.mac),
        h('dt', null, 'Clock'), h('dd', null, s.epoch ? new Date(s.epoch).toLocaleString() + (s.wifi && s.wifi.ntp ? ' (NTP)' : ' (from browser)') : 'not set')),
      h('div', { class: 'row' }, otaFile, h('button', { class: 'btn', onclick: () => otaUpload(otaFile, otaBar) }, 'Update firmware')), otaBar,
      h('p', { class: 'hint' }, 'File: .pio/build/wonderscope/firmware.bin. Reboots on completion.'),
      h('div', { class: 'row' },
        h('button', { class: 'btn', onclick: async () => { if (await confirmBox('Reboot the board?', 'Monitoring pauses during restart.', 'Reboot')) call('sys.reboot').then(() => toast('Rebooting…')).catch(fail); } }, 'Reboot'),
        h('button', { class: 'btn danger', onclick: async () => { if (await confirmBox('Factory reset?', 'Erases all settings (Wi-Fi, bus configuration, login) and the device list, then reboots.', 'Factory reset', true)) call('sys.factory', { confirm: true }).then(() => toast('Resetting…')).catch(fail); } }, 'Factory reset'))),
  ));
}
function otaUpload(input, bar) {
  const f = input.files[0];
  if (!f) return toast('Select a firmware .bin file', 'err');
  const fd = new FormData(); fd.append('firmware', f, f.name);
  const xhr = new XMLHttpRequest();
  bar.hidden = false;
  xhr.upload.onprogress = e => { if (e.lengthComputable) bar.firstChild.style.width = (e.loaded / e.total * 100) + '%'; };
  xhr.onload = () => { if (xhr.status === 200) toast('Firmware updated — rebooting', 'ok', 8000); else toast('Update failed: ' + xhr.responseText, 'err', 8000); };
  xhr.onerror = () => toast('Upload failed', 'err');
  xhr.open('POST', '/api/update');
  xhr.send(fd);
}

// =====================================================================
// Boot
// =====================================================================
function init() {
  applyTheme(store('theme') || 'auto');
  $('#themeBtn').addEventListener('click', () => applyTheme(THEMES[(THEMES.indexOf(currentTheme()) + 1) % 3]));
  $$('.tabs button').forEach(b => b.addEventListener('click', () => setView(b.dataset.view)));
  document.addEventListener('change', e => {
    const t = e.target;
    if (t.dataset && t.dataset.act === 'toggle-bus') {
      const bus = t.dataset.bus;
      if (!t.checked) {
        confirmBox(`Disable ${BUS_INFO[bus].title}?`, 'Stops monitoring and polling on this bus.', 'Disable').then(ok => { if (ok) toggleBus(bus, false); else t.checked = true; });
      } else toggleBus(bus, true);
    }
  });
  $('#scrim').addEventListener('click', closeDrawer);
  document.addEventListener('keydown', e => { if (e.key === 'Escape' && S.selected && !$('#modal').open) closeDrawer(); });
  window.addEventListener('resize', debounce(() => { if (S.view === 'map') renderMap(); }, 150));
  setInterval(() => { if (S.view === 'map') scheduleRender(); if (S.selected) updateDrawerLive(); }, 5000);
  renderConn();
  const v = store('view');
  setView(['map', 'grid', 'traffic', 'ids', 'console', 'settings'].includes(v) ? v : 'map');
  connect();
}
init();
})();
