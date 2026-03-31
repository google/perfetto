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
import {Checkbox} from '../../widgets/checkbox';
import {BaseNodeData} from './node_types';

export interface UnionAllNodeData extends BaseNodeData {
  readonly type: 'union_all';
  readonly distinct: boolean;
}

export function createUnionAllNode(
  id: string,
  x: number,
  y: number,
): UnionAllNodeData {
  return {type: 'union_all', id, x, y, distinct: false};
}

export function renderUnionAllNode(
  node: UnionAllNodeData,
  updateNode: (updates: Partial<Omit<UnionAllNodeData, 'type' | 'id'>>) => void,
): m.Children {
  return m(Checkbox, {
    label: 'Distinct',
    checked: node.distinct,
    onchange: () => updateNode({distinct: !node.distinct}),
  });
}
