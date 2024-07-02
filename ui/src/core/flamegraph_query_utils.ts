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

import {AsyncDisposableStack} from '../base/disposable';

import {Engine} from '../trace_processor/engine';
import {NUM, STR} from '../trace_processor/query_result';
import {createPerfettoTable} from '../trace_processor/sql_utils';

export async function computeFlamegraphTree(
  engine: Engine,
  dependencySql: string,
  sql: string,
  {
    showStack,
    hideStack,
    showFrame,
    hideFrame,
  }: {
    readonly showStack: ReadonlyArray<string>;
    readonly hideStack: ReadonlyArray<string>;
    readonly showFrame: ReadonlyArray<string>;
    readonly hideFrame: ReadonlyArray<string>;
  },
) {
  const allStackBits = (1 << showStack.length) - 1;
  const showStackFilter =
    showStack.length === 0
      ? '0'
      : showStack.map((x, i) => `((name like '%${x}%') << ${i})`).join(' | ');
  const hideStackFilter =
    hideStack.length === 0
      ? 'false'
      : hideStack.map((x) => `name like '%${x}%'`).join(' OR ');
  const showFrameFilter =
    showFrame.length === 0
      ? 'true'
      : showFrame.map((x) => `name like '%${x}%'`).join(' OR ');
  const hideFrameFilter =
    hideFrame.length === 0
      ? 'false'
      : hideFrame.map((x) => `name like '%${x}%'`).join(' OR ');

  await engine.query(dependencySql);
  await engine.query(`include perfetto module viz.flamegraph;`);

  const disposable = new AsyncDisposableStack();
  try {
    disposable.use(
      await createPerfettoTable(
        engine,
        '_flamegraph_source',
        `
        select *
        from _viz_flamegraph_prepare_filter!(
          (${sql}),
          (${showFrameFilter}),
          (${hideFrameFilter}),
          (${showStackFilter}),
          (${hideStackFilter}),
          ${1 << showStack.length}
        )
      `,
      ),
    );
    disposable.use(
      await createPerfettoTable(
        engine,
        '_flamegraph_raw_top_down',
        `select * from _viz_flamegraph_filter_and_hash!(_flamegraph_source)`,
      ),
    );
    disposable.use(
      await createPerfettoTable(
        engine,
        '_flamegraph_top_down',
        `
        select * from _viz_flamegraph_merge_hashes!(
          _flamegraph_raw_top_down,
          _flamegraph_source
        )
      `,
      ),
    );
    disposable.use(
      await createPerfettoTable(
        engine,
        '_flamegraph_raw_bottom_up',
        `
        select *
        from _viz_flamegraph_accumulate!(_flamegraph_top_down, ${allStackBits})
      `,
      ),
    );
    disposable.use(
      await createPerfettoTable(
        engine,
        '_flamegraph_windowed',
        `
        select *
        from _viz_flamegraph_local_layout!(
          _flamegraph_raw_bottom_up,
          _flamegraph_top_down
        );
      `,
      ),
    );
    const res = await engine.query(`
      select *
      from _viz_flamegraph_global_layout!(
        _flamegraph_windowed,
        _flamegraph_raw_bottom_up,
        _flamegraph_top_down
      )
    `);
    const it = res.iter({
      id: NUM,
      parentId: NUM,
      depth: NUM,
      name: STR,
      selfValue: NUM,
      cumulativeValue: NUM,
      xStart: NUM,
      xEnd: NUM,
    });
    let allRootsCumulativeValue = 0;
    let maxDepth = 0;
    const nodes = [];
    for (; it.valid(); it.next()) {
      nodes.push({
        id: it.id,
        parentId: it.parentId,
        depth: it.depth,
        name: it.name,
        selfValue: it.selfValue,
        cumulativeValue: it.cumulativeValue,
        xStart: it.xStart,
        xEnd: it.xEnd,
      });
      if (it.parentId === -1) {
        allRootsCumulativeValue += it.cumulativeValue;
      }
      maxDepth = Math.max(maxDepth, it.depth);
    }
    return {nodes, allRootsCumulativeValue, maxDepth};
  } finally {
    await disposable.disposeAsync();
  }
}
