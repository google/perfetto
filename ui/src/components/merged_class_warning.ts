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

// Warning for Java class names in heap dumps that may cover several source
// classes merged during app optimization (e.g. R8 class merging).
//
// Exposed in three forms that share the same text:
//  1. ClassNameWithWarning: a class name followed by the warning icon. Use
//     this for class names in tables and labels.
//  2. MergedClassWarning: just the warning icon.
//  3. mergedClassNodeIcon() and mergedClassNodeTooltipRenderer(): flamegraph
//     metric hooks that draw the icon on flagged nodes and add a warning
//     callout to their tooltip. The flamegraph is a canvas, so it cannot use
//     1. or 2.

import './merged_class_warning.scss';
import m from 'mithril';
import {Icon} from '../widgets/icon';
import {Intent} from '../widgets/common';
import {PopupPosition} from '../widgets/popup';
import {Tooltip} from '../widgets/tooltip';
import type {
  TreeExplorerNodeIconFn,
  TreeExplorerNodeTooltipRenderer,
} from '../widgets/tree_explorer';

export const MERGED_CLASS_WARNING_ICON = 'warning';
const MERGED_CLASS_WARNING_TITLE = 'Possibly merged class';
const MERGED_CLASS_WARNING_DESCRIPTION =
  'This class may include code from several source classes that were ' +
  'combined during app optimization (e.g. R8 class merging). Its name and ' +
  'memory may cover more than the class shown.';

export interface MergedClassWarningAttrs {
  readonly position?: PopupPosition;
}

// Warning icon with the warning text in a hover tooltip.
export class MergedClassWarning implements m.ClassComponent<MergedClassWarningAttrs> {
  view({attrs}: m.CVnode<MergedClassWarningAttrs>): m.Children {
    return m(
      Tooltip,
      {
        trigger: m(Icon, {
          className: 'pf-merged-class-warning__icon',
          icon: MERGED_CLASS_WARNING_ICON,
          intent: Intent.Warning,
          filled: true,
        }),
        position: attrs.position ?? PopupPosition.Top,
        showArrow: true,
      },
      m(
        '.pf-merged-class-warning__tooltip',
        m('.pf-merged-class-warning__title', MERGED_CLASS_WARNING_TITLE),
        m('.pf-merged-class-warning__desc', MERGED_CLASS_WARNING_DESCRIPTION),
      ),
    );
  }
}

export interface ClassNameWithWarningAttrs {
  // Class name, or content that renders it (e.g. an Anchor).
  readonly name: m.Children;
  // Whether to show the warning icon after the name.
  readonly potentiallyMerged: boolean;
  readonly position?: PopupPosition;
}

// Renders `name`, followed by the warning icon if `potentiallyMerged`.
export class ClassNameWithWarning implements m.ClassComponent<ClassNameWithWarningAttrs> {
  view({attrs}: m.CVnode<ClassNameWithWarningAttrs>): m.Children {
    return m(
      'span.pf-merged-class-warning',
      attrs.name,
      attrs.potentiallyMerged &&
        m(MergedClassWarning, {position: attrs.position}),
    );
  }
}

// Warning callout (icon, title and description) shown at the top of a
// flagged node's flamegraph tooltip.
class MergedClassWarningCallout implements m.ClassComponent {
  view(): m.Children {
    return m(
      '.pf-merged-class-warning-callout',
      m(Icon, {icon: MERGED_CLASS_WARNING_ICON, intent: Intent.Warning}),
      m(
        '.pf-merged-class-warning-callout__body',
        m(
          '.pf-merged-class-warning-callout__title',
          MERGED_CLASS_WARNING_TITLE,
        ),
        m(
          '.pf-merged-class-warning-callout__desc',
          MERGED_CLASS_WARNING_DESCRIPTION,
        ),
      ),
    );
  }
}

// Flamegraph metric hooks below.
// `propertyName` is the column in the flamegraph's SQL query that holds the
// flag (1 = potentially merged). The flamegraph drops extra columns unless
// they're listed in the metric's `unaggregatableProperties`, so make sure it
// is declared in there.

// Hook that tells the flamegraph to draw a warning icon if the node is flagged
// as a merged class.
export function mergedClassNodeIcon(
  propertyName: string,
): TreeExplorerNodeIconFn {
  return (properties) =>
    properties.get(propertyName) === '1'
      ? MERGED_CLASS_WARNING_ICON
      : undefined;
}

// Hook that tells the flamegraph to modify the rendered tooltip with a text
// callout warning explaining that this node has been flagged as a merged
// class.
export function mergedClassNodeTooltipRenderer(
  propertyName: string,
): TreeExplorerNodeTooltipRenderer {
  return ({properties}, defaultContent) => [
    properties.get(propertyName) === '1' && m(MergedClassWarningCallout),
    defaultContent,
  ];
}
