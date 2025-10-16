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
import {NativeModuleDetailView} from './native_module_detail_view';
import {
  LYNX_NATIVE_MODULE_ID,
  NATIVEMODULE_CALLBACK,
  NATIVEMODULE_CALLBACK_CONVERT_PARAMS_END,
  NATIVEMODULE_CALLBACK_INVOKE_END,
  NATIVEMODULE_CONVERT_PARAMS_END,
  NATIVEMODULE_INVOKE,
  NATIVEMODULE_PLATFORM_CALLBACK_START,
  NATIVEMODULE_PLATFORM_METHOD_END,
  NATIVEMODULE_THREAD_SWITCH_END,
  NATIVEMODULE_THREAD_SWITCH_START,
  THREAD_UNKNOWN,
} from '../../lynx_perf/constants';
import {NativeModuleSection} from './types';
import {querySliceRelatedFlows} from '../../lynx_perf/flow_utils';
import {isSpecialNativeModule} from './utils';
import {Flow, FlowPoint} from '../../core/flow_types';
import NativeModuleDataManager from './native_module_data_manager';
import {AppImpl} from '../../core/app_impl';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {dictToTreeNodes, Tree} from '../../widgets/tree';
import {DetailsShell} from '../../widgets/details_shell';
import {Button} from '../../widgets/button';
import {Icons} from '../../base/semantic_icons';
import {getSlice, SliceDetails} from '../../components/sql_utils/slice';
import {asSliceSqlId} from '../../components/sql_utils/core_types';
import {hasArgs, renderArguments} from '../../components/details/slice_args';
import {Timestamp} from '../../components/widgets/timestamp';
import {DurationWidget} from '../../components/widgets/duration';
import {lynxPerfGlobals} from '../../lynx_perf/lynx_perf_globals';
import {eventLoggerState} from '../../event_logger';
import {NUM} from '../../trace_processor/query_result';

const STAGE_CONVERT_INPUT_PARAMS = 'Convert Parameters';
const STAGE_DESCRIPTION_CONVERT_INPUT_PARAMS = `Convert the parameters from JavaScript types to platform-specific types. For more details about the parameters, refer to the 'arg0','arg1','arg2'... fields in the Arguments section.`;
const STAGE_PLATFORM_IMPLEMENTATION = 'Perform Platform Implementation';
const STAGE_DESCRIPTION_PLATFORM_IMPLEMENTATION = `Invoke the NativeModule's implementation within the host environment, execute the specific method call, and retrieve the resulting data from the platform layer. The dotted gray area stage involve other threads performing time-consuming operations, such as network requests.`;
const STAGE_CONVERT_OUTPUT_PARAMS = 'Convert Response Data';
const STAGE_DESCRIPTION_CONVERT_OUTPUT_PARAMS =
  'Convert the data obtained from the platform layer in the previous stage into JavaScript-compatible types.';
const STAGE_INVOKE_CALLBACK = 'Invoke Callback';
const STAGE_DESCRIPTION_INVOKE_CALLBACK =
  'Execute the callback methods that the frontend has registered with the NativeModule.';
const STAGE_FINISH_PLATFORM_IMPLEMENTATION = 'Finish Platform Implementation';
const STAGE_DESCRIPTION_FINISH_PLATFORM_IMPLEMENTATION =
  'Completes cleanup and notifies external registers that the NativeModule execution is finished.';
const STAGE_THREAD_SWITCHING = 'Awaiting Callback Task Execution';

/**
 * Native Module Details Panel
 * 
 * Displays detailed performance analysis of NativeModule execution,
 * breaking down the workflow into distinct stages with timing information.
 */
export class NativeModuleDetailsPanel implements TrackEventDetailsPanel {
  private loading: boolean;
  private ctx: Trace;
  private nativeModuleSections: NativeModuleSection[] | undefined;
  private sliceDetail: SliceDetails | undefined;

  constructor(ctx: Trace) {
    this.ctx = ctx;
    this.loading = false;
  }

  async load({eventId}: TrackEventSelection) {
    this.loading = true;
    this.sliceDetail = await getSlice(this.ctx.engine, asSliceSqlId(eventId));
    const selectionDetail = await this.ctx.tracks
      .getTrack(LYNX_NATIVE_MODULE_ID)
      ?.track.getSelectionDetails?.(eventId);
    if (this.sliceDetail && selectionDetail) {
      this.sliceDetail.name = lynxPerfGlobals.state.traceIdToJSBName.get(eventId) || '';
      this.sliceDetail.dur = selectionDetail.dur ?? BigInt(0);
    }

    const flows = await querySliceRelatedFlows(this.ctx.engine, eventId);
    this.nativeModuleSections = await this.assembleSections(flows);
    NativeModuleDataManager.setNativeModuleSections(
      eventId,
      this.nativeModuleSections,
    );
    this.loading = false;
    if (this.sliceDetail) {
      eventLoggerState.state.eventLogger.logEvent('lynx_feature_usage', {
        type: 'NativeModule'
      });
    }
  }

  /**
   * Organizes flow events into logical execution stages
   * @param flows - Array of flow events
   * @returns Array of categorized execution stages
   */
  private async assembleSections(flows: Flow[]) {
    if (lynxPerfGlobals.state.nonTimingNativeModuleTraces) {
      return await this.assembleOptimizedSections(flows);  
    }
    const beginTs = Number(flows[0].begin.sliceStartTs);
    const sections: NativeModuleSection[] = [];
    const inputParamsEnd = this.findSectionTs(
      NATIVEMODULE_CONVERT_PARAMS_END,
      flows,
    );
    const platformImplEnd = this.findSectionTs(
      NATIVEMODULE_THREAD_SWITCH_START,
      flows,
    );
    const outputParamsEnd = this.findSectionTs(
      NATIVEMODULE_CALLBACK_CONVERT_PARAMS_END,
      flows,
    );
    const callbackInvokeEnd = this.findSectionTs(
      NATIVEMODULE_CALLBACK_INVOKE_END,
      flows,
    );
    const platformMethodEnd = this.findSectionTs(
      NATIVEMODULE_PLATFORM_METHOD_END,
      flows,
    );
    if (isSpecialNativeModule(flows)) {
      const invokecCallbackEnd = this.findSectionTs(
        NATIVEMODULE_CALLBACK_INVOKE_END,
        flows,
      );
      if (
        inputParamsEnd != undefined &&
        platformImplEnd != undefined &&
        outputParamsEnd != undefined &&
        invokecCallbackEnd != undefined && 
        platformMethodEnd != undefined
      ) {
        sections.push({
          beginTs: beginTs,
          endTs: Number(inputParamsEnd.sliceEndTs),
          name: STAGE_CONVERT_INPUT_PARAMS,
          description: STAGE_DESCRIPTION_CONVERT_INPUT_PARAMS,
          thread: inputParamsEnd.threadName,
        });
        sections.push({
          beginTs: Number(inputParamsEnd.sliceEndTs),
          endTs: Number(platformImplEnd.sliceEndTs),
          name: STAGE_PLATFORM_IMPLEMENTATION,
          description: STAGE_DESCRIPTION_PLATFORM_IMPLEMENTATION,
          thread: platformImplEnd.threadName,
        });
        sections.push({
          beginTs: Number(platformImplEnd.sliceEndTs),
          endTs: Number(outputParamsEnd.sliceEndTs),
          name: STAGE_CONVERT_OUTPUT_PARAMS,
          description: STAGE_DESCRIPTION_CONVERT_OUTPUT_PARAMS,
          thread: outputParamsEnd.threadName,
        });
        sections.push({
          beginTs: Number(outputParamsEnd.sliceEndTs),
          endTs: Number(invokecCallbackEnd.sliceEndTs),
          name: STAGE_INVOKE_CALLBACK,
          description: STAGE_DESCRIPTION_INVOKE_CALLBACK,
          thread: invokecCallbackEnd.threadName,
        });
        sections.push({
          beginTs: Number(invokecCallbackEnd.sliceEndTs),
          endTs: Number(platformMethodEnd.sliceEndTs),
          name: STAGE_FINISH_PLATFORM_IMPLEMENTATION,
          description: STAGE_DESCRIPTION_FINISH_PLATFORM_IMPLEMENTATION,
          thread: invokecCallbackEnd.threadName,
        });
      }
    } else {
      const threadSwitchingEnd = this.findSectionTs(
        NATIVEMODULE_THREAD_SWITCH_END,
        flows,
      );
      if (
        inputParamsEnd != undefined &&
        platformImplEnd != undefined &&
        outputParamsEnd != undefined &&
        threadSwitchingEnd != undefined &&
        platformMethodEnd != undefined &&
        callbackInvokeEnd != undefined
      ) {
        sections.push({
          beginTs: beginTs,
          endTs: Number(inputParamsEnd.sliceEndTs),
          name: STAGE_CONVERT_INPUT_PARAMS,
          description: STAGE_DESCRIPTION_CONVERT_INPUT_PARAMS,
          thread: inputParamsEnd.threadName,
        });
        sections.push({
          beginTs: Number(inputParamsEnd.sliceEndTs),
          endTs: Number(platformImplEnd.sliceEndTs),
          name: STAGE_PLATFORM_IMPLEMENTATION,
          description: STAGE_DESCRIPTION_PLATFORM_IMPLEMENTATION,
          thread: this.getRunningThreadInfo(
            inputParamsEnd,
            platformMethodEnd,
            platformImplEnd,
          ),
        });
        sections.push({
          beginTs: Number(platformImplEnd.sliceEndTs),
          endTs: Number(threadSwitchingEnd.sliceEndTs),
          name: STAGE_THREAD_SWITCHING,
          description: `Waiting for callback tasks to be scheduled for execution on the '${threadSwitchingEnd.threadName}' thread.`,
          thread: THREAD_UNKNOWN,
        });
        sections.push({
          beginTs: Number(threadSwitchingEnd.sliceEndTs),
          endTs: Number(outputParamsEnd.sliceEndTs),
          name: STAGE_CONVERT_OUTPUT_PARAMS,
          description: STAGE_DESCRIPTION_CONVERT_OUTPUT_PARAMS,
          thread: outputParamsEnd.threadName,
        });
        sections.push({
          beginTs: Number(outputParamsEnd.sliceEndTs),
          endTs: Number(callbackInvokeEnd.sliceEndTs),
          name: STAGE_INVOKE_CALLBACK,
          description: STAGE_DESCRIPTION_INVOKE_CALLBACK,
          thread: outputParamsEnd.threadName,
        });
      }
    }
    return sections;
  }

  private async assembleOptimizedSections(flows: Flow[]) {
    const sections: NativeModuleSection[] = [];
    const nativeModuleInvoke = this.findSectionTs(
      NATIVEMODULE_INVOKE,
      flows,
    );
    const platformCallbackStart = this.findSectionTs(
      NATIVEMODULE_PLATFORM_CALLBACK_START,
      flows,
    );
    const nativeModuleCallback = this.findSectionTs(
      NATIVEMODULE_CALLBACK,
      flows,
    );
    if (nativeModuleInvoke === undefined || platformCallbackStart === undefined || nativeModuleCallback === undefined) {
      return sections;
    }
    const jsValueToPubValue = await this.getDescendantsWithSpecificName("JSValueToPubValue", nativeModuleInvoke.sliceId);
    const pubValueToJSValue = await this.getDescendantsWithSpecificName("PubValueToJSValue", nativeModuleCallback.sliceId);
    let callPlatformImplementation = await this.getDescendantsWithSpecificName("CallPlatformImplementation", nativeModuleInvoke.sliceId);
    if (!callPlatformImplementation) {
      // 'Network::SendNetworkRequest' intercepts the standard platform implementation flow; it is handled specially here.
      callPlatformImplementation = await this.getDescendantsWithSpecificName("Network::SendNetworkRequest", nativeModuleInvoke.sliceId);
    }
    if (jsValueToPubValue === undefined || pubValueToJSValue === undefined || callPlatformImplementation === undefined) {
      return sections;
    }
     if (isSpecialNativeModule(flows)) {
      sections.push({
        beginTs: Number(nativeModuleInvoke.sliceStartTs),
        endTs: jsValueToPubValue.ts + jsValueToPubValue.dur,
        name: STAGE_CONVERT_INPUT_PARAMS,
        description: STAGE_DESCRIPTION_CONVERT_INPUT_PARAMS,
        thread: nativeModuleInvoke.threadName,
      });
      sections.push({
        beginTs: jsValueToPubValue.ts + jsValueToPubValue.dur,
        endTs: Number(nativeModuleCallback.sliceStartTs),
        name: STAGE_PLATFORM_IMPLEMENTATION,
        description: STAGE_DESCRIPTION_PLATFORM_IMPLEMENTATION,
        thread: nativeModuleCallback.threadName,
      });
      sections.push({
        beginTs: Number(nativeModuleCallback.sliceStartTs),
        endTs: pubValueToJSValue.ts + pubValueToJSValue.dur,
        name: STAGE_CONVERT_OUTPUT_PARAMS,
        description: STAGE_DESCRIPTION_CONVERT_OUTPUT_PARAMS,
        thread: nativeModuleCallback.threadName,
      });
      sections.push({
        beginTs: pubValueToJSValue.ts + pubValueToJSValue.dur,
        endTs: Number(nativeModuleCallback.sliceEndTs),
        name: STAGE_INVOKE_CALLBACK,
        description: STAGE_DESCRIPTION_INVOKE_CALLBACK,
        thread: nativeModuleInvoke.threadName,
      });
      sections.push({
        beginTs: Number(nativeModuleCallback.sliceEndTs),
        endTs: Number(nativeModuleInvoke.sliceEndTs),
        name: STAGE_FINISH_PLATFORM_IMPLEMENTATION,
        description: STAGE_DESCRIPTION_FINISH_PLATFORM_IMPLEMENTATION,
        thread: nativeModuleInvoke.threadName,
      });

    } else {
      sections.push({
        beginTs: Number(nativeModuleInvoke.sliceStartTs),
        endTs: jsValueToPubValue.ts + jsValueToPubValue.dur,
        name: STAGE_CONVERT_INPUT_PARAMS,
        description: STAGE_DESCRIPTION_CONVERT_INPUT_PARAMS,
        thread: nativeModuleInvoke.threadName,
      });
      const threadInfo: Record<string, number> = {};
      threadInfo[nativeModuleInvoke.threadName] = callPlatformImplementation.dur;
      threadInfo['other'] = Number(platformCallbackStart.sliceStartTs) - callPlatformImplementation.dur - callPlatformImplementation.ts;
      sections.push({
        beginTs: jsValueToPubValue.ts + jsValueToPubValue.dur,
        endTs: Number(platformCallbackStart.sliceStartTs),
        name: STAGE_PLATFORM_IMPLEMENTATION,
        description: STAGE_DESCRIPTION_PLATFORM_IMPLEMENTATION,
        thread:threadInfo
      });
      sections.push({
        beginTs: Number(platformCallbackStart.sliceEndTs),
        endTs: Number(nativeModuleCallback.sliceStartTs),
        name: STAGE_THREAD_SWITCHING,
        description: `Waiting for callback tasks to be scheduled for execution on the '${nativeModuleCallback.threadName}' thread.`,
        thread: '/',
      });
      sections.push({
        beginTs: Number(nativeModuleCallback.sliceStartTs),
        endTs: pubValueToJSValue.ts + pubValueToJSValue.dur,
        name: STAGE_CONVERT_OUTPUT_PARAMS,
        description: STAGE_DESCRIPTION_CONVERT_OUTPUT_PARAMS,
        thread: nativeModuleCallback.threadName,
      });
      sections.push({
        beginTs: pubValueToJSValue.ts + pubValueToJSValue.dur,
        endTs: Number(nativeModuleCallback.sliceEndTs),
        name: STAGE_INVOKE_CALLBACK,
        description: STAGE_DESCRIPTION_INVOKE_CALLBACK,
        thread: nativeModuleCallback.threadName,
      });
    }
    return sections;
  }

  private async getDescendantsWithSpecificName(traceName: string, traceId: number) {
    const query = `
      select 
      t.ts,
      t.dur 
      from descendant_slice(${traceId}) t
      where t.name = '${traceName}' limit 1`;
      const result = await this.ctx.engine.query(query);
      if (result.numRows() > 0) {
        return result.firstRow({
          ts: NUM,
          dur: NUM,
        });
      }
      return undefined;
  }

  /**
   * Calculates thread execution time distribution
   * @param start - Flow start point
   * @param mid - Intermediate flow point
   * @param end - Flow end point
   * @returns Object mapping thread names to execution durations
   */
  private getRunningThreadInfo(
    start: FlowPoint,
    mid: FlowPoint,
    end: FlowPoint,
  ) {
    const threadInfo: Record<string, number> = {};
    threadInfo[start.threadName] = Number(mid.sliceEndTs - start.sliceStartTs);
    threadInfo['other'] = Number(end.sliceStartTs - mid.sliceEndTs);
    return threadInfo;
  }

  /**
   * Finds flow point by trace name
   * @param traceName - Name of trace to find
   * @param flows - Array of flow events
   * @returns Matching flow point or undefined
   */
  private findSectionTs(traceName: string, flows: Flow[]) {
    for (const flow of flows) {
      if (flow.begin.sliceName === traceName) {
        return flow.begin;
      } else if (flow.end.sliceName === traceName) {
        return flow.end;
      }
    }
    return undefined;
  }

  render() {
    if (this.loading) {
      return m('h2', 'Loading');
    }
    const hasSections = this.nativeModuleSections && this.nativeModuleSections.length > 0;
    if (AppImpl.instance.trace && this.sliceDetail) {
      return m(
        DetailsShell,
        {
          title: 'NativeModule',
          description: this.sliceDetail.name,
          buttons: m(Button, {
            compact: true,
            label: 'Original Slice',
            rightIcon: Icons.SortedAsc,
            onclick: (_e) => {
              AppImpl.instance.trace?.selection.selectSqlEvent(
                'slice',
                this.sliceDetail?.id ?? 0,
                {
                  switchToCurrentSelectionTab: false,
                  scrollToSelection: true,
                },
              );
            },
          }),
        },
        hasSections && m(
          GridLayout,
          m(
            'div.dynamic-grid-layout',
            this.renderRhs(AppImpl.instance.trace, this.sliceDetail),
            m(NativeModuleDetailView, {
              sectionDetail: this.nativeModuleSections,
              sliceDetail: this.sliceDetail,
            }),
          ),
        ),
        !hasSections && m(
          GridLayout,
          this.renderRhs(AppImpl.instance.trace, this.sliceDetail),
        ),
      );
    } else {
      return null;
    }
  }

  /**
   * Renders the right-hand side panel with arguments and details
   * @param trace - Current trace context
   * @param slice - Slice details to display
   * @returns Mithril virtual DOM elements
   */
  private renderRhs(trace: Trace, slice: SliceDetails): m.Children {
    const args =
      hasArgs(slice.args) &&
      m(
        Section,
        {title: 'Arguments'},
        m(Tree, renderArguments(trace, slice.args)),
      );
    const details =
      this.sliceDetail &&
      dictToTreeNodes({
        'Name': this.sliceDetail.name,
        'Category':
          !this.sliceDetail.category || this.sliceDetail.category === '[NULL]'
            ? 'N/A'
            : slice.category,
        'Start time': m(Timestamp, {ts: this.sliceDetail.ts}),
        'Duration': m(DurationWidget, {dur: this.sliceDetail.dur}),
      });
    const detailSection =
      AppImpl.instance.trace &&
      this.sliceDetail &&
      m(Section, {title: 'Details'}, m(Tree, details));
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    if (args) {
      if (this.nativeModuleSections && this.nativeModuleSections.length > 0) {
        return m(GridLayoutColumn, detailSection, args);
      } else {
        return m(GridLayout, detailSection, args);
      }
    } else {
      return undefined;
    }
  }
}
