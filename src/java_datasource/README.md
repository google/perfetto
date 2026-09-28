# Perfetto Java Custom Data Source SDK

The Perfetto Java Custom Data Source SDK (`dev.perfetto.sdk`) provides a high-performance,
zero-heap-allocation tracing API for Android applications and libraries (such as AndroidX
and UI hierarchy inspection).

Proto encoding is performed entirely in Java using `ProtoWriter`, writing directly to a
pre-allocated thread-local byte buffer using redundant varint length fields (similar to C++
protozero). A single JNI call emits the encoded packet into Perfetto's shared memory buffer
across all active tracing sessions.

## Packaging the AAR

The official Android Archive (AAR) packaging script builds the native JNI libraries for all
standard Android ABIs (`arm64-v8a`, `armeabi-v7a`, `x86_64`, and `x86`) using GN and Ninja,
compiles the Java sources against `android.jar` and Dalvik optimization stubs, strips the
native libraries with `llvm-strip`, and packages an AAR along with consumer Proguard rules
and a Maven POM.

### Building the AAR

To build the default AAR (`out/perfetto-datasource.aar`):

```bash
tools/build_java_sdk_aar
```

To build and publish directly to a local Maven repository directory:

```bash
tools/build_java_sdk_aar --repo-dir /path/to/local-maven-repo
```

To build only specific ABIs (e.g. for faster development or emulator testing):

```bash
tools/build_java_sdk_aar --abis x86_64,arm64-v8a
```

### Options

* `--abis`: Comma-separated list of ABIs to include (default: `arm64-v8a,armeabi-v7a,x86_64,x86`).
* `--aar-out`: Path to write the `.aar` archive (default: `out/perfetto-datasource.aar`).
* `--repo-dir`: Directory of the local Maven repository to publish to.
* `--version`: Maven version string (default: derived from `CHANGELOG`).
* `--min-sdk`: Minimum Android SDK version declared in the manifest (default: `21`).
* `--no-strip`: Skip stripping debug symbols from the packaged `.so` libraries.

## Using the AAR in Gradle

### Option A: Local Maven Repository

Add the local repository to your `settings.gradle.kts` or `build.gradle.kts`:

```kotlin
repositories {
    maven {
        url = uri("/path/to/local-maven-repo")
    }
}

dependencies {
    implementation("dev.perfetto:perfetto-datasource:58.3.0")
}
```

### Option B: Flat Directory

```kotlin
repositories {
    flatDir {
        dirs("/path/to/aar-directory")
    }
}

dependencies {
    implementation(name = "perfetto-datasource", ext = "aar")
}
```

## API Usage

1. Initialize and register the data source once at startup:

```java
public class MyDataSource extends PerfettoDataSource {
    public static final MyDataSource INSTANCE = new MyDataSource();

    static {
        // Loads libperfetto_datasource_jni.so and initializes producer
        PerfettoDataSource.initialize(PerfettoDataSource.BACKEND_SYSTEM);
        INSTANCE.register("com.example.mydatasource");
    }
}
```

2. Trace from hot paths with zero allocations:

```java
TraceContext ctx = MyDataSource.INSTANCE.trace();
if (ctx != null) {
    ctx.newPacket()
        .writeVarInt(1 /* timestamp */, timestamp)
        .beginNested(2 /* payload */)
            .writeString(1 /* name */, name)
        .endNested()
        .commit();
}
```
