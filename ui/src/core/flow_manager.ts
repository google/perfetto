// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {Time} from '../base/time';
import {featureFlags} from './feature_flags';
import {FlowDirection, Flow} from './flow_types';
import {asSliceSqlId} from '../trace_processor/sql_utils/core_types';
import {LONG, NUM, STR_NULL} from '../trace_processor/query_result';
import {
  ACTUAL_FRAMES_SLICE_TRACK_KIND,
  THREAD_SLICE_TRACK_KIND,
} from '../public/track_kinds';
import {TrackDescriptor, TrackManager} from '../public/track';
import {AreaSelection, Selection, SelectionManager} from '../public/selection';
import {raf} from './raf_scheduler';
import {Engine} from '../trace_processor/engine';
import {Workspace} from '../public/workspace';

const SHOW_INDIRECT_PRECEDING_FLOWS_FLAG = featureFlags.register({
  id: 'showIndirectPrecedingFlows',
  name: 'Show indirect preceding flows',
  description:
    'Show indirect preceding flows (connected through ancestor ' +
    'slices) when a slice is selected.',
  defaultValue: false,
});

export class FlowManager {
  private _connectedFlows: Flow[] = [];
  private _selectedFlows: Flow[] = [];
  private _curSelection?: Selection;
  private _focusedFlowIdLeft = -1;
  private _focusedFlowIdRight = -1;
  private _visibleCategories = new Map<string, boolean>();
  private _initialized = false;

  constructor(
    private engine: Engine,
    private trackMgr: TrackManager,
    private selectionMgr: SelectionManager,
    private getCurWorkspace: () => Workspace,
  ) {}

  // TODO(primiano): the only reason why this is not done in the constructor is
  // because when loading the UI with no trace, we initialize globals with a
  // FakeTraceImpl with a FakeEngine, which crashes when issuing queries.
  // This can be moved in the ctor once globals go away.
  private initialize() {
    if (this._initialized) return;
    this._initialized = true;
    // Create |CHROME_CUSTOME_SLICE_NAME| helper, which combines slice name
    // and args for some slices (scheduler tasks and mojo messages) for more
    // helpful messages.
    // In the future, it should be replaced with this a more scalable and
    // customisable solution.
    // Note that a function here is significantly faster than a join.
    this.engine.query(`
      SELECT CREATE_FUNCTION(
        'CHROME_CUSTOM_SLICE_NAME(slice_id LONG)',
        'STRING',
        'select case
           when name="Receive mojo message" then
            printf("Receive mojo message (interface=%s, hash=%s)",
              EXTRACT_ARG(arg_set_id,
                          "chrome_mojo_event_info.mojo_interface_tag"),
              EXTRACT_ARG(arg_set_id, "chrome_mojo_event_info.ipc_hash"))
           when name="ThreadControllerImpl::RunTask" or
                name="ThreadPool_RunTask" then
            printf("RunTask(posted_from=%s:%s)",
             EXTRACT_ARG(arg_set_id, "task.posted_from.file_name"),
             EXTRACT_ARG(arg_set_id, "task.posted_from.function_name"))
         end
         from slice where id=$slice_id'
    );`);
  }

  async queryFlowEvents(query: string, callback: (flows: Flow[]) => void) {
    const result = await this.engine.query(query);
    const flows: Flow[] = [];

    const it = result.iter({
      beginSliceId: NUM,
      beginTrackId: NUM,
      beginSliceName: STR_NULL,
      beginSliceChromeCustomName: STR_NULL,
      beginSliceCategory: STR_NULL,
      beginSliceStartTs: LONG,
      beginSliceEndTs: LONG,
      beginDepth: NUM,
      beginThreadName: STR_NULL,
      beginProcessName: STR_NULL,
      endSliceId: NUM,
      endTrackId: NUM,
      endSliceName: STR_NULL,
      endSliceChromeCustomName: STR_NULL,
      endSliceCategory: STR_NULL,
      endSliceStartTs: LONG,
      endSliceEndTs: LONG,
      endDepth: NUM,
      endThreadName: STR_NULL,
      endProcessName: STR_NULL,
      name: STR_NULL,
      category: STR_NULL,
      id: NUM,
      flowToDescendant: NUM,
    });

    const nullToStr = (s: null | string): string => {
      return s === null ? 'NULL' : s;
    };

    const nullToUndefined = (s: null | string): undefined | string => {
      return s === null ? undefined : s;
    };

    const nodes = [];

    for (; it.valid(); it.next()) {
      // Category and name present only in version 1 flow events
      // It is most likelly NULL for all other versions
      const category = nullToUndefined(it.category);
      const name = nullToUndefined(it.name);
      const id = it.id;

      const begin = {
        trackId: it.beginTrackId,
        sliceId: asSliceSqlId(it.beginSliceId),
        sliceName: nullToStr(it.beginSliceName),
        sliceChromeCustomName: nullToUndefined(it.beginSliceChromeCustomName),
        sliceCategory: nullToStr(it.beginSliceCategory),
        sliceStartTs: Time.fromRaw(it.beginSliceStartTs),
        sliceEndTs: Time.fromRaw(it.beginSliceEndTs),
        depth: it.beginDepth,
        threadName: nullToStr(it.beginThreadName),
        processName: nullToStr(it.beginProcessName),
      };

      const end = {
        trackId: it.endTrackId,
        sliceId: asSliceSqlId(it.endSliceId),
        sliceName: nullToStr(it.endSliceName),
        sliceChromeCustomName: nullToUndefined(it.endSliceChromeCustomName),
        sliceCategory: nullToStr(it.endSliceCategory),
        sliceStartTs: Time.fromRaw(it.endSliceStartTs),
        sliceEndTs: Time.fromRaw(it.endSliceEndTs),
        depth: it.endDepth,
        threadName: nullToStr(it.endThreadName),
        processName: nullToStr(it.endProcessName),
      };

      nodes.push(begin);
      nodes.push(end);

      flows.push({
        id,
        begin,
        end,
        dur: it.endSliceStartTs - it.beginSliceEndTs,
        category,
        name,
        flowToDescendant: !!it.flowToDescendant,
      });
    }

    // Everything below here is a horrible hack to support flows for
    // async slice tracks.
    // In short the issue is this:
    // - For most slice tracks there is a one-to-one mapping between
    //   the track in the UI and the track in the TP. n.b. Even in this
    //   case the UI 'trackId' and the TP 'track.id' may not be the
    //   same. In this case 'depth' in the TP is the exact depth in the
    //   UI.
    // - In the case of aysnc tracks however the mapping is
    //   one-to-many. Each async slice track in the UI is 'backed' but
    //   multiple TP tracks. In order to render this track we need
    //   to adjust depth to avoid overlapping slices. In the render
    //   path we use experimental_slice_layout for this purpose. This
    //   is a virtual table in the TP which, for an arbitrary collection
    //   of TP trackIds, computes for each slice a 'layout_depth'.
    // - Everything above in this function and its callers doesn't
    //   know anything about layout_depth.
    //
    // So if we stopped here we would have incorrect rendering for
    // async slice tracks. Instead we want to 'fix' depth for these
    // cases. We do this in two passes.
    // - First we collect all the information we need in 'Info' POJOs
    // - Secondly we loop over those Infos querying
    //   the database to find the layout_depth for each sliceId
    // TODO(hjd): This should not be needed after TracksV2 lands.

    // We end up with one Info POJOs for each UI async slice track
    // which has at least  one flow {begin,end}ing in one of its slices.
    interface Info {
      siblingTrackIds: number[];
      sliceIds: number[];
      nodes: Array<{
        sliceId: number;
        depth: number;
      }>;
    }

    const trackUriToInfo = new Map<string, null | Info>();
    const trackIdToInfo = new Map<number, null | Info>();

    const trackIdToTrack = new Map<number, TrackDescriptor>();
    this.trackMgr
      .getAllTracks()
      .forEach((trackDescriptor) =>
        trackDescriptor.tags?.trackIds?.forEach((trackId) =>
          trackIdToTrack.set(trackId, trackDescriptor),
        ),
      );

    const getInfo = (trackId: number): null | Info => {
      let info = trackIdToInfo.get(trackId);
      if (info !== undefined) {
        return info;
      }

      const trackDescriptor = trackIdToTrack.get(trackId);
      if (trackDescriptor === undefined) {
        trackIdToInfo.set(trackId, null);
        return null;
      }

      info = trackUriToInfo.get(trackDescriptor.uri);
      if (info !== undefined) {
        return info;
      }

      // If 'trackIds' is undefined this is not an async slice track so
      // we don't need to do anything. We also don't need to do
      // anything if there is only one TP track in this async track. In
      // that case experimental_slice_layout is just an expensive way
      // to find out depth === layout_depth.
      const trackIds = trackDescriptor?.tags?.trackIds;
      if (trackIds === undefined || trackIds.length <= 1) {
        trackUriToInfo.set(trackDescriptor.uri, null);
        trackIdToInfo.set(trackId, null);
        return null;
      }

      const newInfo = {
        siblingTrackIds: [...trackIds],
        sliceIds: [],
        nodes: [],
      };

      trackUriToInfo.set(trackDescriptor.uri, newInfo);
      trackIdToInfo.set(trackId, newInfo);

      return newInfo;
    };

    // First pass, collect:
    // - all slices that belong to async slice track
    // - grouped by the async slice track in question
    for (const node of nodes) {
      const info = getInfo(node.trackId);
      if (info !== null) {
        info.sliceIds.push(node.sliceId);
        info.nodes.push(node);
      }
    }

    // Second pass, for each async track:
    // - Query to find the layout_depth for each relevant sliceId
    // - Iterate through the nodes updating the depth in place
    for (const info of trackUriToInfo.values()) {
      if (info === null) {
        continue;
      }
      const r = await this.engine.query(`
        SELECT
          id,
          layout_depth as depth
        FROM
          experimental_slice_layout
        WHERE
          filter_track_ids = '${info.siblingTrackIds.join(',')}'
          AND id in (${info.sliceIds.join(', ')})
      `);

      // Create the sliceId -> new depth map:
      const it = r.iter({
        id: NUM,
        depth: NUM,
      });
      const sliceIdToDepth = new Map<number, number>();
      for (; it.valid(); it.next()) {
        sliceIdToDepth.set(it.id, it.depth);
      }

      // For each begin/end from an async track update the depth:
      for (const node of info.nodes) {
        const newDepth = sliceIdToDepth.get(node.sliceId);
        if (newDepth !== undefined) {
          node.depth = newDepth;
        }
      }
    }

    callback(flows);
  }

  sliceSelected(sliceId: number) {
    const connectedFlows = SHOW_INDIRECT_PRECEDING_FLOWS_FLAG.get()
      ? `(
           select * from directly_connected_flow(${sliceId})
           union
           select * from preceding_flow(${sliceId})
         )`
      : `directly_connected_flow(${sliceId})`;

    const query = `
    -- Include slices.flow to initialise indexes on 'flow.slice_in' and 'flow.slice_out'.
    INCLUDE PERFETTO MODULE slices.flow;

    select
      f.slice_out as beginSliceId,
      t1.track_id as beginTrackId,
      t1.name as beginSliceName,
      CHROME_CUSTOM_SLICE_NAME(t1.slice_id) as beginSliceChromeCustomName,
      t1.category as beginSliceCategory,
      t1.ts as beginSliceStartTs,
      (t1.ts+t1.dur) as beginSliceEndTs,
      t1.depth as beginDepth,
      (thread_out.name || ' ' || thread_out.tid) as beginThreadName,
      (process_out.name || ' ' || process_out.pid) as beginProcessName,
      f.slice_in as endSliceId,
      t2.track_id as endTrackId,
      t2.name as endSliceName,
      CHROME_CUSTOM_SLICE_NAME(t2.slice_id) as endSliceChromeCustomName,
      t2.category as endSliceCategory,
      t2.ts as endSliceStartTs,
      (t2.ts+t2.dur) as endSliceEndTs,
      t2.depth as endDepth,
      (thread_in.name || ' ' || thread_in.tid) as endThreadName,
      (process_in.name || ' ' || process_in.pid) as endProcessName,
      extract_arg(f.arg_set_id, 'cat') as category,
      extract_arg(f.arg_set_id, 'name') as name,
      f.id as id,
      slice_is_ancestor(t1.slice_id, t2.slice_id) as flowToDescendant
    from ${connectedFlows} f
    join slice t1 on f.slice_out = t1.slice_id
    join slice t2 on f.slice_in = t2.slice_id
    left join thread_track track_out on track_out.id = t1.track_id
    left join thread thread_out on thread_out.utid = track_out.utid
    left join thread_track track_in on track_in.id = t2.track_id
    left join thread thread_in on thread_in.utid = track_in.utid
    left join process process_out on process_out.upid = thread_out.upid
    left join process process_in on process_in.upid = thread_in.upid
    `;
    this.queryFlowEvents(query, (flows: Flow[]) =>
      this.setConnectedFlows(flows),
    );
  }

  private areaSelected(area: AreaSelection) {
    const trackIds: number[] = [];

    for (const trackInfo of area.tracks) {
      const kind = trackInfo?.tags?.kind;
      if (
        kind === THREAD_SLICE_TRACK_KIND ||
        kind === ACTUAL_FRAMES_SLICE_TRACK_KIND
      ) {
        if (trackInfo?.tags?.trackIds) {
          for (const trackId of trackInfo.tags.trackIds) {
            trackIds.push(trackId);
          }
        }
      }
    }

    const tracks = `(${trackIds.join(',')})`;

    const startNs = area.start;
    const endNs = area.end;

    const query = `
    select
      f.slice_out as beginSliceId,
      t1.track_id as beginTrackId,
      t1.name as beginSliceName,
      CHROME_CUSTOM_SLICE_NAME(t1.slice_id) as beginSliceChromeCustomName,
      t1.category as beginSliceCategory,
      t1.ts as beginSliceStartTs,
      (t1.ts+t1.dur) as beginSliceEndTs,
      t1.depth as beginDepth,
      NULL as beginThreadName,
      NULL as beginProcessName,
      f.slice_in as endSliceId,
      t2.track_id as endTrackId,
      t2.name as endSliceName,
      CHROME_CUSTOM_SLICE_NAME(t2.slice_id) as endSliceChromeCustomName,
      t2.category as endSliceCategory,
      t2.ts as endSliceStartTs,
      (t2.ts+t2.dur) as endSliceEndTs,
      t2.depth as endDepth,
      NULL as endThreadName,
      NULL as endProcessName,
      extract_arg(f.arg_set_id, 'cat') as category,
      extract_arg(f.arg_set_id, 'name') as name,
      f.id as id,
      slice_is_ancestor(t1.slice_id, t2.slice_id) as flowToDescendant
    from flow f
    join slice t1 on f.slice_out = t1.slice_id
    join slice t2 on f.slice_in = t2.slice_id
    where
      (t1.track_id in ${tracks}
        and (t1.ts+t1.dur <= ${endNs} and t1.ts+t1.dur >= ${startNs}))
      or
      (t2.track_id in ${tracks}
        and (t2.ts <= ${endNs} and t2.ts >= ${startNs}))
    `;
    this.queryFlowEvents(query, (flows: Flow[]) =>
      this.setSelectedFlows(flows),
    );
  }

  private setConnectedFlows(connectedFlows: Flow[]) {
    this._connectedFlows = connectedFlows;
    // If a chrome slice is selected and we have any flows in connectedFlows
    // we will find the flows on the right and left of that slice to set a default
    // focus. In all other cases the focusedFlowId(Left|Right) will be set to -1.
    this._focusedFlowIdLeft = -1;
    this._focusedFlowIdRight = -1;
    if (this._curSelection?.kind === 'track_event') {
      const sliceId = this._curSelection.eventId;
      for (const flow of connectedFlows) {
        if (flow.begin.sliceId === sliceId) {
          this._focusedFlowIdRight = flow.id;
        }
        if (flow.end.sliceId === sliceId) {
          this._focusedFlowIdLeft = flow.id;
        }
      }
    }
    raf.scheduleFullRedraw();
  }

  private setSelectedFlows(selectedFlows: Flow[]) {
    this._selectedFlows = selectedFlows;
    raf.scheduleFullRedraw();
  }

  updateFlows(selection: Selection) {
    this.initialize();
    this._curSelection = selection;

    if (selection.kind === 'empty') {
      this.setConnectedFlows([]);
      this.setSelectedFlows([]);
      return;
    }

    // TODO(b/155483804): This is a hack as annotation slices don't contain
    // flows. We should tidy this up when fixing this bug.
    if (selection.kind === 'track_event' && selection.tableName === 'slice') {
      this.sliceSelected(selection.eventId);
    } else {
      this.setConnectedFlows([]);
    }

    if (selection.kind === 'area') {
      this.areaSelected(selection);
    } else {
      this.setConnectedFlows([]);
    }
  }

  // Change focus to the next flow event (matching the direction)
  focusOtherFlow(direction: FlowDirection) {
    const currentSelection = this._curSelection;
    if (!currentSelection || currentSelection.kind !== 'track_event') {
      return;
    }
    const sliceId = currentSelection.eventId;
    if (sliceId === -1) {
      return;
    }

    const boundFlows = this._connectedFlows.filter(
      (flow) =>
        (flow.begin.sliceId === sliceId && direction === 'Forward') ||
        (flow.end.sliceId === sliceId && direction === 'Backward'),
    );

    if (direction === 'Backward') {
      const nextFlowId = findAnotherFlowExcept(
        boundFlows,
        this._focusedFlowIdLeft,
      );
      this._focusedFlowIdLeft = nextFlowId;
    } else {
      const nextFlowId = findAnotherFlowExcept(
        boundFlows,
        this._focusedFlowIdRight,
      );
      this._focusedFlowIdRight = nextFlowId;
    }
    raf.scheduleFullRedraw();
  }

  // Select the slice connected to the flow in focus
  moveByFocusedFlow(direction: FlowDirection): void {
    const currentSelection = this._curSelection;
    if (!currentSelection || currentSelection.kind !== 'track_event') {
      return;
    }

    const sliceId = currentSelection.eventId;
    const flowId =
      direction === 'Backward'
        ? this._focusedFlowIdLeft
        : this._focusedFlowIdRight;

    if (sliceId === -1 || flowId === -1) {
      return;
    }

    // Find flow that is in focus and select corresponding slice
    for (const flow of this._connectedFlows) {
      if (flow.id === flowId) {
        const flowPoint = direction === 'Backward' ? flow.begin : flow.end;
        const track = this.getCurWorkspace().flatTracks.find((t) => {
          if (t.uri === undefined) return false;
          return this.trackMgr
            .getTrack(t.uri)
            ?.tags?.trackIds?.includes(flowPoint.trackId);
        });
        if (track) {
          this.selectionMgr.selectSqlEvent('slice', flowPoint.sliceId, {
            scrollToSelection: true,
          });
        }
      }
    }
  }

  get connectedFlows() {
    return this._connectedFlows;
  }

  get selectedFlows() {
    return this._selectedFlows;
  }

  get focusedFlowIdLeft() {
    return this._focusedFlowIdLeft;
  }
  get focusedFlowIdRight() {
    return this._focusedFlowIdRight;
  }

  get visibleCategories(): ReadonlyMap<string, boolean> {
    return this._visibleCategories;
  }

  setCategoryVisible(name: string, value: boolean) {
    this._visibleCategories.set(name, value);
    raf.scheduleFullRedraw();
  }
}

// Search |boundFlows| for |flowId| and return the id following it.
// Returns the first flow id if nothing was found or |flowId| was the last flow
// in |boundFlows|, and -1 if |boundFlows| is empty
function findAnotherFlowExcept(boundFlows: Flow[], flowId: number): number {
  let selectedFlowFound = false;

  if (boundFlows.length === 0) {
    return -1;
  }

  for (const flow of boundFlows) {
    if (selectedFlowFound) {
      return flow.id;
    }

    if (flow.id === flowId) {
      selectedFlowFound = true;
    }
  }
  return boundFlows[0].id;
}
