// Copyright (C) 2026 The Android Open Source Project
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

import {makeDisplayUrl} from './url_utils';

describe('makeDisplayUrl', () => {
  test('formats GitHub server as repo @ ref', () => {
    expect(
      makeDisplayUrl({
        type: 'github',
        repo: 'owner/repo',
        ref: 'main',
        path: '/',
        auth: {type: 'none'},
      }),
    ).toEqual('owner/repo @ main');
  });

  test('includes path when not root', () => {
    expect(
      makeDisplayUrl({
        type: 'github',
        repo: 'owner/repo',
        ref: 'main',
        path: '/subdir',
        auth: {type: 'none'},
      }),
    ).toEqual('owner/repo:/subdir @ main');
  });

  test('strips https:// from HTTPS server URL', () => {
    expect(
      makeDisplayUrl({
        type: 'https',
        url: 'https://example.com/extensions',
        auth: {type: 'none'},
      }),
    ).toEqual('example.com/extensions');
  });

  test('strips http:// from HTTPS server URL', () => {
    expect(
      makeDisplayUrl({
        type: 'https',
        url: 'http://example.com/extensions',
        auth: {type: 'none'},
      }),
    ).toEqual('example.com/extensions');
  });

  test('passes through URL without protocol prefix', () => {
    expect(
      makeDisplayUrl({
        type: 'https',
        url: 'example.com/extensions',
        auth: {type: 'none'},
      }),
    ).toEqual('example.com/extensions');
  });
});
