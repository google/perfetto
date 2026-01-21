// Copyright (C) 2025 The Android Open Source Project
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
  buildFunctionExpression,
  getColumnsForArgType,
  isFunctionModalValid,
  createFunctionColumn,
  createFunctionModalState,
  FunctionModalState,
} from './add_columns_function_modal';
import {FunctionWithModule} from '../function_list';
import {FunctionArgBinding} from './add_columns_types';
import {ColumnInfo} from '../column_info';
import {
  SqlModules,
  SqlModule,
  SqlFunction,
  SqlTable,
} from '../../../dev.perfetto.SqlModules/sql_modules';
import {
  PerfettoSqlTypes,
  PerfettoSqlType,
} from '../../../../trace_processor/perfetto_sql_type';

// Helper to create a mock function
function createMockFunction(
  name: string,
  args: Array<{name: string; type: string; description?: string}> = [],
  returnType: string = 'INT',
  description: string = '',
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

// Helper to create a mock ColumnInfo
function createMockColumnInfo(
  name: string,
  type?: PerfettoSqlType,
): ColumnInfo {
  return {
    name,
    type: type ? type.kind.toUpperCase() : 'UNKNOWN',
    checked: false,
    column: {name, type},
  };
}

describe('buildFunctionExpression', () => {
  it('should build expression for function with no arguments', () => {
    const fn = createMockFunction('current_timestamp', [], 'LONG');
    const module = createMockModule('prelude', [fn]);
    const fnWithModule: FunctionWithModule = {fn, module};

    const result = buildFunctionExpression(fnWithModule, []);

    expect(result).toBe('current_timestamp()');
  });

  it('should build expression for function with single argument', () => {
    const fn = createMockFunction(
      'dur_ns_to_ms',
      [{name: 'dur', type: 'INT'}],
      'DOUBLE',
    );
    const module = createMockModule('prelude.time', [fn]);
    const fnWithModule: FunctionWithModule = {fn, module};

    const argBindings: FunctionArgBinding[] = [
      {argName: 'dur', value: 'duration', isCustomExpression: false},
    ];

    const result = buildFunctionExpression(fnWithModule, argBindings);

    expect(result).toBe('dur_ns_to_ms(duration)');
  });

  it('should build expression for function with multiple arguments', () => {
    const fn = createMockFunction(
      'substr',
      [
        {name: 'str', type: 'STRING'},
        {name: 'start', type: 'INT'},
        {name: 'length', type: 'INT'},
      ],
      'STRING',
    );
    const module = createMockModule('prelude', [fn]);
    const fnWithModule: FunctionWithModule = {fn, module};

    const argBindings: FunctionArgBinding[] = [
      {argName: 'str', value: 'name', isCustomExpression: false},
      {argName: 'start', value: '1', isCustomExpression: true},
      {argName: 'length', value: '10', isCustomExpression: true},
    ];

    const result = buildFunctionExpression(fnWithModule, argBindings);

    expect(result).toBe('substr(name, 1, 10)');
  });

  it('should return empty string for undefined function', () => {
    const result = buildFunctionExpression(undefined, []);

    expect(result).toBe('');
  });

  it('should handle missing bindings by using empty string', () => {
    const fn = createMockFunction(
      'test_func',
      [
        {name: 'a', type: 'INT'},
        {name: 'b', type: 'INT'},
      ],
      'INT',
    );
    const module = createMockModule('test', [fn]);
    const fnWithModule: FunctionWithModule = {fn, module};

    const argBindings: FunctionArgBinding[] = [
      {argName: 'a', value: 'col1', isCustomExpression: false},
      // 'b' binding is missing
    ];

    const result = buildFunctionExpression(fnWithModule, argBindings);

    expect(result).toBe('test_func(col1, )');
  });
});

describe('getColumnsForArgType', () => {
  const sourceCols: ColumnInfo[] = [
    createMockColumnInfo('id', PerfettoSqlTypes.INT),
    createMockColumnInfo('ts', PerfettoSqlTypes.TIMESTAMP),
    createMockColumnInfo('dur', PerfettoSqlTypes.DURATION),
    createMockColumnInfo('name', PerfettoSqlTypes.STRING),
    createMockColumnInfo('count', PerfettoSqlTypes.INT),
    createMockColumnInfo('unknown_col'), // No type
  ];

  it('should return quantitative columns for INT arg type', () => {
    const result = getColumnsForArgType('INT', sourceCols);

    // Should include id, ts, dur, count, and unknown_col (unknown types are included)
    expect(result).toContain('id');
    expect(result).toContain('ts');
    expect(result).toContain('dur');
    expect(result).toContain('count');
    expect(result).toContain('unknown_col');
    expect(result).not.toContain('name');
  });

  it('should return string columns for STRING arg type', () => {
    const result = getColumnsForArgType('STRING', sourceCols);

    expect(result).toContain('name');
    expect(result).toContain('unknown_col');
    expect(result).not.toContain('ts');
    expect(result).not.toContain('dur');
  });

  it('should return all columns for unknown arg type', () => {
    const result = getColumnsForArgType('UNKNOWN_TYPE', sourceCols);

    expect(result.length).toBe(sourceCols.length);
  });
});

describe('isFunctionModalValid', () => {
  const fn = createMockFunction(
    'test_func',
    [{name: 'arg1', type: 'INT'}],
    'INT',
  );
  const module = createMockModule('test', [fn]);
  const fnWithModule: FunctionWithModule = {fn, module};

  const noError = () => undefined;
  const hasError = () => 'Column name already exists';

  it('should return false when no function is selected', () => {
    const state: FunctionModalState = {
      step: 'configure',
      searchQuery: '',
      selectedFunctionWithModule: undefined,
      argBindings: [],
      columnName: 'test_col',
    };

    expect(isFunctionModalValid(state, noError)).toBe(false);
  });

  it('should return false when column name is empty', () => {
    const state: FunctionModalState = {
      step: 'configure',
      searchQuery: '',
      selectedFunctionWithModule: fnWithModule,
      argBindings: [
        {argName: 'arg1', value: 'col1', isCustomExpression: false},
      ],
      columnName: '',
    };

    expect(isFunctionModalValid(state, noError)).toBe(false);
  });

  it('should return false when column name has an error', () => {
    const state: FunctionModalState = {
      step: 'configure',
      searchQuery: '',
      selectedFunctionWithModule: fnWithModule,
      argBindings: [
        {argName: 'arg1', value: 'col1', isCustomExpression: false},
      ],
      columnName: 'existing_col',
    };

    expect(isFunctionModalValid(state, hasError)).toBe(false);
  });

  it('should return false when required argument is missing', () => {
    const state: FunctionModalState = {
      step: 'configure',
      searchQuery: '',
      selectedFunctionWithModule: fnWithModule,
      argBindings: [{argName: 'arg1', value: '', isCustomExpression: false}],
      columnName: 'test_col',
    };

    expect(isFunctionModalValid(state, noError)).toBe(false);
  });

  it('should return true when all fields are valid', () => {
    const state: FunctionModalState = {
      step: 'configure',
      searchQuery: '',
      selectedFunctionWithModule: fnWithModule,
      argBindings: [
        {argName: 'arg1', value: 'col1', isCustomExpression: false},
      ],
      columnName: 'test_col',
    };

    expect(isFunctionModalValid(state, noError)).toBe(true);
  });

  it('should return true for function with no arguments', () => {
    const noArgFn = createMockFunction('no_args', [], 'INT');
    const noArgModule = createMockModule('test', [noArgFn]);
    const noArgFnWithModule: FunctionWithModule = {
      fn: noArgFn,
      module: noArgModule,
    };

    const state: FunctionModalState = {
      step: 'configure',
      searchQuery: '',
      selectedFunctionWithModule: noArgFnWithModule,
      argBindings: [],
      columnName: 'test_col',
    };

    expect(isFunctionModalValid(state, noError)).toBe(true);
  });
});

describe('createFunctionColumn', () => {
  const fn = createMockFunction(
    'dur_ns_to_ms',
    [{name: 'dur', type: 'INT'}],
    'DOUBLE',
  );
  const module = createMockModule('prelude.time', [fn]);
  const fnWithModule: FunctionWithModule = {fn, module};

  it('should create a NewColumn with correct properties', () => {
    const state: FunctionModalState = {
      step: 'configure',
      searchQuery: '',
      selectedFunctionWithModule: fnWithModule,
      argBindings: [
        {argName: 'dur', value: 'duration', isCustomExpression: false},
      ],
      columnName: '  dur_ms  ', // With whitespace
    };

    const result = createFunctionColumn(state);

    expect(result).toBeDefined();
    expect(result?.type).toBe('function');
    expect(result?.name).toBe('dur_ms'); // Trimmed
    expect(result?.expression).toBe('dur_ns_to_ms(duration)');
    expect(result?.module).toBe('prelude.time');
    expect(result?.functionName).toBe('dur_ns_to_ms');
    expect(result?.functionArgs).toEqual(state.argBindings);
    expect(result?.sqlType).toBe('DOUBLE');
  });

  it('should return undefined when no function is selected', () => {
    const state: FunctionModalState = {
      step: 'configure',
      searchQuery: '',
      selectedFunctionWithModule: undefined,
      argBindings: [],
      columnName: 'test_col',
    };

    const result = createFunctionColumn(state);

    expect(result).toBeUndefined();
  });
});

describe('createFunctionModalState', () => {
  const fn = createMockFunction(
    'test_func',
    [{name: 'arg1', type: 'INT'}],
    'INT',
  );
  const module = createMockModule('test.module', [fn]);
  const sqlModules = createMockSqlModules([module]);

  it('should create initial state for new column', () => {
    const state = createFunctionModalState(false, undefined, sqlModules);

    expect(state.step).toBe('select');
    expect(state.searchQuery).toBe('');
    expect(state.selectedFunctionWithModule).toBeUndefined();
    expect(state.argBindings).toEqual([]);
    expect(state.columnName).toBe('');
  });

  it('should create configure state for editing', () => {
    const existingColumn = {
      type: 'function' as const,
      expression: 'test_func(col1)',
      name: 'result_col',
      module: 'test.module',
      functionName: 'test_func',
      functionArgs: [
        {argName: 'arg1', value: 'col1', isCustomExpression: false},
      ],
      sqlType: 'INT',
    };

    const state = createFunctionModalState(true, existingColumn, sqlModules);

    expect(state.step).toBe('configure');
    expect(state.selectedFunctionWithModule?.fn.name).toBe('test_func');
    expect(state.argBindings).toEqual(existingColumn.functionArgs);
    expect(state.columnName).toBe('result_col');
  });

  it('should fallback to select state if function not found', () => {
    const existingColumn = {
      type: 'function' as const,
      expression: 'unknown_func(col1)',
      name: 'result_col',
      module: 'unknown.module',
      functionName: 'unknown_func',
      functionArgs: [],
      sqlType: 'INT',
    };

    const state = createFunctionModalState(true, existingColumn, sqlModules);

    // Function not found, so selectedFunctionWithModule should be undefined
    expect(state.selectedFunctionWithModule).toBeUndefined();
  });
});
