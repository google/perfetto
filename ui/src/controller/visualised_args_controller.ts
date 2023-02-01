// Copyright (C) 2022 The Android Open Source Project
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

import {v4 as uuidv4} from 'uuid';

import {Actions, AddTrackArgs} from '../common/actions';
import {Engine} from '../common/engine';
import {NUM} from '../common/query_result';
import {InThreadTrackSortKey} from '../common/state';
import {globals as frontendGlobals} from '../frontend/globals';
import {
  VISUALISED_ARGS_SLICE_TRACK_KIND,
} from '../tracks/visualised_args/index';

import {Controller} from './controller';
import {globals} from './globals';

export interface VisualisedArgControllerArgs {
  argName: string;
  engine: Engine;
}

export class VisualisedArgController extends Controller<'init'|'running'> {
  private engine: Engine;
  private argName: string;
  private escapedArgName: string;
  private tableName: string;
  private addedTrackIds: string[];

  constructor(args: VisualisedArgControllerArgs) {
    super('init');
    this.argName = args.argName;
    this.engine = args.engine;
    this.escapedArgName = this.argName.replace(/[^a-zA-Z]/g, '_');
    this.tableName = `__arg_visualisation_helper_${this.escapedArgName}_slice`;
    this.addedTrackIds = [];
  }

  onDestroy() {
    this.engine.query(`drop table if exists ${this.tableName}`);
    frontendGlobals.dispatch(
        Actions.removeVisualisedArgTracks({trackIds: this.addedTrackIds}));
  }

  async createTracks() {
    const result = await this.engine.query(`
        drop table if exists ${this.tableName};

        create table ${this.tableName} as
        with slice_with_arg as (
          select
            slice.id,
            slice.track_id,
            slice.ts,
            slice.dur,
            slice.thread_dur,
            NULL as cat,
            args.display_value as name
          from slice
          join args using (arg_set_id)
          where args.key='${this.argName}'
        )
        select
          *,
          (select count()
           from ancestor_slice(s1.id) s2
           join slice_with_arg s3 on s2.id=s3.id
          ) as depth
        from slice_with_arg s1
        order by id;

        select
          track_id as trackId,
          max(depth) as maxDepth
        from ${this.tableName}
        group by track_id;
    `);

    const tracksToAdd: AddTrackArgs[] = [];
    const it = result.iter({'trackId': NUM, 'maxDepth': NUM});
    for (; it.valid(); it.next()) {
      const track =
          globals.state
              .tracks[globals.state.uiTrackIdByTraceTrackId[it.trackId]];
      const utid = (track.trackSortKey as {utid?: number}).utid;
      const id = uuidv4();
      this.addedTrackIds.push(id);
      tracksToAdd.push({
        id,
        trackGroup: track.trackGroup,
        engineId: this.engine.id,
        kind: VISUALISED_ARGS_SLICE_TRACK_KIND,
        name: this.argName,
        trackSortKey: utid === undefined ?
            track.trackSortKey :
            {utid, priority: InThreadTrackSortKey.VISUALISED_ARGS_TRACK},
        config: {
          maxDepth: it.maxDepth,
          namespace: `__arg_visualisation_helper_${this.escapedArgName}`,
          trackId: it.trackId,
          argName: this.argName,
          tid: (track.config as {tid?: number}).tid,
        },
      });
    }
    frontendGlobals.dispatch(Actions.addTracks({tracks: tracksToAdd}));
    frontendGlobals.dispatch(Actions.sortThreadTracks({}));
  }

  run() {
    switch (this.state) {
      case 'init':
        this.createTracks();
        this.setState('running');
        break;
      case 'running':
        // Nothing to do here.
        break;
      default:
        throw new Error(`Unexpected state ${this.state}`);
    }
  }
}
