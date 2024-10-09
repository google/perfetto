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
  SERIALIZED_STATE_VERSION,
  APP_STATE_SCHEMA,
  SerializedNote,
  SerializedPluginState,
  SerializedSelection,
  SerializedAppState,
} from '../public/state_serialization_schema';
import {TimeSpan} from '../base/time';
import {TraceImpl} from './trace_impl';

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
export function serializeAppState(trace: TraceImpl): SerializedAppState {
  const vizWindow = trace.timeline.visibleWindow.toTimeSpan();

  const notes = new Array<SerializedNote>();
  for (const [id, note] of trace.notes.notes.entries()) {
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
  const stateSel = trace.selection.selection;
  if (stateSel.kind === 'track_event') {
    selection.push({
      kind: 'TRACK_EVENT',
      trackKey: stateSel.trackUri,
      eventId: stateSel.eventId.toString(),
    });
  }

  const plugins = new Array<SerializedPluginState>();
  const pluginsStore = trace.getPluginStoreForSerialization();

  for (const [id, pluginState] of Object.entries(pluginsStore)) {
    plugins.push({id, state: pluginState});
  }

  return {
    version: SERIALIZED_STATE_VERSION,
    pinnedTracks: trace.workspace.pinnedTracks
      .map((t) => t.uri)
      .filter((uri) => uri !== undefined),
    viewport: {
      start: vizWindow.start,
      end: vizWindow.end,
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
export function deserializeAppStatePhase1(
  appState: SerializedAppState,
  trace: TraceImpl,
): void {
  // Restore the plugin state.
  trace.getPluginStoreForSerialization().edit((draft) => {
    for (const p of appState.plugins ?? []) {
      draft[p.id] = p.state ?? {};
    }
  });
}

/**
 * This function gets invoked after the trace controller has run and all plugins
 * have executed.
 * @param appState the .data object returned by parseAppState() when successful.
 * @param trace the target trace object to manipulate.
 */
export function deserializeAppStatePhase2(
  appState: SerializedAppState,
  trace: TraceImpl,
): void {
  if (appState.viewport !== undefined) {
    trace.timeline.updateVisibleTime(
      new TimeSpan(appState.viewport.start, appState.viewport.end),
    );
  }

  // Restore the pinned tracks, if they exist.
  for (const uri of appState.pinnedTracks) {
    const track = trace.workspace.findTrackByUri(uri);
    if (track) {
      track.pin();
    }
  }

  // Restore notes.
  for (const note of appState.notes) {
    const commonArgs = {
      id: note.id,
      timestamp: note.start,
      color: note.color,
      text: note.text,
    };
    if (note.noteType === 'DEFAULT') {
      trace.notes.addNote({...commonArgs});
    } else if (note.noteType === 'SPAN') {
      trace.notes.addSpanNote({
        ...commonArgs,
        start: commonArgs.timestamp,
        end: note.end,
      });
    }
  }

  // Restore the selection
  const sel = appState.selection[0];
  if (sel !== undefined) {
    const selMgr = trace.selection;
    switch (sel.kind) {
      case 'TRACK_EVENT':
        selMgr.selectTrackEvent(sel.trackKey, parseInt(sel.eventId));
        break;
      case 'LEGACY_SCHED_SLICE':
        selMgr.selectSqlEvent('sched_slice', sel.id);
        break;
      case 'LEGACY_SLICE':
        selMgr.selectSqlEvent('slice', sel.id);
        break;
      case 'LEGACY_THREAD_STATE':
        selMgr.selectSqlEvent('thread_slice', sel.id);
        break;
    }
  }
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
