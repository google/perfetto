// Copyright (C) 2025 The Android Open Source Project
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

import {ExplorePageState} from './explore_page';
import {serializeState, deserializeState} from './json_handler';
import {Trace} from '../../public/trace';
import {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';

const MAX_HISTORY_SIZE = 10;

export class HistoryManager {
  private history: string[] = [];
  private currentIndex: number = -1;
  private isUndoRedoInProgress: boolean = false;
  private lastSerializedState?: string;

  constructor(
    private trace: Trace,
    private sqlModules: SqlModules,
  ) {}

  // Serialize only the meaningful parts (exclude nodeLayouts and selectedNode)
  private serializeForComparison(state: ExplorePageState): string {
    const stateWithoutLayoutAndSelection = {
      ...state,
      nodeLayouts: new Map(), // Exclude layout from comparison
      selectedNode: undefined, // Exclude selected node from comparison
    };
    return serializeState(stateWithoutLayoutAndSelection);
  }

  // Push a new state to history
  pushState(state: ExplorePageState): void {
    // Don't record history changes triggered by undo/redo
    if (this.isUndoRedoInProgress) {
      return;
    }

    const serialized = serializeState(state);
    const serializedForComparison = this.serializeForComparison(state);

    // Skip if the meaningful state (without layout) hasn't changed
    // This filters out layout-only changes while capturing all other changes
    if (
      this.lastSerializedState &&
      serializedForComparison === this.lastSerializedState
    ) {
      return;
    }

    this.lastSerializedState = serializedForComparison;

    // If we're not at the end of history, remove everything after current index
    if (this.currentIndex < this.history.length - 1) {
      this.history = this.history.slice(0, this.currentIndex + 1);
    }

    // Add the new state
    this.history.push(serialized);

    // Keep only the last MAX_HISTORY_SIZE states
    if (this.history.length > MAX_HISTORY_SIZE) {
      this.history.shift();
    } else {
      this.currentIndex++;
    }
  }

  // Undo to previous state
  undo(): ExplorePageState | null {
    if (!this.canUndo()) {
      return null;
    }

    this.currentIndex--;
    this.isUndoRedoInProgress = true;
    const state = deserializeState(
      this.history[this.currentIndex],
      this.trace,
      this.sqlModules,
    );
    this.isUndoRedoInProgress = false;
    return state;
  }

  // Redo to next state
  redo(): ExplorePageState | null {
    if (!this.canRedo()) {
      return null;
    }

    this.currentIndex++;
    this.isUndoRedoInProgress = true;
    const state = deserializeState(
      this.history[this.currentIndex],
      this.trace,
      this.sqlModules,
    );
    this.isUndoRedoInProgress = false;
    return state;
  }

  canUndo(): boolean {
    return this.currentIndex > 0;
  }

  canRedo(): boolean {
    return this.currentIndex < this.history.length - 1;
  }

  clear(): void {
    this.history = [];
    this.currentIndex = -1;
  }
}
