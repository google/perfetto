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

import {Time} from '../../base/time';
import type {Trace} from '../../public/trace';
import {parseSliceIds} from './actions';
import {
  type StitchedSelection,
  StitchedPipeline,
  viewForDirection,
} from './metrics';
import {STITCHED_PROVIDERS} from './providers';
import {type StitchedSqlModule, StitchedSqlModules} from './sql_modules';
import type {Engine} from '../../trace_processor/engine';

const SELECTION: StitchedSelection = {
  trackIds: [3, 7],
  start: Time.fromRaw(100n),
  end: Time.fromRaw(200n),
  direction: 'DOWN',
};

// The metrics never touch the trace when built, only when their actions run.
const TRACE = {} as Trace;

describe('StitchedFlamegraph parseSliceIds', () => {
  test('parses the merged ids of a node', () => {
    expect(parseSliceIds(undefined)).toEqual([]);
    expect(parseSliceIds('')).toEqual([]);
    expect(parseSliceIds('42')).toEqual([42]);
    expect(parseSliceIds('1, 2,1,,3')).toEqual([1, 2, 3]);
  });

  test('drops anything which is not a slice id', () => {
    expect(parseSliceIds('-1,abc,1.5,7')).toEqual([7]);
  });
});

describe('StitchedFlamegraph viewForDirection', () => {
  test('shows the selection on top in both directions', () => {
    expect(viewForDirection('DOWN')).toEqual({kind: 'TOP_DOWN'});
    expect(viewForDirection('UP')).toEqual({kind: 'BOTTOM_UP'});
  });
});

describe('StitchedFlamegraph StitchedPipeline', () => {
  test('materializes each step in a prefixed table', () => {
    const p = new StitchedPipeline('_p', ['callstacks.stack_profile']);
    const a = p.table('a', 'select 1 as id', 'id');
    const b = p.table('b', `select * from ${a}`);
    expect([a, b]).toEqual(['_p_a', '_p_b']);
    expect(p.tables).toEqual(['_p_a', '_p_b']);
    expect(p.sql).toBe(
      [
        'include perfetto module callstacks.stack_profile;',
        'create or replace perfetto table _p_a as select 1 as id;',
        'create or replace perfetto index _p_a_idx on _p_a(id);',
        'create or replace perfetto table _p_b as select * from _p_a;',
      ].join('\n'),
    );
  });
});

describe('StitchedFlamegraph StitchedSqlModules', () => {
  test('runs each module once, after its dependencies', async () => {
    const queries: string[] = [];
    const engine = {
      query: async (sql: string) => {
        queries.push(sql);
      },
    } as unknown as Engine;
    const core: StitchedSqlModule = {name: 'core', sql: 'core sql'};
    const a: StitchedSqlModule = {name: 'a', sql: 'a sql', deps: [core]};
    const b: StitchedSqlModule = {name: 'b', sql: 'b sql', deps: [core]};
    const modules = new StitchedSqlModules(engine);
    await modules.ensure([a]);
    await modules.ensure([b, a]);
    expect(queries).toEqual(['core sql', 'a sql', 'b sql']);
  });

  test('retries a module whose run failed', async () => {
    let fail = true;
    const engine = {
      query: async () => {
        if (fail) throw new Error('boom');
      },
    } as unknown as Engine;
    const modules = new StitchedSqlModules(engine);
    const m: StitchedSqlModule = {name: 'm', sql: 'm sql'};
    await expect(modules.ensure([m])).rejects.toThrow('boom');
    fail = false;
    await expect(modules.ensure([m])).resolves.toBeUndefined();
  });
});

describe('StitchedFlamegraph providers', () => {
  for (const direction of ['DOWN', 'UP'] as const) {
    test(`build well formed ${direction} metrics`, () => {
      const prefix = '_t';
      const metricIds = new Set<string>();
      for (const provider of STITCHED_PROVIDERS) {
        const selection = {...SELECTION, direction};
        expect(provider.modules(selection).length).toBeGreaterThan(0);
        const res = provider.metrics(TRACE, selection, prefix);
        expect(res.metrics.length).toBeGreaterThan(0);
        for (const t of res.tables) {
          expect(t.startsWith(`${prefix}_`)).toBe(true);
        }
        // Every table the provider creates is one it reports for cleanup.
        const sqls = [
          ...res.prepare.map((p) => p.sql),
          ...res.metrics.map((m) => m.dependencySql ?? ''),
        ];
        for (const sql of sqls) {
          for (const [, name] of sql.matchAll(
            /create or replace perfetto table (\w+)/g,
          )) {
            expect(res.tables).toContain(name);
          }
        }
        for (const metric of res.metrics) {
          const id = metric.id ?? metric.name;
          expect(metricIds.has(id)).toBe(false);
          metricIds.add(id);
          expect(metric.statement).toContain('_stitched_finalize!');
        }
      }
    });
  }

  test('share the flows pipeline between providers', () => {
    const sqls = STITCHED_PROVIDERS.flatMap((provider) =>
      provider.metrics(TRACE, SELECTION, '_t').prepare.map((p) => p.sql),
    );
    const flows = sqls.filter((sql) => sql.includes('_t_flows_seeds as'));
    expect(flows.length).toBe(STITCHED_PROVIDERS.length);
    expect(new Set(flows).size).toBe(1);
  });
});
