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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import m from 'mithril';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {VitalTimestampDetailView} from './detail_view';
import {reConstructElementTree} from '../lynx.element/utils';
import {findAfterwardIdenticalFlowIdSlice} from '../../lynx_perf/trace_utils';
import {
  CRUCIAL_TIMING_KEYS,
  PIPELINE_ID,
  TIMING_MARK_FRAMEWORK_PREFIX,
  TIMING_MARK_PREFIX,
} from '../../lynx_perf/constants';
import {TTraceEvent} from '../../metrics_chart/types';
import {LynxElement} from '../../lynx_perf/common_components/element_tree/types';
import {getSlice, SliceDetails} from '../../components/sql_utils/slice';
import {asArgSetId, asSliceSqlId} from '../../components/sql_utils/core_types';
import {Arg, getArgs} from '../../components/sql_utils/args';
import {eventLoggerState} from '../../event_logger';

/**
 * Pipeline Stage Interface
 * 
 * Represents a single stage in the performance pipeline with timing information
 * and related slice IDs for start/end markers.
 */
export interface PipelineStage {
  id: number;
  endId: number;
  name: string;
  threadName: string;
  ts: number;
  dur: number;
}

/**
 * Pipeline Timestamp Interface
 * 
 * Basic timing marker with ID, name and timestamp information.
 */
export interface PipelineTimeStamp {
  id: number;
  name: string;
  threadName: string;
  ts: number;
}

const TIMING_START = 'Start';
const TIMING_END = 'End';
const FRAMEWORK_TIMING_START = '_start';
const FRAMEWORK_TIMING_END = '_end';

/**
 * Vital Timestamp Details Panel
 * 
 * Analyzes and visualizes performance pipeline stages from trace data.
 * Handles:
 * - Pipeline stage identification
 * - Timing correlation (start/end pairs)
 * - Element tree reconstruction
 * - Performance visualization
 */
export class VitalTimestampDetailsPanel implements TrackEventDetailsPanel {
  private loading: boolean;
  private ctx: Trace;
  private sliceDetail?: SliceDetails;
  private pipelineStages: PipelineStage[];
  private chartEvents: TTraceEvent[];
  private elementTree?: LynxElement;

  constructor(ctx: Trace) {
    this.ctx = ctx;
    this.loading = false;
    this.pipelineStages = [];
    this.chartEvents = [];
  }

  /**
   * Loads and processes pipeline data for a selected trace event
   * @param selection - Contains event ID to analyze
   * @remarks
   * Performs several key operations:
   * 1. Retrieves base slice details
   * 2. Finds all related pipeline stages
   * 3. Correlates start/end markers
   * 4. Builds visualization data
   * 5. Optionally reconstructs element tree
   */
  async load({eventId}: TrackEventSelection) {
    this.loading = true;
    this.pipelineStages = [];
    this.chartEvents = [];
    this.sliceDetail = await getSlice(this.ctx.engine, asSliceSqlId(eventId));
    if (this.sliceDetail) {
      this.sliceDetail.name = this.sliceDetail.name.replace(
        TIMING_MARK_PREFIX,
        '',
      );
    }
    const pipelineId = this.sliceDetail?.args?.find(
      (value: Arg) => value.key === `debug.${PIPELINE_ID}`,
    )?.value as string;
    // find all the trace with same arg value
    const queryRes = await this.ctx.engine.query(
      `select 
      slice.ts as ts, 
      slice.id as id,
      slice.name as name, 
      slice.arg_set_id as argSetId,
      args.key as key,
      args.string_value as stringValue,
      (IFNULL(thread.name, "Thread")) as threadName
      from slice 
      inner join args on args.arg_set_id = slice.arg_set_id
      left join thread_track on thread_track.id = slice.track_id
      left join thread on thread.utid = thread_track.utid
      where key='debug.${PIPELINE_ID}' and stringValue='${pipelineId}'`,
    );
    const it = queryRes.iter({
      argSetId: NUM,
      ts: NUM,
      id: NUM,
      name: STR,
      key: STR,
      threadName: STR,
      stringValue: STR_NULL,
    });
    let vitalTimestamps: PipelineTimeStamp[] = [];
    for (; it.valid(); it.next()) {
      vitalTimestamps.push({
        id: it.id,
        name: it.name,
        threadName: it.threadName,
        ts: it.ts,
      });
    }
    vitalTimestamps = vitalTimestamps
      .filter(
        (item) =>
          item.name.startsWith(TIMING_MARK_PREFIX) ||
          item.name.startsWith(TIMING_MARK_FRAMEWORK_PREFIX) ||
          item.name.endsWith(CRUCIAL_TIMING_KEYS[1]), // FIXME: Timing::OnPipelineStart should change to Timing::Mark::OnPipelineStart
      )
      .map((stage) => {
        let newName = stage.name;
        if (newName.startsWith(TIMING_MARK_FRAMEWORK_PREFIX)) {
          newName = newName.replace(TIMING_MARK_FRAMEWORK_PREFIX, '');
        } else if (newName.startsWith(TIMING_MARK_PREFIX)) {
          newName = newName.replace(TIMING_MARK_PREFIX, '');
        } else if (newName.endsWith(CRUCIAL_TIMING_KEYS[1])) {
          newName = newName.replace('Timing::', '');
        }
        return {...stage, name: newName};
      });

    // the pipeline may contains same name stages 'Timing::MarkFrameWorkTiming.dataProcessorEnd', filter it.
    const seenNames = new Set();
    vitalTimestamps = vitalTimestamps.filter((timestamp) => {
      if (seenNames.has(timestamp.name)) {
        return false;
      } else {
        seenNames.add(timestamp.name);
        return true;
      }
    });

    // each xxStart must have corrsponding xxEnd
    vitalTimestamps.filter((timestamp) => {
      if (this.validPipelineBeginStage(timestamp.name)) {
        const endStage = this.matchedEndStage(vitalTimestamps, timestamp);
        let name = timestamp.name;
        if (
          name.endsWith(FRAMEWORK_TIMING_START) &&
          !CRUCIAL_TIMING_KEYS.includes(name)
        ) {
          name = name.substring(0, name.length - FRAMEWORK_TIMING_START.length);
        } else if (
          name.endsWith(TIMING_START) &&
          !CRUCIAL_TIMING_KEYS.includes(name)
        ) {
          name = name.substring(0, name.length - TIMING_START.length);
        }

        if (endStage != undefined) {
          if (endStage.ts == timestamp.ts) {
            this.addChartEvent(timestamp.threadName, name, timestamp.ts, 'R');
          } else {
            this.addChartEvent(timestamp.threadName, name, timestamp.ts, 'B');
            this.addChartEvent(timestamp.threadName, name, endStage.ts, 'E');
          }
          this.pipelineStages.push({
            ...timestamp,
            ts: this.formatTsToMillsSeconds(timestamp.ts),
            dur: this.formatTsToMillsSeconds(endStage.ts - timestamp.ts),
            name,
            endId: endStage.id,
          });
        }
      }
    });
    this.chartEvents.sort((a, b) => {
      return a.ts - b.ts;
    });
    this.chartEvents.forEach((event) => {
      event.ts = this.formatTsToMillsSeconds(event.ts);
    });

    this.elementTree = await this.queryElementTree(
      Number(this.sliceDetail?.ts),
    );
    this.loading = false;
    eventLoggerState.state.eventLogger.logEvent('lynx_feature_usage', {
      type: 'VitalTimestamp'
    });
  }

  /**
   * Validates if a stage name represents a pipeline beginning
   * @param name - Stage name to check
   * @returns True if name ends with start markers or matches crucial timing keys
   */
  private validPipelineBeginStage(name: string) {
    return (
      name.endsWith(TIMING_START) ||
      name.endsWith(FRAMEWORK_TIMING_START) ||
      CRUCIAL_TIMING_KEYS.includes(name)
    );
  }

  /**
   * Finds matching end stage for a given start stage
   * @param vitalTimestamps - All available timestamps
   * @param timestamp - Start timestamp to match
   * @returns Corresponding end timestamp or undefined if not found
   */
  private matchedEndStage(
    vitalTimestamps: PipelineTimeStamp[],
    timestamp: PipelineTimeStamp
  ): PipelineTimeStamp | undefined {
    if (CRUCIAL_TIMING_KEYS.includes(timestamp.name)) {
      return timestamp;
    }
    if (timestamp.name.endsWith(TIMING_START)) {
      return vitalTimestamps.find(
        (stamp) =>
          stamp.name === timestamp.name.replace(TIMING_START, TIMING_END),
      );
    }
    if (timestamp.name.endsWith(FRAMEWORK_TIMING_START)) {
      return vitalTimestamps.find(
        (stamp) =>
          stamp.name ===
          timestamp.name.replace(FRAMEWORK_TIMING_START, FRAMEWORK_TIMING_END),
      );
    }
    return undefined;
  }

  /**
   * Adds chart event to visualization data
   * @param pid - Process/thread identifier
   * @param name - Event name
   * @param ts - Timestamp in nanoseconds
   * @param ph - Phase type (B=Begin, E=End, R=Complete)
   */
  private addChartEvent(
    pid: string,
    name: string,
    ts: number,
    ph: 'B' | 'E' | 'R'
  ) {
    this.chartEvents.push({
      pid,
      name,
      ts,
      ph,
    });
  }

  /**
   * Converts nanosecond timestamps to milliseconds
   * @param origin - Timestamp in nanoseconds
   * @returns Timestamp in milliseconds with 2 decimal places
   */
  private formatTsToMillsSeconds(origin: number) {
    return Math.round((origin / 1000000) * 100) / 100;
  }

  /**
   * Queries and reconstructs element tree near given timestamp
   * @param timeStamp - Reference timestamp for tree reconstruction
   * @returns Reconstructed element tree or undefined if not found
   */
  private async queryElementTree(timeStamp: number | undefined) {
    if (timeStamp == undefined) {
      return undefined;
    }
    const queryRes = await this.ctx.engine.query(
      `select ts,id,dur,name, arg_set_id as argSetId from slice where slice.name='ConstructElementTree' and ts < ${timeStamp} order by ts desc limit 1`,
    );
    if (queryRes.numRows() <= 0) {
      return undefined;
    }
    const relatedSlice = await findAfterwardIdenticalFlowIdSlice(
      this.ctx.engine,
      queryRes.firstRow({
        id: NUM,
      }).id,
    );
    if (relatedSlice == null || relatedSlice.argSetId == null) {
      return undefined;
    }
    const args = await getArgs(
      this.ctx.engine,
      asArgSetId(relatedSlice.argSetId),
    );
    const contentArg = args.find(
      (item) => item.key === 'debug.content' || item.key === 'args.content',
    );
    const jsonTree = contentArg?.value as string;
    if (!jsonTree) {
      return undefined;
    }
    const rootElementAbbr = JSON.parse(jsonTree);
    return reConstructElementTree(rootElementAbbr, undefined);
  }

  /**
   * Renders the details panel with current analysis results
   * @returns Mithril component with visualization and data tables
   */
  render() {
    if (this.loading) {
      return m('h2', 'Loading');
    }

    return m(VitalTimestampDetailView, {
      pipelineStagesDetail: this.pipelineStages,
      sliceDetail: this.sliceDetail,
      chartEvents: this.chartEvents,
      elementTree: this.elementTree,
    });
  }
}
