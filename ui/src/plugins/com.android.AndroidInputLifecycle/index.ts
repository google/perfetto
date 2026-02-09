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
import {LifecycleOverlay} from './overlay';
import {AndroidInputTab} from './tab';

export default class AndroidInputLifecyclePlugin implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidInputLifecycle';
  static readonly description = `
    Visualise connected input events in the lifecycle from touch to frame, with latencies for the various input stages. 
    Activate by running the command 'Android: View Input Flow'
    `;

  async onTraceLoad(trace: Trace): Promise<void> {
    await trace.engine.query('INCLUDE PERFETTO MODULE android.input;');

    const overlay = new LifecycleOverlay(trace);
    trace.tracks.registerOverlay(overlay);

    const tab = new AndroidInputTab(trace, overlay);
    const tabUri = 'com.android.InputLifecycles';

    trace.tabs.registerTab({
      uri: tabUri,
      isEphemeral: false,
      content: tab,
      onHide() {
        tab.onHide();
      },
    });

    trace.commands.registerCommand({
      id: 'com.android.AndroidInputLifecycle#ViewFlow',
      name: 'Android: View Input Flow',
      callback: () => trace.tabs.showTab(tabUri),
    });
  }
}
