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

test("validates Manifest 1.1 and normalizes timestamps", function () {
    const manifest = createManifest();
    const session = core.buildPlaybackSession(manifest, createFileMap(manifest), "sample");

    assert.equal(session.metadata.version, "1.1");
    assert.equal(session.metadata.depthMaxCm, 5000);
    assert.deepEqual(session.frames.map((frame) => frame.playbackTimeMs), [0, 100, 200]);
    assert.equal(session.warnings.length, 0);
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
