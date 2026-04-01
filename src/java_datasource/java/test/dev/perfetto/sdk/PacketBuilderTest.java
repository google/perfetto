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

import java.util.Arrays;
import org.junit.Test;

public class PacketBuilderTest {

    static long readVarInt(byte[] data, int[] off) {
        long r = 0;
        int shift = 0;
        while (off[0] < data.length) {
            byte b = data[off[0]++];
            r |= (long) (b & 0x7F) << shift;
            if ((b & 0x80) == 0) return r;
            shift += 7;
        }
        throw new RuntimeException("truncated varint");
    }

    static void skipField(byte[] data, int[] off, int wt) {
        switch (wt) {
            case 0: readVarInt(data, off); break;
            case 1: off[0] += 8; break;
            case 2: off[0] += (int) readVarInt(data, off); break;
            case 5: off[0] += 4; break;
        }
    }

    @Test
    public void matchesRawProtoWriter() {
        ProtoWriter w = new ProtoWriter(1024);
        PacketBuilder pb = new PacketBuilder(null);

        pb.start(w)
                .writeVarInt(1, 42)
                .beginNested(2)
                    .writeString(3, "hello")
                .endNested()
                .commit();
        byte[] builderOut = Arrays.copyOf(w.buffer(), w.position());

        ProtoWriter raw = new ProtoWriter(1024);
        raw.writeVarInt(1, 42);
        int tok = raw.beginNested(2);
        raw.writeString(3, "hello");
        raw.endNested(tok);
        byte[] rawOut = Arrays.copyOf(raw.buffer(), raw.position());

        assertArrayEquals(rawOut, builderOut);
    }

    @Test
    public void autoCloseOnCommit() {
        ProtoWriter w = new ProtoWriter(1024);
        PacketBuilder pb = new PacketBuilder(null);

        pb.start(w)
                .beginNested(1)
                    .writeVarInt(2, 42)
                // no endNested -- commit auto-closes
                .commit();

        byte[] b = Arrays.copyOf(w.buffer(), w.position());
        int[] off = {0};
        int tag = (int) readVarInt(b, off);
        assertEquals(1, tag >>> 3);
        int len = (int) readVarInt(b, off);
        int end = off[0] + len;
        readVarInt(b, off);
        assertEquals(42, readVarInt(b, off));
        assertEquals(end, off[0]);
    }

    @Test
    public void reuse() {
        ProtoWriter w = new ProtoWriter(1024);
        PacketBuilder pb = new PacketBuilder(null);

        pb.start(w).writeVarInt(1, 10).commit();
        byte[] first = Arrays.copyOf(w.buffer(), w.position());

        w.reset();
        pb.start(w).writeString(1, "second").commit();
        byte[] second = Arrays.copyOf(w.buffer(), w.position());

        assertFalse(Arrays.equals(first, second));
    }

    @Test
    public void allFieldTypes() {
        ProtoWriter w = new ProtoWriter(1024);
        PacketBuilder pb = new PacketBuilder(null);

        pb.start(w)
                .writeVarInt(1, 42)
                .writeSInt(2, -1)
                .writeBool(3, true)
                .writeFixed32(4, 0x12345678)
                .writeFixed64(5, 0xDEADL)
                .writeFloat(6, 1.5f)
                .writeDouble(7, 3.14)
                .writeString(8, "test")
                .writeBytes(9, new byte[]{1, 2})
                .commit();

        assertTrue(w.position() > 30);
    }
}
