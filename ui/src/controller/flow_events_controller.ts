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
import {Area} from '../common/state';
import {fromNs, toNs} from '../common/time';
import {Flow} from '../frontend/globals';
import {Config, SLICE_TRACK_KIND} from '../tracks/chrome_slices/common';

import {Controller} from './controller';
import {globals} from './globals';

export interface FlowEventsControllerArgs {
  engine: Engine;
}

export class FlowEventsController extends Controller<'main'> {
  private lastSelectedSliceId?: number;
  private lastSelectedArea?: Area;
  private lastSelectedKind: 'CHROME_SLICE'|'AREA'|'NONE' = 'NONE';

  constructor(private args: FlowEventsControllerArgs) {
    super('main');
  }

  queryFlowEvents(query: string, callback: (flows: Flow[]) => void) {
    this.args.engine.query(query).then(res => {
      const flows: Flow[] = [];
      for (let i = 0; i < res.numRecords; i++) {
        const beginSliceId = res.columns[0].longValues![i];
        const beginTrackId = res.columns[1].longValues![i];
        const beginSliceName = res.columns[2].stringValues![i];
        const beginSliceCategory = res.columns[3].stringValues![i];
        const beginSliceStartTs = fromNs(res.columns[4].longValues![i]);
        const beginSliceEndTs = fromNs(res.columns[5].longValues![i]);
        const beginDepth = res.columns[6].longValues![i];

        const endSliceId = res.columns[7].longValues![i];
        const endTrackId = res.columns[8].longValues![i];
        const endSliceName = res.columns[9].stringValues![i];
        const endSliceCategory = res.columns[10].stringValues![i];
        const endSliceStartTs = fromNs(res.columns[11].longValues![i]);
        const endSliceEndTs = fromNs(res.columns[12].longValues![i]);
        const endDepth = res.columns[13].longValues![i];

        // Category and name present only in version 1 flow events
        // It is most likelly NULL for all other versions
        const category = res.columns[14].isNulls![i] ?
            undefined :
            res.columns[14].stringValues![i];
        const name = res.columns[15].isNulls![i] ?
            undefined :
            res.columns[15].stringValues![i];

        flows.push({
          begin: {
            trackId: beginTrackId,
            sliceId: beginSliceId,
            sliceName: beginSliceName,
            sliceCategory: beginSliceCategory,
            sliceStartTs: beginSliceStartTs,
            sliceEndTs: beginSliceEndTs,
            depth: beginDepth
          },
          end: {
            trackId: endTrackId,
            sliceId: endSliceId,
            sliceName: endSliceName,
            sliceCategory: endSliceCategory,
            sliceStartTs: endSliceStartTs,
            sliceEndTs: endSliceEndTs,
            depth: endDepth
          },
          category,
          name
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

    const query = `
    select
      f.slice_out, t1.track_id, t1.name,
      t1.category, t1.ts, (t1.ts+t1.dur), t1.depth,
      f.slice_in, t2.track_id, t2.name,
      t2.category, t2.ts, (t2.ts+t2.dur), t2.depth,
      extract_arg(f.arg_set_id, 'cat'),
      extract_arg(f.arg_set_id, 'name')
    from connected_flow(${sliceId}) f
    join slice t1 on f.slice_out = t1.slice_id
    join slice t2 on f.slice_in = t2.slice_id
    `;
    this.queryFlowEvents(
        query, (flows: Flow[]) => globals.publish('ConnectedFlows', flows));
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
      if (globals.state.tracks[uiTrackId] &&
          globals.state.tracks[uiTrackId].kind === SLICE_TRACK_KIND) {
        trackIds.push(
            (globals.state.tracks[uiTrackId].config as Config).trackId);
      }
    }

    const tracks = `(${trackIds.join(',')})`;

    const startNs = toNs(area.startSec);
    const endNs = toNs(area.endSec);

    const query = `
    select
      f.slice_out, t1.track_id, t1.name,
      t1.category, t1.ts, (t1.ts+t1.dur), t1.depth,
      f.slice_in, t2.track_id, t2.name,
      t2.category, t2.ts, (t2.ts+t2.dur), t2.depth,
      extract_arg(f.arg_set_id, 'cat'),
      extract_arg(f.arg_set_id, 'name')
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
    this.queryFlowEvents(
        query, (flows: Flow[]) => globals.publish('SelectedFlows', flows));
  }

  refreshVisibleFlows() {
    const selection = globals.state.currentSelection;
    if (!selection) {
      this.lastSelectedKind = 'NONE';
      globals.publish('ConnectedFlows', []);
      globals.publish('SelectedFlows', []);
      return;
    }

    if (selection && selection.kind === 'CHROME_SLICE') {
      this.sliceSelected(selection.id);
    } else {
      globals.publish('ConnectedFlows', []);
    }

    if (selection && selection.kind === 'AREA') {
      this.areaSelected(selection.areaId);
    } else {
      globals.publish('SelectedFlows', []);
    }
  }

  run() {
    this.refreshVisibleFlows();
  }
}
