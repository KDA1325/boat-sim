"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const core = require("../viewer-core.js");

function fakeFile(path, textContent = "") {
    return {
        name: path.split(/[\\/]/).pop(),
        webkitRelativePath: path,
        text: async function () {
            return textContent;
        }
    };
}

function createManifest(overrides = {}) {
    return Object.assign({
        version: "1.1",
        width: 1280,
        height: 720,
        capture_interval_ms: 100,
        gpu_readback: "FRHIGPUTextureReadback; asynchronous",
        readback_buffer_count: 3,
        dropped_capture_count: 0,
        failed_capture_count: 0,
        depth_max_cm: 5000,
        capture_source: "SceneColor HDR in RGB; SceneDepth in A",
        color_tone_mapping: "ACES fitted; sRGB",
        depth_encoding: "RGB8; near=255; far=0",
        frame_count: 3,
        frames: [
            { index: 0, timestamp_ms: 10, color: "color/frame_000000.png", depth: "depth/frame_000000.png" },
            { index: 1, timestamp_ms: 110, color: "color/frame_000001.png", depth: "depth/frame_000001.png" },
            { index: 2, timestamp_ms: 210, color: "color/frame_000002.png", depth: "depth/frame_000002.png" }
        ]
    }, overrides);
}

function createFileMap(manifest) {
    const files = new Map();
    manifest.frames.forEach(function (frame) {
        files.set(core.normalizePath(frame.color), fakeFile(frame.color));
        files.set(core.normalizePath(frame.depth), fakeFile(frame.depth));
    });
    return files;
}

function createBinaryFile(options = {}) {
    const metadata = Object.assign({
        session_name: "binary-session",
        width: 1280,
        height: 720,
        capture_interval_ms: 100,
        depth_max_cm: 5000,
        color_encoding: "image/png; ACES fitted; sRGB",
        depth_encoding: "image/png; RGB8; near=255; far=0",
        gpu_readback: "FRHIGPUTextureReadback; asynchronous",
        readback_buffer_count: 3,
        dropped_capture_count: 0,
        failed_capture_count: 0,
        color_tone_mapping: "ACES fitted; sRGB"
    }, options.metadata || {});
    const frames = options.frames || [
        { index: 0, timestampMs: 5, colorLength: 9, depthLength: 10 },
        { index: 1, timestampMs: 105, colorLength: 11, depthLength: 12 }
    ];
    const metadataBytes = new TextEncoder().encode(JSON.stringify(metadata));
    const prefixSize = core.BINARY_PREFIX_SIZE;
    const indexOffset = Math.ceil((prefixSize + metadataBytes.length) / 8) * 8;
    const payloadOffset = indexOffset + frames.length * core.BINARY_INDEX_ENTRY_SIZE;
    const payloadLength = frames.reduce(function (sum, frame) {
        return sum + frame.colorLength + frame.depthLength;
    }, 0);
    const bytes = new Uint8Array(payloadOffset + payloadLength);
    const view = new DataView(bytes.buffer);
    const magic = [0x42, 0x4f, 0x41, 0x54, 0x43, 0x41, 0x50, 0x00];
    bytes.set(magic, 0);
    view.setUint16(8, options.version === undefined ? 1 : options.version, true);
    view.setUint16(10, prefixSize, true);
    view.setUint32(12, metadataBytes.length, true);
    view.setUint32(16, frames.length, true);
    view.setUint32(20, core.BINARY_INDEX_ENTRY_SIZE, true);
    view.setBigUint64(24, BigInt(payloadOffset), true);
    bytes.set(metadataBytes, prefixSize);

    let nextPayloadOffset = payloadOffset;
    frames.forEach(function (frame, position) {
        const entryOffset = indexOffset + position * core.BINARY_INDEX_ENTRY_SIZE;
        view.setUint32(entryOffset, frame.index, true);
        view.setUint32(entryOffset + 4, 0, true);
        view.setFloat64(entryOffset + 8, frame.timestampMs, true);
        view.setBigUint64(entryOffset + 16, BigInt(nextPayloadOffset), true);
        view.setBigUint64(entryOffset + 24, BigInt(frame.colorLength), true);
        bytes.fill(0x43, nextPayloadOffset, nextPayloadOffset + frame.colorLength);
        nextPayloadOffset += frame.colorLength;
        view.setBigUint64(entryOffset + 32, BigInt(nextPayloadOffset), true);
        view.setBigUint64(entryOffset + 40, BigInt(frame.depthLength), true);
        bytes.fill(0x44, nextPayloadOffset, nextPayloadOffset + frame.depthLength);
        nextPayloadOffset += frame.depthLength;
    });

    if (typeof options.mutate === "function") {
        options.mutate(bytes, view, { indexOffset, payloadOffset });
    }

    const file = new Blob([bytes], { type: "application/octet-stream" });
    Object.defineProperty(file, "name", { value: "capture.boatbin" });
    return file;
}

test("validates Manifest 1.1 and normalizes timestamps", function () {
    const manifest = createManifest();
    const files = createFileMap(manifest);
    const session = core.buildPlaybackSession(manifest, files, "sample");

    assert.equal(session.metadata.version, "1.1");
    assert.equal(session.metadata.depthMaxCm, 5000);
    assert.equal(session.sourceType, "folder");
    assert.equal(session.frames[0].colorBlob, files.get("color/frame_000000.png"));
    assert.deepEqual(session.frames.map((frame) => frame.playbackTimeMs), [0, 100, 200]);
    assert.equal(session.warnings.length, 0);
});

test("loads Binary Format 1 without an external Manifest", async function () {
    const session = await core.BinarySequenceLoader.load(createBinaryFile());

    assert.equal(session.sourceType, "binary");
    assert.equal(session.sessionName, "binary-session");
    assert.equal(session.metadata.formatLabel, "Binary 1");
    assert.equal(session.frames.length, 2);
    assert.deepEqual(session.frames.map((frame) => frame.playbackTimeMs), [0, 100]);
    assert.equal(session.frames[0].colorBlob.size, 9);
    assert.equal(session.frames[0].depthBlob.size, 10);
});

test("rejects invalid Binary magic, version, and payload offsets", async function () {
    await assert.rejects(
        core.BinarySequenceLoader.load(createBinaryFile({
            mutate: function (bytes) { bytes[0] = 0; }
        })),
        /Magic/
    );

    await assert.rejects(
        core.BinarySequenceLoader.load(createBinaryFile({ version: 2 })),
        /버전 1만/
    );

    await assert.rejects(
        core.BinarySequenceLoader.load(createBinaryFile({
            mutate: function (_bytes, view, offsets) {
                view.setBigUint64(offsets.indexOffset + 16, BigInt(offsets.payloadOffset + 1), true);
            }
        })),
        /연속적으로 배치/
    );
});

test("rejects truncated Binary files and malformed metadata", async function () {
    const validFile = createBinaryFile();
    await assert.rejects(
        core.BinarySequenceLoader.load(validFile.slice(0, 20)),
        /Prefix가 잘렸습니다/
    );

    await assert.rejects(
        core.BinarySequenceLoader.load(createBinaryFile({
            mutate: function (bytes) {
                bytes[core.BINARY_PREFIX_SIZE] = 0xff;
            }
        })),
        /Metadata JSON/
    );
});

test("creates deterministic random access orders without duplicates", function () {
    const first = core.createShuffledOrder(12, 0xB0A7);
    const second = core.createShuffledOrder(12, 0xB0A7);

    assert.deepEqual(first, second);
    assert.notDeepEqual(first, Array.from({ length: 12 }, (_value, index) => index));
    assert.deepEqual(first.slice().sort((left, right) => left - right), Array.from({ length: 12 }, (_value, index) => index));
});

test("rejects Manifest versions other than 1.1", function () {
    const manifest = createManifest({ version: "1.0" });
    assert.throws(() => core.validateManifest(manifest), /버전 1\.1만/);
});

test("rejects unsafe frame paths", function () {
    assert.equal(core.isSafeRelativePath("color\\frame_000001.png"), true);
    assert.equal(core.isSafeRelativePath("../secret.png"), false);
    assert.equal(core.isSafeRelativePath("C:\\secret.png"), false);
    assert.equal(core.isSafeRelativePath("https://example.com/image.png"), false);
});

test("sorts frame indices numerically", function () {
    const manifest = createManifest({
        frame_count: 3,
        frames: [
            { index: 10, timestamp_ms: 300, color: "color/10.png", depth: "depth/10.png" },
            { index: 2, timestamp_ms: 200, color: "color/2.png", depth: "depth/2.png" },
            { index: 0, timestamp_ms: 100, color: "color/0.png", depth: "depth/0.png" }
        ]
    });
    const session = core.buildPlaybackSession(manifest, createFileMap(manifest), "numeric-sort");
    assert.deepEqual(session.frames.map((frame) => frame.index), [0, 2, 10]);
});

test("rejects duplicate frame indices", function () {
    const manifest = createManifest({
        frame_count: 2,
        frames: [
            { index: 0, timestamp_ms: 0, color: "color/0.png", depth: "depth/0.png" },
            { index: 0, timestamp_ms: 100, color: "color/1.png", depth: "depth/1.png" }
        ]
    });
    assert.throws(() => core.validateManifest(manifest), /중복된 프레임 인덱스/);
});

test("keeps only complete color and depth pairs", function () {
    const manifest = createManifest();
    const files = createFileMap(manifest);
    files.delete("depth/frame_000001.png");

    const session = core.buildPlaybackSession(manifest, files, "missing-depth");
    assert.deepEqual(session.frames.map((frame) => frame.index), [0, 2]);
    assert.match(session.warnings.join("\n"), /Depth 파일이 없는 프레임: 1/);
});

test("warns about frame counts, unreferenced files, drops, and failures", function () {
    const manifest = createManifest({
        frame_count: 9,
        dropped_capture_count: 2,
        failed_capture_count: 1
    });
    const files = createFileMap(manifest);
    files.set("color/orphan.png", fakeFile("color/orphan.png"));

    const session = core.buildPlaybackSession(manifest, files, "warnings");
    const warnings = session.warnings.join("\n");
    assert.match(warnings, /frame_count/);
    assert.match(warnings, /참조하지 않는 PNG/);
    assert.match(warnings, /누락된 캡처가 2개/);
    assert.match(warnings, /실패한 캡처가 1개/);
});

test("rebuilds the timeline when timestamps are not increasing", function () {
    const manifest = createManifest({
        capture_interval_ms: 125,
        frames: [
            { index: 0, timestamp_ms: 200, color: "color/0.png", depth: "depth/0.png" },
            { index: 1, timestamp_ms: 150, color: "color/1.png", depth: "depth/1.png" },
            { index: 2, timestamp_ms: 300, color: "color/2.png", depth: "depth/2.png" }
        ]
    });
    const session = core.buildPlaybackSession(manifest, createFileMap(manifest), "fallback-time");
    assert.deepEqual(session.frames.map((frame) => frame.playbackTimeMs), [0, 125, 250]);
    assert.match(session.warnings.join("\n"), /재생 시간을 재구성/);
});

test("finds the latest frame for the playback time", function () {
    const frames = [
        { playbackTimeMs: 0 },
        { playbackTimeMs: 100 },
        { playbackTimeMs: 240 },
        { playbackTimeMs: 350 }
    ];

    assert.equal(core.findFrameIndexAtTime([], 100), -1);
    assert.equal(core.findFrameIndexAtTime(frames, -1), 0);
    assert.equal(core.findFrameIndexAtTime(frames, 239), 1);
    assert.equal(core.findFrameIndexAtTime(frames, 240), 2);
    assert.equal(core.findFrameIndexAtTime(frames, 999), 3);
});

test("loads a Manifest and frame pairs from a selected folder", async function () {
    const manifest = createManifest();
    const files = [fakeFile("20260809_205310_636/manifest.json", JSON.stringify(manifest))];
    manifest.frames.forEach(function (frame) {
        files.push(fakeFile("20260809_205310_636/" + frame.color));
        files.push(fakeFile("20260809_205310_636/" + frame.depth));
    });

    const session = await core.FolderSequenceLoader.load(files);
    assert.equal(session.sessionName, "20260809_205310_636");
    assert.equal(session.frames.length, 3);
});

test("rejects missing or multiple Manifests", async function () {
    await assert.rejects(
        core.FolderSequenceLoader.load([fakeFile("session/color/0.png")]),
        /manifest\.json을 찾지 못했습니다/
    );

    await assert.rejects(
        core.FolderSequenceLoader.load([
            fakeFile("session-a/manifest.json", "{}"),
            fakeFile("session-b/manifest.json", "{}")
        ]),
        /여러 캡처 세션/
    );
});
