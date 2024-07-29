// Copyright (C) 2024 The Android Open Source Project
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

import {
  ThreadInfo,
  getThreadName,
} from '../../trace_processor/sql_utils/thread';
import {MenuItem, PopupMenu2} from '../../widgets/menu';
import {Anchor} from '../../widgets/anchor';
import {exists} from '../../base/utils';
import {Icons} from '../../base/semantic_icons';
import {copyToClipboard} from '../../base/clipboard';

export function renderThreadRef(info: ThreadInfo): m.Children {
  const name = info.name;
  return m(
    PopupMenu2,
    {
      trigger: m(Anchor, getThreadName(info)),
    },
    exists(name) &&
      m(MenuItem, {
        icon: Icons.Copy,
        label: 'Copy thread name',
        onclick: () => copyToClipboard(name),
      }),
    exists(info.tid) &&
      m(MenuItem, {
        icon: Icons.Copy,
        label: 'Copy tid',
        onclick: () => copyToClipboard(`${info.tid}`),
      }),
    m(MenuItem, {
      icon: Icons.Copy,
      label: 'Copy utid',
      onclick: () => copyToClipboard(`${info.utid}`),
    }),
  );
}
