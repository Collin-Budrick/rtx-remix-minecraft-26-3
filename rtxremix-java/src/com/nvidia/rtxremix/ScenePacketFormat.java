/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Comparator;
import java.util.IdentityHashMap;

/** Versioned retained-scene interchange. Vertex bytes remain HardcodedVertex, not a new mesh format. */
public final class ScenePacketFormat {
    public static final int MAGIC = 0x424C5352;
    public static final int VERSION = 1;
    public static final int HEADER_BYTES = 32;
    public static final int FULL_RESET = 1;
    public static final int INCOMPLETE = 2;
    public static final int TEXTURE = 1, REMOVE_TEXTURE = 2;
    public static final int MATERIAL = 3, REMOVE_MATERIAL = 4;
    public static final int MESH = 5, REMOVE_MESH = 6;
    public static final int INSTANCE = 7, REMOVE_INSTANCE = 8;
    public static final int CAMERA = 9, LIGHT = 10, REMOVE_LIGHT = 11;
    public static final int TEXTURE_PATCH = 12, TEXTURE_FILE = 13;

    private ScenePacketFormat() {}

    static final class Writer {
        private final ByteArrayOutputStream out = new ByteArrayOutputStream();
        Writer i(int value) {
            out.write(value); out.write(value >>> 8); out.write(value >>> 16); out.write(value >>> 24);
            return this;
        }
        Writer l(long value) { return i((int) value).i((int) (value >>> 32)); }
        Writer f(float value) { return i(Float.floatToIntBits(value)); }
        Writer floats(float[] values, int count) {
            if (values == null || values.length < count) throw new IllegalArgumentException("Short float array");
            for (int n = 0; n < count; n++) f(values[n]);
            return this;
        }
        Writer xyz(Float3D value) { return f(value.x).f(value.y).f(value.z); }
        Writer bytes(byte[] value) { out.write(value, 0, value.length); return this; }
        Writer text(String value) {
            byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
            return i(bytes.length).bytes(bytes);
        }
        byte[] finish() { return out.toByteArray(); }
    }

    static byte[] copy(ByteBuffer buffer, long count) {
        if (count == 0) return new byte[0];
        if (count < 0 || count > Integer.MAX_VALUE || buffer == null || buffer.capacity() < count) {
            throw new IllegalArgumentException("Scene buffer does not contain its declared data: " + count);
        }
        byte[] result = new byte[(int) count];
        // JNI passes the address of the buffer base, independently of its current position.
        ByteBuffer view = buffer.duplicate();
        view.clear();
        view.get(result);
        return result;
    }

    static int pixelBytes(int format) {
        switch (format) {
            case TextureFormat.RGBA8_UNORM: case TextureFormat.RGBA8_SRGB:
            case TextureFormat.BGRA8_UNORM: case TextureFormat.BGRA8_SRGB:
            case TextureFormat.R32_SFLOAT: return 4;
            case TextureFormat.R8_UNORM: return 1;
            case TextureFormat.RG8_UNORM: return 2;
            case TextureFormat.RGBA16_SFLOAT: return 8;
            case TextureFormat.RGBA32_SFLOAT: return 16;
            default: throw new IllegalArgumentException("Unsupported texture format " + format);
        }
    }

    static String metadata(Object value) {
        StringBuilder out = new StringBuilder();
        json(value, out, new IdentityHashMap<Object, Boolean>());
        return out.toString();
    }

    private static void json(Object value, StringBuilder out, IdentityHashMap<Object, Boolean> seen) {
        if (value == null) { out.append("null"); return; }
        if (value instanceof String || value instanceof Character) { quote(value.toString(), out); return; }
        if (value instanceof Boolean || value instanceof Number) {
            if (value instanceof Float && !Float.isFinite((Float) value)
                    || value instanceof Double && !Double.isFinite((Double) value)) out.append("null");
            else out.append(value);
            return;
        }
        if (value instanceof ByteBuffer) { out.append("null"); return; }
        if (seen.put(value, Boolean.TRUE) != null) throw new IllegalArgumentException("Cyclic scene metadata");
        try {
            if (value.getClass().isArray()) {
                out.append('[');
                for (int i = 0; i < Array.getLength(value); i++) {
                    if (i != 0) out.append(',');
                    json(Array.get(value, i), out, seen);
                }
                out.append(']');
                return;
            }
            Field[] fields = value.getClass().getFields();
            Arrays.sort(fields, new Comparator<Field>() {
                public int compare(Field a, Field b) { return a.getName().compareTo(b.getName()); }
            });
            out.append('{');
            boolean comma = false;
            for (Field field : fields) {
                if (Modifier.isStatic(field.getModifiers()) || field.getName().equals("nativePtr")) continue;
                if (comma) out.append(',');
                quote(field.getName(), out); out.append(':');
                try { json(field.get(value), out, seen); }
                catch (IllegalAccessException e) { throw new IllegalArgumentException(e); }
                comma = true;
            }
            out.append('}');
        } finally { seen.remove(value); }
    }

    static void quote(String value, StringBuilder out) {
        out.append('"');
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '"' || c == '\\') out.append('\\').append(c);
            else if (c < 32) {
                String hex = Integer.toHexString(c);
                out.append("\\u");
                for (int n = hex.length(); n < 4; n++) out.append('0');
                out.append(hex);
            } else out.append(c);
        }
        out.append('"');
    }
}
