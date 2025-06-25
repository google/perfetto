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
import {LynxPerfTrack} from './tracks';
import {LYNX_ISSUES_PLUGIN_ID} from '../../lynx_perf/constants';
import {AppImpl} from '../../core/app_impl';
import {TrackNode} from '../../public/workspace';
import {isLynxBackgroundScriptThreadGroup} from '../../lynx_perf/track_utils';
import {lynxPerfGlobals} from '../../lynx_perf/lynx_perf_globals';
import {ThreadSortOrder} from '../lynx.ThreadGroups';

const TRACK_TITLE = 'Performance Issues';
/**
 * Lynx Performance Issues Tracking Plugin
 * Manages visualization and tracking of performance issues detected in Lynx traces.
 * Registers a dedicated track for displaying performance metrics and issues.
 */
export default class LynxPerf implements PerfettoPlugin {
  static readonly id = LYNX_ISSUES_PLUGIN_ID;
  /**
   * This hook is called as the trace is loading. At this point the trace is
   * loaded into trace processor and it's ready to process queries. This hook
   * should be used for adding tracks and commands that depend on the trace.
   *
   * It should not be used for finding tracks from other plugins as there is no
   * guarantee those tracks will have been added yet.
   */
  async onTraceLoad(ctx: Trace): Promise<void> {
    lynxPerfGlobals.resetIssueStatus();
    // Register main performance track
    ctx.tracks.registerTrack({
      uri: LYNX_ISSUES_PLUGIN_ID,
      track: new LynxPerfTrack(ctx, LYNX_ISSUES_PLUGIN_ID),
      title: TRACK_TITLE,
    });

    /**
     * Command for updating performance issue status dynamically
     * Scans workspace for Lynx background threads and attaches issue track
     * when performance issues are detected.
     */
    ctx.commands.registerCommand({
      id: 'lynx.PerformanceIssues#update',
      name: 'Update the performance issues status',
      callback: () => {
        const workspace = AppImpl.instance.trace?.workspace;
        if (workspace && workspace.children.length > 0) {
          for (const item of workspace.children) {
            // Skip if issues track already exists
            if (item.children?.some((child) => child.uri === LYNX_ISSUES_PLUGIN_ID)) {
              break;
            }

            // Attach to Lynx background threads when issues exist
            if (isLynxBackgroundScriptThreadGroup(item) &&
                lynxPerfGlobals.state.issues.length > 0) {
              const track = new TrackNode({
                title: `${TRACK_TITLE} (count: ${lynxPerfGlobals.state.issues.length})`,
                uri: LYNX_ISSUES_PLUGIN_ID,
                sortOrder: ThreadSortOrder.PERFORMANCE_ISSUES,
              });
              item.addChildFirst(track);
            }
          }
        }
      },
    });
  }
}
