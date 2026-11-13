/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Opt-in CPU mirror of successful shared API operations. It does not render, replace Remix,
 * or cull the scene. Owned copies survive native buffer reuse and permit asynchronous exports.
 */
public final class ScenePacketRecorder implements AutoCloseable {
    private final Map<Long, Record> textures = new LinkedHashMap<Long, Record>();
    private final Map<Long, TexturePixels> pixels = new HashMap<Long, TexturePixels>();
    private final Map<Long, Record> materials = new LinkedHashMap<Long, Record>();
    private final Map<Long, Record> meshes = new LinkedHashMap<Long, Record>();
    private final Map<Long, Record> instances = new LinkedHashMap<Long, Record>();
    private final Map<Long, Record> lightAssets = new LinkedHashMap<Long, Record>();
    private final Map<Long, Record> activeLights = new LinkedHashMap<Long, Record>();
    private final Map<String, Record> dirty = new LinkedHashMap<String, Record>();
    private final List<Record> filePatches = new ArrayList<Record>();
    private Set<Long> transientInstances = new HashSet<Long>();
    private final Set<Long> currentTransients = new HashSet<Long>();
    private final Set<Long> drawnLights = new HashSet<Long>();
    private final Set<Long> incompleteInstances = new HashSet<Long>();
    private long frameId, nextPatchId, nextTransient = Long.MIN_VALUE;
    private int viewportWidth, viewportHeight;
    private Record camera;
    private String failure;
    private boolean closed;
    private final Object exportLock = new Object();

    public ScenePacketRecorder() { ScenePacketExtensions.begin(); }

    public synchronized void failed(Throwable error) {
        if (failure == null) failure = error.getClass().getSimpleName() + ": " + error.getMessage();
    }

    public synchronized String failure() { return failure; }
    public synchronized long frameId() { return frameId; }
    public synchronized int instanceCount() { return instances.size(); }
    public synchronized int meshCount() { return meshes.size(); }
    public synchronized int textureCount() { return textures.size(); }
    public synchronized boolean complete() { return failure == null && incompleteInstances.isEmpty(); }
    public synchronized boolean ready() {
        return !closed && failure == null && camera != null && viewportWidth > 0 && viewportHeight > 0 && !instances.isEmpty();
    }
    public synchronized void viewport(int width, int height) { viewportWidth = width; viewportHeight = height; }

    public synchronized void texture(long id, TextureFromMemoryInfo info) {
        checkOpen();
        if (info.width <= 0 || info.height <= 0) throw new IllegalArgumentException("Empty texture");
        byte[] data = ScenePacketFormat.copy(info.pixelData,
                (long) info.width * info.height * ScenePacketFormat.pixelBytes(info.format));
        TexturePixels image = new TexturePixels(info.hash, info.width, info.height, info.format, info.purpose, data);
        discardTexturePatches(id);
        pixels.put(id, image);
        put(textures, textureRecord(id, image));
    }

    public synchronized void textureFile(long id, String path) {
        checkOpen();
        discardTexturePatches(id);
        pixels.remove(id);
        put(textures, new Record(ScenePacketFormat.TEXTURE_FILE, id,
                new ScenePacketFormat.Writer().l(id).text(path).finish()));
    }

    public synchronized void texturePatches(TexturePatchInfo info) {
        checkOpen();
        if (info.entries == null) return;
        for (TextureBatchEntry entry : info.entries) {
            TexturePixels old = pixels.get(entry.handle);
            if (old == null) {
                if (!textures.containsKey(entry.handle)) throw new IllegalArgumentException("Patch refers to an uncaptured texture");
                for (TexturePatch patch : entry.patches) {
                    if (patch.dstX < 0 || patch.dstY < 0 || patch.width <= 0 || patch.height <= 0) {
                        throw new IllegalArgumentException("Invalid texture patch bounds");
                    }
                    int rowBytes = Math.multiplyExact(patch.width, ScenePacketFormat.pixelBytes(patch.format));
                    int stride = patch.rowPitch != 0 ? patch.rowPitch
                            : rowBytes;
                    if (stride < rowBytes) throw new IllegalArgumentException("Short patch row pitch");
                    Record record = new Record(ScenePacketFormat.TEXTURE_PATCH, entry.handle,
                            new ScenePacketFormat.Writer().l(entry.handle).i(patch.dstX).i(patch.dstY)
                                    .i(patch.width).i(patch.height).i(stride).i(patch.format)
                                    .i(Math.multiplyExact(stride, patch.height))
                                    .bytes(ScenePacketFormat.copy(patch.pixelData, (long) stride * patch.height)).finish());
                    filePatches.add(record);
                    dirty.put("patch:" + ++nextPatchId, record);
                }
                continue;
            }
            byte[] updated = old.data.clone();
            int pixelSize = ScenePacketFormat.pixelBytes(old.format);
            for (TexturePatch patch : entry.patches) {
                if (patch.format != old.format || patch.dstX < 0 || patch.dstY < 0
                        || patch.width < 0 || patch.height < 0
                        || (long) patch.dstX + patch.width > old.width
                        || (long) patch.dstY + patch.height > old.height) {
                    throw new IllegalArgumentException("Texture patch does not match its captured texture");
                }
                int rowBytes = Math.multiplyExact(patch.width, pixelSize);
                int stride = patch.rowPitch == 0 ? rowBytes : patch.rowPitch;
                if (stride < rowBytes) throw new IllegalArgumentException("Short patch row pitch");
                byte[] source = ScenePacketFormat.copy(patch.pixelData, (long) stride * patch.height);
                for (int row = 0; row < patch.height; row++) {
                    System.arraycopy(source, row * stride, updated,
                            ((patch.dstY + row) * old.width + patch.dstX) * pixelSize, rowBytes);
                }
            }
            TexturePixels image = new TexturePixels(old.hash, old.width, old.height, old.format, old.purpose, updated);
            pixels.put(entry.handle, image);
            put(textures, textureRecord(entry.handle, image));
        }
    }

    private Record textureRecord(long id, TexturePixels image) {
        return new Record(ScenePacketFormat.TEXTURE, id,
                new ScenePacketFormat.Writer().l(id).l(image.hash).i(image.width).i(image.height)
                        .i(image.format).i(image.purpose).i(image.data.length).bytes(image.data).finish());
    }

    public synchronized void removeTexture(long id) {
        pixels.remove(id);
        discardTexturePatches(id);
        remove(textures, id, ScenePacketFormat.REMOVE_TEXTURE);
    }

    private void discardTexturePatches(long id) {
        for (int i = filePatches.size() - 1; i >= 0; i--) if (filePatches.get(i).id == id) filePatches.remove(i);
        java.util.Iterator<Record> iterator = dirty.values().iterator();
        while (iterator.hasNext()) {
            Record record = iterator.next();
            if (record.type == ScenePacketFormat.TEXTURE_PATCH && record.id == id) iterator.remove();
        }
    }

    public synchronized void material(long id, MaterialInfo info) {
        checkOpen();
        MaterialInfoOpaqueEXT opaque = info.opaqueExt;
        MaterialInfoTranslucentEXT glass = info.translucentExt;
        Float3D color = opaque != null ? opaque.albedoConstant : new Float3D(1, 1, 1);
        float opacity = opaque != null ? opaque.opacityConstant : 1;
        int alphaMode = glass != null ? 2 : opaque != null && opaque.alphaTestType != 7 ? 1 : 0;
        byte[] data = new ScenePacketFormat.Writer().l(id).l(info.hash).i(glass != null ? 1 : 0).i(0)
                .l(info.albedoTexture).l(info.normalTexture).l(info.emissiveTexture)
                .l(opaque != null ? opaque.roughnessTexture : 0).l(opaque != null ? opaque.metallicTexture : 0)
                .l(opaque != null ? opaque.heightTexture : 0).l(glass != null ? glass.transmittanceTexture : 0)
                .l(info.tangentTexture).xyz(color).f(opacity).xyz(info.emissiveColorConstant)
                .f(info.emissiveIntensity).f(opaque != null ? opaque.roughnessConstant : 0)
                .f(opaque != null ? opaque.metallicConstant : 0).f(glass != null ? glass.refractiveIndex : 1.5f)
                .f(opaque != null ? (opaque.alphaReferenceValue & 255) / 255f : 0)
                .i(alphaMode).i(0).text(ScenePacketFormat.metadata(info)).finish();
        put(materials, new Record(ScenePacketFormat.MATERIAL, id, data));
    }

    public synchronized void removeMaterial(long id) { remove(materials, id, ScenePacketFormat.REMOVE_MATERIAL); }

    public synchronized void mesh(long id, MeshInfo info) {
        checkOpen();
        if (info.surfaces == null || info.surfaces.length == 0) throw new IllegalArgumentException("Empty mesh");
        ScenePacketFormat.Writer out = new ScenePacketFormat.Writer().l(id).l(info.hash).i(info.surfaces.length);
        for (MeshInfoSurfaceTriangles surface : info.surfaces) {
            int weights = surface.skinning_hasvalue ? surface.skinning_blendWeightsCount : 0;
            int indices = surface.skinning_hasvalue ? surface.skinning_blendIndicesCount : 0;
            int bonesPerVertex = surface.skinning_hasvalue ? surface.skinning_bonesPerVertex : 0;
            if (surface.vertexCount <= 0 || surface.indexCount <= 0 || surface.indexCount % 3 != 0
                    || bonesPerVertex < 0 || weights < 0 || indices < 0) {
                throw new IllegalArgumentException("Invalid captured triangle mesh counts");
            }
            int expectedWeights = Math.multiplyExact(surface.vertexCount, bonesPerVertex);
            if (weights != expectedWeights || indices != expectedWeights) {
                throw new IllegalArgumentException("Skin weights and indices must each contain vertexCount * bonesPerVertex entries");
            }
            out.l(surface.material).i(surface.vertexCount).i(surface.indexCount).i(HardcodedVertex.STRIDE)
                    .i(surface.flags).i(bonesPerVertex)
                    .i(weights).i(indices).i(0)
                    .bytes(ScenePacketFormat.copy(surface.vertexBuffer, (long) surface.vertexCount * HardcodedVertex.STRIDE))
                    .bytes(ScenePacketFormat.copy(surface.indexBuffer, (long) surface.indexCount * 4));
            if (weights != 0) out.bytes(ScenePacketFormat.copy(surface.skinning_blendWeights, (long) weights * 4));
            if (indices != 0) out.bytes(ScenePacketFormat.copy(surface.skinning_blendIndices, (long) indices * 4));
        }
        put(meshes, new Record(ScenePacketFormat.MESH, id, out.finish()));
    }

    public synchronized void removeMesh(long id) { remove(meshes, id, ScenePacketFormat.REMOVE_MESH); }

    public synchronized void instance(long id, long mesh, float[] transform, int flags, int doubleSided, long pNext) {
        checkOpen();
        ScenePacketExtensions.Data ext = ScenePacketExtensions.resolve(pNext);
        if (ext.complete) incompleteInstances.remove(id); else incompleteInstances.add(id);
        String blend = ext.complete ? ext.blend : "{\"unsupportedNativeExtension\":true}";
        byte[] blendBytes = blend.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        byte[] data = new ScenePacketFormat.Writer().l(id).l(mesh).i(flags).i(doubleSided).floats(transform, 12)
                .i(ext.bones.length / 48).i(ext.gpu.length / 48).i(blendBytes.length).i(0)
                .bytes(ext.bones).bytes(ext.gpu).bytes(blendBytes).finish();
        put(instances, new Record(ScenePacketFormat.INSTANCE, id, data));
    }

    public synchronized void transientInstance(long mesh, float[] transform, int flags, int doubleSided, long pNext) {
        long id = ++nextTransient;
        instance(id, mesh, transform, flags, doubleSided, pNext);
        currentTransients.add(id);
    }

    public synchronized void removeInstance(long id) {
        incompleteInstances.remove(id);
        remove(instances, id, ScenePacketFormat.REMOVE_INSTANCE);
    }

    public synchronized void camera(CameraInfo info) {
        checkOpen();
        if (info.type != CameraType.WORLD) return;
        CameraInfoParameterizedEXT p = info.parameterizedExt;
        ScenePacketFormat.Writer out = new ScenePacketFormat.Writer().i(info.type).i(p != null ? 1 : 0)
                .i(viewportWidth).i(viewportHeight).floats(info.view, 16).floats(info.projection, 16);
        if (p != null) out.xyz(p.position).xyz(p.forward).xyz(p.up).xyz(p.right)
                .f(p.fovYInDegrees).f(p.aspect).f(p.nearPlane).f(p.farPlane);
        else out.floats(new float[16], 16);
        camera = new Record(ScenePacketFormat.CAMERA, 0, out.finish());
        dirty.put("camera", camera);
    }

    public synchronized void light(long id, LightInfo info) {
        checkOpen();
        int type;
        Float3D position = new Float3D(), direction = new Float3D(0, -1, 0);
        Float3D xAxis = new Float3D(1, 0, 0), yAxis = new Float3D(0, 1, 0);
        float[] size = new float[4];
        float[] transform = new Transform().matrix;
        long texture = 0;
        if (info.sphereExt != null) {
            type = 0; position = info.sphereExt.position; size[0] = info.sphereExt.radius;
        } else if (info.distantExt != null) {
            type = 1; direction = info.distantExt.direction; size[0] = info.distantExt.angularDiameterDegrees;
        } else if (info.rectExt != null) {
            type = 2; position = info.rectExt.position; direction = info.rectExt.direction;
            xAxis = info.rectExt.xAxis; yAxis = info.rectExt.yAxis;
            size[0] = info.rectExt.xSize; size[1] = info.rectExt.ySize;
        } else if (info.diskExt != null) {
            type = 3; position = info.diskExt.position; direction = info.diskExt.direction;
            xAxis = info.diskExt.xAxis; yAxis = info.diskExt.yAxis;
            size[0] = info.diskExt.xRadius; size[1] = info.diskExt.yRadius;
        } else if (info.cylinderExt != null) {
            type = 4; position = info.cylinderExt.position; direction = info.cylinderExt.axis;
            size[0] = info.cylinderExt.radius; size[1] = info.cylinderExt.axisLength;
        } else if (info.domeExt != null) {
            type = 5; texture = info.domeExt.colorTexture; transform = info.domeExt.transform;
            size[0] = info.domeExt.opacity; size[1] = info.domeExt.blendType;
        } else throw new IllegalArgumentException("Light has no recognized shape");
        Record record = new Record(ScenePacketFormat.LIGHT, id,
                new ScenePacketFormat.Writer().l(id).l(info.hash).i(type).i(0).xyz(info.radiance)
                        .xyz(position).xyz(direction).xyz(xAxis).xyz(yAxis).floats(size, 4).l(texture)
                        .floats(transform, 12).text(lightMetadata(info)).finish());
        lightAssets.put(id, record);
        if (activeLights.containsKey(id)) put(activeLights, record);
    }

    public synchronized void lightDrawn(long id) { drawnLights.add(id); }
    public synchronized void removeLight(long id) {
        lightAssets.remove(id); drawnLights.remove(id);
        remove(activeLights, id, ScenePacketFormat.REMOVE_LIGHT);
    }

    public synchronized void frameSubmitted() {
        checkOpen();
        for (long id : transientInstances) removeInstance(id);
        transientInstances = new HashSet<Long>(currentTransients);
        currentTransients.clear();
        for (long id : new ArrayList<Long>(activeLights.keySet())) {
            if (!drawnLights.contains(id)) remove(activeLights, id, ScenePacketFormat.REMOVE_LIGHT);
        }
        for (long id : drawnLights) {
            Record record = lightAssets.get(id);
            if (record != null && activeLights.get(id) != record) put(activeLights, record);
        }
        drawnLights.clear();
        frameId++;
    }

    /** Discards even unsubmitted frame state on a world change while retaining shared texture/material/mesh assets. */
    public synchronized void clearScene() {
        if (closed) return;
        for (long id : new ArrayList<Long>(instances.keySet())) removeInstance(id);
        for (long id : new ArrayList<Long>(activeLights.keySet())) remove(activeLights, id, ScenePacketFormat.REMOVE_LIGHT);
        transientInstances.clear(); currentTransients.clear(); drawnLights.clear(); incompleteInstances.clear();
        camera = null;
        dirty.remove("camera");
    }

    /** Writes a full scene atomically. Large scenes stream records instead of requiring one giant buffer. */
    public ExportSummary writeSnapshot(Path destination) throws IOException {
        synchronized (exportLock) { return write(destination, true); }
    }

    /** Dirty records are acknowledged only after the complete file was successfully committed. */
    public ExportSummary writeUpdates(Path destination) throws IOException {
        synchronized (exportLock) { return write(destination, false); }
    }

    /** Owns a synchronous native transport packet. Acknowledge only after the receiver accepts every record. */
    public PacketBatch packet(boolean full) {
        List<Record> records = new ArrayList<Record>();
        Map<String, Record> pending;
        long frame;
        int flags;
        synchronized (this) {
            checkOpen();
            pending = new LinkedHashMap<String, Record>(dirty);
            if (full) {
                records.addAll(textures.values()); records.addAll(filePatches);
                records.addAll(materials.values()); records.addAll(meshes.values());
                records.addAll(instances.values()); records.addAll(activeLights.values());
                if (camera != null) records.add(camera);
            } else records.addAll(pending.values());
            frame = frameId;
            flags = (full ? ScenePacketFormat.FULL_RESET : 0) | (complete() ? 0 : ScenePacketFormat.INCOMPLETE);
        }
        long payload = 0;
        for (Record record : records) payload = Math.addExact(payload, 8L + record.data.length);
        if (payload > Integer.MAX_VALUE - ScenePacketFormat.HEADER_BYTES) {
            throw new IllegalStateException("In-process scene packet exceeds the Java direct-buffer limit");
        }
        java.nio.ByteBuffer bytes = java.nio.ByteBuffer.allocateDirect(ScenePacketFormat.HEADER_BYTES + (int) payload)
                .order(java.nio.ByteOrder.LITTLE_ENDIAN);
        bytes.putInt(ScenePacketFormat.MAGIC).putInt(ScenePacketFormat.VERSION).putInt(flags).putInt(records.size())
                .putLong(frame).putInt((int) payload).putInt(0);
        for (Record record : records) bytes.putInt(record.type).putInt(record.data.length).put(record.data);
        bytes.flip();
        return new PacketBatch(this, bytes, pending, frame, full);
    }

    public synchronized void acknowledge(PacketBatch packet) {
        if (packet.owner != this || packet.acknowledged) throw new IllegalArgumentException("Foreign or acknowledged scene packet");
        for (Map.Entry<String, Record> entry : packet.pending.entrySet()) {
            if (dirty.get(entry.getKey()) == entry.getValue()) dirty.remove(entry.getKey());
        }
        packet.acknowledged = true;
    }

    public static final class PacketBatch {
        private final ScenePacketRecorder owner;
        private final java.nio.ByteBuffer bytes;
        private final Map<String, Record> pending;
        private boolean acknowledged;
        public final long frame;
        public final boolean full;
        PacketBatch(ScenePacketRecorder owner, java.nio.ByteBuffer bytes, Map<String, Record> pending, long frame, boolean full) {
            this.owner = owner; this.bytes = bytes; this.pending = pending; this.frame = frame; this.full = full;
        }
        public java.nio.ByteBuffer bytes() {
            return bytes.asReadOnlyBuffer().order(java.nio.ByteOrder.LITTLE_ENDIAN);
        }
    }

    private ExportSummary write(Path destination, boolean full) throws IOException {
        List<Record> records;
        Map<String, Record> pending;
        long frame;
        int flags;
        synchronized (this) {
            if (closed || failure != null) throw new IOException("Scene capture unavailable: " + failure);
            records = new ArrayList<Record>();
            pending = new LinkedHashMap<String, Record>(dirty);
            if (full) {
                records.addAll(textures.values()); records.addAll(filePatches);
                records.addAll(materials.values()); records.addAll(meshes.values());
                records.addAll(instances.values()); records.addAll(activeLights.values());
                if (camera != null) records.add(camera);
            } else records.addAll(pending.values());
            frame = frameId;
            flags = (full ? ScenePacketFormat.FULL_RESET : 0) | (complete() ? 0 : ScenePacketFormat.INCOMPLETE);
        }
        long payload = 0;
        for (Record record : records) payload = Math.addExact(payload, 8L + record.data.length);
        if (payload > 0xffffffffL) throw new IOException("Scene packet exceeds the v1 4 GiB payload limit");
        Path absolute = destination.toAbsolutePath();
        Path parent = absolute.getParent();
        if (parent != null) Files.createDirectories(parent);
        Path temporary = Files.createTempFile(parent, ".scene-", ".tmp");
        try {
            try (OutputStream out = Files.newOutputStream(temporary)) {
                out.write(new ScenePacketFormat.Writer().i(ScenePacketFormat.MAGIC).i(ScenePacketFormat.VERSION)
                        .i(flags).i(records.size()).l(frame).i((int) payload).i(0).finish());
                for (Record record : records) {
                    out.write(new ScenePacketFormat.Writer().i(record.type).i(record.data.length).finish());
                    out.write(record.data);
                }
            }
            try { Files.move(temporary, absolute, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING); }
            catch (AtomicMoveNotSupportedException e) { Files.move(temporary, absolute, StandardCopyOption.REPLACE_EXISTING); }
            synchronized (this) {
                for (Map.Entry<String, Record> entry : pending.entrySet()) {
                    if (dirty.get(entry.getKey()) == entry.getValue()) dirty.remove(entry.getKey());
                }
            }
            int meshCount = 0, instanceCount = 0, textureCount = 0;
            for (Record record : records) {
                if (record.type == ScenePacketFormat.MESH) meshCount++;
                if (record.type == ScenePacketFormat.INSTANCE) instanceCount++;
                if (record.type == ScenePacketFormat.TEXTURE || record.type == ScenePacketFormat.TEXTURE_FILE) textureCount++;
            }
            return new ExportSummary(frame, meshCount, instanceCount, textureCount,
                    (flags & ScenePacketFormat.INCOMPLETE) == 0);
        } finally { Files.deleteIfExists(temporary); }
    }

    private void put(Map<Long, Record> map, Record record) {
        map.put(record.id, record);
        dirty.put(key(record.type, record.id), record);
    }
    private void remove(Map<Long, Record> map, long id, int type) {
        if (map.remove(id) != null) dirty.put(key(type, id),
                new Record(type, id, new ScenePacketFormat.Writer().l(id).finish()));
    }
    private static String key(int type, long id) {
        int family = type == ScenePacketFormat.TEXTURE_FILE ? ScenePacketFormat.TEXTURE
                : type == ScenePacketFormat.LIGHT || type == ScenePacketFormat.REMOVE_LIGHT ? ScenePacketFormat.LIGHT
                : type == ScenePacketFormat.CAMERA ? type : (type & 1) == 0 ? type - 1 : type;
        return family + ":" + id;
    }

    private static String lightMetadata(LightInfo info) {
        Object shape = info.sphereExt != null ? info.sphereExt : info.distantExt != null ? info.distantExt
                : info.rectExt != null ? info.rectExt : info.diskExt != null ? info.diskExt
                : info.cylinderExt != null ? info.cylinderExt : info.domeExt;
        boolean nondefault = false;
        try { nondefault = shape.getClass().getField("shaping_hasvalue").getBoolean(shape); }
        catch (NoSuchFieldException ignored) { }
        catch (IllegalAccessException e) { throw new IllegalArgumentException(e); }
        try { nondefault |= shape.getClass().getField("volumetricRadianceScale").getFloat(shape) != 1f; }
        catch (NoSuchFieldException ignored) { }
        catch (IllegalAccessException e) { throw new IllegalArgumentException(e); }
        return nondefault ? ScenePacketFormat.metadata(shape) : "";
    }
    private void checkOpen() {
        if (closed || failure != null) throw new IllegalStateException("Scene recorder is closed or failed: " + failure);
    }
    public synchronized void close() {
        if (closed) return;
        closed = true;
        textures.clear(); pixels.clear(); materials.clear(); meshes.clear(); instances.clear();
        lightAssets.clear(); activeLights.clear(); dirty.clear(); filePatches.clear();
        ScenePacketExtensions.end();
    }
    /** Counts refer to records in the committed packet, not the concurrently changing scene. */
    public static final class ExportSummary {
        public final long frame;
        public final int meshes, instances, textures;
        public final boolean complete;
        ExportSummary(long frame, int meshes, int instances, int textures, boolean complete) {
            this.frame = frame; this.meshes = meshes; this.instances = instances;
            this.textures = textures; this.complete = complete;
        }
    }
    private static final class Record {
        final int type;
        final long id;
        final byte[] data;
        Record(int type, long id, byte[] data) { this.type = type; this.id = id; this.data = data; }
    }
    private static final class TexturePixels {
        final long hash;
        final int width, height, format, purpose;
        final byte[] data;
        TexturePixels(long hash, int width, int height, int format, int purpose, byte[] data) {
            this.hash = hash; this.width = width; this.height = height; this.format = format;
            this.purpose = purpose; this.data = data;
        }
    }
}
