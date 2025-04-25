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

import m from 'mithril';
import {AggregationPanel} from './aggregation_panel';
import {
  AreaSelection,
  AreaSelectionAggregator,
  areaSelectionsEqual,
  AreaSelectionTab,
} from '../public/selection';
import {Trace} from '../public/trace';
import {SelectionAggregationManager} from './selection_aggregation_manager';

/**
 * Creates an adapter that adapts an old style aggregation to a new area
 * selection sub-tab.
 */
export function createAggregationToTabAdaptor(
  trace: Trace,
  aggregator: AreaSelectionAggregator,
): AreaSelectionTab {
  const schemaSpecificity =
    (aggregator.schema && Object.keys(aggregator.schema).length) ?? 0;
  const kindRating = aggregator.trackKind === undefined ? 0 : 100;
  const priority = kindRating + schemaSpecificity;
  const aggMan = new SelectionAggregationManager(trace.engine, aggregator);
  let currentSelection: AreaSelection | undefined;

  return {
    id: aggregator.id,
    name: aggregator.getTabName(),
    priority,
    render(selection: AreaSelection) {
      if (
        currentSelection === undefined ||
        !areaSelectionsEqual(selection, currentSelection)
      ) {
        aggMan.aggregateArea(selection);
        currentSelection = selection;
      }

      const data = aggMan.aggregatedData;
      if (!data) {
        return undefined;
      }

      return {
        isLoading: false,
        content: m(AggregationPanel, {
          data,
          trace,
          model: aggMan,
        }),
      };
    },
  };
}
