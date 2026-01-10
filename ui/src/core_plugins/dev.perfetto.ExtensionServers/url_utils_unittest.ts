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

import {resolveServerUrl} from './url_utils';

describe('resolveServerUrl', () => {
  test('resolves github:// URLs', () => {
    expect(resolveServerUrl('github://owner/repo/main')).toEqual(
      'https://raw.githubusercontent.com/owner/repo/main',
    );
    expect(resolveServerUrl('github://owner/repo/main/path/to/dir')).toEqual(
      'https://raw.githubusercontent.com/owner/repo/main/path/to/dir',
    );
  });

  test('passes through https:// URLs', () => {
    const url = 'https://perfetto.corp.example.com/extensions';
    expect(resolveServerUrl(url)).toEqual(url);
  });

  test('throws on http:// URL', () => {
    expect(() => resolveServerUrl('http://example.com/path')).toThrow(
      'Invalid server URL: http:// is not supported, use https:// instead',
    );
  });

  test('handles whitespace', () => {
    expect(resolveServerUrl('  github://owner/repo/main  ')).toEqual(
      'https://raw.githubusercontent.com/owner/repo/main',
    );
  });

  test('throws on empty github:// URL', () => {
    expect(() => resolveServerUrl('github://')).toThrow(
      'Invalid GitHub URL: missing owner/repo/ref',
    );
  });

  test('throws on invalid protocol', () => {
    expect(() => resolveServerUrl('ftp://example.com')).toThrow(
      'Invalid server URL: must start with https:// or github://',
    );
  });
});
