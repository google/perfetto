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

import './styles.scss';
import type {PerfettoPlugin} from '../../public/plugin';
import type {App} from '../../public/app';
import {OPEN_MULTIPLE_TRACES_CMD} from '../../public/exposed_commands';
import {showMultiTraceModal} from './multi_trace_modal';

function isFileArray(value: unknown): value is File[] {
  return Array.isArray(value) && value.every((f) => f instanceof File);
}

function openFilePickerAndShowModal() {
  const input = document.createElement('input');
  input.setAttribute('type', 'file');
  input.setAttribute('multiple', 'multiple');
  input.style.display = 'none';
  input.addEventListener('change', async () => {
    if (!input.files) return;
    const files = Array.from(input.files);
    if (files.length > 0) {
      showMultiTraceModal(files);
    }
  });
  input.click();
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.MultiTraceOpen';

  static onActivate(app: App): void {
    // This command is used in two contexts:
    // 1. From the sidebar or the command palette. In this case the files
    //    argument is undefined and we show a file picker.
    // 2. Invoked via runCommand(...) by openTraceFiles() when the user drags
    //    several files onto the UI or multi-selects in the "Open trace file"
    //    picker. In this case the caller passes the files explicitly.
    app.commands.registerCommand({
      id: OPEN_MULTIPLE_TRACES_CMD,
      name: 'Open multiple trace files',
      callback: (filesArg?: unknown) => {
        if (isFileArray(filesArg)) {
          if (filesArg.length > 0) {
            showMultiTraceModal(filesArg);
          }
          return;
        }
        openFilePickerAndShowModal();
      },
    });
    app.sidebar.addMenuItem({
      commandId: OPEN_MULTIPLE_TRACES_CMD,
      section: 'trace_files',
      icon: 'library_books',
      // Just below "Open trace file" and above everything else.
      sortOrder: 1.5,
    });
  }
  async onTraceLoad(): Promise<void> {}
}
