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
import {EngineProxy} from '../../common/engine';
import {raf} from '../../core/raf_scheduler';
import {Form, FormLabel} from '../../widgets/form';
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';

import {addDebugCounterTrack} from './counter_track';
import {addDebugSliceTrack, SqlDataSource} from './slice_track';

export const ARG_PREFIX = 'arg_';

export function uuidToViewName(uuid: string): string {
  return `view_${uuid.split('-').join('_')}`;
}

interface AddDebugTrackMenuAttrs {
  dataSource: SqlDataSource;
  engine: EngineProxy;
}

const TRACK_NAME_FIELD_REF = 'TRACK_NAME_FIELD';

export class AddDebugTrackMenu implements
    m.ClassComponent<AddDebugTrackMenuAttrs> {
  readonly columns: string[];

  name: string = '';
  trackType: 'slice'|'counter' = 'slice';
  // Names of columns which will be used as data sources for rendering.
  // We store the config for all possible columns used for rendering (i.e.
  // 'value' for slice and 'name' for counter) and then just don't the values
  // which don't match the currently selected track type (so changing track type
  // from A to B and back to A is a no-op).
  renderParams: {ts: string; dur: string; name: string; value: string;};

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

  private renderTrackTypeSelect() {
    const options = [];
    for (const type of ['slice', 'counter']) {
      options.push(
          m('option',
            {
              value: type,
              selected: this.trackType === type ? true : undefined,
            },
            type));
    }
    return m(
        Select,
        {
          id: 'track_type',
          oninput: (e: Event) => {
            if (!e.target) return;
            this.trackType =
                (e.target as HTMLSelectElement).value as 'slice' | 'counter';
            raf.scheduleFullRedraw();
          },
        },
        options);
  }

  view(vnode: m.Vnode<AddDebugTrackMenuAttrs>) {
    const renderSelect = (name: 'ts'|'dur'|'name'|'value') => {
      const options = [];
      for (const column of this.columns) {
        options.push(
            m('option',
              {
                selected: this.renderParams[name] === column ? true : undefined,
              },
              column));
      }
      if (name === 'dur') {
        options.push(
            m('option',
              {selected: this.renderParams[name] === '0' ? true : undefined},
              m('i', '0')));
      }
      return [
        m(FormLabel,
          {for: name,
          },
          name),
        m(Select,
          {
            id: name,
            oninput: (e: Event) => {
              if (!e.target) return;
              this.renderParams[name] = (e.target as HTMLSelectElement).value;
            },
          },
          options),
      ];
    };
    return m(
        Form,
        {
          onSubmit: () => {
            switch (this.trackType) {
              case 'slice':
                addDebugSliceTrack(
                    vnode.attrs.engine,
                    vnode.attrs.dataSource,
                    this.name,
                    {
                      ts: this.renderParams.ts,
                      dur: this.renderParams.dur,
                      name: this.renderParams.name,
                    },
                    this.columns);
                break;
              case 'counter':
                addDebugCounterTrack(
                    vnode.attrs.engine, vnode.attrs.dataSource, this.name, {
                      ts: this.renderParams.ts,
                      value: this.renderParams.value,
                    });
                break;
            }
          },
          submitLabel: 'Show',
        },
        m(FormLabel,
          {for: 'track_name',
          },
          'Track name'),
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
        m(FormLabel,
          {for: 'track_type',
          },
          'Track type'),
        this.renderTrackTypeSelect(),
        renderSelect('ts'),
        this.trackType === 'slice' && renderSelect('dur'),
        this.trackType === 'slice' && renderSelect('name'),
        this.trackType === 'counter' && renderSelect('value'),
    );
  }
}
