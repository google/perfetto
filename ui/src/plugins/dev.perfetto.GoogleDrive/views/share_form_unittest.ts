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
import {ShareForm, type ShareState} from './share_form';

function renderForm(stateOverrides: Partial<ShareState> = {}): HTMLElement {
  const container = document.createElement('div');
  const state: ShareState = {
    fileName: 'trace.pftrace',
    location: 'root',
    shareWithOrgOrAnyone: true,
    fileId: undefined,
    resourceKey: undefined,
    upload: undefined,
    error: undefined,
    ...stateOverrides,
  };
  m.render(
    container,
    m(ShareForm, {
      state,
      onFileNameChange: vi.fn(),
      onUpload: vi.fn(),
      onCancelUpload: vi.fn(),
      onPickFolder: vi.fn(),
      onShareWithOrgOrAnyoneChange: vi.fn(),
      onOpenSharingDialog: vi.fn(),
    }),
  );
  return container;
}

describe('ShareForm', () => {
  test('renders the sharing checkbox checked by default before upload', () => {
    const container = renderForm();
    const checkbox = container.querySelector<HTMLInputElement>(
      'input[type="checkbox"]',
    );
    expect(checkbox?.checked).toBe(true);
  });

  test('renders sharing disclaimer and copy-link explanation once uploaded', () => {
    const container = renderForm({fileId: 'file123'});
    const note = container.querySelector('.pf-gdrive-share__note');
    expect(note).not.toBeNull();
    expect(note?.textContent).toContain('share it with them first');
    expect(note?.textContent).toContain('appear in search results');
    expect(note?.textContent).toContain('Perfetto link');
    expect(note?.textContent).toContain('Google Drive link');
  });
});
