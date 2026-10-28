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

// Flamegraph hooks for the merged class warning, passed as the
// TreeExplorerPanel `nodeIcon` and `renderNodeTooltip` attrs. The flamegraph
// is a canvas, so it cannot render ClassNameWithWarning directly.
//
// `propertyName` is the column in the flamegraph's SQL query that holds the
// flag (1 = potentially merged). The flamegraph drops extra columns unless
// they're listed in the metric's `unaggregatableProperties`, so make sure it
// is declared in there.

import m from 'mithril';
import {Icons} from '../base/semantic_icons';
import type {
  TreeExplorerNodeIconFn,
  TreeExplorerNodeTooltipRenderer,
} from '../widgets/tree_explorer';
import {MergedClassWarningCallout} from './merged_class_warning';

// Hook that tells the flamegraph to draw a warning icon if the node is flagged
// as a merged class.
export function mergedClassNodeIcon(
  propertyName: string,
): TreeExplorerNodeIconFn {
  return (properties) =>
    properties.get(propertyName) === '1' ? Icons.Warning : undefined;
}

// Hook that tells the flamegraph to add a warning callout at the top of the
// tooltip of nodes flagged as merged classes.
export function mergedClassNodeTooltipRenderer(
  propertyName: string,
): TreeExplorerNodeTooltipRenderer {
  return ({properties}, defaultContent) => [
    properties.get(propertyName) === '1' &&
      m('.pf-merged-class-flamegraph-callout', m(MergedClassWarningCallout)),
    defaultContent,
  ];
}
