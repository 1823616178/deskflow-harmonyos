// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
import assert from 'node:assert/strict';
import { normalizeTlsFingerprint, tlsPreferenceKey, tlsCertificateNotice }
  from '../../entry/src/main/ets/background/TlsSettings.ts';

const pin = 'AB'.repeat(32);
assert.equal(normalizeTlsFingerprint('ab:'.repeat(31) + 'ab'), pin);
assert.equal(normalizeTlsFingerprint('  '), '');
for (const value of ['A'.repeat(63), 'A'.repeat(65), 'G'.repeat(64), 'SHA256:' + pin, pin + '\0'])
  assert.equal(normalizeTlsFingerprint(value), null);
assert.equal(tlsPreferenceKey(' HOST ', 24800), tlsPreferenceKey('host', 24800));
assert.notEqual(tlsPreferenceKey('host', 24800), tlsPreferenceKey('host', 24801));
assert.notEqual(tlsPreferenceKey('host', 24800), tlsPreferenceKey('other', 24800));
assert.deepEqual(tlsCertificateNotice('error: TLS_UNTRUSTED ' + pin), { changed: false, fingerprint: pin });
assert.deepEqual(tlsCertificateNotice('error: TLS_PIN_MISMATCH ' + pin), { changed: true, fingerprint: pin });
assert.equal(tlsCertificateNotice('error: TLS_UNTRUSTED ' + pin + 'extra'), null);
assert.equal(tlsCertificateNotice('other TLS_UNTRUSTED ' + pin), null);
console.log('tls_settings_test: 14 checks, 0 failures');
