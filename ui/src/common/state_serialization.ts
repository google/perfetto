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

import {globals} from '../frontend/globals';
import {HighPrecisionTimeSpan} from './high_precision_time';
import {
  SERIALIZED_STATE_VERSION,
  APP_STATE_SCHEMA,
  SerializedNote,
  SerializedPluginState,
  SerializedSelection,
  SerializedAppState,
} from './state_serialization_schema';
import {ProfileType} from './state';

// When it comes to serialization & permalinks there are two different use cases
// 1. Uploading the current trace in a Cloud Storage (GCS) file AND serializing
//    the app state into a different GCS JSON file. This is what happens when
//    clicking on "share trace" on a local file manually opened.
// 2. [future use case] Uploading the current state in a GCS JSON file, but
//    letting the trace file come from a deep-link via postMessage().
//    This is the case when traces are opened via Dashboards (e.g. APC) and we
//    want to persist only the state itself, not the trace file.
//
// In order to do so, we have two layers of serialization
// 1. Serialization of the app state (This file):
//    This is a JSON object that represents the visual app state (pinned tracks,
//    visible viewport bounds, etc) BUT not the trace source.
// 2. An outer layer that contains the app state AND a link to the trace file.
//    (permalink.ts)
//
// In a nutshell:
//   AppState:  {viewport: {...}, pinnedTracks: {...}, notes: {...}}
//   Permalink: {appState: {see above}, traceUrl: 'https://gcs/trace/file'}
//
// This file deals with the app state. permalink.ts deals with the outer layer.

/**
 * Serializes the current app state into a JSON-friendly POJO that can be stored
 * in a permalink (@see permalink.ts).
 * @returns A @type {SerializedAppState} object, @see state_serialization_schema.ts
 */
export function serializeAppState(): SerializedAppState {
  const vizState = globals.state.frontendLocalState.visibleState;

  const notes = new Array<SerializedNote>();
  for (const [id, note] of Object.entries(globals.state.notes)) {
    if (note.noteType === 'DEFAULT') {
      notes.push({
        noteType: 'DEFAULT',
        id,
        start: note.timestamp,
        color: note.color,
        text: note.text,
      });
    } else if (note.noteType === 'SPAN') {
      notes.push({
        noteType: 'SPAN',
        id,
        start: note.start,
        end: note.end,
        color: note.color,
        text: note.text,
      });
    }
  }

  const selection = new Array<SerializedSelection>();
  const stateSel = globals.state.selection;
  if (stateSel.kind === 'single') {
    selection.push({
      kind: 'TRACK_EVENT',
      trackKey: stateSel.trackKey,
      eventId: stateSel.eventId.toString(),
    });
  } else if (stateSel.kind === 'legacy') {
    // TODO(primiano): get rid of these once we unify selection.
    switch (stateSel.legacySelection.kind) {
      case 'SLICE':
        selection.push({
          kind: 'LEGACY_SLICE',
          id: stateSel.legacySelection.id,
        });
        break;
      case 'SCHED_SLICE':
        selection.push({
          kind: 'LEGACY_SCHED_SLICE',
          id: stateSel.legacySelection.id,
        });
        break;
      case 'THREAD_STATE':
        selection.push({
          kind: 'LEGACY_THREAD_STATE',
          id: stateSel.legacySelection.id,
        });
        break;
      case 'HEAP_PROFILE':
        selection.push({
          kind: 'LEGACY_HEAP_PROFILE',
          id: stateSel.legacySelection.id,
          upid: stateSel.legacySelection.upid,
          ts: stateSel.legacySelection.ts,
          type: stateSel.legacySelection.type,
        });
    }
  }

  const plugins = new Array<SerializedPluginState>();
  for (const [id, pluginState] of Object.entries(globals.state.plugins)) {
    plugins.push({id, state: pluginState});
  }

  return {
    version: SERIALIZED_STATE_VERSION,
    pinnedTracks: globals.state.pinnedTracks,
    viewport: {
      start: vizState.start,
      end: vizState.end,
    },
    notes,
    selection,
    plugins,
  };
}

export type ParseStateResult =
  | {success: true; data: SerializedAppState}
  | {success: false; error: string};

/**
 * Parses the app state from a JSON blob.
 * @param jsonDecodedObj the output of JSON.parse() that needs validation
 * @returns Either a @type {SerializedAppState} object or an error.
 */
export function parseAppState(jsonDecodedObj: unknown): ParseStateResult {
  const parseRes = APP_STATE_SCHEMA.safeParse(jsonDecodedObj);
  if (parseRes.success) {
    if (parseRes.data.version == SERIALIZED_STATE_VERSION) {
      return {success: true, data: parseRes.data};
    } else {
      return {
        success: false,
        error:
          `SERIALIZED_STATE_VERSION mismatch ` +
          `(actual: ${parseRes.data.version}, ` +
          `expected: ${SERIALIZED_STATE_VERSION})`,
      };
    }
  }
  return {success: false, error: parseRes.error.toString()};
}

/**
 * This function gets invoked after the trace is loaded, but before plugins,
 * track decider and initial selections are run.
 * @param appState the .data object returned by parseAppState() when successful.
 */
export function deserializeAppStatePhase1(appState: SerializedAppState): void {
  // Restore the plugin state.
  globals.store.edit((draft) => {
    for (const p of appState.plugins ?? []) {
      draft.plugins[p.id] = p.state ?? {};
    }
  });
}

/**
 * This function gets invoked after the trace controller has run and all plugins
 * have executed.
 * @param appState the .data object returned by parseAppState() when successful.
 */
export function deserializeAppStatePhase2(appState: SerializedAppState): void {
  if (appState.viewport !== undefined) {
    globals.timeline.updateVisibleTime(
      new HighPrecisionTimeSpan(appState.viewport.start, appState.viewport.end),
    );
  }
  globals.store.edit((draft) => {
    // Restore the pinned tracks, if they exist.
    const tracksToPin: string[] = [];
    for (const trackKey of appState.pinnedTracks) {
      if (trackKey in globals.state.tracks) {
        tracksToPin.push(trackKey);
      }
    }
    draft.pinnedTracks = tracksToPin;

    // Restore notes.
    for (const note of appState.notes) {
      const commonArgs = {
        id: note.id,
        timestamp: note.start,
        color: note.color,
        text: note.text,
      };
      if (note.noteType === 'DEFAULT') {
        draft.notes[note.id] = {
          noteType: 'DEFAULT',
          ...commonArgs,
        };
      } else if (note.noteType === 'SPAN') {
        draft.notes[note.id] = {
          noteType: 'SPAN',
          start: commonArgs.timestamp,
          end: note.end,
          ...commonArgs,
        };
      }
    }

    // Restore the selection
    const sel = appState.selection[0];
    if (sel !== undefined) {
      switch (sel.kind) {
        case 'TRACK_EVENT':
          draft.selection = {
            kind: 'single',
            trackKey: sel.trackKey,
            eventId: parseInt(sel.eventId),
          };
          break;
        case 'LEGACY_SCHED_SLICE':
          draft.selection = {
            kind: 'legacy',
            legacySelection: {kind: 'SCHED_SLICE', id: sel.id},
          };
          break;
        case 'LEGACY_SLICE':
          draft.selection = {
            kind: 'legacy',
            legacySelection: {kind: 'SLICE', id: sel.id},
          };
          break;
        case 'LEGACY_THREAD_STATE':
          draft.selection = {
            kind: 'legacy',
            legacySelection: {kind: 'THREAD_STATE', id: sel.id},
          };
          break;
        case 'LEGACY_HEAP_PROFILE':
          draft.selection = {
            kind: 'legacy',
            legacySelection: {
              kind: 'HEAP_PROFILE',
              id: sel.id,
              upid: sel.upid,
              ts: sel.ts,
              type: sel.type as ProfileType,
            },
          };
          break;
      }
    }
  });
}

/**
 * Performs JSON serialization, taking care of also serializing BigInt->string.
 * For the matching deserializer see zType in state_serialization_schema.ts.
 * @param obj A POJO, typically a SerializedAppState or PermalinkState.
 * @returns JSON-encoded string.
 */
export function JsonSerialize(obj: Object): string {
  return JSON.stringify(obj, (_key, value) => {
    if (typeof value === 'bigint') {
      return value.toString();
    }
    return value;
  });
}
