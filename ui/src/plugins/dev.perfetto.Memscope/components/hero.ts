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
import './hero.scss';
import {Icon as RawIcon, type IconAttrs} from '../../../widgets/icon';

// Hero — a prominent banner row that introduces a page or major section,
// typically pairing a leading Hero.Icon with a block of Hero.Text (heading +
// blurb). Use it at the top of a view to give the user immediate context for
// what they're looking at. Compose via the namespaced slots:
//
//   m(Hero, m(Hero.Icon, {icon: 'memory'}), m(Hero.Text, 'Memory overview'))

export function Hero(): m.Component {
  return {
    view({children}: m.Vnode) {
      return m('.pf-memscope-hero', children);
    },
  };
}

export namespace Hero {
  export const Icon: m.Component<IconAttrs> = {
    view({attrs}: m.Vnode<IconAttrs>) {
      return m(RawIcon, {...attrs, className: 'pf-memscope-hero__icon'});
    },
  };
  export const Text: m.Component = {
    view({children}: m.Vnode) {
      return m('.pf-memscope-hero__text', children);
    },
  };
}
