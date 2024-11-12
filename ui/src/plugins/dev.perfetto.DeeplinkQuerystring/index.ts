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

// When deep linking into Perfetto UI it is possible to pass arguments in the
// query string to automatically select a slice or run a query once the
// trace is loaded. This plugin deals with kicking off the relevant logic
// once the trace has loaded.

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {addQueryResultsTab} from '../../public/lib/query_table/query_result_tab';
import {Time} from '../../base/time';
import {RouteArgs} from '../../public/route_schema';
import {App} from '../../public/app';
import {exists} from '../../base/utils';
import {NUM} from '../../trace_processor/query_result';

let routeArgsForFirstTrace: RouteArgs | undefined;

/**
 * Uses URL args (table, ts, dur) to select events on trace load.
 *
 * E.g. ?table=thread_state&ts=39978672284068&dur=18995809
 *
 * Note: `ts` and `dur` are used rather than id as id is not stable over TP
 * versions.
 *
 * The table passed must have `ts`, `dur` (if a dur value is supplied) and `id`
 * columns, and SQL resolvers must be available for those tables (usually from
 * plugins).
 */
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.DeeplinkQuerystring';

  static onActivate(app: App): void {
    routeArgsForFirstTrace = app.initialRouteArgs;
  }

  async onTraceLoad(trace: Trace) {
    trace.addEventListener('traceready', async () => {
      const initialRouteArgs = routeArgsForFirstTrace;
      routeArgsForFirstTrace = undefined;
      if (initialRouteArgs === undefined) return;

      await selectInitialRouteArgs(trace, initialRouteArgs);
      if (
        initialRouteArgs.visStart !== undefined &&
        initialRouteArgs.visEnd !== undefined
      ) {
        zoomPendingDeeplink(
          trace,
          initialRouteArgs.visStart,
          initialRouteArgs.visEnd,
        );
      }
      if (initialRouteArgs.query !== undefined) {
        addQueryResultsTab(trace, {
          query: initialRouteArgs.query,
          title: 'Deeplink Query',
        });
      }
    });
  }
}

function zoomPendingDeeplink(trace: Trace, visStart: string, visEnd: string) {
  const visualStart = Time.fromRaw(BigInt(visStart));
  const visualEnd = Time.fromRaw(BigInt(visEnd));
  if (
    !(
      visualStart < visualEnd &&
      trace.traceInfo.start <= visualStart &&
      visualEnd <= trace.traceInfo.end
    )
  ) {
    return;
  }
  trace.timeline.setViewportTime(visualStart, visualEnd);
}

async function selectInitialRouteArgs(trace: Trace, args: RouteArgs) {
  const {table = 'slice', ts, dur} = args;

  // We need at least a ts
  if (!exists(ts)) {
    return;
  }

  const conditions = [];
  conditions.push(`ts = ${ts}`);
  exists(dur) && conditions.push(`dur = ${dur}`);

  // Find the id of the slice with this ts & dur in the given table
  const result = await trace.engine.query(`
    select
      id
    from
      ${table}
    where ${conditions.join(' AND ')}
  `);

  if (result.numRows() === 0) {
    return;
  }

  const {id} = result.firstRow({
    id: NUM,
  });

  trace.selection.selectSqlEvent(table, id, {
    scrollToSelection: true,
    switchToCurrentSelectionTab: false,
  });
}
