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
import RelatedEventsPlugin from '../dev.perfetto.RelatedEvents';
import {AndroidLockContentionEventSource} from './android_lock_contention_event_source';
import {AndroidLockContentionTab} from './tab';

export default class AndroidLockContentionPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidLockContention';
  static readonly description = `
  This plugin shows the blocking thread which is causing monitor contention.

  To use this, when selecting a track event beginning with 'monitor contention', you can call the command 'Android Lock
  Contention: Toggle Blocked/Blocking Slice' which shows a tab with details of the blocking + blocked methods with
  links to navigate. The default hotkey for this command is ']'.
  `;
  static readonly dependencies = [RelatedEventsPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.engine.query('INCLUDE PERFETTO MODULE android.monitor_contention');

    const source = new AndroidLockContentionEventSource(trace);
    const tab = new AndroidLockContentionTab({trace, source});

    trace.tabs.registerTab({
      uri: 'com.android.AndroidLockContentionTab',
      isEphemeral: false,
      content: tab,
    });

    trace.commands.registerCommand({
      id: 'toggleContentionNavigation',
      name: 'Android Lock Contention: Toggle Blocked/Blocking Slice',
      defaultHotkey: ']',
      callback: async () => {
        const selection = trace.selection.selection;
        const tabInstance = tab;

        trace.tabs.showTab('com.android.AndroidLockContentionTab');

        if (!tabInstance.hasEvent()) {
          if (selection.kind === 'track_event') {
            await tabInstance.loadData(selection.eventId);
          }
          return;
        }

        const currentEventArgs = tabInstance.getEventArgs();
        if (!currentEventArgs) return;

        const contentionId = tabInstance.getContentionId();
        const {blockingTrackUri, blockingSliceId} = currentEventArgs;

        if (selection.kind === 'track_event') {
          if (selection.eventId === contentionId) {
            // Currently on blocked, jump to blocking
            if (blockingTrackUri && blockingSliceId !== undefined) {
              trace.selection.selectTrackEvent(
                blockingTrackUri,
                blockingSliceId,
                {
                  scrollToSelection: true,
                  switchToCurrentSelectionTab: false,
                },
              );
            }
          } else if (selection.eventId === blockingSliceId) {
            // Currently on blocking, jump back to blocked
            const blockedTrackUri = tabInstance.getEventTrackUri();
            if (blockedTrackUri && contentionId !== undefined) {
              trace.selection.selectTrackEvent(blockedTrackUri, contentionId, {
                scrollToSelection: true,
                switchToCurrentSelectionTab: false,
              });
            }
          } else {
            // New selection, load it
            await tabInstance.loadData(selection.eventId);
          }
        } else {
          // No selection, do nothing to the navigation
        }
      },
    });
  }
}
