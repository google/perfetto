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
import {Icons} from '../../../base/semantic_icons';
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Checkbox} from '../../../widgets/checkbox';
import {Intent} from '../../../widgets/common';
import {CopyToClipboardButton} from '../../../widgets/copy_to_clipboard_button';
import {Form, FormLabel} from '../../../widgets/form';
import {Icon} from '../../../widgets/icon';
import {Inline, Stack, StackAuto} from '../../../widgets/stack';
import {LinearProgress} from '../../../widgets/linear_progress';
import {TextInput} from '../../../widgets/text_input';
import {renderErrorDetails} from '../dialogs';
import {type DriveFilePermission, gdriveOpenLink} from '../google/drive_client';
import './share_form.scss';

// Everything the "Share to Google Drive" dialog shows for a trace.
//
// This is owned by the plugin, per trace, rather than by the dialog. The
// dialog can be closed (with Escape or its close button) while an upload is
// still running, and reopening it must show that upload, not a blank form.
export interface UploadFolder {
  readonly id: string;
  readonly name?: string;
}

export interface SavedDomainShare {
  readonly domain: string;
  readonly displayName?: string;
  readonly role: string;
}

export interface ShareState {
  // The name to upload the trace as.
  fileName: string;
  // Where to upload to: a Drive folder, or 'root' for "My Drive".
  location: UploadFolder | 'root';
  // Domain / target-audience permissions remembered from the last share.
  defaultShareDomains: ReadonlyArray<SavedDomainShare>;
  // Whether to apply `defaultShareDomains` automatically after uploading.
  applyDefaultShareDomains: boolean;
  // The Drive file ID, once the trace is in Drive: either because it was
  // uploaded, or because it was opened from Drive in the first place.
  fileId: string | undefined;
  // The file's resource key, if it has one and it's known. Links to the file
  // include it, so people without direct access to the file can open it.
  resourceKey: string | undefined;
  // The upload in progress, if there is one.
  upload: UploadState | undefined;
  // Permissions currently on the Drive file, once loaded, or 'access_denied'
  // if the user's role does not allow viewing sharing settings.
  permissions: ReadonlyArray<DriveFilePermission> | 'access_denied' | undefined;
  // Whether permissions are currently being fetched.
  loadingPermissions: boolean;
  // What went wrong with the last action, if anything. Cleared when the next
  // action starts.
  error: ShareError | undefined;
}

export interface ShareError {
  // What to tell the user.
  readonly message: string;
  // The underlying error, shown under "Error details".
  readonly type: string;
  readonly cause?: unknown;
}

export interface UploadState {
  // Abort this to cancel the upload.
  readonly controller: AbortController;
  // How far the upload has got. Both are 0 until the first progress report
  // arrives, e.g. while connecting to Google.
  uploadedBytes: number;
  totalBytes: number;
}

export interface ShareFormAttrs {
  readonly state: Readonly<ShareState>;
  readonly onFileNameChange: (fileName: string) => void;
  // Starts uploading the trace.
  readonly onUpload: () => void;
  // Cancels the upload in progress.
  readonly onCancelUpload: () => void;
  // Opens the folder picker to change the location.
  readonly onPickFolder: () => void;
  // Sets the location back to the top of My Drive, which the folder picker
  // can't select.
  readonly onResetLocation: () => void;
  // Toggles applying remembered domain shares after upload.
  readonly onApplyDefaultShareDomainsChange: (apply: boolean) => void;
  // Opens the Google Drive sharing dialog for the uploaded file.
  readonly onOpenSharingDialog: () => void;
  // Refreshes the file's sharing permissions from Google Drive.
  readonly onRefreshPermissions: () => void;
  // Enables search discovery on link-only domain/anyone permissions.
  readonly onMakeSearchable: () => void;
}

// The body of the "Share to Google Drive" dialog. It holds no state of its
// own: it renders the ShareState it is given and reports what the user does
// through the callbacks. It is built from the standard widgets so that it
// looks like the rest of the UI.
export const ShareForm: m.Component<ShareFormAttrs> = {
  view({attrs}) {
    const {fileId} = attrs.state;
    return fileId === undefined || fileId === ''
      ? renderUploadForm(attrs)
      : renderUploaded(attrs, fileId);
  },
};

function renderError(state: Readonly<ShareState>): m.Children {
  if (state.error === undefined) return undefined;
  return m('.pf-gdrive-share__error', [
    m(Icon, {
      icon: 'error',
      className: 'pf-gdrive-share__error-icon',
    }),
    m('.pf-gdrive-share__error-body', [
      m('.pf-gdrive-share__error-message', state.error.message),
      renderErrorDetails(state.error),
    ]),
  ]);
}

// "Uploading… 42% (12.3 of 29.4 MB)", or just "Uploading…" until the first
// progress report arrives.
function progressText(upload: UploadState): string {
  const {uploadedBytes, totalBytes} = upload;
  if (totalBytes === 0) return 'Uploading…';
  const percent = Math.floor((100 * uploadedBytes) / totalBytes);
  const mb = (bytes: number) => (bytes / 1e6).toFixed(1);
  return `Uploading… ${percent}% (${mb(uploadedBytes)} of ${mb(totalBytes)} MB)`;
}

// The trace is already in Drive: offer ways to get at it.
function renderUploaded(attrs: ShareFormAttrs, fileId: string): m.Children {
  const {fileName, permissions} = attrs.state;
  const isPrivate =
    Array.isArray(permissions) &&
    permissions.length > 0 &&
    permissions.every((p) => p.role === 'owner');

  return m(Stack, {className: 'pf-gdrive-share', spacing: 'large'}, [
    renderError(attrs.state),
    m('.pf-gdrive-share__saved-header', [
      m('.pf-gdrive-share__saved', [
        m(
          '.pf-gdrive-share__saved-badge',
          m(Icon, {
            className: 'pf-gdrive-share__saved-icon',
            icon: 'cloud_done',
            filled: true,
          }),
        ),
        m('.pf-gdrive-share__saved-text', [
          m(
            '.pf-gdrive-share__saved-title',
            fileName !== '' ? fileName : 'Saved to Google Drive',
          ),
          fileName !== '' &&
            m('.pf-gdrive-share__saved-subtitle', 'Saved to Google Drive'),
        ]),
      ]),
      m(
        Anchor,
        {
          className: 'pf-gdrive-share__open-drive',
          href: `https://docs.google.com/file/d/${fileId}/view`,
          target: '_blank',
          rel: 'noopener',
          icon: Icons.ExternalLink,
        },
        'Open in Drive',
      ),
    ]),
    renderPermissions(attrs),
    m('.pf-gdrive-share__actions-row', [
      m(Button, {
        label: isPrivate ? 'Share…' : 'Edit sharing',
        icon: 'share',
        title: 'Choose who can open this file in Google Drive',
        variant: isPrivate ? ButtonVariant.Filled : ButtonVariant.Outlined,
        intent: isPrivate ? Intent.Primary : Intent.None,
        onclick: () => attrs.onOpenSharingDialog(),
      }),
      m(CopyToClipboardButton, {
        label: 'Copy Perfetto link',
        title: 'Copy a link that opens this trace directly in Perfetto',
        textToCopy: gdriveOpenLink(
          window.location.origin,
          fileId,
          attrs.state.resourceKey,
        ),
        variant: isPrivate ? ButtonVariant.Outlined : ButtonVariant.Filled,
        intent: isPrivate ? Intent.None : Intent.Primary,
      }),
    ]),
  ]);
}

function permissionIcon(type: DriveFilePermission['type']): string {
  switch (type) {
    case 'user':
      return 'person';
    case 'group':
      return 'group';
    case 'domain':
      return 'domain';
    case 'anyone':
      return 'public';
  }
}

function permissionPrimaryText(perm: DriveFilePermission): string {
  switch (perm.type) {
    case 'user':
    case 'group':
      return perm.displayName ?? perm.emailAddress ?? perm.type;
    case 'domain':
      return perm.displayName ?? perm.domain ?? 'Organization';
    case 'anyone':
      return 'Anyone on the internet';
  }
}

function permissionSecondaryText(
  perm: DriveFilePermission,
): string | undefined {
  switch (perm.type) {
    case 'user':
    case 'group':
      return perm.displayName && perm.emailAddress
        ? perm.emailAddress
        : undefined;
    case 'domain':
    case 'anyone':
      return perm.allowFileDiscovery ? 'Searchable' : 'With link only';
  }
}

function permissionRoleLabel(role: string): string {
  switch (role) {
    case 'owner':
      return 'Owner';
    case 'organizer':
      return 'Manager';
    case 'fileOrganizer':
      return 'Content manager';
    case 'writer':
      return 'Editor';
    case 'commenter':
      return 'Commenter';
    case 'reader':
      return 'Viewer';
    default:
      return role;
  }
}

function renderPermissionsWarning(attrs: ShareFormAttrs): m.Children {
  const {permissions, loadingPermissions} = attrs.state;
  if (
    permissions === undefined ||
    permissions === 'access_denied' ||
    permissions.length === 0
  ) {
    return undefined;
  }

  const isPrivate = permissions.every((p) => p.role === 'owner');
  if (isPrivate) {
    return m(
      '.pf-gdrive-share__permissions-warning.pf-gdrive-share__private-warning',
      [
        m(Icon, {
          icon: 'warning',
          className: 'pf-gdrive-share__warning-icon',
        }),
        m(
          'p.pf-gdrive-share__warning-text',
          'Only you can access this trace. Recipients won\u2019t be able to open the Perfetto link until you share it.',
        ),
      ],
    );
  }

  const hasUnsearchableDomain = permissions.some(
    (p) => p.type === 'domain' && p.allowFileDiscovery !== true,
  );
  if (hasUnsearchableDomain) {
    return m(
      '.pf-gdrive-share__permissions-warning.pf-gdrive-share__unsearchable-warning',
      [
        m(Icon, {
          icon: 'warning',
          className: 'pf-gdrive-share__warning-icon',
        }),
        m('.pf-gdrive-share__warning-body', [
          m(
            'p.pf-gdrive-share__warning-text',
            'Recipients may not be able to open this Perfetto link unless they open the file in Drive first or you make it searchable.',
          ),
          m(Button, {
            className: 'pf-gdrive-share__warning-action',
            label: 'Make searchable',
            icon: 'search',
            variant: ButtonVariant.Outlined,
            loading: loadingPermissions,
            disabled: loadingPermissions,
            onclick: () => attrs.onMakeSearchable(),
          }),
        ]),
      ],
    );
  }

  const hasUnsearchableAnyone = permissions.some(
    (p) => p.type === 'anyone' && p.allowFileDiscovery !== true,
  );
  if (!hasUnsearchableAnyone) return undefined;
  return m(
    '.pf-gdrive-share__permissions-warning.pf-gdrive-share__unsearchable-warning',
    [
      m(Icon, {
        icon: 'warning',
        className: 'pf-gdrive-share__warning-icon',
      }),
      m(
        'p.pf-gdrive-share__warning-text',
        'Recipients may not be able to open this Perfetto link unless they open the file in Drive first.',
      ),
    ],
  );
}

function renderPermissionItem(perm: DriveFilePermission): m.Children {
  const secondary = permissionSecondaryText(perm);
  return m('li.pf-gdrive-share__permission-item', {key: perm.id}, [
    m(
      '.pf-gdrive-share__permission-avatar',
      m(Icon, {
        icon: permissionIcon(perm.type),
        className: 'pf-gdrive-share__permission-icon',
      }),
    ),
    m('.pf-gdrive-share__permission-info', [
      m('span.pf-gdrive-share__permission-label', permissionPrimaryText(perm)),
      secondary !== undefined &&
        m('span.pf-gdrive-share__permission-sublabel', secondary),
    ]),
    m('span.pf-gdrive-share__permission-role', permissionRoleLabel(perm.role)),
  ]);
}

function renderPermissions(attrs: ShareFormAttrs): m.Children {
  const {permissions, loadingPermissions} = attrs.state;
  const loaded = permissions !== undefined && permissions !== 'access_denied';
  const people = loaded
    ? permissions.filter((p) => p.type === 'user' || p.type === 'group')
    : [];
  const general = loaded
    ? permissions.filter((p) => p.type === 'domain' || p.type === 'anyone')
    : [];

  return m('.pf-gdrive-share__permissions', [
    m('.pf-gdrive-share__permissions-header', [
      m('span.pf-gdrive-share__permissions-title', 'People with access'),
      m(StackAuto),
      m(Button, {
        icon: 'refresh',
        loading: loadingPermissions,
        disabled: loadingPermissions,
        rounded: true,
        title: 'Refresh sharing permissions from Google Drive',
        onclick: () => attrs.onRefreshPermissions(),
      }),
    ]),
    permissions === 'access_denied'
      ? m(
          '.pf-gdrive-share__permissions-empty',
          "You don't have permission to view sharing settings for this file.",
        )
      : permissions === undefined
        ? m(
            '.pf-gdrive-share__permissions-empty',
            loadingPermissions
              ? 'Loading permissions…'
              : 'Click Refresh to load sharing permissions.',
          )
        : people.length === 0
          ? m(
              '.pf-gdrive-share__permissions-empty',
              'No specific people or groups.',
            )
          : m(
              'ul.pf-gdrive-share__permissions-list',
              people.map(renderPermissionItem),
            ),
    loaded && [
      m(
        '.pf-gdrive-share__permissions-header.pf-gdrive-share__permissions-section-header',
        m('span.pf-gdrive-share__permissions-title', 'General access'),
      ),
      general.length === 0
        ? m('ul.pf-gdrive-share__permissions-list', [
            m('li.pf-gdrive-share__permission-item', [
              m(
                '.pf-gdrive-share__permission-avatar',
                m(Icon, {
                  icon: 'lock',
                  className: 'pf-gdrive-share__permission-icon',
                }),
              ),
              m('.pf-gdrive-share__permission-info', [
                m('span.pf-gdrive-share__permission-label', 'Restricted'),
                m(
                  'span.pf-gdrive-share__permission-sublabel',
                  'Only people with access can open with the link',
                ),
              ]),
            ]),
          ])
        : m(
            'ul.pf-gdrive-share__permissions-list',
            general.map(renderPermissionItem),
          ),
      renderPermissionsWarning(attrs),
    ],
  ]);
}

// The trace is not in Drive yet: ask for a name and a folder, then upload.
function renderUploadForm(attrs: ShareFormAttrs): m.Children {
  const {state} = attrs;
  const {location, upload} = state;
  const uploading = upload !== undefined;
  return m(
    Form,
    {
      className: 'pf-gdrive-share',
      // Form disables the submit button while this returns false, as well as
      // while a required field is empty.
      validation: () => !uploading,
      onsubmit: (e: Event) => {
        e.preventDefault();
        attrs.onUpload();
      },
    },
    [
      renderError(state),
      m(FormLabel, {for: 'pf-gdrive-share-filename'}, 'File name'),
      m(TextInput, {
        id: 'pf-gdrive-share-filename',
        value: state.fileName,
        placeholder: 'my-trace.perfetto-trace',
        required: true,
        autofocus: true,
        // The name and location cannot change once an upload has started.
        disabled: uploading,
        onInput: (value: string) => attrs.onFileNameChange(value),
      }),
      m(FormLabel, {for: 'pf-gdrive-share-location'}, 'Location'),
      // The location can only be chosen with the folder picker, so it's shown
      // as a single button that opens the picker, rather than a read-only
      // text box that looks editable.
      m(Inline, {className: 'pf-gdrive-share__location-row'}, [
        m(Button, {
          id: 'pf-gdrive-share-location',
          className: 'pf-gdrive-share__location',
          type: 'button',
          icon: 'folder',
          label: location === 'root' ? 'My Drive' : (location.name ?? 'Folder'),
          rightIcon: 'edit',
          title: 'Choose a folder in Google Drive',
          variant: ButtonVariant.Outlined,
          disabled: uploading,
          onclick: () => attrs.onPickFolder(),
        }),
        // The folder picker can't select the top of My Drive, so offer a way
        // back to it once a folder has been chosen.
        location !== 'root' &&
          m(Button, {
            'className': 'pf-gdrive-share__location-reset',
            'type': 'button',
            'icon': 'restore',
            'title': 'Upload to My Drive instead',
            'aria-label': 'Upload to My Drive instead',
            'disabled': uploading,
            'onclick': () => attrs.onResetLocation(),
          }),
      ]),
      state.defaultShareDomains.length > 0 &&
        m(Checkbox, {
          label: `Also share with ${state.defaultShareDomains
            .map((d) => d.displayName ?? d.domain)
            .join(', ')}`,
          checked: state.applyDefaultShareDomains,
          disabled: uploading,
          onchange: (e: Event) =>
            attrs.onApplyDefaultShareDomainsChange(
              (e.target as HTMLInputElement).checked,
            ),
        }),
      upload === undefined
        ? m(Inline, {className: 'pf-gdrive-share__footer'}, [
            m(StackAuto),
            m(Button, {
              type: 'submit',
              label: 'Upload',
              icon: 'upload',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
            }),
          ])
        : m('.pf-gdrive-share__progress', [
            m(LinearProgress, {
              className: 'pf-gdrive-share__progress-bar',
              // Animate until the first progress report says how big the
              // upload is.
              state: 'indeterminate',
              value:
                upload.totalBytes > 0
                  ? upload.uploadedBytes / upload.totalBytes
                  : undefined,
            }),
            m(Inline, [
              m('span.pf-gdrive-share__progress-text', progressText(upload)),
              m(StackAuto),
              m(Button, {
                type: 'button',
                label: 'Cancel',
                variant: ButtonVariant.Outlined,
                onclick: () => attrs.onCancelUpload(),
              }),
            ]),
          ]),
    ],
  );
}
