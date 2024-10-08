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
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {NUM, STR} from '../../trace_processor/query_result';
import {addQueryResultsTab} from '../../public/lib/query_table/query_result_tab';
import {Time} from '../../base/time';
import {RouteArgs} from '../../public/route_schema';
import {App} from '../../public/app';

class DeeplinkQuerystring implements PerfettoPlugin {
  private routeArgsForFirstTrace?: RouteArgs;

  onActivate(app: App): void {
    this.routeArgsForFirstTrace = app.initialRouteArgs;
  }

  async onTraceReady(trace: Trace): Promise<void> {
    const initialRouteArgs = this.routeArgsForFirstTrace;
    this.routeArgsForFirstTrace = undefined;
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
  const conditions = [];
  const {ts, dur} = args;

  if (ts !== undefined) {
    conditions.push(`ts = ${ts}`);
  }
  if (dur !== undefined) {
    conditions.push(`dur = ${dur}`);
  }

  if (conditions.length === 0) {
    return;
  }

  const query = `
      select
        id,
        track_id as traceProcessorTrackId,
        type
      from slice
      where ${conditions.join(' and ')}
    ;`;

  const result = await trace.engine.query(query);
  if (result.numRows() > 0) {
    const row = result.firstRow({
      id: NUM,
      traceProcessorTrackId: NUM,
      type: STR,
    });

    const id = row.traceProcessorTrackId;
    const track = trace.workspace.flatTracks.find(
      (t) =>
        t.uri && trace.tracks.getTrack(t.uri)?.tags?.trackIds?.includes(id),
    );
    if (track === undefined) {
      return;
    }
    trace.selection.selectSqlEvent('slice', row.id, {
      scrollToSelection: true,
      switchToCurrentSelectionTab: false,
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.DeeplinkQuerystring',
  plugin: DeeplinkQuerystring,
};
