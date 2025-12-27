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

import {
  buildServerStates,
  fetchManifest,
  initializeExtensions,
} from './extension_server';

// =============================================================================
// Test Helpers
// =============================================================================

const mockFetch = jest.fn();
global.fetch = mockFetch as unknown as typeof fetch;

function mockJsonResponse(data: unknown, status = 200) {
  return Promise.resolve({
    ok: status >= 200 && status < 300,
    status,
    json: async () => data,
  } as Response);
}

function mockErrorResponse(status = 404) {
  return Promise.resolve({
    ok: false,
    status,
  } as Response);
}

// =============================================================================
// Tests
// =============================================================================

describe('extension_server', () => {
  beforeEach(() => {
    mockFetch.mockClear();
  });

  describe('buildServerStates', () => {
    test('handles empty server list', async () => {
      expect(await buildServerStates([])).toEqual([]);
    });

    test('builds state with manifest', async () => {
      mockFetch.mockImplementation(() =>
        mockJsonResponse({
          name: 'Test Server',
          modules: ['default', 'android'],
        }),
      );

      const states = await buildServerStates([
        {
          url: 'github://owner/repo/main',
          selectedModules: ['default'],
          enabled: true,
        },
      ]);

      expect(states[0]).toMatchObject({
        displayName: 'Test Server',
        availableModules: ['default', 'android'],
        lastFetchStatus: 'success',
      });
    });

    test('handles missing manifest', async () => {
      mockFetch.mockImplementation(() => mockErrorResponse(404));

      const states = await buildServerStates([
        {
          url: 'github://owner/repo/main',
          selectedModules: ['default'],
          enabled: true,
        },
      ]);

      expect(states[0]).toMatchObject({
        displayName: 'github://owner/repo/main',
        availableModules: [],
        lastFetchStatus: 'error',
      });
    });

    test('processes multiple servers in parallel', async () => {
      mockFetch.mockImplementation((url: string) => {
        if (url.includes('owner1')) {
          return mockJsonResponse({name: 'Server 1', modules: ['default']});
        }
        return mockJsonResponse({name: 'Server 2', modules: ['android']});
      });

      const states = await buildServerStates([
        {
          url: 'github://owner1/repo/main',
          selectedModules: ['default'],
          enabled: true,
        },
        {
          url: 'github://owner2/repo/main',
          selectedModules: ['android'],
          enabled: false,
        },
      ]);

      expect(states).toHaveLength(2);
      expect(states[0]?.displayName).toBe('Server 1');
      expect(states[1]?.displayName).toBe('Server 2');
    });
  });

  describe('fetchManifest', () => {
    test('validates manifest schema', async () => {
      const manifest = {
        name: 'Test',
        modules: ['default'],
        csp_allow: ['https://example.com'],
      };
      mockFetch.mockImplementation(() => mockJsonResponse(manifest));

      expect(await fetchManifest('https://server.com')).toEqual(manifest);
    });

    test('returns undefined for errors', async () => {
      mockFetch.mockImplementation(() => mockErrorResponse(404));
      expect(await fetchManifest('https://server.com')).toBeUndefined();

      mockFetch.mockImplementation(() => mockJsonResponse('invalid'));
      expect(await fetchManifest('https://server.com')).toBeUndefined();

      mockFetch.mockImplementation(() => mockJsonResponse({modules: ['test']})); // missing name
      expect(await fetchManifest('https://server.com')).toBeUndefined();
    });
  });

  describe('initializeExtensions', () => {
    test('returns empty promises for no servers', async () => {
      const result = await initializeExtensions([]);

      expect(result.states).toEqual([]);
      expect((await result.macrosPromise).size).toBe(0);
      expect((await result.sqlModulesPromise).size).toBe(0);
      expect((await result.protoDescriptorsPromise).size).toBe(0);
    });

    test('skips disabled servers', async () => {
      mockFetch.mockImplementation(() =>
        mockJsonResponse({name: 'Test', modules: ['default']}),
      );

      const result = await initializeExtensions([
        {
          url: 'github://owner/repo/main',
          selectedModules: ['default'],
          enabled: false,
        },
      ]);

      expect((await result.macrosPromise).size).toBe(0);
    });

    test('loads extensions with proper namespacing', async () => {
      mockFetch.mockImplementation((url: string) => {
        if (url.includes('manifest.json')) {
          return mockJsonResponse({name: 'Test', modules: ['default']});
        }
        if (url.includes('macros')) {
          return mockJsonResponse({
            'Startup Analysis': [
              {id: 'dev.perfetto.RunQuery', args: ['SELECT 1']},
            ],
          });
        }
        if (url.includes('sql_modules')) {
          return mockJsonResponse({'android.startup': 'CREATE TABLE...'});
        }
        if (url.includes('proto_descriptors')) {
          return mockJsonResponse({
            'android-protos': {name: 'Android', descriptor: 'base64'},
          });
        }
        return mockErrorResponse(404);
      });

      const result = await initializeExtensions([
        {
          url: 'github://owner/repo/main',
          selectedModules: ['default'],
          enabled: true,
        },
      ]);

      const macros = await result.macrosPromise;
      const sqlModules = await result.sqlModulesPromise;
      const protos = await result.protoDescriptorsPromise;

      expect(
        macros.has(
          '[raw-githubusercontent-com-owner-repo-main default] Startup Analysis',
        ),
      ).toBe(true);
      expect(sqlModules.get('android.startup')).toBe('CREATE TABLE...');
      expect(protos.get('android-protos')).toEqual({
        name: 'Android',
        descriptor: 'base64',
      });
    });

    test('handles fetch failures gracefully', async () => {
      mockFetch.mockImplementation((url: string) => {
        if (url.includes('manifest.json')) {
          return mockJsonResponse({name: 'Test', modules: ['default']});
        }
        return mockErrorResponse(500);
      });

      const result = await initializeExtensions([
        {
          url: 'github://owner/repo/main',
          selectedModules: ['default'],
          enabled: true,
        },
      ]);

      expect((await result.macrosPromise).size).toBe(0);
      expect((await result.sqlModulesPromise).size).toBe(0);
      expect((await result.protoDescriptorsPromise).size).toBe(0);
    });
  });
});
