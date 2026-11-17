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

import {getPerfettoEnv} from './perfetto_env';

describe('getPerfettoEnv', () => {
  test('should return prod for ui.perfetto.dev', () => {
    expect(getPerfettoEnv(new URL('https://ui.perfetto.dev/#!/viewer'))).toBe(
      'prod',
    );
  });

  test('should return localhost for local builds', () => {
    expect(getPerfettoEnv(new URL('http://localhost:10000/'))).toBe(
      'localhost',
    );
    expect(getPerfettoEnv(new URL('http://127.0.0.1:10000/'))).toBe(
      'localhost',
    );
  });

  test('should return pr_preview for pull request previews', () => {
    const url =
      'https://storage.googleapis.com/perfetto-ci-artifacts/gh-123-1-ui/ui/index.html';
    expect(getPerfettoEnv(new URL(url))).toBe('pr_preview');
  });

  test('should return other for anything else', () => {
    // Other hosts, including other subdomains of perfetto.dev.
    expect(getPerfettoEnv(new URL('https://example.com/'))).toBe('other');
    expect(getPerfettoEnv(new URL('https://foo.perfetto.dev/'))).toBe('other');
    // Other buckets on storage.googleapis.com.
    expect(
      getPerfettoEnv(new URL('https://storage.googleapis.com/other-bucket/')),
    ).toBe('other');
  });
});
