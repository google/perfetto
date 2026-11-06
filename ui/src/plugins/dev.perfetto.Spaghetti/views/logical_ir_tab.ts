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
import {CodeSnippet} from '../../../widgets/code_snippet';
import {EmptyState} from '../../../widgets/empty_state';
import type {LogicalPlan} from '../logical_ir';
import './logical_ir_tab.scss';

export interface LogicalIrTabAttrs {
  readonly logicalPlan: LogicalPlan | undefined;
  readonly activeNodeId: string | undefined;
}

export class LogicalIrTab implements m.ClassComponent<LogicalIrTabAttrs> {
  view({attrs}: m.Vnode<LogicalIrTabAttrs>) {
    const {logicalPlan, activeNodeId} = attrs;
    if (!logicalPlan) {
      if (activeNodeId) {
        return m(
          EmptyState,
          {
            icon: 'warning',
            fillHeight: true,
            title: 'Incomplete query',
          },
          'Fill in all required fields to inspect the logical IR.',
        );
      }
      return m(
        EmptyState,
        {
          fillHeight: true,
          title: 'No node selected',
        },
        'Click on a node to inspect its logical IR.',
      );
    }
    return m(
      '.pf-spag-logical-ir-tab',
      m(CodeSnippet, {
        language: 'JSON',
        text: JSON.stringify(logicalPlan, null, 2),
      }),
    );
  }
}
