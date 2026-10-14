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

import {afterEach, describe, expect, test, vi} from 'vitest';
import {serverHasFeature} from './server_capabilities';
import {endpointStorage} from './endpoint_storage';

afterEach(() => vi.unstubAllGlobals());

describe('serverHasFeature', () => {
  test('is false until `GET /` answers, then reflects `features`', async () => {
    const fetchMock = vi.fn(async () => ({
      ok: true,
      json: async () => ({features: ['duckdb']}),
    }));
    vi.stubGlobal('fetch', fetchMock);
    endpointStorage.get('bigtraceEndpoint')?.set('http://local/v1');

    expect(serverHasFeature('duckdb')).toBe(false);
    await vi.waitFor(() => expect(serverHasFeature('duckdb')).toBe(true));
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  test('a backend without features has none', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => ({ok: true, json: async () => ({})})),
    );
    endpointStorage.get('bigtraceEndpoint')?.set('https://remote/v1');
    serverHasFeature('duckdb');
    await new Promise((r) => setTimeout(r, 0));
    expect(serverHasFeature('duckdb')).toBe(false);
  });
});
