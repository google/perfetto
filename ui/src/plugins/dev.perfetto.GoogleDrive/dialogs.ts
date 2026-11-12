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
import {getErrorMessage} from '../../base/errors';
import type {DriveError, PickerError} from './google/drive_client';
import type {AuthError} from './google/oauth_client';
import {alertModal} from '../../widgets/modal';

// The alerts shared by the flows that open a trace from Google Drive. Each
// one only tells the user what went wrong, and resolves once it is dismissed:
// what happens next is up to the flow.

// Tells the user that connecting to their Google account did not succeed.
export async function alertConnectFailed(auth: AuthError): Promise<void> {
  await alertModal({
    key: 'GoogleDriveConnectFailed',
    title: "Couldn't connect to Google Drive",
    content: [m('p', describeAuthFailure(auth)), renderErrorDetails(auth)],
  });
}

// Tells the user that the file picker could not be shown.
export async function alertPickerFailed(error: PickerError): Promise<void> {
  await alertModal({
    key: 'GoogleDrivePickerFailed',
    title: "Couldn't open the Google Drive file picker",
    content: [
      m('p', 'The Google Drive file picker could not be opened.'),
      renderErrorDetails(error),
    ],
  });
}

// Tells the user that a file could not be downloaded.
export async function alertDownloadFailed(
  error: DriveError,
  filename?: string,
): Promise<void> {
  await alertModal({
    key: 'GoogleDriveDownloadFailed',
    title: `Couldn't open ${filename ?? 'file'} from Google Drive`,
    content: [
      m('p', 'The file could not be downloaded from Google Drive.'),
      renderErrorDetails(error),
    ],
  });
}

// Serializes an inner error cause into a collapsible <details><pre> block
// when one is present, so technical details are available without cluttering
// the message or requiring DevTools.
export function renderErrorDetails(error: {
  readonly type: string;
  readonly cause?: unknown;
}): m.Children {
  if (error.cause === undefined) return undefined;
  let details: string;
  try {
    details = JSON.stringify(
      {type: error.type, cause: serializeCause(error.cause)},
      null,
      2,
    );
  } catch {
    details = `${error.type}: ${getErrorMessage(error.cause)}`;
  }
  return m(
    'details',
    m('summary', 'Error details'),
    m('pre', {style: {maxHeight: '40vh', overflow: 'auto'}}, details),
  );
}

function serializeCause(cause: unknown): unknown {
  if (cause instanceof Error) {
    return {
      name: cause.name,
      message: cause.message,
      ...(cause.stack !== undefined ? {stack: cause.stack} : {}),
      ...(cause.cause !== undefined
        ? {cause: serializeCause(cause.cause)}
        : {}),
    };
  }
  return cause;
}

// Explains, for the user, why connecting to their Google account did not
// succeed.
export function describeAuthFailure(auth: AuthError): string {
  switch (auth.type) {
    case 'popup_blocked':
      return 'The Google authentication popup was blocked.';
    case 'popup_closed':
      return 'The Google window was closed before the connection was completed.';
    case 'missing_scopes':
      return 'Perfetto was not granted all the permissions it asked for.';
    case 'error':
      return `Connecting to Google Drive failed: ${getErrorMessage(auth.cause)}`;
  }
}
