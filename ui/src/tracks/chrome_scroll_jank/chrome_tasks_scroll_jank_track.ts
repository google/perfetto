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

import {Engine} from '../../common/engine';
import {NUM} from '../../common/query_result';
import {InThreadTrackSortKey} from '../../common/state';
import {
  NamedSliceTrack,
  NamedSliceTrackTypes,
} from '../../frontend/named_slice_track';
import {runQueryInNewTab} from '../../frontend/query_result_tab';
import {NewTrackArgs, TrackBase} from '../../frontend/track';

import {
  ENABLE_CHROME_SCROLL_JANK_PLUGIN,
  ScrollJankTracks as DecideTracksResult,
} from './index';

interface ChromeTasksScrollJankTrackConfig {}

interface ChromeTasksScrollJankTrackTypes extends NamedSliceTrackTypes {
  config: ChromeTasksScrollJankTrackConfig;
}

export class ChromeTasksScrollJankTrack extends
    NamedSliceTrack<ChromeTasksScrollJankTrackTypes> {
  static readonly kind = 'org.chromium.ScrollJank.BrowserUIThreadLongTasks';
  static create(args: NewTrackArgs): TrackBase {
    return new ChromeTasksScrollJankTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
  }

  getSqlSource(): string {
    return `select s2.ts as ts, s2.dur as dur, s2.id as id, 0 as depth, s1.full_name as name
from chrome_tasks_delaying_input_processing s1
join slice s2 on s2.id=s1.slice_id`;
  }
}
export type GetTrackGroupUuidFn = (utid: number, upid: number|null) => string;

export async function decideTracks(
    engine: Engine,
    getTrackGroupUuid: GetTrackGroupUuidFn): Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };
  if (!ENABLE_CHROME_SCROLL_JANK_PLUGIN.get()) {
    return result;
  }

  const queryResult = await engine.query(`
    select utid, upid
    from thread
    where name='CrBrowserMain'
    `);

  const it = queryResult.iter({
    utid: NUM,
    upid: NUM,
  });

  if (!it.valid()) {
    return result;
  }

  result.tracksToAdd.push({
    uri: 'perfetto.ChromeScrollJank',
    trackSortKey: {
      utid: it.utid,
      priority: InThreadTrackSortKey.ORDINARY,
    },
    name: 'Scroll Jank causes - long tasks',
    trackGroup: getTrackGroupUuid(it.utid, it.upid),
  });

  // Initialise the chrome_tasks_delaying_input_processing table. It will be
  // used in the sql table above.
  // TODO(stevegolton): Use viewer.tabs.openQuery().
  await engine.query(`
select RUN_METRIC(
   'chrome/chrome_tasks_delaying_input_processing.sql',
   'duration_causing_jank_ms',
   /* duration_causing_jank_ms = */ '8');`);

  const query = `
     select
       s1.full_name,
       s1.duration_ms,
       s1.slice_id,
       s1.thread_dur_ms,
       s2.id,
       s2.ts,
       s2.dur,
       s2.track_id
     from chrome_tasks_delaying_input_processing s1
     join slice s2 on s1.slice_id=s2.id
     `;
  runQueryInNewTab(query, 'Scroll Jank: long tasks');

  return result;
}
