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

use std::{env, fs, path::PathBuf};

const LIB_DIR_ENV: &str = "PERFETTO_TRACE_PROCESSOR_LIB_DIR";

fn main() {
    println!("cargo:rerun-if-env-changed={LIB_DIR_ENV}");
    let lib_dir = env::var_os(LIB_DIR_ENV)
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            panic!(
                "\n\nSet {LIB_DIR_ENV} to the directory containing libtrace_processor_c.\n\
             To build it from a Perfetto checkout:\n\n\
             $ tools/ninja -C out/linux_clang_release libtrace_processor_c\n\
             $ export {LIB_DIR_ENV}=$PWD/out/linux_clang_release\n"
            )
        });

    // Copy the library into OUT_DIR: cargo adds link paths inside the target
    // dir to the dynamic loader path for `cargo run` and `cargo test`.
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let file_name = match env::var("CARGO_CFG_TARGET_OS").unwrap().as_str() {
        "macos" | "ios" => "libtrace_processor_c.dylib",
        "windows" => "trace_processor_c.dll",
        _ => "libtrace_processor_c.so",
    };
    let src = lib_dir.join(file_name);
    println!("cargo:rerun-if-changed={}", src.display());
    let link_dir = if src.exists() {
        fs::copy(&src, out_dir.join(file_name)).expect("Failed to copy library");
        out_dir.clone()
    } else {
        lib_dir
    };
    println!("cargo:rustc-link-search=native={}", link_dir.display());
    println!("cargo:rustc-link-lib=dylib=trace_processor_c");

    #[cfg(feature = "bindgen")]
    {
        let header = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap())
            .join("../../../include/perfetto/trace_processor/trace_processor_c.h");
        println!("cargo:rerun-if-changed={}", header.display());
        bindgen::Builder::default()
            .header(header.display().to_string())
            .allowlist_item("(?:PerfettoTp|PERFETTO_TP_).*")
            .layout_tests(false)
            .derive_default(false)
            .generate()
            .expect("Unable to generate bindings")
            .write_to_file(out_dir.join("bindings.rs"))
            .expect("Couldn't write bindings");
    }
}
