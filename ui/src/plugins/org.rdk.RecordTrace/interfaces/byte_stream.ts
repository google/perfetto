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

/**
 * The base class for implementing byte streams. This is used both for
 * implementing various layers of the ADB stack and for modelling data exhanges
 * on the tracing protocol.
 */
export abstract class ByteStream {
  // Event handlers
  onData: (data: Uint8Array) => void = () => {};
  onClose: () => void = () => {};

  abstract get connected(): boolean;
  abstract write(data: string | Uint8Array): Promise<void>;
  abstract close(): void;
}
