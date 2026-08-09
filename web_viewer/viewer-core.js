(function (root, factory) {
    "use strict";

    const api = factory();

    // 브라우저에서는 전역 객체로, Node.js 테스트에서는 CommonJS 모듈로 같은 로직을 공유합니다.
    if (typeof module === "object" && module.exports) {
        module.exports = api;
    } else {
        root.BoatViewerCore = api;
    }
}(typeof globalThis !== "undefined" ? globalThis : this, function () {
    "use strict";

    const SUPPORTED_MANIFEST_VERSION = "1.1";
    const SUPPORTED_BINARY_VERSION = 1;
    const MANIFEST_FILE_NAME = "manifest.json";
    const BINARY_MAGIC = [0x42, 0x4f, 0x41, 0x54, 0x43, 0x41, 0x50, 0x00];
    const BINARY_PREFIX_SIZE = 32;
    const BINARY_INDEX_ENTRY_SIZE = 48;
    const BINARY_ALIGNMENT = 8;
    const PNG_MIME_TYPE = "image/png";

    /**
     * Windows와 브라우저에서 전달되는 경로 구분자를 하나로 맞춥니다.
     * 폴더 선택 API의 webkitRelativePath는 보통 슬래시를 사용하지만,
     * 테스트나 다른 운영체제에서 역슬래시가 들어와도 같은 파일로 찾기 위한 처리입니다.
     */
    function normalizePath(path) {
        return String(path || "").replace(/\\/g, "/");
    }

    /**
     * Manifest가 캡처 세션 밖의 파일을 참조하지 못하도록 상대 경로를 검사합니다.
     * 드라이브 경로, URL, 루트 경로, 상위 폴더 이동(..)은 모두 허용하지 않습니다.
     */
    function isSafeRelativePath(path) {
        const normalized = normalizePath(path);
        if (!normalized || normalized.startsWith("/") || /^[A-Za-z]:\//.test(normalized)) {
            return false;
        }

        if (/^[A-Za-z][A-Za-z0-9+.-]*:/.test(normalized)) {
            return false;
        }

        return normalized.split("/").every(function (segment) {
            return segment !== "" && segment !== "." && segment !== "..";
        });
    }

    function requireFinitePositive(value, fieldName) {
        if (typeof value !== "number" || !Number.isFinite(value) || value <= 0) {
            throw new Error(fieldName + " 값은 0보다 큰 숫자여야 합니다.");
        }
    }

    function requireNonNegativeInteger(value, fieldName) {
        if (!Number.isInteger(value) || value < 0) {
            throw new Error(fieldName + " 값은 0 이상의 정수여야 합니다.");
        }
    }

    function requireNonEmptyString(value, fieldName) {
        if (typeof value !== "string" || value.trim() === "") {
            throw new Error(fieldName + " 값이 필요합니다.");
        }
    }

    function alignTo(value, alignment) {
        return Math.ceil(value / alignment) * alignment;
    }

    function summarizeIndices(indices) {
        const preview = indices.slice(0, 5).join(", ");
        return indices.length > 5 ? preview + " 외 " + (indices.length - 5) + "개" : preview;
    }

    /**
     * 실제 캡처 Timestamp가 정상적으로 증가하면 첫 프레임을 0ms로 맞춰 사용합니다.
     * 중복되거나 역전된 값이 있으면 저장 간격 기준으로 전체 타임라인을 다시 만들어 재생 순서를 보호합니다.
     */
    function assignPlaybackTimes(frames, captureIntervalMs, warnings) {
        let timestampsIncrease = true;
        for (let index = 1; index < frames.length; index += 1) {
            if (frames[index].timestampMs <= frames[index - 1].timestampMs) {
                timestampsIncrease = false;
                break;
            }
        }

        if (timestampsIncrease) {
            const firstTimestamp = frames[0].timestampMs;
            frames.forEach(function (frame) {
                frame.playbackTimeMs = frame.timestampMs - firstTimestamp;
            });
            return;
        }

        warnings.push("프레임 시간이 증가하지 않아 capture_interval_ms 기준으로 재생 시간을 재구성했습니다.");
        frames.forEach(function (frame, position) {
            frame.playbackTimeMs = position * captureIntervalMs;
        });
    }

    function appendCaptureWarnings(metadata, warnings) {
        if (Number.isInteger(metadata.droppedCaptureCount) && metadata.droppedCaptureCount > 0) {
            warnings.push("GPU Readback 대기 중 누락된 캡처가 " + metadata.droppedCaptureCount + "개 있습니다.");
        }
        if (Number.isInteger(metadata.failedCaptureCount) && metadata.failedCaptureCount > 0) {
            warnings.push("저장에 실패한 캡처가 " + metadata.failedCaptureCount + "개 있습니다.");
        }
    }

    /**
     * 현재 BoatCaptureComponent가 기록하는 Manifest 1.1 형식만 검증합니다.
     * 잘못된 메타데이터를 재생 중에 뒤늦게 발견하지 않도록 로드 단계에서 중단합니다.
     */
    function validateManifest(manifest) {
        if (!manifest || typeof manifest !== "object" || Array.isArray(manifest)) {
            throw new Error("manifest.json의 최상위 값은 객체여야 합니다.");
        }

        if (manifest.version !== SUPPORTED_MANIFEST_VERSION) {
            throw new Error(
                "지원하지 않는 Manifest 버전입니다. 버전 "
                + SUPPORTED_MANIFEST_VERSION
                + "만 사용할 수 있습니다."
            );
        }

        requireFinitePositive(manifest.width, "width");
        requireFinitePositive(manifest.height, "height");
        requireFinitePositive(manifest.capture_interval_ms, "capture_interval_ms");
        requireFinitePositive(manifest.depth_max_cm, "depth_max_cm");
        requireNonNegativeInteger(manifest.frame_count, "frame_count");
        requireNonEmptyString(manifest.depth_encoding, "depth_encoding");

        if (!Array.isArray(manifest.frames)) {
            throw new Error("frames 값은 배열이어야 합니다.");
        }

        const usedIndices = new Set();
        manifest.frames.forEach(function (frame, position) {
            const prefix = "frames[" + position + "]";
            if (!frame || typeof frame !== "object" || Array.isArray(frame)) {
                throw new Error(prefix + " 값은 객체여야 합니다.");
            }

            requireNonNegativeInteger(frame.index, prefix + ".index");
            if (usedIndices.has(frame.index)) {
                throw new Error("중복된 프레임 인덱스가 있습니다: " + frame.index);
            }
            usedIndices.add(frame.index);

            if (typeof frame.timestamp_ms !== "number"
                || !Number.isFinite(frame.timestamp_ms)
                || frame.timestamp_ms < 0) {
                throw new Error(prefix + ".timestamp_ms 값은 0 이상의 숫자여야 합니다.");
            }

            ["color", "depth"].forEach(function (fieldName) {
                if (typeof frame[fieldName] !== "string" || !isSafeRelativePath(frame[fieldName])) {
                    throw new Error(prefix + "." + fieldName + " 값은 안전한 상대 경로여야 합니다.");
                }
            });
        });

        return manifest;
    }

    /**
     * 검증된 Manifest와 실제 선택 파일을 결합해 재생 전용 데이터로 바꿉니다.
     * 컬러와 Depth가 모두 있는 프레임만 남겨 두 화면이 서로 다른 시점을 보이지 않게 합니다.
     */
    function buildPlaybackSession(manifest, filesByRelativePath, sessionName) {
        validateManifest(manifest);

        const warnings = [];
        const referencedPaths = new Set();
        const missingColor = [];
        const missingDepth = [];
        const sortedFrames = manifest.frames.slice().sort(function (left, right) {
            return left.index - right.index;
        });

        if (manifest.frame_count !== manifest.frames.length) {
            warnings.push(
                "Manifest의 frame_count(" + manifest.frame_count
                + ")와 frames 배열 길이(" + manifest.frames.length + ")가 다릅니다."
            );
        }

        const playableFrames = [];
        sortedFrames.forEach(function (frame) {
            const colorPath = normalizePath(frame.color);
            const depthPath = normalizePath(frame.depth);
            const colorBlob = filesByRelativePath.get(colorPath);
            const depthBlob = filesByRelativePath.get(depthPath);

            referencedPaths.add(colorPath);
            referencedPaths.add(depthPath);

            if (!colorBlob) {
                missingColor.push(frame.index);
            }
            if (!depthBlob) {
                missingDepth.push(frame.index);
            }
            if (!colorBlob || !depthBlob) {
                return;
            }

            playableFrames.push({
                index: frame.index,
                timestampMs: frame.timestamp_ms,
                playbackTimeMs: 0,
                colorBlob: colorBlob,
                depthBlob: depthBlob,
                colorPath: colorPath,
                depthPath: depthPath
            });
        });

        if (missingColor.length > 0) {
            warnings.push("컬러 파일이 없는 프레임: " + summarizeIndices(missingColor));
        }
        if (missingDepth.length > 0) {
            warnings.push("Depth 파일이 없는 프레임: " + summarizeIndices(missingDepth));
        }
        if (playableFrames.length === 0) {
            throw new Error("컬러와 Depth 파일이 모두 존재하는 프레임이 없습니다.");
        }

        const unreferencedPng = [];
        filesByRelativePath.forEach(function (_file, relativePath) {
            if (/\.png$/i.test(relativePath) && !referencedPaths.has(relativePath)) {
                unreferencedPng.push(relativePath);
            }
        });
        if (unreferencedPng.length > 0) {
            warnings.push("Manifest에서 참조하지 않는 PNG 파일이 " + unreferencedPng.length + "개 있습니다.");
        }

        const metadata = {
            version: manifest.version,
            formatLabel: "Manifest " + manifest.version,
            sourceType: "folder",
            width: manifest.width,
            height: manifest.height,
            captureIntervalMs: manifest.capture_interval_ms,
            depthMaxCm: manifest.depth_max_cm,
            depthEncoding: manifest.depth_encoding,
            declaredFrameCount: manifest.frame_count,
            gpuReadback: manifest.gpu_readback || "",
            readbackBufferCount: manifest.readback_buffer_count,
            droppedCaptureCount: manifest.dropped_capture_count || 0,
            failedCaptureCount: manifest.failed_capture_count || 0,
            captureSource: manifest.capture_source || "",
            colorToneMapping: manifest.color_tone_mapping || "",
            archiveName: "",
            archiveSizeBytes: 0
        };
        appendCaptureWarnings(metadata, warnings);
        assignPlaybackTimes(playableFrames, metadata.captureIntervalMs, warnings);

        return {
            sessionName: sessionName || "Boat Capture",
            sourceType: "folder",
            metadata: metadata,
            frames: playableFrames,
            warnings: warnings,
            archiveFile: null
        };
    }

    /**
     * 폴더 선택 결과에서 세션 기준 상대 경로 맵을 만듭니다.
     * 사용자가 여러 세션이 든 상위 폴더를 선택하면 Manifest가 여러 개이므로 명확하게 거부합니다.
     */
    async function loadPlaybackSession(fileList) {
        const selectedFiles = Array.from(fileList || []);
        if (selectedFiles.length === 0) {
            throw new Error("선택한 폴더에 파일이 없습니다.");
        }

        const fileEntries = selectedFiles.map(function (file) {
            const fullPath = normalizePath(file.webkitRelativePath || file.name);
            return { file: file, fullPath: fullPath };
        });
        const manifests = fileEntries.filter(function (entry) {
            return entry.fullPath.split("/").pop() === MANIFEST_FILE_NAME;
        });

        if (manifests.length === 0) {
            throw new Error("선택한 폴더에서 manifest.json을 찾지 못했습니다.");
        }
        if (manifests.length > 1) {
            throw new Error("여러 캡처 세션이 선택되었습니다. 타임스탬프 세션 폴더 하나만 선택해 주세요.");
        }

        const manifestEntry = manifests[0];
        const manifestParts = manifestEntry.fullPath.split("/");
        manifestParts.pop();
        const manifestDirectory = manifestParts.join("/");
        const directoryPrefix = manifestDirectory ? manifestDirectory + "/" : "";
        const sessionName = manifestParts.length > 0 ? manifestParts[manifestParts.length - 1] : "Boat Capture";
        const filesByRelativePath = new Map();

        fileEntries.forEach(function (entry) {
            if (!entry.fullPath.startsWith(directoryPrefix)) {
                return;
            }
            const relativePath = entry.fullPath.slice(directoryPrefix.length);
            if (filesByRelativePath.has(relativePath)) {
                throw new Error("같은 상대 경로의 파일이 중복 선택되었습니다: " + relativePath);
            }
            filesByRelativePath.set(relativePath, entry.file);
        });

        let manifest;
        try {
            manifest = JSON.parse(await manifestEntry.file.text());
        } catch (error) {
            throw new Error("manifest.json을 JSON으로 읽지 못했습니다: " + error.message);
        }

        return buildPlaybackSession(manifest, filesByRelativePath, sessionName);
    }

    function validateBinaryMetadata(metadata) {
        if (!metadata || typeof metadata !== "object" || Array.isArray(metadata)) {
            throw new Error("Binary Metadata의 최상위 값은 객체여야 합니다.");
        }

        requireFinitePositive(metadata.width, "Binary Metadata width");
        requireFinitePositive(metadata.height, "Binary Metadata height");
        requireFinitePositive(metadata.capture_interval_ms, "Binary Metadata capture_interval_ms");
        requireFinitePositive(metadata.depth_max_cm, "Binary Metadata depth_max_cm");
        requireNonEmptyString(metadata.color_encoding, "Binary Metadata color_encoding");
        requireNonEmptyString(metadata.depth_encoding, "Binary Metadata depth_encoding");
        return metadata;
    }

    function readSafeUint64(view, byteOffset, fieldName) {
        const value = view.getBigUint64(byteOffset, true);
        if (value > BigInt(Number.MAX_SAFE_INTEGER)) {
            throw new Error(fieldName + " 값이 브라우저에서 안전하게 처리할 수 있는 범위를 초과했습니다.");
        }
        return Number(value);
    }

    async function readExactSlice(file, start, end, description) {
        const buffer = await file.slice(start, end).arrayBuffer();
        if (buffer.byteLength !== end - start) {
            throw new Error(description + " 영역이 잘렸습니다.");
        }
        return buffer;
    }

    /**
     * capture.boatbin은 처음에 Prefix·Metadata·Index만 읽습니다.
     * 실제 PNG Payload는 재생할 프레임이 필요할 때 Blob.slice()로 범위만 참조해 큰 파일 전체 복사를 피합니다.
     */
    async function loadBinaryPlaybackSession(file) {
        if (!file || typeof file.slice !== "function" || !Number.isFinite(file.size)) {
            throw new Error("올바른 바이너리 파일을 선택해 주세요.");
        }
        if (file.size < BINARY_PREFIX_SIZE) {
            throw new Error("바이너리 Prefix가 잘렸습니다.");
        }

        const prefixBuffer = await readExactSlice(file, 0, BINARY_PREFIX_SIZE, "바이너리 Prefix");
        const prefixBytes = new Uint8Array(prefixBuffer);
        const prefixView = new DataView(prefixBuffer);
        BINARY_MAGIC.forEach(function (expectedByte, index) {
            if (prefixBytes[index] !== expectedByte) {
                throw new Error("지원하지 않는 바이너리 Magic입니다.");
            }
        });

        const version = prefixView.getUint16(8, true);
        const prefixSize = prefixView.getUint16(10, true);
        const metadataLength = prefixView.getUint32(12, true);
        const frameCount = prefixView.getUint32(16, true);
        const indexEntrySize = prefixView.getUint32(20, true);
        const payloadOffset = readSafeUint64(prefixView, 24, "Payload Offset");

        if (version !== SUPPORTED_BINARY_VERSION) {
            throw new Error("지원하지 않는 Binary Format 버전입니다. 버전 1만 사용할 수 있습니다.");
        }
        if (prefixSize !== BINARY_PREFIX_SIZE || indexEntrySize !== BINARY_INDEX_ENTRY_SIZE) {
            throw new Error("Binary Format 1의 Prefix 또는 Index 크기와 일치하지 않습니다.");
        }
        if (metadataLength <= 0 || frameCount <= 0) {
            throw new Error("Binary Metadata와 프레임은 한 개 이상 필요합니다.");
        }

        const metadataEnd = BINARY_PREFIX_SIZE + metadataLength;
        const indexOffset = alignTo(metadataEnd, BINARY_ALIGNMENT);
        const indexEnd = indexOffset + frameCount * BINARY_INDEX_ENTRY_SIZE;
        if (payloadOffset !== indexEnd || payloadOffset > file.size) {
            throw new Error("바이너리 Index 또는 Payload Offset이 파일 범위를 벗어났습니다.");
        }

        const metadataBuffer = await readExactSlice(
            file,
            BINARY_PREFIX_SIZE,
            metadataEnd,
            "Binary Metadata"
        );
        let metadataJson;
        try {
            metadataJson = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(metadataBuffer));
        } catch (error) {
            throw new Error("Binary Metadata JSON을 읽지 못했습니다: " + error.message);
        }
        validateBinaryMetadata(metadataJson);

        const indexBuffer = await readExactSlice(file, indexOffset, indexEnd, "바이너리 Index");
        const indexView = new DataView(indexBuffer);
        const frames = [];
        const usedIndices = new Set();
        let expectedPayloadOffset = payloadOffset;

        for (let position = 0; position < frameCount; position += 1) {
            const entryOffset = position * BINARY_INDEX_ENTRY_SIZE;
            const frameIndex = indexView.getUint32(entryOffset, true);
            const timestampMs = indexView.getFloat64(entryOffset + 8, true);
            const colorOffset = readSafeUint64(indexView, entryOffset + 16, "Color Offset");
            const colorLength = readSafeUint64(indexView, entryOffset + 24, "Color Length");
            const depthOffset = readSafeUint64(indexView, entryOffset + 32, "Depth Offset");
            const depthLength = readSafeUint64(indexView, entryOffset + 40, "Depth Length");

            if (usedIndices.has(frameIndex)) {
                throw new Error("바이너리에 중복된 프레임 인덱스가 있습니다: " + frameIndex);
            }
            usedIndices.add(frameIndex);
            if (position > 0 && frameIndex <= frames[position - 1].index) {
                throw new Error("바이너리 프레임 인덱스가 오름차순이 아닙니다.");
            }
            if (!Number.isFinite(timestampMs) || timestampMs < 0) {
                throw new Error("바이너리 프레임 Timestamp가 올바르지 않습니다: " + frameIndex);
            }
            if (colorLength < 8 || depthLength < 8) {
                throw new Error("바이너리 PNG Payload가 너무 짧습니다: " + frameIndex);
            }
            if (colorOffset !== expectedPayloadOffset || depthOffset !== colorOffset + colorLength) {
                throw new Error("바이너리 Payload가 Index 순서와 연속적으로 배치되지 않았습니다: " + frameIndex);
            }
            if (depthOffset + depthLength > file.size) {
                throw new Error("바이너리 Payload가 파일 범위를 벗어났습니다: " + frameIndex);
            }

            frames.push({
                index: frameIndex,
                timestampMs: timestampMs,
                playbackTimeMs: 0,
                colorBlob: file.slice(colorOffset, colorOffset + colorLength, PNG_MIME_TYPE),
                depthBlob: file.slice(depthOffset, depthOffset + depthLength, PNG_MIME_TYPE),
                colorRange: { offset: colorOffset, length: colorLength },
                depthRange: { offset: depthOffset, length: depthLength }
            });
            expectedPayloadOffset = depthOffset + depthLength;
        }

        if (expectedPayloadOffset !== file.size) {
            throw new Error("마지막 Payload 뒤에 형식에 정의되지 않은 데이터가 있습니다.");
        }

        const warnings = [];
        const metadata = {
            version: String(version),
            formatLabel: "Binary " + version,
            sourceType: "binary",
            width: metadataJson.width,
            height: metadataJson.height,
            captureIntervalMs: metadataJson.capture_interval_ms,
            depthMaxCm: metadataJson.depth_max_cm,
            depthEncoding: metadataJson.depth_encoding,
            declaredFrameCount: frameCount,
            gpuReadback: metadataJson.gpu_readback || "",
            readbackBufferCount: metadataJson.readback_buffer_count,
            droppedCaptureCount: metadataJson.dropped_capture_count || 0,
            failedCaptureCount: metadataJson.failed_capture_count || 0,
            captureSource: metadataJson.capture_source || "",
            colorToneMapping: metadataJson.color_tone_mapping || "",
            archiveName: file.name || "capture.boatbin",
            archiveSizeBytes: file.size
        };
        appendCaptureWarnings(metadata, warnings);
        assignPlaybackTimes(frames, metadata.captureIntervalMs, warnings);

        return {
            sessionName: metadataJson.session_name || (file.name || "Boat Capture Binary"),
            sourceType: "binary",
            metadata: metadata,
            frames: frames,
            warnings: warnings,
            archiveFile: file
        };
    }

    /**
     * 현재 재생 시간보다 늦지 않은 가장 최신 프레임을 이진 탐색합니다.
     * 렌더링이 잠시 늦어져도 오래된 프레임을 연속 출력하지 않고 현재 시각으로 바로 따라잡습니다.
     */
    function findFrameIndexAtTime(frames, playbackTimeMs) {
        if (!Array.isArray(frames) || frames.length === 0) {
            return -1;
        }
        if (playbackTimeMs <= frames[0].playbackTimeMs) {
            return 0;
        }

        let low = 0;
        let high = frames.length - 1;
        while (low <= high) {
            const middle = Math.floor((low + high) / 2);
            if (frames[middle].playbackTimeMs <= playbackTimeMs) {
                low = middle + 1;
            } else {
                high = middle - 1;
            }
        }
        return Math.min(high, frames.length - 1);
    }

    /**
     * 같은 Seed에서는 항상 같은 랜덤 순서를 만들어 순차·랜덤 성능 측정을 다시 비교할 수 있게 합니다.
     */
    function createShuffledOrder(length, seed) {
        const order = Array.from({ length: length }, function (_value, index) { return index; });
        let state = seed >>> 0;
        for (let index = order.length - 1; index > 0; index -= 1) {
            state = (Math.imul(1664525, state) + 1013904223) >>> 0;
            const swapIndex = state % (index + 1);
            const temporary = order[index];
            order[index] = order[swapIndex];
            order[swapIndex] = temporary;
        }
        return order;
    }

    return {
        SUPPORTED_MANIFEST_VERSION: SUPPORTED_MANIFEST_VERSION,
        SUPPORTED_BINARY_VERSION: SUPPORTED_BINARY_VERSION,
        BINARY_PREFIX_SIZE: BINARY_PREFIX_SIZE,
        BINARY_INDEX_ENTRY_SIZE: BINARY_INDEX_ENTRY_SIZE,
        normalizePath: normalizePath,
        isSafeRelativePath: isSafeRelativePath,
        validateManifest: validateManifest,
        buildPlaybackSession: buildPlaybackSession,
        loadPlaybackSession: loadPlaybackSession,
        loadBinaryPlaybackSession: loadBinaryPlaybackSession,
        findFrameIndexAtTime: findFrameIndexAtTime,
        createShuffledOrder: createShuffledOrder,
        FolderSequenceLoader: {
            load: loadPlaybackSession
        },
        BinarySequenceLoader: {
            load: loadBinaryPlaybackSession
        }
    };
}));
