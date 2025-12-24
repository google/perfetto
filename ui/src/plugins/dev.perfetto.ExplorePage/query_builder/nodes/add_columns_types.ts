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

import {QueryNodeState} from '../../query_node';

/**
 * Represents an IF clause for conditional column expressions.
 */
export interface IfClause {
  if: string;
  then: string;
}

/**
 * Represents a case branch for SWITCH column expressions.
 */
export interface SwitchCase {
  when: string;
  then: string;
}

/**
 * Represents a computed column definition (expression, SWITCH, or IF).
 */
export interface NewColumn {
  expression: string;
  name: string;
  module?: string;

  // For switch columns
  type?: 'switch' | 'if';
  switchOn?: string;
  cases?: SwitchCase[];
  defaultValue?: string;
  useGlob?: boolean;

  // For if columns
  clauses?: IfClause[];
  elseValue?: string;

  // SQL type for preserving type information across serialization
  sqlType?: string;
}

/**
 * State interface for the AddColumnsNode.
 */
export interface AddColumnsNodeState extends QueryNodeState {
  selectedColumns?: string[];
  leftColumn?: string;
  rightColumn?: string;

  // Pre-selected columns for each suggested table (before connecting)
  suggestionSelections?: Map<string, string[]>;

  // Track which suggestions are expanded to show column selection
  expandedSuggestions?: Set<string>;

  // Currently selected suggestion table (for single-selection UI)
  selectedSuggestionTable?: string;

  // Map from column name to its alias (for renaming added columns)
  columnAliases?: Map<string, string>;

  // Map from column name to its alias for suggestion mode (before applying)
  suggestionAliases?: Map<string, string>;

  // Map from column name to its type (for type casting added columns)
  columnTypes?: Map<string, string>;

  // Track if connection was made through guided suggestion
  isGuidedConnection?: boolean;

  // Computed columns (expressions, SWITCH, IF)
  computedColumns?: NewColumn[];
}
