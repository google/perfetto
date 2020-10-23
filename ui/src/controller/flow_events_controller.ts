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
import {fromNs} from '../common/time';
import {Flow} from '../frontend/globals';

import {Controller} from './controller';
import {globals} from './globals';

export interface FlowEventsControllerArgs {
  engine: Engine;
}

export class FlowEventsController extends Controller<'main'> {
  private lastSelectedId?: number;
  private lastSelectedKind?: string;

  constructor(private args: FlowEventsControllerArgs) {
    super('main');
  }

  run() {
    const selection = globals.state.currentSelection;

    if (!selection || selection.kind !== 'CHROME_SLICE' ||
        selection.id === undefined) {
      this.lastSelectedId = undefined;
      this.lastSelectedKind = undefined;
      globals.publish('ConnectedFlows', []);
      return;
    }

    if (selection.kind === this.lastSelectedKind &&
        selection.id === this.lastSelectedId) {
      return;
    }

    this.lastSelectedId = selection.id;
    this.lastSelectedKind = selection.kind;

    const query = `
      select
        f.slice_out, t1.track_id, t1.name, t1.ts, (t1.ts+t1.dur), t1.depth,
        f.slice_in, t2.track_id, t2.name, t2.ts, (t2.ts+t2.dur), t2.depth,
        extract_arg(f.arg_set_id, 'cat'),
        extract_arg(f.arg_set_id, 'name')
      from connected_flow(${selection.id}) f
      join slice t1 on f.slice_out = t1.slice_id
      join slice t2 on f.slice_in = t2.slice_id
      `;

    this.args.engine.query(query).then(res => {
      const flows: Flow[] = [];
      for (let i = 0; i < res.numRecords; i++) {
        const beginSliceId = res.columns[0].longValues![i];
        const beginTrackId = res.columns[1].longValues![i];
        const beginSliceName = res.columns[2].stringValues![i];
        const beginSliceStartTs = fromNs(res.columns[3].longValues![i]);
        const beginSliceEndTs = fromNs(res.columns[4].longValues![i]);
        const beginDepth = res.columns[5].longValues![i];

        const endSliceId = res.columns[6].longValues![i];
        const endTrackId = res.columns[7].longValues![i];
        const endSliceName = res.columns[8].stringValues![i];
        const endSliceStartTs = fromNs(res.columns[9].longValues![i]);
        const endSliceEndTs = fromNs(res.columns[10].longValues![i]);
        const endDepth = res.columns[11].longValues![i];

        // Category and name present only in version 1 flow events
        // It is most likelly NULL for all other versions
        const category = res.columns[12].isNulls![i] ?
            undefined :
            res.columns[12].stringValues![i];
        const name = res.columns[13].isNulls![i] ?
            undefined :
            res.columns[13].stringValues![i];

        flows.push({
          begin: {
            trackId: beginTrackId,
            sliceId: beginSliceId,
            sliceName: beginSliceName,
            sliceStartTs: beginSliceStartTs,
            sliceEndTs: beginSliceEndTs,
            depth: beginDepth
          },
          end: {
            trackId: endTrackId,
            sliceId: endSliceId,
            sliceName: endSliceName,
            sliceStartTs: endSliceStartTs,
            sliceEndTs: endSliceEndTs,
            depth: endDepth
          },
          category,
          name
        });
      }
      globals.publish('ConnectedFlows', flows);
    });
  }
}
