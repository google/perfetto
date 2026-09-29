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

import './kpi_card.scss';
import m from 'mithril';
import {classNames} from '../base/classnames';
import type {HTMLAttrs} from './common';

export interface KpiCardAttrs extends HTMLAttrs {
  readonly value: m.Children;
  readonly label: m.Children;
  readonly dotColor?: string;
  readonly swatchColor?: string;
  readonly wide?: boolean;
  readonly active?: boolean;
  readonly interactive?: boolean;
}

export class KpiCard implements m.ClassComponent<KpiCardAttrs> {
  view({attrs}: m.CVnode<KpiCardAttrs>): m.Children {
    const {
      value,
      label,
      dotColor,
      swatchColor,
      wide,
      active,
      interactive,
      className,
      ...htmlAttrs
    } = attrs;

    const isInteractive =
      Boolean(interactive) || htmlAttrs.onclick !== undefined;

    return m(
      '.pf-kpi-card',
      {
        ...htmlAttrs,
        className: classNames(
          wide && 'pf-kpi-card--wide',
          active && 'pf-kpi-card--active',
          isInteractive && 'pf-kpi-card--interactive',
          className,
        ),
      },
      m('b.pf-kpi-card__value', value),
      m(
        'span.pf-kpi-card__caption',
        dotColor !== undefined &&
          dotColor !== '' &&
          m('i.pf-kpi-card__dot', {style: {background: dotColor}}),
        swatchColor !== undefined &&
          swatchColor !== '' &&
          m('i.pf-kpi-card__swatch', {style: {background: swatchColor}}),
        label,
      ),
    );
  }
}

export class KpiCardGroup implements m.ClassComponent<HTMLAttrs> {
  view({attrs, children}: m.CVnode<HTMLAttrs>): m.Children {
    return m('.pf-kpi-card-group', attrs, children);
  }
}
