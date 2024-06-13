// Copyright (C) 2024 The Android Open Source Project
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

import {globals} from '../../frontend/globals';
import {LONG, LONG_NULL, NUM} from '../../public';
import {
  BaseCounterTrack,
  BaseCounterTrackArgs,
} from '../../frontend/base_counter_track';

interface TraceProcessorCounterTrackArgs extends BaseCounterTrackArgs {
  trackId: number;
  rootTable?: string;
}

export class TraceProcessorCounterTrack extends BaseCounterTrack {
  private trackId: number;
  private rootTable: string;

  constructor(args: TraceProcessorCounterTrackArgs) {
    super(args);
    this.trackId = args.trackId;
    this.rootTable = args.rootTable ?? 'counter';
  }

  getSqlSource() {
    return `
      select
        ts,
        value
      from ${this.rootTable}
      where track_id = ${this.trackId}
    `;
  }

  onMouseClick({x}: {x: number}): boolean {
    const {visibleTimeScale} = globals.timeline;
    const time = visibleTimeScale.pxToHpTime(x).toTime('floor');

    const query = `
      select
        id,
        ts as leftTs,
        (
          select ts
          from ${this.rootTable}
          where
            track_id = ${this.trackId}
            and ts >= ${time}
          order by ts
          limit 1
        ) as rightTs
      from ${this.rootTable}
      where
        track_id = ${this.trackId}
        and ts < ${time}
      order by ts DESC
      limit 1
    `;

    this.engine.query(query).then((result) => {
      const it = result.iter({
        id: NUM,
        leftTs: LONG,
        rightTs: LONG_NULL,
      });
      if (!it.valid()) {
        return;
      }
      const id = it.id;
      globals.selectSingleEvent(this.trackKey, id);
    });

    return true;
  }
}
