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

import {assertTrue, assertUnreachable} from '../base/logging';
import {
  Selection,
  LegacySelection,
  Area,
  ProfileType,
  SelectionOpts,
  SelectionManager,
} from '../public/selection';
import {duration, time} from '../base/time';
import {
  GenericSliceDetailsTabConfig,
  GenericSliceDetailsTabConfigBase,
} from '../public/details_panel';
import {raf} from './raf_scheduler';

export class SelectionManagerImpl implements SelectionManager {
  private _selection: Selection = {kind: 'empty'};
  onSelectionChange?: (selection: Selection, opts: SelectionOpts) => void;

  clear(): void {
    this.setSelection({kind: 'empty'});
  }

  setEvent(trackUri: string, eventId: number) {
    this.setSelection({
      kind: 'single',
      trackUri,
      eventId,
    });
  }

  setNote(args: {id: string}) {
    this.setSelection({
      kind: 'note',
      id: args.id,
    });
  }

  setArea(args: Area): void {
    const {start, end} = args;
    assertTrue(start <= end);
    this.setSelection({
      kind: 'area',
      ...args,
    });
  }

  toggleTrackAreaSelection(trackUri: string) {
    const curSelection = this._selection;
    if (curSelection.kind !== 'area') return;

    let trackUris = curSelection.trackUris.slice();
    if (!trackUris.includes(trackUri)) {
      trackUris.push(trackUri);
    } else {
      trackUris = trackUris.filter((t) => t !== trackUri);
    }
    this.setSelection({
      ...curSelection,
      trackUris,
    });
  }

  toggleGroupAreaSelection(trackUris: string[]) {
    const curSelection = this._selection;
    if (curSelection.kind !== 'area') return;

    const allTracksSelected = trackUris.every((t) =>
      curSelection.trackUris.includes(t),
    );

    let newTrackUris: string[];
    if (allTracksSelected) {
      // Deselect all tracks in the list
      newTrackUris = curSelection.trackUris.filter(
        (t) => !trackUris.includes(t),
      );
    } else {
      newTrackUris = curSelection.trackUris.slice();
      trackUris.forEach((t) => {
        if (!newTrackUris.includes(t)) {
          newTrackUris.push(t);
        }
      });
    }
    this.setSelection({
      ...curSelection,
      trackUris: newTrackUris,
    });
  }

  // There is no matching addLegacy as we did not support multi-single
  // selection with the legacy selection system.
  setLegacy(legacySelection: LegacySelection, opts?: SelectionOpts): void {
    this.setSelection(
      {
        kind: 'legacy',
        legacySelection,
      },
      opts,
    );
  }

  setHeapProfile(args: {
    id: number;
    upid: number;
    ts: time;
    type: ProfileType;
  }): void {
    this.setSelection({
      kind: 'legacy',
      legacySelection: {
        kind: 'HEAP_PROFILE',
        id: args.id,
        upid: args.upid,
        ts: args.ts,
        type: args.type,
      },
    });
  }

  setPerfSamples(args: {
    id: number;
    utid?: number;
    upid?: number;
    leftTs: time;
    rightTs: time;
    type: ProfileType;
  }) {
    this.setSelection({
      kind: 'legacy',
      legacySelection: {
        kind: 'PERF_SAMPLES',
        id: args.id,
        utid: args.utid,
        upid: args.upid,
        leftTs: args.leftTs,
        rightTs: args.rightTs,
        type: args.type,
      },
    });
  }

  setCpuProfileSample(args: {id: number; utid: number; ts: time}): void {
    this.setSelection({
      kind: 'legacy',
      legacySelection: {
        kind: 'CPU_PROFILE_SAMPLE',
        id: args.id,
        utid: args.utid,
        ts: args.ts,
      },
    });
  }

  setSchedSlice(args: {id: number; trackUri?: string}): void {
    this.setSelection({
      kind: 'legacy',
      legacySelection: {
        kind: 'SCHED_SLICE',
        id: args.id,
        trackUri: args.trackUri,
      },
    });
  }

  setLegacySlice(
    args: {
      id: number;
      trackUri?: string;
      table?: string;
      scroll?: boolean;
    },
    opts?: SelectionOpts,
  ): void {
    this.setSelection(
      {
        kind: 'legacy',
        legacySelection: {
          kind: 'SLICE',
          id: args.id,
          table: args.table,
          trackUri: args.trackUri,
        },
      },
      opts,
    );
  }

  setGenericSlice(args: {
    id: number;
    sqlTableName: string;
    start: time;
    duration: duration;
    trackUri: string;
    detailsPanelConfig: {
      kind: string;
      config: GenericSliceDetailsTabConfigBase;
    };
  }): void {
    const detailsPanelConfig: GenericSliceDetailsTabConfig = {
      id: args.id,
      ...args.detailsPanelConfig.config,
    };
    this.setSelection({
      kind: 'legacy',
      legacySelection: {
        kind: 'GENERIC_SLICE',
        id: args.id,
        sqlTableName: args.sqlTableName,
        start: args.start,
        duration: args.duration,
        trackUri: args.trackUri,
        detailsPanelConfig: {
          kind: args.detailsPanelConfig.kind,
          config: detailsPanelConfig,
        },
      },
    });
  }

  setThreadState(args: {id: number; trackUri?: string}): void {
    this.setSelection({
      kind: 'legacy',
      legacySelection: {
        kind: 'THREAD_STATE',
        id: args.id,
        trackUri: args.trackUri,
      },
    });
  }

  get selection(): Selection {
    return this._selection;
  }

  get legacySelection(): LegacySelection | null {
    return toLegacySelection(this._selection);
  }

  private setSelection(selection: Selection, opts?: SelectionOpts) {
    this._selection = selection;
    if (this.onSelectionChange !== undefined) {
      this.onSelectionChange(selection, opts ?? {});
    }
    raf.scheduleFullRedraw();
  }
}

function toLegacySelection(selection: Selection): LegacySelection | null {
  switch (selection.kind) {
    case 'area':
    case 'single':
    case 'empty':
    case 'note':
      return null;
    case 'union':
      for (const child of selection.selections) {
        const result = toLegacySelection(child);
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
