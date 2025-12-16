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

use perfetto_sdk::{producer::*, track_event::TrackEvent, track_event_categories};
use perfetto_sdk_derive::tracefn;
use std::error::Error;

track_event_categories! {
    pub mod example_te_ns {
        ( "cat1", "Test category 1", [ "tag1" ] ),
        ( "cat2", "Test category 2", [ "tag2", "tag3" ] ),
    }
}

use example_te_ns as perfetto_te_ns;

#[tracefn("cat1", prefix = "parse")]
fn example_function(int_arg: i32, string_arg: String) {
    assert_eq!(int_arg, string_arg.parse::<i32>().unwrap());
    std::thread::sleep(std::time::Duration::from_secs(1));
}

#[derive(Debug)]
struct ExampleData {
    field_int32: Option<i32>,
    field_string: Option<String>,
}

#[tracefn("cat2", flush = true)]
fn another_example_function(struct_arg: &ExampleData) {
    assert_eq!(
        struct_arg.field_int32,
        struct_arg
            .field_string
            .clone()
            .map(|v| v.parse::<i32>().unwrap())
    );
    std::thread::sleep(std::time::Duration::from_secs(1));
}

fn main() -> Result<(), Box<dyn Error>> {
    let producer_args = ProducerInitArgsBuilder::new().backends(Backends::SYSTEM);
    Producer::init(producer_args.build());
    TrackEvent::init();
    perfetto_te_ns::register()?;
    let mut counter: i32 = 1;
    loop {
        example_function(counter, counter.to_string());
        another_example_function(&ExampleData {
            field_int32: Some(counter),
            field_string: Some(counter.to_string()),
        });
        std::thread::sleep(std::time::Duration::from_secs(1));
        counter += 1;
    }
}
