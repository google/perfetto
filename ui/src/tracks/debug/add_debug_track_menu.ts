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

import {EngineProxy} from '../../common/engine';
import {Button} from '../../frontend/widgets/button';
import {Form, FormButtonBar, FormLabel} from '../../frontend/widgets/form';
import {Select} from '../../frontend/widgets/select';
import {TextInput} from '../../frontend/widgets/text_input';

import {addDebugTrack, SliceColumns} from './slice_track';

export const ARG_PREFIX = 'arg_';

export function uuidToViewName(uuid: string): string {
  return `view_${uuid.split('-').join('_')}`;
}

interface AddDebugTrackMenuAttrs {
  sqlViewName: string;
  columns: string[];
  engine: EngineProxy;
}

export class AddDebugTrackMenu implements
    m.ClassComponent<AddDebugTrackMenuAttrs> {
  name: string = '';
  sliceColumns: SliceColumns;

  constructor(vnode: m.Vnode<AddDebugTrackMenuAttrs>) {
    const chooseDefaultOption = (name: string) => {
      for (const column of vnode.attrs.columns) {
        if (column === name) return column;
      }
      for (const column of vnode.attrs.columns) {
        if (column.endsWith(`_${name}`)) return column;
      }
      return vnode.attrs.columns[0];
    };

    this.sliceColumns = {
      ts: chooseDefaultOption('ts'),
      dur: chooseDefaultOption('dur'),
      name: chooseDefaultOption('name'),
    };
  }

  view(vnode: m.Vnode<AddDebugTrackMenuAttrs>) {
    const renderSelect = (name: 'ts'|'dur'|'name') => {
      const options = [];
      for (const column of vnode.attrs.columns) {
        options.push(
            m('option',
              {
                selected: this.sliceColumns[name] === column ? true : undefined,
              },
              column));
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
              this.sliceColumns[name] = (e.target as HTMLSelectElement).value;
            },
          },
          options),
      ];
    };
    return m(
        Form,
        m(FormLabel,
          {for: 'track_name',
          },
          'Name'),
        m(TextInput, {
          id: 'track_name',
          onkeydown: (e: KeyboardEvent) => {
            // Allow Esc to close popup.
            if (e.key === 'Escape') return;
            e.stopPropagation();
          },
          oninput: (e: KeyboardEvent) => {
            if (!e.target) return;
            this.name = (e.target as HTMLInputElement).value;
          },
        }),
        renderSelect('ts'),
        renderSelect('dur'),
        renderSelect('name'),
        m(
            FormButtonBar,
            m(Button, {
              label: 'Show',
              className: 'pf-close-parent-popup-on-click',
              onclick: (e: Event) => {
                e.preventDefault();
                addDebugTrack(
                    vnode.attrs.engine,
                    vnode.attrs.sqlViewName,
                    this.name,
                    this.sliceColumns,
                    vnode.attrs.columns);
              },
            }),
            ),
    );
  }
}
