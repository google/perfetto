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

// Manually generated with bindings for a limited set of InternedData
// fields to limit core proto bindings.

use crate::pb_msg;
use crate::protos::trace::track_event::chrome_histogram_sample::*;
use crate::protos::trace::track_event::debug_annotation::*;
use crate::protos::trace::track_event::log_message::*;
use crate::protos::trace::track_event::source_location::*;
use crate::protos::trace::track_event::track_event::*;

pb_msg!(InternedData {
    event_categories: EventCategory, msg, 1,
    event_names: EventName, msg, 2,
    debug_annotation_names: DebugAnnotationName, msg, 3,
    debug_annotation_value_type_names: DebugAnnotationValueTypeName, msg, 27,
    source_locations: SourceLocation, msg, 4,
    unsymbolized_source_locations: UnsymbolizedSourceLocation, msg, 28,
    log_message_body: LogMessageBody, msg, 20,
    histogram_names: HistogramName, msg, 25,
});
