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
import {AsyncMemo} from '../../base/async_memo';
import type {Filter} from '../../components/widgets/datagrid/model';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {closeModal, showModal} from '../../widgets/modal';
import {
  type MultiSelectDiff,
  PopupMultiSelect,
} from '../../widgets/multiselect';
import {TextInput} from '../../widgets/text_input';
import {
  type ColumnValuesLoader,
  describeLoadError,
} from '../query/column_values_loader';
import {encodeFilters} from '../query/filter_encoding';
import {
  FORM_FACTOR_COLUMN,
  MANUFACTURER_COLUMN,
  MODEL_COLUMN,
  facetFilters,
  readValueSet,
  writeValueSet,
} from '../query/value_set_filters';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import {ValueList} from './value_list';

const MODAL_KEY = 'bt-device-filter';

function count(n: number, noun: string): string {
  return `${n} ${noun}${n === 1 ? '' : 's'}`;
}

export function describeDevice(
  formFactors: ReadonlyArray<string>,
  manufacturers: ReadonlyArray<string>,
  models: ReadonlyArray<string>,
): string {
  const parts: string[] = [];
  const part = (values: ReadonlyArray<string>, noun: string) => {
    if (values.length === 1) parts.push(values[0]);
    else if (values.length > 1) parts.push(count(values.length, noun));
  };
  part(formFactors, 'form factor');
  part(manufacturers, 'manufacturer');
  part(models, 'model');
  return `Device: ${parts.length === 0 ? 'All' : parts.join(' · ')}`;
}

// The form-factor dropdown's button text: names while they fit on a
// button, a count past that (the full list is offered as a tooltip).
export function describeFormFactors(picked: ReadonlyArray<string>): string {
  if (picked.length === 0) return 'Any';
  if (picked.length <= 2) return picked.join(', ');
  return `${picked.length} selected`;
}

// Edited in the dialog; only written to the tab on Apply.
export interface DeviceDraft {
  readonly formFactors: Set<string>;
  readonly manufacturers: Set<string>;
  readonly models: Set<string>;
}

export interface DeviceFilterDialogAttrs {
  readonly draft: DeviceDraft;
  readonly load: ColumnValuesLoader;
  // The tab's other filters (see facetFilters), narrowing every list.
  readonly facet: ReadonlyArray<Filter>;
  // Whether the schema has a form-factor column to offer.
  readonly formFactor: boolean;
  readonly onApply: () => void;
  readonly onCancel: () => void;
}

function withValues(
  facet: ReadonlyArray<Filter>,
  field: string,
  values: ReadonlySet<string>,
): Filter[] {
  return values.size === 0
    ? [...facet]
    : [...facet, {field, op: 'in', value: [...values]}];
}

function toggle(set: Set<string>, value: string, on: boolean): void {
  if (on) set.add(value);
  else set.delete(value);
}

// Two searchable panes — manufacturers, then models within them — under an
// optional form-factor dropdown. Each list is narrowed by what is
// ticked to its left, so the picker stays usable with thousands of values.
// The model list only appears once a manufacturer is ticked: across all
// manufacturers it is hundreds of meaningless rows.
export class DeviceFilterDialog implements m.ClassComponent<DeviceFilterDialogAttrs> {
  private readonly formFactors = new AsyncMemo<ReadonlyArray<string>>();
  // Dialog-lifetime signal for the form-factor load: dispose() only drops
  // queued computes, it does not cancel a request (or its retry backoff)
  // already in flight.
  private readonly lifetime = new AbortController();
  private manufacturerSearch = '';
  private modelSearch = '';

  view({attrs}: m.Vnode<DeviceFilterDialogAttrs>): m.Children {
    const {draft, facet, load} = attrs;
    const manufacturerFacet = withValues(
      facet,
      FORM_FACTOR_COLUMN,
      draft.formFactors,
    );
    const modelFacet = withValues(
      manufacturerFacet,
      MANUFACTURER_COLUMN,
      draft.manufacturers,
    );
    const picked =
      draft.formFactors.size + draft.manufacturers.size + draft.models.size;
    return m('.pf-bt-device-dialog', [
      attrs.formFactor && this.renderFormFactors(attrs),
      m('.pf-bt-device-dialog__panes', [
        m('.pf-bt-device-dialog__pane.pf-bt-device-dialog__pane--left', [
          m('h4', [
            'Manufacturer',
            m(
              'span',
              draft.manufacturers.size === 0
                ? 'any'
                : `${draft.manufacturers.size} selected`,
            ),
          ]),
          m(TextInput, {
            'leftIcon': 'search',
            'placeholder': 'Search manufacturers…',
            'aria-label': 'Search manufacturers',
            'value': this.manufacturerSearch,
            'onInput': (v: string) => (this.manufacturerSearch = v),
          }),
          m(ValueList, {
            column: MANUFACTURER_COLUMN,
            load,
            facet: manufacturerFacet,
            search: this.manufacturerSearch,
            selected: draft.manufacturers,
            pinSelected: true,
            onToggle: (v, on) => toggle(draft.manufacturers, v, on),
          }),
        ]),
        // Stays while models are picked (e.g. restored from a run that had no
        // manufacturer), so they can always be unticked.
        draft.manufacturers.size > 0 || draft.models.size > 0
          ? m('.pf-bt-device-dialog__pane', [
              m('h4', [
                'Model',
                m(
                  'span',
                  draft.manufacturers.size === 0
                    ? 'any manufacturer'
                    : 'within selected manufacturers',
                ),
              ]),
              m(TextInput, {
                'leftIcon': 'search',
                'placeholder': 'Search models…',
                'aria-label': 'Search models',
                'value': this.modelSearch,
                'onInput': (v: string) => (this.modelSearch = v),
              }),
              m(ValueList, {
                column: MODEL_COLUMN,
                load,
                facet: modelFacet,
                search: this.modelSearch,
                selected: draft.models,
                pinSelected: true,
                onToggle: (v, on) => toggle(draft.models, v, on),
              }),
            ])
          : m(
              '.pf-bt-device-dialog__pane.pf-bt-device-dialog__pane--hint',
              'Tick a manufacturer to pick models.',
            ),
      ]),
      m('.pf-bt-filter-dialog__footer', [
        m(
          'span',
          [
            attrs.formFactor && count(draft.formFactors.size, 'form factor'),
            count(draft.manufacturers.size, 'manufacturer'),
            `${count(draft.models.size, 'model')} selected`,
          ]
            .filter((s) => s !== false)
            .join(' · '),
        ),
        picked > 0 &&
          m(Button, {
            label: 'Clear all',
            compact: true,
            onclick: () => {
              draft.formFactors.clear();
              draft.manufacturers.clear();
              draft.models.clear();
            },
          }),
        m('.pf-bt-filter-dialog__spacer'),
        m(Button, {label: 'Cancel', onclick: attrs.onCancel}),
        m(Button, {
          label: 'Apply',
          intent: Intent.Primary,
          variant: ButtonVariant.Filled,
          onclick: attrs.onApply,
        }),
      ]),
    ]);
  }

  private renderFormFactors(attrs: DeviceFilterDialogAttrs): m.Children {
    const {draft, facet, load} = attrs;
    // AsyncMemo rethrows a failed compute from use() for as long as the key
    // stays the same; invalidate() is what lets it try again.
    let values: ReadonlyArray<string> = [];
    let failure: unknown = undefined;
    try {
      values =
        this.formFactors.use({
          key: {column: FORM_FACTOR_COLUMN, facet: encodeFilters(facet)},
          compute: () =>
            load(FORM_FACTOR_COLUMN, facet, '', this.lifetime.signal),
        }).data ?? [];
    } catch (e) {
      failure = e;
    }
    const picked = [...draft.formFactors].sort();
    return m('.pf-bt-device-dialog__row', [
      m('span.pf-bt-device-dialog__label', 'Form factor:'),
      // The full list goes in a tooltip; the button itself stays short.
      m(
        'span',
        {title: picked.length > 2 ? picked.join(', ') : undefined},
        m(PopupMultiSelect, {
          label: describeFormFactors(picked),
          icon: 'arrow_drop_down',
          variant: ButtonVariant.Outlined,
          options: [
            ...picked,
            ...values.filter((v) => !draft.formFactors.has(v)),
          ].map((v) => ({id: v, name: v, checked: draft.formFactors.has(v)})),
          onChange: (diffs: MultiSelectDiff[]) => {
            for (const {id, checked} of diffs) {
              toggle(draft.formFactors, id, checked);
            }
          },
        }),
      ),
      failure !== undefined &&
        m(
          'span.pf-bt-device-dialog__error',
          {title: describeLoadError(failure).detail},
          'Couldn’t load form factors. ',
          m(Button, {
            label: 'Retry',
            compact: true,
            intent: Intent.Primary,
            onclick: () => this.formFactors.invalidate(),
          }),
        ),
    ]);
  }

  onremove(): void {
    this.lifetime.abort();
    this.formFactors.dispose();
  }
}

export function openDeviceFilterModal(
  bindings: SettingsBindings,
  load: ColumnValuesLoader,
  formFactor: boolean,
): void {
  const filters = bindings.getTraceFilters();
  const draft: DeviceDraft = {
    formFactors: new Set(readValueSet(filters, FORM_FACTOR_COLUMN)),
    manufacturers: new Set(readValueSet(filters, MANUFACTURER_COLUMN)),
    models: new Set(readValueSet(filters, MODEL_COLUMN)),
  };
  const facet = facetFilters(
    filters,
    FORM_FACTOR_COLUMN,
    MANUFACTURER_COLUMN,
    MODEL_COLUMN,
  );
  const apply = () => {
    let next = bindings.getTraceFilters();
    next = writeValueSet(next, FORM_FACTOR_COLUMN, [...draft.formFactors]);
    next = writeValueSet(next, MANUFACTURER_COLUMN, [...draft.manufacturers]);
    next = writeValueSet(next, MODEL_COLUMN, [...draft.models]);
    bindings.setTraceFilters(next);
    closeModal(MODAL_KEY);
  };
  showModal({
    key: MODAL_KEY,
    title: 'Select devices',
    className: 'pf-bt-device-modal',
    // The dialog draws its own footer so the summary and the buttons share
    // one row; the modal's button strip is left empty on purpose.
    content: () =>
      m(DeviceFilterDialog, {
        draft,
        load,
        facet,
        formFactor,
        onApply: apply,
        onCancel: () => closeModal(MODAL_KEY),
      }),
  });
}

interface DeviceFilterChipAttrs {
  readonly bindings: SettingsBindings;
  readonly load: ColumnValuesLoader;
  readonly formFactor: boolean;
}

// Unlike the other chips this one opens a modal: three linked lists are too
// much for an anchored popup.
export class DeviceFilterChip implements m.ClassComponent<DeviceFilterChipAttrs> {
  view({attrs}: m.Vnode<DeviceFilterChipAttrs>): m.Children {
    const filters = attrs.bindings.getTraceFilters();
    const formFactors = readValueSet(filters, FORM_FACTOR_COLUMN);
    const manufacturers = readValueSet(filters, MANUFACTURER_COLUMN);
    const models = readValueSet(filters, MODEL_COLUMN);
    return m(Button, {
      label: describeDevice(formFactors, manufacturers, models),
      icon: 'smartphone',
      variant: ButtonVariant.Outlined,
      intent: Intent.Primary,
      className: 'pf-bt-corpus-filters__chip',
      onclick: () =>
        openDeviceFilterModal(attrs.bindings, attrs.load, attrs.formFactor),
    });
  }
}
