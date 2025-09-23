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
import {classForIntent, HTMLAttrs, Intent} from './common';
import {Button} from './button';
import {Icon} from './icon';
import {classNames} from '../base/classnames';

interface CalloutAttrs extends HTMLAttrs {
  // An icon to show to the left of the callout content.
  readonly icon?: string;

  // Color the callout by specifying an intent.
  readonly intent?: Intent;

  // Adds a close button to the callout.
  readonly dismissible?: boolean;

  // A callback to be invoked when the callout's close button is clicked.
  readonly onDismiss?: () => void;
}

export class Callout implements m.ClassComponent<CalloutAttrs> {
  view({attrs, children}: m.CVnode<CalloutAttrs>) {
    const {
      icon,
      intent = Intent.None,
      className,
      dismissible = false,
      onDismiss,
      ...htmlAttrs
    } = attrs;

    return m(
      '.pf-callout',
      {
        className: classNames(classForIntent(intent), className),
        ...htmlAttrs,
      },
      icon && m(Icon, {className: 'pf-left-icon', icon}),
      m('span.pf-callout__content', children),
      dismissible &&
        m(Button, {
          icon: 'close',
          onclick: onDismiss,
          compact: true,
          title: 'Dismiss callout',
        }),
    );
  }
}
