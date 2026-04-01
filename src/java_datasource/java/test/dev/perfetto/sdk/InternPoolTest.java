/*
 * Copyright (C) 2026 The Android Open Source Project
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

import static org.junit.Assert.*;

import org.junit.Test;

public class InternPoolTest {
    @Test
    public void internAndCache() {
        InternPool pool = new InternPool();
        InternPool.InternResult r = pool.intern(0, "hello");
        assertTrue(r.isNew);
        assertEquals(1, r.iid);

        r = pool.intern(0, "hello");
        assertFalse(r.isNew);
        assertEquals(1, r.iid);

        r = pool.intern(0, "world");
        assertTrue(r.isNew);
        assertEquals(2, r.iid);
    }

    @Test
    public void independentTypes() {
        InternPool pool = new InternPool();
        pool.intern(0, "hello");

        InternPool.InternResult r = pool.intern(1, "hello");
        assertTrue(r.isNew);
        assertEquals(1, r.iid); // independent iid space
    }

    @Test
    public void reset() {
        InternPool pool = new InternPool();
        pool.intern(0, "hello");
        assertEquals(0, pool.generation());

        pool.reset();
        assertEquals(1, pool.generation());

        InternPool.InternResult r = pool.intern(0, "hello");
        assertTrue(r.isNew);
        assertEquals(1, r.iid); // restarts from 1
    }

    @Test
    public void resultObjectReused() {
        InternPool pool = new InternPool();
        InternPool.InternResult r1 = pool.intern(0, "a");
        InternPool.InternResult r2 = pool.intern(0, "b");
        assertSame(r1, r2);
    }
}
