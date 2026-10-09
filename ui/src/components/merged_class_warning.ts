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

// Warning shown next to Java class names in heap dumps when the class may
// have been produced by merging several source classes during app
// optimization (e.g. R8 class merging). In that case the (deobfuscated) name
// and the attributed memory may cover more than the class shown.

import './merged_class_warning.scss';
import m from 'mithril';
import {Icon} from '../widgets/icon';
import {Intent} from '../widgets/common';
import {PopupPosition} from '../widgets/popup';
import {Tooltip} from '../widgets/tooltip';
import type {
  TreeExplorerMarkerTone,
  TreeExplorerOptionalMarker,
} from '../widgets/tree_explorer';

export const MERGED_CLASS_WARNING_ICON = 'warning';
export const MERGED_CLASS_WARNING_TITLE = 'Possibly merged class';
export const MERGED_CLASS_WARNING_DESCRIPTION =
  'This class may include code from several source classes that were ' +
  'combined during app optimization (e.g. R8 class merging). Its name and ' +
  'memory may cover more than the class shown.';

// 'warning' = amber icon, 'muted' = gray icon.
export type MergedClassWarningTone = TreeExplorerMarkerTone;

export interface MergedClassWarningAttrs {
  readonly position?: PopupPosition;
  // Defaults to 'warning'.
  readonly tone?: MergedClassWarningTone;
}

// Small warning icon with an explanatory tooltip on hover.
export class MergedClassWarning implements m.ClassComponent<MergedClassWarningAttrs> {
  view({attrs}: m.CVnode<MergedClassWarningAttrs>): m.Children {
    const muted = attrs.tone === 'muted';
    return m(
      Tooltip,
      {
        trigger: m(Icon, {
          className: muted
            ? 'pf-merged-class-warning__icon pf-merged-class-warning__icon--muted'
            : 'pf-merged-class-warning__icon',
          icon: MERGED_CLASS_WARNING_ICON,
          intent: muted ? Intent.None : Intent.Warning,
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
  // Class name, or any content that renders it (e.g. an Anchor).
  readonly name: m.Children;
  readonly potentiallyMerged: boolean;
  readonly position?: PopupPosition;
  readonly tone?: MergedClassWarningTone;
}

// Renders `name` followed by a MergedClassWarning when flagged.
export class ClassNameWithWarning implements m.ClassComponent<ClassNameWithWarningAttrs> {
  view({attrs}: m.CVnode<ClassNameWithWarningAttrs>): m.Children {
    return m(
      'span.pf-merged-class-warning',
      attrs.name,
      attrs.potentiallyMerged &&
        m(MergedClassWarning, {position: attrs.position, tone: attrs.tone}),
    );
  }
}

// Flamegraph marker for nodes whose `propertyName` property is '1'. The
// property must be declared in the metric's unaggregatableProperties.
export function mergedClassFlamegraphMarker(
  propertyName: string,
  tone?: MergedClassWarningTone,
): TreeExplorerOptionalMarker {
  return {
    name: MERGED_CLASS_WARNING_TITLE,
    icon: MERGED_CLASS_WARNING_ICON,
    description: MERGED_CLASS_WARNING_DESCRIPTION,
    tone,
    isVisible: (properties) => properties.get(propertyName) === '1',
  };
}
