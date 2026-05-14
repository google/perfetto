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

#![doc = include_str!("../README.md")]
#![cfg_attr(
    feature = "intrinsics",
    allow(internal_features),
    feature(core_intrinsics)
)]

use std::ffi::CString;

use perfetto_sdk::producer::{Backends, Producer, ProducerInitArgsBuilder};
use perfetto_sdk::track_event::{EventContext, TrackEvent, TrackEventDebugArg, TrackEventType};
use tracing_core::span;
use tracing_subscriber::Layer;
use tracing_subscriber::layer::Context;
use tracing_subscriber::registry::LookupSpan;

perfetto_sdk::track_event_categories! {
    pub mod perfetto_te_ns {
        ("tracing", "Events from the Rust tracing crate", []),
    }
}

/// Initialize the Perfetto producer and track event system with both
/// in-process and system backends enabled.
///
/// Call this once before installing the layer.
pub fn init() {
    init_with_backends(Backends::IN_PROCESS | Backends::SYSTEM);
}

/// Initialize with only the in-process backend.
///
/// Use this when collecting traces programmatically via
/// [`perfetto_sdk::tracing_session::TracingSession`] without a system
/// tracing service.
pub fn init_in_process() {
    init_with_backends(Backends::IN_PROCESS);
}

/// Initialize with only the system backend.
///
/// Use this when connecting to a running Perfetto tracing service
/// (e.g. `traced`).
pub fn init_system() {
    init_with_backends(Backends::SYSTEM);
}

fn init_with_backends(backends: Backends) {
    Producer::init(ProducerInitArgsBuilder::new().backends(backends).build());
    TrackEvent::init();
    perfetto_te_ns::register().ok();
}

/// Per-span data stored in tracing-subscriber's Extensions.
struct SpanData {
    name: CString,
    fields: Vec<(&'static str, FieldValue)>,
}

enum FieldValue {
    Bool(bool),
    I64(i64),
    U64(u64),
    F64(f64),
    Str(String),
}

/// A `tracing_subscriber::Layer` that emits Perfetto track events.
///
/// Spans become duration slices (begin/end) and events become instant
/// events, all routed through the Perfetto SDK's track event system.
pub struct PerfettoLayer {
    debug_annotations: bool,
}

impl PerfettoLayer {
    /// Create a new layer with debug annotations enabled.
    pub fn new() -> Self {
        Self {
            debug_annotations: true,
        }
    }

    /// Create a new layer without debug annotations.
    ///
    /// Fields on spans and events will not be captured, reducing
    /// overhead.
    pub fn without_debug_annotations() -> Self {
        Self {
            debug_annotations: false,
        }
    }
}

impl Default for PerfettoLayer {
    fn default() -> Self {
        Self::new()
    }
}

fn add_debug_args(ctx: &mut EventContext, fields: &[(&'static str, FieldValue)]) {
    for (name, value) in fields {
        let arg = match value {
            FieldValue::Bool(v) => TrackEventDebugArg::Bool(*v),
            FieldValue::I64(v) => TrackEventDebugArg::Int64(*v),
            FieldValue::U64(v) => TrackEventDebugArg::Uint64(*v),
            FieldValue::F64(v) => TrackEventDebugArg::Double(*v),
            FieldValue::Str(v) => TrackEventDebugArg::String(v),
        };
        ctx.add_debug_arg(name, arg);
    }
}

fn add_source_location(ctx: &mut EventContext, meta: &tracing_core::Metadata<'_>) {
    use perfetto_sdk::protos::trace::track_event::source_location::SourceLocationFieldNumber;
    use perfetto_sdk::protos::trace::track_event::track_event::TrackEventFieldNumber;
    use perfetto_sdk::track_event::{TrackEventProtoField, TrackEventProtoFields};

    let file = meta.file().unwrap_or("");
    let line = meta.line().unwrap_or(0);

    ctx.set_proto_fields(&TrackEventProtoFields {
        fields: &[TrackEventProtoField::Nested(
            TrackEventFieldNumber::SourceLocation as u32,
            &[
                TrackEventProtoField::Cstr(SourceLocationFieldNumber::FileName as u32, file),
                TrackEventProtoField::VarInt(
                    SourceLocationFieldNumber::LineNumber as u32,
                    line as u64,
                ),
            ],
        )],
    });
}

struct FieldVisitor {
    fields: Vec<(&'static str, FieldValue)>,
}

impl FieldVisitor {
    fn new() -> Self {
        Self { fields: Vec::new() }
    }
}

impl tracing::field::Visit for FieldVisitor {
    fn record_bool(&mut self, field: &tracing::field::Field, value: bool) {
        self.fields.push((field.name(), FieldValue::Bool(value)));
    }

    fn record_i64(&mut self, field: &tracing::field::Field, value: i64) {
        self.fields.push((field.name(), FieldValue::I64(value)));
    }

    fn record_u64(&mut self, field: &tracing::field::Field, value: u64) {
        self.fields.push((field.name(), FieldValue::U64(value)));
    }

    fn record_f64(&mut self, field: &tracing::field::Field, value: f64) {
        self.fields.push((field.name(), FieldValue::F64(value)));
    }

    fn record_str(&mut self, field: &tracing::field::Field, value: &str) {
        self.fields
            .push((field.name(), FieldValue::Str(value.to_string())));
    }

    fn record_debug(&mut self, field: &tracing::field::Field, value: &dyn std::fmt::Debug) {
        self.fields
            .push((field.name(), FieldValue::Str(format!("{value:?}"))));
    }
}

impl<S> Layer<S> for PerfettoLayer
where
    S: tracing::Subscriber + for<'a> LookupSpan<'a>,
{
    fn on_new_span(&self, attrs: &span::Attributes<'_>, id: &span::Id, ctx: Context<'_, S>) {
        let span = ctx.span(id).expect("span not found");
        let name = CString::new(attrs.metadata().name()).unwrap_or_default();

        let fields = if self.debug_annotations {
            let mut visitor = FieldVisitor::new();
            attrs.values().record(&mut visitor);
            visitor.fields
        } else {
            Vec::new()
        };

        span.extensions_mut().insert(SpanData { name, fields });
    }

    fn on_record(&self, id: &span::Id, values: &span::Record<'_>, ctx: Context<'_, S>) {
        if !self.debug_annotations {
            return;
        }
        let span = ctx.span(id).expect("span not found");
        let mut extensions = span.extensions_mut();
        if let Some(data) = extensions.get_mut::<SpanData>() {
            let mut visitor = FieldVisitor::new();
            values.record(&mut visitor);
            data.fields.extend(visitor.fields);
        }
    }

    fn on_enter(&self, id: &span::Id, ctx: Context<'_, S>) {
        let span = ctx.span(id).expect("span not found");
        let extensions = span.extensions();
        let Some(data) = extensions.get::<SpanData>() else {
            return;
        };
        let name_ptr = data.name.as_ptr();
        let fields = &data.fields;
        let meta = span.metadata();
        let debug_annotations = self.debug_annotations;
        perfetto_sdk::track_event!(
            "tracing",
            TrackEventType::SliceBegin(name_ptr),
            |ctx: &mut EventContext| {
                add_source_location(ctx, meta);
                if debug_annotations && !fields.is_empty() {
                    add_debug_args(ctx, fields);
                }
            }
        );
    }

    fn on_exit(&self, _id: &span::Id, _ctx: Context<'_, S>) {
        perfetto_sdk::track_event!("tracing", TrackEventType::SliceEnd);
    }

    fn on_event(&self, event: &tracing::Event<'_>, _ctx: Context<'_, S>) {
        let meta = event.metadata();
        let name = CString::new(meta.name()).unwrap_or_default();
        let name_ptr = name.as_ptr();

        let fields = if self.debug_annotations {
            let mut visitor = FieldVisitor::new();
            event.record(&mut visitor);
            visitor.fields
        } else {
            Vec::new()
        };

        perfetto_sdk::track_event!(
            "tracing",
            TrackEventType::Instant(name_ptr),
            |ctx: &mut EventContext| {
                add_source_location(ctx, meta);
                if !fields.is_empty() {
                    add_debug_args(ctx, &fields);
                }
            }
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tracing_subscriber::prelude::*;

    #[test]
    fn layer_can_be_installed() {
        init();
        let _subscriber = tracing_subscriber::registry().with(PerfettoLayer::new());
    }

    #[test]
    fn layer_without_debug_annotations() {
        init();
        let _subscriber =
            tracing_subscriber::registry().with(PerfettoLayer::without_debug_annotations());
    }

    #[test]
    fn default_layer() {
        init();
        let layer = PerfettoLayer::default();
        assert!(layer.debug_annotations);
    }

    #[test]
    fn span_enter_exit() {
        init();
        let subscriber = tracing_subscriber::registry().with(PerfettoLayer::new());
        tracing::subscriber::with_default(subscriber, || {
            let span = tracing::info_span!("test_span", x = 42);
            let _guard = span.enter();
        });
    }

    #[test]
    fn nested_spans() {
        init();
        let subscriber = tracing_subscriber::registry().with(PerfettoLayer::new());
        tracing::subscriber::with_default(subscriber, || {
            let outer = tracing::info_span!("outer").entered();
            {
                let _inner = tracing::info_span!("inner", value = "hello").entered();
            }
            drop(outer);
        });
    }

    #[test]
    fn instant_event() {
        init();
        let subscriber = tracing_subscriber::registry().with(PerfettoLayer::new());
        tracing::subscriber::with_default(subscriber, || {
            tracing::info!("test event");
        });
    }

    #[test]
    fn event_with_fields() {
        init();
        let subscriber = tracing_subscriber::registry().with(PerfettoLayer::new());
        tracing::subscriber::with_default(subscriber, || {
            tracing::info!(count = 42, name = "test", enabled = true, "an event");
        });
    }

    #[test]
    fn event_inside_span() {
        init();
        let subscriber = tracing_subscriber::registry().with(PerfettoLayer::new());
        tracing::subscriber::with_default(subscriber, || {
            let _span = tracing::info_span!("parent").entered();
            tracing::warn!("warning inside span");
        });
    }

    #[test]
    fn span_record_late_fields() {
        init();
        let subscriber = tracing_subscriber::registry().with(PerfettoLayer::new());
        tracing::subscriber::with_default(subscriber, || {
            let span = tracing::info_span!("span", answer = tracing::field::Empty);
            span.record("answer", 42);
            let _guard = span.enter();
        });
    }

    #[test]
    fn no_annotations_mode() {
        init();
        let subscriber =
            tracing_subscriber::registry().with(PerfettoLayer::without_debug_annotations());
        tracing::subscriber::with_default(subscriber, || {
            let _span = tracing::info_span!("span", field = "ignored").entered();
            tracing::info!(also = "ignored", "event");
        });
    }
}
