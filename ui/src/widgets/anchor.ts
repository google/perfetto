// Copyright (C) 2023 The Android Open Source Project
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
import {HTMLAnchorAttrs} from './common';
import {Icon} from './icon';

interface AnchorAttrs extends HTMLAnchorAttrs {
  // Optional icon to show at the end of the content.
  icon?: string;
}

export class Anchor implements m.ClassComponent<AnchorAttrs> {
  view({attrs, children}: m.CVnode<AnchorAttrs>) {
    const {icon, ...htmlAttrs} = attrs;

    return m('a.pf-anchor', htmlAttrs, children, icon && m(Icon, {icon}));
  }
}

/**
 * Converts a string input in a <span>, extracts URLs and converts them into
 * clickable links.
 * @param text the input string, e.g., "See https://example.org for details".
 * @returns a Mithril vnode, e.g.
 *    <span>See <a href="https://example.org">example.org<a> for more details.
 */
export function linkify(text: string): m.Children {
  const urlPattern = /(https?:\/\/[^\s]+)|(go\/[^\s]+)/g;
  const parts = text.split(urlPattern);
  return m(
    'span',
    parts.map((part) => {
      if (/^(https?:\/\/[^\s]+)$/.test(part)) {
        return m(Anchor, {href: part, target: '_blank'}, part.split('://')[1]);
      } else if (/^(go\/[^\s]+)$/.test(part)) {
        return m(Anchor, {href: `http://${part}`, target: '_blank'}, part);
      } else {
        return part;
      }
    }),
  );
}
