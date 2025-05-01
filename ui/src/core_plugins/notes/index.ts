// Copyright (C) 2025 The Android Open Source Project
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
import {NotesManager} from './notes_manager';
import {PerfettoPlugin} from '../../public/plugin';
import {TraceImpl} from '../../core/trace_impl';

export default class implements PerfettoPlugin {
  static readonly id = 'perfetto.Notes';
  async onTraceLoad(trace: TraceImpl): Promise<void> {
    trace.tabs.registerTab({
      uri: 'perfetto.Notes#NotesManager',
      isEphemeral: false,
      content: {
        getTitle: () => 'Notes & markers',
        render: () => m(NotesManager, {trace}),
      },
    });

    trace.commands.registerCommand({
      id: 'perfetto.SetTemporarySpanNote',
      name: 'Set the temporary span note based on the current selection',
      callback: () => {
        const range = trace.selection.findTimeRangeOfSelection();
        if (range) {
          trace.notes.addSpanNote({
            start: range.start,
            end: range.end,
            id: '__temp__',
          });

          // Also select an area for this span
          const selection = trace.selection.selection;
          if (selection.kind === 'track_event') {
            trace.selection.selectArea({
              start: range.start,
              end: range.end,
              trackUris: [selection.trackUri],
            });
          }
        }
      },
      defaultHotkey: 'M',
    });

    trace.commands.registerCommand({
      id: 'perfetto.AddSpanNote',
      name: 'Add a new span note based on the current selection',
      callback: () => {
        const range = trace.selection.findTimeRangeOfSelection();
        if (range) {
          trace.notes.addSpanNote({
            start: range.start,
            end: range.end,
          });
        }
      },
      defaultHotkey: 'Shift+M',
    });

    trace.commands.registerCommand({
      id: 'perfetto.RemoveSelectedNote',
      name: 'Remove selected note',
      callback: () => {
        const selection = trace.selection.selection;
        if (selection.kind === 'note') {
          trace.notes.removeNote(selection.id);
        }
      },
      defaultHotkey: 'Delete',
    });
  }
}
