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
import {GenericRelatedEventsOverlay} from '../dev.perfetto.RelatedEvents/generic_overlay';
import {AndroidLockContentionEventSource} from './android_lock_contention_event_source';
import {AndroidLockContentionTab} from './tab';

export default class AndroidLockContentionPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidLockContention';
  static readonly dependencies = [RelatedEventsPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.engine.query('INCLUDE PERFETTO MODULE android.monitor_contention');

    const overlay = new GenericRelatedEventsOverlay(trace);
    trace.tracks.registerOverlay(overlay);

    const source = new AndroidLockContentionEventSource(trace);

    const tab = new AndroidLockContentionTab({trace, source});
    source.setOnDataLoadedCallback((data) => {
      overlay.update(data);
    });

    trace.tabs.registerTab({
      uri: 'com.android.AndroidLockContentionTab',
      isEphemeral: false,
      content: tab,
      onHide() {
        overlay.update({
          events: [],
          relations: [],
        });
      },
    });

    trace.commands.registerCommand({
      id: 'openAndroidLockContentionTab',
      name: 'Show Android Lock Contention',
      callback: () => {
        trace.tabs.showTab('com.android.AndroidLockContentionTab');
        tab.syncSelection();
      },
    });
  }
}
