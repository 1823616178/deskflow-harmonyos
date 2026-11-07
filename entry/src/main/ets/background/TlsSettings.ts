// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
export function normalizeTlsFingerprint(value: string): string | null {
  const normalized = value.replace(/[: \t\r\n]/g, '').toUpperCase();
  return normalized.length === 0 || /^[0-9A-F]{64}$/.test(normalized) ? normalized : null;
}

export function tlsPreferenceKey(host: string, port: number): string {
  return `tls_peer_${encodeURIComponent(host.trim().toLowerCase())}_${port}`;
}

export interface TlsCertificateNotice {
  changed: boolean;
  fingerprint: string;
}

export function tlsCertificateNotice(message: string): TlsCertificateNotice | null {
  const match = /^error: TLS_(UNTRUSTED|PIN_MISMATCH) ([0-9A-F]{64})$/.exec(message);
  if (match === null) return null;
  return { changed: match[1] === 'PIN_MISMATCH', fingerprint: match[2] };
}
