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
import {classNames} from '../../../base/classnames';
import {Button, type ButtonAttrs} from '../../../widgets/button';
import {Icon} from '../../../widgets/icon';
import './row.scss';

export function Row(): m.Component<m.Attributes> {
  return {
    view({attrs, children}) {
      return m('.pf-spag-row', attrs, children);
    },
  };
}

export namespace Row {
  export const DragHandle: m.Component<m.Attributes> = {
    view({attrs}) {
      const {className, ...rest} = attrs;
      return m(Icon, {
        ...rest,
        icon: 'drag_indicator',
        className: classNames('pf-spag-draghandle', className),
      });
    },
  };

  export const DeleteButton: m.Component<Partial<ButtonAttrs>> = {
    view({attrs}) {
      const {
        icon = 'close',
        tooltip = 'Remove',
        compact = true,
        className,
        ...rest
      } = attrs;
      return m(Button, {
        className: classNames('pf-spag-delete', className),
        icon,
        tooltip,
        compact,
        ...rest,
      });
    },
  };
}
