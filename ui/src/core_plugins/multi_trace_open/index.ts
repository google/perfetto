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

import {PerfettoPlugin} from '../../public/plugin';
import {App} from '../../public/app';
import {showMultiTraceModal} from './multi_trace_modal';

const MULTI_TRACE_COMMAND_ID = 'dev.perfetto.MultiTraceOpen#openMultipleTraces';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.MultiTraceOpen';

  static onActivate(app: App): void {
    app.commands.registerCommand({
      id: MULTI_TRACE_COMMAND_ID,
      name: 'Open multiple trace files',
      callback: () => showMultiTraceModal(),
    });
    app.sidebar.addMenuItem({
      commandId: MULTI_TRACE_COMMAND_ID,
      section: 'navigation',
      icon: 'folder_open',
    });
  }
  async onTraceLoad(): Promise<void> {}
}
