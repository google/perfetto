// Copyright (C) 2022 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {BigInteger, RSAKey} from 'jsbn-rsa';
import {assertExists, assertTrue} from '../../../../base/logging';
import {
  base64Decode,
  base64Encode,
  hexEncode,
} from '../../../../base/string_utils';

const WORD_SIZE = 4;
const MODULUS_SIZE_BITS = 2048;
const MODULUS_SIZE = MODULUS_SIZE_BITS / 8;
const MODULUS_SIZE_WORDS = MODULUS_SIZE / WORD_SIZE;
const PUBKEY_ENCODED_SIZE = 3 * WORD_SIZE + 2 * MODULUS_SIZE;
const ADB_WEB_CRYPTO_ALGORITHM = {
  name: 'RSASSA-PKCS1-v1_5',
  hash: {name: 'SHA-1'},
  publicExponent: new Uint8Array([0x01, 0x00, 0x01]), // 65537
  modulusLength: MODULUS_SIZE_BITS,
};

const ADB_WEB_CRYPTO_EXPORTABLE = true;
const ADB_WEB_CRYPTO_OPERATIONS: KeyUsage[] = ['sign'];

const SIGNING_ASN1_PREFIX = [
  0x00, 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05,
  0x00, 0x04, 0x14,
];

const R32 = BigInteger.ONE.shiftLeft(32); // 1 << 32

interface ValidJsonWebKey {
  n: string;
  e: string;
  d: string;
  p: string;
  q: string;
  dp: string;
  dq: string;
  qi: string;
}

export class AdbKey {
  // We use this JsonWebKey to:
  // - create a private key and sign with it
  // - create a public key and send it to the device
  // - serialize the JsonWebKey and send it to the device (or retrieve it
  // from the device and deserialize)
  jwkPrivate: ValidJsonWebKey;

  static deserialize(serializedKey: string): AdbKey {
    return new AdbKey(JSON.parse(serializedKey));
  }

  private constructor(jwkPrivate: ValidJsonWebKey) {
    this.jwkPrivate = jwkPrivate;
  }

  static async generateNewKeyPair(): Promise<AdbKey> {
    // Construct a new CryptoKeyPair and keep its private key in JWB format.
    const keyPair = await crypto.subtle.generateKey(
      ADB_WEB_CRYPTO_ALGORITHM,
      ADB_WEB_CRYPTO_EXPORTABLE,
      ADB_WEB_CRYPTO_OPERATIONS,
    );
    const jwkPrivate = await crypto.subtle.exportKey('jwk', keyPair.privateKey);
    if (!isValidJsonWebKey(jwkPrivate)) {
      throw new Error('Could not generate a valid ADB private key');
    }
    return new AdbKey(jwkPrivate);
  }

  // Perform an RSA signing operation for the ADB auth challenge.
  //
  // For the RSA signature, the token is expected to have already
  // had the SHA-1 message digest applied.
  //
  // However, the adb token we receive from the device is made up of 20 randomly
  // generated bytes that are treated like a SHA-1. Therefore, we need to update
  // the message format.
  sign(token: Uint8Array): Uint8Array {
    const rsaKey = new RSAKey();
    rsaKey.setPrivateEx(
      hexEncode(base64Decode(this.jwkPrivate.n)),
      hexEncode(base64Decode(this.jwkPrivate.e)),
      hexEncode(base64Decode(this.jwkPrivate.d)),
      hexEncode(base64Decode(this.jwkPrivate.p)),
      hexEncode(base64Decode(this.jwkPrivate.q)),
      hexEncode(base64Decode(this.jwkPrivate.dp)),
      hexEncode(base64Decode(this.jwkPrivate.dq)),
      hexEncode(base64Decode(this.jwkPrivate.qi)),
    );
    assertTrue(rsaKey.n.bitLength() === MODULUS_SIZE_BITS);

    // Message Layout (size equals that of the key modulus):
    // 00 01 FF FF FF FF ... FF [ASN.1 PREFIX] [TOKEN]
    const message = new Uint8Array(MODULUS_SIZE);

    // Initially fill the buffer with the padding
    message.fill(0xff);

    // add prefix
    message[0] = 0x00;
    message[1] = 0x01;

    // add the ASN.1 prefix
    message.set(
      SIGNING_ASN1_PREFIX,
      message.length - SIGNING_ASN1_PREFIX.length - token.length,
    );

    // then the actual token at the end
    message.set(token, message.length - token.length);

    const messageInteger = new BigInteger(Array.from(message));
    const signature = rsaKey.doPrivate(messageInteger);
    return new Uint8Array(bigIntToFixedByteArray(signature, MODULUS_SIZE));
  }

  // Construct public key to match the adb format:
  // go/codesearch/rvc-arc/system/core/libcrypto_utils/android_pubkey.c;l=38-53
  getPublicKey(): string {
    const rsaKey = new RSAKey();
    rsaKey.setPublic(
      hexEncode(base64Decode(assertExists(this.jwkPrivate.n))),
      hexEncode(base64Decode(assertExists(this.jwkPrivate.e))),
    );

    const n0inv = R32.subtract(rsaKey.n.modInverse(R32)).intValue();
    const r = BigInteger.ONE.shiftLeft(1).pow(MODULUS_SIZE_BITS);
    const rr = r.multiply(r).mod(rsaKey.n);

    const buffer = new ArrayBuffer(PUBKEY_ENCODED_SIZE);
    const dv = new DataView(buffer);
    dv.setUint32(0, MODULUS_SIZE_WORDS, true);
    dv.setUint32(WORD_SIZE, n0inv, true);

    const dvU8 = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
    dvU8.set(
      bigIntToFixedByteArray(rsaKey.n, MODULUS_SIZE).reverse(),
      2 * WORD_SIZE,
    );
    dvU8.set(
      bigIntToFixedByteArray(rr, MODULUS_SIZE).reverse(),
      2 * WORD_SIZE + MODULUS_SIZE,
    );

    dv.setUint32(2 * WORD_SIZE + 2 * MODULUS_SIZE, rsaKey.e, true);
    return base64Encode(dvU8) + ' ui.perfetto.dev';
  }

  serialize(): string {
    return JSON.stringify(this.jwkPrivate);
  }
}

function isValidJsonWebKey(key: JsonWebKey): key is ValidJsonWebKey {
  return (
    key.n !== undefined &&
    key.e !== undefined &&
    key.d !== undefined &&
    key.p !== undefined &&
    key.q !== undefined &&
    key.dp !== undefined &&
    key.dq !== undefined &&
    key.qi !== undefined
  );
}

// Convert a BigInteger to an array of a specified size in bytes.
function bigIntToFixedByteArray(bn: BigInteger, size: number): Uint8Array {
  const paddedBnBytes = bn.toByteArray();
  let firstNonZeroIndex = 0;
  while (
    firstNonZeroIndex < paddedBnBytes.length &&
    paddedBnBytes[firstNonZeroIndex] === 0
  ) {
    firstNonZeroIndex++;
  }
  const bnBytes = Uint8Array.from(paddedBnBytes.slice(firstNonZeroIndex));
  const res = new Uint8Array(size);
  assertTrue(bnBytes.length <= res.length);
  res.set(bnBytes, res.length - bnBytes.length);
  return res;
}
