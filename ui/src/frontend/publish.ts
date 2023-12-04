// Copyright (C) 2021 The Android Open Source Project
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

import {time} from '../base/time';
import {Actions} from '../common/actions';
import {AggregateData, isEmptyData} from '../common/aggregation_data';
import {ConversionJobStatusUpdate} from '../common/conversion_jobs';
import {
  LogBoundsKey,
  LogEntriesKey,
  LogExists,
  LogExistsKey,
} from '../common/logs';
import {MetricResult} from '../common/metric_data';
import {CurrentSearchResults, SearchSummary} from '../common/search_data';
import {raf} from '../core/raf_scheduler';

import {
  CounterDetails,
  CpuProfileDetails,
  FlamegraphDetails,
  Flow,
  FtracePanelData,
  FtraceStat,
  globals,
  QuantizedLoad,
  SliceDetails,
  ThreadDesc,
  ThreadStateDetails,
} from './globals';
import {findCurrentSelection} from './keyboard_event_handler';

export function publishOverviewData(
    data: {[key: string]: QuantizedLoad|QuantizedLoad[]}) {
  for (const [key, value] of Object.entries(data)) {
    if (!globals.overviewStore.has(key)) {
      globals.overviewStore.set(key, []);
    }
    if (value instanceof Array) {
      globals.overviewStore.get(key)!.push(...value);
    } else {
      globals.overviewStore.get(key)!.push(value);
    }
  }
  raf.scheduleRedraw();
}

export function clearOverviewData() {
  globals.overviewStore.clear();
  raf.scheduleRedraw();
}

export function publishTrackData(args: {id: string, data: {}}) {
  globals.setTrackData(args.id, args.data);
  if ([LogExistsKey, LogBoundsKey, LogEntriesKey].includes(args.id)) {
    const data = globals.trackDataStore.get(LogExistsKey) as LogExists;
    if (data && data.exists) raf.scheduleFullRedraw();
  } else {
    raf.scheduleRedraw();
  }
}

export function publishMetricResult(metricResult: MetricResult) {
  globals.setMetricResult(metricResult);
  globals.publishRedraw();
}

export function publishSelectedFlows(selectedFlows: Flow[]) {
  globals.selectedFlows = selectedFlows;
  globals.publishRedraw();
}

export function publishCounterDetails(click: CounterDetails) {
  globals.counterDetails = click;
  globals.publishRedraw();
}

export function publishFlamegraphDetails(click: FlamegraphDetails) {
  globals.flamegraphDetails = click;
  globals.publishRedraw();
}

export function publishCpuProfileDetails(details: CpuProfileDetails) {
  globals.cpuProfileDetails = details;
  globals.publishRedraw();
}

export function publishFtraceCounters(counters: FtraceStat[]) {
  globals.ftraceCounters = counters;
  globals.publishRedraw();
}

export function publishRealtimeOffset(offset: time, utcOffset: time) {
  globals.realtimeOffset = offset;
  globals.utcOffset = utcOffset;
  globals.publishRedraw();
}

export function publishConversionJobStatusUpdate(
    job: ConversionJobStatusUpdate) {
  globals.setConversionJobStatus(job.jobName, job.jobStatus);
  globals.publishRedraw();
}

export function publishLoading(numQueuedQueries: number) {
  globals.numQueuedQueries = numQueuedQueries;
  // TODO(hjd): Clean up loadingAnimation given that this now causes a full
  // redraw anyways. Also this should probably just go via the global state.
  raf.scheduleFullRedraw();
}

export function publishBufferUsage(args: {percentage: number}) {
  globals.setBufferUsage(args.percentage);
  globals.publishRedraw();
}

export function publishSearch(args: SearchSummary) {
  globals.searchSummary = args;
  globals.publishRedraw();
}

export function publishSearchResult(args: CurrentSearchResults) {
  globals.currentSearchResults = args;
  globals.publishRedraw();
}

export function publishRecordingLog(args: {logs: string}) {
  globals.setRecordingLog(args.logs);
  globals.publishRedraw();
}

export function publishTraceErrors(numErrors: number) {
  globals.setTraceErrors(numErrors);
  globals.publishRedraw();
}

export function publishMetricError(error: string) {
  globals.setMetricError(error);
  globals.logging.logError(error, false);
  globals.publishRedraw();
}

export function publishAggregateData(
    args: {data: AggregateData, kind: string}) {
  globals.setAggregateData(args.kind, args.data);
  if (!isEmptyData(args.data)) {
    globals.dispatch(Actions.setCurrentTab({tab: args.data.tabName}));
  }
  globals.publishRedraw();
}

export function publishQueryResult(args: {id: string, data?: {}}) {
  globals.queryResults.set(args.id, args.data);
  globals.dispatch(Actions.setCurrentTab({tab: `query_result_${args.id}`}));
  globals.publishRedraw();
}

export function publishThreads(data: ThreadDesc[]) {
  globals.threads.clear();
  data.forEach((thread) => {
    globals.threads.set(thread.utid, thread);
  });
  globals.publishRedraw();
}

export function publishSliceDetails(click: SliceDetails) {
  globals.sliceDetails = click;
  const id = click.id;
  if (id !== undefined && id === globals.state.pendingScrollId) {
    findCurrentSelection();
    globals.dispatch(Actions.setCurrentTab({tab: 'slice'}));
    globals.dispatch(Actions.clearPendingScrollId({id: undefined}));
  }
  globals.publishRedraw();
}

export function publishThreadStateDetails(click: ThreadStateDetails) {
  globals.threadStateDetails = click;
  globals.publishRedraw();
}

export function publishConnectedFlows(connectedFlows: Flow[]) {
  globals.connectedFlows = connectedFlows;
  // If a chrome slice is selected and we have any flows in connectedFlows
  // we will find the flows on the right and left of that slice to set a default
  // focus. In all other cases the focusedFlowId(Left|Right) will be set to -1.
  globals.dispatch(Actions.setHighlightedFlowLeftId({flowId: -1}));
  globals.dispatch(Actions.setHighlightedFlowRightId({flowId: -1}));
  if (globals.state.currentSelection?.kind === 'CHROME_SLICE') {
    const sliceId = globals.state.currentSelection.id;
    for (const flow of globals.connectedFlows) {
      if (flow.begin.sliceId === sliceId) {
        globals.dispatch(Actions.setHighlightedFlowRightId({flowId: flow.id}));
      }
      if (flow.end.sliceId === sliceId) {
        globals.dispatch(Actions.setHighlightedFlowLeftId({flowId: flow.id}));
      }
    }
  }

  globals.publishRedraw();
}

export function publishFtracePanelData(data: FtracePanelData) {
  globals.ftracePanelData = data;
  globals.publishRedraw();
}
