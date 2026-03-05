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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {AndroidLockContentionEventSource} from './android_lock_contention_event_source';
import {AndroidLockContentionTab} from './tab';
import {TrackPinningManager} from '../../components/related_events/utils';
import {addDebugSliceTrack} from '../../components/tracks/debug_tracks';

interface LockContentionArgs {
  blockingTrackUri?: string;
  blockingSliceId?: number;
}

export default class AndroidLockContentionPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidLockContention';
  static readonly description = `
  This plugin shows the blocking thread which is causing monitor contention.

  To use this, when selecting a track event beginning with 'monitor contention', you can call the command 'Android Lock
  Contention: Toggle Blocked/Blocking Slice' which shows a tab with details of the blocking + blocked methods with
  links to navigate. The default hotkey for this command is 'Ctrl+]'.
  `;

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.engine.query('INCLUDE PERFETTO MODULE android.monitor_contention');

    const source = new AndroidLockContentionEventSource(trace);
    const pinningManager = new TrackPinningManager();
    const tab = new AndroidLockContentionTab(
      trace,
      source,
      pinningManager,
      () => {
        trace.tabs.showTab('com.android.AndroidLockContentionTab');
      },
    );

    trace.commands.registerCommand({
      id: 'com.android.ToggleLockContentionNavigation',
      name: 'Android Lock Contention: Toggle Blocked/Blocking Slice',
      defaultHotkey: 'Ctrl+]',
      callback: async () => {
        const selection = trace.selection.selection;
        const tabInstance = tab;
        // Register and show the tab first
        trace.tabs.registerTab({
          uri: 'com.android.AndroidLockContentionTab',
          isEphemeral: true,
          content: tab,
        });
        trace.tabs.showTab('com.android.AndroidLockContentionTab');

        if (selection.kind !== 'track_event') {
          return;
        }

        const data = await source.getRelatedEventData(selection.eventId);

        if (data.events.length > 0) {
          // We are on the blocked slice
          // Update the tab data asynchronously in the background
          tab.load(selection.eventId);

          const event = data.events[0];
          const args = event.customArgs as LockContentionArgs | undefined;
          if (
            args !== undefined &&
            args.blockingTrackUri !== undefined &&
            args.blockingSliceId !== undefined
          ) {
            trace.selection.selectTrackEvent(
              args.blockingTrackUri,
              args.blockingSliceId,
              {
                scrollToSelection: true,
                switchToCurrentSelectionTab: false,
              },
            );
          }
        } else {
          // We are likely on the blocking slice, so source returned 0 events
          // Let's check the existing tab state to see if we can jump back
          const currentEventArgs = tabInstance.getEventArgs();
          if (!currentEventArgs) return;

          const contentionId = tabInstance.getContentionId();
          const {blockingSliceId} = currentEventArgs;

          if (selection.eventId === blockingSliceId) {
            // Currently on blocking, jump back to blocked
            const blockedTrackUri = tabInstance.getEventTrackUri();
            if (blockedTrackUri && contentionId !== undefined) {
              trace.selection.selectTrackEvent(blockedTrackUri, contentionId, {
                scrollToSelection: true,
                switchToCurrentSelectionTab: false,
              });
            }
          } else {
            // Not a recognized part of the contention, just select normally
            trace.selection.selectTrackEvent(
              selection.trackUri,
              selection.eventId,
              {
                scrollToSelection: true,
                switchToCurrentSelectionTab: false,
              },
            );
          }
        }
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
  }
}
