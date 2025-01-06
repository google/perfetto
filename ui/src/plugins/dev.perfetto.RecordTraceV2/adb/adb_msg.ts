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

import {assertTrue} from '../../../base/logging';
import {isString} from '../../../base/object_utils';
import {binaryEncode, utf8Decode, utf8Encode} from '../../../base/string_utils';

const ADB_MSG_SIZE = 6 * 4; // 6 * int32.

export interface AdbMsgHdr {
  readonly cmd: string;
  readonly arg0: number;
  readonly arg1: number;
  readonly dataLen: number;
  readonly dataChecksum: number;
}

export interface AdbMsg extends AdbMsgHdr {
  data: Uint8Array;
}

// A brief description of the message can be found here:
// https://android.googlesource.com/platform/system/core/+/main/adb/protocol.txt
//
// struct amessage {
//     uint32_t command;    // command identifier constant
//     uint32_t arg0;       // first argument
//     uint32_t arg1;       // second argument
//     uint32_t data_length;// length of payload (0 is allowed)
//     uint32_t data_check; // checksum of data payload
//     uint32_t magic;      // command ^ 0xffffffff
// };
export function parseAdbMsgHdr(dv: DataView): AdbMsgHdr {
  assertTrue(dv.byteLength === ADB_MSG_SIZE);
  const cmd = utf8Decode(dv.buffer.slice(0, 4));
  const cmdNum = dv.getUint32(0, true);
  const arg0 = dv.getUint32(4, true);
  const arg1 = dv.getUint32(8, true);
  const dataLen = dv.getUint32(12, true);
  const dataChecksum = dv.getUint32(16, true);
  const cmdChecksum = dv.getUint32(20, true);
  const magic = dv.getUint32(20, true);
  assertTrue(magic === (cmdNum ^ 0xffffffff) >>> 0);
  assertTrue(cmdNum === (cmdChecksum ^ 0xffffffff));
  return {cmd, arg0, arg1, dataLen, dataChecksum};
}

export function encodeAdbMsg(
  cmd: string,
  arg0: number,
  arg1: number,
  data: Uint8Array,
  useChecksum = false,
) {
  const checksum = useChecksum ? generateChecksum(data) : 0;
  const buf = new Uint8Array(ADB_MSG_SIZE);
  const dv = new DataView(buf.buffer);
  for (let i = 0; i < 4; i++) {
    dv.setUint8(i, cmd.charCodeAt(i));
  }
  dv.setUint32(4, arg0, true);
  dv.setUint32(8, arg1, true);
  dv.setUint32(12, data.byteLength, true);
  dv.setUint32(16, checksum, true);
  dv.setUint32(20, dv.getUint32(0, true) ^ 0xffffffff, true);

  return buf;
}

export function encodeAdbData(data?: Uint8Array | string): Uint8Array {
  if (data === undefined) return new Uint8Array([]);
  if (isString(data)) return utf8Encode(data + '\0');
  return data;
}

function generateChecksum(data: Uint8Array): number {
  let res = 0;
  for (let i = 0; i < data.byteLength; i++) res += data[i];
  return res & 0xffffffff;
}

export function adbMsgToString(msg: AdbMsg | AdbMsgHdr) {
  return (
    `cmd=${msg.cmd}, arg0=${msg.arg0}, arg1=${msg.arg1}, ` +
    `cksm=${msg.dataChecksum}, dlen=${msg.dataLen}` +
    ('data' in msg && msg.data !== undefined
      ? `, data=${binaryEncode(msg.data)}`
      : '')
  );
}
