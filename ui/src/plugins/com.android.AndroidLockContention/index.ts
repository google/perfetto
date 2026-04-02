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

import {QuerySlot, SerialTaskQueue} from '../../base/query_slot';
import {LockOwnerDetailsPanel} from './lock_owner_details_panel';
import {TrackNode} from '../../public/workspace';
import {Selection} from '../../public/selection';
import {Time} from '../../base/time';
import {LONG, NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {HSLColor} from '../../base/color';
import {ArrowConnection} from '../../components/related_events/arrow_visualiser';
import {
  getTrackUriForTrackId,
  enrichDepths,
} from '../../components/related_events/utils';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {addDebugSliceTrack} from '../../components/tracks/debug_tracks';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {RelatedEventsOverlay} from '../../components/related_events/related_events_overlay';

export default class AndroidLockContentionPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidLockContention';
  static readonly description =
    'Visualise lock contention events in the trace. Activate by running the command ' +
    "'Android Lock Contention: Toggle View' or using Ctrl+]. You can then navigate " +
    'between contention events using Ctrl+] and Ctrl+[';

  private readonly connectionsTaskQueue = new SerialTaskQueue();
  private readonly connectionsSlot = new QuerySlot<ArrowConnection[]>(
    this.connectionsTaskQueue,
  );

  /**
   * Navigates to the source of a monitor contention.
   * Jumps to the parent slice in the chain if available, otherwise
   * shifts focus to the track of the blocking thread.
   */
  private async contextualJump(trace: Trace) {
    const selection = trace.selection.selection;
    if (selection.kind !== 'track_event') return;

    const tableName = '__android_lock_contention_owner_events';

    // Case 1: On a debug track -> Jump to original
    if (
      selection.trackUri.startsWith(
        'com.android.AndroidLockContention#OwnerEvents',
      )
    ) {
      const query = await trace.engine.query(`
        SELECT original_id FROM ${tableName} WHERE id = ${selection.eventId} LIMIT 1
      `);
      if (query.numRows() > 0) {
        const originalId = query.firstRow({original_id: NUM}).original_id;
        trace.selection.selectSqlEvent('slice', originalId, {
          scrollToSelection: true,
          switchToCurrentSelectionTab: false,
        });
        return;
      }
    }

    // Case 2: On the original track -> Jump to debug
    const query = await trace.engine.query(`
      SELECT owner_tid, id FROM ${tableName} WHERE original_id = ${selection.eventId} LIMIT 1
    `);
    if (query.numRows() > 0) {
      const row = query.firstRow({owner_tid: NUM, id: NUM});
      const targetUri = `com.android.AndroidLockContention#OwnerEvents_${row.owner_tid}`;
      trace.selection.selectTrackEvent(targetUri, row.id, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: false,
      });
      return;
    }
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.monitor_contention;',
    );

    trace.tracks.registerOverlay(
      new RelatedEventsOverlay(trace, () => this.getConnections(trace)),
    );

    trace.commands.registerCommand({
      id: 'com.android.AndroidLockContention:ToggleView',
      name: 'Android Lock Contention: Toggle View',
      defaultHotkey: 'Ctrl+]',
      callback: () => {
        this.contextualJump(trace);
      },
    });

    trace.commands.registerCommand({
      id: 'com.android.visualiseHeldLocks',
      name: 'Lock Contention: Visualise held locks',
      callback: async () => {
        await addDebugSliceTrack({
          trace: trace,
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
                    MIN(LEAD(ts) OVER(PARTITION BY lock_name ORDER BY ts), ts + dur) - ts AS dur
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

    const tableName = '__android_lock_contention_owner_events';
    await trace.engine.query(`
      CREATE PERFETTO TABLE ${tableName} AS
      WITH lock_events AS (
        SELECT 
          s.id AS original_id,
          s.ts, 
          s.dur, 
          '[Lock Owner] Blocking: ' || s.name AS name,
          CAST(SUBSTR(
            s.name, 
            INSTR(s.name, '(owner tid: ') + 12, 
            INSTR(s.name, ')') - (INSTR(s.name, '(owner tid: ') + 12)
          ) AS INTEGER) AS owner_tid,
          bt.name AS blocked_thread_name,
          obt.name AS blocking_thread_name
        FROM slice s
        JOIN thread_track tt ON s.track_id = tt.id
        JOIN thread bt USING (utid)
        LEFT JOIN thread obt ON obt.tid = (
          CAST(SUBSTR(
            s.name, 
            INSTR(s.name, '(owner tid: ') + 12, 
            INSTR(s.name, ')') - (INSTR(s.name, '(owner tid: ') + 12)
          ) AS INTEGER)
        ) AND obt.upid = bt.upid
        WHERE s.name GLOB 'Lock contention*' AND s.name GLOB '*(owner tid: *)*'
      ),
      monitor_events AS (
        SELECT 
          id AS original_id,
          ts,
          COALESCE(dur, -1) AS dur,
          '[Lock Owner] Blocking: Lock contention on a monitor lock (owner tid: ' || COALESCE(blocking_tid, '-') || ')' AS name,
          blocking_tid AS owner_tid,
          blocked_thread_name,
          blocking_thread_name
        FROM android_monitor_contention_chain
      ),
      all_events AS (
        SELECT * FROM lock_events
        UNION ALL
        SELECT * FROM monitor_events
      ),
      events_with_depth AS (
        SELECT *,
          internal_layout(ts, dur) OVER (PARTITION BY owner_tid ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS depth
        FROM all_events e
      )
      SELECT 
        row_number() OVER () AS id,
        * 
      FROM events_with_depth
      -- Filter out null TIDs.
      WHERE owner_tid IS NOT NULL
    `);

    const tidsQuery = await trace.engine.query(`
      SELECT DISTINCT owner_tid FROM __android_lock_contention_owner_events
    `);
    const tidsIt = tidsQuery.iter({owner_tid: NUM});

    for (; tidsIt.valid(); tidsIt.next()) {
      const tid = tidsIt.owner_tid;
      if (tid === null) continue; // Skip invalid TIDs

      const ownerTrackUri = `com.android.AndroidLockContention#OwnerEvents_${tid}`;

      const threadNameQuery = await trace.engine.query(`
        SELECT name FROM thread WHERE tid = ${tid} LIMIT 1
      `);
      let threadName = 'Unknown';
      if (threadNameQuery.numRows() > 0) {
        threadName =
          threadNameQuery.firstRow({name: STR_NULL}).name || 'Unknown';
      }
      const trackName = `${threadName} [${tid}] Lock Owners`;

      trace.tracks.registerTrack({
        uri: ownerTrackUri,
        renderer: SliceTrack.create({
          trace,
          uri: ownerTrackUri,
          dataset: new SourceDataset({
            schema: {
              id: NUM,
              ts: LONG,
              dur: LONG,
              name: STR,
              depth: NUM,
            },
            src: '__android_lock_contention_owner_events',
            filter: {
              col: 'owner_tid',
              eq: tid,
            },
          }),

          sliceName: (row) => row.name,
          colorizer: (_) => {
            // Default to a visible blue-ish color
            return {
              base: new HSLColor([210, 80, 50]),
              variant: new HSLColor([210, 80, 60]),
              disabled: new HSLColor([210, 80, 50], 0.5),
              textBase: new HSLColor([0, 0, 100]),
              textVariant: new HSLColor([0, 0, 100]),
              textDisabled: new HSLColor([0, 0, 100], 0.5),
            };
          },
          sliceLayout: {
            sliceHeight: 18,
            titleSizePx: 12,
          },
          detailsPanel: (row) => new LockOwnerDetailsPanel(trace, row.id),
        }),
      });

      const ownerTrackNode = new TrackNode({
        uri: ownerTrackUri,
        name: trackName,
        removable: true,
      });
      trace.currentWorkspace.pinnedTracksNode.addChildLast(ownerTrackNode);
      await moveTrackToThread(trace, ownerTrackNode, tid);
    }
  }
  private getConnections(trace: Trace): ArrowConnection[] {
    const selection = trace.selection.selection;
    if (selection.kind !== 'track_event') {
      return [];
    }

    const result = this.connectionsSlot.use({
      key: {eventId: selection.eventId},
      queryFn: () => this.fetchConnections(trace, selection),
    });

    return result.data ?? [];
  }

  private async fetchConnections(
    trace: Trace,
    selection: Selection & {kind: 'track_event'},
  ): Promise<ArrowConnection[]> {
    const trackUri = selection.trackUri;
    const eventId = selection.eventId;

    // Case 1: Custom Owner track selected (New -> Old)
    if (trackUri.startsWith('com.android.AndroidLockContention#OwnerEvents')) {
      const tableName = '__android_lock_contention_owner_events';
      const query = await trace.engine.query(`
        SELECT original_id, ts, depth FROM ${tableName} WHERE id = ${eventId} LIMIT 1
      `);
      if (query.numRows() === 0) return [];
      const row = query.firstRow({original_id: NUM, ts: LONG, depth: NUM});

      // Manual lookup of track URI since enrichDepths requires real trackUri to find depth.
      const sliceQuery = await trace.engine.query(`
        SELECT track_id FROM slice WHERE id = ${row.original_id} LIMIT 1
      `);
      if (sliceQuery.numRows() === 0) return [];
      const trackId = sliceQuery.firstRow({track_id: NUM}).track_id;

      const origTrackUri = getTrackUriForTrackId(trace, trackId);
      if (!origTrackUri) return [];

      const targets = [{id: row.original_id, trackUri: origTrackUri, depth: 0}];
      await enrichDepths(trace, targets);

      return [
        {
          start: {
            trackUri: trackUri,
            ts: Time.fromRaw(row.ts),
            depth: row.depth,
          },
          end: {
            trackUri: origTrackUri,
            ts: Time.fromRaw(row.ts),
            depth: targets[0].depth,
          },
        },
      ];
    }

    // Case 2: Original track selected (Old -> New)
    const tableName = '__android_lock_contention_owner_events';
    const query = await trace.engine.query(`
      SELECT owner_tid, id, ts, depth FROM ${tableName} WHERE original_id = ${eventId} LIMIT 1
    `);
    if (query.numRows() > 0) {
      const row = query.firstRow({
        owner_tid: NUM,
        id: NUM,
        ts: LONG,
        depth: NUM,
      });
      const ownerTrackUri = `com.android.AndroidLockContention#OwnerEvents_${row.owner_tid}`;

      const targets = [{id: eventId, trackUri: trackUri, depth: 0}];
      await enrichDepths(trace, targets);

      return [
        {
          start: {
            trackUri: trackUri,
            ts: Time.fromRaw(row.ts),
            depth: targets[0].depth,
          },
          end: {
            trackUri: ownerTrackUri,
            ts: Time.fromRaw(row.ts),
            depth: row.depth,
          },
        },
      ];
    }

    return [];
  }
}

async function moveTrackToThread(
  trace: Trace,
  nodeToMove: TrackNode,
  tid: number,
) {
  const query = await trace.engine.query(`
    SELECT t.id FROM thread_track t JOIN thread USING (utid) WHERE tid = ${tid} LIMIT 1
  `);
  if (query.numRows() === 0) return;
  const trackId = query.firstRow({id: NUM}).id;

  const track = trace.tracks.findTrack((t) =>
    t.tags?.trackIds?.includes(trackId),
  );
  if (!track) return;

  const threadNode = trace.currentWorkspace.getTrackByUri(track.uri);
  if (!threadNode || !threadNode.parent) return;

  nodeToMove.remove();
  threadNode.parent.addChildBefore(nodeToMove, threadNode);
}
