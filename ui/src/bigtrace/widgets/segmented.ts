// Copyright (C) 2026 The Android Open Source Project
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
import {classNames} from '../../base/classnames';

export interface SegmentedItem {
  readonly key: string;
  readonly label: m.Children;
  readonly title?: string;
}

// Flat segmented control: one button per item, the active one highlighted.
// Deliberately not the document-tab widget (no panel chrome). Stateless — the
// caller owns the active key and updates it from onSelect.
export function renderSegmented(
  items: ReadonlyArray<SegmentedItem>,
  active: string,
  onSelect: (key: string) => void,
  className?: string,
): m.Children {
  return m(
    '.pf-bt-segmented',
    {className},
    items.map((item) =>
      m(
        'button.pf-bt-segmented__item',
        {
          className: classNames(
            item.key === active && 'pf-bt-segmented__item--active',
          ),
          title: item.title,
          onclick: () => onSelect(item.key),
        },
        item.label,
      ),
    ),
  );
}
