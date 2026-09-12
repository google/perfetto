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

//! Loads a trace and prints the result of a query as tab-separated values.
//!
//! Usage: cargo run --example query -- <trace> <sql>

use perfetto_trace_processor::{TraceProcessor, Value};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    let [_, trace, sql] = args.as_slice() else {
        return Err("usage: query <trace> <sql>".into());
    };

    let tp = TraceProcessor::open(trace)?;

    let mut rows = tp.query(sql)?;
    println!("{}", rows.column_names().join("\t"));
    while let Some(row) = rows.next()? {
        let mut cells = Vec::new();
        for i in 0..row.column_count() {
            cells.push(match row.get(i)? {
                Value::Null => "NULL".to_string(),
                Value::Int(v) => v.to_string(),
                Value::Double(v) => v.to_string(),
                Value::Text(v) => String::from_utf8_lossy(v).into_owned(),
                Value::Bytes(v) => format!("<{} bytes>", v.len()),
            });
        }
        println!("{}", cells.join("\t"));
    }
    Ok(())
}
