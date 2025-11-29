#!/bin/bash

SRC_FOLDER="/usr/local/google/home/ktimofeev/Work/perfetto-standalone"
DST_FOLDER="/usr/local/google/home/ktimofeev/android-google-internal/main/external/perfetto"

# List of files to copy (relative paths)
FILES=(
"perfetto.rc"
"protos/perfetto/config/perfetto_config.proto"
"protos/perfetto/config/trace_config.proto"
"protos/perfetto/trace/perfetto_trace.proto"
"python/perfetto/protos/perfetto/trace/perfetto_trace_pb2.py"
"python/perfetto/protos/perfetto/trace/perfetto_trace_pb2.pyi"
"src/perfetto_cmd/perfetto_cmd.cc"
"src/perfetto_cmd/perfetto_cmd.h"
"src/perfetto_cmd/perfetto_cmd_android.cc"
"test/cts/reporter/Android.bp"
"test/cts/reporter/AndroidTest.xml"
"test/cts/reporter/ReporterRebootHostTest.xml"
"test/cts/reporter/reporter_test_cts.cc"
"test/cts/reporter/src/android/perfetto/cts/test/PerfettoReporterRebootTest.java"
)

echo "Copying files from $SRC_FOLDER to $DST_FOLDER"
echo "----------------------------------------"

# Copy each file, preserving directory structure
for file in "${FILES[@]}"; do
    src_path="$SRC_FOLDER/$file"
    dst_path="$DST_FOLDER/$file"
    
    # Check if source file exists
    if [ ! -f "$src_path" ]; then
        echo "⚠️  Warning: Source file not found: $src_path"
        continue
    fi
    
    # Create destination directory if it doesn't exist
    dst_dir=$(dirname "$dst_path")
    mkdir -p "$dst_dir"
    
    # Check if destination file exists and compare with source
    if [ -f "$dst_path" ]; then
        # Compare files; cmp returns 0 if files are identical
        if cmp -s "$src_path" "$dst_path"; then
            echo "≡ Skipped (unchanged): $file"
            continue
        fi
    fi
    
    # Copy the file (either destination doesn't exist or files differ)
    cp "$src_path" "$dst_path"
    
    if [ $? -eq 0 ]; then
        echo "✓ Copied: $file"
    else
        echo "✗ Failed to copy: $file"
    fi
done

echo "----------------------------------------"
echo "Copy complete!"