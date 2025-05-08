// Copyright (C) 2025 The Android Open Source Project
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

import {z} from 'zod';

// This file defines the JSON schema of the responses to the /track-devices-json
// websocket endpoint. See google's internal web_device_proxy.proto for the
// source of truth.

const WDP_DEVICE_SCHEMA = z
  .object({
    serialNumber: z.string(),
  })
  .and(
    z.union([
      z.object({
        proxyStatus: z.literal('ADB'),
        adbStatus: z.string(),
        adbProps: z.record(z.string(), z.string()).optional(),
      }),
      z.object({
        proxyStatus: z.literal('PROXY_UNAUTHORIZED'),
        adbStatus: z.string(),
        approveUrl: z.string(),
      }),
    ]),
  );

export const WDP_TRACK_DEVICES_SCHEMA = z.object({
  error: z
    .object({
      type: z.string(), // ORIGIN_NOT_ALLOWLISTED, or others
      message: z.string(),
      approveUrl: z.string().optional(),
    })
    .optional(),
  device: WDP_DEVICE_SCHEMA.array().optional(),
  version: z.string().optional(),
});

export type WdpTrackDevicesResponse = z.infer<typeof WDP_TRACK_DEVICES_SCHEMA>;
export type WdpDevice = z.infer<typeof WDP_DEVICE_SCHEMA>;
