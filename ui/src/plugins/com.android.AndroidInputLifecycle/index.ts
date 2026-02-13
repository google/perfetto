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
import RelatedEventsPlugin, {
  TrackPinningManager,
} from '../dev.perfetto.RelatedEvents';
import {GenericRelatedEventsOverlay} from '../dev.perfetto.RelatedEvents/generic_overlay';
import {AndroidInputEventSource} from './android_input_event_source';
import {AndroidInputLifecycleTab} from './tab';

export default class AndroidInputLifecyclePlugin implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidInputLifecycle';
  static readonly description = `
  Visualise connected input events in the lifecycle from touch to frame, with latencies for the various input stages. 
  Activate by running the command 'Android: View Input Lifecycle'
  `;
  static readonly dependencies = [RelatedEventsPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.engine.query('INCLUDE PERFETTO MODULE android.input');

    const overlay = new GenericRelatedEventsOverlay(trace);
    trace.tracks.registerOverlay(overlay);

    const source = new AndroidInputEventSource(trace);
    source.setOnDataLoadedCallback((data) => {
      overlay.update(data);
    });

    const pinningManager = new TrackPinningManager();

    const tab = new AndroidInputLifecycleTab(trace, source, pinningManager);

    trace.tabs.registerTab({
      uri: 'com.android.AndroidInputLifecycleTab',
      isEphemeral: false,
      content: tab,
    });

    trace.commands.registerCommand({
      id: 'openAndroidInputLifecycleTab',
      name: 'Android: View Input Lifecycle',
      callback: () => {
        trace.tabs.showTab('com.android.AndroidInputLifecycleTab');
      },
    });
  }
}
