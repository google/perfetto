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

import {duration, time} from '../base/time';
import {Store} from '../base/store';
import {assertUnreachable} from '../base/logging';
import {GenericSliceDetailsTabConfigBase} from './generic_slice_details_types';

export enum ProfileType {
  HEAP_PROFILE = 'heap_profile',
  MIXED_HEAP_PROFILE = 'heap_profile:com.android.art,libc.malloc',
  NATIVE_HEAP_PROFILE = 'heap_profile:libc.malloc',
  JAVA_HEAP_SAMPLES = 'heap_profile:com.android.art',
  JAVA_HEAP_GRAPH = 'graph',
  PERF_SAMPLE = 'perf',
}

// LEGACY Selection types:
export interface AreaSelection {
  kind: 'AREA';
  areaId: string;
  // When an area is marked it will be assigned a unique note id and saved as
  // an AreaNote for the user to return to later. id = 0 is the special id that
  // is overwritten when a new area is marked. Any other id is a persistent
  // marking that will not be overwritten.
  // When not set, the area selection will be replaced with any
  // new area selection (i.e. not saved anywhere).
  noteId?: string;
}

export interface NoteSelection {
  kind: 'NOTE';
  id: string;
}

export interface SliceSelection {
  kind: 'SLICE';
  id: number;
}

export interface CounterSelection {
  kind: 'COUNTER';
  leftTs: time;
  rightTs: time;
  id: number;
}

export interface HeapProfileSelection {
  kind: 'HEAP_PROFILE';
  id: number;
  upid: number;
  ts: time;
  type: ProfileType;
}

export interface PerfSamplesSelection {
  kind: 'PERF_SAMPLES';
  id: number;
  upid: number;
  leftTs: time;
  rightTs: time;
  type: ProfileType;
}

export interface CpuProfileSampleSelection {
  kind: 'CPU_PROFILE_SAMPLE';
  id: number;
  utid: number;
  ts: time;
}

export interface ChromeSliceSelection {
  kind: 'CHROME_SLICE';
  id: number;
  table?: string;
}

export interface ThreadStateSelection {
  kind: 'THREAD_STATE';
  id: number;
}

export interface LogSelection {
  kind: 'LOG';
  id: number;
  trackKey: string;
}

export interface GenericSliceSelection {
  kind: 'GENERIC_SLICE';
  id: number;
  sqlTableName: string;
  start: time;
  duration: duration;
  // NOTE: this config can be expanded for multiple details panel types.
  detailsPanelConfig: {kind: string; config: GenericSliceDetailsTabConfigBase};
}

export type LegacySelection = (
  | NoteSelection
  | SliceSelection
  | CounterSelection
  | HeapProfileSelection
  | CpuProfileSampleSelection
  | ChromeSliceSelection
  | ThreadStateSelection
  | AreaSelection
  | PerfSamplesSelection
  | LogSelection
  | GenericSliceSelection
) & {trackKey?: string};
export type SelectionKind = LegacySelection['kind']; // 'THREAD_STATE' | 'SLICE' ...

// New Selection types:
export interface LegacySelectionWrapper {
  kind: 'legacy';
  legacySelection: LegacySelection;
}

export interface SingleSelection {
  kind: 'single';
  trackKey: string;
  eventId: string;
}

export interface NewAreaSelection {
  kind: 'area';
  trackKey: string;
  start: time;
  end: time;
}

export interface UnionSelection {
  kind: 'union';
  selections: Selection[];
}

export interface EmptySelection {
  kind: 'empty';
}

export type Selection =
  | SingleSelection
  | NewAreaSelection
  | UnionSelection
  | EmptySelection
  | LegacySelectionWrapper;

export function selectionToLegacySelection(
  selection: Selection,
): LegacySelection | null {
  switch (selection.kind) {
    case 'area':
    case 'single':
    case 'empty':
      return null;
    case 'union':
      for (const child of selection.selections) {
        const result = selectionToLegacySelection(child);
        if (result !== null) {
          return result;
        }
      }
      return null;
    case 'legacy':
      return selection.legacySelection;
    default:
      assertUnreachable(selection);
      return null;
  }
}

interface SelectionState {
  selection: Selection;
}

export class SelectionManager {
  private store: Store<SelectionState>;

  constructor(store: Store<SelectionState>) {
    this.store = store;
  }

  clear(): void {
    this.store.edit((draft) => {
      draft.selection = {
        kind: 'empty',
      };
    });
  }

  private addSelection(selection: Selection): void {
    this.store.edit((draft) => {
      switch (draft.selection.kind) {
        case 'empty':
          draft.selection = selection;
          break;
        case 'union':
          draft.selection.selections.push(selection);
          break;
        case 'single':
        case 'legacy':
        case 'area':
          draft.selection = {
            kind: 'union',
            selections: [draft.selection, selection],
          };
          break;
        default:
          assertUnreachable(draft.selection);
          break;
      }
    });
  }

  // There is no matching addLegacy as we did not support multi-single
  // selection with the legacy selection system.
  setLegacy(legacySelection: LegacySelection): void {
    this.clear();
    this.addSelection({
      kind: 'legacy',
      legacySelection,
    });
  }

  setEvent(
    trackKey: string,
    eventId: string,
    legacySelection?: LegacySelection,
  ) {
    this.clear();
    this.addEvent(trackKey, eventId, legacySelection);
  }

  addEvent(
    trackKey: string,
    eventId: string,
    legacySelection?: LegacySelection,
  ) {
    this.addSelection({
      kind: 'single',
      trackKey,
      eventId,
    });
    if (legacySelection) {
      this.addSelection({
        kind: 'legacy',
        legacySelection,
      });
    }
  }
}
