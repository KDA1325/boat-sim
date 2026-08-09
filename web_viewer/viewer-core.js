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
    const MANIFEST_FILE_NAME = "manifest.json";

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

        if (typeof manifest.depth_encoding !== "string" || manifest.depth_encoding.trim() === "") {
            throw new Error("depth_encoding 값이 필요합니다.");
        }

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

    function summarizeIndices(indices) {
        const preview = indices.slice(0, 5).join(", ");
        return indices.length > 5 ? preview + " 외 " + (indices.length - 5) + "개" : preview;
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
            const colorFile = filesByRelativePath.get(colorPath);
            const depthFile = filesByRelativePath.get(depthPath);

            referencedPaths.add(colorPath);
            referencedPaths.add(depthPath);

            if (!colorFile) {
                missingColor.push(frame.index);
            }
            if (!depthFile) {
                missingDepth.push(frame.index);
            }
            if (!colorFile || !depthFile) {
                return;
            }

            playableFrames.push({
                index: frame.index,
                timestampMs: frame.timestamp_ms,
                playbackTimeMs: 0,
                colorFile: colorFile,
                depthFile: depthFile,
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

        // 캡처 누락과 저장 실패는 재생을 막지는 않지만 결과의 완전성을 판단할 수 있도록 알립니다.
        if (Number.isInteger(manifest.dropped_capture_count) && manifest.dropped_capture_count > 0) {
            warnings.push("GPU Readback 대기 중 누락된 캡처가 " + manifest.dropped_capture_count + "개 있습니다.");
        }
        if (Number.isInteger(manifest.failed_capture_count) && manifest.failed_capture_count > 0) {
            warnings.push("저장에 실패한 캡처가 " + manifest.failed_capture_count + "개 있습니다.");
        }

        let timestampsIncrease = true;
        for (let index = 1; index < playableFrames.length; index += 1) {
            if (playableFrames[index].timestampMs <= playableFrames[index - 1].timestampMs) {
                timestampsIncrease = false;
                break;
            }
        }

        if (timestampsIncrease) {
            const firstTimestamp = playableFrames[0].timestampMs;
            playableFrames.forEach(function (frame) {
                frame.playbackTimeMs = frame.timestampMs - firstTimestamp;
            });
        } else {
            warnings.push("프레임 시간이 증가하지 않아 capture_interval_ms 기준으로 재생 시간을 재구성했습니다.");
            playableFrames.forEach(function (frame, position) {
                frame.playbackTimeMs = position * manifest.capture_interval_ms;
            });
        }

        return {
            sessionName: sessionName || "Boat Capture",
            metadata: {
                version: manifest.version,
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
                colorToneMapping: manifest.color_tone_mapping || ""
            },
            frames: playableFrames,
            warnings: warnings
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

    return {
        SUPPORTED_MANIFEST_VERSION: SUPPORTED_MANIFEST_VERSION,
        normalizePath: normalizePath,
        isSafeRelativePath: isSafeRelativePath,
        validateManifest: validateManifest,
        buildPlaybackSession: buildPlaybackSession,
        loadPlaybackSession: loadPlaybackSession,
        findFrameIndexAtTime: findFrameIndexAtTime,
        FolderSequenceLoader: {
            load: loadPlaybackSession
        }
    };
}));
