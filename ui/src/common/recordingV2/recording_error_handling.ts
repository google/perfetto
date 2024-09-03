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

import {getErrorMessage} from '../../base/errors';
import {
  showAllowUSBDebugging,
  showConnectionLostError,
  showExtensionNotInstalled,
  showFailedToPushBinary,
  showIssueParsingTheTracedResponse,
  showNoDeviceSelected,
  showWebsocketConnectionIssue,
  showWebUSBErrorV2,
} from '../../frontend/error_dialog';
import {OnMessageCallback} from './recording_interfaces_v2';
import {
  ALLOW_USB_DEBUGGING,
  BINARY_PUSH_FAILURE,
  BINARY_PUSH_UNKNOWN_RESPONSE,
  EXTENSION_NOT_INSTALLED,
  NO_DEVICE_SELECTED,
  PARSING_UNABLE_TO_DECODE_METHOD,
  PARSING_UNKNWON_REQUEST_ID,
  PARSING_UNRECOGNIZED_MESSAGE,
  PARSING_UNRECOGNIZED_PORT,
  WEBSOCKET_UNABLE_TO_CONNECT,
} from './recording_utils';

// The pattern for handling recording error can have the following nesting in
// case of errors:
// A. wrapRecordingError -> wraps a promise
// B. onFailure -> has user defined logic and calls showRecordingModal
// C. showRecordingModal -> shows UX for a given error; this is not called
//    directly by wrapRecordingError, because we want the caller (such as the
//    UI) to dictate the UX

// This method takes a promise and a callback to be execute in case the promise
// fails. It then awaits the promise and executes the callback in case of
// failure. In the recording code it is used to wrap:
// 1. Acessing the WebUSB API.
// 2. Methods returning promises which can be rejected. For instance:
// a) When the user clicks 'Add a new device' but then doesn't select a valid
//    device.
// b) When the user starts a tracing session, but cancels it before they
//    authorize the session on the device.
export async function wrapRecordingError<T>(
  promise: Promise<T>,
  onFailure: OnMessageCallback,
): Promise<T | undefined> {
  try {
    return await promise;
  } catch (e) {
    // Sometimes the message is wrapped in an Error object, sometimes not, so
    // we make sure we transform it into a string.
    const errorMessage = getErrorMessage(e);
    onFailure(errorMessage);
    return undefined;
  }
}

// Shows a modal for every known type of error which can arise during recording.
// In this way, errors occuring at different levels of the recording process
// can be handled in a central location.
export function showRecordingModal(message: string): void {
  if (
    [
      'Unable to claim interface.',
      'The specified endpoint is not part of a claimed and selected ' +
        'alternate interface.',
      // thrown when calling the 'reset' method on a WebUSB device.
      'Unable to reset the device.',
    ].some((partOfMessage) => message.includes(partOfMessage))
  ) {
    showWebUSBErrorV2();
  } else if (
    [
      'A transfer error has occurred.',
      'The device was disconnected.',
      'The transfer was cancelled.',
    ].some((partOfMessage) => message.includes(partOfMessage)) ||
    isDeviceDisconnectedError(message)
  ) {
    showConnectionLostError();
  } else if (message === ALLOW_USB_DEBUGGING) {
    showAllowUSBDebugging();
  } else if (
    isMessageComposedOf(message, [
      BINARY_PUSH_FAILURE,
      BINARY_PUSH_UNKNOWN_RESPONSE,
    ])
  ) {
    showFailedToPushBinary(message.substring(message.indexOf(':') + 1));
  } else if (message === NO_DEVICE_SELECTED) {
    showNoDeviceSelected();
  } else if (WEBSOCKET_UNABLE_TO_CONNECT === message) {
    showWebsocketConnectionIssue(message);
  } else if (message === EXTENSION_NOT_INSTALLED) {
    showExtensionNotInstalled();
  } else if (
    isMessageComposedOf(message, [
      PARSING_UNKNWON_REQUEST_ID,
      PARSING_UNABLE_TO_DECODE_METHOD,
      PARSING_UNRECOGNIZED_PORT,
      PARSING_UNRECOGNIZED_MESSAGE,
    ])
  ) {
    showIssueParsingTheTracedResponse(message);
  } else {
    throw new Error(`${message}`);
  }
}

function isDeviceDisconnectedError(message: string) {
  return (
    message.includes('Device with serial') &&
    message.includes('was disconnected.')
  );
}

function isMessageComposedOf(message: string, issues: string[]) {
  for (const issue of issues) {
    if (message.includes(issue)) {
      return true;
    }
  }
  return false;
}

// Exception thrown by the Recording logic.
export class RecordingError extends Error {}
