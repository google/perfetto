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

import {Engine} from '../common/engine';
import {featureFlags} from '../common/feature_flags';
import {NUM, STR_NULL} from '../common/query_result';
import {Area} from '../common/state';
import {fromNs, toNs} from '../common/time';
import {Flow} from '../frontend/globals';
import {publishConnectedFlows, publishSelectedFlows} from '../frontend/publish';
import {
  ACTUAL_FRAMES_SLICE_TRACK_KIND,
  Config as ActualConfig,
} from '../tracks/actual_frames';
import {
  Config as SliceConfig,
  SLICE_TRACK_KIND,
} from '../tracks/chrome_slices';

import {Controller} from './controller';
import {globals} from './globals';

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

  queryFlowEvents(query: string, callback: (flows: Flow[]) => void) {
    this.args.engine.query(query).then((result) => {
      const flows: Flow[] = [];
      const it = result.iter({
        beginSliceId: NUM,
        beginTrackId: NUM,
        beginSliceName: STR_NULL,
        beginSliceChromeCustomName: STR_NULL,
        beginSliceCategory: STR_NULL,
        beginSliceStartTs: NUM,
        beginSliceEndTs: NUM,
        beginDepth: NUM,
        beginThreadName: STR_NULL,
        beginProcessName: STR_NULL,
        endSliceId: NUM,
        endTrackId: NUM,
        endSliceName: STR_NULL,
        endSliceChromeCustomName: STR_NULL,
        endSliceCategory: STR_NULL,
        endSliceStartTs: NUM,
        endSliceEndTs: NUM,
        endDepth: NUM,
        endThreadName: STR_NULL,
        endProcessName: STR_NULL,
        name: STR_NULL,
        category: STR_NULL,
        id: NUM,
      });
      for (; it.valid(); it.next()) {
        const beginSliceId = it.beginSliceId;
        const beginTrackId = it.beginTrackId;
        const beginSliceName =
            it.beginSliceName === null ? 'NULL' : it.beginSliceName;
        const beginSliceChromeCustomName =
            it.beginSliceChromeCustomName === null ?
            undefined :
            it.beginSliceChromeCustomName;
        const beginSliceCategory =
            it.beginSliceCategory === null ? 'NULL' : it.beginSliceCategory;
        const beginSliceStartTs = fromNs(it.beginSliceStartTs);
        const beginSliceEndTs = fromNs(it.beginSliceEndTs);
        const beginDepth = it.beginDepth;
        const beginThreadName =
            it.beginThreadName === null ? 'NULL' : it.beginThreadName;
        const beginProcessName =
            it.beginProcessName === null ? 'NULL' : it.beginProcessName;

        const endSliceId = it.endSliceId;
        const endTrackId = it.endTrackId;
        const endSliceName =
            it.endSliceName === null ? 'NULL' : it.endSliceName;
        const endSliceChromeCustomName = it.endSliceChromeCustomName === null ?
            undefined :
            it.endSliceChromeCustomName;
        const endSliceCategory =
            it.endSliceCategory === null ? 'NULL' : it.endSliceCategory;
        const endSliceStartTs = fromNs(it.endSliceStartTs);
        const endSliceEndTs = fromNs(it.endSliceEndTs);
        const endDepth = it.endDepth;
        const endThreadName =
            it.endThreadName === null ? 'NULL' : it.endThreadName;
        const endProcessName =
            it.endProcessName === null ? 'NULL' : it.endProcessName;

        // Category and name present only in version 1 flow events
        // It is most likelly NULL for all other versions
        const category = it.category === null ? undefined : it.category;
        const name = it.name === null ? undefined : it.name;
        const id = it.id;

        flows.push({
          id,
          begin: {
            trackId: beginTrackId,
            sliceId: beginSliceId,
            sliceName: beginSliceName,
            sliceChromeCustomName: beginSliceChromeCustomName,
            sliceCategory: beginSliceCategory,
            sliceStartTs: beginSliceStartTs,
            sliceEndTs: beginSliceEndTs,
            depth: beginDepth,
            threadName: beginThreadName,
            processName: beginProcessName,
          },
          end: {
            trackId: endTrackId,
            sliceId: endSliceId,
            sliceName: endSliceName,
            sliceChromeCustomName: endSliceChromeCustomName,
            sliceCategory: endSliceCategory,
            sliceStartTs: endSliceStartTs,
            sliceEndTs: endSliceEndTs,
            depth: endDepth,
            threadName: endThreadName,
            processName: endProcessName,
          },
          dur: endSliceStartTs - beginSliceEndTs,
          category,
          name,
        });
      }
      callback(flows);
    });
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
        this.lastSelectedArea.endSec === area.endSec &&
        this.lastSelectedArea.startSec === area.startSec) {
      return;
    }

    this.lastSelectedArea = area;
    this.lastSelectedKind = 'AREA';

    const trackIds: number[] = [];

    for (const uiTrackId of area.tracks) {
      const track = globals.state.tracks[uiTrackId];
      if (track === undefined) {
        continue;
      }
      if (track.kind === SLICE_TRACK_KIND) {
        trackIds.push((track.config as SliceConfig).trackId);
      } else if (track.kind === ACTUAL_FRAMES_SLICE_TRACK_KIND) {
        const actualConfig = track.config as ActualConfig;
        for (const trackId of actualConfig.trackIds) {
          trackIds.push(trackId);
        }
      }
    }

    const tracks = `(${trackIds.join(',')})`;

    const startNs = toNs(area.startSec);
    const endNs = toNs(area.endSec);

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
