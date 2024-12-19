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

import protos from '../../../protos';
import {errResult, okResult, Result} from '../../../base/result';
import {PreflightCheck} from '../interfaces/connection_check';
import {AdbDevice} from './adb_device';
import {getAdbTracingServiceState} from './adb_tracing_session';

/**
 * Common pre-flight checks for Android targets. This function is used by
 * both the AdbWebusbTarget and AdbWebsocketTarget. In both cases we want to
 * perform the same types of checks regardless of the transport.
 * @yields a sequence of pre-flight checks.
 */
export async function* checkAndroidTarget(
  adbDevice: AdbDevice,
): AsyncGenerator<PreflightCheck> {
  yield {
    name: 'Android version',
    status: await (async (): Promise<Result<string>> => {
      const status = await adbDevice.shell('getprop ro.build.version.sdk');
      if (!status.ok) return status;
      const sdkVer = parseInt(status.value);
      const minApi = 29;
      if (sdkVer < minApi) {
        return errResult(`Android API level ${minApi}+ (Q+) required`);
      }
      return okResult(`API level ${sdkVer} >= ${minApi}`);
    })(),
  };
  yield {
    name: 'traced running?',
    status: await (async (): Promise<Result<string>> => {
      const status = await adbDevice.shell('pidof traced');
      if (!status.ok) return status;
      if (isFinite(parseInt(status.value))) {
        return okResult(`pid = ${status.value}`);
      }
      return errResult(
        'Not running. Try `adb shell setprop persist.traced.enable 1`',
      );
    })(),
  };
  const svcStatus = await getAdbTracingServiceState(adbDevice);
  yield {
    name: 'Traced version',
    status: await (async (): Promise<Result<string>> => {
      if (!svcStatus.ok) return svcStatus;
      return okResult(svcStatus.value.tracingServiceVersion ?? 'N/A');
    })(),
  };
  if (svcStatus === undefined) return;
  yield {
    name: 'Traced state',
    status: await (async (): Promise<Result<string>> => {
      if (!svcStatus.ok) return svcStatus;
      const tss: protos.ITracingServiceState = svcStatus.value;
      return okResult(
        `#producers: ${tss.producers?.length ?? 'N/A'}, ` +
          `#datasources: ${tss.dataSources?.length ?? 'N/A'}, ` +
          `#sessions: ${tss.numSessionsStarted ?? 'N/A'}`,
      );
    })(),
  };
}
