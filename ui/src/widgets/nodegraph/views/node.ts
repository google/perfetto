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
import {Icon} from '../../icon';

/**
 * - Node
 *   - Card
 *     - Header
 *     - Body
 *     - Port 1
 *     - Port 2
 *     - Port N
 *   - Node
 *     - Card
 *       - Header
 *       - Body
 *       - Port N
 */

export interface NGNodeAttrs extends m.Attributes {
  readonly id: string;
  readonly position?: {readonly x: number; readonly y: number};
  // The next docked node in the chain (rendered below the body).
  readonly nextNode?: m.Children;
}

export const NGNode: m.Component<NGNodeAttrs> = {
  view({attrs, children}: m.Vnode<NGNodeAttrs>): m.Children {
    const {id, position, nextNode, ...htmlAttrs} = attrs;

    return m(
      '.pf-ng__node',
      {
        ...htmlAttrs,
        'data-node-id': id,
        'style': {
          ...(position
            ? {
                position: 'absolute',
                left: `${position.x}px`,
                top: `${position.y}px`,
              }
            : {}),
          ...attrs.style,
        },
      },
      children,
      nextNode,
    );
  },
};

export interface NGCardAttrs extends m.Attributes {
  readonly hue?: number;
  readonly accent?: boolean;
  readonly selected?: boolean;
}

// Wraps arbitrary content inside a node body with standard padding.
export const NGCard: m.Component<NGCardAttrs> = {
  view({attrs, children}: m.Vnode<NGCardAttrs>): m.Children {
    const {accent, selected, className, ...htmlAttrs} = attrs;
    return m(
      '.pf-ng__card',
      {
        ...htmlAttrs,
        style: {
          '--pf-ng-hue': attrs.hue ?? 0,
        },
        // Styles actually apply to the card
        className: classNames(
          accent && 'pf-ng__card--accent',
          selected && 'pf-ng__card--selected',
          className,
        ),
      },
      children,
    );
  },
};

export interface NGHeaderAttrs extends m.Attributes {
  readonly title: m.Children;
  readonly icon?: string;
}

export const NGCardHeader: m.Component<NGHeaderAttrs> = {
  view({attrs}: m.Vnode<NGHeaderAttrs>): m.Children {
    const {title, icon, ...htmlAttrs} = attrs;
    return m('.pf-ng__card-header', htmlAttrs, [
      icon !== undefined && m(Icon, {icon, className: 'pf-node-title-icon'}),
      m('.pf-node-title', title),
    ]);
  },
};

export const NGCardBody: m.Component<m.Attributes> = {
  view({attrs, children}: m.Vnode<m.Attributes>): m.Children {
    return m('.pf-ng__card-body', attrs, children);
  },
};

export interface NGPortAttrs extends m.Attributes {
  // Identifier used as a Mithril key when rendering port lists.
  readonly id: string;
  readonly direction: 'north' | 'south' | 'east' | 'west';
  readonly portType: 'input' | 'output';
  readonly connected?: boolean;
  // Label shown alongside the port dot (hidden on north/south ports).
  readonly label?: m.Children;
}

export const NGPort: m.Component<NGPortAttrs> = {
  view({attrs}: m.Vnode<NGPortAttrs>) {
    // Destructure our own attrs; pass the rest (e.g. onpointerdown) to the dot.
    const {direction, connected, label, portType, ...dotAttrs} = attrs;

    const portDot = m('.pf-ng__port-dot', {
      ...dotAttrs,
      'data-port-id': attrs.id,
      'className': classNames(
        portType === 'input' ? 'pf-input' : 'pf-output',
        `pf-port-${direction}`,
        connected && 'pf-port--connected',
      ),
    });

    // Left/right ports get a labelled row; top/bottom are bare dots.
    if (direction === 'east' || direction === 'west') {
      return m(`.pf-ng__port.pf-ng__port--${direction}`, [portDot, label]);
    } else {
      return portDot;
    }
  },
};
