/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import * as m from 'mithril';
import {globals} from './globals';

interface ArgumentPopupArgs {
  onArgumentChange: (arg: string) => void;
  knownArguments: string[];
}

function longestString(array: string[]): string {
  if (array.length === 0) {
    return '';
  }

  let answer = array[0];
  for (let i = 1; i < array.length; i++) {
    if (array[i].length > answer.length) {
      answer = array[i];
    }
  }
  return answer;
}

// Component rendering popup for entering an argument name to use as a pivot.
export class ArgumentPopup implements m.ClassComponent<ArgumentPopupArgs> {
  argument = '';

  setArgument(attrs: ArgumentPopupArgs, arg: string) {
    this.argument = arg;
    attrs.onArgumentChange(arg);
    globals.rafScheduler.scheduleFullRedraw();
  }

  renderMatches(attrs: ArgumentPopupArgs): m.Child[] {
    const result: m.Child[] = [];

    for (const option of attrs.knownArguments) {
      // Would be great to have smarter fuzzy matching, but in the meantime
      // simple substring check should work fine.
      const index = option.indexOf(this.argument);

      if (index === -1) {
        continue;
      }

      if (result.length === 10) {
        break;
      }

      result.push(
          m('div',
            {
              onclick: () => {
                this.setArgument(attrs, option);
              },
            },
            option.substring(0, index),
            // Highlight the matching part with bold font
            m('strong', this.argument),
            option.substring(index + this.argument.length)));
    }

    return result;
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
        m('.arguments-popup-sizer', longestString(attrs.knownArguments)),
        this.renderMatches(attrs));
  }
}
