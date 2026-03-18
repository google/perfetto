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
import {BaseNodeData} from './node_types';
import {TextInput} from '../../widgets/text_input';

export interface LimitNodeData extends BaseNodeData {
  readonly type: 'limit';
  readonly limitCount: string;
}

export function createLimitNode(
  id: string,
  x: number,
  y: number,
): LimitNodeData {
  return {type: 'limit', id, x, y, limitCount: '100'};
}

export function renderLimitNode(
  node: LimitNodeData,
  updateNode: (updates: Partial<Omit<LimitNodeData, 'type' | 'id'>>) => void,
): m.Children {
  return m(
    '.pf-qb-stack',
    m(TextInput, {
      placeholder: 'Row count...',
      value: node.limitCount,
      onChange: (value: string) => {
        updateNode({limitCount: value});
      },
    }),
  );
}
