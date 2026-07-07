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

import './callout.scss';
import m from 'mithril';
import {Button} from '../../../widgets/button';
import {Icon} from '../../../widgets/icon';
import {classNames} from '../../../base/classnames';
import {classForIntent, type HTMLAttrs, Intent} from '../../../widgets/common';

// Default leading icon per intent. The shared Callout widget renders an icon
// only when one is explicitly supplied; this Memscope variant additionally
// falls back to an intent-specific default so callers can omit `icon` and
// still get a sensible glyph.
const DEFAULT_ICONS: Record<Intent, string | undefined> = {
  [Intent.None]: undefined,
  [Intent.Primary]: 'lightbulb',
  [Intent.Success]: 'check_circle',
  [Intent.Warning]: 'info',
  [Intent.Danger]: 'error',
};

interface CalloutAttrs extends HTMLAttrs {
  // An icon to show to the left of the callout content. Falls back to a
  // default glyph based on `intent` when omitted.
  readonly icon?: string;

  // Color the callout by specifying an intent. Uses the same palette as the
  // shared `widgets/callout` (pf-color-primary/success/warning/danger).
  readonly intent?: Intent;

  // Adds a close button to the callout.
  readonly dismissible?: boolean;

  // A callback to be invoked when the callout's close button is clicked.
  readonly onDismiss?: () => void;
}

// Callout — a compact strip pairing a leading icon with a one-line message,
// tinted by intent. Use it to surface the single most relevant takeaway or
// caveat next to a panel's figures (a hint, a success confirmation, a warning).
// Mirrors the interface and colour treatment of the shared `widgets/callout`
// Callout, with a Memscope-specific geometry and intent-based default icons.
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
    const resolvedIcon = icon ?? DEFAULT_ICONS[intent];

    return m(
      '.pf-memscope-callout',
      {
        className: classNames(classForIntent(intent), className),
        ...htmlAttrs,
      },
      resolvedIcon && m(Icon, {className: 'pf-left-icon', icon: resolvedIcon}),
      m('span.pf-memscope-callout__content', children),
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
