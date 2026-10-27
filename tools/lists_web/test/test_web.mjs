// Tests for tools/lists_web/lists.js:
//  - the JS document parser/serializer agrees with the firmware's C parser (src/lists_doc.c)
//  - ListsClient talks the protocol correctly to a fake device that follows src/lists_ble.c
//
// Run: node tools/lists_web/test/test_web.mjs   (needs gcc on PATH for the C cross-check)

import { execFileSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as L from '../lists.js';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..', '..');
let failures = 0;
const check = (cond, msg) => {
  if (!cond) {
    failures++;
    console.log(`FAIL ${msg}`);
  }
};

// ------------------------------------------------------------ build the C checker

const exe = join(mkdtempSync(join(tmpdir(), 'lists-')), process.platform === 'win32' ? 'doc_test.exe' : 'doc_test');
execFileSync('gcc', ['-Wall', '-Wextra', '-Werror', '-std=c11', '-I', join(repo, 'src'), '-o', exe,
                     join(here, 'doc_test.c'), join(repo, 'src', 'lists_doc.c')]);
execFileSync(exe, [], { stdio: 'inherit' });
const cParse = (text) => execFileSync(exe, ['--check'], { input: Buffer.from(text, 'utf8') }).toString().trim();

function jsParse(text) {
  try {
    const d = L.parseDoc(text);
    const items = d.lists.flatMap((l) => l.items);
    return `ok ${d.rev} ${d.lists.length} ${items.length} ${items.filter((i) => i.checked).length}`;
  } catch (e) {
    if (e instanceof L.DocError) return 'err';
    throw e;
  }
}

function agree(text, label) {
  const c = cParse(text);
  const js = jsParse(text);
  const same = c.startsWith('err') ? js === 'err' : c === js;
  check(same, `${label}: C says "${c}", JS says "${js}" for ${JSON.stringify(text.slice(0, 80))}`);
}

// ------------------------------------------------------------ document format

const sample = '@rev 3\n# Groceries\n[ ] Milk\n[x] Eggs\n\n# To-do\n[ ] Call the bank\n';
const doc = L.parseDoc(sample);
check(doc.rev === 3 && doc.lists.length === 2 && doc.lists[0].items[1].checked, 'parse sample');
check(L.serializeDoc(doc) === sample.replace('\n\n', '\n'), 'serialize drops blank lines only');
check(L.serializeDoc(doc, 9).startsWith('@rev 9\n'), 'serialize with rev');

for (const [text, label] of [
  [sample, 'sample'], ['@rev 0', 'no newline'], ['@rev 7\n# Empty\n', 'empty list'],
  ['@rev 1\n# Café\n[ ] crème brûlée\n', 'utf-8'], ['', 'empty'], ['# G\n', 'no rev'],
  ['@rev x\n', 'bad rev'], ['@rev 99999999999\n', 'rev overflow'], ['@rev 1\n[ ] orphan\n', 'orphan'],
  ['@rev 1\n# L\nMilk\n', 'bad line'], ['@rev 1\n# L\n[X] Milk\n', 'capital X'], ['@rev 1\n# L\r\n', 'CR'],
  ['@rev 1\n# \n', 'empty name'], ['@rev 1\n# L\n[ ] \n', 'empty item'],
  [`@rev 1\n# ${'a'.repeat(96)}\n`, 'max name'], [`@rev 1\n# ${'a'.repeat(97)}\n`, 'long name'],
  [`@rev 1\n# ${'é'.repeat(48)}\n`, 'max name multibyte'], [`@rev 1\n# ${'é'.repeat(49)}\n`, 'long name multibyte'],
  ['@rev 1\n' + '# L\n'.repeat(17), 'too many lists'], ['@rev 1\n# L\n' + '[ ] i\n'.repeat(129), 'too many items'],
  ['@rev 1\n# L\n' + '[ ] i\n'.repeat(128), 'max items'],
]) agree(text, label);

// Random documents built the way the page builds them
let seed = 42;
const rand = (n) => { seed = (seed * 1103515245 + 12345) % 2 ** 31; return seed % n; };
const words = ['Milk', 'Eggs', 'Crème fraîche', 'Brød', 'Tea', 'Call mum', '"quoted"', 'x'.repeat(90), '日本茶', 'a  b'];
for (let i = 0; i < 200; i++) {
  const d = { rev: rand(100000), lists: [] };
  for (let l = 0, nl = rand(5); l < nl; l++) {
    const list = { name: L.cleanText(words[rand(words.length)] + ' ' + l), items: [] };
    for (let k = 0, ni = rand(12); k < ni; k++) {
      list.items.push({ text: L.cleanText(words[rand(words.length)] + '\n' + k), checked: !!rand(2) });
    }
    d.lists.push(list);
  }
  const text = L.serializeDoc(d);
  check(!L.docProblem(d) || L.docSize(d) > L.PUT_MAX, `random ${i}: docProblem flagged a valid doc`);
  agree(text, `random ${i}`);
  check(L.serializeDoc(L.parseDoc(text)) === text, `random ${i}: round trip`);
}

check(L.cleanText('  a\tb\nc  ') === 'a b c', 'cleanText whitespace');
check(new TextEncoder().encode(L.cleanText('é'.repeat(80))).length <= L.TEXT_MAX, 'cleanText length');
check(L.crc32(new TextEncoder().encode('123456789')) === 0xcbf43926, 'crc32 vector');

// ------------------------------------------------------------ protocol against a fake device

// Same rules as src/lists_ble.c + src/lists.c
class FakeDevice {
  constructor(text, maxWrite = 20) {
    this.text = text;
    this.maxWrite = maxWrite;
    this.rx = null;
    this.writes = [];
  }
  get rev() { return L.parseDoc(this.text).rev; }
  connect(client) { this.client = client; }
  notify(bytes) { queueMicrotask(() => this.client.onNotify(bytes)); }
  u16(b, o) { return b[o] | (b[o + 1] << 8); }
  u32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0; }
  ev(...bytes) { return new Uint8Array(bytes); }
  le(n, size) { return Array.from({ length: size }, (_, i) => (n >>> (8 * i)) & 0xff); }
  err(code) { this.notify(this.ev(0x94, code, ...this.le(this.rev, 4))); this.rx = null; }

  async write(f) {
    this.writes.push(f);
    check(f.length <= this.maxWrite + 3, `write of ${f.length} bytes exceeds the MTU`);
    if (f[0] === 0x10) {
      const bytes = new TextEncoder().encode(this.text);
      this.notify(this.ev(0x90, ...this.le(bytes.length, 2), ...this.le(this.rev, 4), ...this.le(L.crc32(bytes), 4),
                          ...this.le(this.maxWrite, 2)));
      for (let o = 0; o < bytes.length; o += this.maxWrite) {
        this.notify(this.ev(0x91, ...this.le(o, 2), ...bytes.subarray(o, o + this.maxWrite)));
      }
      this.notify(this.ev(0x92));
    } else if (f[0] === 0x11) {
      const size = this.u16(f, 1);
      if (size > L.PUT_MAX) return this.err(2);
      this.rx = { size, base: this.u32(f, 3), crc: this.u32(f, 7), buf: new Uint8Array(size), got: 0 };
    } else if (f[0] === 0x12) {
      const off = this.u16(f, 1);
      if (!this.rx || off !== this.rx.got) return this.err(1);
      this.rx.buf.set(f.subarray(3), off);
      this.rx.got += f.length - 3;
    } else if (f[0] === 0x13) {
      const r = this.rx;
      if (!r || r.got !== r.size) return this.err(1);
      if (L.crc32(r.buf) !== r.crc) return this.err(4);
      const text = new TextDecoder().decode(r.buf);
      if (cParse(text).startsWith('err')) return this.err(5);
      if (r.base !== this.rev) return this.err(3);
      this.text = text.replace(/^@rev \d+/, `@rev ${r.base + 1}`);
      this.rx = null;
      this.notify(this.ev(0x93, ...this.le(this.rev, 4)));
    }
  }

  // Simulates checking an item on the reader
  deviceEdit() {
    this.text = this.text.replace(/^@rev (\d+)/, (_, n) => `@rev ${Number(n) + 1}`).replace('[ ]', '[x]');
    this.notify(this.ev(0x95, ...this.le(this.rev, 4)));
  }
}

async function protocolTests() {
  const dev = new FakeDevice(sample, 20);
  const client = new L.ListsClient((b) => dev.write(b), { timeoutMs: 500 });
  dev.connect(client);
  let changedRev = null;
  client.onChanged = (rev) => { changedRev = rev; };

  const { doc: got } = await client.get();
  check(got.rev === 3 && got.lists[1].name === 'To-do', 'GET');
  check(client.maxWrite === 20, 'max write learned from DOC_BEGIN');

  got.lists[0].items.push({ text: 'Crème fraîche', checked: false });
  got.lists.push({ name: 'Hardware store', items: [{ text: 'M3 screws', checked: false }] });
  const rev = await client.put(got, got.rev);
  check(rev === 4 && dev.rev === 4, 'PUT accepted, revision bumped');
  check(L.parseDoc(dev.text).lists.length === 3, 'device has the new list');
  check(dev.writes.filter((w) => w[0] === 0x12).length > 3, 'PUT chunked to max write');

  // Device edits in between: stale PUT is a conflict
  const { doc: fresh } = await client.get();
  dev.deviceEdit();
  await new Promise((r) => setTimeout(r, 0));
  check(changedRev === 5, 'CHANGED notification');
  fresh.lists[0].name = 'Food';
  try {
    await client.put(fresh, fresh.rev);
    check(false, 'conflict not reported');
  } catch (e) {
    check(e instanceof L.PutError && e.code === L.ERR_CONFLICT, `conflict: ${e.message}`);
  }
  const { doc: again } = await client.get();
  again.lists[0].name = 'Food';
  check((await client.put(again, again.rev)) === 6, 'PUT after reload');

  // Client refuses invalid documents before sending anything
  const before = dev.writes.length;
  again.lists[0].items.push({ text: '', checked: false });
  try {
    await client.put(again, again.rev);
    check(false, 'empty item accepted');
  } catch (e) {
    check(dev.writes.length === before, `invalid doc not sent (${e.message})`);
  }

  // No device answer -> clear error
  const silent = new L.ListsClient(async () => {}, { timeoutMs: 50 });
  try {
    await silent.get();
    check(false, 'silent device');
  } catch (e) {
    check(/Lists screen/.test(e.message), 'timeout message');
  }
}

await protocolTests();
console.log(failures ? `${failures} FAILURES` : 'all lists web tests passed');
process.exit(failures ? 1 : 0);
