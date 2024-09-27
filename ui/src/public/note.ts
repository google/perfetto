// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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

export interface NoteManager {
  getNote(id: string): Note | SpanNote | undefined;

  // Adds a note (a flag on the timeline marker). Returns the id.
  addNote(args: AddNoteArgs): string;

  // Adds a span note (a flagged range). Returns the id.
  addSpanNote(args: AddSpanNoteArgs): string;
}

export interface AddNoteArgs {
  readonly timestamp: time;
  readonly color?: string; // Default: randomColor().
  readonly text?: string; // Default: ''.
  // The id is optional. If present, allows overriding a previosly created note.
  // If not present it will be auto-assigned with a montonic counter.
  readonly id?: string;
}

export interface Note extends AddNoteArgs {
  readonly noteType: 'DEFAULT';
  readonly id: string;
  readonly color: string;
  readonly text: string;
}

export interface AddSpanNoteArgs {
  readonly start: time;
  readonly end: time;
  readonly color?: string; // Default: randomColor().
  readonly text?: string; // Default: ''.
  // The id is optional. If present, allows overriding a previosly created note.
  // If not present it will be auto-assigned with a montonic counter.
  readonly id?: string;
}

export interface SpanNote extends AddSpanNoteArgs {
  readonly noteType: 'SPAN';
  readonly id: string;
  readonly color: string;
  readonly text: string;
}
