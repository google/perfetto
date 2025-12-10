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

use crate::{
    data_source::TraceContextBase,
    fnv1a,
    heap_buffer::HeapBuffer,
    pb_msg::{PbMsg, PbMsgWriter},
    protos::trace::{
        interned_data::interned_data::InternedDataFieldNumber,
        track_event::{counter_descriptor::CounterDescriptor, track_descriptor::TrackDescriptor},
    },
};
use perfetto_sdk_sys::*;
use std::{
    ffi::{CStr, CString},
    marker::PhantomData,
    os::raw::{c_char, c_void},
    ptr,
    sync::atomic::{AtomicBool, Ordering},
    time::Duration,
};
use thiserror::Error;

/// Track event errors.
#[derive(Error, Debug, PartialEq)]
pub enum TrackEventError {
    /// Failed to register categories as already registered.
    #[error("Categories have already been registered.")]
    CategoriesAlreadyRegisteredError,
    /// Failure because categories are not yet registered.
    #[error("Categories are not registered.")]
    CategoriesNotRegisteredError,
}

/// Trace context struct passed to track event trace callbacks.
pub struct TraceContext {
    base: TraceContextBase,
    incr: *mut PerfettoTeLlImplIncr,
}

impl TraceContext {
    /// Returns true if the track event incremental state has already seen in the
    /// past the given track UUID.
    pub fn track_seen(&mut self, uuid: u64) -> bool {
        // SAFETY: `self.incr` must be a pointer provided by a call to
        // PerfettoTeLlImplBegin/Next.
        unsafe { PerfettoTeLlImplTrackSeen(self.incr, uuid) }
    }

    /// Interning:
    ///
    /// it's possible to avoid repeating the same data over and over in a trace by
    /// using "interning".
    ///
    /// `type` is a field id in the `perfetto.protos.InternedData` protobuf message.
    /// `data` reference raw data that is potentially repeated.
    /// The data referenced by `data` can be anything (e.g. a serialized protobuf
    /// message, or a small integer) that uniquely identifies the potentially
    /// repeated data.
    ///
    /// The function returns a tuple containing an integer (the iid) that can be used
    /// instead of serializing the data directly in the packet and a boolean that is set
    /// to false if this is the first time the library observed this data for this specific
    /// type (therefore it allocated a new iid).
    pub fn intern(&mut self, r#type: InternedDataFieldNumber, data: &[u8]) -> (u64, bool) {
        let mut seen: bool = false;
        // SAFETY:
        //
        // - `self.incr` must be a pointer provided by a call to
        // PerfettoTeLlImplBegin/Next.
        // - `seen` must be storage for a boolean return value.
        let iid = unsafe {
            PerfettoTeLlImplIntern(
                self.incr,
                r#type as i32,
                data.as_ptr() as *mut c_void,
                data.len(),
                &raw mut seen,
            )
        };
        (iid, seen)
    }
}

impl std::ops::Deref for TraceContext {
    type Target = TraceContextBase;
    fn deref(&self) -> &Self::Target {
        &self.base
    }
}

impl std::ops::DerefMut for TraceContext {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.base
    }
}

/// An opaque struct used to represent the track event machinery.
pub struct TrackEvent {}

impl TrackEvent {
    /// Initialize global track event machinery. Safe to call multiple times
    /// as a no-op when already called.
    pub fn init() {
        // SAFETY: FFI call with no outstanding preconditions.
        unsafe { PerfettoTeInit() };
    }

    /// Tells the tracing service about newly registered categories. Must be called
    /// after one or more calls to `TrackEventCategory::register()` or
    /// `TrackEventCategory::unregister()`.
    pub fn publish_categories() {
        // SAFETY: FFI call with no outstanding preconditions.
        unsafe { PerfettoTePublishCategories() };
    }
}

/// Category callback type.
pub type CategoryCallback = Box<dyn FnMut(u32, bool, bool) + Send + Sync + 'static>;

/// Struct used to represent a registered category.
///
/// Recommended usage is the track_event_categories macro instead of using
/// this struct directly.
pub struct TrackEventCategory {
    enabled: *mut bool,
    impl_: *mut PerfettoTeCategoryImpl,
    desc: PerfettoTeCategoryDescriptor,
    cat_iid: u64,
    _marker: PhantomData<&'static ()>,
}

impl TrackEventCategory {
    /// Creates a new category that can be registered.
    ///
    /// # Safety
    ///
    /// - `tags` must be a number of null-terminated C strings.
    pub const unsafe fn new(name: &CStr, desc: &CStr, tags: &[*const c_char]) -> Self {
        Self {
            enabled: &raw mut perfetto_atomic_false,
            impl_: ptr::null_mut(),
            desc: PerfettoTeCategoryDescriptor {
                name: name.as_ptr() as *const c_char,
                desc: desc.as_ptr() as *const c_char,
                tags: tags.as_ptr() as *mut *const c_char,
                num_tags: tags.len(),
            },
            cat_iid: 0,
            _marker: PhantomData,
        }
    }

    /// Returns a boolean that tells if the category is enabled or not.
    pub fn is_enabled(&self) -> bool {
        // SAFETY: `self.enabled` must be a pointer to a primitive with layout that matches C11
        // atomic_bool.
        unsafe {
            let atomic_ptr = self.enabled as *const AtomicBool;
            (*atomic_ptr).load(Ordering::Relaxed)
        }
    }

    /// Registers the category.
    pub fn register(&mut self) {
        assert!(!self.desc.name.is_null());
        assert!(!self.desc.desc.is_null());
        // SAFETY:
        // - `self.desc` must be a pointer to a PerfettoTeCategoryDescriptor struct with:
        // - name and desc fields set to null-terminated C strings.
        // - tags field set to an array of null-terminated C strings.
        // - num_tags field set to the number of items in the tags array.
        unsafe {
            self.impl_ = PerfettoTeCategoryImplCreate(&raw mut self.desc);
            self.enabled = PerfettoTeCategoryImplGetEnabled(self.impl_);
            self.cat_iid = PerfettoTeCategoryImplGetIid(self.impl_);
        }
    }

    /// Unregisters the category. Must have been previously registered.
    pub fn unregister(&mut self) {
        assert!(!self.impl_.is_null());
        // SAFETY: `self.impl_` must be previously created using PerfettoTeCategoryImplCreate.
        unsafe { PerfettoTeCategoryImplDestroy(self.impl_) };
        self.impl_ = ptr::null_mut();
        self.enabled = &raw mut perfetto_atomic_false;
        self.cat_iid = 0;
    }

    unsafe extern "C" fn callback_trampoline(
        _c: *mut PerfettoTeCategoryImpl,
        inst_id: PerfettoDsInstanceIndex,
        enabled: bool,
        global_state_changed: bool,
        user_arg: *mut c_void,
    ) {
        let result = std::panic::catch_unwind(|| {
            // SAFETY: `user_arg` must be a CategoryCallback.
            let f: &mut CategoryCallback = unsafe { &mut *(user_arg as *mut _) };
            f(inst_id, enabled, global_state_changed);
        });
        if let Err(err) = result {
            eprintln!("Fatal panic: {:?}", err);
            std::process::abort();
        }
    }

    /// Set category callback called when category is enabled.
    ///
    /// # Safety
    ///
    /// - `callback` must be kept alive until replaced by a new callback or
    ///   category has been unregistered.
    pub unsafe fn set_callback(&mut self, callback: &mut CategoryCallback) {
        assert!(!self.impl_.is_null());
        let user_arg = crate::__box_as_mut_ptr(callback) as *mut c_void;
        // SAFETY:
        // - `self.impl_` must be previously created using PerfettoTeCategoryImplCreate.
        // - `user_arg` must be a CategoryCallback.
        unsafe {
            PerfettoTeCategoryImplSetCallback(self.impl_, Some(Self::callback_trampoline), user_arg)
        };
    }

    /// Emit track event of a specific type.
    pub fn emit(&mut self, variant: TrackEventType, ctx: &mut EventContext) {
        assert!(!self.impl_.is_null());
        let te_type = match variant {
            TrackEventType::Instant(_) => PerfettoTeType_PERFETTO_TE_TYPE_INSTANT,
            TrackEventType::SliceBegin(_) => PerfettoTeType_PERFETTO_TE_TYPE_SLICE_BEGIN,
            TrackEventType::SliceEnd => PerfettoTeType_PERFETTO_TE_TYPE_SLICE_END,
            TrackEventType::Counter => PerfettoTeType_PERFETTO_TE_TYPE_COUNTER,
        };
        let te_name = match variant {
            TrackEventType::Instant(name) => Some(name),
            TrackEventType::SliceBegin(name) => Some(name),
            _ => None,
        };
        let mut te_extras: Vec<*mut PerfettoTeHlExtra> = ctx
            .extras
            .iter_mut()
            .map(|e| match e {
                TeHlExtra::Flush(te_flush) => te_flush as *mut PerfettoTeHlExtra,
                TeHlExtra::NoIntern(te_no_intern) => te_no_intern as *mut PerfettoTeHlExtra,
                TeHlExtra::Timestamp(te_timstamp) => {
                    &mut te_timstamp.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::DebugArgBool(te_debug_arg, _) => {
                    &mut te_debug_arg.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::DebugArgUint64(te_debug_arg, _) => {
                    &mut te_debug_arg.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::DebugArgInt64(te_debug_arg, _) => {
                    &mut te_debug_arg.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::DebugArgDouble(te_debug_arg, _) => {
                    &mut te_debug_arg.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::DebugArgString(te_debug_arg, _, _) => {
                    &mut te_debug_arg.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::DebugArgPointer(te_debug_arg, _) => {
                    &mut te_debug_arg.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::Track(te_track) => &mut te_track.header as *mut PerfettoTeHlExtra,
                TeHlExtra::NamedTrack(te_track, _) => {
                    &mut te_track.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::Flow(te_flow) => &mut te_flow.header as *mut PerfettoTeHlExtra,
                TeHlExtra::CounterInt64(te_counter) => {
                    &mut te_counter.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::CounterDouble(te_counter) => {
                    &mut te_counter.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::ProtoFields(te_fields, _, _) => {
                    &mut te_fields.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::ProtoTrack(te_fields, _, _) => {
                    &mut te_fields.header as *mut PerfettoTeHlExtra
                }
                TeHlExtra::NestedTracks(te_fields, _, _) => {
                    &mut te_fields.header as *mut PerfettoTeHlExtra
                }
            })
            .collect();
        te_extras.push(ptr::null_mut());

        // SAFETY:
        // - `self.impl_` must be previously created using PerfettoTeCategoryImplCreate.
        // - `te_type` must be a valid PerfettoTeType_* value.
        // - `name` must be a null-terminated C string or null.
        // - `te_extras` must be a null-terminated array of PerfettoTeHlExtra pointers.
        unsafe {
            PerfettoTeHlEmitImpl(
                self.impl_,
                te_type as i32,
                te_name.unwrap_or(ptr::null_mut()),
                te_extras.as_ptr(),
            )
        };
    }

    /// Calls `cb` for all active track event data source instances for this category.
    pub fn trace<F>(&self, mut cb: F)
    where
        F: FnMut(&mut TraceContext),
    {
        // SAFETY: FFI call with no outstanding preconditions.
        let timestamp = unsafe { PerfettoTeGetTimestamp() };

        // SAFETY:
        // - `self.impl_` must be previously created using PerfettoTeCategoryImplCreate.
        // - `timestamp` must be timestamp from PerfettoTeGetTimestamp().
        let mut iterator = unsafe { PerfettoTeLlImplBegin(self.impl_, timestamp) };
        loop {
            if iterator.ds.tracer.is_null() {
                break;
            }

            let mut ctx = TraceContext {
                base: TraceContextBase {
                    iterator: iterator.ds,
                },
                incr: iterator.incr,
            };
            cb(&mut ctx);

            // SAFETY:
            // - `self.impl_` must be previously created using PerfettoTeCategoryImplCreate.
            // - `timestamp` must be timestamp from PerfettoTeGetTimestamp().
            // - `iterator` must be a value returned from PerfettoTeLlImplBegin or
            //   PerfettoTeLlImplNext with a non-null `iterator.ds.tracer`.
            unsafe { PerfettoTeLlImplNext(self.impl_, timestamp, &raw mut iterator) };
        }
    }
}

/// Internal helper macro used to count expressions.
#[doc(hidden)]
#[macro_export]
macro_rules! __count_exprs {
    ($($item:expr),* $(,)?) => {
        <[()]>::len(&[$($crate::__count_exprs!(@sub $item)),*])
    };
    (@sub $item:expr) => { () };
}

/// Defines track event categories.
///
/// Example:
///
/// ```
/// use perfetto_sdk::*;
///
/// track_event_categories! {
///     pub mod my_categories_te_ns {
///         ( "c1", "My category 1 description", [ "tag1", "tag2" ] ),
///         ( "c2", "My category 2 description", [ "tag1" ] ),
///         ( "c3", "My category 3 description", [] ),
///     }
/// }
///
/// use my_categories_te_ns as perfetto_te_ns;
///
/// //...
///
/// use std::error::Error;
///
/// fn main() -> Result<(), Box<dyn Error>> {
///     producer::Producer::init(
///         producer::ProducerInitArgsBuilder::new()
///             .backends(producer::Backends::SYSTEM)
///             .build(),
///     );
///     track_event::TrackEvent::init();
///     perfetto_te_ns::register()?;
///     //...
///     Ok(())
/// }
/// ```
#[macro_export]
macro_rules! track_event_categories {
    (
        $vis:vis mod $modname:ident {
            $( ($key:literal, $desc:literal, [$($tag:literal),* $(,)?]) ),* $(,)?
        }
    ) => {
        $vis mod $modname {
            use $crate::{
                track_event::{
                    TraceContext,
                    TrackEvent,
                    CategoryCallback,
                    EventContext,
                    TrackEventCategory,
                    TrackEventError,
                    TrackEventType,
                },
            };
            use std::{ffi::CStr, os::raw::c_char, sync::Mutex};

            const CATEGORY_COUNT: usize = $crate::__count_exprs!($($key),*);
            const CATEGORY_TAGS: &[(&str, &[*const c_char])] = &[
                $(
                    (
                        $key,
                        &[$(concat!($tag, "\0").as_bytes().as_ptr() as *const c_char),*],
                    )
                ),+
            ];
            // Note: `CATEGORIES` are not guarded by a Mutex to allow `enabled` fields
            // to be checked using atomics.
            static mut CATEGORIES: [TrackEventCategory; CATEGORY_COUNT] = [
                $(
                    // SAFETY: `CATEGORY_TAGS` must be a null-terminated array of C strings.
                    unsafe {
                        TrackEventCategory::new(
                            CStr::from_bytes_with_nul_unchecked(concat!($key, "\0").as_bytes()),
                            CStr::from_bytes_with_nul_unchecked(concat!($desc, "\0").as_bytes()),
                            CATEGORY_TAGS[category_index($key)].1,
                        )
                    }
                ),+
            ];
            static CATEGORIES_REGISTERED: Mutex<bool> = Mutex::new(false);
            static CATEGORY_CALLBACKS: Mutex<[Option<CategoryCallback>; CATEGORY_COUNT]> =
                Mutex::new([const { None }; CATEGORY_COUNT]);

            const fn str_eq(a: &str, b: &str) -> bool {
                let a_bytes = a.as_bytes();
                let b_bytes = b.as_bytes();
                if a_bytes.len() != b_bytes.len() {
                    return false;
                }
                let mut i = 0;
                while i < a_bytes.len() {
                    if a_bytes[i] != b_bytes[i] {
                        return false;
                    }
                    i += 1;
                }
                true
            }

            #[allow(unused)]
            $vis fn register() -> Result<(), TrackEventError> {
                let mut registered = CATEGORIES_REGISTERED.lock().unwrap();
                if *registered {
                    return Err(TrackEventError::CategoriesAlreadyRegisteredError);
                }
                *registered = true;
                // SAFETY:
                //
                // - Requires exclusive access to `CATEGORIES`, which is provided by
                //   `CATEGORIES_REGISTERED`.
                unsafe {
                    #[allow(static_mut_refs)]
                    for c in &mut CATEGORIES {
                        c.register();
                    }
                }
                let mut callbacks = CATEGORY_CALLBACKS.lock().unwrap();
                for category_index in 0..CATEGORY_COUNT {
                    if let Some(boxed_cb) = callbacks[category_index].as_mut() {
                        // SAFETY:
                        //
                        // - Requires exclusive access to `CATEGORIES`, which is
                        //   provided by `CATEGORIES_REGISTERED`.
                        unsafe { CATEGORIES[category_index].set_callback(boxed_cb) };
                    }
                }
                TrackEvent::publish_categories();
                Ok(())
            }

            #[allow(unused)]
            $vis fn unregister() -> Result<(), TrackEventError> {
                let mut registered = CATEGORIES_REGISTERED.lock().unwrap();
                if !*registered {
                    return Err(TrackEventError::CategoriesNotRegisteredError);
                }
                *registered = false;
                // SAFETY:
                //
                // - Requires exclusive access to `CATEGORIES`, which is provided by
                //   `CATEGORIES_REGISTERED`.
                unsafe {
                    #[allow(static_mut_refs)]
                    for c in &mut CATEGORIES {
                        c.unregister();
                    }
                }
                TrackEvent::publish_categories();
                Ok(())
            }

            // allow unused_assignments for last iteration
            #[allow(unused_assignments)]
            $vis const fn category_index(s: &str) -> usize {
                let mut i = 0;
                $(
                    if str_eq(s, $key) { return i; }
                    i += 1;
                )*
                panic!("unknown category");
            }

            /// Safe to call this before categories have been registered.
            #[allow(unused)]
            $vis fn is_category_enabled(category_index: usize) -> bool {
                assert!(category_index < CATEGORY_COUNT);
                // SAFETY:
                //
                // - Safe to call on any thread as it uses atomics.
                unsafe { CATEGORIES[category_index].is_enabled() }
            }

            #[allow(unused)]
            $vis fn set_category_callback<F>(category_index: usize, cb: F)
            where
                F: FnMut(u32, bool, bool) + Send + Sync + 'static,
            {
                assert!(category_index < CATEGORY_COUNT);
                let registered = CATEGORIES_REGISTERED.lock().unwrap();
                let boxed: Box<CategoryCallback> = Box::new(Box::new(cb));
                let mut callbacks = CATEGORY_CALLBACKS.lock().unwrap();
                // Drop old callback after having set a new callback.
                let _old = callbacks[category_index].replace(boxed);
                if *registered {
                    if let Some(boxed_cb) = callbacks[category_index].as_mut() {
                        // SAFETY:
                        //
                        // - Requires exclusive access to `CATEGORIES`, which is
                        //   provided by `CATEGORIES_REGISTERED`.
                        unsafe { CATEGORIES[category_index].set_callback(boxed_cb) };
                    }
                }
            }

            #[allow(unused)]
            $vis fn emit(
                category_index: usize,
                variant: TrackEventType,
                ctx: &mut EventContext,
            ) {
                assert!(category_index < CATEGORY_COUNT);
                let registered = CATEGORIES_REGISTERED.lock().unwrap();
                if *registered {
                    // SAFETY:
                    //
                    // - Requires exclusive access to `CATEGORIES`, which is provided by
                    //   `CATEGORIES_REGISTERED`.
                    unsafe { CATEGORIES[category_index].emit(variant, ctx) };
                }
            }

            #[allow(unused)]
            $vis fn trace<F>(category_index: usize, cb: F)
            where
                F: FnMut(&mut TraceContext)
            {
                assert!(category_index < CATEGORY_COUNT);
                let registered = CATEGORIES_REGISTERED.lock().unwrap();
                if *registered {
                    // SAFETY:
                    //
                    // - Requires exclusive access to `CATEGORIES`, which is provided by
                    //   `CATEGORIES_REGISTERED`.
                    unsafe { CATEGORIES[category_index].trace(cb) };
                }
            }
        }
    }
}

/// Determines if a category is enabled.
#[macro_export]
macro_rules! track_event_category_enabled {
    ($category:literal) => {{
        const CATEGORY_INDEX: usize = perfetto_te_ns::category_index($category);
        perfetto_te_ns::is_category_enabled(CATEGORY_INDEX)
    }};
}

/// Sets the callback to invoke when a specific category is enabled.
#[macro_export]
macro_rules! track_event_set_category_callback {
    ($category:literal, $cb:expr) => {{
        const CATEGORY_INDEX: usize = perfetto_te_ns::category_index($category);
        perfetto_te_ns::set_category_callback(CATEGORY_INDEX, $cb);
    }};
}

/// Track event types.
#[derive(Debug, Copy, Clone)]
pub enum TrackEventType {
    /// Instant track event type with a name.
    Instant(*const c_char),
    /// Begin track event type with a name.
    SliceBegin(*const c_char),
    /// End track event type.
    SliceEnd,
    /// Counter track event type.
    Counter,
}

/// Track event timestamp types.
#[derive(Debug, Copy, Clone)]
pub enum TrackEventTimestamp {
    /// Monotonic timestamp.
    Monotonic(Duration),
    /// Boot clock timestamp.
    Boot(Duration),
    /// Incremental timestamp.
    Incremental(Duration),
    /// Absolute timestamp.
    Absolute(Duration),
    /// Custom clock timestamp.
    Custom {
        /// Custom clock ID.
        id: u32,
        /// Timestamp value.
        value: Duration,
    },
}

impl TrackEventTimestamp {
    /// Get a track event timestamp.
    pub fn now() -> Self {
        // SAFETY: Track event machinery must have been initialized.
        let te_timestamp = unsafe { PerfettoTeGetTimestamp() };
        #[allow(non_upper_case_globals)]
        match te_timestamp.clock_id {
            PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_MONOTONIC => {
                TrackEventTimestamp::Monotonic(Duration::from_nanos(te_timestamp.value))
            }
            PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_BOOT => {
                TrackEventTimestamp::Boot(Duration::from_nanos(te_timestamp.value))
            }
            _ => {
                panic!("unexpected clock id: {}", te_timestamp.clock_id);
            }
        }
    }

    /// Returns the timestamp clock ID.
    pub fn clock_id(&self) -> u32 {
        use TrackEventTimestamp::*;
        match &self {
            Monotonic(_) => PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_MONOTONIC,
            Boot(_) => PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_BOOT,
            Incremental(_) => PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_INCREMENTAL,
            Absolute(_) => PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_ABSOLUTE,
            Custom { id, value: _ } => *id,
        }
    }

    /// Returns the timestamp value.
    pub fn timestamp(&self) -> u64 {
        use TrackEventTimestamp::*;
        match &self {
            Monotonic(value) => value.as_nanos() as u64,
            Boot(value) => value.as_nanos() as u64,
            Incremental(value) => value.as_nanos() as u64,
            Absolute(value) => value.as_nanos() as u64,
            Custom { id: _, value } => value.as_nanos() as u64,
        }
    }
}

const COUNTER_MAGIC: u64 = 0xb1a4a67d7970839e;

/// Struct used to represent a track event track.
#[derive(Debug)]
pub struct TrackEventTrack {
    _descriptor: Vec<u8>,
    impl_: PerfettoTeRegisteredTrackImpl,
}

impl TrackEventTrack {
    /// Get the track event UUID for a counter track.
    pub const fn counter_track_uuid(name: &str, parent_uuid: u64) -> u64 {
        let mut uuid = COUNTER_MAGIC;
        uuid ^= parent_uuid;
        uuid ^= fnv1a(name.as_bytes());
        uuid
    }

    /// Get the track event UUID for a named track.
    pub const fn named_track_uuid(name: &str, id: u64, parent_uuid: u64) -> u64 {
        let mut uuid = parent_uuid;
        uuid ^= fnv1a(name.as_bytes());
        uuid ^= id;
        uuid
    }

    /// Get the track event UUID for the current process.
    pub fn process_track_uuid() -> u64 {
        // SAFETY: Track event machinery must have been initialized.
        unsafe { perfetto_te_process_track_uuid }
    }

    /// Register a named track.
    pub fn register_named_track(
        name: &str,
        id: u64,
        parent_track_uuid: u64,
    ) -> Result<Self, TrackEventError> {
        let uuid = Self::named_track_uuid(name, id, parent_track_uuid);
        let writer = PbMsgWriter::new();
        let hb = HeapBuffer::new(&writer.writer);
        let mut msg = PbMsg::new(&writer).unwrap();
        {
            let mut desc = TrackDescriptor { msg: &mut msg };
            desc.set_uuid(uuid);
            if parent_track_uuid != 0 {
                desc.set_parent_uuid(parent_track_uuid);
            }
            desc.set_name(name);
        }
        msg.finalize();
        let descriptor_size = writer.writer.get_written_size();
        let mut descriptor: Vec<u8> = vec![0u8; descriptor_size];
        hb.copy_into(&mut descriptor);
        let descriptor_ptr = descriptor.as_mut_ptr() as *mut c_void;
        let track = Self {
            _descriptor: descriptor,
            impl_: PerfettoTeRegisteredTrackImpl {
                descriptor: descriptor_ptr,
                descriptor_size,
                uuid,
            },
        };
        Ok(track)
    }

    /// Register a counter track.
    pub fn register_counter_track(
        name: &str,
        parent_track_uuid: u64,
    ) -> Result<Self, TrackEventError> {
        let uuid = Self::counter_track_uuid(name, parent_track_uuid);
        let writer = PbMsgWriter::new();
        let hb = HeapBuffer::new(&writer.writer);
        let mut msg = PbMsg::new(&writer).unwrap();
        {
            let mut desc = TrackDescriptor { msg: &mut msg };
            desc.set_uuid(uuid);
            if parent_track_uuid != 0 {
                desc.set_parent_uuid(parent_track_uuid);
            }
            desc.set_name(name);
            desc.set_counter(|counter: &mut CounterDescriptor| {
                counter.set_is_incremental(false);
            });
        }
        msg.finalize();
        let descriptor_size = writer.writer.get_written_size();
        let mut descriptor: Vec<u8> = vec![0u8; descriptor_size];
        hb.copy_into(&mut descriptor);
        let descriptor_ptr = descriptor.as_mut_ptr() as *mut c_void;
        let track = Self {
            _descriptor: descriptor,
            impl_: PerfettoTeRegisteredTrackImpl {
                descriptor: descriptor_ptr,
                descriptor_size,
                uuid,
            },
        };
        Ok(track)
    }

    /// Returns the UUID for the track.
    pub fn uuid(&self) -> u64 {
        self.impl_.uuid
    }
}

/// SAFETY: Internal handle must be thread-safe.
unsafe impl Send for TrackEventTrack {}

/// SAFETY: Internal handle must be thread-safe.
unsafe impl Sync for TrackEventTrack {}

/// Struct used to represent a track event flow.
#[derive(Debug)]
pub struct TrackEventFlow {
    id: u64,
}

impl TrackEventFlow {
    /// Creates a process scoped flow.
    pub fn process_scoped_flow(id: u64) -> TrackEventFlow {
        TrackEventFlow {
            // SAFETY: Track event machinery must have been initialized.
            id: unsafe { id ^ perfetto_te_process_track_uuid },
        }
    }

    /// Creates a global scoped flow.
    pub fn global_flow(id: u64) -> TrackEventFlow {
        TrackEventFlow { id }
    }
}

/// Debug argument types.
#[derive(Debug)]
pub enum TrackEventDebugArg<'a> {
    /// Boolean argument.
    Bool(bool),
    /// Uint64 argument.
    Uint64(u64),
    /// Int64 argument.
    Int64(i64),
    /// Double argument.
    Double(f64),
    /// String argument.
    String(&'a str),
    /// Pointer argument.
    Pointer(usize),
}

/// Counter value types.
#[derive(Debug)]
pub enum TrackEventCounter {
    /// Int64 counter value.
    Int64(i64),
    /// Double counter value.
    Double(f64),
}

// Allow dead code as variants hold data that need to be kept alive.
#[allow(dead_code)]
pub(crate) enum TeHlProtoField {
    Nested(
        PerfettoTeHlProtoFieldNested,
        Vec<TeHlProtoField>,
        Vec<*mut PerfettoTeHlProtoField>,
    ),
    VarInt(PerfettoTeHlProtoFieldVarInt),
    Cstr(PerfettoTeHlProtoFieldCstr, CString),
    Bytes(PerfettoTeHlProtoFieldBytes, Vec<u8>),
}

pub(crate) trait AsTeHlProtoFieldPtr {
    fn as_proto_field_ptr(&mut self) -> *mut PerfettoTeHlProtoField;
}

impl AsTeHlProtoFieldPtr for TeHlProtoField {
    fn as_proto_field_ptr(&mut self) -> *mut PerfettoTeHlProtoField {
        use TeHlProtoField::*;
        match self {
            Nested(field, _, _) => &mut field.header as *mut PerfettoTeHlProtoField,
            VarInt(field) => &mut field.header as *mut PerfettoTeHlProtoField,
            Cstr(field, _) => &mut field.header as *mut PerfettoTeHlProtoField,
            Bytes(field, _) => &mut field.header as *mut PerfettoTeHlProtoField,
        }
    }
}

/// Proto field types.
#[derive(Debug)]
pub enum TrackEventProtoField<'a> {
    /// Nested message type.
    Nested(u32, &'a [TrackEventProtoField<'a>]),
    /// VarInt type.
    VarInt(u32, u64),
    /// String type.
    Cstr(u32, &'a str),
    /// Bytes type.
    Bytes(u32, &'a [u8]),
}

pub(crate) trait ToTeHlProtoField {
    fn to_proto_field(&self) -> TeHlProtoField;
}

impl ToTeHlProtoField for TrackEventProtoField<'_> {
    fn to_proto_field(&self) -> TeHlProtoField {
        use TrackEventProtoField::*;
        use std::{os::raw::c_void, ptr};
        match self {
            Nested(id, nested_fields) => {
                let mut te_fields: Vec<TeHlProtoField> =
                    nested_fields.iter().map(|f| f.to_proto_field()).collect();
                let mut te_field_ptrs: Vec<*mut PerfettoTeHlProtoField> = te_fields
                    .iter_mut()
                    .map(|f| f.as_proto_field_ptr())
                    .collect();
                te_field_ptrs.push(ptr::null_mut());
                TeHlProtoField::Nested(
                    PerfettoTeHlProtoFieldNested {
                        header: PerfettoTeHlProtoField {
                            type_: PerfettoTeHlProtoFieldType_PERFETTO_TE_HL_PROTO_TYPE_NESTED,
                            id: *id,
                        },
                        fields: te_field_ptrs.as_ptr(),
                    },
                    te_fields,
                    te_field_ptrs,
                )
            }
            VarInt(id, value) => TeHlProtoField::VarInt(PerfettoTeHlProtoFieldVarInt {
                header: PerfettoTeHlProtoField {
                    type_: PerfettoTeHlProtoFieldType_PERFETTO_TE_HL_PROTO_TYPE_VARINT,
                    id: *id,
                },
                value: *value,
            }),
            Cstr(id, value) => {
                let cvalue = CString::new(*value).unwrap();
                TeHlProtoField::Cstr(
                    PerfettoTeHlProtoFieldCstr {
                        header: PerfettoTeHlProtoField {
                            type_: PerfettoTeHlProtoFieldType_PERFETTO_TE_HL_PROTO_TYPE_CSTR,
                            id: *id,
                        },
                        str_: cvalue.as_ptr(),
                    },
                    cvalue,
                )
            }
            Bytes(id, value) => {
                let bytes = value.to_vec();
                TeHlProtoField::Bytes(
                    PerfettoTeHlProtoFieldBytes {
                        header: PerfettoTeHlProtoField {
                            type_: PerfettoTeHlProtoFieldType_PERFETTO_TE_HL_PROTO_TYPE_BYTES,
                            id: *id,
                        },
                        buf: bytes.as_ptr() as *const c_void,
                        len: bytes.len(),
                    },
                    bytes,
                )
            }
        }
    }
}

/// Struct containing references to a number of proto fields.
#[derive(Debug)]
pub struct TrackEventProtoFields<'a> {
    /// Proto fields.
    pub fields: &'a [TrackEventProtoField<'a>],
}

/// Struct describing a track using proto fields.
#[derive(Debug)]
pub struct TrackEventProtoTrack<'a> {
    /// Track UUID.
    pub uuid: u64,
    /// Proto fields.
    pub fields: &'a [TrackEventProtoField<'a>],
}

// Allow dead code as variants hold data that need to be kept alive.
#[allow(dead_code)]
pub(crate) enum TeHlNestedTrack {
    Named(PerfettoTeHlNestedTrackNamed, CString),
    Proto(
        PerfettoTeHlNestedTrackProto,
        Vec<TeHlProtoField>,
        Vec<*mut PerfettoTeHlProtoField>,
    ),
    Registered(PerfettoTeHlNestedTrackRegistered),
    Thread(PerfettoTeHlNestedTrack),
    Process(PerfettoTeHlNestedTrack),
}

pub(crate) trait AsTeHlNestedTrackPtr {
    fn as_nested_track_ptr(&mut self) -> *mut PerfettoTeHlNestedTrack;
}

impl AsTeHlNestedTrackPtr for TeHlNestedTrack {
    fn as_nested_track_ptr(&mut self) -> *mut PerfettoTeHlNestedTrack {
        use TeHlNestedTrack::*;
        match self {
            Named(track, _) => &mut track.header as *mut PerfettoTeHlNestedTrack,
            Proto(track, _, _) => &mut track.header as *mut PerfettoTeHlNestedTrack,
            Registered(track) => &mut track.header as *mut PerfettoTeHlNestedTrack,
            Thread(track) => track,
            Process(track) => track,
        }
    }
}

/// Nested track types.
#[derive(Debug)]
pub enum TrackEventNestedTrack<'a> {
    /// Named track.
    Named(&'a str, u64),
    /// Track described by proto fields.
    Proto(u64, &'a [TrackEventProtoField<'a>]),
    /// Registered track.
    Registered(&'a TrackEventTrack),
    /// Current thread track.
    Thread,
    /// Current process track.
    Process,
}

pub(crate) trait ToTeHlNestedTrack {
    fn to_nested_track(&self) -> TeHlNestedTrack;
}

impl ToTeHlNestedTrack for TrackEventNestedTrack<'_> {
    fn to_nested_track(&self) -> TeHlNestedTrack {
        use TrackEventNestedTrack::*;
        use std::ptr;
        match self {
            Named(name, id) => {
                let cname = CString::new(*name).unwrap();
                TeHlNestedTrack::Named(
                    PerfettoTeHlNestedTrackNamed {
                        header: PerfettoTeHlNestedTrack {
                            type_:
                                PerfettoTeHlNestedTrackType_PERFETTO_TE_HL_NESTED_TRACK_TYPE_NAMED,
                        },
                        name: cname.as_ptr(),
                        id: *id,
                    },
                    cname,
                )
            }
            Proto(id, fields) => {
                let mut te_fields: Vec<TeHlProtoField> =
                    fields.iter().map(|f| f.to_proto_field()).collect();
                let mut te_field_ptrs: Vec<*mut PerfettoTeHlProtoField> = te_fields
                    .iter_mut()
                    .map(|f| f.as_proto_field_ptr())
                    .collect();
                te_field_ptrs.push(ptr::null_mut());
                TeHlNestedTrack::Proto(
                    PerfettoTeHlNestedTrackProto {
                        header: PerfettoTeHlNestedTrack {
                            type_:
                                PerfettoTeHlNestedTrackType_PERFETTO_TE_HL_NESTED_TRACK_TYPE_PROTO,
                        },
                        id: *id,
                        fields: te_field_ptrs.as_ptr(),
                    },
                    te_fields,
                    te_field_ptrs,
                )
            }
            Registered(track) => TeHlNestedTrack::Registered(PerfettoTeHlNestedTrackRegistered {
                header: PerfettoTeHlNestedTrack {
                    type_: PerfettoTeHlNestedTrackType_PERFETTO_TE_HL_NESTED_TRACK_TYPE_REGISTERED,
                },
                track: &raw const track.impl_,
            }),
            Thread => TeHlNestedTrack::Thread(PerfettoTeHlNestedTrack {
                type_: PerfettoTeHlNestedTrackType_PERFETTO_TE_HL_NESTED_TRACK_TYPE_THREAD,
            }),
            Process => TeHlNestedTrack::Process(PerfettoTeHlNestedTrack {
                type_: PerfettoTeHlNestedTrackType_PERFETTO_TE_HL_NESTED_TRACK_TYPE_PROCESS,
            }),
        }
    }
}

/// Struct containing references to a hierarchy of nested tracks.
#[derive(Debug)]
pub struct TrackEventNestedTracks<'a> {
    /// The first reference is the outermost track (the parent track), the
    /// (second to) last reference is the innermost track (the child track).
    pub tracks: &'a [TrackEventNestedTrack<'a>],
}

// Allow dead code as variants hold data that need to be kept alive.
#[allow(dead_code)]
pub(crate) enum TeHlExtra {
    Flush(PerfettoTeHlExtra),
    NoIntern(PerfettoTeHlExtra),
    Timestamp(PerfettoTeHlExtraTimestamp),
    DebugArgBool(PerfettoTeHlExtraDebugArgBool, CString),
    DebugArgUint64(PerfettoTeHlExtraDebugArgUint64, CString),
    DebugArgInt64(PerfettoTeHlExtraDebugArgInt64, CString),
    DebugArgDouble(PerfettoTeHlExtraDebugArgDouble, CString),
    DebugArgString(PerfettoTeHlExtraDebugArgString, CString, CString),
    DebugArgPointer(PerfettoTeHlExtraDebugArgPointer, CString),
    Track(PerfettoTeHlExtraRegisteredTrack),
    NamedTrack(PerfettoTeHlExtraNamedTrack, CString),
    Flow(PerfettoTeHlExtraFlow),
    CounterInt64(PerfettoTeHlExtraCounterInt64),
    CounterDouble(PerfettoTeHlExtraCounterDouble),
    ProtoFields(
        PerfettoTeHlExtraProtoFields,
        Vec<TeHlProtoField>,
        Vec<*mut PerfettoTeHlProtoField>,
    ),
    ProtoTrack(
        PerfettoTeHlExtraProtoTrack,
        Vec<TeHlProtoField>,
        Vec<*mut PerfettoTeHlProtoField>,
    ),
    NestedTracks(
        PerfettoTeHlExtraNestedTracks,
        Vec<TeHlNestedTrack>,
        Vec<*mut PerfettoTeHlNestedTrack>,
    ),
}

/// Struct with extra data for a track event instance.
#[derive(Default)]
pub struct EventContext {
    pub(crate) extras: Vec<TeHlExtra>,
}

impl EventContext {
    /// Add flush flag.
    pub fn set_flush(&mut self) -> &mut Self {
        let flush = PerfettoTeHlExtra {
            type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_FLUSH,
        };
        self.extras.push(TeHlExtra::Flush(flush));
        self
    }

    /// Add "no intern" flag.
    pub fn set_no_intern(&mut self) -> &mut Self {
        let no_intern = PerfettoTeHlExtra {
            type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_NO_INTERN,
        };
        self.extras.push(TeHlExtra::NoIntern(no_intern));
        self
    }

    /// Add timestamp.
    pub fn set_timestamp(&mut self, timestamp: TrackEventTimestamp) -> &mut Self {
        use TrackEventTimestamp::*;
        let (clock_id, duration) = match timestamp {
            Monotonic(value) => (
                PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_MONOTONIC,
                value,
            ),
            Boot(value) => (
                PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_BOOT,
                value,
            ),
            Incremental(value) => (
                PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_INCREMENTAL,
                value,
            ),
            Absolute(value) => (
                PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_ABSOLUTE,
                value,
            ),
            Custom { id, value } => (id, value),
        };
        let timestamp = PerfettoTeHlExtraTimestamp {
            header: PerfettoTeHlExtra {
                type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_TIMESTAMP,
            },
            timestamp: PerfettoTeTimestamp {
                clock_id,
                value: duration.as_nanos() as u64,
            },
        };
        self.extras.push(TeHlExtra::Timestamp(timestamp));
        self
    }

    /// Add debug arg.
    pub fn add_debug_arg(&mut self, name: &str, arg: TrackEventDebugArg) -> &mut Self {
        use TrackEventDebugArg::*;
        let cname = CString::new(name).unwrap();
        match arg {
            Bool(value) => {
                let debug_arg = PerfettoTeHlExtraDebugArgBool {
                    header: PerfettoTeHlExtra {
                        type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL,
                    },
                    name: cname.as_ptr(),
                    value,
                };
                self.extras.push(TeHlExtra::DebugArgBool(debug_arg, cname));
            }
            Uint64(value) => {
                let debug_arg = PerfettoTeHlExtraDebugArgUint64 {
                    header: PerfettoTeHlExtra {
                        type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_UINT64,
                    },
                    name: cname.as_ptr(),
                    value,
                };
                self.extras
                    .push(TeHlExtra::DebugArgUint64(debug_arg, cname));
            }
            Int64(value) => {
                let debug_arg = PerfettoTeHlExtraDebugArgInt64 {
                    header: PerfettoTeHlExtra {
                        type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64,
                    },
                    name: cname.as_ptr(),
                    value,
                };
                self.extras.push(TeHlExtra::DebugArgInt64(debug_arg, cname));
            }
            Double(value) => {
                let debug_arg = PerfettoTeHlExtraDebugArgDouble {
                    header: PerfettoTeHlExtra {
                        type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_DOUBLE,
                    },
                    name: cname.as_ptr(),
                    value,
                };
                self.extras
                    .push(TeHlExtra::DebugArgDouble(debug_arg, cname));
            }
            String(value) => {
                let cvalue = CString::new(value).unwrap();
                let debug_arg = PerfettoTeHlExtraDebugArgString {
                    header: PerfettoTeHlExtra {
                        type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING,
                    },
                    name: cname.as_ptr(),
                    value: cvalue.as_ptr(),
                };
                self.extras
                    .push(TeHlExtra::DebugArgString(debug_arg, cname, cvalue));
            }
            Pointer(value) => {
                let debug_arg = PerfettoTeHlExtraDebugArgPointer {
                    header: PerfettoTeHlExtra {
                        type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_POINTER,
                    },
                    name: cname.as_ptr(),
                    value,
                };
                self.extras
                    .push(TeHlExtra::DebugArgPointer(debug_arg, cname));
            }
        }
        self
    }

    /// Add counter value.
    pub fn set_counter(&mut self, counter: TrackEventCounter) -> &mut Self {
        use TrackEventCounter::*;
        match counter {
            Int64(value) => {
                let int_counter = PerfettoTeHlExtraCounterInt64 {
                    header: PerfettoTeHlExtra {
                        type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_INT64,
                    },
                    value,
                };
                self.extras.push(TeHlExtra::CounterInt64(int_counter));
            }
            Double(value) => {
                let double_counter = PerfettoTeHlExtraCounterDouble {
                    header: PerfettoTeHlExtra {
                        type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_DOUBLE,
                    },
                    value,
                };
                self.extras.push(TeHlExtra::CounterDouble(double_counter));
            }
        }
        self
    }

    /// Add track.
    pub fn set_track(&mut self, track: &TrackEventTrack) -> &mut Self {
        let registered_track = PerfettoTeHlExtraRegisteredTrack {
            header: PerfettoTeHlExtra {
                type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_REGISTERED_TRACK,
            },
            track: &raw const track.impl_,
        };
        self.extras.push(TeHlExtra::Track(registered_track));
        self
    }

    /// Add named track.
    pub fn set_named_track(&mut self, name: &str, id: u64, parent_uuid: u64) -> &mut Self {
        let cname = CString::new(name).unwrap();
        let track = PerfettoTeHlExtraNamedTrack {
            header: PerfettoTeHlExtra {
                type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_NAMED_TRACK,
            },
            name: cname.as_ptr(),
            id,
            parent_uuid,
        };
        self.extras.push(TeHlExtra::NamedTrack(track, cname));
        self
    }

    /// Add flow.
    pub fn set_flow(&mut self, flow: &TrackEventFlow) -> &mut Self {
        let begin_flow = PerfettoTeHlExtraFlow {
            header: PerfettoTeHlExtra {
                type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_FLOW,
            },
            id: flow.id,
        };
        self.extras.push(TeHlExtra::Flow(begin_flow));
        self
    }

    /// Add terminating flow.
    pub fn set_terminating_flow(&mut self, flow: &TrackEventFlow) -> &mut Self {
        let terminating_flow = PerfettoTeHlExtraFlow {
            header: PerfettoTeHlExtra {
                type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_TERMINATING_FLOW,
            },
            id: flow.id,
        };
        self.extras.push(TeHlExtra::Flow(terminating_flow));
        self
    }

    /// Add proto fields.
    pub fn set_proto_fields(&mut self, fields: &TrackEventProtoFields) -> &mut Self {
        use std::ptr;

        let mut te_fields: Vec<TeHlProtoField> =
            fields.fields.iter().map(|f| f.to_proto_field()).collect();
        let mut te_field_ptrs: Vec<*mut PerfettoTeHlProtoField> = te_fields
            .iter_mut()
            .map(|f| f.as_proto_field_ptr())
            .collect();
        te_field_ptrs.push(ptr::null_mut());

        let proto_fields = PerfettoTeHlExtraProtoFields {
            header: PerfettoTeHlExtra {
                type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_PROTO_FIELDS,
            },
            fields: te_field_ptrs.as_ptr(),
        };
        self.extras.push(TeHlExtra::ProtoFields(
            proto_fields,
            te_fields,
            te_field_ptrs,
        ));
        self
    }

    /// Add proto track.
    pub fn set_proto_track(&mut self, track: &TrackEventProtoTrack) -> &mut Self {
        use std::ptr;

        let mut te_fields: Vec<TeHlProtoField> =
            track.fields.iter().map(|f| f.to_proto_field()).collect();
        let mut te_field_ptrs: Vec<*mut PerfettoTeHlProtoField> = te_fields
            .iter_mut()
            .map(|f| f.as_proto_field_ptr())
            .collect();
        te_field_ptrs.push(ptr::null_mut());

        let proto_track = PerfettoTeHlExtraProtoTrack {
            header: PerfettoTeHlExtra {
                type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_PROTO_TRACK,
            },
            uuid: track.uuid,
            fields: te_field_ptrs.as_ptr(),
        };
        self.extras
            .push(TeHlExtra::ProtoTrack(proto_track, te_fields, te_field_ptrs));
        self
    }

    /// Add nested track.
    pub fn set_nested_tracks(&mut self, tracks: &TrackEventNestedTracks) -> &mut Self {
        use std::ptr;

        let mut te_tracks: Vec<TeHlNestedTrack> =
            tracks.tracks.iter().map(|f| f.to_nested_track()).collect();
        let mut te_track_ptrs: Vec<*mut PerfettoTeHlNestedTrack> = te_tracks
            .iter_mut()
            .map(|f| f.as_nested_track_ptr())
            .collect();
        te_track_ptrs.push(ptr::null_mut());

        let nested_tracks = PerfettoTeHlExtraNestedTracks {
            header: PerfettoTeHlExtra {
                type_: PerfettoTeHlExtraType_PERFETTO_TE_HL_EXTRA_TYPE_NESTED_TRACKS,
            },
            tracks: te_track_ptrs.as_ptr(),
        };
        self.extras.push(TeHlExtra::NestedTracks(
            nested_tracks,
            te_tracks,
            te_track_ptrs,
        ));
        self
    }
}

/// Emits a track event when `category` is enabled. The optional `lambda` is only called
/// when emitting an event.
#[macro_export]
macro_rules! track_event {
    ($category:literal, $variant:expr) => {{ $crate::track_event!($category, $variant, |_| {}) }};
    ($category:literal, $variant:expr, $lambda:expr) => {{
        const CATEGORY_INDEX: usize = perfetto_te_ns::category_index($category);
        if $crate::__unlikely!(perfetto_te_ns::is_category_enabled(CATEGORY_INDEX)) {
            let mut ctx = $crate::track_event::EventContext::default();

            $lambda(&mut ctx);

            perfetto_te_ns::emit(CATEGORY_INDEX, $variant, &mut ctx);
        }
    }};
}

/// Emits an instant track event when `category` is enabled.
#[macro_export]
macro_rules! track_event_instant {
    ($category:literal, $name:literal) => {{ $crate::track_event_instant!($category, $name, |_| {}) }};
    ($category:literal, $name:literal, $lambda:expr) => {{
        $crate::track_event!(
            $category,
            $crate::track_event::TrackEventType::Instant(
                concat!($name, "\0").as_ptr() as *const std::os::raw::c_char
            ),
            $lambda
        )
    }};
}

/// Emits a begin track event when `category` is enabled.
#[macro_export]
macro_rules! track_event_begin {
    ($category:literal, $name:literal) => {{ $crate::track_event_begin!($category, $name, |_| {}) }};
    ($category:literal, $name:literal, $lambda:expr) => {{
        $crate::track_event!(
            $category,
            $crate::track_event::TrackEventType::SliceBegin(
                concat!($name, "\0").as_ptr() as *const std::os::raw::c_char
            ),
            $lambda
        )
    }};
}

/// Emits an end track event when `category` is enabled.
#[macro_export]
macro_rules! track_event_end {
    ($category:literal) => {{ $crate::track_event_end!($category, |_| {}) }};
    ($category:literal, $lambda:expr) => {{
        $crate::track_event!(
            $category,
            $crate::track_event::TrackEventType::SliceEnd,
            $lambda
        )
    }};
}

/// Emits a counter track event when `category` is enabled.
#[macro_export]
macro_rules! track_event_counter {
    ($category:literal) => {{ $crate::track_event_counter!($category, |_| {}) }};
    ($category:literal, $lambda:expr) => {{
        $crate::track_event!(
            $category,
            $crate::track_event::TrackEventType::Counter,
            $lambda
        )
    }};
}

/// Utility struct used to emit scoped track events.
pub struct ScopeGuard<F: FnOnce()>(Option<F>);

impl<F: FnOnce()> ScopeGuard<F> {
    /// Create a new scope guard that calls `f` when dropped.
    pub fn new(f: F) -> Self {
        Self(Some(f))
    }
}

impl<F: FnOnce()> Drop for ScopeGuard<F> {
    fn drop(&mut self) {
        if let Some(f) = self.0.take() {
            f();
        }
    }
}

/// Emits a pair of begin/end track events when `category` is enabled.
/// The end event is emitted when the current scope ends.
#[macro_export]
macro_rules! scoped_track_event {
    ($category:expr, $name:literal) => {
        $crate::scoped_track_event!($category, $name, |_| {}, |_| {})
    };
    ($category:expr, $name:literal, $lambda:expr) => {
        $crate::scoped_track_event!($category, $name, $lambda, |_| {})
    };
    ($category:expr, $name:literal, $begin_lambda:expr, $end_lambda:expr) => {
        $crate::track_event_begin!($category, $name, $begin_lambda);
        let __scope_guard = $crate::track_event::ScopeGuard::new(|| {
            $crate::track_event_end!($category, $end_lambda)
        });
    };
}

/// Calls `lambda` for each active instance where `category` is enabled.
#[macro_export]
macro_rules! trace_for_category {
    ($category:literal, $lambda:expr) => {{
        const CATEGORY_INDEX: usize = perfetto_te_ns::category_index($category);
        if $crate::__unlikely!(perfetto_te_ns::is_category_enabled(CATEGORY_INDEX)) {
            perfetto_te_ns::trace(CATEGORY_INDEX, $lambda);
        }
    }};
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pb_decoder::{PbDecoder, PbDecoderField};
    use crate::protos::trace::track_event::debug_annotation::*;
    use crate::protos::trace::track_event::track_event::TrackEventFieldNumber;
    use crate::protos::trace::track_event::track_event::TrackEventType as EventType;
    use crate::tests::{TracingSessionBuilder, acquire_test_environment};
    use crate::tracing_session::TracingSession;
    use std::{error::Error, sync::MutexGuard};

    track_event_categories! {
        pub mod test_te_ns {
            ( "cat1", "Test category 1", [ "tag1" ] ),
            ( "cat2", "Test category 2", [ "tag2", "tag3" ] ),
        }
    }

    #[test]
    fn category_index() {
        assert_eq!(test_te_ns::category_index("cat1"), 0);
        assert_eq!(test_te_ns::category_index("cat2"), 1);
    }

    struct TeTestFixture {
        _lock: MutexGuard<'static, ()>,
    }

    impl TeTestFixture {
        fn new() -> Self {
            let _lock = acquire_test_environment();
            TrackEvent::init();
            test_te_ns::register().expect("register failed");
            Self { _lock }
        }
    }

    impl Drop for TeTestFixture {
        fn drop(&mut self) {
            test_te_ns::unregister().expect("unregister failed");
        }
    }

    #[derive(Default, Clone)]
    struct DebugAnnotation {
        bool_value: Option<bool>,
        uint64_value: Option<u64>,
        int64_value: Option<i64>,
        double_value: Option<f64>,
        string_value: Option<String>,
        pointer_value: Option<u64>,
    }

    impl DebugAnnotation {
        fn decode(data: &[u8]) -> Self {
            use PbDecoderField::*;
            let mut da = DebugAnnotation::default();
            const BOOL_VALUE_ID: u32 = DebugAnnotationFieldNumber::BoolValue as u32;
            const UINT_VALUE_ID: u32 = DebugAnnotationFieldNumber::UintValue as u32;
            const INT_VALUE_ID: u32 = DebugAnnotationFieldNumber::IntValue as u32;
            const DOUBLE_VALUE_ID: u32 = DebugAnnotationFieldNumber::DoubleValue as u32;
            const STRING_VALUE_ID: u32 = DebugAnnotationFieldNumber::StringValue as u32;
            const POINTER_VALUE_ID: u32 = DebugAnnotationFieldNumber::PointerValue as u32;
            for field in PbDecoder::new(data) {
                match field.as_ref().unwrap_or_else(|e| panic!("Error: {}", e)) {
                    (BOOL_VALUE_ID, Varint(v)) => da.bool_value = Some(*v != 0),
                    (UINT_VALUE_ID, Varint(v)) => da.uint64_value = Some(*v),
                    (INT_VALUE_ID, Varint(v)) => da.int64_value = Some(*v as i64),
                    (DOUBLE_VALUE_ID, Fixed64(v)) => da.double_value = Some(f64::from_bits(*v)),
                    (STRING_VALUE_ID, Delimited(v)) => {
                        da.string_value = Some(String::from_utf8(v.to_vec()).unwrap())
                    }
                    (POINTER_VALUE_ID, Varint(v)) => da.pointer_value = Some(*v),
                    _ => println!("WARNING: unknown DebugAnnotation field: {:?}", field),
                }
            }
            da
        }
    }

    #[derive(Default, Clone)]
    struct Event {
        timestamp: u64,
        category_iids: Option<u64>,
        name_iid: Option<u64>,
        name: Option<String>,
        r#type: Option<EventType>,
        counter_value: Option<i64>,
        debug_annotations: Vec<DebugAnnotation>,
    }

    impl Event {
        fn decode(data: &[u8]) -> Self {
            use PbDecoderField::*;
            let mut event = Event::default();
            const CATEGORY_IIDS_ID: u32 = TrackEventFieldNumber::CategoryIids as u32;
            const NAME_IID_ID: u32 = TrackEventFieldNumber::NameIid as u32;
            const NAME_ID: u32 = TrackEventFieldNumber::Name as u32;
            const TYPE_ID: u32 = TrackEventFieldNumber::Type as u32;
            const COUNTER_VALUE_ID: u32 = TrackEventFieldNumber::CounterValue as u32;
            const DEBUG_ANNOTATIONS_ID: u32 = TrackEventFieldNumber::DebugAnnotations as u32;
            for field in PbDecoder::new(data) {
                match field.as_ref().unwrap_or_else(|e| panic!("Error: {}", e)) {
                    (CATEGORY_IIDS_ID, Varint(v)) => event.category_iids = Some(*v),
                    (NAME_IID_ID, Varint(v)) => event.name_iid = Some(*v),
                    (NAME_ID, Delimited(v)) => {
                        event.name = Some(String::from_utf8(v.to_vec()).unwrap())
                    }
                    (TYPE_ID, Varint(v)) => {
                        event.r#type = Some(EventType::try_from(*v as u32).unwrap())
                    }
                    (COUNTER_VALUE_ID, Varint(v)) => event.counter_value = Some(*v as i64),
                    (DEBUG_ANNOTATIONS_ID, Delimited(v)) => {
                        event.debug_annotations.push(DebugAnnotation::decode(v))
                    }
                    _ => println!("WARNING: unknown TrackEvent field: {:?}", field),
                }
            }
            event
        }
    }

    fn read_trace_events(tracing_session: &mut TracingSession) -> Vec<Event> {
        use crate::protos::trace::{trace::*, trace_packet::*};
        use PbDecoderField::*;
        use std::sync::{Arc, Mutex};
        let trace_data = Arc::new(Mutex::new(vec![]));
        let trace_data_for_write = Arc::clone(&trace_data);
        tracing_session.read_trace_blocking(move |data, _end| {
            let mut written_data = trace_data_for_write.lock().unwrap();
            written_data.extend_from_slice(data);
        });
        let data = trace_data.lock().unwrap();
        let mut events = vec![];
        const PACKET_ID: u32 = TraceFieldNumber::Packet as u32;
        for trace_field in PbDecoder::new(&data) {
            if let (PACKET_ID, PbDecoderField::Delimited(data)) = trace_field.unwrap() {
                const TIMESTAMP_ID: u32 = TracePacketFieldNumber::Timestamp as u32;
                const TRACK_EVENT_ID: u32 = TracePacketFieldNumber::TrackEvent as u32;
                let mut timestamp = 0;
                let mut event = None;
                for packet_field in PbDecoder::new(data) {
                    match packet_field
                        .as_ref()
                        .unwrap_or_else(|e| panic!("Error: {}", e))
                    {
                        (TIMESTAMP_ID, Varint(v)) => timestamp = *v,
                        (TRACK_EVENT_ID, Delimited(v)) => event = Some(Event::decode(v)),
                        // Ignore all other packet fields.
                        _ => {}
                    }
                }
                if let Some(event) = &mut event {
                    event.timestamp = timestamp;
                    events.push(event.clone());
                }
            }
        }
        events
    }

    #[test]
    fn register_track() -> Result<(), Box<dyn Error>> {
        let _fx = TeTestFixture::new();
        let named_track = TrackEventTrack::register_named_track(
            "mytrack",
            123,
            TrackEventTrack::process_track_uuid(),
        )?;
        assert!(named_track.uuid() != 0);
        let counter_track = TrackEventTrack::register_counter_track(
            "mycounter",
            TrackEventTrack::process_track_uuid(),
        )?;
        assert!(counter_track.uuid() != 0);
        assert!(counter_track.uuid() != named_track.uuid());
        Ok(())
    }

    #[test]
    fn is_category_enabled() -> Result<(), Box<dyn Error>> {
        use test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat1")
            .add_disabled_category("*")
            .build()?;
        assert!(!track_event_category_enabled!("cat1"));
        assert!(!track_event_category_enabled!("cat2"));
        session.start_blocking();
        assert!(track_event_category_enabled!("cat1"));
        assert!(!track_event_category_enabled!("cat2"));
        session.stop_blocking();
        assert!(!track_event_category_enabled!("cat1"));
        assert!(!track_event_category_enabled!("cat2"));
        Ok(())
    }

    #[test]
    fn category_callback() -> Result<(), Box<dyn Error>> {
        use std::sync::{
            Arc,
            atomic::{AtomicUsize, Ordering},
        };
        use test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat1")
            .add_disabled_category("*")
            .build()?;
        session.start_blocking();
        let executed = Arc::new(AtomicUsize::new(0));
        let executed_for_callback = Arc::clone(&executed);
        track_event_set_category_callback!(
            "cat1",
            move |_inst_id, enabled, _global_state_changed| {
                if enabled {
                    executed_for_callback.fetch_add(1, Ordering::Relaxed);
                }
            }
        );
        session.stop_blocking();
        assert_eq!(executed.load(Ordering::Relaxed), 1);
        Ok(())
    }

    #[test]
    fn instant() -> Result<(), Box<dyn Error>> {
        use test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat1")
            .add_disabled_category("*")
            .build()?;
        session.start_blocking();
        track_event_instant!("cat1", "name1");
        session.stop_blocking();
        let events = read_trace_events(&mut session);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].r#type, Some(EventType::TypeInstant));
        assert!(events[0].name_iid.is_some());
        Ok(())
    }

    #[test]
    fn no_intern() -> Result<(), Box<dyn Error>> {
        use test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat1")
            .add_disabled_category("*")
            .build()?;
        session.start_blocking();
        track_event_instant!("cat1", "name1", |ctx: &mut EventContext| {
            ctx.set_no_intern();
        });
        session.stop_blocking();
        let events = read_trace_events(&mut session);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].r#type, Some(EventType::TypeInstant));
        assert_eq!(events[0].name, Some("name1".to_string()));
        Ok(())
    }

    #[test]
    fn slice() -> Result<(), Box<dyn Error>> {
        use test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat1")
            .add_disabled_category("*")
            .build()?;
        session.start_blocking();
        track_event_begin!("cat1", "name2");
        track_event_end!("cat1");
        session.stop_blocking();
        let events = read_trace_events(&mut session);
        assert_eq!(events.len(), 2);
        assert_eq!(events[0].r#type, Some(EventType::TypeSliceBegin));
        assert!(events[0].name_iid.is_some());
        assert_eq!(events[1].r#type, Some(EventType::TypeSliceEnd));
        assert!(events[1].name_iid.is_none());
        Ok(())
    }

    #[test]
    fn counter() -> Result<(), Box<dyn Error>> {
        use test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat2")
            .add_disabled_category("*")
            .build()?;
        session.start_blocking();
        track_event_counter!("cat2", |ctx: &mut EventContext| {
            ctx.set_counter(TrackEventCounter::Int64(56));
        });
        session.stop_blocking();
        let events = read_trace_events(&mut session);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].r#type, Some(EventType::TypeCounter));
        assert_eq!(events[0].counter_value, Some(56));
        Ok(())
    }

    #[test]
    fn with_timestamp() -> Result<(), Box<dyn Error>> {
        use test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat2")
            .add_disabled_category("*")
            .build()?;
        session.start_blocking();
        let te_timestamp = TrackEventTimestamp::now();
        track_event_instant!("cat2", "name3", |ctx: &mut EventContext| {
            ctx.set_timestamp(te_timestamp);
        });
        session.stop_blocking();
        let events = read_trace_events(&mut session);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].r#type, Some(EventType::TypeInstant));
        assert_eq!(events[0].timestamp, te_timestamp.timestamp());
        Ok(())
    }

    #[test]
    fn with_debug_args() -> Result<(), Box<dyn Error>> {
        use test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat2")
            .add_disabled_category("*")
            .build()?;
        session.start_blocking();
        track_event_instant!("cat2", "name4", |ctx: &mut EventContext| {
            // Use optional builder pattern to set debug arguments.
            ctx.add_debug_arg("dbg_arg1", TrackEventDebugArg::Bool(true))
                .add_debug_arg("dbg_arg2", TrackEventDebugArg::Uint64(1234))
                .add_debug_arg("dbg_arg3", TrackEventDebugArg::Int64(-1234))
                .add_debug_arg("dbg_arg4", TrackEventDebugArg::Double(std::f64::consts::PI))
                .add_debug_arg(
                    "dbg_arg5",
                    TrackEventDebugArg::String("this is a string value"),
                )
                .add_debug_arg(
                    "dbg_arg6",
                    TrackEventDebugArg::Pointer("random".as_ptr() as usize),
                );
        });
        session.stop_blocking();
        let events = read_trace_events(&mut session);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].r#type, Some(EventType::TypeInstant));
        assert_eq!(events[0].debug_annotations.len(), 6);
        assert_eq!(events[0].debug_annotations[0].bool_value, Some(true));
        assert_eq!(events[0].debug_annotations[1].uint64_value, Some(1234));
        assert_eq!(events[0].debug_annotations[2].int64_value, Some(-1234));
        assert_eq!(
            events[0].debug_annotations[3].double_value,
            Some(std::f64::consts::PI)
        );
        assert_eq!(
            events[0].debug_annotations[4].string_value,
            Some("this is a string value".to_string())
        );
        assert!(events[0].debug_annotations[5].pointer_value.is_some());
        Ok(())
    }

    #[test]
    fn with_dynamic_name() -> Result<(), Box<dyn Error>> {
        use test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat2")
            .add_disabled_category("*")
            .build()?;
        session.start_blocking();
        let cname = CString::new("dynamic_name").unwrap();
        track_event!("cat2", TrackEventType::Instant(cname.as_ptr()));
        session.stop_blocking();
        let events = read_trace_events(&mut session);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].r#type, Some(EventType::TypeInstant));
        assert!(events[0].name_iid.is_some());
        Ok(())
    }

    #[test]
    fn scoped() -> Result<(), Box<dyn Error>> {
        use test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat1")
            .add_disabled_category("*")
            .build()?;
        session.start_blocking();
        {
            scoped_track_event!("cat1", "name5");
            {
                scoped_track_event!("cat1", "name6", |ctx: &mut EventContext| {
                    ctx.add_debug_arg("scoped_dbg_arg", TrackEventDebugArg::Bool(true));
                });
                scoped_track_event!(
                    "cat1",
                    "name7",
                    |ctx: &mut EventContext| {
                        ctx.add_debug_arg("scoped_begin_dbg_arg", TrackEventDebugArg::Bool(true));
                    },
                    |ctx: &mut EventContext| {
                        ctx.add_debug_arg("scoped_end_dbg_arg", TrackEventDebugArg::Bool(false));
                    }
                );
            }
        }
        session.stop_blocking();
        let events = read_trace_events(&mut session);
        assert_eq!(events.len(), 6);
        assert_eq!(events[0].r#type, Some(EventType::TypeSliceBegin));
        assert_eq!(events[1].r#type, Some(EventType::TypeSliceBegin));
        assert_eq!(events[1].debug_annotations.len(), 1);
        assert_eq!(events[1].debug_annotations[0].bool_value, Some(true));
        assert_eq!(events[2].r#type, Some(EventType::TypeSliceBegin));
        assert_eq!(events[2].debug_annotations.len(), 1);
        assert_eq!(events[2].debug_annotations[0].bool_value, Some(true));
        assert_eq!(events[3].r#type, Some(EventType::TypeSliceEnd));
        assert_eq!(events[3].debug_annotations.len(), 1);
        assert_eq!(events[3].debug_annotations[0].bool_value, Some(false));
        assert_eq!(events[4].r#type, Some(EventType::TypeSliceEnd));
        assert_eq!(events[5].r#type, Some(EventType::TypeSliceEnd));
        Ok(())
    }

    const CUSTOM_CLOCK_ID: u32 = 123456;

    #[test]
    fn trace_for_category() -> Result<(), Box<dyn Error>> {
        use crate::protos::trace::{clock_snapshot::*, trace::*, trace_packet::*};
        use std::sync::{Arc, Mutex};
        use test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat1")
            .add_disabled_category("*")
            .build()?;
        session.start_blocking();
        trace_for_category!("cat1", |ctx: &mut TraceContext| {
            ctx.add_packet(|packet: &mut TracePacket| {
                packet
                    .set_timestamp_clock_id(PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_BOOT)
                    .set_timestamp(42)
                    .set_clock_snapshot(|clock_snapshot: &mut ClockSnapshot| {
                        clock_snapshot
                            .set_clocks(|clock: &mut Clock| {
                                clock.set_clock_id(
                                    PerfettoTeTimestampType_PERFETTO_TE_TIMESTAMP_TYPE_BOOT,
                                );
                                clock.set_timestamp(42);
                            })
                            .set_clocks(|clock: &mut Clock| {
                                clock.set_clock_id(CUSTOM_CLOCK_ID);
                                clock.set_timestamp(10000);
                            });
                    });
            });
        });
        session.stop_blocking();
        let trace_data = Arc::new(Mutex::new(vec![]));
        let trace_data_for_write = Arc::clone(&trace_data);
        session.read_trace_blocking(move |data, _end| {
            let mut written_data = trace_data_for_write.lock().unwrap();
            written_data.extend_from_slice(data);
        });
        let data = trace_data.lock().unwrap();
        assert!(!data.is_empty());
        let mut clock_snapshot_found = false;
        for trace_field in PbDecoder::new(&data) {
            const PACKET_ID: u32 = TraceFieldNumber::Packet as u32;
            if let (PACKET_ID, PbDecoderField::Delimited(data)) = trace_field.unwrap() {
                for packet_field in PbDecoder::new(data) {
                    const CLOCK_SNAPSHOT_ID: u32 = TracePacketFieldNumber::ClockSnapshot as u32;
                    if let (CLOCK_SNAPSHOT_ID, PbDecoderField::Delimited(_)) = packet_field.unwrap()
                    {
                        clock_snapshot_found = true;
                    }
                }
            }
        }
        assert!(clock_snapshot_found);
        Ok(())
    }
}
