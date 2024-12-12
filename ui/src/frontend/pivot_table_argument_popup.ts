// Copyright (C) 2022 The Android Open Source Project
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
import {raf} from '../core/raf_scheduler';

interface ArgumentPopupArgs {
  onArgumentChange: (arg: string) => void;
}

// Component rendering popup for entering an argument name to use as a pivot.
export class ArgumentPopup implements m.ClassComponent<ArgumentPopupArgs> {
  argument = '';

  setArgument(attrs: ArgumentPopupArgs, arg: string) {
    this.argument = arg;
    attrs.onArgumentChange(arg);
    raf.scheduleFullRedraw();
  }

  view({attrs}: m.Vnode<ArgumentPopupArgs>): m.Child {
    return m(
      '.name-completion',
      m('input', {
        oncreate: (vnode: m.VnodeDOM) =>
          (vnode.dom as HTMLInputElement).focus(),
        oninput: (e: Event) => {
          const input = e.target as HTMLInputElement;
          this.setArgument(attrs, input.value);
        },
        value: this.argument,
      }),
    );
  }
}
