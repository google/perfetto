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

import m from 'mithril';
import {globals} from './globals';
import {NotesListEditor} from './notes_list_editor';
import {NotesEditorTab} from './notes_panel';
import {DisposableStack} from '../base/disposable_stack';

/**
 * Registers with the tab manager to show notes details panels when notes are
 * selected.
 *
 * Notes are core functionality thus don't really belong in a plugin.
 */
export class Notes implements Disposable {
  private trash = new DisposableStack();

  constructor() {
    this.trash.use(
      globals.tabManager.registerDetailsPanel(new NotesEditorTab()),
    );

    this.trash.use(
      globals.tabManager.registerTab({
        uri: 'notes.manager',
        isEphemeral: false,
        content: {
          getTitle: () => 'Notes & markers',
          render: () => m(NotesListEditor),
        },
      }),
    );
  }

  [Symbol.dispose]() {
    this.trash.dispose();
  }
}
