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
import {assertUnreachable} from '../base/logging';
import {Icons} from '../base/semantic_icons';
import {Timestamp} from '../components/widgets/timestamp';
import {TraceImpl} from '../core/trace_impl';
import {Note, SpanNote} from '../public/note';
import {NoteSelection} from '../public/selection';
import {Button} from '../widgets/button';
import {TextInput, TextInputAttrs} from '../widgets/text_input';
import {Tree, TreeNode} from '../widgets/tree';
import {DetailsShell} from '../widgets/details_shell';
import {Section} from '../widgets/section';
import {GridLayout} from '../widgets/grid_layout';

function getStartTimestamp(note: Note | SpanNote) {
  const noteType = note.noteType;
  switch (noteType) {
    case 'SPAN':
      return note.start;
    case 'DEFAULT':
      return note.timestamp;
    default:
      assertUnreachable(noteType);
  }
}

interface NodeDetailsPanelAttrs {
  readonly trace: TraceImpl;
  readonly selection: NoteSelection;
}

export class NoteEditor implements m.ClassComponent<NodeDetailsPanelAttrs> {
  view(vnode: m.CVnode<NodeDetailsPanelAttrs>) {
    const {selection, trace} = vnode.attrs;
    const id = selection.id;
    const note = trace.notes.getNote(id);
    if (note === undefined) {
      return m('.', `No Note with id ${id}`);
    }
    const startTime = getStartTimestamp(note);
    return m(
      DetailsShell,
      {
        title: 'Note',
        key: id, // Every note should get its own brand new DOM.
      },
      m(
        GridLayout,
        m(
          Section,
          {title: 'Details'},
          m(
            Tree,
            m(TreeNode, {
              left: 'Annotation',
              right: m(Timestamp, {trace, ts: startTime}),
            }),
            m(TreeNode, {
              left: 'Name',
              right: m(TextInput, {
                oncreate: (v: m.VnodeDOM<TextInputAttrs>) => {
                  // NOTE: due to bad design decisions elsewhere this component is
                  // rendered every time the mouse moves on the canvas. We cannot set
                  // `value: note.text` as an input as that will clobber the input
                  // value as we move the mouse.
                  const inputElement = v.dom as HTMLInputElement;
                  inputElement.value = note.text;
                },
                onchange: (e: InputEvent) => {
                  const newText = (e.target as HTMLInputElement).value;
                  trace.notes.changeNote(id, {text: newText});
                },
              }),
            }),
            m(TreeNode, {
              left: 'Color',
              right: m('input[type=color]', {
                value: note.color,
                onchange: (e: Event) => {
                  const newColor = (e.target as HTMLInputElement).value;
                  trace.notes.changeNote(id, {color: newColor});
                },
              }),
            }),
            m(Button, {
              label: 'Remove',
              icon: Icons.Delete,
              onclick: () => trace.notes.removeNote(id),
            }),
          ),
        ),
      ),
    );
  }
}
