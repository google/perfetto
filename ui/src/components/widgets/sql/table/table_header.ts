// Copyright (C) 2025 The Android Open Source Project
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
import {SortDirection} from '../../../../base/comparison_utils';
import {Icons} from '../../../../base/semantic_icons';
import {MenuItem} from '../../../../widgets/menu';

export function renderColumnIcon(sorted: SortDirection | undefined) {
  if (sorted === undefined) return Icons.ContextMenu;
  if (sorted === 'ASC') return Icons.SortedAsc;
  return Icons.SortedDesc;
}

export function renderSortMenuItems(
  sorted: SortDirection | undefined,
  sort: (direction: SortDirection | undefined) => void,
) {
  return [
    sorted !== 'DESC' &&
      m(MenuItem, {
        label: 'Sort: highest first',
        icon: Icons.SortedDesc,
        onclick: () => sort('DESC'),
      }),
    sorted !== 'ASC' &&
      m(MenuItem, {
        label: 'Sort: lowest first',
        icon: Icons.SortedAsc,
        onclick: () => sort('ASC'),
      }),
    sorted !== undefined &&
      m(MenuItem, {
        label: 'Unsort',
        icon: Icons.Close,
        onclick: () => sort(undefined),
      }),
  ];
}
