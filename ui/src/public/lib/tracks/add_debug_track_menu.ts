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
import {findRef} from '../../../base/dom_utils';
import {Form, FormLabel} from '../../../widgets/form';
import {Select} from '../../../widgets/select';
import {TextInput} from '../../../widgets/text_input';
import {
  addDebugCounterTrack,
  addDebugSliceTrack,
  addPivotedTracks,
} from './debug_tracks';
import {Trace} from '../../trace';
import {SliceColumnMapping, SqlDataSource} from './query_slice_track';
import {CounterColumnMapping} from './query_counter_track';

interface AddDebugTrackMenuAttrs {
  dataSource: Required<SqlDataSource>;
  trace: Trace;
}

const TRACK_NAME_FIELD_REF = 'TRACK_NAME_FIELD';

export class AddDebugTrackMenu
  implements m.ClassComponent<AddDebugTrackMenuAttrs>
{
  readonly columns: string[];

  name: string = '';
  trackType: 'slice' | 'counter' = 'slice';
  // Names of columns which will be used as data sources for rendering.
  // We store the config for all possible columns used for rendering (i.e.
  // 'value' for slice and 'name' for counter) and then just don't the values
  // which don't match the currently selected track type (so changing track type
  // from A to B and back to A is a no-op).
  renderParams: {
    ts: string;
    dur: string;
    name: string;
    value: string;
    pivot: string;
  };

  constructor(vnode: m.Vnode<AddDebugTrackMenuAttrs>) {
    this.columns = [...vnode.attrs.dataSource.columns];

    const chooseDefaultOption = (name: string) => {
      for (const column of this.columns) {
        if (column === name) return column;
      }
      for (const column of this.columns) {
        if (column.endsWith(`_${name}`)) return column;
      }
      // Debug tracks support data without dur, in which case it's treated as
      // 0.
      if (name === 'dur') {
        return '0';
      }
      return this.columns[0];
    };

    this.renderParams = {
      ts: chooseDefaultOption('ts'),
      dur: chooseDefaultOption('dur'),
      name: chooseDefaultOption('name'),
      value: chooseDefaultOption('value'),
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

  private renderTrackTypeSelect(trace: Trace) {
    const options = [];
    for (const type of ['slice', 'counter']) {
      options.push(
        m(
          'option',
          {
            value: type,
            selected: this.trackType === type ? true : undefined,
          },
          type,
        ),
      );
    }
    return m(
      Select,
      {
        id: 'track_type',
        oninput: (e: Event) => {
          if (!e.target) return;
          this.trackType = (e.target as HTMLSelectElement).value as
            | 'slice'
            | 'counter';
          trace.scheduleFullRedraw();
        },
      },
      options,
    );
  }

  view(vnode: m.Vnode<AddDebugTrackMenuAttrs>) {
    const renderSelect = (name: 'ts' | 'dur' | 'name' | 'value' | 'pivot') => {
      const options = [];

      if (name === 'pivot') {
        options.push(
          m(
            'option',
            {selected: this.renderParams[name] === '' ? true : undefined},
            m('i', ''),
          ),
        );
      }
      for (const column of this.columns) {
        options.push(
          m(
            'option',
            {selected: this.renderParams[name] === column ? true : undefined},
            column,
          ),
        );
      }
      if (name === 'dur') {
        options.push(
          m(
            'option',
            {selected: this.renderParams[name] === '0' ? true : undefined},
            m('i', '0'),
          ),
        );
      }
      return [
        m(FormLabel, {for: name}, name),
        m(
          Select,
          {
            id: name,
            oninput: (e: Event) => {
              if (!e.target) return;
              this.renderParams[name] = (e.target as HTMLSelectElement).value;
            },
          },
          options,
        ),
      ];
    };

    return m(
      Form,
      {
        onSubmit: () => {
          switch (this.trackType) {
            case 'slice':
              const sliceColumns: SliceColumnMapping = {
                ts: this.renderParams.ts,
                dur: this.renderParams.dur,
                name: this.renderParams.name,
              };
              if (this.renderParams.pivot) {
                addPivotedTracks(
                  vnode.attrs.trace,
                  vnode.attrs.dataSource,
                  this.name,
                  this.renderParams.pivot,
                  async (ctx, data, trackName) =>
                    addDebugSliceTrack({
                      trace: ctx,
                      data,
                      title: trackName,
                      columns: sliceColumns,
                      argColumns: this.columns,
                    }),
                );
              } else {
                addDebugSliceTrack({
                  trace: vnode.attrs.trace,
                  data: vnode.attrs.dataSource,
                  title: this.name,
                  columns: sliceColumns,
                  argColumns: this.columns,
                });
              }
              break;
            case 'counter':
              const counterColumns: CounterColumnMapping = {
                ts: this.renderParams.ts,
                value: this.renderParams.value,
              };

              if (this.renderParams.pivot) {
                addPivotedTracks(
                  vnode.attrs.trace,
                  vnode.attrs.dataSource,
                  this.name,
                  this.renderParams.pivot,
                  async (ctx, data, trackName) =>
                    addDebugCounterTrack({
                      trace: ctx,
                      data,
                      title: trackName,
                      columns: counterColumns,
                    }),
                );
              } else {
                addDebugCounterTrack({
                  trace: vnode.attrs.trace,
                  data: vnode.attrs.dataSource,
                  title: this.name,
                  columns: counterColumns,
                });
              }
              break;
          }
        },
        submitLabel: 'Show',
      },
      m(FormLabel, {for: 'track_name'}, 'Track name'),
      m(TextInput, {
        id: 'track_name',
        ref: TRACK_NAME_FIELD_REF,
        onkeydown: (e: KeyboardEvent) => {
          // Allow Esc to close popup.
          if (e.key === 'Escape') return;
        },
        oninput: (e: KeyboardEvent) => {
          if (!e.target) return;
          this.name = (e.target as HTMLInputElement).value;
        },
      }),
      m(FormLabel, {for: 'track_type'}, 'Track type'),
      this.renderTrackTypeSelect(vnode.attrs.trace),
      renderSelect('ts'),
      this.trackType === 'slice' && renderSelect('dur'),
      this.trackType === 'slice' && renderSelect('name'),
      this.trackType === 'counter' && renderSelect('value'),
      renderSelect('pivot'),
    );
  }
}
