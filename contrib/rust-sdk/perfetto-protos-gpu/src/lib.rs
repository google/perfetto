// Copyright (C) 2025 Rivos Inc.
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

/// Re-export pb_msg macro from this crate.
pub use perfetto_sdk::pb_msg;

/// Re-export pb_msg_ext macro from this crate.
pub use perfetto_sdk::pb_msg_ext;

/// Re-export pb_enum macro from this crate.
pub use perfetto_sdk::pb_enum;

/// Protobuf bindings module.
pub mod protos;
