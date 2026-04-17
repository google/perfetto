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

import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {
  isLegacyTrace,
  openFileWithLegacyTraceViewer,
  openInOldUIWithSizeCheck,
} from './legacy_trace_viewer';

const OPEN_LEGACY_COMMAND_ID = 'dev.perfetto.OpenTraceInLegacyUi';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Catapult';

  static onActivate(app: App): void {
    app.commands.registerCommand({
      id: OPEN_LEGACY_COMMAND_ID,
      name: 'Open with legacy UI',
      callback: () => {
        const input = document.createElement('input');
        input.type = 'file';
        input.style.display = 'none';
        document.body.appendChild(input);
        input.addEventListener('change', async () => {
          const file = input.files?.[0];
          input.remove();
          if (!file) return;
          app.analytics.logEvent('Trace Actions', 'Open trace in Legacy UI');
          if (await isLegacyTrace(file)) {
            await openFileWithLegacyTraceViewer(file);
          } else {
            await openInOldUIWithSizeCheck(file);
          }
        });
        input.click();
      },
    });

    app.sidebar.addMenuItem({
      commandId: OPEN_LEGACY_COMMAND_ID,
      section: 'trace_files',
      icon: 'filter_none',
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.sidebar.addMenuItem({
      section: 'convert_trace',
      text: 'Switch to legacy UI',
      icon: 'filter_none',
      disabled: () => {
        return trace.traceInfo.downloadable
          ? false
          : 'Cannot download external trace';
      },
      action: async () => {
        trace.analytics.logEvent(
          'Trace Actions',
          'Open current trace in legacy UI',
        );
        const file = await trace.getTraceFile();
        await openInOldUIWithSizeCheck(file);
      },
    });
  }
}
