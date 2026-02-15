// Copyright (C) 2026 The Android Open Source Project
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

import {Filter, FilterGroup, FilterNotification} from './types';

/**
 * Manages cross-chart filtering with history support.
 *
 * Filters are grouped so complex selections (e.g., x >= 10 AND x <= 20)
 * can be added/removed atomically. History tracks group-level changes.
 *
 * Charts are notified of filter changes along with the source chart ID,
 * allowing them to decide whether to update their own data.
 */
export class FilterStore {
  /** Filter groups indexed by group ID. */
  private groups = new Map<string, FilterGroup>();

  /** Callbacks notified when filters change. */
  private listeners = new Set<(notification: FilterNotification) => void>();

  /** History stack for undo functionality. */
  private history: Map<string, FilterGroup>[] = [];

  /**
   * Whether the source chart that created a filter should update itself.
   * When true, filtering a chart updates that chart's own data.
   * When false, filtering affects only other charts.
   */
  private updateSourceChart = true;

  /** Callbacks notified when settings (like updateSourceChart) change. */
  private settingsListeners = new Set<() => void>();

  /** Adds or updates a filter group and notifies listeners. */
  setFilterGroup(group: FilterGroup, sourceChartId: string) {
    this.history.push(new Map(this.groups));
    this.groups.set(group.id, group);
    this.notify(sourceChartId);
  }

  /** Removes a filter group and notifies listeners. */
  clearFilterGroup(id: string, sourceChartId: string) {
    this.history.push(new Map(this.groups));
    this.groups.delete(id);
    this.notify(sourceChartId);
  }

  /** Clears all filters and notifies listeners. */
  clearAll() {
    this.history.push(new Map(this.groups));
    this.groups.clear();
    this.notify('system');
  }

  /** Returns all active filters from all groups. */
  getFilters(): Filter[] {
    const result: Filter[] = [];
    for (const group of this.groups.values()) {
      result.push(...group.filters);
    }
    return result;
  }

  /** Returns all filter groups. */
  getFilterGroups(): FilterGroup[] {
    return Array.from(this.groups.values());
  }

  /** Returns a specific filter group by ID. */
  getFilterGroup(id: string): FilterGroup | undefined {
    return this.groups.get(id);
  }

  /**
   * Subscribes to filter changes.
   * Immediately invokes callback with current state.
   * Returns unsubscribe function.
   */
  subscribe(callback: (notification: FilterNotification) => void): () => void {
    this.listeners.add(callback);
    // Fire immediately with current state
    callback({
      filters: this.getFilters(),
      sourceChartId: 'system',
    });
    return () => this.listeners.delete(callback);
  }

  /**
   * Subscribes to settings changes (e.g., updateSourceChart toggle).
   * Returns unsubscribe function.
   */
  subscribeToSettings(callback: () => void): () => void {
    this.settingsListeners.add(callback);
    return () => this.settingsListeners.delete(callback);
  }

  /** Returns whether the source chart updates itself when applying filters. */
  getUpdateSourceChart(): boolean {
    return this.updateSourceChart;
  }

  /** Sets whether the source chart updates itself when applying filters. */
  setUpdateSourceChart(value: boolean) {
    if (this.updateSourceChart !== value) {
      this.updateSourceChart = value;
      this.notifySettings();
    }
  }

  /** Notifies all filter listeners with current filters and source chart ID. */
  private notify(sourceChartId: string) {
    const notification: FilterNotification = {
      filters: this.getFilters(),
      sourceChartId,
    };
    this.listeners.forEach((cb) => cb(notification));
  }

  /** Notifies all settings listeners that a setting has changed. */
  private notifySettings() {
    this.settingsListeners.forEach((cb) => cb());
  }
}
