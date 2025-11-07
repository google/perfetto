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

// Manually generated with bindings for an extra set of
// DataSourceDescriptor fields.

use crate::pb_msg;
use crate::pb_msg_ext;
use crate::protos::common::gpu_counter_descriptor::*;

use perfetto_sdk::protos::common::data_source_descriptor::DataSourceDescriptor;

pb_msg_ext!(DataSourceDescriptor {
    gpu_counter_descriptor: GpuCounterDescriptor, msg, 5,
});

/// Import this to use the extra `DataSourceDescriptor` fields.
pub mod prelude {
    pub use super::DataSourceDescriptorExt;
}
