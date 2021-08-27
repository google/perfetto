// Copyright (C) 2021 The Android Open Source Project
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

// This file defines the API of messages exchanged between frontend and
// {engine, controller} worker when bootstrapping the workers.
// Those messages are sent only once. The rest of the communication happens
// over the MessagePort(s) that are sent in the init message.

// This is so we can create all the workers in a central place in the frontend
// (Safari still doesn't spawning workers from other workers) but then let them
// communicate by sending the right MessagePort to them.

// Frontend -> Engine initialization message.
export interface EngineWorkerInitMessage {
  // The port used to receive engine messages (e.g., query commands).
  // The controller owns the other end of the MessageChannel
  // (see resetEngineWorker()).
  enginePort: MessagePort;
}

// Frontend -> Controller initialization message.
export interface ControllerWorkerInitMessage {
  // For receiving dispatch() commands from the frontend. This is where most of
  // the frontend <> controller interaction happens.
  controllerPort: MessagePort;

  // For controller <> Chrome extension communication.
  extensionPort: MessagePort;

  // For reporting errors back to the frontend. This is a dedicated port to
  // reduce depdencies on the business logic behind the other ports.
  errorReportingPort: MessagePort;
}
