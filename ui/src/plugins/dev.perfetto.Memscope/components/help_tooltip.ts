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
import {Icons} from '../../../base/semantic_icons';
import {Icon} from '../../../widgets/icon';
import {Tooltip} from '../../../widgets/tooltip';

export interface HelpTooltipAttrs {
  // Content revealed in the tooltip on hover.
  readonly content: m.Children;
}

// A small filled question-mark icon that reveals `content` in a tooltip.
// Used for section/billboard help text so the visible layout stays terse.
export const HelpTooltip: m.Component<HelpTooltipAttrs> = {
  view({attrs}: m.Vnode<HelpTooltipAttrs>) {
    return m(
      Tooltip,
      {
        trigger: m('span.pf-memscope-help', m(Icon, {icon: Icons.Help})),
      },
      attrs.content,
    );
  },
};
