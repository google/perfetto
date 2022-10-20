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

export const FORCE_RESET_MESSAGE = 'Force reset the USB interface';
export const DEFAULT_WEBSOCKET_URL = 'ws://127.0.0.1:8037';
export const ADB_ENDPOINT = '/adb';
export const TRACED_ENDPOINT = '/traced';
export const DEFAULT_ADB_WEBSOCKET_URL = DEFAULT_WEBSOCKET_URL + ADB_ENDPOINT;
export const DEFAULT_TRACED_WEBSOCKET_URL =
    DEFAULT_WEBSOCKET_URL + TRACED_ENDPOINT;
