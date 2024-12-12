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
import {showModal} from '../../widgets/modal';
import {ArgumentPopup} from '../pivot_table_argument_popup';

export class AttributeModalHolder {
  typedArgument = '';

  callback: (arg: string) => void;

  constructor(callback: (arg: string) => void) {
    this.callback = callback;
  }

  start() {
    showModal({
      title: 'Enter argument name',
      content: () => this.renderModalContents(),
      buttons: [
        {
          text: 'Add',
          action: () => {
            this.callback(this.typedArgument);
            this.typedArgument = '';
          },
        },
      ],
    });
  }

  private renderModalContents() {
    return m(ArgumentPopup, {
      onArgumentChange: (arg) => {
        this.typedArgument = arg;
      },
    });
  }
}
