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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {assetSrc} from '../../base/assets';
import {
  Description,
  DescriptionState,
} from '../../description/description_state';
import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';

function strToReg(str: string): RegExp | string {
  try {
    return eval(str);
  } catch (error) {
    return str;
  }
}

export default class LynxDescriptionPlugin implements PerfettoPlugin {
  static readonly id = 'lynx.Description';
  static async onActivate(_: App): Promise<void> {
    const desc = await this.fetchDescription(
      assetSrc(`assets/description.json`),
    );
    this.addDescription(desc ?? []);
  }

  private static addDescription(desc: Description[]) {
    DescriptionState.edit((draft) => {
      desc.forEach((desc) => {
        let name: string | RegExp = desc.name;
        if (name.startsWith('/')) {
          name = strToReg(name);
        }
        if (name instanceof RegExp) {
          draft.descReg.set(name, desc.description);
        } else {
          draft.descStr.set(name, desc.description);
        }
      });
    });
  }

  private static async fetchDescription(url: string) {
    try {
      const resp = await fetch(url);
      if (resp.status === 200) {
        const desc = await resp.json();
        return desc;
      }
      return [];
    } catch (error) {
      return [];
    }
  }
}
