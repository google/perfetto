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

import * as m from 'mithril';

import {
  RecordingTargetV2,
  TargetFactory,
} from '../../common/recordingV2/recording_interfaces_v2';
import {
  RecordingPageController,
} from '../../common/recordingV2/recording_page_controller';
import {fullscreenModalContainer} from '../modal';

interface RecordingMultipleChoiceAttrs {
  targetFactories: TargetFactory[];
  // Reference to the controller which maintains the state of the recording
  // page.
  controller: RecordingPageController;
}

export class RecordingMultipleChoice implements
    m.ClassComponent<RecordingMultipleChoiceAttrs> {
  private selectedIndex: number = -1;

  targetSelection(
      targets: RecordingTargetV2[],
      controller: RecordingPageController): m.Vnode|undefined {
    const targetInfo = controller.getTargetInfo();
    const targetNames = [];
    this.selectedIndex = -1;
    for (let i = 0; i < targets.length; i++) {
      const targetName = targets[i].getInfo().name;
      targetNames.push(m('option', targetName));
      if (targetInfo && targetName === targetInfo.name) {
        this.selectedIndex = i;
      }
    }

    const selectedIndex = this.selectedIndex;
    return m(
        'label',
        m('select',
          {
            selectedIndex,
            onchange: (e: Event) => {
              controller.onTargetSelection(
                  (e.target as HTMLSelectElement).value);
            },
            onupdate: (select) => {
              // Work around mithril bug
              // (https://github.com/MithrilJS/mithril.js/issues/2107): We
              // may update the select's options while also changing the
              // selectedIndex at the same time. The update of selectedIndex
              // may be applied before the new options are added to the
              // select element. Because the new selectedIndex may be
              // outside of the select's options at that time, we have to
              // reselect the correct index here after any new children were
              // added.
              (select.dom as HTMLSelectElement).selectedIndex =
                  this.selectedIndex;
            },
            ...{size: targets.length, multiple: 'multiple'},
          },
          ...targetNames),
    );
  }

  view({attrs}: m.CVnode<RecordingMultipleChoiceAttrs>): m.Vnode[]|undefined {
    const controller = attrs.controller;
    if (!controller.shouldShowTargetSelection()) {
      return undefined;
    }
    const targets: RecordingTargetV2[] = [];
    for (const targetFactory of attrs.targetFactories) {
      for (const target of targetFactory.listTargets()) {
        targets.push(target);
      }
    }
    if (targets.length === 0) {
      return undefined;
    }

    return [
      m('text', 'Select target:'),
      m('.record-modal-command',
        this.targetSelection(targets, controller),
        m('button.record-modal-button-high',
          {
            disabled: this.selectedIndex === -1,
            onclick: () => {
              fullscreenModalContainer.close();
              controller.onStartRecordingPressed();
            },
          },
          'Connect')),
    ];
  }
}
