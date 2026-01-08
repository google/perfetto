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
import {Engine} from '../../../trace_processor/engine';
import {Section} from '../../../widgets/section';
import {GridLayout} from '../../../widgets/grid_layout';
import {
  StatsSectionRow,
  loadStatsWithFilter,
  groupByCategory,
  renderErrorCategoryCard,
  renderCategorySection,
} from '../utils';

export interface TraceErrorsData {
  errors: StatsSectionRow[];
}

export async function loadTraceErrorsData(
  engine: Engine,
): Promise<TraceErrorsData> {
  const errors = await loadStatsWithFilter(
    engine,
    "severity = 'error' AND source = 'trace' AND value > 0",
  );
  return {errors};
}

export interface TraceErrorsTabAttrs {
  data: TraceErrorsData;
}

export class TraceErrorsTab implements m.ClassComponent<TraceErrorsTabAttrs> {
  view({attrs}: m.CVnode<TraceErrorsTabAttrs>) {
    const categories = groupByCategory(attrs.data.errors);

    return m(
      '.pf-trace-info-page__tab-content',
      // Category cards at the top
      m(
        Section,
        {
          title: 'Trace Error Categories',
          subtitle:
            'Summary of trace errors grouped by category. These errors occurred during trace recording',
        },
        categories.length === 0
          ? m('')
          : m(
              GridLayout,
              {},
              categories.map((cat) =>
                renderErrorCategoryCard(cat, 'danger', 'error'),
              ),
            ),
      ),
      // Detailed breakdown by category
      categories.length > 0 &&
        m(
          Section,
          {
            title: 'Detailed Breakdown',
            subtitle: 'Individual trace error entries grouped by category',
          },
          categories.map((cat) =>
            renderCategorySection(cat, {
              className: 'pf-trace-info-page__logs-grid',
            }),
          ),
        ),
    );
  }
}
