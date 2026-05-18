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

import {AddExtensionServerModal} from './add_extension_server_modal';
import type {ExtensionServer} from './types';

const mockFetch = jest.fn();
global.fetch = mockFetch as unknown as typeof fetch;

function mockJsonResponse(data: unknown, status = 200) {
  return Promise.resolve({
    ok: status >= 200 && status < 300,
    status,
    json: async () => data,
    clone() {
      return this;
    },
  } as unknown as Response);
}

// Flushes microtasks and any timers so AsyncLimiter-scheduled work can run.
async function flushAsync() {
  for (let i = 0; i < 5; i++) {
    await Promise.resolve();
  }
}

const PREFILL: ExtensionServer = {
  type: 'https',
  url: 'https://example.com/api',
  enabledModules: [],
  enabled: true,
  auth: {type: 'https_sso'},
  origin: 'user_added',
};

const EXISTING: ExtensionServer = {
  type: 'https',
  url: 'https://example.com',
  enabledModules: ['default'],
  enabled: true,
  auth: {type: 'none'},
  origin: 'user_added',
};

describe('AddExtensionServerModal', () => {
  beforeEach(() => {
    mockFetch.mockClear();
    mockFetch.mockImplementation(() =>
      mockJsonResponse({
        name: 'X',
        namespace: 'x',
        features: [],
        modules: [],
      }),
    );
  });

  test('does not fetch when opened with prefill only', async () => {
    new AddExtensionServerModal(undefined, PREFILL);
    await flushAsync();
    expect(mockFetch).not.toHaveBeenCalled();
  });

  test('fetches when opened with an existing server', async () => {
    new AddExtensionServerModal(EXISTING);
    await flushAsync();
    expect(mockFetch).toHaveBeenCalled();
  });

  test('confirmLoad triggers the deferred fetch', async () => {
    const modal = new AddExtensionServerModal(undefined, PREFILL);
    await flushAsync();
    expect(mockFetch).not.toHaveBeenCalled();

    (modal as unknown as {confirmLoad(): void}).confirmLoad();
    await flushAsync();
    expect(mockFetch).toHaveBeenCalled();
  });
});
