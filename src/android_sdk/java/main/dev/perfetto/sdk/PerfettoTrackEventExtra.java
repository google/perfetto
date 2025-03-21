/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package dev.perfetto.sdk;

import dalvik.annotation.optimization.CriticalNative;
import dalvik.annotation.optimization.FastNative;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Supplier;

/**
 * Holds extras to be passed to Perfetto track events in {@link PerfettoTrace}.
 *
 * @hide
 */
public final class PerfettoTrackEventExtra {
    private static final boolean DEBUG = false;
    private static final int DEFAULT_EXTRA_CACHE_SIZE = 5;
    private static final Builder NO_OP_BUILDER = new Builder(/* extra= */ null, /* isCategoryEnabled= */ false);
    private static final ThreadLocal<PerfettoTrackEventExtra> sTrackEventExtra =
            new ThreadLocal<PerfettoTrackEventExtra>() {
                @Override
                protected PerfettoTrackEventExtra initialValue() {
                    return new PerfettoTrackEventExtra();
                }
            };
    private static final AtomicLong sNamedTrackId = new AtomicLong();
    private static final Supplier<Flow> sFlowSupplier = Flow::new;
    private static final Supplier<Builder> sBuilderSupplier = Builder::new;
    private static final Supplier<FieldInt64> sFieldInt64Supplier = FieldInt64::new;
    private static final Supplier<FieldDouble> sFieldDoubleSupplier = FieldDouble::new;
    private static final Supplier<FieldString> sFieldStringSupplier = FieldString::new;
    private static final Supplier<FieldNested> sFieldNestedSupplier = FieldNested::new;

    private final List<PerfettoPointer> mPendingPointers = new ArrayList<>();
    private CounterInt64 mCounterInt64;
    private CounterDouble mCounterDouble;
    private Proto mProto;
    private Flow mFlow;
    private Flow mTerminatingFlow;

    static class NativeAllocationRegistry {
        public static NativeAllocationRegistry createMalloced(
                ClassLoader classLoader, long freeFunction) {
            // do nothing
            return new NativeAllocationRegistry();
        }
        public void registerNativeAllocation(Object obj, long ptr) {
            // do nothing
        }
    }

    /**
     * Represents a native pointer to a Perfetto C SDK struct. E.g. PerfettoTeHlExtra.
     */
    public interface PerfettoPointer {
        /**
         * Returns the perfetto struct native pointer.
         */
        long getPtr();
    }

    /**
     * Container for {@link Field} instances.
     */
    public interface FieldContainer {
        /**
         * Add {@link Field} to the container.
         */
        void addField(PerfettoPointer field);
    }

    /**
     * RingBuffer implemented on top of a SparseArray.
     *
     * Bounds a SparseArray with a FIFO algorithm.
     */
    private static final class RingBuffer<T> {
        private final int mCapacity;
        private final int[] mKeyArray;
        private final T[] mValueArray;
        private int mWriteEnd = 0;

        RingBuffer(int capacity) {
            mCapacity = capacity;
            mKeyArray = new int[capacity];
            mValueArray = (T[]) new Object[capacity];
        }

        public void put(int key, T value) {
            mKeyArray[mWriteEnd] = key;
            mValueArray[mWriteEnd] = value;
            mWriteEnd = (mWriteEnd + 1) % mCapacity;
        }

        public T get(int key) {
            for (int i = 0; i < mCapacity; i++) {
                if (mKeyArray[i] == key) {
                    return mValueArray[i];
                }
            }
            return null;
        }
    }

    private static final class Pool<T> {
        private final int mCapacity;
        private final T[] mValueArray;
        private int mIdx = 0;

        Pool(int capacity) {
            mCapacity = capacity;
            mValueArray = (T[]) new Object[capacity];
        }

        public void reset() {
            mIdx = 0;
        }

        public T get(Supplier<T> supplier) {
            if (mIdx >= mCapacity) {
                return supplier.get();
            }
            if (mValueArray[mIdx] == null) {
                mValueArray[mIdx] = supplier.get();
            }
            return mValueArray[mIdx++];
        }
    }

    /**
     * Builder for Perfetto track event extras.
     */
    public static final class Builder {
        // For performance reasons, we hold a reference to mExtra as a holder for
        // perfetto pointers being added. This way, we avoid an additional list to hold
        // the pointers in Java and we can pass them down directly to native code.
        private final PerfettoTrackEventExtra mExtra;

        private int mTraceType;
        private PerfettoTrace.Category mCategory;
        private String mEventName;
        private boolean mIsBuilt;

        private Builder mParent;
        private FieldContainer mCurrentContainer;

        private final boolean mIsCategoryEnabled;
        private final CounterInt64 mCounterInt64;
        private final CounterDouble mCounterDouble;
        private final Proto mProto;
        private final Flow mFlow;
        private final Flow mTerminatingFlow;

        private final RingBuffer<NamedTrack> mNamedTrackCache;
        private final RingBuffer<CounterTrack> mCounterTrackCache;
        private final RingBuffer<ArgInt64> mArgInt64Cache;
        private final RingBuffer<ArgBool> mArgBoolCache;
        private final RingBuffer<ArgDouble> mArgDoubleCache;
        private final RingBuffer<ArgString> mArgStringCache;

        private final Pool<FieldInt64> mFieldInt64Cache;
        private final Pool<FieldDouble> mFieldDoubleCache;
        private final Pool<FieldString> mFieldStringCache;
        private final Pool<FieldNested> mFieldNestedCache;
        private final Pool<Builder> mBuilderCache;

        private Builder() {
            this(sTrackEventExtra.get(), true);
        }

        public Builder(PerfettoTrackEventExtra extra, boolean isCategoryEnabled) {
            mIsCategoryEnabled = isCategoryEnabled;
            mExtra = extra;
            mNamedTrackCache = mExtra == null ? null : mExtra.mNamedTrackCache;
            mCounterTrackCache = mExtra == null ? null : mExtra.mCounterTrackCache;
            mArgInt64Cache = mExtra == null ? null : mExtra.mArgInt64Cache;
            mArgDoubleCache = mExtra == null ? null : mExtra.mArgDoubleCache;
            mArgBoolCache = mExtra == null ? null : mExtra.mArgBoolCache;
            mArgStringCache = mExtra == null ? null : mExtra.mArgStringCache;
            mFieldInt64Cache = mExtra == null ? null : mExtra.mFieldInt64Cache;
            mFieldDoubleCache = mExtra == null ? null : mExtra.mFieldDoubleCache;
            mFieldStringCache = mExtra == null ? null : mExtra.mFieldStringCache;
            mFieldNestedCache = mExtra == null ? null : mExtra.mFieldNestedCache;
            mBuilderCache = mExtra == null ? null : mExtra.mBuilderCache;

            mCounterInt64 = mExtra == null ? null : mExtra.getCounterInt64();
            mCounterDouble = mExtra == null ? null : mExtra.getCounterDouble();
            mProto = mExtra == null ? null : mExtra.getProto();
            mFlow = mExtra == null ? null : mExtra.getFlow();
            mTerminatingFlow = mExtra == null ? null : mExtra.getTerminatingFlow();
        }

        /**
         * Emits the track event.
         */
        public void emit() {
            if (!mIsCategoryEnabled) {
                return;
            }
            if (DEBUG) {
                checkParent();
            }

            mIsBuilt = true;
            native_emit(mTraceType, mCategory.getPtr(), mEventName, mExtra.getPtr());
        }

        /**
         * Initialize the builder for a new trace event.
         */
        public Builder init(int traceType, PerfettoTrace.Category category) {
            if (!mIsCategoryEnabled) {
                return this;
            }

            mTraceType = traceType;
            mCategory = category;
            mEventName = "";
            mFieldInt64Cache.reset();
            mFieldDoubleCache.reset();
            mFieldStringCache.reset();
            mFieldNestedCache.reset();
            mBuilderCache.reset();

            mExtra.reset();
            // Reset after on init in case the thread created builders without calling emit
            return initInternal(this, null);
        }

        /**
         * Sets the event name for the track event.
         */
        public Builder setEventName(String eventName) {
            mEventName = eventName;
            return this;
        }

        /**
         * Adds a debug arg with key {@code name} and value {@code val}.
         */
        public Builder addArg(String name, long val) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkParent();
            }
            ArgInt64 arg = mArgInt64Cache.get(name.hashCode());
            if (arg == null || !arg.getName().equals(name)) {
                arg = new ArgInt64(name);
                mArgInt64Cache.put(name.hashCode(), arg);
            }
            arg.setValue(val);
            mExtra.addPerfettoPointer(arg);
            return this;
        }

        /**
         * Adds a debug arg with key {@code name} and value {@code val}.
         */
        public Builder addArg(String name, boolean val) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkParent();
            }
            ArgBool arg = mArgBoolCache.get(name.hashCode());
            if (arg == null || !arg.getName().equals(name)) {
                arg = new ArgBool(name);
                mArgBoolCache.put(name.hashCode(), arg);
            }
            arg.setValue(val);
            mExtra.addPerfettoPointer(arg);
            return this;
        }

        /**
         * Adds a debug arg with key {@code name} and value {@code val}.
         */
        public Builder addArg(String name, double val) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkParent();
            }
            ArgDouble arg = mArgDoubleCache.get(name.hashCode());
            if (arg == null || !arg.getName().equals(name)) {
                arg = new ArgDouble(name);
                mArgDoubleCache.put(name.hashCode(), arg);
            }
            arg.setValue(val);
            mExtra.addPerfettoPointer(arg);
            return this;
        }

        /**
         * Adds a debug arg with key {@code name} and value {@code val}.
         */
        public Builder addArg(String name, String val) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkParent();
            }
            ArgString arg = mArgStringCache.get(name.hashCode());
            if (arg == null || !arg.getName().equals(name)) {
                arg = new ArgString(name);
                mArgStringCache.put(name.hashCode(), arg);
            }
            arg.setValue(val);
            mExtra.addPerfettoPointer(arg);
            return this;
        }

        /**
         * Adds a flow with {@code id}.
         */
        public Builder setFlow(long id) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkParent();
            }
            mFlow.setProcessFlow(id);
            mExtra.addPerfettoPointer(mFlow);
            return this;
        }

        /**
         * Adds a terminating flow with {@code id}.
         */
        public Builder setTerminatingFlow(long id) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkParent();
            }
            mTerminatingFlow.setProcessTerminatingFlow(id);
            mExtra.addPerfettoPointer(mTerminatingFlow);
            return this;
        }

        /**
         * Adds the events to a named track instead of the thread track where the
         * event occurred.
         */
        public Builder usingNamedTrack(long parentUuid, String name) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkParent();
            }

            NamedTrack track = mNamedTrackCache.get(name.hashCode());
            if (track == null || !track.getName().equals(name)) {
                track = new NamedTrack(name, parentUuid);
                mNamedTrackCache.put(name.hashCode(), track);
            }
            mExtra.addPerfettoPointer(track);
            return this;
        }

        /**
         * Adds the events to a process scoped named track instead of the thread track where the
         * event occurred.
         */
        public Builder usingProcessNamedTrack(String name) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            return usingNamedTrack(PerfettoTrace.getProcessTrackUuid(), name);
        }

        /**
         * Adds the events to a thread scoped named track instead of the thread track where the
         * event occurred.
         */
        public Builder usingThreadNamedTrack(long tid, String name) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            return usingNamedTrack(PerfettoTrace.getThreadTrackUuid(tid), name);
        }

        /**
         * Adds the events to a counter track instead. This is required for
         * setting counter values.
         */
        public Builder usingCounterTrack(long parentUuid, String name) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkParent();
            }

            CounterTrack track = mCounterTrackCache.get(name.hashCode());
            if (track == null || !track.getName().equals(name)) {
                track = new CounterTrack(name, parentUuid);
                mCounterTrackCache.put(name.hashCode(), track);
            }
            mExtra.addPerfettoPointer(track);
            return this;
        }

        /**
         * Adds the events to a process scoped counter track instead. This is required for
         * setting counter values.
         */
        public Builder usingProcessCounterTrack(String name) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            return usingCounterTrack(PerfettoTrace.getProcessTrackUuid(), name);
        }

        /**
         * Adds the events to a thread scoped counter track instead. This is required for
         * setting counter values.
         */
        public Builder usingThreadCounterTrack(long tid, String name) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            return usingCounterTrack(PerfettoTrace.getThreadTrackUuid(tid), name);
        }

        /**
         * Sets a long counter value on the event.
         *
         */
        public Builder setCounter(long val) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkParent();
            }
            mCounterInt64.setValue(val);
            mExtra.addPerfettoPointer(mCounterInt64);
            return this;
        }

        /**
         * Sets a double counter value on the event.
         *
         */
        public Builder setCounter(double val) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkParent();
            }
            mCounterDouble.setValue(val);
            mExtra.addPerfettoPointer(mCounterDouble);
            return this;
        }

        /**
         * Adds a proto field with field id {@code id} and value {@code val}.
         */
        public Builder addField(long id, long val) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkContainer();
            }
            FieldInt64 field = mFieldInt64Cache.get(sFieldInt64Supplier);
            field.setValue(id, val);
            mExtra.addPerfettoPointer(mCurrentContainer, field);
            return this;
        }

        /**
         * Adds a proto field with field id {@code id} and value {@code val}.
         */
        public Builder addField(long id, double val) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkContainer();
            }
            FieldDouble field = mFieldDoubleCache.get(sFieldDoubleSupplier);
            field.setValue(id, val);
            mExtra.addPerfettoPointer(mCurrentContainer, field);
            return this;
        }

        /**
         * Adds a proto field with field id {@code id} and value {@code val}.
         */
        public Builder addField(long id, String val) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkContainer();
            }
            FieldString field = mFieldStringCache.get(sFieldStringSupplier);
            field.setValue(id, val);
            mExtra.addPerfettoPointer(mCurrentContainer, field);
            return this;
        }

        /**
         * Begins a proto field.
         * Fields can be added from this point and there must be a corresponding
         * {@link endProto}.
         *
         * The proto field is a singleton and all proto fields get added inside the
         * one {@link beginProto} and {@link endProto} within the {@link Builder}.
         */
        public Builder beginProto() {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkParent();
            }
            mProto.clearFields();
            mExtra.addPerfettoPointer(mProto);
            return mBuilderCache.get(sBuilderSupplier).initInternal(this, mProto);
        }

        /**
         * Ends a proto field.
         */
        public Builder endProto() {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (mParent == null || mCurrentContainer == null) {
                throw new IllegalStateException("No proto to end");
            }
            return mParent;
        }

        /**
         * Begins a nested proto field with field id {@code id}.
         * Fields can be added from this point and there must be a corresponding
         * {@link endNested}.
         */
        public Builder beginNested(long id) {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (DEBUG) {
                checkContainer();
            }
            FieldNested field = mFieldNestedCache.get(sFieldNestedSupplier);
            field.setId(id);
            mExtra.addPerfettoPointer(mCurrentContainer, field);
            return mBuilderCache.get(sBuilderSupplier).initInternal(this, field);
        }

        /**
         * Ends a nested proto field.
         */
        public Builder endNested() {
            if (!mIsCategoryEnabled) {
                return this;
            }
            if (mParent == null || mCurrentContainer == null) {
                throw new IllegalStateException("No nested field to end");
            }
            return mParent;
        }


        private Builder initInternal(Builder parent, FieldContainer field) {
            mParent = parent;
            mCurrentContainer = field;
            mIsBuilt = false;

            return this;
        }

        private void checkState() {
            if (mIsBuilt) {
                throw new IllegalStateException(
                    "This builder has already been used. Create a new builder for another event.");
            }
        }

        private void checkParent() {
            checkState();
            if (!this.equals(mParent)) {
                throw new IllegalStateException("Operation not supported for proto");
            }
        }

        private void checkContainer() {
            checkState();
            if (mCurrentContainer == null) {
                throw new IllegalStateException(
                    "Field operations must be within beginProto/endProto block");
            }
        }
    }

    /**
     * Start a {@link Builder} to build a {@link PerfettoTrackEventExtra}.
     */
    public static Builder builder(boolean isCategoryEnabled) {
        if (isCategoryEnabled) {
            return sTrackEventExtra.get().mBuilderCache.get(sBuilderSupplier)
                .initInternal(null, null);
        }
        return NO_OP_BUILDER;
    }

    private final RingBuffer<NamedTrack> mNamedTrackCache =
            new RingBuffer(DEFAULT_EXTRA_CACHE_SIZE);
    private final RingBuffer<CounterTrack> mCounterTrackCache =
            new RingBuffer(DEFAULT_EXTRA_CACHE_SIZE);

    private final RingBuffer<ArgInt64> mArgInt64Cache = new RingBuffer(DEFAULT_EXTRA_CACHE_SIZE);
    private final RingBuffer<ArgBool> mArgBoolCache = new RingBuffer(DEFAULT_EXTRA_CACHE_SIZE);
    private final RingBuffer<ArgDouble> mArgDoubleCache = new RingBuffer(DEFAULT_EXTRA_CACHE_SIZE);
    private final RingBuffer<ArgString> mArgStringCache = new RingBuffer(DEFAULT_EXTRA_CACHE_SIZE);

    private final Pool<FieldInt64> mFieldInt64Cache = new Pool(DEFAULT_EXTRA_CACHE_SIZE);
    private final Pool<FieldDouble> mFieldDoubleCache = new Pool(DEFAULT_EXTRA_CACHE_SIZE);
    private final Pool<FieldString> mFieldStringCache = new Pool(DEFAULT_EXTRA_CACHE_SIZE);
    private final Pool<FieldNested> mFieldNestedCache = new Pool(DEFAULT_EXTRA_CACHE_SIZE);
    private final Pool<Builder> mBuilderCache = new Pool(DEFAULT_EXTRA_CACHE_SIZE);

    private static final NativeAllocationRegistry sRegistry =
            NativeAllocationRegistry.createMalloced(
                    PerfettoTrackEventExtra.class.getClassLoader(), native_delete());

    private final long mPtr;
    private static final String TAG = "PerfettoTrackEventExtra";

    private PerfettoTrackEventExtra() {
        mPtr = native_init();
    }

    /**
     * Returns the native pointer.
     */
    public long getPtr() {
        return mPtr;
    }

    /**
     * Adds a pointer representing a track event parameter.
     */
    public void addPerfettoPointer(PerfettoPointer extra) {
        native_add_arg(mPtr, extra.getPtr());
        mPendingPointers.add(extra);
    }

    /**
     * Adds a pointer representing a track event parameter to the {@code container}.
     */
    public void addPerfettoPointer(FieldContainer container, PerfettoPointer extra) {
        container.addField(extra);
        mPendingPointers.add(extra);
    }

    /**
     * Resets the track event extra.
     */
    public void reset() {
        native_clear_args(mPtr);
        mPendingPointers.clear();
    }

    private CounterInt64 getCounterInt64() {
        if (mCounterInt64 == null) {
            mCounterInt64 = new CounterInt64();
        }
        return mCounterInt64;
    }

    private CounterDouble getCounterDouble() {
        if (mCounterDouble == null) {
            mCounterDouble = new CounterDouble();
        }
        return mCounterDouble;
    }

    private Proto getProto() {
        if (mProto == null) {
            mProto = new Proto();
        }
        return mProto;
    }

    private Flow getFlow() {
        if (mFlow == null) {
            mFlow = new Flow();
        }
        return mFlow;
    }

    private Flow getTerminatingFlow() {
        if (mTerminatingFlow == null) {
            mTerminatingFlow = new Flow();
        }
        return mTerminatingFlow;
    }

    private static final class Flow implements PerfettoPointer {
        private final long mPtr;
        private final long mExtraPtr;

        Flow() {
            mPtr = native_init();
            mExtraPtr = native_get_extra_ptr(mPtr);
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        public void setProcessFlow(long type) {
            native_set_process_flow(mPtr, type);
        }

        public void setProcessTerminatingFlow(long id) {
            native_set_process_terminating_flow(mPtr, id);
        }

        @Override
        public long getPtr() {
            return mExtraPtr;
        }

        @CriticalNative
        private static native long native_init();
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native void native_set_process_flow(long ptr, long type);
        @CriticalNative
        private static native void native_set_process_terminating_flow(long ptr, long id);
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
    }

    private static class NamedTrack implements PerfettoPointer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        NamedTrack.class.getClassLoader(), native_delete());

        private final long mPtr;
        private final long mExtraPtr;
        private final String mName;

        NamedTrack(String name, long parentUuid) {
            mPtr = native_init(sNamedTrackId.incrementAndGet(), name, parentUuid);
            mExtraPtr = native_get_extra_ptr(mPtr);
            mName = name;
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mExtraPtr;
        }

        public String getName() {
            return mName;
        }

        @FastNative
        private static native long native_init(long id, String name, long parentUuid);
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
    }

    private static final class CounterTrack implements PerfettoPointer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        CounterTrack.class.getClassLoader(), native_delete());

        private final long mPtr;
        private final long mExtraPtr;
        private final String mName;

        CounterTrack(String name, long parentUuid) {
            mPtr = native_init(name, parentUuid);
            mExtraPtr = native_get_extra_ptr(mPtr);
            mName = name;
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mExtraPtr;
        }

        public String getName() {
            return mName;
        }

        @FastNative
        private static native long native_init(String name, long parentUuid);
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
    }

    private static final class CounterInt64 implements PerfettoPointer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        CounterInt64.class.getClassLoader(), native_delete());

        private final long mPtr;
        private final long mExtraPtr;

        CounterInt64() {
            mPtr = native_init();
            mExtraPtr = native_get_extra_ptr(mPtr);
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mExtraPtr;
        }

        public void setValue(long value) {
            native_set_value(mPtr, value);
        }

        @CriticalNative
        private static native long native_init();
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native void native_set_value(long ptr, long value);
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
    }

    private static final class CounterDouble implements PerfettoPointer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        CounterDouble.class.getClassLoader(), native_delete());

        private final long mPtr;
        private final long mExtraPtr;

        CounterDouble() {
            mPtr = native_init();
            mExtraPtr = native_get_extra_ptr(mPtr);
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mExtraPtr;
        }

        public void setValue(double value) {
            native_set_value(mPtr, value);
        }

        @CriticalNative
        private static native long native_init();
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native void native_set_value(long ptr, double value);
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
    }

    private static final class ArgInt64 implements PerfettoPointer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        ArgInt64.class.getClassLoader(), native_delete());

        // Private pointer holding Perfetto object with metadata
        private final long mPtr;

        // Public pointer to Perfetto object itself
        private final long mExtraPtr;

        private final String mName;

        ArgInt64(String name) {
            mPtr = native_init(name);
            mExtraPtr = native_get_extra_ptr(mPtr);
            mName = name;
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mExtraPtr;
        }

        public String getName() {
            return mName;
        }

        public void setValue(long val) {
            native_set_value(mPtr, val);
        }

        @FastNative
        private static native long native_init(String name);
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
        @CriticalNative
        private static native void native_set_value(long ptr, long val);
    }

    private static final class ArgBool implements PerfettoPointer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        ArgBool.class.getClassLoader(), native_delete());

        // Private pointer holding Perfetto object with metadata
        private final long mPtr;

        // Public pointer to Perfetto object itself
        private final long mExtraPtr;

        private final String mName;

        ArgBool(String name) {
            mPtr = native_init(name);
            mExtraPtr = native_get_extra_ptr(mPtr);
            mName = name;
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mExtraPtr;
        }

        public String getName() {
            return mName;
        }

        public void setValue(boolean val) {
            native_set_value(mPtr, val);
        }

        @FastNative
        private static native long native_init(String name);
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
        @CriticalNative
        private static native void native_set_value(long ptr, boolean val);
    }

    private static final class ArgDouble implements PerfettoPointer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        ArgDouble.class.getClassLoader(), native_delete());

        // Private pointer holding Perfetto object with metadata
        private final long mPtr;

        // Public pointer to Perfetto object itself
        private final long mExtraPtr;

        private final String mName;

        ArgDouble(String name) {
            mPtr = native_init(name);
            mExtraPtr = native_get_extra_ptr(mPtr);
            mName = name;
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mExtraPtr;
        }

        public String getName() {
            return mName;
        }

        public void setValue(double val) {
            native_set_value(mPtr, val);
        }

        @FastNative
        private static native long native_init(String name);
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
        @CriticalNative
        private static native void native_set_value(long ptr, double val);
    }

    private static final class ArgString implements PerfettoPointer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        ArgString.class.getClassLoader(), native_delete());

        // Private pointer holding Perfetto object with metadata
        private final long mPtr;

        // Public pointer to Perfetto object itself
        private final long mExtraPtr;

        private final String mName;

        ArgString(String name) {
            mPtr = native_init(name);
            mExtraPtr = native_get_extra_ptr(mPtr);
            mName = name;
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mExtraPtr;
        }

        public String getName() {
            return mName;
        }

        public void setValue(String val) {
            native_set_value(mPtr, val);
        }

        @FastNative
        private static native long native_init(String name);
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
        @FastNative
        private static native void native_set_value(long ptr, String val);
    }

    private static final class Proto implements PerfettoPointer, FieldContainer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        Proto.class.getClassLoader(), native_delete());

        // Private pointer holding Perfetto object with metadata
        private final long mPtr;

        // Public pointer to Perfetto object itself
        private final long mExtraPtr;

        Proto() {
            mPtr = native_init();
            mExtraPtr = native_get_extra_ptr(mPtr);
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mExtraPtr;
        }

        @Override
        public void addField(PerfettoPointer field) {
            native_add_field(mPtr, field.getPtr());
        }

        public void clearFields() {
            native_clear_fields(mPtr);
        }

        @CriticalNative
        private static native long native_init();
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
        @CriticalNative
        private static native void native_add_field(long ptr, long extraPtr);
        @CriticalNative
        private static native void native_clear_fields(long ptr);
    }

    private static final class FieldInt64 implements PerfettoPointer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        FieldInt64.class.getClassLoader(), native_delete());

        // Private pointer holding Perfetto object with metadata
        private final long mPtr;

        // Public pointer to Perfetto object itself
        private final long mFieldPtr;

        FieldInt64() {
            mPtr = native_init();
            mFieldPtr = native_get_extra_ptr(mPtr);
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mFieldPtr;
        }

        public void setValue(long id, long val) {
            native_set_value(mPtr, id, val);
        }

        @CriticalNative
        private static native long native_init();
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
        @CriticalNative
        private static native void native_set_value(long ptr, long id, long val);
    }

    private static final class FieldDouble implements PerfettoPointer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        FieldDouble.class.getClassLoader(), native_delete());

        // Private pointer holding Perfetto object with metadata
        private final long mPtr;

        // Public pointer to Perfetto object itself
        private final long mFieldPtr;

        FieldDouble() {
            mPtr = native_init();
            mFieldPtr = native_get_extra_ptr(mPtr);
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mFieldPtr;
        }

        public void setValue(long id, double val) {
            native_set_value(mPtr, id, val);
        }

        @CriticalNative
        private static native long native_init();
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
        @CriticalNative
        private static native void native_set_value(long ptr, long id, double val);
    }

    private static final class FieldString implements PerfettoPointer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        FieldString.class.getClassLoader(), native_delete());

        // Private pointer holding Perfetto object with metadata
        private final long mPtr;

        // Public pointer to Perfetto object itself
        private final long mFieldPtr;

        FieldString() {
            mPtr = native_init();
            mFieldPtr = native_get_extra_ptr(mPtr);
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mFieldPtr;
        }

        public void setValue(long id, String val) {
            native_set_value(mPtr, id, val);
        }

        @CriticalNative
        private static native long native_init();
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
        @FastNative
        private static native void native_set_value(long ptr, long id, String val);
    }

    private static final class FieldNested implements PerfettoPointer, FieldContainer {
        private static final NativeAllocationRegistry sRegistry =
                NativeAllocationRegistry.createMalloced(
                        FieldNested.class.getClassLoader(), native_delete());

        // Private pointer holding Perfetto object with metadata
        private final long mPtr;

        // Public pointer to Perfetto object itself
        private final long mFieldPtr;

        FieldNested() {
            mPtr = native_init();
            mFieldPtr = native_get_extra_ptr(mPtr);
            sRegistry.registerNativeAllocation(this, mPtr);
        }

        @Override
        public long getPtr() {
            return mFieldPtr;
        }

        @Override
        public void addField(PerfettoPointer field) {
            native_add_field(mPtr, field.getPtr());
        }

        public void setId(long id) {
            native_set_id(mPtr, id);
        }

        @CriticalNative
        private static native long native_init();
        @CriticalNative
        private static native long native_delete();
        @CriticalNative
        private static native long native_get_extra_ptr(long ptr);
        @CriticalNative
        private static native void native_add_field(long ptr, long extraPtr);
        @CriticalNative
        private static native void native_set_id(long ptr, long id);
    }

    @CriticalNative
    private static native long native_init();
    @CriticalNative
    private static native long native_delete();
    @CriticalNative
    private static native void native_add_arg(long ptr, long extraPtr);
    @CriticalNative
    private static native void native_clear_args(long ptr);
    @FastNative
    private static native void native_emit(int type, long tag, String name, long ptr);
}
