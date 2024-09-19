// Copyright (C) 2024 The Android Open Source Project
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

import {Actions} from '../../common/actions';
import {globals} from '../../frontend/globals';
import {App} from '../../public/app';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';

const EXAMPLE_ANDROID_TRACE_URL =
  'https://storage.googleapis.com/perfetto-misc/example_android_trace_15s';

const EXAMPLE_CHROME_TRACE_URL =
  'https://storage.googleapis.com/perfetto-misc/chrome_example_wikipedia.perfetto_trace.gz';

function openTraceUrl(url: string): void {
  globals.logging.logEvent('Trace Actions', 'Open example trace');
  globals.dispatch(Actions.openTraceFromUrl({url}));
}

class ExampleTracesPlugin implements PerfettoPlugin {
  onActivate(ctx: App) {
    const OPEN_EXAMPLE_ANDROID_TRACE_COMMAND_ID =
      'perfetto.CoreCommands#openExampleAndroidTrace';
    ctx.commands.registerCommand({
      id: OPEN_EXAMPLE_ANDROID_TRACE_COMMAND_ID,
      name: 'Open Android example',
      callback: () => {
        openTraceUrl(EXAMPLE_ANDROID_TRACE_URL);
      },
    });
    ctx.sidebar.addMenuItem({
      commandId: OPEN_EXAMPLE_ANDROID_TRACE_COMMAND_ID,
      group: 'example_traces',
      icon: 'description',
    });

    const OPEN_EXAMPLE_CHROME_TRACE_COMMAND_ID =
      'perfetto.CoreCommands#openExampleChromeTrace';
    ctx.commands.registerCommand({
      id: OPEN_EXAMPLE_CHROME_TRACE_COMMAND_ID,
      name: 'Open Chrome example',
      callback: () => {
        openTraceUrl(EXAMPLE_CHROME_TRACE_URL);
      },
    });
    ctx.sidebar.addMenuItem({
      commandId: OPEN_EXAMPLE_CHROME_TRACE_COMMAND_ID,
      group: 'example_traces',
      icon: 'description',
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.ExampleTraces',
  plugin: ExampleTracesPlugin,
};
