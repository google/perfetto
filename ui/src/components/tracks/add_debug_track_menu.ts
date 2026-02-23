// Copyright (C) 2023 The Android Open Source Project
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
import {assertUnreachable} from '../../base/logging';
import {Trace} from '../../public/trace';
import {Form, FormLabel, FormSection} from '../../widgets/form';
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';
import {addDebugCounterTrack, addDebugSliceTrack} from './debug_tracks';

interface AddDebugTrackMenuAttrs {
  readonly trace: Trace; // Required for adding new tracks and modifying the workspace.
  // A list of available columns in the query results - used to work out sensible defaults for each field.
  readonly availableColumns: ReadonlyArray<string>;
  // The actual query used to define the debug track.
  readonly query: string;

  // Called when the user adds the track.
  readonly onAdd?: () => void;
}

const TRACK_NAME_FIELD_REF = 'TRACK_NAME_FIELD';

function chooseDefaultColumn(
  columns: ReadonlyArray<string>,
  name: string,
): string | undefined {
  // Search for exact match
  const exactMatch = columns.find((col) => col === name);
  if (exactMatch) return exactMatch;

  // Search for partial match
  const partialMatch = columns.find((col) => col.endsWith(`_${name}`));
  if (partialMatch) return partialMatch;

  // Debug tracks support data without dur, in which case it's treated as 0.
  if (name === 'dur') {
    return '0';
  }

  return undefined;
}

type TrackType = 'slice' | 'counter';
const trackTypes: ReadonlyArray<TrackType> = ['slice', 'counter'];

interface ConfigurationOptions {
  ts: string;
  dur: string;
  name: string;
  value: string;
  argSetId: string;
  pivot: string;
  color: string;
}

export class AddDebugTrackMenu
  implements m.ClassComponent<AddDebugTrackMenuAttrs>
{
  private trackName = '';
  private trackType: TrackType = 'slice';
  private readonly options: Partial<ConfigurationOptions>;

  constructor({attrs}: m.Vnode<AddDebugTrackMenuAttrs>) {
    const columns = attrs.availableColumns;

    // Initialize the settings to some sensible defaults.
    this.options = {
      ts: chooseDefaultColumn(columns, 'ts'),
      dur: chooseDefaultColumn(columns, 'dur'),
      name: chooseDefaultColumn(columns, 'name'),
      value: chooseDefaultColumn(columns, 'value'),
      argSetId: chooseDefaultColumn(columns, 'arg_set_id'),
      pivot: undefined,
      color: '', // Empty string means "from slice name"
    };
  }

  oncreate({dom}: m.VnodeDOM<AddDebugTrackMenuAttrs>) {
    this.focusTrackNameField(dom);
  }

  private focusTrackNameField(dom: Element) {
    const element = findRef(dom, TRACK_NAME_FIELD_REF);
    if (element) {
      if (element instanceof HTMLInputElement) {
        element.focus();
      }
    }
  }

  view({attrs}: m.Vnode<AddDebugTrackMenuAttrs>) {
    return m(
      Form,
      {
        onSubmit: () => {
          attrs.onAdd?.();
          this.createTracks(attrs);
        },
        submitLabel: 'Add Track',
        cancelLabel: 'Cancel',
      },
      m(FormLabel, {for: 'track_name'}, 'Track name'),
      m(
        TextInput,
        {
          id: 'track_name',
          ref: TRACK_NAME_FIELD_REF,
          onkeydown: (e: KeyboardEvent) => {
            // Allow Esc to close popup.
            if (e.key === 'Escape') return;
          },
          oninput: (e: InputEvent) => {
            if (!e.target) return;
            this.trackName = (e.target as HTMLInputElement).value;
          },
          placeholder: 'Enter track name...',
        },
        this.trackName,
      ),
      m(FormLabel, {for: 'track_type'}, 'Track type'),
      this.renderTrackTypeSelect(),
      m(
        FormSection,
        {label: 'Column mapping'},
        this.renderOptions(attrs.availableColumns),
      ),
    );
  }

  private renderTrackTypeSelect() {
    return m(
      Select,
      {
        id: 'track_type',
        oninput: (e: Event) => {
          if (!e.target) return;
          this.trackType = (e.target as HTMLSelectElement).value as TrackType;
        },
      },
      trackTypes.map((value) =>
        m(
          'option',
          {
            value: value,
            selected: this.trackType === value,
          },
          value,
        ),
      ),
    );
  }

  private renderOptions(availableColumns: ReadonlyArray<string>) {
    switch (this.trackType) {
      case 'slice':
        return this.renderSliceOptions(availableColumns);
      case 'counter':
        return this.renderCounterTrackOptions(availableColumns);
      default:
        assertUnreachable(this.trackType);
    }
  }

  private renderSliceOptions(availableColumns: ReadonlyArray<string>) {
    return [
      this.renderFormSelectInput('Timestamp column', 'ts', availableColumns),
      this.renderFormSelectInput('Duration column', 'dur', [
        '0',
        ...availableColumns,
      ]),
      this.renderFormSelectInput('Name column', 'name', availableColumns),
      this.renderColorSelect(availableColumns),
      this.renderFormSelectInput(
        'Arguments ID column (optional)',
        'argSetId',
        availableColumns,
        {
          optional: true,
        },
      ),
      this.renderFormSelectInput(
        'Pivot column (optional)',
        'pivot',
        availableColumns,
        {
          optional: true,
        },
      ),
    ];
  }

  private renderCounterTrackOptions(availableColumns: ReadonlyArray<string>) {
    return [
      this.renderFormSelectInput('Timestamp column', 'ts', availableColumns),
      this.renderFormSelectInput('Value column', 'value', availableColumns),
      this.renderFormSelectInput(
        'Pivot column (optional)',
        'pivot',
        availableColumns,
        {
          optional: true,
        },
      ),
    ];
  }

  private renderColorSelect(availableColumns: ReadonlyArray<string>) {
    return [
      m(FormLabel, {for: 'color'}, 'Color'),
      m(
        Select,
        {
          id: 'color',
          oninput: (e: Event) => {
            if (!e.target) return;
            this.options.color = (e.target as HTMLSelectElement).value;
          },
        },
        m(
          'option',
          {selected: this.options.color === '', value: ''},
          'Automatic (from slice name)',
        ),
        availableColumns.map((col) =>
          m('option', {selected: this.options.color === col, value: col}, col),
        ),
      ),
    ];
  }

  private renderFormSelectInput<K extends keyof ConfigurationOptions>(
    label: m.Children,
    optionKey: K,
    options: ReadonlyArray<string>,
    opts: Partial<{optional: boolean}> = {},
  ) {
    const {optional} = opts;
    return [
      m(FormLabel, {for: optionKey}, label),
      m(
        Select,
        {
          id: optionKey,
          required: !optional,
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
        optional
          ? m(
              'option',
              {selected: this.options[optionKey] === undefined, value: ''},
              '--None--',
            )
          : m(
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

  private createTracks(attrs: AddDebugTrackMenuAttrs) {
    switch (this.trackType) {
      case 'slice':
        addDebugSliceTrack({
          trace: attrs.trace,
          data: {
            sqlSource: attrs.query,
            columns: attrs.availableColumns,
          },
          title: this.trackName,
          columns: {
            ts: this.options.ts,
            dur: this.options.dur,
            name: this.options.name,
          },
          argSetIdColumn: this.options.argSetId,
          rawColumns: attrs.availableColumns,
          pivotOn: this.options.pivot,
          colorColumn: this.options.color || undefined,
        });
        break;
      case 'counter':
        addDebugCounterTrack({
          trace: attrs.trace,
          data: {
            sqlSource: attrs.query,
            columns: attrs.availableColumns,
          },
          title: this.trackName,
          columns: {
            ts: this.options.ts,
            value: this.options.value,
          },
          pivotOn: this.options.pivot,
        });
        break;
      default:
        assertUnreachable(this.trackType);
    }
  }
}
