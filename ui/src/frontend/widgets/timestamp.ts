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

import {formatTime} from '../../common/time';
import {Anchor} from '../anchor';
import {copyToClipboard} from '../clipboard';
import {Icons} from '../semantic_icons';
import {TPTimestamp} from '../sql_types';

import {MenuItem, PopupMenu2} from './menu';

interface TimestampAttrs {
  // The timestamp to print, this should be the absolute, raw timestamp as
  // found in trace processor.
  ts: TPTimestamp;
  minimal?: boolean;
}

export class Timestamp implements m.ClassComponent<TimestampAttrs> {
  view({attrs}: m.Vnode<TimestampAttrs>) {
    const {ts, minimal = false} = attrs;
    return m(
        PopupMenu2,
        {
          trigger:
              m(Anchor, {icon: Icons.ContextMenu}, formatTime(ts, minimal)),
        },
        m(MenuItem, {
          icon: Icons.Copy,
          label: 'Copy raw timestamp',
          onclick: () => {
            copyToClipboard(ts.toString());
          },
        }),
    );
  }
}
