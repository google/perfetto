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
import {Form, FormLabel} from '../../widgets/form';
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';
import {addDebugCounterTrack, addDebugSliceTrack} from './debug_tracks';

interface AddDebugTrackMenuAttrs {
  readonly trace: Trace; // Required for adding new tracks and modifying the workspace.
  // A list of available columns in the query results - used to work out sensible defaults for each field.
  readonly availableColumns: ReadonlyArray<string>;
  // The actual query used to define the debug track.
  readonly query: string;
}

const TRACK_NAME_FIELD_REF = 'TRACK_NAME_FIELD';

function chooseDefaultColumn(
  columns: ReadonlyArray<string>,
  name: string,
): string {
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

  return '';
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
}

export class AddDebugTrackMenu
  implements m.ClassComponent<AddDebugTrackMenuAttrs>
{
  private trackName = '';
  private trackType: TrackType = 'slice';
  private readonly options: ConfigurationOptions;

  constructor({attrs}: m.Vnode<AddDebugTrackMenuAttrs>) {
    const columns = attrs.availableColumns;

    // Initialize the settings to some sensible defaults.
    this.options = {
      ts: chooseDefaultColumn(columns, 'ts'),
      dur: chooseDefaultColumn(columns, 'dur'),
      name: chooseDefaultColumn(columns, 'name'),
      value: chooseDefaultColumn(columns, 'value'),
      argSetId: chooseDefaultColumn(columns, 'arg_set_id'),
      pivot: '',
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
        onSubmit: () => this.createTracks(attrs),
        submitLabel: 'Add Track',
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
          oninput: (e: KeyboardEvent) => {
            if (!e.target) return;
            this.trackName = (e.target as HTMLInputElement).value;
          },
        },
        this.trackName,
      ),
      m(FormLabel, {for: 'track_type'}, 'Track type'),
      this.renderTrackTypeSelect(),
      this.renderOptions(attrs.availableColumns),
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
      this.renderFormSelectInput('ts', 'ts', availableColumns),
      this.renderFormSelectInput('dur', 'dur', ['0', ...availableColumns]),
      this.renderFormSelectInput('name', 'name', availableColumns),
      this.renderFormSelectInput('arg_set_id', 'argSetId', [
        '',
        ...availableColumns,
      ]),
      this.renderFormSelectInput('pivot', 'pivot', ['', ...availableColumns]),
    ];
  }

  private renderCounterTrackOptions(availableColumns: ReadonlyArray<string>) {
    return [
      this.renderFormSelectInput('ts', 'ts', availableColumns),
      this.renderFormSelectInput('value', 'value', availableColumns),
      this.renderFormSelectInput('pivot', 'pivot', ['', ...availableColumns]),
    ];
  }

  private renderFormSelectInput<K extends keyof ConfigurationOptions>(
    name: string,
    optionKey: K,
    options: ReadonlyArray<string>,
  ) {
    return [
      m(FormLabel, {for: name}, name),
      m(
        Select,
        {
          id: name,
          oninput: (e: Event) => {
            if (!e.target) return;
            this.options[optionKey] = (e.target as HTMLSelectElement).value;
          },
          value: this.options[optionKey],
        },
        options.map((opt) =>
          m('option', {selected: this.options[optionKey] === opt}, opt),
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
          argColumns: attrs.availableColumns,
          pivotOn: this.options.pivot,
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
