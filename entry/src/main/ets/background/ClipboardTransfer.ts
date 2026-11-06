// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

// Leave room for the 12-byte IClipboard container in the protocol limit.
export const MAX_CLIPBOARD_BYTES = 1024 * 1024 - 12;
export const CLIPBOARD_CHUNK_CHARS = 4096;
export const MAX_CLIPBOARD_CHUNKS = 256;

export interface ClipboardChunk {
  clipChunk: boolean;
  id: string;
  index: number;
  total: number;
  data: string;
}

interface ClipboardAssembly {
  parts: string[];
  seen: boolean[];
  total: number;
  received: number;
  bytes: number;
  startedAt: number;
}

export function utf8ByteLength(text: string): number {
  let bytes = 0;
  for (let i = 0; i < text.length; i++) {
    const code = text.charCodeAt(i);
    if (code < 0x80) bytes++;
    else if (code < 0x800) bytes += 2;
    else if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.length &&
      text.charCodeAt(i + 1) >= 0xDC00 && text.charCodeAt(i + 1) <= 0xDFFF) {
      bytes += 4;
      i++;
    } else bytes += 3;
  }
  return bytes;
}

export function splitClipboardText(text: string, id: string): ClipboardChunk[] {
  if (utf8ByteLength(text) > MAX_CLIPBOARD_BYTES || !id || id.length > 128) {
    throw new Error('剪贴板文本过大（最多约 1MB）或传输编号无效');
  }
  const parts: string[] = [];
  for (let offset = 0; offset < text.length;) {
    let end = Math.min(offset + CLIPBOARD_CHUNK_CHARS, text.length);
    // Do not split a UTF-16 surrogate pair between JSON messages.
    if (end < text.length && text.charCodeAt(end - 1) >= 0xD800 &&
      text.charCodeAt(end - 1) <= 0xDBFF && text.charCodeAt(end) >= 0xDC00 &&
      text.charCodeAt(end) <= 0xDFFF) end--;
    parts.push(text.substring(offset, end));
    offset = end;
  }
  if (parts.length === 0) parts.push('');
  const result: ClipboardChunk[] = [];
  for (let index = 0; index < parts.length; index++) {
    // Always use an envelope: user text that happens to be JSON must
    // never be interpreted as a transport header.
    result.push({ clipChunk: true, id: id, index: index, total: parts.length, data: parts[index] });
  }
  return result;
}

export class ClipboardAssembler {
  private transfers: Map<string, ClipboardAssembly> = new Map<string, ClipboardAssembly>();

  accept(chunk: ClipboardChunk, now: number = Date.now()): string | null {
    this.transfers.forEach((entry, id) => {
      if (now - entry.startedAt >= 30000) this.transfers.delete(id);
    });
    if (!chunk || chunk.clipChunk !== true || typeof chunk.id !== 'string' ||
      !chunk.id || chunk.id.length > 128 || !Number.isInteger(chunk.index) ||
      !Number.isInteger(chunk.total) || chunk.total < 1 || chunk.total > MAX_CLIPBOARD_CHUNKS ||
      chunk.index < 0 || chunk.index >= chunk.total || typeof chunk.data !== 'string' ||
      chunk.data.length > CLIPBOARD_CHUNK_CHARS) return null;
    let entry = this.transfers.get(chunk.id);
    if (entry && entry.total !== chunk.total) {
      this.transfers.delete(chunk.id);
      return null;
    }
    if (!entry) {
      if (this.transfers.size >= 4) return null;
      entry = { parts: new Array<string>(chunk.total).fill(''),
        seen: new Array<boolean>(chunk.total).fill(false), total: chunk.total,
        received: 0, bytes: 0, startedAt: now };
      this.transfers.set(chunk.id, entry);
    }
    if (entry.seen[chunk.index]) {
      if (entry.parts[chunk.index] !== chunk.data) this.transfers.delete(chunk.id);
      return null;
    }
    entry.bytes += utf8ByteLength(chunk.data);
    if (entry.bytes > MAX_CLIPBOARD_BYTES) {
      this.transfers.delete(chunk.id);
      return null;
    }
    entry.seen[chunk.index] = true;
    entry.parts[chunk.index] = chunk.data;
    entry.received++;
    if (entry.received !== entry.total) return null;
    this.transfers.delete(chunk.id);
    return entry.parts.join('');
  }

  clear(): void {
    this.transfers.clear();
  }
}
