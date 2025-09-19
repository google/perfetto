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
import {Button} from '../../widgets/button';
import {Icons} from '../../base/semantic_icons';
import {TraceImpl} from '../../core/trace_impl';

interface NotesManagerAttrs {
  readonly trace: TraceImpl;
}

export class NotesManager implements m.ClassComponent<NotesManagerAttrs> {
  view({attrs}: m.CVnode<NotesManagerAttrs>) {
    const notes = attrs.trace.notes.notes;
    if (notes.size === 0) {
      return 'No notes found';
    }

    return m(
      'table',
      m(
        'thead',
        m(
          'tr',
          m('td', 'ID'),
          m('td', 'Color'),
          m('td', 'Type'),
          m('td', 'Text'),
          m('td', 'Delete'),
        ),
      ),
      m(
        'tbody',
        Array.from(notes.entries()).map(([id, note]) => {
          return m(
            'tr',
            m('td', id),
            m('td', note.color),
            m('td', note.noteType),
            m('td', note.text),
            m(
              'td',
              m(Button, {
                icon: Icons.Delete,
                onclick: () => {
                  attrs.trace.notes.removeNote(id);
                },
              }),
            ),
          );
        }),
      ),
    );
  }
}
