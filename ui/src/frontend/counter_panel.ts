// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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

import {DetailsShell} from '../widgets/details_shell';
import {DurationWidget} from '../widgets/duration';
import {GridLayout} from '../widgets/grid_layout';
import {Section} from '../widgets/section';
import {Tree, TreeNode} from '../widgets/tree';

import {globals} from './globals';
import {Timestamp} from './widgets/timestamp';

export class CounterDetailsPanel implements m.ClassComponent {
  view() {
    const counterInfo = globals.counterDetails;
    if (counterInfo && counterInfo.startTime &&
        counterInfo.name !== undefined && counterInfo.value !== undefined &&
        counterInfo.delta !== undefined && counterInfo.duration !== undefined) {
      return m(
          DetailsShell,
          {title: 'Counter', description: `${counterInfo.name}`},
          m(GridLayout,
            m(
                Section,
                {title: 'Properties'},
                m(
                    Tree,
                    m(TreeNode, {left: 'Name', right: `${counterInfo.name}`}),
                    m(TreeNode, {
                      left: 'Start time',
                      right: m(Timestamp, {ts: counterInfo.startTime}),
                    }),
                    m(TreeNode, {
                      left: 'Value',
                      right: `${counterInfo.value.toLocaleString()}`,
                    }),
                    m(TreeNode, {
                      left: 'Delta',
                      right: `${counterInfo.delta.toLocaleString()}`,
                    }),
                    m(TreeNode, {
                      left: 'Duration',
                      right: m(DurationWidget, {dur: counterInfo.duration}),
                    }),
                    ),
                )),
      );
    } else {
      return m(DetailsShell, {title: 'Counter', description: 'Loading...'});
    }
  }

  renderCanvas() {}
}
