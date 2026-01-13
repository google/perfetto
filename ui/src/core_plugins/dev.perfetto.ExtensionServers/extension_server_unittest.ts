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

import {loadManifest, initializeExtensions} from './extension_server';
import {AppImpl} from '../../core/app_impl';
import {Macro} from '../../core/command_manager';
import {SqlPackage} from '../../public/extra_sql_packages';

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

// Mock AppImpl for testing initializeExtensions
function createMockAppImpl() {
  const macrosAdded: Array<Promise<ReadonlyArray<Macro>>> = [];
  const sqlPackagesAdded: Array<Promise<ReadonlyArray<SqlPackage>>> = [];
  const protoDescriptorsAdded: Array<Promise<ReadonlyArray<string>>> = [];

  return {
    addMacros: jest.fn((p: Promise<ReadonlyArray<Macro>>) => {
      macrosAdded.push(p);
    }),
    addSqlPackages: jest.fn((p: Promise<ReadonlyArray<SqlPackage>>) => {
      sqlPackagesAdded.push(p);
    }),
    addProtoDescriptors: jest.fn((p: Promise<ReadonlyArray<string>>) => {
      protoDescriptorsAdded.push(p);
    }),
    getMacrosAdded: () => macrosAdded,
    getSqlPackagesAdded: () => sqlPackagesAdded,
    getProtoDescriptorsAdded: () => protoDescriptorsAdded,
  };
}

// =============================================================================
// Tests
// =============================================================================

describe('extension_server', () => {
  beforeEach(() => {
    mockFetch.mockClear();
  });

  describe('loadManifest', () => {
    test('validates manifest schema', async () => {
      const manifest = {
        name: 'Test',
        namespace: 'test',
        features: ['macros', 'sql_modules'],
        modules: ['default'],
      };
      mockFetch.mockImplementation(() => mockJsonResponse(manifest));

      const result = await loadManifest('https://server.com');
      expect(result.ok).toBe(true);
      if (result.ok) {
        expect(result.value).toEqual(manifest);
      }
    });

    test('returns error for HTTP failures', async () => {
      mockFetch.mockImplementation(() => mockErrorResponse(404));
      const result = await loadManifest('https://server.com');
      expect(result.ok).toBe(false);
    });

    test('returns error for invalid JSON', async () => {
      mockFetch.mockImplementation(() => mockJsonResponse('invalid'));
      const result = await loadManifest('https://server.com');
      expect(result.ok).toBe(false);
    });

    test('returns error for missing required fields', async () => {
      // Missing namespace and features
      mockFetch.mockImplementation(() =>
        mockJsonResponse({name: 'Test', modules: ['test']}),
      );
      const result = await loadManifest('https://server.com');
      expect(result.ok).toBe(false);
    });
  });

  describe('initializeExtensions', () => {
    test('handles empty server list', () => {
      const mockApp = createMockAppImpl();
      initializeExtensions(mockApp as unknown as AppImpl, []);

      expect(mockApp.addMacros).not.toHaveBeenCalled();
      expect(mockApp.addSqlPackages).not.toHaveBeenCalled();
      expect(mockApp.addProtoDescriptors).not.toHaveBeenCalled();
    });

    test('skips disabled servers', async () => {
      const manifest = {
        name: 'Test',
        namespace: 'test',
        features: ['macros'],
        modules: ['default'],
      };
      mockFetch.mockImplementation(() => mockJsonResponse(manifest));

      const mockApp = createMockAppImpl();
      initializeExtensions(mockApp as unknown as AppImpl, [
        {
          url: 'https://server.com',
          enabledModules: ['default'],
          enabled: false,
        },
      ]);

      expect(mockApp.addMacros).not.toHaveBeenCalled();
      expect(mockApp.addSqlPackages).not.toHaveBeenCalled();
      expect(mockApp.addProtoDescriptors).not.toHaveBeenCalled();
    });

    test('loads extensions for each enabled module', async () => {
      const manifest = {
        name: 'Test Server',
        namespace: 'test',
        features: ['macros', 'sql_modules', 'proto_descriptors'],
        modules: ['default', 'android'],
      };

      mockFetch.mockImplementation((url: string) => {
        if (url.endsWith('/manifest')) {
          return mockJsonResponse(manifest);
        }
        if (url.includes('/macros')) {
          return mockJsonResponse({
            macros: [{id: 'test.macro1', name: 'Test Macro', run: []}],
          });
        }
        if (url.includes('/sql_modules')) {
          return mockJsonResponse({
            sqlModules: [{name: 'test.module', sql: 'SELECT 1'}],
          });
        }
        if (url.includes('/proto_descriptors')) {
          return mockJsonResponse({
            descriptors: ['base64descriptor'],
          });
        }
        return mockErrorResponse(404);
      });

      const mockApp = createMockAppImpl();
      initializeExtensions(mockApp as unknown as AppImpl, [
        {
          url: 'https://server.com',
          enabledModules: ['default'],
          enabled: true,
        },
      ]);

      // Wait for all promises to settle
      await Promise.all([
        ...mockApp.getMacrosAdded(),
        ...mockApp.getSqlPackagesAdded(),
        ...mockApp.getProtoDescriptorsAdded(),
      ]);

      expect(mockApp.addMacros).toHaveBeenCalledTimes(1);
      expect(mockApp.addSqlPackages).toHaveBeenCalledTimes(1);
      expect(mockApp.addProtoDescriptors).toHaveBeenCalledTimes(1);
    });

    test('loads macros when feature is supported', async () => {
      const manifest = {
        name: 'Test',
        namespace: 'myext',
        features: ['macros'],
        modules: ['default'],
      };
      // Macro IDs must start with namespace
      const macros = [{id: 'myext.macro1', name: 'My Macro', run: []}];

      mockFetch.mockImplementation((url: string) => {
        if (url.endsWith('/manifest')) {
          return mockJsonResponse(manifest);
        }
        if (url.includes('/macros')) {
          return mockJsonResponse({macros});
        }
        return mockErrorResponse(404);
      });

      const mockApp = createMockAppImpl();
      initializeExtensions(mockApp as unknown as AppImpl, [
        {
          url: 'https://server.com',
          enabledModules: ['default'],
          enabled: true,
        },
      ]);

      const macrosResult = await mockApp.getMacrosAdded()[0];
      expect(macrosResult).toEqual(macros);
    });

    test('returns empty array when feature is not supported', async () => {
      const manifest = {
        name: 'Test',
        namespace: 'test',
        features: [], // No features supported
        modules: ['default'],
      };

      mockFetch.mockImplementation((url: string) => {
        if (url.endsWith('/manifest')) {
          return mockJsonResponse(manifest);
        }
        return mockErrorResponse(404);
      });

      const mockApp = createMockAppImpl();
      initializeExtensions(mockApp as unknown as AppImpl, [
        {
          url: 'https://server.com',
          enabledModules: ['default'],
          enabled: true,
        },
      ]);

      const [macros, sqlPackages, protos] = await Promise.all([
        mockApp.getMacrosAdded()[0],
        mockApp.getSqlPackagesAdded()[0],
        mockApp.getProtoDescriptorsAdded()[0],
      ]);

      expect(macros).toEqual([]);
      expect(sqlPackages).toEqual([]);
      expect(protos).toEqual([]);
    });

    test('returns empty array when module not found', async () => {
      const manifest = {
        name: 'Test',
        namespace: 'test',
        features: ['macros'],
        modules: ['other'], // 'default' is not available
      };

      mockFetch.mockImplementation((url: string) => {
        if (url.endsWith('/manifest')) {
          return mockJsonResponse(manifest);
        }
        return mockErrorResponse(404);
      });

      const mockApp = createMockAppImpl();
      initializeExtensions(mockApp as unknown as AppImpl, [
        {
          url: 'https://server.com',
          enabledModules: ['default'], // Requesting non-existent module
          enabled: true,
        },
      ]);

      const macros = await mockApp.getMacrosAdded()[0];
      expect(macros).toEqual([]);
    });

    test('handles fetch failures gracefully', async () => {
      mockFetch.mockImplementation(() => mockErrorResponse(500));

      const mockApp = createMockAppImpl();
      initializeExtensions(mockApp as unknown as AppImpl, [
        {
          url: 'https://server.com',
          enabledModules: ['default'],
          enabled: true,
        },
      ]);

      const [macros, sqlPackages, protos] = await Promise.all([
        mockApp.getMacrosAdded()[0],
        mockApp.getSqlPackagesAdded()[0],
        mockApp.getProtoDescriptorsAdded()[0],
      ]);

      expect(macros).toEqual([]);
      expect(sqlPackages).toEqual([]);
      expect(protos).toEqual([]);
    });

    test('processes multiple modules from same server', async () => {
      const manifest = {
        name: 'Test',
        namespace: 'test',
        features: ['macros'],
        modules: ['default', 'android'],
      };

      mockFetch.mockImplementation((url: string) => {
        if (url.endsWith('/manifest')) {
          return mockJsonResponse(manifest);
        }
        if (url.includes('/default/macros')) {
          return mockJsonResponse({
            macros: [{id: 'test.default1', name: 'Default Macro', run: []}],
          });
        }
        if (url.includes('/android/macros')) {
          return mockJsonResponse({
            macros: [{id: 'test.android1', name: 'Android Macro', run: []}],
          });
        }
        return mockErrorResponse(404);
      });

      const mockApp = createMockAppImpl();
      initializeExtensions(mockApp as unknown as AppImpl, [
        {
          url: 'https://server.com',
          enabledModules: ['default', 'android'],
          enabled: true,
        },
      ]);

      // Should be called twice (once per module)
      expect(mockApp.addMacros).toHaveBeenCalledTimes(2);
      expect(mockApp.addSqlPackages).toHaveBeenCalledTimes(2);
      expect(mockApp.addProtoDescriptors).toHaveBeenCalledTimes(2);
    });

    test('loads sql packages with correct structure', async () => {
      const manifest = {
        name: 'Test',
        namespace: 'myext',
        features: ['sql_modules'],
        modules: ['default'],
      };

      mockFetch.mockImplementation((url: string) => {
        if (url.endsWith('/manifest')) {
          return mockJsonResponse(manifest);
        }
        if (url.includes('/sql_modules')) {
          return mockJsonResponse({
            sqlModules: [
              {name: 'myext.helpers', sql: 'CREATE TABLE t(x INT)'},
              {name: 'myext.utils', sql: 'SELECT 1'},
            ],
          });
        }
        return mockErrorResponse(404);
      });

      const mockApp = createMockAppImpl();
      initializeExtensions(mockApp as unknown as AppImpl, [
        {
          url: 'https://server.com',
          enabledModules: ['default'],
          enabled: true,
        },
      ]);

      const sqlPackages = await mockApp.getSqlPackagesAdded()[0];
      expect(sqlPackages).toHaveLength(1);
      expect(sqlPackages[0]?.name).toBe('myext');
      expect(sqlPackages[0]?.modules).toHaveLength(2);
    });

    test('rejects macros with invalid namespace prefix', async () => {
      const manifest = {
        name: 'Test',
        namespace: 'myext',
        features: ['macros'],
        modules: ['default'],
      };

      mockFetch.mockImplementation((url: string) => {
        if (url.endsWith('/manifest')) {
          return mockJsonResponse(manifest);
        }
        if (url.includes('/macros')) {
          // Macro ID doesn't start with namespace
          return mockJsonResponse({
            macros: [{id: 'wrong.macro1', name: 'Bad Macro', run: []}],
          });
        }
        return mockErrorResponse(404);
      });

      const mockApp = createMockAppImpl();
      initializeExtensions(mockApp as unknown as AppImpl, [
        {
          url: 'https://server.com',
          enabledModules: ['default'],
          enabled: true,
        },
      ]);

      // Should return empty array due to validation failure
      const macros = await mockApp.getMacrosAdded()[0];
      expect(macros).toEqual([]);
    });

    test('rejects sql modules with invalid namespace prefix', async () => {
      const manifest = {
        name: 'Test',
        namespace: 'myext',
        features: ['sql_modules'],
        modules: ['default'],
      };

      mockFetch.mockImplementation((url: string) => {
        if (url.endsWith('/manifest')) {
          return mockJsonResponse(manifest);
        }
        if (url.includes('/sql_modules')) {
          // SQL module name doesn't start with namespace
          return mockJsonResponse({
            sqlModules: [{name: 'wrong.module', sql: 'SELECT 1'}],
          });
        }
        return mockErrorResponse(404);
      });

      const mockApp = createMockAppImpl();
      initializeExtensions(mockApp as unknown as AppImpl, [
        {
          url: 'https://server.com',
          enabledModules: ['default'],
          enabled: true,
        },
      ]);

      // Should return empty array due to validation failure
      const sqlPackages = await mockApp.getSqlPackagesAdded()[0];
      expect(sqlPackages).toEqual([]);
    });
  });
});
