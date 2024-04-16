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

import {isTrustedOrigin} from './post_message_handler';

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
    expect(isTrustedOrigin('https://localhost')).toBeTruthy();
    expect(isTrustedOrigin('http://localhost')).toBeTruthy();
    expect(isTrustedOrigin('https://127.0.0.1')).toBeTruthy();
    expect(isTrustedOrigin('http://127.0.0.1')).toBeTruthy();
    // IPv6 localhost
    expect(isTrustedOrigin('https://[::1]')).toBeTruthy();
    expect(isTrustedOrigin('http://[::1]')).toBeTruthy();
  });
});
