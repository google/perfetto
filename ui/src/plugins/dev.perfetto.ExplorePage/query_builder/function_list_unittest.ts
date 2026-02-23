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

import {
  SqlModules,
  SqlModule,
  SqlFunction,
  SqlTable,
} from '../../dev.perfetto.SqlModules/sql_modules';

// Helper to create a mock function
function createMockFunction(
  name: string,
  description: string = '',
  args: Array<{name: string; type: string; description?: string}> = [],
  returnType: string = 'INT',
): SqlFunction {
  return {
    name,
    description,
    args: args.map((a) => ({
      name: a.name,
      type: a.type,
      description: a.description ?? '',
    })),
    returnType,
    returnDesc: '',
  };
}

// Helper to create a mock module
function createMockModule(
  includeKey: string,
  functions: SqlFunction[],
  tags: string[] = [],
): SqlModule {
  return {
    includeKey,
    tags,
    tables: [],
    functions,
    tableFunctions: [],
    macros: [],
    getTable: () => undefined,
    getSqlTableDefinition: () => undefined,
  };
}

// Helper to create mock SqlModules
function createMockSqlModules(modules: SqlModule[]): SqlModules {
  return {
    listTables: (): SqlTable[] => [],
    listModules: () => modules,
    listTablesNames: () => [],
    getTable: () => undefined,
    getModuleForTable: () => undefined,
    isModuleDisabled: () => false,
    getDisabledModules: () => new Set(),
    ensureInitialized: async () => {},
  };
}

describe('FunctionList search logic', () => {
  // Test data
  const durNsToMs = createMockFunction(
    'dur_ns_to_ms',
    'Converts duration from nanoseconds to milliseconds',
    [{name: 'dur', type: 'INT'}],
    'DOUBLE',
  );

  const tsToUnixMs = createMockFunction(
    'ts_to_unix_ms',
    'Converts timestamp to Unix milliseconds',
    [{name: 'ts', type: 'LONG'}],
    'LONG',
  );

  const formatDuration = createMockFunction(
    'format_duration',
    'Formats a duration value as a human-readable string',
    [{name: 'dur', type: 'INT'}],
    'STRING',
  );

  const _privateFunc = createMockFunction(
    '_private_internal',
    'This is a private function',
    [],
    'INT',
  );

  const modules = [
    createMockModule('prelude.time', [durNsToMs, tsToUnixMs], ['time']),
    createMockModule(
      'android.helpers',
      [formatDuration, _privateFunc],
      ['android', 'formatting'],
    ),
  ];

  const sqlModules = createMockSqlModules(modules);

  describe('filtering private functions', () => {
    it('should exclude functions starting with underscore', () => {
      const allFunctions = sqlModules
        .listModules()
        .flatMap((module) =>
          module.functions
            .filter((fn) => !fn.name.startsWith('_'))
            .map((fn) => ({fn, module})),
        );

      expect(allFunctions.length).toBe(3);
      expect(allFunctions.map((f) => f.fn.name)).not.toContain(
        '_private_internal',
      );
    });
  });

  describe('search by function name', () => {
    it('should find functions by exact name match', () => {
      const allFunctions = sqlModules
        .listModules()
        .flatMap((module) =>
          module.functions
            .filter((fn) => !fn.name.startsWith('_'))
            .map((fn) => ({fn, module})),
        );

      const results = allFunctions.filter((f) =>
        f.fn.name.toLowerCase().includes('dur_ns'),
      );

      expect(results.length).toBe(1);
      expect(results[0].fn.name).toBe('dur_ns_to_ms');
    });

    it('should find functions by partial name match', () => {
      const allFunctions = sqlModules
        .listModules()
        .flatMap((module) =>
          module.functions
            .filter((fn) => !fn.name.startsWith('_'))
            .map((fn) => ({fn, module})),
        );

      const results = allFunctions.filter((f) =>
        f.fn.name.toLowerCase().includes('dur'),
      );

      expect(results.length).toBe(2);
      expect(results.map((r) => r.fn.name)).toContain('dur_ns_to_ms');
      expect(results.map((r) => r.fn.name)).toContain('format_duration');
    });
  });

  describe('search by description', () => {
    it('should find functions by description content', () => {
      const allFunctions = sqlModules
        .listModules()
        .flatMap((module) =>
          module.functions
            .filter((fn) => !fn.name.startsWith('_'))
            .map((fn) => ({fn, module})),
        );

      const query = 'nanoseconds';
      const results = allFunctions.filter(
        (f) =>
          f.fn.description &&
          f.fn.description.toLowerCase().includes(query.toLowerCase()),
      );

      expect(results.length).toBe(1);
      expect(results[0].fn.name).toBe('dur_ns_to_ms');
    });
  });

  describe('search by argument name', () => {
    it('should find functions by argument name', () => {
      const allFunctions = sqlModules
        .listModules()
        .flatMap((module) =>
          module.functions
            .filter((fn) => !fn.name.startsWith('_'))
            .map((fn) => ({fn, module})),
        );

      const query = 'ts';
      const results = allFunctions.filter((f) =>
        f.fn.args.some((arg) =>
          arg.name.toLowerCase().includes(query.toLowerCase()),
        ),
      );

      expect(results.length).toBe(1);
      expect(results[0].fn.name).toBe('ts_to_unix_ms');
    });
  });

  describe('tag filtering', () => {
    it('should filter functions by module tag', () => {
      const selectedTags = new Set(['time']);

      const allFunctions = sqlModules
        .listModules()
        .flatMap((module) =>
          module.functions
            .filter((fn) => !fn.name.startsWith('_'))
            .map((fn) => ({fn, module})),
        );

      const filteredFunctions = allFunctions.filter((item) =>
        Array.from(selectedTags).every((tag) => item.module.tags.includes(tag)),
      );

      expect(filteredFunctions.length).toBe(2);
      expect(filteredFunctions.map((f) => f.fn.name)).toContain('dur_ns_to_ms');
      expect(filteredFunctions.map((f) => f.fn.name)).toContain(
        'ts_to_unix_ms',
      );
    });

    it('should filter functions by multiple tags (AND logic)', () => {
      const selectedTags = new Set(['android', 'formatting']);

      const allFunctions = sqlModules
        .listModules()
        .flatMap((module) =>
          module.functions
            .filter((fn) => !fn.name.startsWith('_'))
            .map((fn) => ({fn, module})),
        );

      const filteredFunctions = allFunctions.filter((item) =>
        Array.from(selectedTags).every((tag) => item.module.tags.includes(tag)),
      );

      expect(filteredFunctions.length).toBe(1);
      expect(filteredFunctions[0].fn.name).toBe('format_duration');
    });
  });
});
