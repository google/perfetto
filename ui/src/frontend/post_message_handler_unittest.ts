// Copyright (C) 2024 The Android Open Source Project
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

import {isTrustedOrigin, parsePostedTrace} from './post_message_handler';

describe('postMessageHandler', () => {
  test('baked-in trusted origins are trusted', () => {
    expect(isTrustedOrigin('https://chrometto.googleplex.com')).toBeTruthy();
    expect(isTrustedOrigin('https://uma.googleplex.com')).toBeTruthy();
    expect(
      isTrustedOrigin('https://android-build.googleplex.com'),
    ).toBeTruthy();
    expect(isTrustedOrigin('https://html5zombo.com')).toBeFalsy();
  });

  test('user trusted origins in local storage are trusted', () => {
    try {
      expect(isTrustedOrigin('https://html5zombo.com')).toBeFalsy();
      window.localStorage['trustedOrigins'] = '["https://html5zombo.com"]';
      expect(isTrustedOrigin('https://html5zombo.com')).toBeTruthy();
    } finally {
      window.localStorage.clear();
    }
  });

  test('developer hostnames are trusted', () => {
    expect(isTrustedOrigin('https://google.com')).toBeFalsy();
    expect(isTrustedOrigin('https://broccoliman.corp.google.com')).toBeTruthy();
    expect(isTrustedOrigin('http://broccoliman.corp.google.com')).toBeTruthy();
    expect(isTrustedOrigin('https://broccoliman.c.googlers.com')).toBeTruthy();
    expect(isTrustedOrigin('http://broccoliman.c.googlers.com')).toBeTruthy();
    expect(isTrustedOrigin('https://broccolimancorp.google.com')).toBeFalsy();
    expect(isTrustedOrigin('https://broccolimanc.googlers.com')).toBeFalsy();
    expect(
      isTrustedOrigin('https://b1234567890abcdef.proxy.googlers.com'),
    ).toBeTruthy();
    expect(
      isTrustedOrigin('https://b1234567890abcdefproxy.googlers.com'),
    ).toBeFalsy();
    expect(isTrustedOrigin('https://localhost')).toBeTruthy();
    expect(isTrustedOrigin('http://localhost')).toBeTruthy();
    expect(isTrustedOrigin('https://127.0.0.1')).toBeTruthy();
    expect(isTrustedOrigin('http://127.0.0.1')).toBeTruthy();
    // IPv6 localhost
    expect(isTrustedOrigin('https://[::1]')).toBeTruthy();
    expect(isTrustedOrigin('http://[::1]')).toBeTruthy();
  });
});

describe('parsePostedTrace', () => {
  describe('flat buffer', () => {
    test('arraybuffer returned verbatim', () => {
      const buffer = new ArrayBuffer();
      const result = parsePostedTrace(buffer);
      expect(result?.buffer).toBe(buffer);
    });

    test('view converted to arraybuffer', () => {
      const result = parsePostedTrace(new Uint8Array());
      expect(result?.buffer).toBeInstanceOf(ArrayBuffer);
    });

    test('view is snipped to the view, not the underlying buffer', () => {
      // A 10-byte view starting at offset 2 of a 16-byte buffer.
      const underlying = new Uint8Array(16);
      underlying.forEach((_, i) => (underlying[i] = i));
      const view = new Uint8Array(underlying.buffer, 2, 10);

      const result = parsePostedTrace(view);

      expect(result?.buffer).toBeInstanceOf(ArrayBuffer);
      // Spans exactly the view's bytes, not the full 16-byte buffer.
      expect(result?.buffer.byteLength).toBe(10);
      expect(new Uint8Array(result!.buffer)).toEqual(
        new Uint8Array([2, 3, 4, 5, 6, 7, 8, 9, 10, 11]),
      );
    });
  });

  describe('wrapped trace', () => {
    test('arraybuffer returned verbatim', () => {
      const buffer = new ArrayBuffer();
      const result = parsePostedTrace({perfetto: {buffer, title: 'foo'}});
      expect(result?.buffer).toBe(buffer);
    });

    test('view converted to arraybuffer', () => {
      const result = parsePostedTrace({
        perfetto: {buffer: new Uint8Array(), title: 'foo'},
      });
      expect(result?.buffer).toBeInstanceOf(ArrayBuffer);
    });

    test('file name is preserved and sanitized', () => {
      const result = parsePostedTrace({
        perfetto: {
          buffer: new ArrayBuffer(),
          title: 'foo',
          fileName: 'my<trace>.pftrace',
        },
      });
      expect(result?.fileName).toBe('my trace .pftrace');
    });

    test('view is snipped to the view, not the underlying buffer', () => {
      // A 10-byte view starting at offset 2 of a 16-byte buffer.
      const underlying = new Uint8Array(16);
      underlying.forEach((_, i) => (underlying[i] = i));
      const view = new Uint8Array(underlying.buffer, 2, 10);

      const result = parsePostedTrace({
        perfetto: {buffer: view, title: 'foo'},
      });

      expect(result?.buffer).toBeInstanceOf(ArrayBuffer);
      // Spans exactly the view's bytes, not the full 16-byte buffer.
      expect(result?.buffer.byteLength).toBe(10);
      expect(new Uint8Array(result!.buffer)).toEqual(
        new Uint8Array([2, 3, 4, 5, 6, 7, 8, 9, 10, 11]),
      );
    });
  });
});
