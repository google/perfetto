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

import m from 'mithril';
import {AsyncMemo} from '../../base/async_memo';
import type {Engine} from '../../trace_processor/engine';
import {NUM} from '../../trace_processor/query_result';
import type {Trace} from '../../public/trace';
import {EmptyState} from '../../widgets/empty_state';
import {
  extractSnapshotData,
  type SnapshotData,
} from '../dev.perfetto.Memscope/live';
import {Dashboard} from './dashboard';

export const SYSMEM_TRACE_ROUTE = '/sysmem_trace';

// Whether the trace has the system or per-process memory counters the
// dashboard is built from (linux.sys_stats meminfo, linux.process_stats).
export async function traceHasSysmemData(engine: Engine): Promise<boolean> {
  const res = await engine.query(`
    SELECT
      EXISTS(SELECT 1 FROM counter_track WHERE name = 'MemTotal')
      OR EXISTS(SELECT 1 FROM process_counter_track WHERE name = 'mem.rss')
      AS has
  `);
  return res.firstRow({has: NUM}).has !== 0;
}

interface TraceDashboardAttrs {
  readonly trace: Trace;
}

// The live dashboard, read-only, over a loaded trace.
export class TraceDashboardPage implements m.ClassComponent<TraceDashboardAttrs> {
  private readonly memo = new AsyncMemo<SnapshotData | undefined>();

  onremove() {
    this.memo.dispose();
  }

  view({attrs}: m.CVnode<TraceDashboardAttrs>): m.Children {
    const {trace} = attrs;
    const res = this.memo.use({
      key: trace.traceInfo.uuid,
      compute: async () => {
        if (!(await traceHasSysmemData(trace.engine))) return undefined;
        return (await extractSnapshotData(trace.engine)).data;
      },
    });
    if (res.isPending) {
      return m(EmptyState, {icon: 'hourglass', title: 'Loading trace...'});
    }
    if (res.data === undefined) {
      return m(
        EmptyState,
        {icon: 'monitoring', title: 'No memory counters in this trace'},
        'Record with the linux.sys_stats (meminfo) or linux.process_stats ' +
          'data sources to see system memory here.',
      );
    }
    return m(Dashboard, {
      app: trace,
      data: res.data,
      title: trace.traceInfo.traceTitle,
    });
  }
}
