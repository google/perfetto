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

import {makeColorScheme} from '../../components/colorizer';
import {HSLColor} from '../../base/color';
import type {Trace} from '../../public/trace';
import {STR} from '../../trace_processor/query_result';
import {createLogTrack} from '../../components/tracks/log_track';

// Journald priority: 0=EMERG … 7=DEBUG (lower number = worse severity).
// Depth 0 = most severe (EMERG/ALERT/CRIT), depth 4 = least (DEBUG).
const DEPTH_TO_COLOR = [
  makeColorScheme(new HSLColor({h: 291, s: 64, l: 42})), // EMERG/ALERT/CRIT — purple
  makeColorScheme(new HSLColor({h: 4, s: 90, l: 58})), // ERR — red
  makeColorScheme(new HSLColor({h: 45, s: 100, l: 51})), // WARNING — orange
  makeColorScheme(new HSLColor({h: 0, s: 0, l: 70})), // NOTICE/INFO — grey
  makeColorScheme(new HSLColor({h: 122, s: 39, l: 49})), // DEBUG — green
];

export function createJournaldLogTrack(trace: Trace, uri: string) {
  return createLogTrack(trace, uri, {
    title: 'Journald Log',
    rootTableName: 'linux_systemd_journald_logs',
    sqlSource: `
        select
          id,
          ts,
          prio,
          utid,
          tag,
          msg,
          CASE
            WHEN prio <= 2 THEN 0
            WHEN prio = 3 THEN 1
            WHEN prio = 4 THEN 2
            WHEN prio >= 5 AND prio <= 6 THEN 3
            WHEN prio = 7 THEN 4
            ELSE -1
          END as depth
        from linux_systemd_journald_logs
        order by ts
        -- linux_systemd_journald_logs aren't guaranteed to be ordered by ts, but this is a
        -- requirement for SliceTrack's mipmap operator to work
        -- correctly, so we must explicitly sort them above.
      `,
    depthColors: DEPTH_TO_COLOR,
    fetchMsg: async (t, id) => {
      const result = await t.engine.query(
        `select msg from linux_systemd_journald_logs where id = ${id}`,
      );
      return result.maybeFirstRow({msg: STR})?.msg;
    },
  });
}
