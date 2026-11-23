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

// Component harness fixtures for ShareForm, the content of the "Share to
// Google Drive" dialog. Only bundled into the component harness
// (ui/build --enable-component-harness), never shipped.

import m from 'mithril';
import {
  type FixtureArgs,
  type FixtureModule,
  record,
} from '../../../component_harness/registry';
import {ShareForm, type ShareState} from './share_form';

export default {
  // Nothing uploaded yet.
  empty: (args) => shareForm(args, {}),

  // An upload that has started but not yet reported any progress.
  connecting: (args) =>
    shareForm(args, {
      upload: {
        controller: new AbortController(),
        uploadedBytes: 0,
        totalBytes: 0,
      },
    }),

  // An upload in progress, 41% done.
  uploading: (args) =>
    shareForm(args, {
      upload: {
        controller: new AbortController(),
        uploadedBytes: 12_300_000,
        totalBytes: 29_400_000,
      },
    }),

  // The trace is already in Drive.
  uploaded: (args) =>
    shareForm(args, {
      fileId: 'fake-file-id',
      permissions: [
        {
          id: '1',
          type: 'user',
          role: 'owner',
          displayName: 'Alice Smith',
          emailAddress: 'alice@example.com',
        },
        {
          id: '2',
          type: 'domain',
          role: 'reader',
          domain: 'example.com',
          allowFileDiscovery: true,
        },
      ],
    }),

  // Uploaded but not shared with anyone else yet.
  private: (args) =>
    shareForm(args, {
      fileId: 'fake-file-id',
      permissions: [
        {
          id: '1',
          type: 'user',
          role: 'owner',
          displayName: 'Alice Smith',
          emailAddress: 'alice@example.com',
        },
      ],
    }),

  // Uploaded and shared with a domain without searchability enabled.
  unsearchable: (args) =>
    shareForm(args, {
      fileId: 'fake-file-id',
      permissions: [
        {
          id: '1',
          type: 'user',
          role: 'owner',
          displayName: 'Alice Smith',
          emailAddress: 'alice@example.com',
        },
        {
          id: '2',
          type: 'domain',
          role: 'reader',
          domain: '00gjdgxs4cdk4oe.audience.googledomains.com',
          displayName: 'Google',
          allowFileDiscovery: false,
        },
      ],
    }),

  // The last upload failed.
  error: (args) =>
    shareForm(args, {
      error: {
        message: 'Upload failed',
        type: 'FakeError',
        cause: new Error('Something went wrong'),
      },
    }),
} satisfies FixtureModule;

// Renders ShareForm on its own, without the dialog around it, starting from
// `initial`. Fully stubbed: every callback only updates the local state and
// records an event, so nothing talks to Google. Clicks on links (e.g. "Open in
// Drive") are swallowed and recorded as {openUrl} instead of navigating away.
//
// Args, for tweaking any of the states above:
//   fileName=<name>    file name (default 'trace.perfetto-trace')
//   folder=<name>      upload location folder name (default: My Drive)
function shareForm(
  args: FixtureArgs,
  initial: Partial<ShareState>,
): m.Component {
  const share: ShareState = {
    fileName: args.fileName ?? 'trace.perfetto-trace',
    location:
      args.folder !== undefined
        ? {id: 'fake-folder-id', name: args.folder}
        : 'root',
    defaultShareDomains: [],
    applyDefaultShareDomains: false,
    fileId: undefined,
    resourceKey: undefined,
    upload: undefined,
    permissions: undefined,
    loadingPermissions: false,
    error: undefined,
    ...initial,
  };
  return {
    view: () =>
      m(
        'div',
        {
          style: {maxWidth: '480px'},
          onclick: (e: MouseEvent) => {
            const target = e.target as Element | null;
            const link = target?.closest('a[href]');
            if (link === null || link === undefined) return;
            e.preventDefault();
            record({openUrl: link.getAttribute('href')});
          },
        },
        m(ShareForm, {
          state: share,
          onFileNameChange: (fileName: string) => {
            share.fileName = fileName;
            record({fileName});
          },
          onUpload: () => {
            share.error = undefined;
            share.upload = {
              controller: new AbortController(),
              uploadedBytes: 0,
              totalBytes: 0,
            };
            record('upload');
          },
          onCancelUpload: () => {
            share.upload = undefined;
            record('cancelUpload');
          },
          onPickFolder: () => record('pickFolder'),
          onResetLocation: () => {
            share.location = 'root';
            record('resetLocation');
          },
          onApplyDefaultShareDomainsChange: (apply) => {
            share.applyDefaultShareDomains = apply;
            record({applyDefaultShareDomains: apply});
          },
          onOpenSharingDialog: () => record('openSharingDialog'),
          onRefreshPermissions: () => record('refreshPermissions'),
          onMakeSearchable: () => record('makeSearchable'),
        }),
      ),
  };
}
