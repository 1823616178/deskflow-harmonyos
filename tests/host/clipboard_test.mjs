// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Node.js 24+ runs the shared, SDK-independent TypeScript directly.
import assert from 'node:assert/strict';
import { ClipboardAssembler, splitClipboardText, utf8ByteLength,
  MAX_CLIPBOARD_BYTES, CLIPBOARD_CHUNK_CHARS } from '../../entry/src/main/ets/background/ClipboardTransfer.ts';

let checks = 0;
function check(condition) { assert.ok(condition); checks++; }
function assemble(text, id) {
  const chunks = splitClipboardText(text, id);
  const assembler = new ClipboardAssembler();
  let result = null;
  for (const chunk of chunks.toReversed()) {
    const encoded = JSON.stringify(chunk);
    check(Buffer.byteLength(encoded) < 65536);
    check(chunk.data.length <= CLIPBOARD_CHUNK_CHARS);
    result = assembler.accept(JSON.parse(encoded), 1000);
  }
  check(result === text);
}

for (const [index, text] of [
  '', 'short text', '{"clipChunk":true,"id":"ordinary user text"}',
  '中文测试'.repeat(70000), '🙂'.repeat(120000),
  '\u0000\n"\\'.repeat(50000),
  'x'.repeat(CLIPBOARD_CHUNK_CHARS - 1) + '🙂' + 'tail',
  'x'.repeat(MAX_CLIPBOARD_BYTES)
].entries()) {
  check(utf8ByteLength(text) === Buffer.byteLength(text));
  assemble(text, `roundtrip-${index}`);
}
assert.throws(() => splitClipboardText('x'.repeat(MAX_CLIPBOARD_BYTES + 1), 'large'));
checks++;

const chunks = splitClipboardText('x'.repeat(CLIPBOARD_CHUNK_CHARS + 1), 'duplicate');
const duplicate = new ClipboardAssembler();
check(duplicate.accept(chunks[0], 0) === null);
check(duplicate.accept(chunks[0], 1) === null);
check(duplicate.accept(chunks[1], 2) === 'x'.repeat(CLIPBOARD_CHUNK_CHARS + 1));

const mismatched = new ClipboardAssembler();
check(mismatched.accept(chunks[0], 0) === null);
check(mismatched.accept({ ...chunks[1], total: 3 }, 1) === null);
check(mismatched.accept(chunks[1], 2) === null);

const expired = new ClipboardAssembler();
check(expired.accept(chunks[0], 0) === null);
check(expired.accept(chunks[1], 30000) === null);
for (const packet of [
  { ...chunks[0], total: 0 }, { ...chunks[0], total: 257 },
  { ...chunks[0], index: -1 }, { ...chunks[0], index: 0.5 },
  { ...chunks[0], data: 'x'.repeat(CLIPBOARD_CHUNK_CHARS + 1) },
  { ...chunks[0], id: '' }, { ...chunks[0], clipChunk: false }, null
]) check(new ClipboardAssembler().accept(packet, 0) === null);

// Incomplete transfers are bounded, and expiry makes room again.
const bounded = new ClipboardAssembler();
for (let id = 0; id < 4; id++) bounded.accept({ ...chunks[0], id: `${id}` }, 0);
check(bounded.accept({ ...chunks[0], id: 'fifth' }, 1) === null);
check(bounded.accept({ ...chunks[1], id: 'fifth' }, 2) === null);
check(bounded.accept({ ...chunks[0], id: 'fifth' }, 30001) === null);
check(bounded.accept({ ...chunks[1], id: 'fifth' }, 30002) === 'x'.repeat(CLIPBOARD_CHUNK_CHARS + 1));
console.log(`clipboard_test: ${checks} checks, 0 failures`);
