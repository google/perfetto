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

import m from 'mithril';
import {GcsUploader} from '../../../base/gcs_uploader';
import {assertExists} from '../../../base/logging';
import {CopyableLink} from '../../../widgets/copyable_link';
import {showModal} from '../../../widgets/modal';
import {RecordSessionSchema} from '../serialization_schema';

export const SHARE_SUBPAGE = 'share';

export async function shareRecordConfig(config: RecordSessionSchema) {
  const msg =
    'This will generate a publicly-readable link to the ' +
    'current config which cannot be deleted. Continue?';
  if (!confirm(msg)) return;

  const json = JSON.stringify(config);
  const uploader = new GcsUploader(json, {mimeType: 'application/json'});
  await uploader.waitForCompletion();
  const url = uploader.uploadedUrl;
  const hash = assertExists(url.split('/').pop());
  showModal({
    title: 'Permalink',
    content: m(CopyableLink, {
      url: `${self.location.origin}/#!/record/${SHARE_SUBPAGE}/${hash}`,
    }),
  });
}
