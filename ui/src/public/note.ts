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

/**
 * Manages notes and span notes on the timeline.
 *
 * Notes are flags on the timeline marker, while span notes represent flagged
 * ranges.
 */
export interface NoteManager {
  /**
   * Retrieves a note or span note by its ID.
   * @param id The unique identifier of the note.
   * @returns The note or span note if found, or `undefined`.
   */
  getNote(id: string): Note | SpanNote | undefined;

  /**
   * Adds a new note (a flag on the timeline marker).
   * @param args The arguments for adding the note.
   * @returns The unique ID of the newly added note.
   */
  addNote(args: AddNoteArgs): string;

  /**
   * Adds a new span note (a flagged range).
   * @param args The arguments for adding the span note.
   * @returns The unique ID of the newly added span note.
   */
  addSpanNote(args: AddSpanNoteArgs): string;
}

/**
 * Arguments for adding a new note.
 */
export interface AddNoteArgs {
  /**
   * The timestamp of the note.
   */
  readonly timestamp: time;
  /**
   * The color of the note. If not provided, a random color will be assigned.
   */
  readonly color?: string;
  /**
   * The text content of the note. If not provided, an empty string will be used.
   */
  readonly text?: string;
  /**
   * The unique ID of the note. If provided, it allows overriding a previously
   * created note. If not present, an ID will be auto-assigned with a monotonic
   * counter.
   */
  readonly id?: string;
}

/**
 * Represents a note (a flag on the timeline marker).
 */
export interface Note extends AddNoteArgs {
  /**
   * The type of the note, always 'DEFAULT' for a regular note.
   */
  readonly noteType: 'DEFAULT';
  /**
   * The unique ID of the note.
   */
  readonly id: string;
  /**
   * The color of the note.
   */
  readonly color: string;
  /**
   * The text content of the note.
   */
  readonly text: string;
}

/**
 * Arguments for adding a new span note.
 */
export interface AddSpanNoteArgs {
  /**
   * The start timestamp of the span note.
   */
  readonly start: time;
  /**
   * The end timestamp of the span note.
   */
  readonly end: time;
  /**
   * The color of the span note. If not provided, a random color will be assigned.
   */
  readonly color?: string;
  /**
   * The text content of the span note. If not provided, an empty string will be used.
   */
  readonly text?: string;
  /**
   * The unique ID of the span note. If provided, it allows overriding a previously
   * created span note. If not present, an ID will be auto-assigned with a monotonic
   * counter.
   */
  readonly id?: string;
}

/**
 * Represents a span note (a flagged range).
 */
export interface SpanNote extends AddSpanNoteArgs {
  /**
   * The type of the note, always 'SPAN' for a span note.
   */
  readonly noteType: 'SPAN';
  /**
   * The unique ID of the span note.
   */
  readonly id: string;
  /**
   * The color of the span note.
   */
  readonly color: string;
  /**
   * The text content of the span note.
   */
  readonly text: string;
}
