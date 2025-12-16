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

use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

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
                $ tools/gen_amalgamated --sdk c \
                --output contrib/rust-sdk/perfetto-sys/libperfetto_c/perfetto\n\
                \n\
                💡 Tip: invoke cargo with --no-default-features to use an external library\n",
                source_file.display()
            );
        }
        // Extra code to verify that size of `std::atomic<bool>` and `_Atomic(bool)`
        // match `bool` type. Only targets where this is the case are supported.
        let atomic_bool_check_file = out_path.join("atomic_bool_check.cc");
        fs::write(
            &atomic_bool_check_file,
            r#"
            #include <atomic>
            int check_size[sizeof(std::atomic<bool>) == sizeof(bool) ? 1 : -1];
        "#,
        )
        .unwrap();
        let mut build = cc::Build::new();
        // `PERFETTO_SYS_LIB_DEBUG=true` enables debug build of the shared library.
        let lib_debug = env::var("PERFETTO_SYS_LIB_DEBUG").ok().as_deref() == Some("true");
        if !lib_debug {
            build.define("NDEBUG", None);
        }
        if env::var("CXX").is_err() {
            if Command::new("clang++").arg("--version").output().is_ok() {
                build.compiler("clang++");
            } else {
                println!("cargo:warning=Clang not found; falling back to default compiler");
            }
        }
        build
            .cpp(true)
            .file(source_file)
            .file(atomic_bool_check_file)
            .std("c++17")
            .debug(lib_debug)
            .flag("-Wno-redundant-move")
            .flag("-Wno-unused-const-variable")
            .flag_if_supported("-Wno-pragma-system-header-outside-header")
            .flag_if_supported("-Wno-unneeded-internal-declaration")
            .compile("perfetto_c");
        println!("cargo:rerun-if-changed=libperfetto_c/perfetto_c.cc");
        println!("cargo:rerun-if-changed=libperfetto_c/perfetto_c.h");
        println!("cargo:rerun-if-env-changed=PERFETTO_SYS_LIB_DEBUG");
        println!("cargo:rerun-if-env-changed=CXX");
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
        // This ensures that bindgen generates `bool` type for `_Atomic(bool)`.
        // We include some extra code in the vendored shared library above to
        // verify that the size of `_Atomic(bool)` and `bool` match.
        .clang_arg("-DINCLUDE_PERFETTO_PUBLIC_ABI_ATOMIC_H_")
        .clang_arg("-DPERFETTO_ATOMIC(x)=x")
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
