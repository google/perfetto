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
  RecordingPageController,
} from '../../common/recordingV2/recording_page_controller';
import {EXTENSION_URL} from '../../common/recordingV2/recording_utils';
import {
  CHROME_TARGET_FACTORY,
  ChromeTargetFactory,
} from '../../common/recordingV2/target_factories/chrome_target_factory';
import {
  targetFactoryRegistry,
} from '../../common/recordingV2/target_factory_registry';
import {
  WebsocketMenuController,
} from '../../common/recordingV2/websocket_menu_controller';
import {fullscreenModalContainer, ModalDefinition} from '../modal';
import {CodeSnippet} from '../record_widgets';

import {RecordingMultipleChoice} from './recording_multiple_choice';

const RUN_WEBSOCKET_CMD = '# Get tracebox\n' +
    'curl -LO https://get.perfetto.dev/tracebox\n' +
    'chmod +x ./tracebox\n' +
    '# Option A - trace android devices\n' +
    'adb start-server\n' +
    '# Option B - trace the host OS\n' +
    './tracebox traced --background\n' +
    './tracebox traced_probes --background\n' +
    '# Start the websocket server\n' +
    './tracebox websocket_bridge\n';

export function addNewTarget(recordingPageController: RecordingPageController):
    ModalDefinition {
  const components = [];
  components.push(m('text', 'Select platform:'));

  components.push(assembleWebusbSection(recordingPageController));

  components.push(m('.line'));
  components.push(assembleWebsocketSection(recordingPageController));

  components.push(m('.line'));
  components.push(assembleChromeSection(recordingPageController));

  return {
    title: 'Add new recording target',
    content: m('.record-modal', components),
  };
}

function assembleWebusbSection(
    recordingPageController: RecordingPageController): m.Vnode {
  return m(
      '.record-modal-section',
      m('.logo-wrapping', m('i.material-icons', 'usb')),
      m('.record-modal-description',
        m('h3', 'Android device over WebUSB'),
        m('h4', 'JustWorks from the browser with one click'),
        m('text',
          'Android developers: this option cannot co-operate ' +
              'with the adb host on your machine. Only one entity between ' +
              'the browser and adb can control the USB endpoint. If adb is ' +
              'running, you will be prompted to re-assign the device to the ' +
              'browser. Use the websocket option below to use both ' +
              'simultaneously.'),
        m('.record-modal-button',
          {
            onclick: () => {
              fullscreenModalContainer.close();
              recordingPageController.addAndroidDevice();
            },
          },
          'Connect new WebUSB driver')));
}

function assembleWebsocketSection(
    recordingPageController: RecordingPageController): m.Vnode {
  const websocketComponents = [];
  websocketComponents.push(
      m('h3', 'Android / Linux / MacOS device via Websocket'));
  websocketComponents.push(
      m('text',
        'This option assumes that the adb server is already ' +
            'running on your machine.'),
      m('.record-modal-command', m(CodeSnippet, {
          text: RUN_WEBSOCKET_CMD,
        })));

  websocketComponents.push(m(
      '.record-modal-command',
      m('text', 'Websocket bridge address: '),
      m('input[type=text]', {
        value: websocketMenuController.getPath(),
        oninput() {
          websocketMenuController.setPath(this.value);
        },
      }),
      m('.record-modal-logo-button',
        {
          onclick: () => websocketMenuController.onPathChange(),
        },
        m('i.material-icons', 'refresh')),
      ));

  websocketComponents.push(m(RecordingMultipleChoice, {
    controller: recordingPageController,
    targetFactories: websocketMenuController.getTargetFactories(),
  }));

  return m(
      '.record-modal-section',
      m('.logo-wrapping', m('i.material-icons', 'settings_ethernet')),
      m('.record-modal-description', ...websocketComponents));
}

function assembleChromeSection(
    recordingPageController: RecordingPageController): m.Vnode|undefined {
  if (!targetFactoryRegistry.has(CHROME_TARGET_FACTORY)) {
    return undefined;
  }

  const chromeComponents = [];
  chromeComponents.push(m('h3', 'Chrome Browser instance or ChromeOS device'));

  const chromeFactory: ChromeTargetFactory =
      targetFactoryRegistry.get(CHROME_TARGET_FACTORY) as ChromeTargetFactory;

  if (!chromeFactory.isExtensionInstalled) {
    chromeComponents.push(
        m('text',
          'Install the extension ',
          m('a', {href: EXTENSION_URL, target: '_blank'}, 'from this link '),
          'and refresh the page.'));
  } else {
    chromeComponents.push(m(RecordingMultipleChoice, {
      controller: recordingPageController,
      targetFactories: [chromeFactory],
    }));
  }

  return m(
      '.record-modal-section',
      m('.logo-wrapping', m('i.material-icons', 'web')),
      m('.record-modal-description', ...chromeComponents));
}

const websocketMenuController = new WebsocketMenuController();
