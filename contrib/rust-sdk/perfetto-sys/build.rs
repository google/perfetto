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

use std::env;
use std::path::{Path, PathBuf};

fn main() {
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    let crate_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let include_path = env::var("PERFETTO_SYS_INCLUDE_DIR").unwrap_or_else(|_| {
        PathBuf::from(&crate_dir)
            .join("include")
            .display()
            .to_string()
    });

    // Vendored bindings can be generated using:
    if cfg!(feature = "vendored") {
        let source_file = Path::new("libperfetto_c/perfetto_c.cc");
        if !source_file.exists() {
            panic!(
                "\n\
                ❌ Missing amalgamated source file: {}.\n\n\
                To fix this, run:\n\
                \n\
                $ tools/gen_amalgamated --gn_args \"is_debug=false \
                is_clang=true use_custom_libcxx=false \
                enable_perfetto_ipc=true \
                perfetto_enable_git_rev_version_header=true \
                is_perfetto_build_generator=true \
                enable_perfetto_zlib=false\" \
                --output contrib/rust-sdk/perfetto-sys/libperfetto_c/perfetto_c \
                //src/shared_lib:libperfetto_c\n\
                \n\
                💡 Tip: invoke cargo with --no-default-features to use an external library\n",
                source_file.display()
            );
        }
        let mut build = cc::Build::new();
        // `PERFETTO_SYS_LIB_DEBUG=true` enables debug build of the shared library.
        let lib_debug = env::var("PERFETTO_SYS_LIB_DEBUG").ok().as_deref() == Some("true");
        if !lib_debug {
            build.define("NDEBUG", None);
        }
        build
            .cpp(true)
            .file(source_file)
            .std("c++17")
            .debug(lib_debug)
            .warnings(false)
            .compile("libperfetto_c");
        println!("cargo:rerun-if-changed=libperfetto_c/perfetto_c.cc");
        println!("cargo:rerun-if-changed=libperfetto_c/perfetto_c.h");
        println!("cargo:rerun-if-env-changed=PERFETTO_SYS_LIB_DEBUG");
    } else {
        let lib_path = env::var("PERFETTO_SYS_LIB_DIR")
            .expect("Set PERFETTO_SYS_LIB_DIR for non-vendored builds");
        println!("cargo:rustc-link-search=native={}", lib_path);
        println!("cargo:rustc-link-lib=dylib=perfetto_c");
        println!("cargo:rerun-if-env-changed=PERFETTO_SYS_LIB_DIR");
    }

    println!("cargo:rerun-if-env-changed=PERFETTO_SYS_INCLUDE_DIR");
    println!("cargo:rerun-if-changed=wrapper.h");

    let bindings = bindgen::Builder::default()
        .header("wrapper.h")
        .clang_arg(format!("-I{}", include_path))
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .allowlist_type("(?:Perfetto|perfetto).*")
        .allowlist_function("(?:Perfetto|perfetto).*")
        .allowlist_var("(?:PERFETTO|perfetto)_.*")
        .layout_tests(false)
        .derive_default(false)
        .derive_eq(false)
        .blocklist_type("max_align_t")
        .generate()
        .expect("Unable to generate bindings");

    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
