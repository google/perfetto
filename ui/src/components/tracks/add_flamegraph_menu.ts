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
import {findRef} from '../../base/dom_utils';
import type {Trace} from '../../public/trace';
import {Form, FormGrid, FormLabel, FormSection} from '../../widgets/form';
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';
import {addQueryFlamegraphTab} from './query_flamegraph_tab';

interface AddFlamegraphMenuAttrs {
  readonly trace: Trace; // Required for opening the flamegraph tab.
  // A list of available columns in the query results - used to work out sensible defaults for each field.
  readonly availableColumns: ReadonlyArray<string>;
  // The actual query that produces the rows.
  readonly query: string;

  // Called when the user opens the flamegraph.
  readonly onAdd?: () => void;
}

const TITLE_FIELD_REF = 'FLAMEGRAPH_TITLE_FIELD';

function chooseDefaultColumn(
  columns: ReadonlyArray<string>,
  name: string,
): string | undefined {
  // Search for exact match.
  const exactMatch = columns.find((col) => col === name);
  if (exactMatch) return exactMatch;

  // Search for partial match (handles `<table>_id`-style aliases).
  const partialMatch = columns.find((col) => col.endsWith(`_${name}`));
  if (partialMatch) return partialMatch;

  return undefined;
}

interface ConfigurationOptions {
  id: string;
  parentId: string;
  name: string;
  value: string;
}

export class AddFlamegraphMenu
  implements m.ClassComponent<AddFlamegraphMenuAttrs>
{
  private title = '';
  private sampleType = 'samples';
  private unit = 'count';
  private readonly options: Partial<ConfigurationOptions>;

  constructor({attrs}: m.Vnode<AddFlamegraphMenuAttrs>) {
    const columns = attrs.availableColumns;

    // Initialize the settings to some sensible defaults.
    this.options = {
      id: chooseDefaultColumn(columns, 'id'),
      parentId: chooseDefaultColumn(columns, 'parent_id'),
      name: chooseDefaultColumn(columns, 'name'),
      value:
        chooseDefaultColumn(columns, 'self_value') ??
        chooseDefaultColumn(columns, 'value') ??
        chooseDefaultColumn(columns, 'self_size') ??
        chooseDefaultColumn(columns, 'self_count') ??
        chooseDefaultColumn(columns, 'dur'),
    };
  }

  oncreate({dom}: m.VnodeDOM<AddFlamegraphMenuAttrs>) {
    this.focusTitleField(dom);
  }

  private focusTitleField(dom: Element) {
    const element = findRef(dom, TITLE_FIELD_REF);
    if (element) {
      if (element instanceof HTMLInputElement) {
        element.focus();
      }
    }
  }

  view({attrs}: m.Vnode<AddFlamegraphMenuAttrs>) {
    return m(
      Form,
      {
        className: 'pf-add-flamegraph-menu',
        onSubmit: () => {
          attrs.onAdd?.();
          this.openFlamegraph(attrs);
        },
        submitLabel: 'Add Flamegraph',
        cancelLabel: 'Cancel',
      },
      m(FormLabel, {for: 'flamegraph_title'}, 'Title'),
      m(
        TextInput,
        {
          id: 'flamegraph_title',
          ref: TITLE_FIELD_REF,
          onkeydown: (e: KeyboardEvent) => {
            // Allow Esc to close popup.
            if (e.key === 'Escape') return;
          },
          oninput: (e: InputEvent) => {
            if (!e.target) return;
            this.title = (e.target as HTMLInputElement).value;
          },
          placeholder: 'Enter flamegraph title...',
        },
        this.title,
      ),
      m(
        FormSection,
        {label: 'Column mapping'},
        m(
          FormGrid,
          this.renderFormSelectInput('Id *', 'id', attrs.availableColumns),
          this.renderFormSelectInput(
            'Parent id *',
            'parentId',
            attrs.availableColumns,
          ),
          this.renderFormSelectInput('Name *', 'name', attrs.availableColumns),
          this.renderFormSelectInput(
            'Self value *',
            'value',
            attrs.availableColumns,
          ),
          m(FormLabel, {for: 'flamegraph_sample_type'}, 'Sample type'),
          m(TextInput, {
            id: 'flamegraph_sample_type',
            value: this.sampleType,
            oninput: (e: InputEvent) => {
              this.sampleType = (e.target as HTMLInputElement).value;
            },
          }),
          m(FormLabel, {for: 'flamegraph_unit'}, 'Unit'),
          m(TextInput, {
            id: 'flamegraph_unit',
            value: this.unit,
            oninput: (e: InputEvent) => {
              this.unit = (e.target as HTMLInputElement).value;
            },
          }),
        ),
      ),
    );
  }

  private renderFormSelectInput<K extends keyof ConfigurationOptions>(
    label: m.Children,
    optionKey: K,
    options: ReadonlyArray<string>,
  ) {
    return [
      m(FormLabel, {for: optionKey}, label),
      m(
        Select,
        {
          id: optionKey,
          required: true,
          oninput: (e: Event) => {
            if (!e.target) return;
            const newValue = (e.target as HTMLSelectElement).value;
            if (newValue === '') {
              delete this.options[optionKey];
            } else {
              this.options[optionKey] = newValue;
            }
          },
        },
        m(
          'option',
          {
            selected: this.options[optionKey] === undefined,
            value: '',
            hidden: true,
            disabled: true,
          },
          'Select a column...',
        ),
        options.map((opt) =>
          m(
            'option',
            {selected: this.options[optionKey] === opt, value: opt},
            opt,
          ),
        ),
      ),
    ];
  }

  private openFlamegraph(attrs: AddFlamegraphMenuAttrs) {
    const {id, parentId, name, value} = this.options;
    if (
      id === undefined ||
      parentId === undefined ||
      name === undefined ||
      value === undefined
    ) {
      return;
    }
    addQueryFlamegraphTab({
      trace: attrs.trace,
      title: this.title.trim() || 'flamegraph',
      sourceQuery: attrs.query,
      idColumn: id,
      parentIdColumn: parentId,
      nameColumn: name,
      valueColumn: value,
      sampleType: this.sampleType.trim() || 'samples',
      unit: this.unit.trim() || 'count',
    });
  }
}
