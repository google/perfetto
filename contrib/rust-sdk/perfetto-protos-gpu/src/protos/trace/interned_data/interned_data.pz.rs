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

// Manually generated with bindings for an extra set of InternedData fields.

use crate::pb_msg;
use crate::pb_msg_ext;
use crate::protos::trace::gpu::gpu_render_stage_event::*;

use perfetto_sdk::protos::trace::interned_data::interned_data::InternedData;
use perfetto_sdk::protos::trace::profiling::profile_common::InternedString;

pb_msg_ext!(InternedData {
    vulkan_memory_keys: InternedString, msg, 22,
    graphics_contexts: InternedGraphicsContext, msg, 23,
    gpu_specifications: InternedGpuRenderStageSpecification, msg, 24,
});

/// Import this to use the extra `InternedData` fields.
pub mod prelude {
    pub use super::InternedDataExt;
}
