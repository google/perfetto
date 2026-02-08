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

/**
 * Shared utility functions for bar charts.
 */

import {Filter} from '../../../../components/widgets/datagrid/model';
import {BarDatum, SortConfig} from './bar_types';

/**
 * Extract unique categories from bar data, maintaining order.
 */
export function extractCategories(bars: readonly BarDatum[]): string[] {
  const seen = new Set<string>();
  const categories: string[] = [];
  for (const bar of bars) {
    if (!seen.has(bar.category)) {
      seen.add(bar.category);
      categories.push(bar.category);
    }
  }
  return categories;
}

/**
 * Extract unique groups from bar data, maintaining order.
 */
export function extractGroups(bars: readonly BarDatum[]): string[] {
  const seen = new Set<string>();
  const groups: string[] = [];
  for (const bar of bars) {
    if (bar.group && !seen.has(bar.group)) {
      seen.add(bar.group);
      groups.push(bar.group);
    }
  }
  return groups;
}

/**
 * Sort bars according to configuration.
 * For grouped/stacked charts, sorting is done by summing values per category.
 */
export function sortBars(
  bars: readonly BarDatum[],
  config: SortConfig,
  isGrouped: boolean,
): readonly BarDatum[] {
  if (config.by === 'category') {
    // Sort by category name
    const sorted = [...bars].sort((a, b) => {
      const comparison = a.category.localeCompare(b.category);
      return config.direction === 'asc' ? comparison : -comparison;
    });
    return sorted;
  } else {
    // Sort by value
    if (isGrouped) {
      // For grouped data, compute sum per category
      const categoryTotals = new Map<string, number>();
      for (const bar of bars) {
        const current = categoryTotals.get(bar.category) ?? 0;
        categoryTotals.set(bar.category, current + bar.value);
      }

      // Sort categories by total
      const categories = extractCategories(bars);
      const sortedCategories = categories.sort((a, b) => {
        const aTotal = categoryTotals.get(a) ?? 0;
        const bTotal = categoryTotals.get(b) ?? 0;
        const comparison = aTotal - bTotal;
        return config.direction === 'asc' ? comparison : -comparison;
      });

      // Rebuild bars array in sorted category order
      const sorted: BarDatum[] = [];
      for (const category of sortedCategories) {
        for (const bar of bars) {
          if (bar.category === category) {
            sorted.push(bar);
          }
        }
      }
      return sorted;
    } else {
      // For simple data, sort directly by value
      const sorted = [...bars].sort((a, b) => {
        const comparison = a.value - b.value;
        return config.direction === 'asc' ? comparison : -comparison;
      });
      return sorted;
    }
  }
}

/**
 * Get categories that are currently selected by filters.
 * For bar charts, we use op='in' with an array of categories.
 */
export function getSelectedCategories(
  filters: readonly Filter[],
  column: string,
): Set<string> {
  const selected = new Set<string>();
  for (const filter of filters) {
    if (filter.field === column && 'op' in filter && filter.op === 'in') {
      // TypeScript narrowing: filter is OpFilter with 'in' operator
      if (Array.isArray(filter.value)) {
        for (const val of filter.value) {
          if (typeof val === 'string') {
            selected.add(val);
          }
        }
      }
    }
  }
  return selected;
}

/**
 * Check if a category is currently selected.
 */
export function isCategorySelected(
  category: string,
  selectedCategories: Set<string>,
): boolean {
  return selectedCategories.has(category);
}

/**
 * Create new filters array with category selection.
 * Replaces any existing filters for the given column.
 * Uses 'in' operator with array of categories.
 */
export function createFiltersWithCategories(
  currentFilters: readonly Filter[],
  column: string,
  categories: readonly string[],
): Filter[] {
  // Remove existing filters for this column
  const otherFilters = currentFilters.filter((f) => f.field !== column);

  // Add new filter with 'in' operator for selected categories
  if (categories.length === 0) {
    return otherFilters;
  }

  const newFilter: Filter = {
    field: column,
    op: 'in' as const,
    value: [...categories],
  };

  return [...otherFilters, newFilter];
}

/**
 * Create new filters array with single category toggled.
 * If category is selected, removes it; if not selected, adds it.
 * Uses 'in' operator with array of categories.
 */
export function toggleCategoryFilter(
  currentFilters: readonly Filter[],
  column: string,
  category: string,
): Filter[] {
  const selectedCategories = getSelectedCategories(currentFilters, column);
  const otherFilters = currentFilters.filter((f) => f.field !== column);

  if (selectedCategories.has(category)) {
    // Remove this category from selection
    selectedCategories.delete(category);
  } else {
    // Add this category to selection
    selectedCategories.add(category);
  }

  // If no categories selected, return filters without this column
  if (selectedCategories.size === 0) {
    return otherFilters;
  }

  // Create new filter with updated categories
  const newFilter: Filter = {
    field: column,
    op: 'in' as const,
    value: Array.from(selectedCategories),
  };

  return [...otherFilters, newFilter];
}

/**
 * Clear all filters for a given column.
 */
export function clearColumnFilters(
  currentFilters: readonly Filter[],
  column: string,
): Filter[] {
  return currentFilters.filter((f) => f.field !== column);
}

/**
 * Compute stacked bar layout.
 * Returns map from category -> group -> {y0, y1} for positioning.
 */
export function computeStackedLayout(
  bars: readonly BarDatum[],
  categories: readonly string[],
  groups: readonly string[],
): Map<string, Map<string, {y0: number; y1: number}>> {
  const layout = new Map<string, Map<string, {y0: number; y1: number}>>();

  for (const category of categories) {
    const categoryLayout = new Map<string, {y0: number; y1: number}>();
    let y0 = 0;

    for (const group of groups) {
      // Find bar for this category/group combination
      const bar = bars.find(
        (b) => b.category === category && b.group === group,
      );
      const value = bar?.value ?? 0;

      categoryLayout.set(group, {y0, y1: y0 + value});
      y0 += value;
    }

    layout.set(category, categoryLayout);
  }

  return layout;
}

/**
 * Compute maximum stacked value across all categories.
 */
export function getMaxStackedValue(
  bars: readonly BarDatum[],
  categories: readonly string[],
): number {
  let maxValue = 0;

  for (const category of categories) {
    const categoryBars = bars.filter((b) => b.category === category);
    const sum = categoryBars.reduce((acc, b) => acc + b.value, 0);
    maxValue = Math.max(maxValue, sum);
  }

  return maxValue;
}
