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

import {
  ChartSpec,
  Row,
  Filter,
  FilterGroup,
  FilterNotification,
} from '../data/types';
import {DataSource} from '../data/source';
import {FilterStore} from '../data/filter_store';

export class Chart {
  private static nextId = 0;
  private readonly chartId: string;
  private data: Row[] = [];
  private unsub: () => void;
  private loading = false;
  private filterGroupStack: string[] = [];
  private pendingFilters: Filter[] = [];
  private batchScheduled = false;
  private currentFilters: Filter[] = [];
  private loadRequestId = 0;

  onDataChange?: (data: Row[], loading: boolean) => void;
  onFilterStateChange?: (hasActiveFilter: boolean) => void;

  constructor(
    public spec: ChartSpec,
    private source: DataSource,
    private filterStore: FilterStore,
  ) {
    this.chartId = `chart-${Chart.nextId++}`;
    this.unsub = filterStore.subscribe((notification: FilterNotification) => {
      this.handleFilterNotification(notification);
    });
  }

  private handleFilterNotification(notification: FilterNotification) {
    const {filters, sourceChartId} = notification;
    const isSourceChart = sourceChartId === this.chartId;
    const updateSourceChart = this.filterStore.getUpdateSourceChart();

    this.currentFilters = filters;

    // Check if any of this chart's filter groups were removed
    const activeGroupIds = new Set(
      this.filterStore.getFilterGroups().map((g) => g.id),
    );
    const wasCleared = this.filterGroupStack.some(
      (id) => !activeGroupIds.has(id),
    );
    if (wasCleared) {
      this.filterGroupStack = this.filterGroupStack.filter((id) =>
        activeGroupIds.has(id),
      );
      this.notifyFilterStateChange();
    }

    // Chart that created the filter can skip its own update if disabled in settings
    if (!isSourceChart || updateSourceChart) {
      this.load(filters);
    }
  }

  private async load(filters: Filter[]) {
    const requestId = ++this.loadRequestId;
    this.loading = true;
    this.notifyChange();

    try {
      const data = await this.source.query(filters, this.spec);
      // Guard against stale requests completing after newer ones
      if (this.loadRequestId === requestId) {
        this.data = data;
        this.loading = false;
        this.notifyChange();
      }
    } catch (error) {
      if (this.loadRequestId === requestId) {
        console.error('Chart load error:', error);
        this.data = [];
        this.loading = false;
        this.notifyChange();
      }
    }
  }

  getData(): Row[] {
    return this.data;
  }

  getSource(): DataSource {
    return this.source;
  }

  getFilterStore(): FilterStore {
    return this.filterStore;
  }

  getCurrentFilters(): Filter[] {
    return this.currentFilters;
  }

  getChartId(): string {
    return this.chartId;
  }

  isLoading(): boolean {
    return this.loading;
  }

  addPendingFilter(col: string, op: Filter['op'], val: Filter['val']) {
    this.pendingFilters.push({col, op, val});

    // Batch multiple brush events together in a single microtask
    if (!this.batchScheduled) {
      this.batchScheduled = true;
      queueMicrotask(() => this.processPendingFilters());
    }
  }

  private processPendingFilters() {
    this.batchScheduled = false;

    if (this.pendingFilters.length === 0) return;

    const group: FilterGroup = {
      id: `${this.chartId}-${Date.now()}`,
      filters: [...this.pendingFilters],
      label: this.createFilterLabel(this.pendingFilters),
    };

    if (this.filterGroupStack.length > 0) {
      this.clearChartFilters();
    }

    this.filterStore.setFilterGroup(group, this.chartId);
    this.filterGroupStack.push(group.id);
    this.pendingFilters = [];

    this.notifyFilterStateChange();
  }

  clearChartFilters() {
    for (const groupId of this.filterGroupStack) {
      this.filterStore.clearFilterGroup(groupId, this.chartId);
    }
    this.filterGroupStack = [];
    this.notifyFilterStateChange();
  }

  hasActiveFilters(): boolean {
    return this.filterGroupStack.length > 0;
  }

  private createFilterLabel(filters: Filter[]): string {
    if (filters.length === 1) {
      return this.formatFilter(filters[0]);
    }

    const byColumn = new Map<string, Filter[]>();
    for (const filter of filters) {
      const existing = byColumn.get(filter.col) ?? [];
      existing.push(filter);
      byColumn.set(filter.col, existing);
    }

    const labels: string[] = [];
    for (const [col, colFilters] of byColumn) {
      const rangeLabel = this.tryFormatRange(col, colFilters);
      if (rangeLabel) {
        labels.push(rangeLabel);
      } else {
        for (const f of colFilters) {
          labels.push(this.formatFilter(f));
        }
      }
    }

    return labels.join(', ');
  }

  private formatFilter(filter: Filter): string {
    return `${filter.col} ${filter.op} ${JSON.stringify(filter.val)}`;
  }

  private tryFormatRange(col: string, filters: Filter[]): string | null {
    if (filters.length !== 2) return null;

    const ge = filters.find((f) => f.op === '>=');
    const le = filters.find((f) => f.op === '<=');

    if (ge && le) {
      return `${col}: ${JSON.stringify(ge.val)} - ${JSON.stringify(le.val)}`;
    }

    return null;
  }

  destroy() {
    this.unsub();
  }

  clone(): Chart {
    return new Chart(
      JSON.parse(JSON.stringify(this.spec)),
      this.source,
      this.filterStore,
    );
  }

  private notifyChange() {
    this.onDataChange?.(this.data, this.loading);
  }

  private notifyFilterStateChange() {
    this.onFilterStateChange?.(this.hasActiveFilters());
  }
}
