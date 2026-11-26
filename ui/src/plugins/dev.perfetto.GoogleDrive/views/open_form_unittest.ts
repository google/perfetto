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
import {OpenForm, type OpenFormAttrs} from './open_form';

function renderForm(overrides: Partial<OpenFormAttrs> = {}) {
  const container = document.createElement('div');
  const attrs: OpenFormAttrs = {
    link: '',
    onLinkChange: vi.fn(),
    onBrowse: vi.fn(),
    onOpenLink: vi.fn(),
    ...overrides,
  };
  m.render(container, m(OpenForm, attrs));
  return {container, attrs};
}

describe('OpenForm', () => {
  test('invokes onBrowse when clicking Browse Google Drive', () => {
    const {container, attrs} = renderForm();
    const browseBtn = Array.from(container.querySelectorAll('button')).find(
      (b) => b.textContent?.includes('Browse Google Drive'),
    );
    expect(browseBtn).toBeDefined();
    browseBtn!.click();
    expect(attrs.onBrowse).toHaveBeenCalledTimes(1);
  });

  test('disables Open and shows no error when link is empty', () => {
    const {container} = renderForm({link: ''});
    const submitBtn = container.querySelector<HTMLButtonElement>(
      'button[type="submit"]',
    );
    expect(submitBtn?.disabled).toBe(true);
    expect(container.querySelector('.pf-gdrive-open__error')).toBeNull();
  });

  test('shows an error and disables Open for an invalid link', () => {
    const {container} = renderForm({link: 'https://example.com/not-drive'});
    const submitBtn = container.querySelector<HTMLButtonElement>(
      'button[type="submit"]',
    );
    expect(submitBtn?.disabled).toBe(true);
    expect(container.querySelector('.pf-gdrive-open__error')).not.toBeNull();
  });

  test('parses fileId and resourceKey and calls onOpenLink on submit', () => {
    const {container, attrs} = renderForm({
      link: 'https://drive.google.com/file/d/file_123/view?resourcekey=0-key_9',
    });
    const submitBtn = container.querySelector<HTMLButtonElement>(
      'button[type="submit"]',
    );
    expect(submitBtn?.disabled).toBe(false);
    expect(container.querySelector('.pf-gdrive-open__error')).toBeNull();

    const form = container.querySelector('form')!;
    form.dispatchEvent(new Event('submit', {cancelable: true}));
    expect(attrs.onOpenLink).toHaveBeenCalledWith({
      fileId: 'file_123',
      resourceKey: '0-key_9',
    });
  });
});
