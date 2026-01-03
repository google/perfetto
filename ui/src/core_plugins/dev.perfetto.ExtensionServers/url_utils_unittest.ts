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

import {normalizeServerKey, resolveServerUrl} from './url_utils';

describe('resolveServerUrl', () => {
  test('resolves github:// URLs', () => {
    expect(resolveServerUrl('github://owner/repo/main')).toEqual(
      'https://raw.githubusercontent.com/owner/repo/main',
    );
    expect(resolveServerUrl('github://owner/repo/main/path/to/dir')).toEqual(
      'https://raw.githubusercontent.com/owner/repo/main/path/to/dir',
    );
  });

  test('resolves gs:// URLs', () => {
    expect(resolveServerUrl('gs://my-bucket/path/to/file')).toEqual(
      'https://storage.googleapis.com/my-bucket/path/to/file',
    );
  });

  test('resolves s3:// URLs', () => {
    expect(resolveServerUrl('s3://my-bucket/path/to/file')).toEqual(
      'https://my-bucket.s3.amazonaws.com/path/to/file',
    );
  });

  test('passes through https:// URLs', () => {
    const url = 'https://perfetto.corp.example.com/extensions';
    expect(resolveServerUrl(url)).toEqual(url);
  });

  test('upgrades http:// to https://', () => {
    expect(resolveServerUrl('http://example.com/path')).toEqual(
      'https://example.com/path',
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

  test('throws on s3:// with bucket but no path', () => {
    expect(() => resolveServerUrl('s3://my-bucket')).toThrow(
      'Invalid S3 URL: missing path after bucket',
    );
  });

  test('throws on invalid protocol', () => {
    expect(() => resolveServerUrl('ftp://example.com')).toThrow(
      'Invalid server URL: must start with https://, github://, gs://, or s3://',
    );
  });
});

describe('normalizeServerKey', () => {
  test('normalizes simple domain', () => {
    expect(normalizeServerKey('https://perfetto.acme.com')).toEqual(
      'perfetto-acme-com',
    );
  });

  test('normalizes domain with port and path', () => {
    expect(normalizeServerKey('https://corp.example.com:8443/modules')).toEqual(
      'corp-example-com-8443-modules',
    );
  });

  test('normalizes GitHub URL', () => {
    expect(
      normalizeServerKey(
        'https://raw.githubusercontent.com/acme/perfetto-ext/main',
      ),
    ).toEqual('raw-githubusercontent-com-acme-perfetto-ext-main');
  });

  test('normalizes GCS URL', () => {
    expect(
      normalizeServerKey('https://storage.googleapis.com/my-bucket/extensions'),
    ).toEqual('storage-googleapis-com-my-bucket-extensions');
  });

  test('normalizes S3 URL', () => {
    expect(
      normalizeServerKey('https://my-bucket.s3.amazonaws.com/extensions'),
    ).toEqual('my-bucket-s3-amazonaws-com-extensions');
  });

  test('handles special characters', () => {
    expect(normalizeServerKey('https://my_server.example.com')).toEqual(
      'my-server-example-com',
    );
    expect(normalizeServerKey('https://server---example.com')).toEqual(
      'server-example-com',
    );
  });

  test('removes query strings and fragments', () => {
    expect(normalizeServerKey('https://example.com/api?v=1#docs')).toEqual(
      'example-com-api',
    );
  });

  test('lowercases mixed case URLs', () => {
    expect(normalizeServerKey('https://Example.Com/Path')).toEqual(
      'example-com-path',
    );
  });

  test('throws on non-https URL', () => {
    expect(() => normalizeServerKey('http://example.com')).toThrow(
      'Server key normalization requires canonical HTTPS URL',
    );
  });
});
