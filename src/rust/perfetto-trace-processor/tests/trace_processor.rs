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

use std::fs::File;
use std::io::Read;
use std::path::PathBuf;

use perfetto_trace_processor::{TraceProcessor, Value};

const SYSTRACE: &str = "# tracer: nop\n  \
    sh-1 (1) [000] .... 10.000000: sched_switch: prev_comm=sh prev_pid=1 \
    prev_prio=120 prev_state=S ==> next_comm=foo next_pid=2 next_prio=120\n  \
    foo-2 (2) [000] .... 11.000000: sched_switch: prev_comm=foo prev_pid=2 \
    prev_prio=120 prev_state=S ==> next_comm=sh next_pid=1 next_prio=120\n";

fn test_trace() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../test/data/example_android_trace_30s.pb")
}

fn query_int(tp: &TraceProcessor, sql: &str) -> i64 {
    let mut rows = tp.query(sql).unwrap();
    let row = rows.next().unwrap().expect("no rows");
    row.get(0).unwrap()
}

/// A reader returning at most 7 bytes per read, to exercise chunking.
struct SlowReader<R>(R);

impl<R: Read> Read for SlowReader<R> {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let len = buf.len().min(7);
        self.0.read(&mut buf[..len])
    }
}

#[test]
fn query_values() {
    let tp = TraceProcessor::from_reader(SYSTRACE.as_bytes()).unwrap();
    let mut rows = tp
        .query("select name, 42 as i, 1.5 as d, null as n, x'0102' as b from thread where name = 'foo'")
        .unwrap();
    assert_eq!(rows.column_names(), ["name", "i", "d", "n", "b"]);

    let row = rows.next().unwrap().unwrap();
    assert_eq!(row.get::<&str>(0).unwrap(), "foo");
    assert_eq!(row.get::<String>(0).unwrap(), "foo");
    assert_eq!(row.get::<i64>(1).unwrap(), 42);
    assert_eq!(row.get::<f64>(2).unwrap(), 1.5);
    assert_eq!(row.get::<Option<i64>>(3).unwrap(), None);
    assert_eq!(row.get::<Option<i64>>(1).unwrap(), Some(42));
    assert_eq!(row.get::<&[u8]>(4).unwrap(), [1u8, 2]);
    assert_eq!(row.get::<Value>(0).unwrap(), Value::Text(b"foo"));
    assert_eq!(row.get::<Value>(3).unwrap(), Value::Null);

    assert!(row.get::<i64>(0).is_err());
    assert!(row.get::<&str>(3).is_err());
    assert!(row.get::<i64>(5).is_err());
    assert!(rows.next().unwrap().is_none());
}

#[test]
fn query_error() {
    let tp = TraceProcessor::from_reader(std::io::empty()).unwrap();
    assert!(tp.query("select * from does_not_exist").is_err());
}

#[test]
fn open_matches_from_reader() {
    let mmapped = TraceProcessor::open(test_trace()).unwrap();
    let count = query_int(&mmapped, "select count(*) from sched");
    assert!(count > 0);

    let read = TraceProcessor::from_reader(File::open(test_trace()).unwrap()).unwrap();
    assert_eq!(query_int(&read, "select count(*) from sched"), count);
}

#[test]
fn from_reader_with_short_reads() {
    let tp = TraceProcessor::from_reader(SlowReader(SYSTRACE.as_bytes())).unwrap();
    assert_eq!(query_int(&tp, "select count(*) from sched"), 2);
}

#[test]
fn errors() {
    assert!(TraceProcessor::open("/does/not/exist").is_err());

    struct FailingReader;
    impl Read for FailingReader {
        fn read(&mut self, _: &mut [u8]) -> std::io::Result<usize> {
            Err(std::io::Error::other("boom"))
        }
    }
    let err = TraceProcessor::from_reader(FailingReader).err().unwrap();
    assert!(err.message().contains("boom"));
}
