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
import {NUM, STR} from '../../trace_processor/query_result';
import {
  COMMAND_FOCUS_LYNX_VIEW,
  COMMAND_QUERY_LYNX_VIEW,
  LYNX_LOAD_BUNDLE,
  LYNX_NATIVE_MODULE_ID,
  NATIVEMODULE_INVOKE_LIST,
  NO_INSTANCE_ID,
  PARAMETER_FOCUS_LYNX_VIEWS,
} from '../../lynx_perf/constants';
import {lynxPerfGlobals} from '../../lynx_perf/lynx_perf_globals';
import {LynxViewInstance} from '../../lynx_perf/types';
import {getUrlParameter} from '../../lynx_perf/url_utils';
import {AppImpl} from '../../core/app_impl';
import {TrackNode, Workspace} from '../../public/workspace';
import {
  getBackgroundScriptThreadTrackNode,
  getMainScriptThreadTrackNode,
  isLynxBackgroundScriptThreadGroup,
} from '../../lynx_perf/track_utils';
import LynxThreadGroupPlugin from '../lynx.ThreadGroups';
import FrameJankPlugin from '../lynx.frameJank';
import LynxNativeModule from '../lynx.nativemodule';
import {eventLoggerState} from '../../event_logger';

export default class FocusMode implements PerfettoPlugin {
  static readonly id = 'lynx.FocusMode';
  // we depend on FrameJankPlugin to init 'lynxPerfGlobals.state.trackUriToThreadMap'
  // we depend on LynxNativeModule to wait it add to the track.
  static readonly dependencies = [
    LynxThreadGroupPlugin,
    LynxNativeModule,
    FrameJankPlugin,
  ];

  async onTraceLoad(trace: Trace): Promise<void> {
    await this.queryLoadBundle(trace);

    trace.commands.registerCommand({
      id: COMMAND_FOCUS_LYNX_VIEW,
      name: 'Focus LynxView',
      callback: async () => {
        if (!lynxPerfGlobals.state.showRightSidebar) {
          lynxPerfGlobals.toggleRightSidebar();
        }
      },
    });

    trace.commands.registerCommand({
      id: COMMAND_QUERY_LYNX_VIEW,
      name: 'Query LynxView(s)',
      callback: async () => {
        await this.filterSpecificInstanceId(trace);
      },
    });
    await this.initTraceStatus(trace);
  }

  private async initTraceStatus(trace: Trace) {
    const focusLynxViews = getUrlParameter(PARAMETER_FOCUS_LYNX_VIEWS);
    if (focusLynxViews) {
      const instanceIds = focusLynxViews.split(',');
      const instances: LynxViewInstance[] = [];
      instanceIds.forEach((instanceId) => {
        instances.push({
          instanceId,
          url: '',
        });
      });
      lynxPerfGlobals.updateSelectedLynxViewInstances(instances);
      if (
        instances.some((instance) => instance.instanceId === NO_INSTANCE_ID)
      ) {
        lynxPerfGlobals.setHighlightNoInstanceIdTrace(true);
      } else {
        lynxPerfGlobals.setHighlightNoInstanceIdTrace(false);
      }
    }

    await this.filterSpecificInstanceId(trace);
  }

  private async queryLoadBundle(trace: Trace) {
    // TODO: remove this after we have a better way to get the instance_id and url
    const lynxBundleNameQuery = LYNX_LOAD_BUNDLE.map(
      (name) => `'${name}'`,
    ).join(',');
    const queryLoadBundle = `
      select
        args.key as key,
        args.display_value as value
      FROM slice
      JOIN args ON slice.arg_set_id=args.arg_set_id
      WHERE slice.name in (${lynxBundleNameQuery})
      ORDER BY slice.ts`;
    const res = await trace.engine.query(queryLoadBundle);
    const it = res.iter({
      key: STR,
      value: STR,
    });
    const instances: LynxViewInstance[] = [];
    let url = '';
    let instanceId = '';
    for (; it.valid(); it.next()) {
      if (it.key === 'debug.url' || it.key === 'args.url') {
        url = it.value;
      } else if (
        it.key === 'debug.instance_id' ||
        it.key === 'args.instance_id'
      ) {
        instanceId = it.value;
      }
      if (url && instanceId) {
        instances.push({
          url,
          instanceId,
        });
        url = '';
        instanceId = '';
      }
    }
    // some instance_id may have already execute the LoadLynxBundle
    const instanceIdWithUrlQuery = instances
      .map((instance) => `'${instance.instanceId}'`)
      .join(',');

    const queryInstanceIdWithoutLoadBundle = `
      select distinct slice.instance_id as instanceId
       from instance_id_slice as slice
       where slice.instance_id not in (${instanceIdWithUrlQuery})
    `;
    const instanceIdWithLoadBundle = await trace.engine.query(
      queryInstanceIdWithoutLoadBundle,
    );
    const withoutLoadBundleIt = instanceIdWithLoadBundle.iter({
      instanceId: STR,
    });
    for (; withoutLoadBundleIt.valid(); withoutLoadBundleIt.next()) {
      instances.push({
        url: '',
        instanceId: withoutLoadBundleIt.instanceId,
      });
    }
    instances.sort((a, b) => {
      // empty url will be put at the bottom
      if (!a.url && !b.url) {
        return a.instanceId.localeCompare(b.instanceId);
      } else if (!a.url) {
        return 1;
      } else if (!b.url) {
        return -1;
      } else {
        if (a.url === b.url) {
          return a.instanceId.localeCompare(b.instanceId);
        } else {
          return a.url.localeCompare(b.url);
        }
      }
    });
    instances.unshift({
      url: '',
      instanceId: NO_INSTANCE_ID,
    });

    lynxPerfGlobals.updateLynxViewInstances(instances);

    lynxPerfGlobals.updateSelectedLynxViewInstances(instances);
    lynxPerfGlobals.setHighlightNoInstanceIdTrace(true);
  }

  private async getTrackIdToCountMap(
    trace: Trace,
  ): Promise<Map<number, number>> {
    const trackIdCountQuery = `
      select
        count(*) as count,
        track_id as trackId
      from slice
      group by track_id
    `;
    const trackIdCountResult = await trace.engine.query(trackIdCountQuery);
    const trackIdCountIt = trackIdCountResult.iter({
      count: NUM,
      trackId: NUM,
    });
    const trackIdToCountMap = new Map<number, number>();
    for (; trackIdCountIt.valid(); trackIdCountIt.next()) {
      trackIdToCountMap.set(trackIdCountIt.trackId, trackIdCountIt.count);
    }
    return trackIdToCountMap;
  }

  private async getFilteredTraceSet(
    trace: Trace,
    instanceIdArray: string,
    otherSlicesQuery: string,
    trackIdToCountMap: Map<number, number>,
  ): Promise<Set<number>> {
    const querySpecicInstanceId = `
      SELECT 
        s.id AS id,
        s.track_id AS trackId
      FROM slice s
      LEFT JOIN instance_id_slice i ON s.id = i.slice_id
      WHERE i.instance_id NOT IN (${instanceIdArray}) ${otherSlicesQuery}
    `;
    const filteredTraceSet: Set<number> = new Set();
    const res = await trace.engine.query(querySpecicInstanceId);
    const it = res.iter({
      id: NUM,
      trackId: NUM,
    });
    for (; it.valid(); it.next()) {
      filteredTraceSet.add(it.id);
      if (trackIdToCountMap.has(it.trackId)) {
        trackIdToCountMap.set(
          it.trackId,
          trackIdToCountMap.get(it.trackId)! - 1,
        );
      }
    }
    return filteredTraceSet;
  }

  private async reorderWorkspaceTracks(
    workspace: Workspace,
    filteredTraceSet: Set<number>,
    trackIdToCountMap: Map<number, number>,
    trace: Trace,
  ) {
    for (let i = 0; i < workspace.children.length; i++) {
      const item: TrackNode = workspace.children[i];
      if (!isLynxBackgroundScriptThreadGroup(item)) {
        continue;
      }
      let firstTrackNode = getMainScriptThreadTrackNode(item);
      if (!firstTrackNode) {
        break;
      }
      for (let j = 0; j < item.children.length - 1; j++) {
        const trackNode = item.children[j];
        // NativeModule track special handling
        if (trackNode.uri === LYNX_NATIVE_MODULE_ID) {
          const queryRes = await trace.engine.query(
            `select 
                slice.id as id
              from slice 
              where slice.name in (${NATIVEMODULE_INVOKE_LIST.join(',')})`,
          );
          const it = queryRes.iter({
            id: NUM,
          });
          let exists = false;
          for (; it.valid(); it.next()) {
            if (!filteredTraceSet.has(it.id)) {
              exists = true;
              break;
            }
          }
          const backgroundThreadNode = getBackgroundScriptThreadTrackNode(item);
          if (exists && backgroundThreadNode) {
            item.addChildAfter(trackNode, backgroundThreadNode);
            firstTrackNode = trackNode;
            continue;
          }
        }

        if (trackNode.children.length <= 0) {
          continue;
        }
        const trackChildNode = trackNode.children[0];
        const trackItem = lynxPerfGlobals.state.trackUriToThreadMap.get(
          trackChildNode.uri ?? '',
        );
        if (
          trackItem &&
          trackIdToCountMap.has(trackItem.trackId) &&
          trackIdToCountMap.get(trackItem.trackId)! > 0
        ) {
          item.addChildAfter(trackNode, firstTrackNode);
          firstTrackNode = trackNode;
        }
      }
      break;
    }
  }

  private async filterSpecificInstanceId(trace: Trace) {
    // Step 1: Get trackId to count map
    const trackIdToCountMap = await this.getTrackIdToCountMap(trace);
    // Step 2: Prepare instanceId array and query condition
    const instanceIdArray = lynxPerfGlobals.state.selectedLynxviewInstances
      .map((item) => `'${item.instanceId}'`)
      .join(',');
    const otherSlicesQuery = lynxPerfGlobals.state.highlightNoInstanceIdTrace
      ? ' AND i.slice_id IS NOT NULL'
      : 'OR i.slice_id IS NULL';
    // Step 3: Get filtered trace set and update global state
    const filteredTraceSet = await this.getFilteredTraceSet(
      trace,
      instanceIdArray,
      otherSlicesQuery,
      trackIdToCountMap,
    );
    if (
      lynxPerfGlobals.state.selectedLynxviewInstances.length <
      lynxPerfGlobals.state.lynxviewInstances.length
    ) {
      eventLoggerState.state.eventLogger.logEvent('lynx_feature_usage', {
        type: 'FocusLynxView',
      });
    }
    lynxPerfGlobals.updateFilteredTraceSet(filteredTraceSet);
    // Step 4: Reorder workspace tracks
    const workspace = AppImpl.instance.trace?.workspace;
    if (!workspace || workspace.children.length <= 0) {
      return;
    }
    await this.reorderWorkspaceTracks(
      workspace,
      filteredTraceSet,
      trackIdToCountMap,
      trace,
    );
  }
}
