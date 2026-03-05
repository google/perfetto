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
import {HTMLInputAttrs} from './common';

export class Select implements m.ClassComponent<HTMLInputAttrs> {
  view({attrs, children}: m.CVnode<HTMLInputAttrs>) {
    return m(
      'label.pf-select',
      m('select.pf-select__input', attrs, children),
      m(
        'svg.pf-select__chevron',
        {
          viewBox: '0 0 12 12',
          fill: 'none',
          xmlns: 'http://www.w3.org/2000/svg',
        },
        m('path', {
          'd': 'M3 5L6 8L9 5',
          'stroke': 'currentColor',
          'stroke-width': '1.5',
          'stroke-linecap': 'round',
          'stroke-linejoin': 'round',
        }),
      ),
    );
  }
}
