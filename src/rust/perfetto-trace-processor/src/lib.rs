// Copyright (C) 2026 The Android Open Source Project
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

//! Safe Rust bindings for Perfetto's Trace Processor.
//!
//! ```no_run
//! use perfetto_trace_processor::TraceProcessor;
//!
//! let tp = TraceProcessor::open("trace.pb")?;
//! let mut rows = tp.query("select name, dur from slice limit 10")?;
//! while let Some(row) = rows.next()? {
//!     let name: &str = row.get(0)?;
//!     let dur: i64 = row.get(1)?;
//!     println!("{name} {dur}");
//! }
//! # Ok::<(), perfetto_trace_processor::Error>(())
//! ```
//!
//! None of the types are `Send` or `Sync`.

use std::ffi::{CStr, CString, c_void};
use std::fmt;
use std::fs::File;
use std::io::Read;
use std::marker::PhantomData;
use std::ops::Range;
use std::path::Path;
use std::ptr::NonNull;
use std::sync::Arc;

use memmap2::Mmap;
use perfetto_trace_processor_sys as sys;

/// An error returned by Trace Processor.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    message: String,
}

impl Error {
    fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }

    pub fn message(&self) -> &str {
        &self.message
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// Converts a `PerfettoTpError*` returned by the C API, taking ownership.
fn check(err: *mut sys::PerfettoTpError) -> Result<()> {
    if err.is_null() {
        return Ok(());
    }
    // SAFETY: `err` is a valid error owned by us.
    let message = unsafe {
        let msg = CStr::from_ptr(sys::PerfettoTpErrorMessage(err))
            .to_string_lossy()
            .into_owned();
        sys::PerfettoTpErrorDestroy(err);
        msg
    };
    Err(Error::new(message))
}

/// Owns a `PerfettoTp`.
struct Handle(NonNull<sys::PerfettoTp>);

impl Handle {
    fn as_ptr(&self) -> *mut sys::PerfettoTp {
        self.0.as_ptr()
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        // SAFETY: the pointer is valid and no `Rows` can outlive the handle.
        unsafe { sys::PerfettoTpDestroy(self.0.as_ptr()) }
    }
}

/// Size of the chunks a trace is parsed in, as ReadTrace() does in C++.
const READ_CHUNK_SIZE: usize = 1024 * 1024;
const MMAP_CHUNK_SIZE: usize = 128 * 1024 * 1024;

/// A chunk of a memory-mapped file, keeping the whole mapping alive.
struct MmapChunk {
    mmap: Arc<Mmap>,
    range: Range<usize>,
}

impl AsRef<[u8]> for MmapChunk {
    fn as_ref(&self) -> &[u8] {
        &self.mmap[self.range.clone()]
    }
}

/// A loaded trace which can be queried with PerfettoSQL.
pub struct TraceProcessor {
    handle: Handle,
}

impl TraceProcessor {
    /// Loads a trace by reading `reader` until the end.
    pub fn from_reader(mut reader: impl Read) -> Result<Self> {
        let mut tp = Self::create();
        loop {
            let mut chunk = Vec::with_capacity(READ_CHUNK_SIZE);
            let size = (&mut reader)
                .take(READ_CHUNK_SIZE as u64)
                .read_to_end(&mut chunk)
                .map_err(|e| Error::new(format!("failed to read trace: {e}")))?;
            if size == 0 {
                break;
            }
            tp.parse(chunk)?;
        }
        tp.notify_end_of_file()?;
        Ok(tp)
    }

    /// Loads the trace file at `path` by memory-mapping it.
    ///
    /// The file must not be modified while it is being loaded or queried.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref();
        let io_err = |e: std::io::Error| Error::new(format!("{}: {e}", path.display()));
        let file = File::open(path).map_err(io_err)?;
        // SAFETY: see the requirement documented above.
        let mmap = Arc::new(unsafe { Mmap::map(&file) }.map_err(io_err)?);
        let mut tp = Self::create();
        for start in (0..mmap.len()).step_by(MMAP_CHUNK_SIZE) {
            let end = (start + MMAP_CHUNK_SIZE).min(mmap.len());
            tp.parse(MmapChunk {
                mmap: mmap.clone(),
                range: start..end,
            })?;
        }
        tp.notify_end_of_file()?;
        Ok(tp)
    }

    fn create() -> Self {
        // SAFETY: always returns a valid instance.
        let ptr = unsafe { sys::PerfettoTpCreate() };
        Self {
            handle: Handle(NonNull::new(ptr).expect("unexpected null trace processor")),
        }
    }

    /// Pushes a chunk of trace data without copying it: Trace Processor keeps
    /// `data` alive for as long as it references the bytes.
    fn parse<T: AsRef<[u8]> + 'static>(&mut self, data: T) -> Result<()> {
        unsafe extern "C" fn drop_owner<T>(ctx: *mut c_void) {
            // SAFETY: `ctx` is the `Box<T>` leaked below, released once.
            drop(unsafe { Box::from_raw(ctx as *mut T) });
        }
        let owner = Box::new(data);
        let bytes = (*owner).as_ref();
        let (ptr, len) = (bytes.as_ptr(), bytes.len());
        // SAFETY: the bytes are owned by the boxed `T`, which doesn't move and
        // is only dropped by `drop_owner`.
        check(unsafe {
            sys::PerfettoTpParse(
                self.handle.as_ptr(),
                ptr,
                len,
                Box::into_raw(owner) as *mut c_void,
                Some(drop_owner::<T>),
            )
        })
    }

    fn notify_end_of_file(&mut self) -> Result<()> {
        // SAFETY: the handle is valid.
        check(unsafe { sys::PerfettoTpNotifyEndOfFile(self.handle.as_ptr()) })
    }

    /// Executes `sql`, which can contain multiple semicolon-separated
    /// statements, and returns the rows of the last one.
    pub fn query(&self, sql: &str) -> Result<Rows<'_>> {
        let sql = CString::new(sql).map_err(|_| Error::new("SQL contains a NUL byte"))?;
        // SAFETY: the handle is valid and `sql` is NUL-terminated.
        let ptr = unsafe { sys::PerfettoTpExecuteQuery(self.handle.as_ptr(), sql.as_ptr()) };
        let mut rows = Rows {
            ptr: NonNull::new(ptr).expect("unexpected null iterator"),
            column_names: Vec::new(),
            _tp: PhantomData,
        };
        rows.status()?;
        let it = rows.ptr.as_ptr();
        // SAFETY: `it` is valid, the column is in bounds and each name is
        // copied before the next call invalidates it.
        rows.column_names = unsafe {
            (0..sys::PerfettoTpIteratorColumnCount(it))
                .map(|i| {
                    CStr::from_ptr(sys::PerfettoTpIteratorColumnName(it, i))
                        .to_string_lossy()
                        .into_owned()
                })
                .collect()
        };
        Ok(rows)
    }
}

/// The rows returned by [`TraceProcessor::query`].
pub struct Rows<'tp> {
    ptr: NonNull<sys::PerfettoTpIterator>,
    column_names: Vec<String>,
    _tp: PhantomData<&'tp TraceProcessor>,
}

impl<'tp> Rows<'tp> {
    pub fn column_names(&self) -> &[String] {
        &self.column_names
    }

    /// Advances to the next row, returning `None` once all rows were read.
    #[allow(clippy::should_implement_trait)]
    pub fn next(&mut self) -> Result<Option<Row<'_>>> {
        // SAFETY: `self.ptr` is valid.
        if unsafe { sys::PerfettoTpIteratorNext(self.ptr.as_ptr()) } {
            return Ok(Some(Row { rows: self }));
        }
        self.status()?;
        Ok(None)
    }

    fn status(&self) -> Result<()> {
        // SAFETY: `self.ptr` is valid.
        check(unsafe { sys::PerfettoTpIteratorStatus(self.ptr.as_ptr()) })
    }
}

impl Drop for Rows<'_> {
    fn drop(&mut self) {
        // SAFETY: `self.ptr` is valid and owned by us.
        unsafe { sys::PerfettoTpIteratorDestroy(self.ptr.as_ptr()) }
    }
}

/// The current row of a [`Rows`]. Valid until the next call to
/// [`Rows::next`].
pub struct Row<'r> {
    rows: &'r Rows<'r>,
}

impl<'r> Row<'r> {
    pub fn column_count(&self) -> usize {
        self.rows.column_names.len()
    }

    /// Reads column `col` as `T`, fetching only what `T` needs. Returns an
    /// error if `col` is out of bounds or the value can't be converted.
    ///
    /// Use `Option<T>` for nullable columns and [`Value`] for columns whose
    /// type isn't known in advance.
    pub fn get<T: FromValue<'r>>(&self, col: usize) -> Result<T> {
        if col >= self.column_count() {
            return Err(Error::new(format!("column {col} out of bounds")));
        }
        T::from_value(ValueRef {
            it: self.rows.ptr,
            col: col as u32,
            _row: PhantomData,
        })
    }
}

/// The type of a value in a query result.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValueType {
    Null,
    Int,
    Double,
    Text,
    Bytes,
}

/// A handle to a single value of the current row, used by [`FromValue`] to
/// fetch only what it needs.
#[derive(Clone, Copy)]
pub struct ValueRef<'r> {
    it: NonNull<sys::PerfettoTpIterator>,
    col: u32,
    _row: PhantomData<&'r ()>,
}

impl<'r> ValueRef<'r> {
    pub fn value_type(&self) -> ValueType {
        // SAFETY: `it` is valid, points at a row and `col` is in bounds.
        match unsafe { sys::PerfettoTpIteratorGetType(self.it.as_ptr(), self.col) } {
            sys::PerfettoTpValueType_PERFETTO_TP_VALUE_TYPE_INT64 => ValueType::Int,
            sys::PerfettoTpValueType_PERFETTO_TP_VALUE_TYPE_DOUBLE => ValueType::Double,
            sys::PerfettoTpValueType_PERFETTO_TP_VALUE_TYPE_STRING => ValueType::Text,
            sys::PerfettoTpValueType_PERFETTO_TP_VALUE_TYPE_BYTES => ValueType::Bytes,
            _ => ValueType::Null,
        }
    }

    fn expect(&self, expected: ValueType) -> Result<()> {
        let actual = self.value_type();
        if actual != expected {
            return Err(Error::new(format!(
                "column {}: expected {expected:?}, got {actual:?}",
                self.col
            )));
        }
        Ok(())
    }

    pub fn as_i64(&self) -> Result<i64> {
        self.expect(ValueType::Int)?;
        // SAFETY: the type matches.
        Ok(unsafe { sys::PerfettoTpIteratorGetInt64(self.it.as_ptr(), self.col) })
    }

    pub fn as_f64(&self) -> Result<f64> {
        self.expect(ValueType::Double)?;
        // SAFETY: the type matches.
        Ok(unsafe { sys::PerfettoTpIteratorGetDouble(self.it.as_ptr(), self.col) })
    }

    /// Returns the raw text, which is usually but not necessarily UTF-8.
    pub fn as_text(&self) -> Result<&'r [u8]> {
        self.expect(ValueType::Text)?;
        // SAFETY: the type matches and the string stays valid until the next
        // `Rows::next()`, which can't be called while the row is borrowed.
        Ok(
            unsafe { CStr::from_ptr(sys::PerfettoTpIteratorGetString(self.it.as_ptr(), self.col)) }
                .to_bytes(),
        )
    }

    pub fn as_str(&self) -> Result<&'r str> {
        std::str::from_utf8(self.as_text()?)
            .map_err(|e| Error::new(format!("column {}: {e}", self.col)))
    }

    pub fn as_bytes(&self) -> Result<&'r [u8]> {
        self.expect(ValueType::Bytes)?;
        let mut size = 0;
        // SAFETY: the type matches and the bytes stay valid until the next
        // `Rows::next()`, which can't be called while the row is borrowed.
        unsafe {
            let data = sys::PerfettoTpIteratorGetBytes(self.it.as_ptr(), self.col, &mut size);
            Ok(if size == 0 {
                &[]
            } else {
                std::slice::from_raw_parts(data, size)
            })
        }
    }
}

/// Types which can be read from a query result with [`Row::get`].
pub trait FromValue<'r>: Sized {
    fn from_value(value: ValueRef<'r>) -> Result<Self>;
}

impl<'r> FromValue<'r> for i64 {
    fn from_value(value: ValueRef<'r>) -> Result<Self> {
        value.as_i64()
    }
}

impl<'r> FromValue<'r> for f64 {
    fn from_value(value: ValueRef<'r>) -> Result<Self> {
        value.as_f64()
    }
}

impl<'r> FromValue<'r> for &'r str {
    fn from_value(value: ValueRef<'r>) -> Result<Self> {
        value.as_str()
    }
}

impl<'r> FromValue<'r> for &'r [u8] {
    fn from_value(value: ValueRef<'r>) -> Result<Self> {
        value.as_bytes()
    }
}

impl<'r> FromValue<'r> for String {
    fn from_value(value: ValueRef<'r>) -> Result<Self> {
        value.as_str().map(str::to_owned)
    }
}

impl<'r, T: FromValue<'r>> FromValue<'r> for Option<T> {
    fn from_value(value: ValueRef<'r>) -> Result<Self> {
        match value.value_type() {
            ValueType::Null => Ok(None),
            _ => T::from_value(value).map(Some),
        }
    }
}

impl<'r> FromValue<'r> for Value<'r> {
    fn from_value(value: ValueRef<'r>) -> Result<Self> {
        Ok(match value.value_type() {
            ValueType::Null => Value::Null,
            ValueType::Int => Value::Int(value.as_i64()?),
            ValueType::Double => Value::Double(value.as_f64()?),
            ValueType::Text => Value::Text(value.as_text()?),
            ValueType::Bytes => Value::Bytes(value.as_bytes()?),
        })
    }
}

/// A dynamically typed value in a query result, borrowed from the current row.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Value<'a> {
    Null,
    Int(i64),
    Double(f64),
    /// SQL text, usually but not necessarily UTF-8.
    Text(&'a [u8]),
    Bytes(&'a [u8]),
}
