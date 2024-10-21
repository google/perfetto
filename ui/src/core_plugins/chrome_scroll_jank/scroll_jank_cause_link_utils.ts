// Copyright (C) 2023 The Android Open Source Project
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

import m from 'mithril';
import {Icons} from '../../base/semantic_icons';
import {duration, Time, time} from '../../base/time';
import {exists} from '../../base/utils';
import {SliceSqlId} from '../../trace_processor/sql_utils/core_types';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {
  CauseProcess,
  CauseThread,
  ScrollJankCauseMap,
} from './scroll_jank_cause_map';
import {scrollTo} from '../../public/scroll_helper';
import {Trace} from '../../public/trace';

const UNKNOWN_NAME = 'Unknown';

export interface EventLatencyStage {
  name: string;
  // Slice id of the top level EventLatency slice (not a stage).
  eventLatencyId: SliceSqlId;
  ts: time;
  dur: duration;
}

export interface EventLatencyCauseThreadTracks {
  // A thread may have multiple tracks associated with it (e.g. from ATrace
  // events).
  trackIds: number[];
  thread: CauseThread;
  causeDescription: string;
}

export async function getScrollJankCauseStage(
  engine: Engine,
  eventLatencyId: SliceSqlId,
): Promise<EventLatencyStage | undefined> {
  const queryResult = await engine.query(`
    SELECT
      IFNULL(cause_of_jank, '${UNKNOWN_NAME}') AS causeOfJank,
      IFNULL(sub_cause_of_jank, '${UNKNOWN_NAME}') AS subCauseOfJank,
      IFNULL(substage.ts, -1) AS ts,
      IFNULL(substage.dur, -1) AS dur
    FROM chrome_janky_frame_presentation_intervals
      JOIN descendant_slice(event_latency_id) substage
    WHERE event_latency_id = ${eventLatencyId}
      AND substage.name = COALESCE(sub_cause_of_jank, cause_of_jank)
  `);

  const causeIt = queryResult.iter({
    causeOfJank: STR,
    subCauseOfJank: STR,
    ts: LONG,
    dur: LONG,
  });

  for (; causeIt.valid(); causeIt.next()) {
    const causeOfJank = causeIt.causeOfJank;
    const subCauseOfJank = causeIt.subCauseOfJank;

    if (causeOfJank == '' || causeOfJank == UNKNOWN_NAME) return undefined;
    const cause = subCauseOfJank == UNKNOWN_NAME ? causeOfJank : subCauseOfJank;
    const stageDetails: EventLatencyStage = {
      name: cause,
      eventLatencyId: eventLatencyId,
      ts: Time.fromRaw(causeIt.ts),
      dur: causeIt.dur,
    };

    return stageDetails;
  }

  return undefined;
}

export async function getEventLatencyCauseTracks(
  engine: Engine,
  scrollJankCauseStage: EventLatencyStage,
): Promise<EventLatencyCauseThreadTracks[]> {
  const threadTracks: EventLatencyCauseThreadTracks[] = [];
  const causeDetails = ScrollJankCauseMap.getEventLatencyDetails(
    scrollJankCauseStage.name,
  );
  if (causeDetails === undefined) return threadTracks;

  for (const cause of causeDetails.jankCauses) {
    switch (cause.process) {
      case CauseProcess.RENDERER:
      case CauseProcess.BROWSER:
      case CauseProcess.GPU:
        const tracksForProcess = await getChromeCauseTracks(
          engine,
          scrollJankCauseStage.eventLatencyId,
          cause.process,
          cause.thread,
        );
        for (const track of tracksForProcess) {
          track.causeDescription = cause.description;
          threadTracks.push(track);
        }
        break;
      case CauseProcess.UNKNOWN:
      default:
        break;
    }
  }

  return threadTracks;
}

async function getChromeCauseTracks(
  engine: Engine,
  eventLatencySliceId: number,
  processName: CauseProcess,
  threadName: CauseThread,
): Promise<EventLatencyCauseThreadTracks[]> {
  const queryResult = await engine.query(`
      INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_jank_cause_utils;

      SELECT DISTINCT
        utid,
        id AS trackId
      FROM thread_track
      WHERE utid IN (
        SELECT DISTINCT
          utid
        FROM chrome_select_scroll_jank_cause_thread(
          ${eventLatencySliceId},
          '${processName}',
          '${threadName}'
        )
      );
  `);

  const it = queryResult.iter({
    utid: NUM,
    trackId: NUM,
  });

  const threadsWithTrack: {[id: number]: EventLatencyCauseThreadTracks} = {};
  const utids: number[] = [];
  for (; it.valid(); it.next()) {
    const utid = it.utid;
    if (!(utid in threadsWithTrack)) {
      threadsWithTrack[utid] = {
        trackIds: [it.trackId],
        thread: threadName,
        causeDescription: '',
      };
      utids.push(utid);
    } else {
      threadsWithTrack[utid].trackIds.push(it.trackId);
    }
  }

  return utids.map((each) => threadsWithTrack[each]);
}

export function getCauseLink(
  trace: Trace,
  threadTracks: EventLatencyCauseThreadTracks,
  tracksByTrackId: Map<number, string>,
  ts: time | undefined,
  dur: duration | undefined,
): m.Child {
  const trackUris: string[] = [];
  for (const trackId of threadTracks.trackIds) {
    const track = tracksByTrackId.get(trackId);
    if (track === undefined) {
      return `Could not locate track ${trackId} for thread ${threadTracks.thread} in the global state`;
    }
    trackUris.push(track);
  }

  if (trackUris.length == 0) {
    return `No valid tracks for thread ${threadTracks.thread}.`;
  }

  // Fixed length of a container to ensure that the icon does not overlap with
  // the text due to table formatting.
  return m(
    `div[style='width:250px']`,
    m(
      Anchor,
      {
        icon: Icons.UpdateSelection,
        onclick: () => {
          scrollTo({
            track: {uri: trackUris[0], expandGroup: true},
          });
          if (exists(ts) && exists(dur)) {
            scrollTo({
              time: {
                start: ts,
                end: Time.fromRaw(ts + dur),
                viewPercentage: 0.3,
              },
            });
            trace.selection.selectArea({
              start: ts,
              end: Time.fromRaw(ts + dur),
              trackUris,
            });
          }
        },
      },
      threadTracks.thread,
    ),
  );
}
