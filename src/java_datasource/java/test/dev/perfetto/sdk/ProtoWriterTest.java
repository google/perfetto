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

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import org.junit.Before;
import org.junit.Test;

public class ProtoWriterTest {
    private ProtoWriter w;

    @Before
    public void setUp() {
        w = new ProtoWriter(1024);
    }

    private byte[] out() {
        return Arrays.copyOf(w.buffer(), w.position());
    }

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

    static int fieldId(int tag) { return tag >>> 3; }
    static int wireType(int tag) { return tag & 0x7; }

    @Test
    public void varInt() {
        w.writeVarInt(1, 0);
        w.writeVarInt(2, 42);
        w.writeVarInt(3, 0xFFFFFFFFL);
        w.writeVarInt(4, -1L);

        byte[] b = out();
        int[] off = {0};
        assertEquals(1, fieldId((int) readVarInt(b, off))); assertEquals(0, readVarInt(b, off));
        assertEquals(2, fieldId((int) readVarInt(b, off))); assertEquals(42, readVarInt(b, off));
        assertEquals(3, fieldId((int) readVarInt(b, off))); assertEquals(0xFFFFFFFFL, readVarInt(b, off));
        assertEquals(4, fieldId((int) readVarInt(b, off))); assertEquals(-1L, readVarInt(b, off));
    }

    @Test
    public void sInt() {
        w.writeSInt(1, 0);
        w.writeSInt(2, -1);
        w.writeSInt(3, 1);

        byte[] b = out();
        int[] off = {0};
        readVarInt(b, off); assertEquals(0, readVarInt(b, off));   // zigzag(0) = 0
        readVarInt(b, off); assertEquals(1, readVarInt(b, off));   // zigzag(-1) = 1
        readVarInt(b, off); assertEquals(2, readVarInt(b, off));   // zigzag(1) = 2
    }

    @Test
    public void boolField() {
        w.writeBool(1, true);
        w.writeBool(2, false);

        byte[] b = out();
        int[] off = {0};
        readVarInt(b, off); assertEquals(1, readVarInt(b, off));
        readVarInt(b, off); assertEquals(0, readVarInt(b, off));
    }

    @Test
    public void fixed() {
        w.writeFixed32(1, 0xDEADBEEF);
        w.writeFixed64(2, 0xCAFEBABEDEADFEEDL);
        w.writeFloat(3, 3.14f);
        w.writeDouble(4, 2.71828);

        byte[] b = out();
        int[] off = {0};
        int tag = (int) readVarInt(b, off);
        assertEquals(5, wireType(tag));
        assertEquals(0xDEADBEEFL, ByteBuffer.wrap(b, off[0], 4).order(ByteOrder.LITTLE_ENDIAN).getInt() & 0xFFFFFFFFL);
        off[0] += 4;

        tag = (int) readVarInt(b, off);
        assertEquals(1, wireType(tag));
        assertEquals(0xCAFEBABEDEADFEEDL, ByteBuffer.wrap(b, off[0], 8).order(ByteOrder.LITTLE_ENDIAN).getLong());
        off[0] += 8;

        readVarInt(b, off);
        assertEquals(3.14f, ByteBuffer.wrap(b, off[0], 4).order(ByteOrder.LITTLE_ENDIAN).getFloat(), 0);
        off[0] += 4;

        readVarInt(b, off);
        assertEquals(2.71828, ByteBuffer.wrap(b, off[0], 8).order(ByteOrder.LITTLE_ENDIAN).getDouble(), 0);
    }

    @Test
    public void stringAscii() {
        w.writeString(1, "hello");
        byte[] b = out();
        int[] off = {0};
        assertEquals(1, fieldId((int) readVarInt(b, off)));
        int len = (int) readVarInt(b, off);
        assertEquals("hello", new String(b, off[0], len));
    }

    @Test
    public void stringUtf8() {
        // 2-byte, 3-byte, and 4-byte UTF-8 code points.
        String s = "caf\u00e9\u4e16\uD83D\uDE00";
        w.writeString(1, s);
        byte[] b = out();
        int[] off = {0};
        readVarInt(b, off);
        int len = (int) readVarInt(b, off);
        assertEquals(s, new String(b, off[0], len, java.nio.charset.StandardCharsets.UTF_8));
    }

    @Test
    public void bytesField() {
        byte[] data = {0x01, 0x02, (byte) 0xFF};
        w.writeBytes(1, data);
        byte[] b = out();
        int[] off = {0};
        readVarInt(b, off);
        int len = (int) readVarInt(b, off);
        assertArrayEquals(data, Arrays.copyOfRange(b, off[0], off[0] + len));
    }

    @Test
    public void nestedEmpty() {
        int tok = w.beginNested(1);
        w.endNested(tok);

        byte[] b = out();
        int[] off = {0};
        readVarInt(b, off);
        // Redundant varint: size 0 encoded as 4 bytes.
        assertEquals(0x80, b[off[0]] & 0xFF);
        assertEquals(0x80, b[off[0] + 1] & 0xFF);
        assertEquals(0x80, b[off[0] + 2] & 0xFF);
        assertEquals(0x00, b[off[0] + 3] & 0xFF);
        assertEquals(0, (int) readVarInt(b, off));
    }

    @Test
    public void nestedWithFields() {
        int tok = w.beginNested(1);
        w.writeVarInt(2, 100);
        w.writeString(3, "test");
        w.endNested(tok);

        byte[] b = out();
        int[] off = {0};
        readVarInt(b, off);
        int lenStart = off[0];
        int len = (int) readVarInt(b, off);
        assertEquals(lenStart + 4, off[0]); // redundant varint = 4 bytes
        int end = off[0] + len;

        assertEquals(2, fieldId((int) readVarInt(b, off)));
        assertEquals(100, readVarInt(b, off));
        assertEquals(3, fieldId((int) readVarInt(b, off)));
        int slen = (int) readVarInt(b, off);
        assertEquals("test", new String(b, off[0], slen));
        off[0] += slen;
        assertEquals(end, off[0]);
    }

    @Test
    public void doublyNested() {
        int o = w.beginNested(1);
          int i = w.beginNested(2);
            w.writeVarInt(3, 99);
          w.endNested(i);
          w.writeVarInt(4, 100);
        w.endNested(o);

        byte[] b = out();
        int[] off = {0};

        assertEquals(1, fieldId((int) readVarInt(b, off)));
        int oLen = (int) readVarInt(b, off);
        int oEnd = off[0] + oLen;

        assertEquals(2, fieldId((int) readVarInt(b, off)));
        int iLen = (int) readVarInt(b, off);
        int iEnd = off[0] + iLen;

        assertEquals(3, fieldId((int) readVarInt(b, off)));
        assertEquals(99, readVarInt(b, off));
        assertEquals(iEnd, off[0]);

        assertEquals(4, fieldId((int) readVarInt(b, off)));
        assertEquals(100, readVarInt(b, off));
        assertEquals(oEnd, off[0]);
    }

    @Test
    public void triplyNested() {
        int l1 = w.beginNested(1);
          int l2 = w.beginNested(2);
            int l3 = w.beginNested(3);
              w.writeVarInt(4, 42);
            w.endNested(l3);
          w.endNested(l2);
        w.endNested(l1);

        byte[] b = out();
        int[] off = {0};
        readVarInt(b, off); int s1 = (int) readVarInt(b, off); int e1 = off[0] + s1;
        readVarInt(b, off); int s2 = (int) readVarInt(b, off); int e2 = off[0] + s2;
        readVarInt(b, off); int s3 = (int) readVarInt(b, off); int e3 = off[0] + s3;
        readVarInt(b, off); assertEquals(42, readVarInt(b, off));
        assertEquals(e3, off[0]);
        assertEquals(e2, off[0]);
        assertEquals(e1, off[0]);
    }

    @Test
    public void allPrimitivesInNested() {
        int tok = w.beginNested(1);
        w.writeVarInt(2, 42);
        w.writeSInt(3, -100);
        w.writeBool(4, true);
        w.writeFixed32(5, 0x12345678);
        w.writeFixed64(6, 0xCAFEBABEL);
        w.writeFloat(7, 1.5f);
        w.writeDouble(8, 9.81);
        w.writeString(9, "test");
        w.writeBytes(10, new byte[]{1, 2, 3});
        w.endNested(tok);

        byte[] b = out();
        int[] off = {0};
        readVarInt(b, off);
        int len = (int) readVarInt(b, off);
        assertTrue(len > 30);
        assertEquals(b.length, off[0] + len);
    }

    @Test
    public void resetReusesBuffer() {
        byte[] before = w.buffer();
        w.writeVarInt(1, 42);
        w.reset();
        assertSame(before, w.buffer());
        assertEquals(0, w.position());
    }

    @Test
    public void bufferGrowth() {
        ProtoWriter small = new ProtoWriter(16);
        for (int i = 0; i < 10; i++) {
            small.writeVarInt(1, 0xFFFFFFFFL);
        }
        assertTrue(small.position() > 16);
        // Verify output is still valid.
        byte[] b = Arrays.copyOf(small.buffer(), small.position());
        int[] off = {0};
        for (int i = 0; i < 10; i++) {
            readVarInt(b, off);
            assertEquals(0xFFFFFFFFL, readVarInt(b, off));
        }
    }

    @Test
    public void largeRedundantVarInt() {
        // Nested message > 127 bytes to exercise multi-byte redundant varint.
        int tok = w.beginNested(1);
        for (int i = 0; i < 40; i++) {
            w.writeVarInt(2, 0xFFFFL);
        }
        w.endNested(tok);

        byte[] b = out();
        int[] off = {0};
        readVarInt(b, off);
        int lenStart = off[0];
        int len = (int) readVarInt(b, off);
        assertEquals(lenStart + 4, off[0]); // still 4-byte redundant varint
        assertTrue(len > 127);
    }
}
