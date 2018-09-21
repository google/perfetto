// Copyright (C) 2018 The Android Open Source Project
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

import {createPage} from './pages';

const RECORD_COMMAND_LINE =
    'echo CgYIgKAGIAESIwohCgxsaW51eC5mdHJhY2UQAKIGDhIFc2NoZWQSBWlucHV0GJBOMh0KFnBlcmZldHRvLnRyYWNlZF9wcm9iZXMQgCAYBEAASAA= | base64 --decode | adb shell "perfetto -c - -o /data/misc/perfetto-traces/trace" && adb pull /data/misc/perfetto-traces/trace /tmp/trace';

async function copyToClipboard(text: string): Promise<void> {
  try {
    // TODO(hjd): Fix typescript type for navigator.
    // tslint:disable-next-line no-any
    await(navigator as any).clipboard.writeText(text);
  } catch (err) {
    console.error(`Failed to copy "${text}" to clipboard: ${err}`);
  }
}

interface CodeSampleAttrs {
  text: string;
}

class CodeSample implements m.ClassComponent<CodeSampleAttrs> {
  view({attrs}: m.CVnode<CodeSampleAttrs>) {
    return m(
        '.example-code',
        m('code', attrs.text),
        m('button',
          {
            onclick: () => copyToClipboard(attrs.text),
          },
          'Copy to clipboard'), );
  }
}

export const RecordPage = createPage({
  view() {
    return m(
        '.text-column',
        'To collect a 10 second Perfetto trace from an Android phone run this',
        ' command:',
        m(CodeSample, {text: RECORD_COMMAND_LINE}),
        'Then click "Open trace file" in the menu to the left and select',
        ' "/tmp/trace".');
  }
});
