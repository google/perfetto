// Copyright (C) 2024 The Android Open Source Project
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

import {assertTrue} from '../../../../base/logging';
import {Result, okResult, errResult} from '../../../../base/result';
import {AsyncWebsocket} from '../../websocket/async_websocket';
import {prefixWithHexLen} from '../../websocket/websocket_utils';

/**
 * Sends an ADB command over the websocket and waits for an OKAY or FAIL.
 * If `wantResponse` == true, expects a payload after the OKAY.
 * For all intents and purposes, the websocket here is the moral equivalent of
 * talking directly to ADB on 127.0.0.1:5037.
 * See //packages/modules/adb/docs/dev/services.md .
 */
export async function adbCmdAndWait(
  ws: AsyncWebsocket,
  cmd: string,
  wantResponse: boolean,
): Promise<Result<string>> {
  ws.send(prefixWithHexLen(cmd));
  const hdr = await ws.waitForString(4);
  if (hdr === 'FAIL' || (hdr === 'OKAY' && wantResponse)) {
    const hexLen = await ws.waitForString(4);
    const len = parseInt(hexLen, 16);
    assertTrue(!isNaN(len));
    const payload = await ws.waitForString(len);
    if (hdr === 'OKAY') {
      return okResult(payload);
    } else {
      return errResult(payload);
    }
  } else if (hdr === 'OKAY') {
    return okResult('');
  } else {
    return errResult(`ADB protocol error, hdr ${hdr}`);
  }
}
