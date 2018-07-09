// Copyright (C) 2018 The Android Open Source Project
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

import * as m from 'mithril';

import {CanvasWrapper} from './canvas_wrapper';
import {Track} from './track';

export const Frontend = {
  view({attrs}) {
    return m(
        '.frontend',
        {
          style: {
            padding: '20px',
            position: 'relative',
            width: attrs.width.toString() + 'px'
          }
        },
        m(CanvasWrapper, {width: attrs.width, height: attrs.height}),
        m(Track, {name: 'Track 123'}), );
  }
} as m.Component<{width: number, height: number}>;
