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

//! A minimal Rust example that connects to `trace_processor_shell -D` via HTTP
//! and provides an interactive SQL prompt for querying traces.
//!
//! Usage:
//! ```bash
//! # Start trace_processor_shell with HTTP interface
//! trace_processor_shell -D trace.perfetto-trace
//!
//! # Run this example
//! cargo run --example trace_processor_shell -- --addr 127.0.0.1:9001
//! ```

use perfetto_sdk::heap_buffer::HeapBuffer;
use perfetto_sdk::pb_decoder::{PbDecoder, PbDecoderField};
use perfetto_sdk::pb_msg::{PbMsg, PbMsgWriter};
use perfetto_sdk::pb_utils::pb_parse_packed_varints;
use perfetto_sdk_protos_trace_processor::protos::trace_processor::trace_processor::{
    CellsBatchCellType, QueryArgsFieldNumber, QueryResultCellsBatchFieldNumber,
    QueryResultFieldNumber, StatusResultFieldNumber,
};
use std::error::Error;
use std::io::{self, BufRead, Write};

/// Represents a cell value in a query result
#[derive(Debug, Clone)]
enum CellValue {
    Null,
    Varint(i64),
    Float64(f64),
    String(String),
    Blob(Vec<u8>),
}

impl std::fmt::Display for CellValue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            CellValue::Null => write!(f, "NULL"),
            CellValue::Varint(v) => write!(f, "{}", v),
            CellValue::Float64(v) => write!(f, "{}", v),
            CellValue::String(s) => write!(f, "{}", s),
            CellValue::Blob(b) => write!(f, "<blob {} bytes>", b.len()),
        }
    }
}

/// Status information from the trace processor
#[derive(Debug, Default)]
struct StatusResultData {
    loaded_trace_name: String,
    human_readable_version: String,
    api_version: i32,
}

/// Parsed query result
#[derive(Debug, Default)]
struct QueryResultData {
    column_names: Vec<String>,
    rows: Vec<Vec<CellValue>>,
    error: Option<String>,
}

/// Client for communicating with trace_processor_shell HTTP interface
struct TraceProcessorClient {
    base_url: String,
    agent: ureq::Agent,
}

impl TraceProcessorClient {
    /// Create a new client connecting to the given address
    fn new(addr: &str) -> Self {
        Self {
            base_url: format!("http://{}", addr),
            agent: ureq::Agent::new(),
        }
    }

    /// Get the status of the trace processor
    fn status(&self) -> Result<StatusResultData, Box<dyn Error>> {
        let url = format!("{}/status", self.base_url);
        let response = self.agent.get(&url).call()?;

        let mut body = Vec::new();
        response.into_reader().read_to_end(&mut body)?;

        decode_status_result(&body)
    }

    /// Execute a SQL query and return the results
    fn query(&self, sql: &str) -> Result<QueryResultData, Box<dyn Error>> {
        let url = format!("{}/query", self.base_url);
        let body = encode_query_args(sql);

        let response = self
            .agent
            .post(&url)
            .set("Content-Type", "application/x-protobuf")
            .send_bytes(&body)?;

        let mut response_body = Vec::new();
        response.into_reader().read_to_end(&mut response_body)?;

        decode_query_result(&response_body)
    }
}

/// Encode a QueryArgs protobuf message
fn encode_query_args(sql: &str) -> Vec<u8> {
    let writer = PbMsgWriter::new();
    let hb = HeapBuffer::new(writer.stream_writer());
    let mut msg = PbMsg::new(&writer).unwrap();
    msg.append_cstr_field(QueryArgsFieldNumber::SqlQuery as u32, sql);
    msg.finalize();
    let size = writer.stream_writer().get_written_size();
    let mut buffer = vec![0u8; size];
    hb.copy_into(&mut buffer);
    buffer
}

/// Decode a StatusResult protobuf message
fn decode_status_result(data: &[u8]) -> Result<StatusResultData, Box<dyn Error>> {
    let mut result = StatusResultData::default();

    for item in PbDecoder::new(data) {
        let (field_id, field) = item?;
        match field_id {
            x if x == StatusResultFieldNumber::LoadedTraceName as u32 => {
                if let PbDecoderField::Delimited(bytes) = field {
                    result.loaded_trace_name = String::from_utf8_lossy(bytes).to_string();
                }
            }
            x if x == StatusResultFieldNumber::HumanReadableVersion as u32 => {
                if let PbDecoderField::Delimited(bytes) = field {
                    result.human_readable_version = String::from_utf8_lossy(bytes).to_string();
                }
            }
            x if x == StatusResultFieldNumber::ApiVersion as u32 => {
                if let PbDecoderField::Varint(v) = field {
                    result.api_version = v as i32;
                }
            }
            _ => {}
        }
    }

    Ok(result)
}

/// Parsed data from a CellsBatch message
struct CellsBatchData {
    cell_types: Vec<CellsBatchCellType>,
    varint_cells: Vec<i64>,
    float64_cells: Vec<f64>,
    string_cells: Vec<String>,
    blob_cells: Vec<Vec<u8>>,
}

/// Parse a CellsBatch and extract cell values
fn parse_cells_batch(data: &[u8]) -> CellsBatchData {
    let mut result = CellsBatchData {
        cell_types: Vec::new(),
        varint_cells: Vec::new(),
        float64_cells: Vec::new(),
        string_cells: Vec::new(),
        blob_cells: Vec::new(),
    };

    for (field_id, field) in PbDecoder::new(data).flatten() {
        match field_id {
            x if x == QueryResultCellsBatchFieldNumber::Cells as u32 => {
                if let PbDecoderField::Varint(v) = field {
                    if let Ok(ct) = CellsBatchCellType::try_from(v as u32) {
                        result.cell_types.push(ct);
                    }
                } else if let PbDecoderField::Delimited(bytes) = field {
                    // Packed repeated enum - parse raw varints
                    for v in pb_parse_packed_varints(bytes) {
                        if let Ok(ct) = CellsBatchCellType::try_from(v as u32) {
                            result.cell_types.push(ct);
                        }
                    }
                }
            }
            x if x == QueryResultCellsBatchFieldNumber::VarintCells as u32 => {
                if let PbDecoderField::Varint(v) = field {
                    result.varint_cells.push(v as i64);
                } else if let PbDecoderField::Delimited(bytes) = field {
                    // Packed repeated int64 - parse raw varints
                    for v in pb_parse_packed_varints(bytes) {
                        result.varint_cells.push(v as i64);
                    }
                }
            }
            x if x == QueryResultCellsBatchFieldNumber::Float64Cells as u32 => {
                if let PbDecoderField::Fixed64(v) = field {
                    result.float64_cells.push(f64::from_bits(v));
                } else if let PbDecoderField::Delimited(bytes) = field {
                    // Packed repeated double
                    let mut i = 0;
                    while i + 8 <= bytes.len() {
                        let bits = u64::from_le_bytes(bytes[i..i + 8].try_into().unwrap());
                        result.float64_cells.push(f64::from_bits(bits));
                        i += 8;
                    }
                }
            }
            x if x == QueryResultCellsBatchFieldNumber::BlobCells as u32 => {
                if let PbDecoderField::Delimited(bytes) = field {
                    result.blob_cells.push(bytes.to_vec());
                }
            }
            x if x == QueryResultCellsBatchFieldNumber::StringCells as u32 => {
                if let PbDecoderField::Delimited(bytes) = field {
                    // NUL-separated concatenated strings, with trailing NUL
                    // Split by NUL and skip the last empty element
                    let parts: Vec<&[u8]> = bytes.split(|&b| b == 0).collect();
                    for s in parts.iter().take(parts.len().saturating_sub(1)) {
                        result
                            .string_cells
                            .push(String::from_utf8_lossy(s).to_string());
                    }
                }
            }
            _ => {}
        }
    }

    result
}

/// Decode a QueryResult protobuf message
fn decode_query_result(data: &[u8]) -> Result<QueryResultData, Box<dyn Error>> {
    let mut result = QueryResultData::default();

    for item in PbDecoder::new(data) {
        let (field_id, field) = item?;
        match field_id {
            x if x == QueryResultFieldNumber::ColumnNames as u32 => {
                if let PbDecoderField::Delimited(bytes) = field {
                    result
                        .column_names
                        .push(String::from_utf8_lossy(bytes).to_string());
                }
            }
            x if x == QueryResultFieldNumber::Error as u32 => {
                if let PbDecoderField::Delimited(bytes) = field {
                    let error = String::from_utf8_lossy(bytes).to_string();
                    if !error.is_empty() {
                        result.error = Some(error);
                    }
                }
            }
            x if x == QueryResultFieldNumber::Batch as u32 => {
                if let PbDecoderField::Delimited(batch_data) = field {
                    let batch = parse_cells_batch(batch_data);

                    // Track current index into each cell array
                    let mut varint_idx = 0;
                    let mut float64_idx = 0;
                    let mut string_idx = 0;
                    let mut blob_idx = 0;

                    let num_cols = result.column_names.len();
                    if num_cols > 0 {
                        let mut current_row = Vec::new();
                        for cell_type in batch.cell_types {
                            let value = match cell_type {
                                CellsBatchCellType::CellNull => CellValue::Null,
                                CellsBatchCellType::CellVarint => {
                                    let v =
                                        batch.varint_cells.get(varint_idx).copied().unwrap_or(0);
                                    varint_idx += 1;
                                    CellValue::Varint(v)
                                }
                                CellsBatchCellType::CellFloat64 => {
                                    let v = batch
                                        .float64_cells
                                        .get(float64_idx)
                                        .copied()
                                        .unwrap_or(0.0);
                                    float64_idx += 1;
                                    CellValue::Float64(v)
                                }
                                CellsBatchCellType::CellString => {
                                    let v = batch
                                        .string_cells
                                        .get(string_idx)
                                        .cloned()
                                        .unwrap_or_default();
                                    string_idx += 1;
                                    CellValue::String(v)
                                }
                                CellsBatchCellType::CellBlob => {
                                    let v =
                                        batch.blob_cells.get(blob_idx).cloned().unwrap_or_default();
                                    blob_idx += 1;
                                    CellValue::Blob(v)
                                }
                                _ => CellValue::Null,
                            };
                            current_row.push(value);
                            if current_row.len() == num_cols {
                                result.rows.push(std::mem::take(&mut current_row));
                            }
                        }
                    }
                }
            }
            _ => {}
        }
    }

    Ok(result)
}

/// Print query results in a tabular format
fn print_results(result: &QueryResultData) {
    if let Some(ref error) = result.error {
        eprintln!("Error: {}", error);
        return;
    }

    if result.column_names.is_empty() {
        return;
    }

    // Calculate column widths
    let mut widths: Vec<usize> = result.column_names.iter().map(|s| s.len()).collect();
    for row in &result.rows {
        for (i, cell) in row.iter().enumerate() {
            let len = format!("{}", cell).len();
            if i < widths.len() && len > widths[i] {
                widths[i] = len;
            }
        }
    }

    // Print header
    for (i, name) in result.column_names.iter().enumerate() {
        if i > 0 {
            print!(" | ");
        }
        print!("{:width$}", name, width = widths[i]);
    }
    println!();

    // Print separator
    for (i, width) in widths.iter().enumerate() {
        if i > 0 {
            print!("-+-");
        }
        print!("{:-<width$}", "", width = width);
    }
    println!();

    // Print rows
    for row in &result.rows {
        for (i, cell) in row.iter().enumerate() {
            if i > 0 {
                print!(" | ");
            }
            let cell_str = format!("{}", cell);
            if i < widths.len() {
                print!("{:width$}", cell_str, width = widths[i]);
            } else {
                print!("{}", cell_str);
            }
        }
        println!();
    }

    println!("\n{} row(s)", result.rows.len());
}

/// Parse command line arguments
fn parse_args() -> String {
    let args: Vec<String> = std::env::args().collect();
    let mut addr = "127.0.0.1:9001".to_string();

    let mut i = 1;
    while i < args.len() {
        if args[i] == "--addr" && i + 1 < args.len() {
            addr = args[i + 1].clone();
            i += 2;
        } else {
            i += 1;
        }
    }

    addr
}

fn main() -> Result<(), Box<dyn Error>> {
    let addr = parse_args();
    let client = TraceProcessorClient::new(&addr);

    // Connect and verify
    println!("Connecting to trace_processor_shell at {}...", addr);
    match client.status() {
        Ok(status) => {
            println!("Connected to Trace Processor");
            println!("  Version: {}", status.human_readable_version);
            println!("  API Version: {}", status.api_version);
            if !status.loaded_trace_name.is_empty() {
                println!("  Loaded Trace: {}", status.loaded_trace_name);
            }
            println!();
        }
        Err(e) => {
            eprintln!(
                "Failed to connect to trace_processor_shell at {}: {}",
                addr, e
            );
            eprintln!("Make sure trace_processor_shell is running with -D flag:");
            eprintln!("  trace_processor_shell -D <trace_file>");
            return Err(e);
        }
    }

    // REPL loop
    let stdin = io::stdin();
    let mut stdout = io::stdout();

    loop {
        print!("> ");
        stdout.flush()?;

        let mut line = String::new();
        if stdin.lock().read_line(&mut line)? == 0 {
            // EOF
            println!();
            break;
        }

        let line = line.trim();

        // Skip empty lines
        if line.is_empty() {
            continue;
        }

        // Handle quit/exit commands
        if line.eq_ignore_ascii_case("quit") || line.eq_ignore_ascii_case("exit") {
            break;
        }

        // Execute query
        match client.query(line) {
            Ok(result) => print_results(&result),
            Err(e) => eprintln!("Query error: {}", e),
        }
    }

    Ok(())
}

use std::io::Read;
