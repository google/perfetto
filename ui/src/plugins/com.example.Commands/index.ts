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

import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';

// This is just an example plugin, used to prove that the plugin system works.
export default class implements PerfettoPlugin {
  static readonly id = 'com.example.Commands';
  static readonly description =
    'Example plugin to show how to register commands.';

  static onActivate(app: App): void {
    // Register a command that logs "Hello, world!" to the console.
    app.commands.registerCommand({
      id: 'com.example.Commands#LogHelloWorld',
      name: 'Log "Hello, world!"',
      callback: () => console.log('Hello, world!'),
    });

    // Register a command which can be triggered using a hotkey.
    app.commands.registerCommand({
      id: 'com.example.Commands#CommandWithHotkey',
      name: 'Log "Hello, world!" with hotkey',
      callback: () => console.log('Hello, world!'),
      defaultHotkey: 'Mod+Shift+H',
    });
  }

  async onTraceLoad(trace: Trace) {
    // Register a command that logs "Hello, trace!" to the console. This command
    // is only available when a trace is loaded. It'll automatically be removed
    // when the trace is closed, or a new trace is loaded.
    trace.commands.registerCommand({
      id: 'com.example.Commands#LogHelloTrace',
      name: 'Log "Hello, trace!"',
      callback: () => console.log('Hello, trace!'),
    });
  }
}
