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

// In case the device doesn't have the tracebox, we upload the latest version
// to this path.
export const TRACEBOX_DEVICE_PATH = '/data/local/tmp/tracebox';

// Experimentally, this takes 900ms on the first fetch and 20-30ms after
// because of caching.
export const TRACEBOX_FETCH_TIMEOUT = 30000;

// Message shown to the user when they need to allow authentication on the
// device in order to connect.
export const ALLOW_USB_DEBUGGING =
    'Please allow USB debugging on device and try again.';
