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

import m from 'mithril';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Popup, PopupPosition} from '../../widgets/popup';
import type {TraceColumnDescriptor} from '../query/bigtrace_query_client';
import {
  type ColumnValuesLoader,
  columnValuesLoader,
} from '../query/column_values_loader';
import {UPLOAD_DATE_COLUMN} from '../query/date_filter';
import {
  FORM_FACTOR_COLUMN,
  MANUFACTURER_COLUMN,
  MODEL_COLUMN,
  POPULATION_COLUMN,
  facetFilters,
  readValueSet,
  writeValueSet,
} from '../query/value_set_filters';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import {DateFilterChip} from './date_filter_chip';
import {DeviceFilterChip} from './device_filter_modal';
import {ValueList} from './value_list';

// ----- Population chip: a checklist popup, written on Apply -----

export function describePopulation(values: ReadonlyArray<string>): string {
  if (values.length === 0) return 'Population: All';
  if (values.length === 1) return `Population: ${values[0]}`;
  return `Population: ${values.length} selected`;
}

interface PopulationFilterChipAttrs {
  readonly bindings: SettingsBindings;
  readonly load: ColumnValuesLoader;
}

class PopulationFilterChip implements m.ClassComponent<PopulationFilterChipAttrs> {
  private open = false;
  private draft = new Set<string>();

  view({attrs}: m.Vnode<PopulationFilterChipAttrs>): m.Children {
    const {bindings, load} = attrs;
    const filters = bindings.getTraceFilters();
    const selected = readValueSet(filters, POPULATION_COLUMN);
    const close = () => (this.open = false);
    return m(
      Popup,
      {
        isOpen: this.open,
        onChange: (open: boolean) => (this.open = open),
        position: PopupPosition.BottomStart,
        fitContent: true,
        className: 'pf-bt-filter-popup',
        trigger: m(Button, {
          label: describePopulation(selected),
          icon: 'filter_alt',
          variant: ButtonVariant.Outlined,
          intent: Intent.Primary,
          className: 'pf-bt-corpus-filters__chip',
          onclick: () => {
            // Every opening starts over from what is applied.
            this.draft = new Set(selected);
            this.open = !this.open;
          },
        }),
      },
      m('.pf-bt-population-dialog', [
        m(ValueList, {
          column: POPULATION_COLUMN,
          load,
          facet: facetFilters(filters, POPULATION_COLUMN),
          selected: this.draft,
          onToggle: (value, checked) =>
            checked ? this.draft.add(value) : this.draft.delete(value),
        }),
        m('.pf-bt-filter-dialog__footer', [
          m('span', `${this.draft.size} selected`),
          m('span.pf-bt-filter-dialog__spacer'),
          this.draft.size > 0 &&
            m(Button, {
              label: 'Clear',
              compact: true,
              onclick: () => this.draft.clear(),
            }),
          m(Button, {label: 'Cancel', onclick: close}),
          m(Button, {
            label: 'Apply',
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            onclick: () => {
              bindings.setTraceFilters(
                writeValueSet(bindings.getTraceFilters(), POPULATION_COLUMN, [
                  ...this.draft,
                ]),
              );
              close();
            },
          }),
        ]),
      ]),
    );
  }
}

// ----- The bar -----

export interface CorpusFilterBarAttrs {
  readonly bindings: SettingsBindings;
  // The trace-metadata schema, once known; nothing renders before.
  readonly columns: ReadonlyArray<TraceColumnDescriptor> | undefined;
}

// Top-level filters over the trace corpus, under the page title: population
// and device picker on the left, upload date on the right. Each chip exists
// only if the schema has its column(s); the whole bar is skipped when none
// does. What they pick is written as plain grid filters on the tab, so the
// grid below, its match count and the run all see the same selection.
export class CorpusFilterBar implements m.ClassComponent<CorpusFilterBarAttrs> {
  view({attrs}: m.Vnode<CorpusFilterBarAttrs>): m.Children {
    const names = new Set((attrs.columns ?? []).map((c) => c.name));
    const hasPopulation = names.has(POPULATION_COLUMN);
    const hasDevice = names.has(MANUFACTURER_COLUMN) && names.has(MODEL_COLUMN);
    const hasDate = names.has(UPLOAD_DATE_COLUMN);
    if (!hasPopulation && !hasDevice && !hasDate) return null;
    const load = columnValuesLoader(attrs.bindings);
    return m('.pf-bt-corpus-filters', [
      m('.pf-bt-corpus-filters__left', [
        hasPopulation &&
          m(PopulationFilterChip, {bindings: attrs.bindings, load}),
        hasDevice &&
          m(DeviceFilterChip, {
            bindings: attrs.bindings,
            load,
            // Form factor is optional on the device picker.
            formFactor: names.has(FORM_FACTOR_COLUMN),
          }),
      ]),
      m(
        '.pf-bt-corpus-filters__right',
        hasDate && m(DateFilterChip, {bindings: attrs.bindings}),
      ),
    ]);
  }
}
