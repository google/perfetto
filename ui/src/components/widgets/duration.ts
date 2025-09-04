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
import {copyToClipboard} from '../../base/clipboard';
import {Icons} from '../../base/semantic_icons';
import {duration} from '../../base/time';
import {Anchor} from '../../widgets/anchor';
import {MenuDivider, MenuItem, PopupMenu} from '../../widgets/menu';
import {Trace} from '../../public/trace';
import {formatDuration} from '../time_utils';
import {DurationPrecisionMenuItem} from './duration_precision_menu_items';
import {TimestampFormatMenuItem} from './timestamp_format_menu';

interface DurationWidgetAttrs {
  trace: Trace;
  dur: duration;
  extraMenuItems?: m.Child[];
}

export class DurationWidget implements m.ClassComponent<DurationWidgetAttrs> {
  view({attrs}: m.Vnode<DurationWidgetAttrs>) {
    const {trace, dur} = attrs;

    const value: m.Children =
      dur === -1n ? m('i', '(Did not end)') : formatDuration(trace, dur);

    return m(
      PopupMenu,
      {
        trigger: m(Anchor, value),
      },
      m(MenuItem, {
        icon: Icons.Copy,
        label: `Copy raw value`,
        onclick: () => {
          copyToClipboard(dur.toString());
        },
      }),
      m(TimestampFormatMenuItem, {trace: trace}),
      m(DurationPrecisionMenuItem, {trace: trace}),
      attrs.extraMenuItems ? [m(MenuDivider), attrs.extraMenuItems] : null,
    );
  }
}
