// Copyright (C) 2026 The Android Open Source Project
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

import {addDebugSliceTrack} from '../../components/tracks/debug_tracks';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';

export default class LockContention implements PerfettoPlugin {
  static readonly id = 'com.android.LockContention';

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'com.android.visualizeHeldLocks',
      name: 'Lock Contention: visualize held locks',
      callback: async () => {
        await addDebugSliceTrack({
          trace: ctx,
          data: {
            sqlSource: `
                WITH lock_held_slices AS (
                SELECT ts, dur, lock_name, utid
                FROM interval_merge_overlapping_partitioned!((
                    SELECT ts, dur, name AS lock_name, utid
                    FROM thread_slice
                    WHERE dur > 0 AND thread_slice.name GLOB '*_lock_held'
                ), (lock_name, utid))
                )
                SELECT
                row_number() OVER () AS id,
                name AS thread_name,
                lock_name,
                utid,
                ts,
                min(lead(ts) OVER(PARTITION BY lock_name ORDER BY ts), ts + dur) - ts AS dur
                FROM lock_held_slices
                JOIN thread USING (utid)
            `,
          },
          title: 'Held Lock',
          columns: {
            name: 'thread_name',
          },
          pivotOn: 'lock_name',
        });
      },
    });
  }
}
