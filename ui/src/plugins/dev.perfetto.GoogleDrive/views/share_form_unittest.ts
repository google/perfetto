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

function renderForm(
  stateOverrides: Partial<ShareState> = {},
  onRefreshPermissions = vi.fn(),
  onMakeSearchable = vi.fn(),
): HTMLElement {
  const container = document.createElement('div');
  const state: ShareState = {
    fileName: 'trace.pftrace',
    location: 'root',
    defaultShareDomains: [],
    applyDefaultShareDomains: false,
    fileId: undefined,
    resourceKey: undefined,
    upload: undefined,
    permissions: undefined,
    loadingPermissions: false,
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
      onResetLocation: vi.fn(),
      onApplyDefaultShareDomainsChange: vi.fn(),
      onOpenSharingDialog: vi.fn(),
      onRefreshPermissions,
      onMakeSearchable,
    }),
  );
  return container;
}

describe('ShareForm', () => {
  test('renders loaded permissions and triggers onRefreshPermissions on click', () => {
    const onRefresh = vi.fn();
    const container = renderForm(
      {
        fileId: 'file123',
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
            allowFileDiscovery: true,
          },
          {
            id: '3',
            type: 'anyone',
            role: 'reader',
            allowFileDiscovery: false,
          },
        ],
      },
      onRefresh,
    );

    const items = Array.from(
      container.querySelectorAll('.pf-gdrive-share__permission-item'),
    ).map((el) => el.textContent);
    expect(items).toHaveLength(3);
    expect(items[0]).toContain('Alice Smith');
    expect(items[0]).toContain('alice@example.com');
    expect(items[0]).toContain('Owner');
    expect(items[1]).toContain('Google');
    expect(items[1]).toContain('Searchable');
    expect(items[1]).toContain('Viewer');
    expect(items[2]).toContain('Anyone on the internet');
    expect(items[2]).toContain('With link only');

    const refreshBtn = container.querySelector<HTMLButtonElement>(
      'button[title*="Refresh"]',
    );
    expect(refreshBtn).not.toBeNull();
    refreshBtn!.click();
    expect(onRefresh).toHaveBeenCalledTimes(1);
  });

  test('shows warning when file is not shared with anyone else', () => {
    const container = renderForm({
      fileId: 'file123',
      permissions: [
        {
          id: '1',
          type: 'user',
          role: 'owner',
          displayName: 'Alice Smith',
          emailAddress: 'alice@example.com',
        },
      ],
    });
    const warning = container.querySelector(
      '.pf-gdrive-share__private-warning',
    );
    expect(warning).not.toBeNull();
    expect(warning?.textContent).toContain('Only you can access this trace');
  });

  test('shows warning and Make searchable button when a domain or anyone share is not searchable', () => {
    const onMakeSearchable = vi.fn();
    const withUnsearchable = renderForm(
      {
        fileId: 'file123',
        permissions: [
          {
            id: '1',
            type: 'domain',
            role: 'reader',
            displayName: 'Google',
            allowFileDiscovery: false,
          },
        ],
      },
      vi.fn(),
      onMakeSearchable,
    );
    const warning = withUnsearchable.querySelector(
      '.pf-gdrive-share__unsearchable-warning',
    );
    expect(warning).not.toBeNull();
    const makeSearchableBtn = warning?.querySelector('button');
    expect(makeSearchableBtn?.textContent).toContain('Make searchable');
    makeSearchableBtn!.click();
    expect(onMakeSearchable).toHaveBeenCalledTimes(1);

    const withSearchable = renderForm({
      fileId: 'file123',
      permissions: [
        {
          id: '1',
          type: 'domain',
          role: 'reader',
          displayName: 'Google',
          allowFileDiscovery: true,
        },
      ],
    });
    expect(
      withSearchable.querySelector('.pf-gdrive-share__unsearchable-warning'),
    ).toBeNull();
  });

  test('shows warning without Make searchable button for anyone link-only share', () => {
    const withAnyone = renderForm({
      fileId: 'file123',
      permissions: [
        {
          id: '1',
          type: 'anyone',
          role: 'reader',
          allowFileDiscovery: false,
        },
      ],
    });
    const warning = withAnyone.querySelector(
      '.pf-gdrive-share__unsearchable-warning',
    );
    expect(warning).not.toBeNull();
    expect(warning?.querySelector('button')).toBeNull();
  });

  test('renders remembered domain share checkbox on upload form when present', () => {
    const withoutDefaults = renderForm();
    expect(withoutDefaults.querySelector('input[type="checkbox"]')).toBeNull();

    const withDefaults = renderForm({
      defaultShareDomains: [
        {
          domain: '00gjdgxs4cdk4oe.audience.googledomains.com',
          displayName: 'Google',
          role: 'reader',
        },
      ],
      applyDefaultShareDomains: true,
    });
    const checkbox = withDefaults.querySelector<HTMLInputElement>(
      'input[type="checkbox"]',
    );
    expect(checkbox?.checked).toBe(true);
    expect(withDefaults.textContent).toContain('Also share with Google');
  });
});
