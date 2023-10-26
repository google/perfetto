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
import {Engine} from '../common/engine';
import {featureFlags} from '../common/feature_flags';
import {pluginManager} from '../common/plugins';
import {LONG, NUM, STR_NULL} from '../common/query_result';
import {Area} from '../common/state';
import {Flow, globals} from '../frontend/globals';
import {publishConnectedFlows, publishSelectedFlows} from '../frontend/publish';
import {asSliceSqlId} from '../frontend/sql_types';
import {ACTUAL_FRAMES_SLICE_TRACK_KIND} from '../tracks/actual_frames';
import {SLICE_TRACK_KIND} from '../tracks/chrome_slices';

import {Controller} from './controller';

export interface FlowEventsControllerArgs {
  engine: Engine;
}

const SHOW_INDIRECT_PRECEDING_FLOWS_FLAG = featureFlags.register({
  id: 'showIndirectPrecedingFlows',
  name: 'Show indirect preceding flows',
  description: 'Show indirect preceding flows (connected through ancestor ' +
      'slices) when a slice is selected.',
  defaultValue: false,
});

export class FlowEventsController extends Controller<'main'> {
  private lastSelectedSliceId?: number;
  private lastSelectedArea?: Area;
  private lastSelectedKind: 'CHROME_SLICE'|'AREA'|'NONE' = 'NONE';

  constructor(private args: FlowEventsControllerArgs) {
    super('main');

    // Create |CHROME_CUSTOME_SLICE_NAME| helper, which combines slice name
    // and args for some slices (scheduler tasks and mojo messages) for more
    // helpful messages.
    // In the future, it should be replaced with this a more scalable and
    // customisable solution.
    // Note that a function here is significantly faster than a join.
    this.args.engine.query(`
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
    const result = await this.args.engine.query(query);
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
    });

    const nullToStr = (s: null|string): string => {
      return s === null ? 'NULL' : s;
    };

    const nullToUndefined = (s: null|string): undefined|string => {
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
      uiTrackId: string;
      siblingTrackIds: number[];
      sliceIds: number[];
      nodes: Array<{
        sliceId: number,
        depth: number,
      }>;
    }

    const uiTrackIdToInfo = new Map<string, null|Info>();
    const trackIdToInfo = new Map<number, null|Info>();

    const trackIdToUiTrackId = globals.state.trackKeyByTrackId;
    const tracks = globals.state.tracks;

    const getInfo = (trackId: number): null|Info => {
      let info = trackIdToInfo.get(trackId);
      if (info !== undefined) {
        return info;
      }

      const uiTrackId = trackIdToUiTrackId[trackId];
      if (uiTrackId === undefined) {
        trackIdToInfo.set(trackId, null);
        return null;
      }

      const track = tracks[uiTrackId];
      if (track === undefined) {
        trackIdToInfo.set(trackId, null);
        return null;
      }

      info = uiTrackIdToInfo.get(uiTrackId);
      if (info !== undefined) {
        return info;
      }

      // If 'trackIds' is undefined this is not an async slice track so
      // we don't need to do anything. We also don't need to do
      // anything if there is only one TP track in this async track. In
      // that case experimental_slice_layout is just an expensive way
      // to find out depth === layout_depth.
      const trackInfo = pluginManager.resolveTrackInfo(track.uri);
      const trackIds = trackInfo?.trackIds;
      if (trackIds === undefined || trackIds.length <= 1) {
        uiTrackIdToInfo.set(uiTrackId, null);
        trackIdToInfo.set(trackId, null);
        return null;
      }

      const newInfo = {
        uiTrackId,
        siblingTrackIds: trackIds,
        sliceIds: [],
        nodes: [],
      };

      uiTrackIdToInfo.set(uiTrackId, newInfo);
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
    for (const info of uiTrackIdToInfo.values()) {
      if (info === null) {
        continue;
      }
      const r = await this.args.engine.query(`
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
    if (this.lastSelectedKind === 'CHROME_SLICE' &&
        this.lastSelectedSliceId === sliceId) {
      return;
    }
    this.lastSelectedSliceId = sliceId;
    this.lastSelectedKind = 'CHROME_SLICE';

    const connectedFlows = SHOW_INDIRECT_PRECEDING_FLOWS_FLAG.get() ?
        `(
           select * from directly_connected_flow(${sliceId})
           union
           select * from preceding_flow(${sliceId})
         )` :
        `directly_connected_flow(${sliceId})`;

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
      f.id as id
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
    this.queryFlowEvents(
        query, (flows: Flow[]) => publishConnectedFlows(flows));
  }

  areaSelected(areaId: string) {
    const area = globals.state.areas[areaId];
    if (this.lastSelectedKind === 'AREA' && this.lastSelectedArea &&
        this.lastSelectedArea.tracks.join(',') === area.tracks.join(',') &&
        this.lastSelectedArea.end === area.end &&
        this.lastSelectedArea.start === area.start) {
      return;
    }

    this.lastSelectedArea = area;
    this.lastSelectedKind = 'AREA';

    const trackIds: number[] = [];

    for (const uiTrackId of area.tracks) {
      const track = globals.state.tracks[uiTrackId];
      if (track?.uri !== undefined) {
        const trackInfo = pluginManager.resolveTrackInfo(track.uri);
        const kind = trackInfo?.kind;
        if (kind === SLICE_TRACK_KIND ||
            kind === ACTUAL_FRAMES_SLICE_TRACK_KIND) {
          if (trackInfo?.trackIds) {
            for (const trackId of trackInfo.trackIds) {
              trackIds.push(trackId);
            }
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
      f.id as id
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
    this.queryFlowEvents(query, (flows: Flow[]) => publishSelectedFlows(flows));
  }

  refreshVisibleFlows() {
    const selection = globals.state.currentSelection;
    if (!selection) {
      this.lastSelectedKind = 'NONE';
      publishConnectedFlows([]);
      publishSelectedFlows([]);
      return;
    }

    // TODO(b/155483804): This is a hack as annotation slices don't contain
    // flows. We should tidy this up when fixing this bug.
    if (selection && selection.kind === 'CHROME_SLICE' &&
        selection.table !== 'annotation') {
      this.sliceSelected(selection.id);
    } else {
      publishConnectedFlows([]);
    }

    if (selection && selection.kind === 'AREA') {
      this.areaSelected(selection.areaId);
    } else {
      publishSelectedFlows([]);
    }
  }

  run() {
    this.refreshVisibleFlows();
  }
}
