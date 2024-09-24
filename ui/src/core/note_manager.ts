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

import {
  AddNoteArgs,
  AddSpanNoteArgs,
  Note,
  NoteManager,
  SpanNote,
} from '../public/note';
import {randomColor} from './colorizer';
import {raf} from './raf_scheduler';

export class NoteManagerImpl implements NoteManager {
  private _lastNodeId = 0;
  private _notes = new Map<string, Note | SpanNote>();

  // This function is wired up to clear the SelectionManager state if the
  // current selection is a note.
  // TODO(primiano): figure out some better (de-)coupling here.
  // We cannot pass SelectionManager in our constructor because doing so would
  // create a cyclic ctor dependency (SelectionManager requires NoteManager in
  // its ctor). There is a 2way logical dependency between NoteManager and
  // SelectionManager:
  // 1. SM needs NM to handle SM.findTimeRangeOfSelection(), for [M]ark.
  // 2. NM needs SM to tell it that a note has been delete and should be
  //   deselected if it was currently selected.
  onNoteDeleted?: (nodeId: string) => void;

  get notes(): ReadonlyMap<string, Note | SpanNote> {
    return this._notes;
  }

  getNote(id: string): Note | SpanNote | undefined {
    return this._notes.get(id);
  }

  addNote(args: AddNoteArgs): string {
    const id = args.id ?? `note_${++this._lastNodeId}`;
    this._notes.set(id, {
      ...args,
      noteType: 'DEFAULT',
      id,
      color: args.color ?? randomColor(),
      text: args.text ?? '',
    });
    raf.scheduleFullRedraw();
    return id;
  }

  addSpanNote(args: AddSpanNoteArgs): string {
    const id = args.id ?? `note_${++this._lastNodeId}`;
    this._notes.set(id, {
      ...args,
      noteType: 'SPAN',
      id,
      color: args.color ?? randomColor(),
      text: args.text ?? '',
    });
    raf.scheduleFullRedraw();
    return id;
  }

  changeNote(id: string, args: {color?: string; text?: string}) {
    const note = this._notes.get(id);
    if (note === undefined) return;

    this._notes.set(id, {
      ...note,
      color: args.color ?? note.color,
      text: args.text ?? note.text,
    });
    raf.scheduleFullRedraw();
  }

  removeNote(id: string) {
    raf.scheduleFullRedraw();
    this._notes.delete(id);
    this.onNoteDeleted?.(id);
  }
}
