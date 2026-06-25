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

import type {ColumnContext, IrContext, SqlStatement} from './node_types';
import type {ColumnDef} from './graph_utils';
// Port type is used for context but not directly needed here

import {manifest as fromNode} from './nodes/from';
import {
  manifest as filterNode,
  conditionsToSql,
  type FilterCondition,
  type FilterConfig,
  type FilterConjunction,
  type FilterOp,
} from './nodes/filter';
import {
  manifest as selectNode,
  type SelectConfig,
  type SelectEntry,
  type SelectExpression,
} from './nodes/select';
import {
  manifest as joinNode,
  getExtendColumnAliases,
  type JoinColumn,
  type JoinConfig,
} from './nodes/join';
import {
  manifest as groupByNode,
  type Aggregation,
  type GroupByConfig,
} from './nodes/groupby';
import {
  manifest as sortNode,
  getSortConditions,
  sortConditionsToSql,
  type SortCondition,
  type SortConfig,
} from './nodes/sort';
import {manifest as unionNode, type UnionConfig} from './nodes/union';
import {
  manifest as sqlNode,
  type SqlConfig,
  type SqlOutputColumn,
} from './nodes/sql';
import {
  manifest as extendNode,
  type ExtendConfig,
  type ExtendEntry,
} from './nodes/extend';
import {manifest as dropNode} from './nodes/drop';
import {manifest as limitNode, type LimitConfig} from './nodes/limit';
import {
  manifest as timeRangeNode,
  type TimeRangeConfig,
} from './nodes/time_range';
import {
  manifest as extractArgNode,
  type ExtractArgConfig,
  type ExtractArgEntry,
} from './nodes/extract_arg';
import {
  manifest as intervalIntersectNode,
  type IntervalIntersectConfig,
} from './nodes/interval_intersect';

// --- Helper stubs ---

function makeIrContext(
  inputRefs: Record<string, string>,
  inputCols?: Record<string, ColumnDef[]>,
): IrContext {
  return {
    inputPorts: Object.keys(inputRefs).map((name) => ({name, content: name})),
    getInputRef: (portName: string) => inputRefs[portName] ?? '',
    getInputColumns: (portName: string) => inputCols?.[portName] ?? undefined,
  };
}

function makeColumnContext(
  inputCols: Record<string, ColumnDef[]>,
): ColumnContext {
  return {
    inputPorts: Object.keys(inputCols).map((name) => ({name, content: name})),
    getInputColumns: (portName: string) => inputCols[portName],
    sqlModules: undefined,
  };
}

// Helper to create filter configs without type issues
function filterConfig(
  conds: FilterCondition[],
  conjunction?: FilterConjunction,
): FilterConfig {
  return {conditions: conds, conjunction};
}

// Helper to create groupby configs
function groupByConfig(
  groupCols: string[],
  aggs: Aggregation[],
): GroupByConfig {
  return {groupColumns: groupCols, aggregations: aggs};
}

// Helper to create sort configs
function sortConfig(
  sortCol: string,
  sortOrder: 'ASC' | 'DESC',
  conds?: SortCondition[],
): SortConfig {
  return {sortColumn: sortCol, sortOrder, conditions: conds ?? []};
}

// Helper to create select configs
function selectConfig(
  entries?: SelectEntry[],
  expressions?: SelectExpression[],
): SelectConfig {
  return {entries: entries ?? [], expressions: expressions ?? []};
}

// Helper to create extend configs
function extendConfig(entries: ExtendEntry[]): ExtendConfig {
  return {entries};
}

// Helper to create join configs
function joinConfig(
  leftCol: string,
  rightCol: string,
  joinType: 'LEFT' | 'INNER' = 'LEFT',
  cols?: JoinColumn[],
): JoinConfig {
  return {
    joinType,
    leftColumn: leftCol,
    rightColumn: rightCol,
    columns: cols ?? [],
  };
}

// Helper to create limit configs
function limitConfig(count: string): LimitConfig {
  return {limitCount: count};
}

// Helper to create sql configs
function sqlConfig(
  sql: string,
  inputPorts?: string[],
  columns?: SqlOutputColumn[],
): SqlConfig {
  return {sql, inputPorts: inputPorts ?? [], columns: columns ?? []};
}

// Helper to create union configs
function unionConfig(distinct: boolean, numInputs: number): UnionConfig {
  return {distinct, numInputs};
}

// Helper to create timeRange configs
function timeRangeConfig(ts: string, dur: string): TimeRangeConfig {
  return {ts, dur};
}

// Helper to create extractArg configs
function extractArgConfig(
  argSetIdCol: string,
  extractions: ExtractArgEntry[],
): ExtractArgConfig {
  return {argSetIdCol, extractions};
}

// Helper to create intervalIntersect configs
function intervalIntersectConfig(
  numInputs: number,
  partitionCols: string[],
  filterNeg: boolean,
): IntervalIntersectConfig {
  return {
    numInputs,
    partitionColumns: partitionCols,
    filterNegativeDur: filterNeg,
  };
}

// ============================================================================
// From
// ============================================================================

describe('From manifest', () => {
  it('has correct metadata', () => {
    expect(fromNode.title).toBe('From');
    expect(fromNode.hue).toBe(210);
  });

  it('isValid rejects empty table', () => {
    expect(fromNode.isValid({table: ''})).toBe(false);
  });

  it('isValid accepts non-empty table', () => {
    expect(fromNode.isValid({table: 'slice'})).toBe(true);
  });

  it('emitIr produces SELECT * FROM <table>', () => {
    const result = fromNode.emitIr!({table: 'slice'}, makeIrContext({}));
    expect(result?.sql).toBe('SELECT *\nFROM slice');
  });

  it('emitIr handles table names with underscores', () => {
    const result = fromNode.emitIr!(
      {table: 'android_perfetto_slices'},
      makeIrContext({}),
    );
    expect(result?.sql).toBe('SELECT *\nFROM android_perfetto_slices');
  });

  it('defaultConfig returns slice table', () => {
    expect(fromNode.defaultConfig()).toEqual({table: 'slice'});
  });
});

// ============================================================================
// Filter
// ============================================================================

describe('Filter manifest', () => {
  it('has correct metadata', () => {
    expect(filterNode.title).toBe('Filter');
    expect(filterNode.hue).toBe(35);
  });

  it('isValid accepts empty conditions', () => {
    expect(filterNode.isValid({conditions: []})).toBe(true);
  });

  it('isValid accepts condition with empty column (no-op)', () => {
    expect(
      filterNode.isValid({conditions: [{column: '', op: '>', value: '1000'}]}),
    ).toBe(true);
  });

  it('isValid requires value for non-NULL ops with column', () => {
    expect(
      filterNode.isValid({
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      }),
    ).toBe(true);
    expect(
      filterNode.isValid({conditions: [{column: 'dur', op: '>', value: ''}]}),
    ).toBe(false);
  });

  it('isValid accepts IS NULL without value', () => {
    expect(
      filterNode.isValid({
        conditions: [{column: 'dur', op: 'IS NULL', value: ''}],
      }),
    ).toBe(true);
  });

  it('isValid accepts IS NOT NULL without value', () => {
    expect(
      filterNode.isValid({
        conditions: [{column: 'dur', op: 'IS NOT NULL', value: ''}],
      }),
    ).toBe(true);
  });

  it('tryFold appends WHERE clause', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = filterNode.tryFold!(
      stmt,
      filterConfig([{column: 'dur', op: '>', value: '1000'}]),
    );
    expect(result).toBe(true);
    expect(stmt.where).toBe('dur > 1000');
  });

  it('tryFold appends to existing WHERE with AND', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd', where: 'x > 1'};
    const result = filterNode.tryFold!(
      stmt,
      filterConfig([{column: 'dur', op: '>', value: '1000'}]),
    );
    expect(result).toBe(true);
    expect(stmt.where).toBe('(x > 1) AND (dur > 1000)');
  });

  it('tryFold does NOT fold when groupBy is present', () => {
    const stmt: SqlStatement = {
      columns: '*',
      from: '_qb_abcd',
      groupBy: 'name',
    };
    const result = filterNode.tryFold!(
      stmt,
      filterConfig([{column: 'dur', op: '>', value: '1000'}]),
    );
    expect(result).toBe(false);
    expect(stmt.where).toBeUndefined();
  });

  it('tryFold does NOT fold when limit is present', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd', limit: 100};
    const result = filterNode.tryFold!(
      stmt,
      filterConfig([{column: 'dur', op: '>', value: '1000'}]),
    );
    expect(result).toBe(false);
  });

  it('tryFold does NOT fold when orderBy is present', () => {
    const stmt: SqlStatement = {
      columns: '*',
      from: '_qb_abcd',
      orderBy: 'dur DESC',
    };
    const result = filterNode.tryFold!(
      stmt,
      filterConfig([{column: 'dur', op: '>', value: '1000'}]),
    );
    expect(result).toBe(false);
  });

  it('tryFold with OR conjunction', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = filterNode.tryFold!(
      stmt,
      filterConfig(
        [
          {column: 'dur', op: '>', value: '1000'},
          {column: 'ts', op: '<', value: '500'},
        ],
        'OR',
      ),
    );
    expect(result).toBe(true);
    expect(stmt.where).toBe('dur > 1000 OR ts < 500');
  });

  it('getOutputColumns passes through input columns', () => {
    const ctx = makeColumnContext({
      input: [
        {name: 'ts', type: {kind: 'timestamp'}},
        {name: 'dur', type: {kind: 'duration'}},
      ],
    });
    const result = filterNode.getOutputColumns!({conditions: []}, ctx);
    expect(result).toEqual([
      {name: 'ts', type: {kind: 'timestamp'}},
      {name: 'dur', type: {kind: 'duration'}},
    ]);
  });

  it('defaultConfig returns empty conditions with AND', () => {
    expect(filterNode.defaultConfig()).toEqual({
      conditions: [],
      conjunction: 'AND',
    });
  });
});

// ============================================================================
// conditionsToSql
// ============================================================================

describe('conditionsToSql', () => {
  it('handles single condition', () => {
    const conds: FilterCondition[] = [{column: 'dur', op: '>', value: '1000'}];
    expect(conditionsToSql(conds)).toBe('dur > 1000');
  });

  it('handles IS NULL without value', () => {
    const conds: FilterCondition[] = [
      {column: 'name', op: 'IS NULL', value: ''},
    ];
    expect(conditionsToSql(conds)).toBe('name IS NULL');
  });

  it('handles IS NOT NULL without value', () => {
    const conds: FilterCondition[] = [
      {column: 'name', op: 'IS NOT NULL', value: ''},
    ];
    expect(conditionsToSql(conds)).toBe('name IS NOT NULL');
  });

  it('quotes string values', () => {
    const conds: FilterCondition[] = [{column: 'name', op: '=', value: 'foo'}];
    expect(conditionsToSql(conds)).toBe("name = 'foo'");
  });

  it('does NOT quote numeric literals', () => {
    const conds: FilterCondition[] = [{column: 'dur', op: '>', value: '1000'}];
    expect(conditionsToSql(conds)).toBe('dur > 1000');
  });

  it('does NOT quote negative numbers', () => {
    const conds: FilterCondition[] = [{column: 'val', op: '=', value: '-42'}];
    expect(conditionsToSql(conds)).toBe('val = -42');
  });

  it('does NOT quote floats', () => {
    const conds: FilterCondition[] = [{column: 'val', op: '=', value: '3.14'}];
    expect(conditionsToSql(conds)).toBe('val = 3.14');
  });

  it('does NOT quote scientific notation', () => {
    const conds: FilterCondition[] = [{column: 'val', op: '=', value: '1e10'}];
    expect(conditionsToSql(conds)).toBe('val = 1e10');
  });

  it('does NOT quote NULL keyword', () => {
    const conds: FilterCondition[] = [{column: 'val', op: '=', value: 'null'}];
    expect(conditionsToSql(conds)).toBe('val = null');
  });

  it('does NOT quote already-quoted strings', () => {
    const conds: FilterCondition[] = [
      {column: 'name', op: '=', value: "'foo'"},
    ];
    expect(conditionsToSql(conds)).toBe("name = 'foo'");
  });

  it('escapes single quotes in values', () => {
    const conds: FilterCondition[] = [{column: 'name', op: '=', value: "it's"}];
    expect(conditionsToSql(conds)).toBe("name = 'it''s'");
  });

  it('does NOT quote parenthesized expressions', () => {
    const conds: FilterCondition[] = [
      {column: 'val', op: '=', value: '(1,2,3)'},
    ];
    expect(conditionsToSql(conds)).toBe('val = (1,2,3)');
  });

  it('combines conditions with AND by default', () => {
    const conds: FilterCondition[] = [
      {column: 'dur', op: '>', value: '1000'},
      {column: 'ts', op: '<', value: '500'},
    ];
    expect(conditionsToSql(conds, 'AND')).toBe('dur > 1000 AND ts < 500');
  });

  it('combines conditions with OR', () => {
    const conds: FilterCondition[] = [
      {column: 'dur', op: '>', value: '1000'},
      {column: 'ts', op: '<', value: '500'},
    ];
    expect(conditionsToSql(conds, 'OR')).toBe('dur > 1000 OR ts < 500');
  });

  it('filters out conditions with empty column', () => {
    const conds: FilterCondition[] = [
      {column: '', op: '=', value: 'x'},
      {column: 'dur', op: '>', value: '1000'},
    ];
    expect(conditionsToSql(conds)).toBe('dur > 1000');
  });

  it('returns empty string for all empty conditions', () => {
    const conds: FilterCondition[] = [{column: '', op: '=', value: ''}];
    expect(conditionsToSql(conds)).toBe('');
  });

  it('handles all comparison FILTER_OPS', () => {
    const ops: FilterOp[] = [
      '=',
      '!=',
      '>',
      '>=',
      '<',
      '<=',
      'LIKE',
      'NOT LIKE',
      'GLOB',
    ];
    for (const op of ops) {
      const conds: FilterCondition[] = [{column: 'col', op, value: 'val'}];
      const sql = conditionsToSql(conds);
      expect(sql).toContain(`col ${op} 'val'`);
    }
  });

  it('handles LIKE operator', () => {
    const conds: FilterCondition[] = [
      {column: 'name', op: 'LIKE', value: '%foo%'},
    ];
    expect(conditionsToSql(conds)).toBe("name LIKE '%foo%'");
  });

  it('handles GLOB operator', () => {
    const conds: FilterCondition[] = [
      {column: 'name', op: 'GLOB', value: '*foo*'},
    ];
    expect(conditionsToSql(conds)).toBe("name GLOB '*foo*'");
  });
});

// ============================================================================
// Select
// ============================================================================

describe('Select manifest', () => {
  it('has correct metadata', () => {
    expect(selectNode.title).toBe('Select');
    expect(selectNode.hue).toBe(145);
  });

  it('tryFold with entries replaces SELECT *', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = selectNode.tryFold!(
      stmt,
      selectConfig([
        {column: 'name', alias: ''},
        {column: 'dur', alias: ''},
      ]),
    );
    expect(result).toBe(true);
    expect(stmt.columns).toBe('name, dur');
  });

  it('tryFold with entries and aliases', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = selectNode.tryFold!(
      stmt,
      selectConfig([{column: 'name', alias: 'n'}]),
    );
    expect(result).toBe(true);
    expect(stmt.columns).toBe('name AS n');
  });

  it('tryFold without entries keeps * and appends expressions', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = selectNode.tryFold!(
      stmt,
      selectConfig(undefined, [{expression: 'dur * 1000', alias: 'dur_ms'}]),
    );
    expect(result).toBe(true);
    expect(stmt.columns).toBe('*, dur * 1000 AS dur_ms');
  });

  it('tryFold with both entries and expressions', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = selectNode.tryFold!(
      stmt,
      selectConfig(
        [{column: 'name', alias: ''}],
        [{expression: 'COUNT(*)', alias: 'cnt'}],
      ),
    );
    expect(result).toBe(true);
    expect(stmt.columns).toBe('name, COUNT(*) AS cnt');
  });

  it('tryFold does NOT fold when columns is not *', () => {
    const stmt: SqlStatement = {columns: 'name, dur', from: '_qb_abcd'};
    const result = selectNode.tryFold!(
      stmt,
      selectConfig([{column: 'ts', alias: ''}]),
    );
    expect(result).toBe(false);
  });

  it('getOutputColumns passes through all columns when no entries', () => {
    const ctx = makeColumnContext({input: [{name: 'ts'}, {name: 'dur'}]});
    const result = selectNode.getOutputColumns!({expressions: []}, ctx);
    expect(result).toEqual([{name: 'ts'}, {name: 'dur'}]);
  });

  it('getOutputColumns resolves selected columns with aliases', () => {
    const ctx = makeColumnContext({
      input: [{name: 'ts'}, {name: 'dur', type: {kind: 'duration'}}],
    });
    const result = selectNode.getOutputColumns!(
      {entries: [{column: 'dur', alias: 'duration'}]},
      ctx,
    );
    expect(result).toEqual([{name: 'duration', type: {kind: 'duration'}}]);
  });

  it('getOutputColumns adds expression aliases', () => {
    const ctx = makeColumnContext({input: [{name: 'ts'}]});
    const result = selectNode.getOutputColumns!(
      {expressions: [{expression: 'COUNT(*)', alias: 'cnt'}]},
      ctx,
    );
    expect(result).toEqual([{name: 'ts'}, {name: 'cnt'}]);
  });

  it('isValid rejects alias without expression', () => {
    expect(
      selectNode.isValid({expressions: [{expression: '', alias: 'foo'}]}),
    ).toBe(false);
  });

  it('isValid accepts expression without alias', () => {
    expect(
      selectNode.isValid({expressions: [{expression: 'COUNT(*)', alias: ''}]}),
    ).toBe(true);
  });

  it('defaultConfig returns empty entries and expressions', () => {
    expect(selectNode.defaultConfig()).toEqual({entries: [], expressions: []});
  });
});

// ============================================================================
// Join
// ============================================================================

describe('Join manifest', () => {
  it('has correct metadata', () => {
    expect(joinNode.title).toBe('Join');
    expect(joinNode.hue).toBe(308);
  });

  it('has two input ports', () => {
    const inputs = joinNode.getInputs!({
      joinType: 'LEFT',
      leftColumn: '',
      rightColumn: '',
      columns: [],
    });
    expect(inputs).toHaveLength(2);
    expect(inputs[0].name).toBe('left');
    expect(inputs[1].name).toBe('right');
  });

  it('emitIr produces LEFT JOIN', () => {
    const result = joinNode.emitIr!(
      joinConfig('ts', 'ts'),
      makeIrContext(
        {left: '_qb_1111', right: '_qb_2222'},
        {
          left: [{name: 'ts', type: {kind: 'timestamp'}}],
          right: [{name: 'ts', type: {kind: 'timestamp'}}],
        },
      ),
    );
    expect(result?.sql).toContain('LEFT JOIN');
    expect(result?.sql).toContain('AS l');
    expect(result?.sql).toContain('AS r');
    expect(result?.sql).toContain('ON l.ts = r.ts');
  });

  it('emitIr produces INNER JOIN', () => {
    const result = joinNode.emitIr!(
      joinConfig('id', 'id', 'INNER'),
      makeIrContext(
        {left: '_qb_aaaa', right: '_qb_bbbb'},
        {left: [{name: 'id'}], right: [{name: 'id'}]},
      ),
    );
    expect(result?.sql).toContain('JOIN');
    expect(result?.sql).not.toContain('LEFT JOIN');
    expect(result?.sql).toContain('ON l.id = r.id');
  });

  it('emitIr with no right input produces SELECT * FROM left', () => {
    const result = joinNode.emitIr!(
      joinConfig('ts', 'ts'),
      makeIrContext({left: '_qb_1111'}, {left: [{name: 'ts'}]}),
    );
    expect(result?.sql).toBe('SELECT *\nFROM _qb_1111');
  });

  it('emitIr with columns includes them in SELECT', () => {
    const result = joinNode.emitIr!(
      joinConfig('ts', 'ts', 'LEFT', [{column: 'dur', alias: 'duration'}]),
      makeIrContext(
        {left: '_qb_1111', right: '_qb_2222'},
        {
          left: [{name: 'ts'}],
          right: [{name: 'dur', type: {kind: 'duration'}}],
        },
      ),
    );
    expect(result?.sql).toContain('l.*');
    expect(result?.sql).toContain('r.dur AS duration');
  });

  it('emitIr handles column aliasing for name collisions', () => {
    const result = joinNode.emitIr!(
      joinConfig('id', 'id', 'LEFT', [{column: 'ts', alias: 'right_ts'}]),
      makeIrContext(
        {left: '_qb_1111', right: '_qb_2222'},
        {left: [{name: 'ts'}], right: [{name: 'ts'}]},
      ),
    );
    expect(result?.sql).toContain('r.ts AS right_ts');
  });

  it('getOutputColumns combines left + right columns', () => {
    const ctx = makeColumnContext({
      left: [{name: 'ts', type: {kind: 'timestamp'}}],
      right: [{name: 'dur', type: {kind: 'duration'}}],
    });
    const result = joinNode.getOutputColumns!(
      joinConfig('ts', 'ts', 'LEFT', [{column: 'dur', alias: 'duration'}]),
      ctx,
    );
    expect(result).toEqual([
      {name: 'ts', type: {kind: 'timestamp'}},
      {name: 'duration', type: {kind: 'duration'}},
    ]);
  });

  it('isValid requires both left and right columns', () => {
    expect(
      joinNode.isValid({joinType: 'LEFT', leftColumn: 'ts', rightColumn: ''}),
    ).toBe(false);
    expect(
      joinNode.isValid({joinType: 'LEFT', leftColumn: '', rightColumn: 'ts'}),
    ).toBe(false);
    expect(
      joinNode.isValid({joinType: 'LEFT', leftColumn: 'ts', rightColumn: 'ts'}),
    ).toBe(true);
  });

  it('resolveIcon returns correct icon based on joinType', () => {
    expect(
      joinNode.resolveIcon?.({
        joinType: 'LEFT' as const,
        leftColumn: '',
        rightColumn: '',
      }),
    ).toBe('join_left');
    expect(
      joinNode.resolveIcon?.({
        joinType: 'INNER' as const,
        leftColumn: '',
        rightColumn: '',
      }),
    ).toBe('join_inner');
  });

  it('defaultConfig', () => {
    expect(joinNode.defaultConfig()).toEqual({
      joinType: 'LEFT',
      leftColumn: '',
      rightColumn: '',
      columns: [],
    });
  });
});

// ============================================================================
// getExtendColumnAliases
// ============================================================================

describe('getExtendColumnAliases', () => {
  it('uses explicit alias when provided', () => {
    const result = getExtendColumnAliases(
      {
        columns: [{column: 'dur', alias: 'duration'}],
        leftColumn: '',
        rightColumn: '',
        joinType: 'LEFT',
      },
      ['ts'],
    );
    expect(result[0].alias).toBe('duration');
  });

  it('prefers left set name (no prefix) when column not in left', () => {
    const result = getExtendColumnAliases(
      {
        columns: [{column: 'dur', alias: ''}],
        leftColumn: '',
        rightColumn: '',
        joinType: 'LEFT',
      },
      ['ts'],
    );
    expect(result[0].alias).toBe('dur');
  });

  it('adds right_ prefix when column exists in left', () => {
    const result = getExtendColumnAliases(
      {
        columns: [{column: 'ts', alias: ''}],
        leftColumn: '',
        rightColumn: '',
        joinType: 'LEFT',
      },
      ['ts'],
    );
    expect(result[0].alias).toBe('right_ts');
  });

  it('handles empty columns', () => {
    const result = getExtendColumnAliases(
      {columns: [], leftColumn: '', rightColumn: '', joinType: 'LEFT'},
      ['ts'],
    );
    expect(result).toEqual([]);
  });
});

// ============================================================================
// GroupBy
// ============================================================================

describe('GroupBy manifest', () => {
  it('has correct metadata', () => {
    expect(groupByNode.title).toBe('Group By');
    expect(groupByNode.hue).toBe(275);
  });

  it('isValid requires at least one group column', () => {
    expect(groupByNode.isValid({groupColumns: [], aggregations: []})).toBe(
      false,
    );
    expect(
      groupByNode.isValid({groupColumns: ['name'], aggregations: []}),
    ).toBe(true);
    expect(
      groupByNode.isValid({groupColumns: ['', 'name'], aggregations: []}),
    ).toBe(true);
  });

  it('tryFold produces GROUP BY and aggregation SELECT', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = groupByNode.tryFold!(
      stmt,
      groupByConfig(['name'], [{func: 'COUNT', column: '*', alias: 'cnt'}]),
    );
    expect(result).toBe(true);
    expect(stmt.columns).toBe('name, COUNT(*) AS cnt');
    expect(stmt.groupBy).toBe('name');
  });

  it('tryFold does NOT fold when columns is not *', () => {
    const stmt: SqlStatement = {columns: 'name, dur', from: '_qb_abcd'};
    const result = groupByNode.tryFold!(stmt, groupByConfig(['name'], []));
    expect(result).toBe(false);
  });

  it('tryFold does NOT fold when groupBy is already set', () => {
    const stmt: SqlStatement = {
      columns: '*',
      from: '_qb_abcd',
      groupBy: 'name',
    };
    const result = groupByNode.tryFold!(stmt, groupByConfig(['ts'], []));
    expect(result).toBe(false);
  });

  it('tryFold does NOT fold when limit is present', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd', limit: 100};
    const result = groupByNode.tryFold!(stmt, groupByConfig(['name'], []));
    expect(result).toBe(false);
  });

  it('tryFold does NOT fold when orderBy is present', () => {
    const stmt: SqlStatement = {
      columns: '*',
      from: '_qb_abcd',
      orderBy: 'name',
    };
    const result = groupByNode.tryFold!(stmt, groupByConfig(['name'], []));
    expect(result).toBe(false);
  });

  it('tryFold with multiple group columns', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    groupByNode.tryFold!(
      stmt,
      groupByConfig(
        ['name', 'pid'],
        [{func: 'COUNT', column: '*', alias: 'cnt'}],
      ),
    );
    expect(stmt.columns).toBe('name, pid, COUNT(*) AS cnt');
    expect(stmt.groupBy).toBe('name, pid');
  });

  it('tryFold with multiple aggregations', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    groupByNode.tryFold!(
      stmt,
      groupByConfig(
        ['name'],
        [
          {func: 'COUNT', column: '*', alias: 'cnt'},
          {func: 'SUM', column: 'dur', alias: 'total_dur'},
        ],
      ),
    );
    expect(stmt.columns).toBe('name, COUNT(*) AS cnt, SUM(dur) AS total_dur');
  });

  it('getOutputColumns resolves group columns and aggregation aliases', () => {
    const ctx = makeColumnContext({
      input: [{name: 'name'}, {name: 'dur', type: {kind: 'duration'}}],
    });
    const result = groupByNode.getOutputColumns!(
      {
        groupColumns: ['name'],
        aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
      },
      ctx,
    );
    expect(result).toEqual([
      {name: 'name'},
      {name: 'cnt', type: {kind: 'int'}},
    ]);
  });

  it('getOutputColumns infers aggregation types', () => {
    const ctx = makeColumnContext({
      input: [{name: 'dur', type: {kind: 'duration'}}],
    });
    const result = groupByNode.getOutputColumns!(
      {
        groupColumns: [],
        aggregations: [{func: 'SUM', column: 'dur', alias: ''}],
      },
      ctx,
    );
    expect(result).toEqual([{name: 'sum_dur', type: {kind: 'duration'}}]);
  });

  it('getOutputColumns defaults COUNT to int', () => {
    const ctx = makeColumnContext({input: [{name: 'x'}]});
    const result = groupByNode.getOutputColumns!(
      {
        groupColumns: [],
        aggregations: [{func: 'COUNT', column: '*', alias: ''}],
      },
      ctx,
    );
    expect(result).toEqual([{name: 'count_star', type: {kind: 'int'}}]);
  });

  it('aggregation aliases default to func_column format', () => {
    const ctx = makeColumnContext({
      input: [{name: 'dur', type: {kind: 'duration'}}],
    });
    const result = groupByNode.getOutputColumns!(
      {
        groupColumns: [],
        aggregations: [{func: 'AVG', column: 'dur', alias: ''}],
      },
      ctx,
    );
    expect(result).toEqual([{name: 'avg_dur', type: {kind: 'duration'}}]);
  });

  it('defaultConfig returns empty arrays', () => {
    expect(groupByNode.defaultConfig()).toEqual({
      groupColumns: [],
      aggregations: [],
    });
  });
});

// ============================================================================
// Sort
// ============================================================================

describe('Sort manifest', () => {
  it('has correct metadata', () => {
    expect(sortNode.title).toBe('Sort');
    expect(sortNode.hue).toBe(178);
  });

  it('isValid requires at least one sort condition', () => {
    expect(
      sortNode.isValid({
        sortColumn: '',
        sortOrder: 'ASC' as const,
        conditions: [],
      }),
    ).toBe(false);
    expect(
      sortNode.isValid({
        sortColumn: 'dur',
        sortOrder: 'ASC' as const,
        conditions: [],
      }),
    ).toBe(true);
    expect(
      sortNode.isValid({
        sortColumn: '',
        sortOrder: 'ASC' as const,
        conditions: [{column: 'dur', order: 'ASC' as const}],
      }),
    ).toBe(true);
  });

  it('tryFold appends ORDER BY', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = sortNode.tryFold!(stmt, sortConfig('dur', 'DESC'));
    expect(result).toBe(true);
    expect(stmt.orderBy).toBe('dur DESC');
  });

  it('tryFold does NOT fold when orderBy is already set', () => {
    const stmt: SqlStatement = {
      columns: '*',
      from: '_qb_abcd',
      orderBy: 'ts ASC',
    };
    const result = sortNode.tryFold!(stmt, sortConfig('dur', 'DESC'));
    expect(result).toBe(false);
  });

  it('tryFold does NOT fold when limit is already set', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd', limit: 100};
    const result = sortNode.tryFold!(stmt, sortConfig('dur', 'DESC'));
    expect(result).toBe(false);
  });

  it('tryFold with multiple sort conditions', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = sortNode.tryFold!(
      stmt,
      sortConfig('', 'ASC', [
        {column: 'dur', order: 'DESC' as const},
        {column: 'ts', order: 'ASC' as const},
      ]),
    );
    expect(result).toBe(true);
    expect(stmt.orderBy).toBe('dur DESC, ts ASC');
  });

  it('getOutputColumns passes through input columns', () => {
    const ctx = makeColumnContext({input: [{name: 'ts'}, {name: 'dur'}]});
    const result = sortNode.getOutputColumns!(
      {sortColumn: '', sortOrder: 'ASC' as const},
      ctx,
    );
    expect(result).toEqual([{name: 'ts'}, {name: 'dur'}]);
  });

  it('defaultConfig', () => {
    expect(sortNode.defaultConfig()).toEqual({
      sortColumn: '',
      sortOrder: 'ASC' as const,
      conditions: [],
    });
  });
});

// ============================================================================
// sortConditionsToSql / getSortConditions
// ============================================================================

describe('sortConditionsToSql', () => {
  it('handles single condition', () => {
    expect(sortConditionsToSql([{column: 'dur', order: 'DESC' as const}])).toBe(
      'dur DESC',
    );
  });

  it('handles multiple conditions', () => {
    expect(
      sortConditionsToSql([
        {column: 'dur', order: 'DESC' as const},
        {column: 'ts', order: 'ASC' as const},
      ]),
    ).toBe('dur DESC, ts ASC');
  });

  it('filters out empty column names', () => {
    expect(
      sortConditionsToSql([
        {column: '', order: 'ASC' as const},
        {column: 'dur', order: 'DESC' as const},
      ]),
    ).toBe('dur DESC');
  });

  it('returns empty string for all empty conditions', () => {
    expect(sortConditionsToSql([])).toBe('');
  });
});

describe('getSortConditions', () => {
  it('uses conditions when present', () => {
    const config = {
      sortColumn: 'old',
      sortOrder: 'ASC' as const,
      conditions: [{column: 'new', order: 'DESC' as const}],
    };
    expect(getSortConditions(config)).toEqual([
      {column: 'new', order: 'DESC' as const},
    ]);
  });

  it('migrates legacy single-column format', () => {
    const config = {
      sortColumn: 'dur',
      sortOrder: 'DESC' as const,
      conditions: [],
    };
    expect(getSortConditions(config)).toEqual([
      {column: 'dur', order: 'DESC' as const},
    ]);
  });

  it('returns empty array for empty legacy config', () => {
    const config = {sortColumn: '', sortOrder: 'ASC' as const, conditions: []};
    expect(getSortConditions(config)).toEqual([]);
  });
});

// ============================================================================
// Union
// ============================================================================

describe('Union manifest', () => {
  it('has correct metadata', () => {
    expect(unionNode.title).toBe('Union');
    expect(unionNode.hue).toBe(242);
  });

  it('has dynamic input ports based on numInputs', () => {
    const inputs = unionNode.getInputs!({numInputs: 3, distinct: false});
    expect(inputs).toHaveLength(3);
    expect(inputs[0].name).toBe('input_0');
    expect(inputs[2].name).toBe('input_2');
  });

  it('emitIr with 2 inputs produces UNION ALL', () => {
    const result = unionNode.emitIr!(
      unionConfig(false, 2),
      makeIrContext(
        {input_0: '_qb_aaaa', input_1: '_qb_bbbb'},
        {input_0: [{name: 'x'}], input_1: [{name: 'x'}]},
      ),
    );
    expect(result?.sql).toContain('UNION ALL');
  });

  it('emitIr with distinct produces UNION', () => {
    const result = unionNode.emitIr!(
      unionConfig(true, 2),
      makeIrContext(
        {input_0: '_qb_aaaa', input_1: '_qb_bbbb'},
        {input_0: [{name: 'x'}], input_1: [{name: 'x'}]},
      ),
    );
    expect(result?.sql).toContain('UNION');
    expect(result?.sql).not.toContain('UNION ALL');
  });

  it('emitIr with 1 input produces SELECT * FROM that input', () => {
    const result = unionNode.emitIr!(
      unionConfig(false, 1),
      makeIrContext({input_0: '_qb_aaaa'}, {input_0: [{name: 'x'}]}),
    );
    expect(result?.sql).toBe('SELECT *\nFROM _qb_aaaa');
  });

  it('emitIr with 0 inputs returns undefined', () => {
    const result = unionNode.emitIr!(
      unionConfig(false, 0),
      makeIrContext({}, {}),
    );
    expect(result).toBeUndefined();
  });

  it('emitIr with 3 inputs', () => {
    const result = unionNode.emitIr!(
      unionConfig(false, 3),
      makeIrContext({input_0: '_qb_a', input_1: '_qb_b', input_2: '_qb_c'}, {}),
    );
    expect(result?.sql).toContain('UNION ALL');
    expect(result?.sql).toContain('_qb_a');
    expect(result?.sql).toContain('_qb_b');
    expect(result?.sql).toContain('_qb_c');
  });

  it('getOutputColumns returns first available input columns', () => {
    const ctx = makeColumnContext({
      input_0: [{name: 'x'}, {name: 'y'}],
      input_1: [{name: 'a'}, {name: 'b'}],
    });
    const result = unionNode.getOutputColumns!(
      {distinct: false, numInputs: 2},
      ctx,
    );
    expect(result).toEqual([{name: 'x'}, {name: 'y'}]);
  });

  it('getOutputColumns returns undefined when no input columns', () => {
    const ctx = makeColumnContext({});
    const result = unionNode.getOutputColumns!(
      {distinct: false, numInputs: 2},
      ctx,
    );
    expect(result).toBeUndefined();
  });

  it('isValid always returns true', () => {
    expect(unionNode.isValid({distinct: false, numInputs: 0})).toBe(true);
    expect(unionNode.isValid({distinct: true, numInputs: 5})).toBe(true);
  });

  it('defaultConfig', () => {
    expect(unionNode.defaultConfig()).toEqual({distinct: false, numInputs: 2});
  });
});

// ============================================================================
// SQL
// ============================================================================

describe('SQL manifest', () => {
  it('has correct metadata', () => {
    expect(sqlNode.title).toBe('SQL');
    expect(sqlNode.hue).toBe(280);
  });

  it('emitIr with no inputs returns raw SQL', () => {
    const result = sqlNode.emitIr!(
      sqlConfig('SELECT * FROM slice'),
      makeIrContext({}),
    );
    expect(result?.sql).toBe('SELECT * FROM slice');
  });

  it('emitIr with inputs wraps them in CTEs', () => {
    const result = sqlNode.emitIr!(
      sqlConfig('SELECT * FROM events', ['events']),
      makeIrContext({input_0: '_qb_1111'}),
    );
    expect(result?.sql).toContain('WITH');
    expect(result?.sql).toContain('events AS');
    expect(result?.sql).toContain('SELECT * FROM _qb_1111');
  });

  it('emitIr handles SQL that already starts with WITH', () => {
    const result = sqlNode.emitIr!(
      sqlConfig('WITH cte AS (SELECT 1) SELECT * FROM cte'),
      makeIrContext({}),
    );
    expect(result?.sql).toBe('WITH cte AS (SELECT 1) SELECT * FROM cte');
  });

  it('emitIr with multiple inputs', () => {
    const result = sqlNode.emitIr!(
      sqlConfig('SELECT * FROM a JOIN b ON a.id = b.id', ['a', 'b']),
      makeIrContext({input_0: '_qb_aaa', input_1: '_qb_bbb'}),
    );
    expect(result?.sql).toContain('WITH');
    expect(result?.sql).toContain('a AS');
    expect(result?.sql).toContain('b AS');
    expect(result?.sql).toContain('SELECT * FROM a JOIN b ON a.id = b.id');
  });

  it('emitIr with aliased inputs', () => {
    const result = sqlNode.emitIr!(
      sqlConfig('SELECT * FROM events', ['events']),
      makeIrContext({input_0: '_qb_1111'}),
    );
    expect(result?.sql).toContain('events AS');
  });

  it('getOutputColumns returns user-defined columns', () => {
    const ctx = makeColumnContext({});
    const result = sqlNode.getOutputColumns!(
      sqlConfig('SELECT 1', [], [{name: 'one', type: 'int'}]),
      ctx,
    );
    expect(result).toEqual([{name: 'one', type: {kind: 'int'}}]);
  });

  it('getOutputColumns returns undefined when no columns defined', () => {
    const ctx = makeColumnContext({});
    const result = sqlNode.getOutputColumns!(sqlConfig('SELECT 1'), ctx);
    expect(result).toBeUndefined();
  });

  it('isValid requires non-empty SQL', () => {
    expect(sqlNode.isValid({sql: ''})).toBe(false);
    expect(sqlNode.isValid({sql: '   '})).toBe(false);
    expect(sqlNode.isValid({sql: 'SELECT 1'})).toBe(true);
  });

  it('defaultConfig', () => {
    expect(sqlNode.defaultConfig()).toEqual({
      sql: 'SELECT\n  *\nFROM slice\nLIMIT 100',
      columns: [],
      inputPorts: [],
    });
  });
});

// ============================================================================
// Extend
// ============================================================================

describe('Extend manifest', () => {
  it('has correct metadata', () => {
    expect(extendNode.title).toBe('Extend');
    expect(extendNode.hue).toBe(125);
  });

  it('tryFold appends columns to SELECT', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = extendNode.tryFold!(
      stmt,
      extendConfig([{expression: 'dur * 1000', alias: 'dur_ms'}]),
    );
    expect(result).toBe(true);
    expect(stmt.columns).toBe('*, dur * 1000 AS dur_ms');
  });

  it('tryFold does NOT fold when groupBy is present', () => {
    const stmt: SqlStatement = {
      columns: '*',
      from: '_qb_abcd',
      groupBy: 'name',
    };
    const result = extendNode.tryFold!(
      stmt,
      extendConfig([{expression: 'dur * 1000', alias: 'dur_ms'}]),
    );
    expect(result).toBe(false);
  });

  it('tryFold with multiple entries', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    extendNode.tryFold!(
      stmt,
      extendConfig([
        {expression: 'dur * 1000', alias: 'dur_ms'},
        {expression: 'ts + 100', alias: 'ts_plus'},
      ]),
    );
    expect(stmt.columns).toBe('*, dur * 1000 AS dur_ms, ts + 100 AS ts_plus');
  });

  it('tryFold skips empty expressions', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    extendNode.tryFold!(
      stmt,
      extendConfig([
        {expression: '', alias: ''},
        {expression: 'dur', alias: 'd'},
      ]),
    );
    expect(stmt.columns).toBe('*, dur AS d');
  });

  it('getOutputColumns appends new column aliases', () => {
    const ctx = makeColumnContext({
      input: [{name: 'ts'}, {name: 'dur', type: {kind: 'duration'}}],
    });
    const result = extendNode.getOutputColumns!(
      {entries: [{expression: 'dur * 1000', alias: 'dur_ms'}]},
      ctx,
    );
    expect(result).toEqual([
      {name: 'ts'},
      {name: 'dur', type: {kind: 'duration'}},
      {name: 'dur_ms'},
    ]);
  });

  it('getOutputColumns returns input columns when no entries', () => {
    const ctx = makeColumnContext({input: [{name: 'ts'}]});
    const result = extendNode.getOutputColumns!({entries: []}, ctx);
    expect(result).toEqual([{name: 'ts'}]);
  });

  it('isValid rejects alias without expression', () => {
    expect(
      extendNode.isValid({entries: [{expression: '', alias: 'foo'}]}),
    ).toBe(false);
  });

  it('isValid accepts expression without alias', () => {
    expect(
      extendNode.isValid({entries: [{expression: 'dur', alias: ''}]}),
    ).toBe(true);
  });

  it('defaultConfig returns empty entries', () => {
    expect(extendNode.defaultConfig()).toEqual({entries: []});
  });
});

// ============================================================================
// Drop
// ============================================================================

describe('Drop manifest', () => {
  it('has correct metadata', () => {
    expect(dropNode.title).toBe('Drop');
    expect(dropNode.hue).toBe(5);
    expect(dropNode.icon).toBe('remove');
  });

  it('getOutputColumns removes dropped columns', () => {
    const ctx = makeColumnContext({
      input: [{name: 'ts'}, {name: 'dur', type: {kind: 'duration'}}],
    });
    const result = dropNode.getOutputColumns!({columns: ['ts']}, ctx);
    expect(result).toEqual([{name: 'dur', type: {kind: 'duration'}}]);
  });

  it('getOutputColumns returns all columns when nothing to drop', () => {
    const ctx = makeColumnContext({input: [{name: 'ts'}, {name: 'dur'}]});
    const result = dropNode.getOutputColumns!({columns: []}, ctx);
    expect(result).toEqual([{name: 'ts'}, {name: 'dur'}]);
  });

  it('getOutputColumns returns undefined when no input columns', () => {
    const ctx = makeColumnContext({});
    const result = dropNode.getOutputColumns!({columns: []}, ctx);
    expect(result).toBeUndefined();
  });

  it('emitIr drops columns from SELECT', () => {
    const result = dropNode.emitIr!(
      {columns: ['ts']},
      makeIrContext(
        {input: '_qb_abcd'},
        {input: [{name: 'ts'}, {name: 'dur'}]},
      ),
    );
    expect(result?.sql).toBe('SELECT dur\nFROM _qb_abcd');
  });

  it('emitIr produces SELECT * when no columns to drop', () => {
    const result = dropNode.emitIr!(
      {columns: []},
      makeIrContext({input: '_qb_abcd'}, {input: [{name: 'ts'}]}),
    );
    expect(result?.sql).toBe('SELECT *\nFROM _qb_abcd');
  });

  it('emitIr produces SELECT * when no input columns known', () => {
    const result = dropNode.emitIr!(
      {columns: ['ts']},
      makeIrContext({input: '_qb_abcd'}, {}),
    );
    expect(result?.sql).toBe('SELECT *\nFROM _qb_abcd');
  });

  it('isValid always returns true', () => {
    expect(dropNode.isValid({columns: []})).toBe(true);
    expect(dropNode.isValid({columns: ['ts']})).toBe(true);
  });

  it('defaultConfig', () => {
    expect(dropNode.defaultConfig()).toEqual({columns: []});
  });
});

// ============================================================================
// Limit
// ============================================================================

describe('Limit manifest', () => {
  it('has correct metadata', () => {
    expect(limitNode.title).toBe('Limit');
    expect(limitNode.hue).toBe(60);
  });

  it('tryFold appends LIMIT', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = limitNode.tryFold!(stmt, limitConfig('100'));
    expect(result).toBe(true);
    expect(stmt.limit).toBe(100);
  });

  it('tryFold does NOT fold when limit is already set', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd', limit: 50};
    const result = limitNode.tryFold!(stmt, limitConfig('100'));
    expect(result).toBe(false);
  });

  it('getOutputColumns passes through input columns', () => {
    const ctx = makeColumnContext({input: [{name: 'ts'}, {name: 'dur'}]});
    const result = limitNode.getOutputColumns!({limitCount: '10'}, ctx);
    expect(result).toEqual([{name: 'ts'}, {name: 'dur'}]);
  });

  it('isValid requires non-empty numeric limit', () => {
    expect(limitNode.isValid({limitCount: ''})).toBe(false);
    expect(limitNode.isValid({limitCount: 'abc'})).toBe(false);
    expect(limitNode.isValid({limitCount: '100'})).toBe(true);
    expect(limitNode.isValid({limitCount: '0'})).toBe(true);
  });

  it('defaultConfig', () => {
    expect(limitNode.defaultConfig()).toEqual({limitCount: '100'});
  });
});

// ============================================================================
// TimeRange
// ============================================================================

describe('TimeRange manifest', () => {
  it('has correct metadata', () => {
    expect(timeRangeNode.title).toBe('Time Range');
    expect(timeRangeNode.hue).toBe(15);
  });

  it('emitIr produces timestamp query', () => {
    const result = timeRangeNode.emitIr!(
      timeRangeConfig('1000', '5000'),
      makeIrContext({}),
    );
    expect(result?.sql).toBe('SELECT 0 AS id, 1000 AS ts, 5000 AS dur');
  });

  it('getOutputColumns returns id, ts, dur columns', () => {
    const result = timeRangeNode.getOutputColumns!(
      timeRangeConfig('0', '0'),
      makeColumnContext({}),
    );
    expect(result).toEqual([
      {name: 'id', type: {kind: 'int'}},
      {name: 'ts', type: {kind: 'timestamp'}},
      {name: 'dur', type: {kind: 'duration'}},
    ]);
  });

  it('isValid requires non-zero ts or dur', () => {
    expect(timeRangeNode.isValid({ts: '0', dur: '0'})).toBe(false);
    expect(timeRangeNode.isValid({ts: '1000', dur: '0'})).toBe(true);
    expect(timeRangeNode.isValid({ts: '0', dur: '5000'})).toBe(true);
    expect(timeRangeNode.isValid({ts: '1000', dur: '5000'})).toBe(true);
  });

  it('defaultConfig', () => {
    expect(timeRangeNode.defaultConfig()).toEqual({ts: '0', dur: '0'});
  });
});

// ============================================================================
// ExtractArg
// ============================================================================

describe('ExtractArg manifest', () => {
  it('has correct metadata', () => {
    expect(extractArgNode.title).toBe('Extract Args');
    expect(extractArgNode.hue).toBe(95);
  });

  it('tryFold appends extract_arg expressions', () => {
    const stmt: SqlStatement = {columns: '*', from: '_qb_abcd'};
    const result = extractArgNode.tryFold!(
      stmt,
      extractArgConfig('id', [{argName: 'pid', alias: ''}]),
    );
    expect(result).toBe(true);
    expect(stmt.columns).toContain("extract_arg(id, 'pid')");
  });

  it('tryFold does NOT fold when groupBy is present', () => {
    const stmt: SqlStatement = {
      columns: '*',
      from: '_qb_abcd',
      groupBy: 'name',
    };
    const result = extractArgNode.tryFold!(
      stmt,
      extractArgConfig('id', [{argName: 'pid', alias: ''}]),
    );
    expect(result).toBe(false);
  });

  it('getOutputColumns adds extracted column aliases', () => {
    const ctx = makeColumnContext({input: [{name: 'args'}]});
    const result = extractArgNode.getOutputColumns!(
      {argSetIdCol: 'id', extractions: [{argName: 'pid', alias: ''}]},
      ctx,
    );
    expect(result).toEqual([{name: 'args'}, {name: 'pid'}]);
  });

  it('getOutputColumns returns input columns when no extractions', () => {
    const ctx = makeColumnContext({input: [{name: 'args'}, {name: 'id'}]});
    const result = extractArgNode.getOutputColumns!(
      {argSetIdCol: 'id', extractions: []},
      ctx,
    );
    expect(result).toEqual([{name: 'args'}, {name: 'id'}]);
  });

  it('isValid always returns true (extractions always valid)', () => {
    expect(extractArgNode.isValid({argSetIdCol: '', extractions: []})).toBe(
      true,
    );
    expect(
      extractArgNode.isValid({
        argSetIdCol: 'id',
        extractions: [{argName: 'pid', alias: ''}],
      }),
    ).toBe(true);
  });

  it('defaultConfig', () => {
    expect(extractArgNode.defaultConfig()).toEqual({
      argSetIdCol: '',
      extractions: [],
    });
  });
});

// ============================================================================
// IntervalIntersect
// ============================================================================

describe('IntervalIntersect manifest', () => {
  it('has correct metadata', () => {
    expect(intervalIntersectNode.title).toBe('Interval Intersect');
    expect(intervalIntersectNode.hue).toBe(340);
  });

  it('has dynamic input ports based on numInputs', () => {
    const inputs = intervalIntersectNode.getInputs!({
      numInputs: 3,
      partitionColumns: [],
      filterNegativeDur: true,
    });
    expect(inputs).toHaveLength(3);
    expect(inputs[0].name).toBe('input_0');
    expect(inputs[2].name).toBe('input_2');
  });

  it('emitIr with 2 inputs produces _interval_intersect function call', () => {
    const result = intervalIntersectNode.emitIr!(
      intervalIntersectConfig(2, [], true),
      makeIrContext(
        {input_0: '_qb_aaaa', input_1: '_qb_bbbb'},
        {
          input_0: [{name: 'id', type: {kind: 'int'}}, {name: 'name'}],
          input_1: [{name: 'id', type: {kind: 'int'}}, {name: 'value'}],
        },
      ),
    );
    expect(result?.sql).toContain('_interval_intersect!');
    expect(result?.sql).toContain('_qb_aaaa');
    expect(result?.sql).toContain('_qb_bbbb');
    expect(result?.sql).toContain('ii.ts');
    expect(result?.sql).toContain('ii.dur');
    expect(result?.sql).toContain('ii.id_0');
    expect(result?.sql).toContain('ii.id_1');
    expect(result?.sql).toContain('t1.name');
    expect(result?.sql).toContain('t2.value');
  });

  it('emitIr with partition columns includes partition clause', () => {
    const result = intervalIntersectNode.emitIr!(
      intervalIntersectConfig(2, ['pid'], true),
      makeIrContext(
        {input_0: '_qb_a', input_1: '_qb_b'},
        {input_0: [{name: 'id'}], input_1: [{name: 'id'}]},
      ),
    );
    expect(result?.sql).toContain('pid');
  });

  it('emitIr with filterNegativeDur=false omits WHERE dur >= 0', () => {
    const result = intervalIntersectNode.emitIr!(
      intervalIntersectConfig(2, [], false),
      makeIrContext(
        {input_0: '_qb_a', input_1: '_qb_b'},
        {input_0: [{name: 'id'}], input_1: [{name: 'id'}]},
      ),
    );
    expect(result?.sql).not.toContain('WHERE dur >= 0');
  });

  it('emitIr with 0 connected inputs returns undefined', () => {
    const result = intervalIntersectNode.emitIr!(
      intervalIntersectConfig(2, [], true),
      makeIrContext({}, {}),
    );
    expect(result).toBeUndefined();
  });

  it('getOutputColumns returns ts, dur, id_N, and source columns', () => {
    const ctx = makeColumnContext({
      input_0: [{name: 'id', type: {kind: 'int'}}, {name: 'name'}],
      input_1: [{name: 'id', type: {kind: 'int'}}, {name: 'value'}],
    });
    const result = intervalIntersectNode.getOutputColumns!(
      {numInputs: 2, partitionColumns: [], filterNegativeDur: true},
      ctx,
    );
    expect(result).toContainEqual({name: 'ts', type: {kind: 'timestamp'}});
    expect(result).toContainEqual({name: 'dur', type: {kind: 'duration'}});
    expect(result).toContainEqual({name: 'id_0', type: {kind: 'int'}});
    expect(result).toContainEqual({name: 'id_1', type: {kind: 'int'}});
    expect(result).toContainEqual({name: 'name'});
    expect(result).toContainEqual({name: 'value'});
  });

  it('getOutputColumns returns undefined when no connected ports', () => {
    const ctx = makeColumnContext({});
    const result = intervalIntersectNode.getOutputColumns!(
      {numInputs: 2, partitionColumns: [], filterNegativeDur: true},
      ctx,
    );
    expect(result).toBeUndefined();
  });

  it('isValid always returns true', () => {
    expect(
      intervalIntersectNode.isValid({
        numInputs: 2,
        partitionColumns: [],
        filterNegativeDur: true,
      }),
    ).toBe(true);
    expect(
      intervalIntersectNode.isValid({
        numInputs: 5,
        partitionColumns: ['pid'],
        filterNegativeDur: false,
      }),
    ).toBe(true);
  });

  it('defaultConfig', () => {
    expect(intervalIntersectNode.defaultConfig()).toEqual({
      numInputs: 2,
      partitionColumns: [],
      filterNegativeDur: true,
    });
  });
});
