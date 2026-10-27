// Lists document + Bluetooth protocol for the nRF EPD Reader.
// Mirrors src/lists_doc.h (document format) and src/lists_ble.h (GATT protocol).
// No browser APIs here except in ListsClient, so the rest runs under Node for tests.

export const SERVICE_UUID = '8d3a0001-6f1b-4c1e-9b8a-2f0e7c5d4a10';
export const DATA_UUID = '8d3a0002-6f1b-4c1e-9b8a-2f0e7c5d4a10';
export const DEVICE_NAME = 'nRF EPD Reader';

// Limits from src/lists_doc.h
export const DOC_MAX = 3072;
export const PUT_MAX = DOC_MAX - 12;
export const MAX_LISTS = 16;
export const MAX_ITEMS = 128;
export const TEXT_MAX = 96; // bytes of UTF-8

const OP_GET = 0x10, OP_PUT_BEGIN = 0x11, OP_PUT_DATA = 0x12, OP_PUT_COMMIT = 0x13;
const EV_DOC_BEGIN = 0x90, EV_DOC_DATA = 0x91, EV_DOC_END = 0x92;
const EV_PUT_OK = 0x93, EV_PUT_ERR = 0x94, EV_CHANGED = 0x95;

export const PUT_ERRORS = {
  1: 'protocol error',
  2: 'lists too large for the device',
  3: 'changed on the device',
  4: 'checksum mismatch',
  5: 'device rejected the lists',
  6: 'SD card write failed',
  7: 'device busy',
};
export const ERR_CONFLICT = 3;

const enc = new TextEncoder();
const dec = new TextDecoder('utf-8', { fatal: true });

// ---------------------------------------------------------------- document

export class DocError extends Error {}

function textOk(s) {
  const n = enc.encode(s).length;
  // eslint-disable-next-line no-control-regex
  return n > 0 && n <= TEXT_MAX && !/[\u0000-\u001f\u007f]/.test(s);
}

// Text -> { rev, lists: [{ name, items: [{ text, checked }] }] }. Same rules as the firmware.
export function parseDoc(text) {
  if (enc.encode(text).length > DOC_MAX) throw new DocError('document too large');
  const lines = text.split('\n');
  if (lines.length && lines[lines.length - 1] === '') lines.pop();
  const m = /^@rev (\d+)$/.exec(lines[0] ?? '');
  if (!m || Number(m[1]) > 0xffffffff) throw new DocError('missing @rev line');
  const doc = { rev: Number(m[1]), lists: [] };
  let items = 0;
  for (const line of lines.slice(1)) {
    if (line === '') continue;
    if (line.startsWith('# ')) {
      const name = line.slice(2);
      if (!textOk(name)) throw new DocError(`bad list name: ${JSON.stringify(name)}`);
      if (doc.lists.length === MAX_LISTS) throw new DocError('too many lists');
      doc.lists.push({ name, items: [] });
    } else if (/^\[[ x]\] /.test(line)) {
      if (!doc.lists.length) throw new DocError('item before the first list');
      const itemText = line.slice(4);
      if (!textOk(itemText)) throw new DocError(`bad item: ${JSON.stringify(itemText)}`);
      if (++items > MAX_ITEMS) throw new DocError('too many items');
      doc.lists[doc.lists.length - 1].items.push({ text: itemText, checked: line[1] === 'x' });
    } else {
      throw new DocError(`unrecognised line: ${JSON.stringify(line)}`);
    }
  }
  return doc;
}

export function serializeDoc(doc, rev = doc.rev) {
  let out = `@rev ${rev}\n`;
  for (const list of doc.lists) {
    out += `# ${list.name}\n`;
    for (const it of list.items) out += `${it.checked ? '[x]' : '[ ]'} ${it.text}\n`;
  }
  return out;
}

// User input -> valid name/item text: one line, no control characters, at most TEXT_MAX bytes
export function cleanText(s) {
  // eslint-disable-next-line no-control-regex
  let t = String(s).replace(/[\u0000-\u001f\u007f]+/g, ' ').replace(/\s+/g, ' ').trim();
  while (enc.encode(t).length > TEXT_MAX) t = [...t].slice(0, -1).join('').trimEnd();
  return t;
}

// What would stop the device from accepting doc; null if fine
export function docProblem(doc) {
  const items = doc.lists.reduce((n, l) => n + l.items.length, 0);
  if (doc.lists.length > MAX_LISTS) return `at most ${MAX_LISTS} lists`;
  if (items > MAX_ITEMS) return `at most ${MAX_ITEMS} items in total`;
  for (const l of doc.lists) {
    if (!textOk(l.name)) return 'every list needs a name';
    for (const it of l.items) if (!textOk(it.text)) return `empty item in "${l.name}"`;
  }
  const size = docSize(doc);
  if (size > PUT_MAX) return `too much text (${size} of ${PUT_MAX} bytes)`;
  return null;
}

export function docSize(doc) {
  return enc.encode(serializeDoc(doc)).length;
}

// ---------------------------------------------------------------- protocol

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

// zlib / Python zlib.crc32, same as the firmware's up_crc32
export function crc32(bytes) {
  let c = 0xffffffff;
  for (const b of bytes) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

export function frameGet() {
  return new Uint8Array([OP_GET]);
}

export function framePutBegin(size, baseRev, crc) {
  const f = new Uint8Array(11);
  const v = new DataView(f.buffer);
  f[0] = OP_PUT_BEGIN;
  v.setUint16(1, size, true);
  v.setUint32(3, baseRev, true);
  v.setUint32(7, crc, true);
  return f;
}

export function framePutData(offset, bytes) {
  const f = new Uint8Array(3 + bytes.length);
  f[0] = OP_PUT_DATA;
  new DataView(f.buffer).setUint16(1, offset, true);
  f.set(bytes, 3);
  return f;
}

export function framePutCommit() {
  return new Uint8Array([OP_PUT_COMMIT]);
}

// Notification bytes -> event object
export function parseEvent(bytes) {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const need = (n) => {
    if (bytes.length < n) throw new Error(`short notification ${bytes[0]?.toString(16)}`);
  };
  switch (bytes[0]) {
    case EV_DOC_BEGIN:
      need(13);
      return { type: 'begin', size: v.getUint16(1, true), rev: v.getUint32(3, true),
               crc: v.getUint32(7, true), maxWrite: v.getUint16(11, true) };
    case EV_DOC_DATA:
      need(3);
      return { type: 'data', offset: v.getUint16(1, true), bytes: bytes.slice(3) };
    case EV_DOC_END:
      return { type: 'end' };
    case EV_PUT_OK:
      need(5);
      return { type: 'put_ok', rev: v.getUint32(1, true) };
    case EV_PUT_ERR:
      need(6);
      return { type: 'put_err', code: bytes[1], rev: v.getUint32(2, true) };
    case EV_CHANGED:
      need(5);
      return { type: 'changed', rev: v.getUint32(1, true) };
    default:
      throw new Error(`unknown notification 0x${bytes[0]?.toString(16)}`);
  }
}

export class PutError extends Error {
  constructor(code, rev) {
    super(PUT_ERRORS[code] ?? `error ${code}`);
    this.code = code;
    this.rev = rev;
  }
}

// Talks to the lists characteristic. `write(bytes)` must do a write-with-response;
// call `onNotify(bytes)` for each notification. Works with Web Bluetooth or a test fake.
export class ListsClient {
  constructor(write, { timeoutMs = 6000 } = {}) {
    this.write = write;
    this.timeoutMs = timeoutMs;
    this.maxWrite = 20;      // learned from DOC_BEGIN
    this.onChanged = null;   // (rev) => void, edits made on the device
    this.waiter = null;
    this.early = null;
    this.incoming = null;
  }

  onNotify(bytes) {
    const ev = parseEvent(bytes);
    if (ev.type === 'changed') {
      this.onChanged?.(ev.rev);
      return;
    }
    if (ev.type === 'begin') {
      this.maxWrite = Math.max(1, ev.maxWrite);
      this.incoming = { ...ev, buf: new Uint8Array(ev.size), got: 0 };
      return;
    }
    if (ev.type === 'data' && this.incoming) {
      this.incoming.buf.set(ev.bytes, ev.offset);
      this.incoming.got = Math.max(this.incoming.got, ev.offset + ev.bytes.length);
      return;
    }
    if (this.waiter) {
      this.waiter(ev);
    } else {
      // e.g. PUT_ERR for a frame rejected before COMMIT: keep it for the next wait
      this.early = ev;
    }
  }

  waitFor() {
    if (this.early) {
      const ev = this.early;
      this.early = null;
      return Promise.resolve(ev);
    }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiter = null;
        reject(new Error('the device did not answer - is the Lists screen open on it?'));
      }, this.timeoutMs);
      this.waiter = (ev) => {
        clearTimeout(timer);
        this.waiter = null;
        resolve(ev);
      };
    });
  }

  // -> { doc, text }
  async get() {
    this.early = null;
    const done = this.waitFor();
    await this.write(frameGet());
    const ev = await done;
    const inc = this.incoming;
    this.incoming = null;
    if (ev.type !== 'end' || !inc) throw new Error(`unexpected reply: ${ev.type}`);
    if (inc.got !== inc.size || crc32(inc.buf) !== inc.crc) throw new Error('lists arrived damaged, try again');
    const text = dec.decode(inc.buf);
    return { doc: parseDoc(text), text };
  }

  // Sends doc, edited from revision baseRev. Resolves with the new revision.
  async put(doc, baseRev) {
    const problem = docProblem(doc);
    if (problem) throw new Error(problem);
    const bytes = enc.encode(serializeDoc(doc, baseRev));
    parseDoc(dec.decode(bytes)); // never send something the device would reject

    this.early = null;
    await this.write(framePutBegin(bytes.length, baseRev, crc32(bytes)));
    for (let off = 0; off < bytes.length; off += this.maxWrite) {
      await this.write(framePutData(off, bytes.subarray(off, off + this.maxWrite)));
    }
    const done = this.waitFor();
    await this.write(framePutCommit());
    const ev = await done;
    if (ev.type === 'put_err') throw new PutError(ev.code, ev.rev);
    if (ev.type !== 'put_ok') throw new Error(`unexpected reply: ${ev.type}`);
    return ev.rev;
  }
}
