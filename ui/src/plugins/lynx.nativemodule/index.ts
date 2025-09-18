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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {LynxNativeModuleTrack} from './tracks';
import {TrackNode} from '../../public/workspace';
import {AppImpl} from '../../core/app_impl';
import {
  LYNX_NATIVE_MODULE_ID,
  NATIVEMODULE_CALL,
  DEPRECATED_NATIVEMODULE_CALL,
  NATIVEMODULE_EVENTS_WITH_FLOW_ID_LIST,
} from '../../lynx_perf/constants';
import {
  getBackgroundScriptThreadTrackNode,
  isLynxBackgroundScriptThreadGroup,
} from '../../lynx_perf/track_utils';
import {NUM} from '../../trace_processor/query_result';
import {getArgs} from '../../components/sql_utils/args';
import {asArgSetId} from '../../components/sql_utils/core_types';
import {lynxPerfGlobals} from '../../lynx_perf/lynx_perf_globals';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import LynxThreadGroupPlugin from '../lynx.ThreadGroups';

/**
 * Native Module Plugin
 *
 * Tracks and visualizes NativeModule (JS Bridge) calls in Lynx applications,
 * providing insights into cross-platform communication performance.
 */
export default class LynxNativeModule implements PerfettoPlugin {
  static readonly id = LYNX_NATIVE_MODULE_ID;
  static readonly dependencies = [
    LynxThreadGroupPlugin,
    TraceProcessorTrackPlugin,
  ];

  /**
   * This hook is called as the trace is loading. At this point the trace is
   * loaded into trace processor and it's ready to process queries. This hook
   * should be used for adding tracks and commands that depend on the trace.
   *
   * It should not be used for finding tracks from other plugins as there is no
   * guarantee those tracks will have been added yet.
   */
  async onTraceLoad(ctx: Trace): Promise<void> {
    // Register main visualization track
    ctx.tracks.registerTrack({
      uri: LYNX_NATIVE_MODULE_ID,
      track: new LynxNativeModuleTrack(ctx, LYNX_NATIVE_MODULE_ID),
      title: 'NativeModule',
    });

    // Only show track if valid NativeModule calls exist
    const showTrack = await this.containValidNativeModule(ctx);
    if (!showTrack) return;

    this.tryAddNativeModuleTrack();
  }

  /**
   * Attempts to add NativeModule track to workspace hierarchy
   * @remarks Positions track after JavaScript thread track when found
   */
  private tryAddNativeModuleTrack() {
    const track = new TrackNode({
      title: 'NativeModule',
      uri: LYNX_NATIVE_MODULE_ID,
      sortOrder: 20,
    });
    const workspace = AppImpl.instance.trace?.workspace;
    if (workspace && workspace?.children.length > 0) {
      for (let i = 0; i < workspace.children.length; i++) {
        const item: TrackNode = workspace.children[i];
        if (isLynxBackgroundScriptThreadGroup(item)) {
          const jsThreadTrackNode = getBackgroundScriptThreadTrackNode(item);
          if (jsThreadTrackNode) {
            // this.addNativeModuleTrack = true;
            item.addChildAfter(track, jsThreadTrackNode);
            break;
          }
        }
      }
    }
  }

  /**
   * Checks if trace contains valid NativeModule calls
   * @param ctx - Trace context
   * @returns True if valid bridge calls with flow IDs are found
   */
  private async containValidNativeModule(ctx: Trace) {
    // Check if slice table contains all three specific NativeModule trace events
    const nativeModuleEventsWithFlowIdStr =
      NATIVEMODULE_EVENTS_WITH_FLOW_ID_LIST.map((item) => `'${item}'`).join(
        ',',
      );
    const checkEventsQuery = await ctx.engine.query(
      `select count(distinct slice.name) as count from slice where slice.name in (${nativeModuleEventsWithFlowIdStr})`,
    );
    const IMPORTANT_EVENT_COUNT = 3;
    const checkIt = checkEventsQuery.iter({count: NUM});
    if (checkIt.valid() && checkIt.count === IMPORTANT_EVENT_COUNT) {
      lynxPerfGlobals.setNonTimingNativeModuleTraces(true);
      return true;
    }

    const queryRes = await ctx.engine.query(
      `select arg_set_id as argSetId from slice where slice.name='${NATIVEMODULE_CALL}' or slice.name='${DEPRECATED_NATIVEMODULE_CALL}'`,
    );
    const it = queryRes.iter({
      argSetId: NUM,
    });
    for (; it.valid(); it.next()) {
      const args = await getArgs(ctx.engine, asArgSetId(it.argSetId));
      const containsBridge = args.find(
        (item) => item.key === 'debug.module_name' && item.value === 'bridge',
      );
      const containsCall = args.find(
        (item) => item.key === 'debug.method_name' && item.value === 'call',
      );
      const containsFlowId = args.find((item) => item.key === 'debug.flowId');
      if (containsBridge && containsCall && containsFlowId) {
        return true;
      }
    }
    return false;
  }
}
