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
  table,
  pipelineRef,
  slices,
  sql,
  intervalIntersect,
  union,
  PfgraphModule,
} from './pfgraph_query_builder';

describe('PfgraphPipeline', () => {
  test('simple table source', () => {
    expect(table('slice').render()).toBe("  table('slice')");
  });

  test('pipeline reference source', () => {
    expect(pipelineRef('_my_pipeline').render()).toBe('  _my_pipeline');
  });

  test('slices source', () => {
    expect(slices({name: 'binder*', process: 'com.*'}).render()).toBe(
      "  slices(name: 'binder*', process: 'com.*')",
    );
  });

  test('sql source single line', () => {
    expect(sql('SELECT id FROM slice').render()).toBe(
      "  sql('SELECT id FROM slice')",
    );
  });

  test('sql source multi-line', () => {
    const result = sql('SELECT id\nFROM slice\nWHERE dur > 0').render();
    expect(result).toBe(
      "  sql('''SELECT id\nFROM slice\nWHERE dur > 0''')",
    );
  });

  test('interval_intersect source', () => {
    expect(
      intervalIntersect({inputs: ['a', 'b'], partition: ['utid']}).render(),
    ).toBe('  interval_intersect(a, b, partition: [utid])');
  });

  test('union source', () => {
    expect(union('a', 'b', 'c').render()).toBe('  union(a, b, c)');
  });

  test('filter', () => {
    expect(table('slice').filter('dur > 1000').render()).toBe(
      "  table('slice')\n  .filter(dur > 1000)",
    );
  });

  test('select', () => {
    expect(table('slice').select('id', 'ts', 'dur').render()).toBe(
      "  table('slice')\n  .select(id, ts, dur)",
    );
  });

  test('computed', () => {
    expect(
      table('slice').computed({end_ts: 'ts + dur', is_long: 'dur > 1e6'})
        .render(),
    ).toBe(
      "  table('slice')\n  .computed(end_ts: ts + dur, is_long: dur > 1e6)",
    );
  });

  test('group_by with agg', () => {
    const result = table('slice')
      .groupBy('process_name', 'state')
      .agg({total_dur: 'sum(dur)', count: 'count()'})
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      '  .group_by(process_name, state)\n' +
      '  .agg(total_dur: sum(dur), count: count())',
    );
  });

  test('sort', () => {
    expect(table('slice').sort('dur DESC', 'ts ASC').render()).toBe(
      "  table('slice')\n  .sort(dur DESC, ts ASC)",
    );
  });

  test('limit and offset', () => {
    expect(table('slice').limit(10).offset(20).render()).toBe(
      "  table('slice')\n  .limit(10)\n  .offset(20)",
    );
  });

  test('distinct', () => {
    expect(table('slice').distinct().render()).toBe(
      "  table('slice')\n  .distinct()",
    );
  });

  test('add_columns', () => {
    const result = table('slice')
      .addColumns({from: 'process', on: 'upid = upid', cols: ['name AS process_name', 'pid']})
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      "  .add_columns(from: table('process'), on: upid = upid, cols: [name AS process_name, pid])",
    );
  });

  test('join operation', () => {
    const result = table('slice')
      .join({right: 'thread', on: 'utid = thread.utid', type: 'LEFT'})
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      '  .join(thread, on: utid = thread.utid, type: LEFT)',
    );
  });

  test('join operation INNER (default, omitted)', () => {
    const result = table('slice')
      .join({right: 'thread', on: 'utid = thread.utid'})
      .render();
    expect(result).toBe(
      "  table('slice')\n  .join(thread, on: utid = thread.utid)",
    );
  });

  test('cross_join', () => {
    expect(table('slice').crossJoin('trace_bounds').render()).toBe(
      "  table('slice')\n  .cross_join(trace_bounds)",
    );
  });

  test('span_join', () => {
    const result = table('slice')
      .spanJoin({right: 'thread_state', partition: ['utid'], type: 'LEFT'})
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      '  .span_join(thread_state, partition: [utid], type: LEFT)',
    );
  });

  test('filter_during', () => {
    const result = table('slice')
      .filterDuring({intervals: 'startup', partition: ['upid'], clip: false})
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      '  .filter_during(startup, partition: [upid], clip: false)',
    );
  });

  test('filter_in', () => {
    const result = table('slice')
      .filterIn({match: 'valid_ids', baseCol: 'id', matchCol: 'id'})
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      '  .filter_in(valid_ids, base_col: id, match_col: id)',
    );
  });

  test('except', () => {
    expect(table('slice').except('broken_entries').render()).toBe(
      "  table('slice')\n  .except(broken_entries)",
    );
  });

  test('counter_to_intervals', () => {
    expect(table('counter').counterToIntervals().render()).toBe(
      "  table('counter')\n  .counter_to_intervals()",
    );
  });

  test('classify', () => {
    const result = table('slice')
      .classify({
        column: 'gc_type',
        from: 'gc_name',
        rules: {'*NativeAlloc*': 'native_alloc', '*young*': 'young', '_': 'full'},
      })
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      "  .classify(gc_type, from: gc_name, '*NativeAlloc*' => native_alloc, '*young*' => young, '_' => full)",
    );
  });

  test('extract_args', () => {
    const result = table('slice')
      .extractArgs({event_type: 'event.type', event_seq: 'event.seq'})
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      "  .extract_args(event_type: 'event.type', event_seq: 'event.seq')",
    );
  });

  test('find_ancestor', () => {
    const result = table('slice')
      .findAncestor({where: "_anc.name = 'binder reply'", cols: ['id AS binder_reply_id']})
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      "  .find_ancestor(where: _anc.name = 'binder reply', cols: [id AS binder_reply_id])",
    );
  });

  test('find_descendant', () => {
    const result = table('slice')
      .findDescendant({where: "_desc.name = 'Foo'", cols: ['name AS foo_name']})
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      "  .find_descendant(where: _desc.name = 'Foo', cols: [name AS foo_name])",
    );
  });

  test('flow_reachable default', () => {
    expect(table('slice').flowReachable().render()).toBe(
      "  table('slice')\n  .flow_reachable()",
    );
  });

  test('flow_reachable in', () => {
    expect(table('slice').flowReachable({direction: 'in'}).render()).toBe(
      "  table('slice')\n  .flow_reachable(direction: in)",
    );
  });

  test('flatten_intervals', () => {
    expect(table('slice').flattenIntervals().render()).toBe(
      "  table('slice')\n  .flatten_intervals()",
    );
  });

  test('merge_overlapping', () => {
    const result = table('slice')
      .mergeOverlapping({epsilon: 100, partition: ['utid']})
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      '  .merge_overlapping(epsilon: 100, partition: [utid])',
    );
  });

  test('graph_reachable', () => {
    const result = table('nodes')
      .graphReachable({edges: '_edges', method: 'bfs'})
      .render();
    expect(result).toBe(
      "  table('nodes')\n  .graph_reachable(_edges, method: bfs)",
    );
  });

  test('parse_name', () => {
    expect(table('slice').parseName('owner {thread} ({tid})').render()).toBe(
      "  table('slice')\n  .parse_name('owner {thread} ({tid})')",
    );
  });

  test('window', () => {
    const result = table('thread_state')
      .window({
        prev_state: {expr: 'lag(state)', partition: ['utid'], order: 'ts'},
      })
      .render();
    expect(result).toBe(
      "  table('thread_state')\n" +
      '  .window(prev_state: lag(state), partition: [utid], order: ts)',
    );
  });

  test('unpivot', () => {
    const result = table('wide')
      .unpivot({columns: ['a', 'b', 'c'], nameCol: 'metric', valueCol: 'val'})
      .render();
    expect(result).toBe(
      "  table('wide')\n" +
      '  .unpivot(columns: [a, b, c], name_col: metric, value_col: val)',
    );
  });

  test('pivot', () => {
    const result = table('long')
      .pivot({from: 'state', value: 'dur', agg: 'sum', values: {R: 'running', S: 'sleeping'}})
      .render();
    expect(result).toBe(
      "  table('long')\n" +
      "  .pivot(from: state, value: dur, agg: sum, values: {'R' => running, 'S' => sleeping})",
    );
  });

  test('self_join_temporal', () => {
    const result = table('slice')
      .selfJoinTemporal({leftKey: 'utid', rightKey: 'utid', overlap: 'intersects'})
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      '  .self_join_temporal(left_key: utid, right_key: utid, overlap: intersects)',
    );
  });

  test('index', () => {
    expect(table('slice').index('utid', 'ts').render()).toBe(
      "  table('slice')\n  .index(utid, ts)",
    );
  });

  test('chained operations', () => {
    const result = table('slice')
      .filter('dur > 1000')
      .select('id', 'ts', 'dur', 'name')
      .sort('dur DESC')
      .limit(100)
      .render();
    expect(result).toBe(
      "  table('slice')\n" +
      '  .filter(dur > 1000)\n' +
      '  .select(id, ts, dur, name)\n' +
      '  .sort(dur DESC)\n' +
      '  .limit(100)',
    );
  });

  test('renderAs intermediate', () => {
    const result = table('slice').filter('dur > 0').renderAs('_my_step');
    expect(result).toBe(
      "_my_step:\n  table('slice')\n  .filter(dur > 0)",
    );
  });

  test('renderAs table', () => {
    const result = table('slice')
      .filter('dur > 0')
      .renderAs('my_output', {type: 'table'});
    expect(result).toBe(
      "@table my_output:\n  table('slice')\n  .filter(dur > 0)",
    );
  });

  test('renderAs view', () => {
    const result = table('slice')
      .select('id', 'ts')
      .renderAs('my_view', {type: 'view'});
    expect(result).toBe(
      "@view my_view:\n  table('slice')\n  .select(id, ts)",
    );
  });

  test('renderAs with index', () => {
    const result = table('counter')
      .counterToIntervals()
      .renderAs('_oom', {type: 'table', index: ['upid', 'ts']});
    expect(result).toBe(
      "@table _oom:\n  table('counter')\n  .counter_to_intervals()\n  .index(upid, ts)",
    );
  });

  test('custom indent', () => {
    expect(table('slice').filter('dur > 0').render(4)).toBe(
      "    table('slice')\n    .filter(dur > 0)",
    );
  });
});

describe('PfgraphModule', () => {
  test('renders a complete module', () => {
    const mod = new PfgraphModule('android.binder')
      .import('android.process_metadata', 'slices.with_context')
      .addPipeline(
        '_filtered_slices',
        table('slice').filter("name GLOB 'binder*'"),
      )
      .addPipeline(
        'android_binder_metrics',
        pipelineRef('_filtered_slices')
          .groupBy('process_name')
          .agg({count: 'count()'}),
        {type: 'view'},
      );

    expect(mod.render()).toBe(
      'module android.binder\n' +
      '\n' +
      'import android.process_metadata\n' +
      'import slices.with_context\n' +
      '\n' +
      "_filtered_slices:\n" +
      "  table('slice')\n" +
      "  .filter(name GLOB 'binder*')\n" +
      '\n' +
      '@view android_binder_metrics:\n' +
      '  _filtered_slices\n' +
      '  .group_by(process_name)\n' +
      '  .agg(count: count())',
    );
  });
});
