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

import m from 'mithril';

import {QueryResult} from '../../base/query_slot';
import {addDebugSliceTrack} from '../../components/tracks/debug_tracks';
import {PerfettoPlugin} from '../../public/plugin';
import {Selection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {NUM_NULL} from '../../trace_processor/query_result';

import {
  AndroidLockContentionEventSource,
  LockContentionDetails,
} from './android_lock_contention_event_source';
import {AndroidLockContentionTab} from './tab';

export default class AndroidLockContentionPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidLockContention';
  static readonly description =
    'Visualise lock contention events in the trace. Activate by running the command ' +
    "'Android Lock Contention: Go Forward' or using Ctrl+]. You can then navigate " +
    'between contention events using Ctrl+] and Ctrl+[';

  private nav = new LockContentionNavigation();
  private activeContentionId?: number;

  /**
   * Navigates to the source of a monitor contention.
   * Jumps to the parent slice in the chain if available, otherwise
   * shifts focus to the track of the blocking thread.
   */
  private async contextualJump(trace: Trace) {
    const selection = trace.selection.selection;
    if (selection.kind !== 'track_event') return;

    const queryRes = await trace.engine.query(`
      SELECT 
        parent_id,
        (SELECT id FROM thread_track WHERE utid = blocking_utid) as track_id
      FROM android_monitor_contention_chain
      WHERE id = ${selection.eventId}
    `);

    if (queryRes.numRows() === 0) return;

    const row = queryRes.firstRow({parent_id: NUM_NULL, track_id: NUM_NULL});

    if (row.parent_id !== null) {
      this.nav.push(selection);
      trace.selection.selectSqlEvent('slice', row.parent_id, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: false,
      });
      this.nav.push({
        kind: 'track_event',
        trackUri: 'unknown',
        eventId: row.parent_id,
      } as Selection);
    } else if (row.track_id !== null) {
      const track = trace.tracks.findTrack((t) =>
        t.tags?.trackIds?.includes(row.track_id as number),
      );
      const trackUri = track?.uri || '/slice_' + row.track_id;
      this.nav.push(selection);
      trace.selection.selectTrack(trackUri, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: false,
      });
      this.nav.push({kind: 'track', trackUri} as Selection);
    }
  }

  private useDetailsState(
    trace: Trace,
    source: AndroidLockContentionEventSource,
  ): QueryResult<LockContentionDetails | null> {
    const selection = trace.selection.selection;

    if (this.nav.has(selection) && this.activeContentionId !== undefined) {
      return source.use(this.activeContentionId);
    }

    if (selection.kind !== 'track_event') {
      return {data: null, isPending: false, isFresh: true};
    }

    this.activeContentionId = selection.eventId;
    return source.use(selection.eventId);
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.monitor_contention;',
    );

    const source = new AndroidLockContentionEventSource(trace);

    trace.tabs.registerTab({
      uri: 'com.android.AndroidLockContentionTab',
      isEphemeral: false,
      content: {
        getTitle: () => 'Lock Contention Analysis',
        render: () => {
          const {data: row, isPending} = this.useDetailsState(trace, source);
          const goToSlice = (id: number) => {
            this.nav.push(trace.selection.selection);
            trace.selection.selectSqlEvent('slice', id, {
              scrollToSelection: true,
              switchToCurrentSelectionTab: false,
            });
            this.nav.push({
              kind: 'track_event',
              trackUri: 'unknown',
              eventId: id,
            } as Selection);
          };
          const goToTrack = (uri: string) => {
            this.nav.push(trace.selection.selection);
            trace.selection.selectTrack(uri, {
              scrollToSelection: true,
              switchToCurrentSelectionTab: false,
            });
            this.nav.push({kind: 'track', trackUri: uri} as Selection);
          };
          return m(AndroidLockContentionTab, {
            trace,
            row: row ?? null,
            isPending,
            goToSlice,
            goToTrack,
          });
        },
      },
    });

    trace.commands.registerCommand({
      id: 'com.android.AndroidLockContention:GoBack',
      name: 'Android Lock Contention: Go Back',
      defaultHotkey: 'Ctrl+[',
      callback: () => {
        trace.tabs.showTab('com.android.AndroidLockContentionTab');
        this.nav.goBack(trace);
      },
    });

    trace.commands.registerCommand({
      id: 'com.android.AndroidLockContention:GoForward',
      name: 'Android Lock Contention: Go Forward',
      defaultHotkey: 'Ctrl+]',
      callback: async () => {
        trace.tabs.showTab('com.android.AndroidLockContentionTab');
        if (this.nav.canGoForward()) {
          this.nav.goForward(trace);
        } else {
          await this.contextualJump(trace);
        }
      },
    });

    // Visualise "big" locks on a debug track
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
  }
}

class LockContentionNavigation {
  private stack: Selection[] = [];
  private index = -1;

  push(selection: Selection) {
    if (this.index >= 0 && selectionsEqual(this.stack[this.index], selection)) {
      return;
    }
    this.stack.splice(this.index + 1);
    this.stack.push(selection);
    this.index++;
  }

  goBack(trace: Trace) {
    if (this.index > 0) {
      this.index--;
      this.restore(trace, this.stack[this.index]);
    }
  }

  goForward(trace: Trace) {
    if (this.index < this.stack.length - 1) {
      this.index++;
      this.restore(trace, this.stack[this.index]);
    }
  }

  canGoForward(): boolean {
    return this.index < this.stack.length - 1;
  }

  private restore(trace: Trace, selection: Selection) {
    if (selection.kind === 'track_event') {
      if (selection.trackUri === 'unknown' || selection.trackUri === '') {
        trace.selection.selectSqlEvent('slice', selection.eventId, {
          scrollToSelection: true,
          switchToCurrentSelectionTab: false,
        });
      } else {
        trace.selection.selectTrackEvent(
          selection.trackUri,
          selection.eventId,
          {
            scrollToSelection: true,
            switchToCurrentSelectionTab: false,
          },
        );
      }
    } else if (selection.kind === 'track') {
      trace.selection.selectTrack(selection.trackUri, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: false,
      });
    }
  }

  has(selection: Selection) {
    return this.stack.some((s) => selectionsEqual(s, selection));
  }
}

function selectionsEqual(a: Selection, b: Selection): boolean {
  if (a.kind !== b.kind) return false;
  if (a.kind === 'track_event' && b.kind === 'track_event') {
    return a.eventId === b.eventId;
  }
  if (a.kind === 'track' && b.kind === 'track') {
    return a.trackUri === b.trackUri;
  }
  return false;
}
