/* Mock WonderScope backend for developing the dashboard without hardware.
   Replaces window.WebSocket with a simulator of the firmware's protocol. */
(() => {
'use strict';
const t0 = performance.now();
const up = () => Math.round(performance.now() - t0) + 60000;
const hex = a => a.map(b => (b & 255).toString(16).toUpperCase().padStart(2, '0')).join('');
const settings = {
  rs485: { enabled: true, baud: 9600, parity: 'N', stop: 1, timeoutMs: 200, scanTimeoutMs: 80 },
  can: { enabled: true, bitrate: 250000, mode: 'listen', autoRecover: true, canopenPassive: true, j1939Passive: true, j1939Sa: 249, sdoTimeoutMs: 300, scanTimeoutMs: 40 },
  qwiic: { enabled: true, hz: 400000, autoScan: true },
  i2c: { enabled: false, hz: 100000, autoScan: true },
  spi: { enabled: true, hz: 8000000, mode: 0, cs: [10], readBit: true },
  wifi: { apSsid: 'WonderScope-0C78', apHasPass: true, staSsid: '', staHasPass: false, hostname: 'wonderscope' },
  auth: { user: 'admin', enabled: false },
};
const counters = { rs485: { rx: 0, tx: 0, err: 0 }, can: { rx: 0, tx: 0 }, qwiic: { rx: 0, tx: 0, err: 0 }, i2c: { rx: 0, tx: 0, err: 0 }, spi: { rx: 0, tx: 0, err: 0 } };
const DRIVERS = [['bme280', 'BME280 temperature, humidity, pressure', '0x76 0x77'], ['bmp280', 'BMP280 temperature, pressure', '0x76 0x77'],
  ['sht3x', 'SHT3x temperature, humidity', '0x44 0x45'], ['sht4x', 'SHT4x temperature, humidity', '0x44 0x45'], ['aht20', 'AHT20 / AHT21 temperature, humidity', '0x38'],
  ['bh1750', 'BH1750 ambient light', '0x23 0x5C'], ['tmp102', 'TMP102 temperature', '0x48-0x4B'], ['mcp9808', 'MCP9808 temperature', '0x18-0x1F'],
  ['ina219', 'INA219 bus voltage, current (0.1 ohm shunt)', '0x40-0x4F'], ['mpu6050', 'MPU-6050 accelerometer, gyroscope', '0x68 0x69'],
  ['mpu6500', 'MPU-6500 / MPU-9250 accelerometer, gyroscope', '0x68 0x69'], ['scd4x', 'SCD40 / SCD41 CO2, temperature, humidity', '0x62']]
  .map(([id, name, addrs]) => ({ id, name, addrs }));
let seq = 0;
const devs = new Map();
function addDev(d) {
  const full = Object.assign({ present: true, passive: false, firstSeen: 1000, lastSeen: up(), rx: 10, errs: 0, consecErr: 0, label: '', pollMs: 0, nWatch: 0, notes: '', watch: [] }, d);
  full.key = `${d.bus}:${d.proto}:${d.addr}`;
  devs.set(full.key, full);
}
addDev({ bus: 'rs485', proto: 'modbus', addr: 1, vendor: 'Eastron', product: 'SDM630', revision: '2.1', baud: 9600, parity: 'N', stop: 1, label: 'Main energy meter',
  pollMs: 1000, watch: [{ name: 'Voltage L1', fn: 4, addr: 0, sub: 0, count: 2, fmt: 'f32', scale: 1, unit: 'V' }, { name: 'Frequency', fn: 4, addr: 70, sub: 0, count: 2, fmt: 'f32', scale: 1, unit: 'Hz' }] });
addDev({ bus: 'rs485', proto: 'modbus', addr: 2, baud: 9600, parity: 'N', stop: 1, name: 'XY-MD02' , label: 'Cabinet temp/humidity', pollMs: 2000,
  watch: [{ name: 'Temperature', fn: 4, addr: 1, sub: 0, count: 1, fmt: 's16', scale: 0.1, unit: '°C' }, { name: 'Humidity', fn: 4, addr: 2, sub: 0, count: 1, fmt: 'u16', scale: 0.1, unit: '%' }] });
addDev({ bus: 'rs485', proto: 'modbus', addr: 17, baud: 19200, parity: 'E', stop: 1, vendor: 'Schneider', product: 'ATV320', passive: true });
addDev({ bus: 'rs485', proto: 'modbus', addr: 40, baud: 9600, parity: 'N', stop: 1, consecErr: 4, errs: 4, lastSeen: 1000, label: 'Old flow meter' });
addDev({ bus: 'can', proto: 'canopen', addr: 5, state: 5, name: 'CANopen IO 16DI', co: { deviceType: 0x00030191, vendorId: 0x000002DE, productCode: 0x1234, revision: 0x00010002, serial: 99812 }, revision: '1.2 / 3.0.4' });
addDev({ bus: 'can', proto: 'canopen', addr: 12, state: 127, name: 'Servo drive', co: { deviceType: 0x00020192, vendorId: 0x9A, productCode: 0x402, revision: 1, serial: 4471 }, emcy: { code: 0x2310, reg: 0x03, ts: up() - 5000, count: 2 } });
addDev({ bus: 'can', proto: 'j1939', addr: 0, j1939Name: '0000000000A00F21', vendor: 'CMMNS', product: 'ISB6.7', pgns: [] });
addDev({ bus: 'can', proto: 'j1939', addr: 3, j1939Name: '0000000003201234', passive: true, pgns: [] });
addDev({ bus: 'qwiic', proto: 'i2c', addr: 0x76, product: 'BME280', vendor: 'Bosch', driver: 'bme280', pollMs: 1000, label: 'Enclosure climate' });
addDev({ bus: 'qwiic', proto: 'i2c', addr: 0x44, product: 'SHT4x', vendor: 'Sensirion', driver: 'sht4x', pollMs: 1000 });
addDev({ bus: 'qwiic', proto: 'i2c', addr: 0x3C, product: 'SSD1306 / SH1106 OLED', vendor: '' });
addDev({ bus: 'spi', proto: 'spi', addr: 10, product: 'SPI flash 128 Mbit (JEDEC EF 40 18)', vendor: 'Winbond' });
// dir as the firmware: 1 = write (DIR_TX), 0 = read (DIR_RX)
function i2cFrame(bus, dir, addr, bytes) { counters[bus][dir ? 'tx' : 'rx']++; return [seq++, up(), bus === 'qwiic' ? 2 : 3, dir, 0, addr, hex(bytes)]; }

const ids = new Map();
function canFrame(id, ext, data) {
  const k = id + (ext ? 'x' : '');
  const e = ids.get(k) || { id, ext, count: 0, first: up() };
  e.count++; e.dlc = data.length; e.data = hex(data); e.last = up();
  ids.set(k, e);
  counters.can.rx++;
  return [seq++, up(), 1, 0, ext ? 1 : 0, id, hex(data)];
}
function mbFrame(bytes, dir = 0) {
  if (dir) counters.rs485.tx++; else counters.rs485.rx++;
  return [seq++, up(), 0, dir, 4, bytes[0], hex(bytes)];
}
function crc(b) { let c = 0xffff; for (const x of b) { c ^= x; for (let i = 0; i < 8; i++) c = c & 1 ? (c >> 1) ^ 0xa001 : c >> 1; } return b.concat([c & 255, c >> 8]); }

class MockWS {
  constructor() {
    this.readyState = 0;
    this.subs = {};
    setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); this.emit({ ev: 'hello', d: hello() }); this.start(); }, 150);
  }
  emit(m) { if (this.readyState === 1 && this.onmessage) this.onmessage({ data: JSON.stringify(m) }); }
  close() { this.readyState = 3; clearInterval(this.i1); clearInterval(this.i2); this.onclose && this.onclose(); }
  start() {
    let tick = 0;
    this.i1 = setInterval(() => {
      tick++;
      const f = [];
      if (settings.can.enabled) {
        f.push(canFrame(0x705, 0, [5]), canFrame(0x70C, 0, [127]));
        f.push(canFrame(0x185, 0, [tick & 255, 0, 0x0f, 0]));
        f.push(canFrame(0x0CF00400, 1, [0xF0, 0x7D, 0x8C, (1600 * 8 + tick) & 255, ((1600 * 8 + tick) >> 8) & 255, 0, 0xF0, 0xFF]));
        if (tick % 5 === 0) f.push(canFrame(0x18FEEE00, 1, [92, 40, 0x20, 0x4E, 0xFF, 0xFF, 0xFF, 0xFF]));
        if (tick % 3 === 0) f.push(canFrame(0x18FEF103, 1, [0xFF, 0x00, 0x1E, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]));
        if (tick % 7 === 0) f.push(canFrame(0x3A1, 0, [1, 2, 3, tick & 255]));
      }
      if (settings.rs485.enabled && tick % 2 === 0) {
        f.push(mbFrame(crc([1, 4, 0, 0, 0, 2]), 1));
        const v = new DataView(new ArrayBuffer(4)); v.setFloat32(0, 229.5 + Math.sin(tick / 10) * 2);
        f.push(mbFrame(crc([1, 4, 4, v.getUint8(0), v.getUint8(1), v.getUint8(2), v.getUint8(3)])));
      }
      for (const d of devs.values()) {
        if (d.consecErr >= 3) continue;
        if (d.proto === 'canopen' || d.proto === 'j1939' || d.watch.length) { d.lastSeen = up(); d.rx++; }
        if (d.watch.length) d.watch.forEach((w, i) => {
          const b = new DataView(new ArrayBuffer(4));
          if (w.fmt === 'f32') b.setFloat32(0, (w.unit === 'Hz' ? 50 : 230) + Math.sin(tick / 8 + i) * (w.unit === 'Hz' ? 0.05 : 2));
          else b.setInt16(0, w.unit === '°C' ? 231 + Math.round(Math.sin(tick / 20) * 15) : 455 + Math.round(Math.cos(tick / 25) * 30));
          w.raw = hex([...new Uint8Array(b.buffer)].slice(0, w.count * 2)); w.err = 0; w.ts = up();
        });
        if (d.proto === 'i2c' && d.driver && tick % 10 === 0) {
          const s = tick / 10;
          d.vts = up(); d.lastSeen = up(); d.rx++;
          if (d.driver === 'bme280') {
            d.values = [{ n: 'temperature', u: '°C', v: 24.2 + Math.sin(s / 12) * 1.4 }, { n: 'pressure', u: 'hPa', v: 1012.6 + Math.sin(s / 40) * 0.8 },
              { n: 'humidity', u: '%', v: 41 + Math.cos(s / 15) * 4 }];
            if (settings.qwiic.enabled) f.push(i2cFrame('qwiic', 1, 0x76, [0xF4, 0x25]), i2cFrame('qwiic', 1, 0x76, [0xF7]), i2cFrame('qwiic', 0, 0x76, [0x51, 0x2A, 0x00, 0x82, 0x1C, 0x40, 0x6B, 0x3E]));
          } else {
            d.values = [{ n: 'temperature', u: '°C', v: 23.9 + Math.sin(s / 11) * 1.2 }, { n: 'humidity', u: '%', v: 43 + Math.cos(s / 14) * 3.5 }];
            if (settings.qwiic.enabled) f.push(i2cFrame('qwiic', 1, 0x44, [0xFD]), i2cFrame('qwiic', 0, 0x44, [0x66, 0x12, 0xB4, 0x6E, 0x3A, 0x5C]));
          }
        }
        if (d.proto === 'j1939') d.pgns = d.addr === 0 ? [
          { pgn: 61444, count: tick * 5, ts: up(), data: 'F07D8C400D00F0FF' }, { pgn: 65262, count: tick, ts: up(), data: '5C28204EFFFFFFFF' }, { pgn: 60928, count: 1, ts: 2000, data: '214F0A0000000000' }]
          : [{ pgn: 65265, count: tick * 3, ts: up(), data: 'FF001EFFFFFFFFFF' }];
      }
      if (this.subs.trace && f.length) this.emit({ ev: 'tr', f });
      if (tick % 3 === 0) this.emit({ ev: 'dev', d: [...devs.values()].filter(d => d.consecErr < 3), now: up() });
    }, 100);
    this.i2 = setInterval(() => {
      this.emit({ ev: 'status', s: status() });
      if (this.subs.ids) this.emit({ ev: 'ids', ids: idsJson() });
    }, 1000);
  }
  send(text) {
    const m = JSON.parse(text);
    const ok = r => setTimeout(() => this.emit({ id: m.id, ok: true, result: r || {} }), 80);
    const err = e => setTimeout(() => this.emit({ id: m.id, ok: false, error: e }), 80);
    const c = m.cmd;
    if (c === 'sub') { Object.assign(this.subs, m.topics); return ok({}); }
    if (c === 'time.set') return ok({ epoch: Date.now() });
    if (c === 'dev.list') return ok({ devices: [...devs.values()].map(({ notes, watch, pgns, ...d }) => d), now: up() });
    if (c === 'dev.get') { const d = devs.get(m.key); return d ? ok(Object.assign({ now: up() }, d)) : err('no such device'); }
    if (c === 'dev.update' || c === 'dev.add') {
      let d = devs.get(m.key);
      if (!d) { const [bus, proto, addr] = m.key.split(':'); addDev({ bus, proto, addr: +addr, present: false, rx: 0 }); d = devs.get(m.key); }
      for (const k of ['label', 'notes', 'pollMs', 'watch', 'baud', 'parity', 'stop']) if (m[k] !== undefined) d[k] = m[k];
      d.nWatch = d.watch.length;
      return ok(d);
    }
    if (c === 'dev.remove') { devs.delete(m.key); this.emit({ ev: 'devdel', keys: [m.key] }); return ok(); }
    if (c === 'dev.clear') { const n = devs.size; devs.clear(); this.emit({ ev: 'devclear' }); return ok({ removed: n }); }
    if (c === 'settings.get') return ok(settings);
    if (c === 'rs485.config' || c === 'can.config') {
      const b = c.split('.')[0];
      for (const k of Object.keys(settings[b])) if (m[k] !== undefined) settings[b][k] = m[k];
      if (b === 'can' && m.bitrate && m.bitrate <= 1000) settings.can.bitrate = m.bitrate * 1000;
      return ok(status()[b]);
    }
    if (c === 'can.ids') return ok({ ids: idsJson() });
    if (c === 'qwiic.config' || c === 'i2c.config' || c === 'spi.config') {
      const b = c.split('.')[0];
      for (const k of Object.keys(settings[b])) if (m[k] !== undefined) settings[b][k] = m[k];
      return ok(status()[b]);
    }
    if (c === 'i2c.read') return ok({ bus: m.bus, addr: m.addr, reg: m.reg, rx: m.reg === 0xD0 ? '60' : hex(Array.from({ length: m.count || 1 }, (_, i) => (0x88 + i * 7) & 255)) });
    if (c === 'i2c.write') return ok({ bus: m.bus, addr: m.addr, tx: hex([m.reg]) + m.hex.replace(/\s/g, '') });
    if (c === 'i2c.ident') { const d = devs.get(`${m.bus}:i2c:${m.addr}`); return ok({ addr: m.addr, present: !!d, product: d && d.product }); }
    if (c === 'spi.xfer') return ok({ cs: m.cs, tx: m.hex.replace(/\s/g, ''), rx: m.hex.trim().startsWith('9F') ? '00EF4018' : '00'.repeat(m.hex.replace(/\s/g, '').length / 2) });
    if (c === 'spi.read') return ok({ cs: m.cs, reg: m.reg, data: '00'.repeat(m.count || 1) });
    if (c === 'spi.write' || c === 'spi.ident') return ok({ cs: m.cs, present: true, product: 'SPI flash 128 Mbit (JEDEC EF 40 18)' });
    if (c === 'can.ids.clear') { ids.clear(); return ok(); }
    if (c === 'trace.clear') return ok();
    if (c === 'mb.read') {
      const values = []; for (let i = 0; i < m.count; i++) values.push(m.fn <= 2 ? (i % 3 === 0 ? 1 : 0) : (m.start + i) * 10 + (i % 2 ? 0 : 0x4000));
      const raw = m.fn <= 2 ? hex(Array.from({ length: Math.ceil(m.count / 8) }, () => 0x49)) : hex(values.flatMap(v => [v >> 8, v & 255]));
      if (m.addr === 40) return setTimeout(() => err('timeout (no response)'), 300);
      return ok({ addr: m.addr, fn: m.fn, start: m.start, count: m.count, values, raw });
    }
    if (c === 'mb.write') return ok({ addr: m.addr, fn: m.fn, start: m.start, written: (m.values || [1]).length });
    if (c === 'mb.ident') return ok({ addr: m.addr, present: m.addr < 30, probe: m.addr < 30 ? 'ok' : 'timeout (no response)', vendor: 'Acme' });
    if (c === 'co.sdo.read') {
      if (settings.can.mode === 'listen') return err('CAN is in listen-only mode; switch to normal (active) mode for this command');
      if (m.index === 0x1008) return ok({ node: m.node, index: m.index, sub: m.sub, size: 15, hex: hex([...'CANopen IO 16DI'].map(x => x.charCodeAt(0))), text: 'CANopen IO 16DI' });
      if (m.index === 0x2000) return ok({ node: m.node, index: m.index, sub: m.sub, abort: 0x06020000 });
      return ok({ node: m.node, index: m.index, sub: m.sub, size: 4, hex: '91010300', value: 0x00030191 });
    }
    if (c === 'co.sdo.write' || c === 'co.nmt' || c === 'co.info' || c === 'can.send' || c === 'j1939.send') {
      if (settings.can.mode === 'listen') return err('CAN is in listen-only mode; switch to normal (active) mode for this command');
      return ok({ written: 2, sent: 1, node: m.node, cmd: m.cmd });
    }
    if (c === 'j1939.request') return ok({ pgn: m.pgn, da: m.da, responses: [{ sa: m.da === 255 ? 0 : m.da, data: '434D4D4E532A495342362E372A31323334352A2A', tp: true }] });
    if (c === 'can.autobaud') return setTimeout(() => { settings.can.bitrate = 250000; this.emit({ id: m.id, ok: true, result: { detected: 250000, tried: [{ bitrate: 500000, frames: 0, errors: 12 }, { bitrate: 250000, frames: 41, errors: 0 }] } }); }, 1500);
    if (c === 'can.selftest') return ok({ pass: true, roundTripUs: 412 });
    if (c === 'can.recover' || c === 'scan.cancel' || c === 'wifi.config' || c === 'auth.config' || c === 'sys.reboot') { this.cancel = true; return ok({ note: 'ok' }); }
    if (c === 'scan') {
      if (m.bus === 'can' && settings.can.mode === 'listen') return err('CAN is in listen-only mode; switch to normal (active) mode for this command');
      this.cancel = false;
      const proto = m.bus === 'rs485' ? 'modbus' : m.bus === 'can' ? m.proto : m.bus === 'spi' ? 'spi' : 'i2c';
      if (proto === 'spi') {
        this.emit({ ev: 'scan', bus: 'spi', proto, state: 'done', cur: 10, from: 10, to: 10, found: 1 });
        return ok({ found: [{ cs: 10, present: true, product: 'SPI flash 128 Mbit (JEDEC EF 40 18)', vendor: 'Winbond' }] });
      }
      if (proto === 'i2c') {
        let a = 8;
        const found = [...devs.values()].filter(d => d.bus === m.bus).map(d => ({ addr: d.addr, product: d.product, driver: d.driver }));
        const tick = () => {
          if (a > 0x77) { this.emit({ ev: 'scan', bus: m.bus, proto, state: 'done', cur: 0x77, from: 8, to: 0x77, found: found.length }); this.emit({ id: m.id, ok: true, result: { found } }); return; }
          this.emit({ ev: 'scan', bus: m.bus, proto, state: 'running', cur: a, from: 8, to: 0x77, found: found.filter(f => f.addr < a).length });
          a += 4; setTimeout(tick, 20);
        };
        return tick();
      }
      const from = m.from || (proto === 'j1939' ? 0 : 1), to = m.to || (proto === 'modbus' ? 247 : proto === 'canopen' ? 127 : 253);
      let cur = from, found = [];
      const step = () => {
        if (this.cancel || cur > to) {
          this.emit({ ev: 'scan', bus: m.bus, proto, state: this.cancel ? 'cancelled' : 'done', cur: to, from, to, found: found.length });
          this.emit({ id: m.id, ok: true, result: { found, cancelled: !!this.cancel } });
          return;
        }
        if (proto === 'modbus' && (cur === 9 || cur === 33)) { found.push({ addr: cur, baud: 9600, parity: 'N', vendor: 'Found Co' }); addDev({ bus: 'rs485', proto: 'modbus', addr: cur, baud: 9600, parity: 'N', stop: 1, vendor: 'Found Co', product: 'Model ' + cur }); this.emit({ ev: 'dev', d: [devs.get('rs485:modbus:' + cur)], now: up() }); }
        if (proto === 'canopen' && cur === 21) { found.push({ node: 21, name: 'Encoder' }); addDev({ bus: 'can', proto: 'canopen', addr: 21, state: 127, name: 'Encoder' }); this.emit({ ev: 'dev', d: [devs.get('can:canopen:21')], now: up() }); }
        this.emit({ ev: 'scan', bus: m.bus, proto, state: 'running', cur, from, to, found: found.length, baud: 9600, parity: 'N', pass: 1, passes: 1 });
        cur += proto === 'j1939' ? 300 : 1;
        setTimeout(step, 25);
      };
      return step();
    }
    if (c === 'cli') {
      const l = m.line.trim();
      let text = `(mock) ${l}`;
      if (l === 'help') text = HELP;
      if (l === 'status') text = `WonderScope up ${Math.round(up() / 1000)}s  heap 186k  psram 7030k  web clients 1  devices ${devs.size}\nTime   ${new Date().toISOString().replace('T', ' ').slice(0, 19)} UTC\nWi-Fi  AP "WonderScope-0C78" 192.168.4.1 (0 clients)\n       STA "Workshop" 192.168.0.106  RSSI -52 dBm  http://wonderscope.local/\nRS485  ON   9600 8N1  timeout 200ms  rx ${counters.rs485.rx}  tx ${counters.rs485.tx}  err 0\nCAN    ON   250 kbit/s  listen-only  state running  TEC 0 REC 0  rx ${counters.can.rx}  tx 0  busErr 0  ids ${ids.size}`;
      if (l === 'devices') {
        text = 'KEY                LABEL                STATUS            LAST SEEN  IDENTITY\n' + [...devs.values()].map(d => {
          const st = d.consecErr >= 3 ? 'not responding' : d.passive ? 'online (passive)' : 'online';
          const ident = [d.vendor, d.product, d.name].filter(Boolean).join(' ');
          return `${d.key.padEnd(18)} ${(d.label || '').slice(0, 20).padEnd(20)} ${st.padEnd(17)} ${(d.consecErr >= 3 ? '58m ago' : '0s ago').padEnd(10)} ${ident}`;
        }).join('\n');
      }
      if (l === 'mb read 1 ir 0 4') text = 'reg     hex      unsigned  signed\n0       0x4365   17253     17253\n1       0x8A3D   35389     -30147\n2       0x4366   17254     17254\n3       0x0F5C   3932      3932';
      if (l === 'sdo read 5 1018:1') text = 'node 5  0x1018:1  = 0x000002DE  (734)  [4 bytes]';
      if (l === 'sdo read 5 1008') text = 'node 5  0x1008:0  = "CANopen IO 16DI"  [15 bytes] 43414E6F70656E20494F2031364449';
      if (l.startsWith('trace')) { text = l.includes('off') ? 'Live trace off.' : 'Live trace ON (all buses) - \'trace off\' to stop.'; }
      if (l === 'bogus') text = "Unknown command 'bogus'. Type 'help' for the list.";
      return setTimeout(() => this.emit({ ev: 'cli', id: m.id, text }), 60);
    }
    err(`unknown command '${c}'`);
  }
}
const HELP = `WonderScope console. All dashboard functions are available as commands.
'help <command>' shows usage and examples.

General:
  help           List commands, or show details for one
  status         Bus, Wi-Fi and system status
  info           Firmware and board information
  clear          Clear the screen

Bus control:
  rs485          Show or configure the RS485 port
  can            Show or configure the CAN port

Discovery:
  scan           Discover devices on a bus
  devices        List discovered devices
  dev            Inspect or edit one device
  ids            Table of every CAN identifier seen (count, rate, last data)

Modbus RTU (RS485):
  mb             Modbus RTU master requests

CANopen:
  sdo            CANopen SDO read/write
  nmt            Send a CANopen NMT command
  co             Read a CANopen node's identity objects

J1939:
  j1939          J1939 requests and messages

Raw frames:
  cansend        Send a raw CAN frame (can-utils syntax)
  rs485send      Send raw bytes on RS485 (no CRC unless 'crc')
  trace          Stream live bus frames to this console

System:
  wifi           Show or change Wi-Fi
  auth           Require a login for the web dashboard
  time           Show or set the clock
  reboot         Restart the board
  factory-reset  Erase all settings and the device list`;

function idsJson() {
  return [...ids.values()].map(e => [e.id, e.ext ? 1 : 0, e.dlc, e.count, 10, up() - e.last, e.data, 0]);
}
function status() {
  return {
    up: up(), epoch: Date.now(), heap: 197000, minHeap: 180000, psram: 7100000, clients: 1, dropped: 0,
    wifi: { ap: { ssid: settings.wifi.apSsid, ip: '192.168.4.1', clients: 1 }, sta: { ssid: '', connected: false }, hostname: 'wonderscope', ntp: false },
    rs485: Object.assign({ up: settings.rs485.enabled, busy: '', queued: 0 }, settings.rs485, counters.rs485),
    can: Object.assign({ up: settings.can.enabled, state: settings.can.enabled ? 'running' : 'off', tec: 0, rec: 0, busErrors: 0, rxMissed: 0, rxOverrun: 0, txFailed: 0, arbLost: 0, busy: '', queued: 0, ids: ids.size }, settings.can, counters.can),
    qwiic: Object.assign({ up: settings.qwiic.enabled, sda: 2, scl: 1, busy: '' }, settings.qwiic, counters.qwiic),
    i2c: Object.assign({ up: settings.i2c.enabled, sda: 8, scl: 9, busy: '' }, settings.i2c, counters.i2c),
    spi: Object.assign({ up: settings.spi.enabled, sck: 12, mosi: 11, miso: 13, busy: '' }, settings.spi, counters.spi),
    devices: devs.size, traceHead: seq,
  };
}
function hello() {
  return { fw: 'WonderScope', version: '1.0.0', build: 'mock', board: 'Waveshare ESP32-S3-RS485-CAN', mac: '28:84:85:55:0C:78', chip: 'ESP32-S3', flash: 16777216, psramSize: 8388608,
    canBitrates: [10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000], drivers: DRIVERS, settings, status: status() };
}
window.WebSocket = MockWS;
})();
