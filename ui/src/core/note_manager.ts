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

import {time} from '../base/time';
import {Note, SpanNote} from '../public/note';
import {randomColor} from './colorizer';
import {raf} from './raf_scheduler';
import {SelectionManagerImpl} from './selection_manager';

export class NoteManagerImpl {
  private _lastNodeId = 0;
  private _notes = new Map<string, Note | SpanNote>();
  private _selectionManager: SelectionManagerImpl;

  constructor(selectionManager: SelectionManagerImpl) {
    this._selectionManager = selectionManager;
  }

  get notes(): ReadonlyMap<string, Note | SpanNote> {
    return this._notes;
  }

  getNote(id: string): Note | SpanNote | undefined {
    return this._notes.get(id);
  }

  addNote(args: {
    timestamp: time;
    color: string;
    id?: string;
    text?: string;
  }): string {
    const {
      timestamp,
      color,
      id = `note_${++this._lastNodeId}`,
      text = '',
    } = args;
    this._notes.set(id, {
      noteType: 'DEFAULT',
      id,
      timestamp,
      color,
      text,
    });
    raf.scheduleFullRedraw();
    return id;
  }

  addSpanNote(args: {
    start: time;
    end: time;
    id?: string;
    color?: string;
  }): string {
    const {
      id = `note_${++this._lastNodeId}`,
      color = randomColor(),
      end,
      start,
    } = args;

    this._notes.set(id, {
      noteType: 'SPAN',
      start,
      end,
      color,
      id,
      text: '',
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
    this._notes.delete(id);
    if (
      this._selectionManager.selection.kind === 'note' &&
      this._selectionManager.selection.id === id
    ) {
      this._selectionManager.clear();
    }
    raf.scheduleFullRedraw();
  }
}
