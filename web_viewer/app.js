(function () {
    "use strict";

    const core = window.BoatViewerCore;
    const MAX_CACHED_FRAME_PAIRS = 6;
    const BENCHMARK_WARMUP_IMAGE_COUNT = 10;
    const BENCHMARK_ROUNDS_PER_MODE = 2;
    const BENCHMARK_RANDOM_SEED = 0xB0A7;
    const BENCHMARK_YIELD_INTERVAL = 16;

    const elements = {
        folderInput: document.getElementById("folder-input"),
        binaryInput: document.getElementById("binary-input"),
        emptyState: document.getElementById("empty-state"),
        statusMessage: document.getElementById("status-message"),
        viewer: document.getElementById("viewer"),
        sessionName: document.getElementById("session-name"),
        warningPanel: document.getElementById("warning-panel"),
        warningList: document.getElementById("warning-list"),
        colorImage: document.getElementById("color-image"),
        depthImage: document.getElementById("depth-image"),
        colorDescription: document.getElementById("color-description"),
        depthDescription: document.getElementById("depth-description"),
        timeline: document.getElementById("timeline"),
        currentTime: document.getElementById("current-time"),
        totalTime: document.getElementById("total-time"),
        firstButton: document.getElementById("first-button"),
        previousButton: document.getElementById("previous-button"),
        playButton: document.getElementById("play-button"),
        nextButton: document.getElementById("next-button"),
        lastButton: document.getElementById("last-button"),
        framePosition: document.getElementById("frame-position"),
        frameIndex: document.getElementById("frame-index"),
        frameTimestamp: document.getElementById("frame-timestamp"),
        metaVersion: document.getElementById("meta-version"),
        metaResolution: document.getElementById("meta-resolution"),
        metaFrameCount: document.getElementById("meta-frame-count"),
        metaInterval: document.getElementById("meta-interval"),
        metaDuration: document.getElementById("meta-duration"),
        metaReadback: document.getElementById("meta-readback"),
        metaDropped: document.getElementById("meta-dropped"),
        metaFailed: document.getElementById("meta-failed"),
        benchmarkPanel: document.getElementById("benchmark-panel"),
        benchmarkButton: document.getElementById("benchmark-button"),
        benchmarkDownloadButton: document.getElementById("benchmark-download-button"),
        benchmarkProgress: document.getElementById("benchmark-progress"),
        benchmarkStatus: document.getElementById("benchmark-status"),
        benchmarkResults: document.getElementById("benchmark-results"),
        benchmarkSequentialTotal: document.getElementById("benchmark-sequential-total"),
        benchmarkSequentialRead: document.getElementById("benchmark-sequential-read"),
        benchmarkSequentialDecode: document.getElementById("benchmark-sequential-decode"),
        benchmarkRandomTotal: document.getElementById("benchmark-random-total"),
        benchmarkRandomRead: document.getElementById("benchmark-random-read"),
        benchmarkRandomDecode: document.getElementById("benchmark-random-decode"),
        benchmarkSampleCount: document.getElementById("benchmark-sample-count"),
        benchmarkArchiveSize: document.getElementById("benchmark-archive-size")
    };

    const state = {
        session: null,
        currentPosition: 0,
        displayedPosition: -1,
        requestedPosition: -1,
        isPlaying: false,
        animationFrameId: 0,
        playbackAnchorNow: 0,
        playbackAnchorMs: 0,
        renderRequestToken: 0,
        cacheUseCounter: 0,
        framePairCache: new Map(),
        isBenchmarking: false,
        benchmarkResult: null
    };

    function formatMilliseconds(milliseconds) {
        const safeValue = Math.max(0, Number.isFinite(milliseconds) ? milliseconds : 0);
        const minutes = Math.floor(safeValue / 60000);
        const seconds = Math.floor((safeValue % 60000) / 1000);
        const millis = Math.floor(safeValue % 1000);
        return String(minutes).padStart(2, "0")
            + ":" + String(seconds).padStart(2, "0")
            + "." + String(millis).padStart(3, "0");
    }

    function formatBytes(bytes) {
        const safeBytes = Math.max(0, Number.isFinite(bytes) ? bytes : 0);
        const units = ["B", "KB", "MB", "GB", "TB"];
        let value = safeBytes;
        let unitIndex = 0;
        while (value >= 1024 && unitIndex < units.length - 1) {
            value /= 1024;
            unitIndex += 1;
        }
        return value.toFixed(unitIndex === 0 ? 0 : 2) + " " + units[unitIndex];
    }

    function setStatus(message, kind) {
        elements.statusMessage.textContent = message;
        elements.statusMessage.dataset.kind = kind || "info";
        elements.statusMessage.hidden = !message;
    }

    function setControlsEnabled(enabled) {
        [
            elements.timeline,
            elements.firstButton,
            elements.previousButton,
            elements.playButton,
            elements.nextButton,
            elements.lastButton
        ].forEach(function (element) {
            element.disabled = !enabled;
        });
    }

    function updatePlayButton() {
        elements.playButton.textContent = state.isPlaying ? "Ⅱ" : "▶";
        elements.playButton.setAttribute("aria-label", state.isPlaying ? "일시 정지" : "재생");
    }

    function stopPlayback(message) {
        state.isPlaying = false;
        if (state.animationFrameId) {
            cancelAnimationFrame(state.animationFrameId);
            state.animationFrameId = 0;
        }
        updatePlayButton();
        if (message) {
            setStatus(message, "info");
        }
    }

    /**
     * createObjectURL()로 만든 주소는 브라우저가 원본 File을 계속 참조하게 합니다.
     * 캐시에서 빠진 프레임은 두 URL을 반드시 해제해 장시간 세션에서도 메모리가 쌓이지 않게 합니다.
     */
    function releaseCacheEntry(entry) {
        if (!entry || entry.released) {
            return;
        }
        entry.released = true;
        URL.revokeObjectURL(entry.colorUrl);
        URL.revokeObjectURL(entry.depthUrl);
    }

    function clearFramePairCache() {
        state.framePairCache.forEach(releaseCacheEntry);
        state.framePairCache.clear();
        elements.colorImage.removeAttribute("src");
        elements.depthImage.removeAttribute("src");
    }

    /**
     * 한 프레임의 Color와 Depth를 함께 디코딩합니다.
     * 어느 한쪽만 먼저 표시하면 서로 다른 시점처럼 보일 수 있으므로 Promise.all()이 끝날 때까지 기다립니다.
     */
    function getDecodedFramePair(position) {
        const existing = state.framePairCache.get(position);
        if (existing) {
            existing.lastUsed = ++state.cacheUseCounter;
            return existing.decodePromise;
        }

        const frame = state.session.frames[position];
        const colorUrl = URL.createObjectURL(frame.colorBlob);
        const depthUrl = URL.createObjectURL(frame.depthBlob);
        const colorImage = new Image();
        const depthImage = new Image();
        colorImage.decoding = "async";
        depthImage.decoding = "async";
        colorImage.src = colorUrl;
        depthImage.src = depthUrl;

        const entry = {
            position: position,
            colorUrl: colorUrl,
            depthUrl: depthUrl,
            colorImage: colorImage,
            depthImage: depthImage,
            released: false,
            lastUsed: ++state.cacheUseCounter,
            decodePromise: null
        };

        entry.decodePromise = Promise.all([colorImage.decode(), depthImage.decode()])
            .then(function () {
                if (entry.released) {
                    throw new Error("이미 해제된 프레임입니다.");
                }
                return entry;
            })
            .catch(function (error) {
                if (state.framePairCache.get(position) === entry) {
                    state.framePairCache.delete(position);
                }
                releaseCacheEntry(entry);
                throw error;
            });

        state.framePairCache.set(position, entry);
        return entry.decodePromise;
    }

    /**
     * 현재 위치에 필요한 프레임은 보호하고, 가장 오래 사용하지 않은 프레임부터 제거합니다.
     * 최대 6쌍만 유지해 1280×720 PNG를 길게 재생해도 디코딩 메모리 사용량이 계속 늘지 않게 합니다.
     */
    function trimFramePairCache(protectedPositions) {
        while (state.framePairCache.size > MAX_CACHED_FRAME_PAIRS) {
            let evictionCandidate = null;
            state.framePairCache.forEach(function (entry, position) {
                if (protectedPositions.has(position)) {
                    return;
                }
                if (!evictionCandidate || entry.lastUsed < evictionCandidate.lastUsed) {
                    evictionCandidate = entry;
                }
            });

            if (!evictionCandidate) {
                break;
            }
            state.framePairCache.delete(evictionCandidate.position);
            releaseCacheEntry(evictionCandidate);
        }
    }

    function prefetchNearbyFrames(position) {
        const requested = [];
        const candidates = [position - 1, position, position + 1, position + 2, position + 3, position + 4];
        candidates.forEach(function (candidate) {
            if (candidate < 0 || candidate >= state.session.frames.length) {
                return;
            }
            requested.push(candidate);
            // 미리 읽기 실패는 실제 표시 시 상세 오류를 내므로 여기서는 처리 완료된 Promise로 바꿉니다.
            getDecodedFramePair(candidate).catch(function () {});
        });
        trimFramePairCache(new Set(requested));
    }

    function updateFrameReadout(position) {
        const frame = state.session.frames[position];
        const totalDuration = state.session.frames[state.session.frames.length - 1].playbackTimeMs;

        elements.timeline.value = String(position);
        elements.currentTime.textContent = formatMilliseconds(frame.playbackTimeMs);
        elements.totalTime.textContent = formatMilliseconds(totalDuration);
        elements.framePosition.textContent = (position + 1) + " / " + state.session.frames.length;
        elements.frameIndex.textContent = String(frame.index);
        elements.frameTimestamp.textContent = frame.timestampMs.toFixed(3) + " ms";
    }

    /**
     * renderRequestToken은 이전 비동기 요청의 완료 결과를 무효화합니다.
     * 사용자가 빠르게 탐색하거나 새 세션을 열었을 때 늦게 끝난 옛 이미지가 화면을 덮는 것을 막습니다.
     */
    async function showFrame(position) {
        if (!state.session) {
            return false;
        }

        const clampedPosition = Math.max(0, Math.min(position, state.session.frames.length - 1));
        const requestToken = ++state.renderRequestToken;
        state.requestedPosition = clampedPosition;

        try {
            const pair = await getDecodedFramePair(clampedPosition);
            if (requestToken !== state.renderRequestToken || !state.session || pair.released) {
                return false;
            }

            // 두 이미지가 모두 디코딩된 다음 같은 렌더 단계에서 src를 바꿔 프레임 쌍을 맞춥니다.
            elements.colorImage.src = pair.colorUrl;
            elements.depthImage.src = pair.depthUrl;
            state.currentPosition = clampedPosition;
            state.displayedPosition = clampedPosition;
            state.requestedPosition = -1;
            updateFrameReadout(clampedPosition);
            prefetchNearbyFrames(clampedPosition);
            return true;
        } catch (error) {
            if (requestToken !== state.renderRequestToken) {
                return false;
            }
            state.requestedPosition = -1;
            stopPlayback();
            const frame = state.session.frames[clampedPosition];
            setStatus(
                "프레임 " + frame.index + "의 Color 또는 Depth 이미지를 디코딩하지 못했습니다: " + error.message,
                "error"
            );
            return false;
        }
    }

    function playbackTick(now) {
        if (!state.isPlaying || !state.session) {
            return;
        }

        const frames = state.session.frames;
        const lastPosition = frames.length - 1;
        const targetTime = state.playbackAnchorMs + (now - state.playbackAnchorNow);
        const targetPosition = core.findFrameIndexAtTime(frames, targetTime);

        // 렌더링이 늦으면 중간 프레임을 몰아서 재생하지 않고 현재 시간에 맞는 위치를 즉시 요청합니다.
        if (targetPosition !== state.currentPosition && targetPosition !== state.requestedPosition) {
            showFrame(targetPosition);
        }

        if (targetTime >= frames[lastPosition].playbackTimeMs) {
            showFrame(lastPosition);
            stopPlayback("마지막 프레임까지 재생했습니다.");
            return;
        }

        state.animationFrameId = requestAnimationFrame(playbackTick);
    }

    async function startPlayback() {
        if (!state.session || state.session.frames.length <= 1) {
            return;
        }

        const lastPosition = state.session.frames.length - 1;
        if (state.currentPosition >= lastPosition) {
            const firstFrameReady = await showFrame(0);
            if (!firstFrameReady) {
                return;
            }
        }

        setStatus("", "info");
        state.isPlaying = true;
        state.playbackAnchorNow = performance.now();
        state.playbackAnchorMs = state.session.frames[state.currentPosition].playbackTimeMs;
        updatePlayButton();
        state.animationFrameId = requestAnimationFrame(playbackTick);
    }

    function togglePlayback() {
        if (state.isPlaying) {
            stopPlayback();
        } else {
            startPlayback();
        }
    }

    async function navigateTo(position) {
        stopPlayback();
        setStatus("", "info");
        await showFrame(position);
    }

    function renderWarnings(warnings) {
        elements.warningList.replaceChildren();
        warnings.forEach(function (warning) {
            const item = document.createElement("li");
            item.textContent = warning;
            elements.warningList.appendChild(item);
        });
        elements.warningPanel.hidden = warnings.length === 0;
    }

    function renderSessionMetadata(session) {
        const metadata = session.metadata;
        const duration = session.frames[session.frames.length - 1].playbackTimeMs;
        const targetHz = 1000 / metadata.captureIntervalMs;

        document.documentElement.style.setProperty("--capture-aspect", metadata.width + " / " + metadata.height);
        elements.sessionName.textContent = session.sessionName;
        elements.metaVersion.textContent = metadata.formatLabel || ("Manifest " + metadata.version);
        elements.metaResolution.textContent = metadata.width + " × " + metadata.height;
        elements.metaFrameCount.textContent = session.frames.length + " / " + metadata.declaredFrameCount;
        elements.metaInterval.textContent = metadata.captureIntervalMs.toFixed(2) + " ms · " + targetHz.toFixed(2) + " Hz";
        elements.metaDuration.textContent = formatMilliseconds(duration);
        elements.metaReadback.textContent = metadata.readbackBufferCount
            ? "비동기 · " + metadata.readbackBufferCount + " buffers"
            : (metadata.gpuReadback || "-");
        elements.metaDropped.textContent = String(metadata.droppedCaptureCount);
        elements.metaFailed.textContent = String(metadata.failedCaptureCount);
        elements.colorDescription.textContent = metadata.colorToneMapping || "Manifest에 톤 매핑 정보가 없습니다.";
        elements.depthDescription.textContent = metadata.depthEncoding
            + " · Near 255 · Far 0 · Max " + metadata.depthMaxCm.toFixed(0) + " cm";
        elements.timeline.max = String(Math.max(0, session.frames.length - 1));
        elements.timeline.value = "0";
        renderWarnings(session.warnings);
    }

    function resetBenchmarkView() {
        state.benchmarkResult = null;
        elements.benchmarkProgress.hidden = true;
        elements.benchmarkProgress.value = 0;
        elements.benchmarkStatus.textContent = "측정 전";
        elements.benchmarkResults.hidden = true;
        elements.benchmarkDownloadButton.disabled = true;
    }

    /**
     * 폴더와 바이너리 로더는 입력 방식만 다르고 이후 재생 상태 초기화 과정은 같습니다.
     * 새 세션을 열 때 이전 Object URL과 성능 결과를 모두 정리해 서로 다른 세션 정보가 섞이지 않게 합니다.
     */
    async function loadSelectedSession(sessionLoader) {
        stopPlayback();
        state.renderRequestToken += 1;
        clearFramePairCache();
        state.session = null;
        state.currentPosition = 0;
        state.displayedPosition = -1;
        state.requestedPosition = -1;
        setControlsEnabled(false);
        elements.benchmarkPanel.hidden = true;
        resetBenchmarkView();
        setStatus("캡처 세션을 확인하고 첫 프레임을 준비하고 있습니다.", "info");

        try {
            const session = await sessionLoader();
            state.session = session;
            renderSessionMetadata(session);
            elements.emptyState.hidden = true;
            elements.viewer.hidden = false;
            elements.benchmarkPanel.hidden = session.sourceType !== "binary";
            setControlsEnabled(true);
            const firstFrameReady = await showFrame(0);
            if (!firstFrameReady) {
                return;
            }

            if (session.frames.length > 1) {
                startPlayback();
            } else {
                setStatus("유효한 프레임이 한 개이므로 정지 화면으로 표시합니다.", "info");
            }
        } catch (error) {
            state.session = null;
            elements.viewer.hidden = true;
            elements.emptyState.hidden = false;
            elements.benchmarkPanel.hidden = true;
            clearFramePairCache();
            setControlsEnabled(false);
            setStatus(error.message, "error");
        }
    }

    function loadSelectedFolder(fileList) {
        return loadSelectedSession(function () {
            return core.FolderSequenceLoader.load(fileList);
        });
    }

    function loadSelectedBinary(file) {
        return loadSelectedSession(function () {
            return core.BinarySequenceLoader.load(file);
        });
    }

    function createBinaryImageAccesses(session) {
        const accesses = [];
        session.frames.forEach(function (frame) {
            accesses.push({
                frameIndex: frame.index,
                imageType: "color",
                offset: frame.colorRange.offset,
                length: frame.colorRange.length
            });
            accesses.push({
                frameIndex: frame.index,
                imageType: "depth",
                offset: frame.depthRange.offset,
                length: frame.depthRange.length
            });
        });
        return accesses;
    }

    function nextAnimationFrame() {
        return new Promise(function (resolve) {
            requestAnimationFrame(resolve);
        });
    }

    /**
     * 한 이미지의 File Slice를 실제 ArrayBuffer로 읽은 뒤 PNG를 디코딩합니다.
     * 화면 DOM 교체는 제외하고, 과제에서 비교할 수 있도록 Read와 Decode 시간을 따로 기록합니다.
     */
    async function measureBinaryImageAccess(archiveFile, access) {
        const readStartedAt = performance.now();
        const encodedBytes = await archiveFile
            .slice(access.offset, access.offset + access.length)
            .arrayBuffer();
        const readFinishedAt = performance.now();

        const decodeStartedAt = performance.now();
        const bitmap = await createImageBitmap(new Blob([encodedBytes], { type: "image/png" }));
        bitmap.close();
        const decodeFinishedAt = performance.now();

        return {
            readMs: readFinishedAt - readStartedAt,
            decodeMs: decodeFinishedAt - decodeStartedAt,
            totalMs: decodeFinishedAt - readStartedAt
        };
    }

    function createMetricAccumulator() {
        return { samples: 0, readMs: 0, decodeMs: 0, totalMs: 0 };
    }

    function addMeasurement(accumulator, measurement) {
        accumulator.samples += 1;
        accumulator.readMs += measurement.readMs;
        accumulator.decodeMs += measurement.decodeMs;
        accumulator.totalMs += measurement.totalMs;
    }

    function averageMetric(accumulator) {
        const divisor = Math.max(1, accumulator.samples);
        return {
            samples: accumulator.samples,
            averageReadMs: accumulator.readMs / divisor,
            averageDecodeMs: accumulator.decodeMs / divisor,
            averageTotalMs: accumulator.totalMs / divisor
        };
    }

    async function measureAccessOrder(accesses, order, accumulator, progressState, label) {
        elements.benchmarkStatus.textContent = label;
        for (let position = 0; position < order.length; position += 1) {
            const measurement = await measureBinaryImageAccess(
                state.session.archiveFile,
                accesses[order[position]]
            );
            addMeasurement(accumulator, measurement);
            progressState.completed += 1;
            elements.benchmarkProgress.value = progressState.completed;

            // 긴 세션에서도 버튼과 진행률이 갱신되도록 측정 구간 밖에서 브라우저에 제어권을 반환합니다.
            if ((position + 1) % BENCHMARK_YIELD_INTERVAL === 0) {
                await nextAnimationFrame();
            }
        }
    }

    function renderBenchmarkResult(result) {
        const sequential = result.sequential;
        const random = result.random;
        elements.benchmarkSequentialTotal.textContent = sequential.averageTotalMs.toFixed(3);
        elements.benchmarkSequentialRead.textContent = sequential.averageReadMs.toFixed(3) + " ms";
        elements.benchmarkSequentialDecode.textContent = sequential.averageDecodeMs.toFixed(3) + " ms";
        elements.benchmarkRandomTotal.textContent = random.averageTotalMs.toFixed(3);
        elements.benchmarkRandomRead.textContent = random.averageReadMs.toFixed(3) + " ms";
        elements.benchmarkRandomDecode.textContent = random.averageDecodeMs.toFixed(3) + " ms";
        elements.benchmarkSampleCount.textContent = sequential.samples + " / mode";
        elements.benchmarkArchiveSize.textContent = formatBytes(result.archive.sizeBytes);
        elements.benchmarkResults.hidden = false;
        elements.benchmarkDownloadButton.disabled = false;
    }

    /**
     * 순차와 랜덤 순서를 앞뒤로 교차해 어느 한쪽만 두 번째 OS 캐시 효과를 받는 편향을 줄입니다.
     * 재생 캐시는 먼저 비우며, 워밍업 10장은 평균 계산에서 제외합니다.
     */
    async function runBinaryBenchmark() {
        if (!state.session || state.session.sourceType !== "binary" || state.isBenchmarking) {
            return;
        }
        if (typeof createImageBitmap !== "function") {
            elements.benchmarkStatus.textContent = "이 브라우저는 createImageBitmap()을 지원하지 않습니다.";
            return;
        }

        stopPlayback();
        const restorePosition = state.currentPosition;
        state.renderRequestToken += 1;
        clearFramePairCache();
        state.isBenchmarking = true;
        state.benchmarkResult = null;
        setControlsEnabled(false);
        elements.folderInput.disabled = true;
        elements.binaryInput.disabled = true;
        elements.benchmarkButton.disabled = true;
        elements.benchmarkDownloadButton.disabled = true;
        elements.benchmarkResults.hidden = true;
        elements.benchmarkProgress.hidden = false;

        const accesses = createBinaryImageAccesses(state.session);
        const sequentialOrder = Array.from({ length: accesses.length }, function (_value, index) { return index; });
        const randomOrder = core.createShuffledOrder(accesses.length, BENCHMARK_RANDOM_SEED);
        const warmupCount = Math.min(BENCHMARK_WARMUP_IMAGE_COUNT, accesses.length);
        const progressState = { completed: 0 };
        elements.benchmarkProgress.max = warmupCount + accesses.length * BENCHMARK_ROUNDS_PER_MODE * 2;
        elements.benchmarkProgress.value = 0;

        try {
            elements.benchmarkStatus.textContent = "워밍업 이미지를 읽고 있습니다.";
            for (let index = 0; index < warmupCount; index += 1) {
                await measureBinaryImageAccess(state.session.archiveFile, accesses[sequentialOrder[index]]);
                progressState.completed += 1;
                elements.benchmarkProgress.value = progressState.completed;
            }

            const sequentialAccumulator = createMetricAccumulator();
            const randomAccumulator = createMetricAccumulator();
            await measureAccessOrder(accesses, sequentialOrder, sequentialAccumulator, progressState, "1/4 순차 접근 측정 중");
            await measureAccessOrder(accesses, randomOrder, randomAccumulator, progressState, "2/4 랜덤 접근 측정 중");
            await measureAccessOrder(accesses, randomOrder, randomAccumulator, progressState, "3/4 랜덤 접근 측정 중");
            await measureAccessOrder(accesses, sequentialOrder, sequentialAccumulator, progressState, "4/4 순차 접근 측정 중");

            state.benchmarkResult = {
                schemaVersion: "1.0",
                measuredAt: new Date().toISOString(),
                userAgent: navigator.userAgent,
                metricDefinition: "File.slice().arrayBuffer() read + createImageBitmap() PNG decode; DOM paint excluded",
                warmupImageCount: warmupCount,
                roundsPerMode: BENCHMARK_ROUNDS_PER_MODE,
                randomSeed: BENCHMARK_RANDOM_SEED,
                archive: {
                    name: state.session.metadata.archiveName,
                    sizeBytes: state.session.metadata.archiveSizeBytes,
                    frameCount: state.session.frames.length,
                    imageCount: accesses.length
                },
                sequential: averageMetric(sequentialAccumulator),
                random: averageMetric(randomAccumulator)
            };
            renderBenchmarkResult(state.benchmarkResult);
            elements.benchmarkStatus.textContent = "측정 완료 · 보고서에는 Total 평균 ms/image 값을 사용합니다.";
        } catch (error) {
            elements.benchmarkStatus.textContent = "성능 측정 실패: " + error.message;
        } finally {
            state.isBenchmarking = false;
            elements.folderInput.disabled = false;
            elements.binaryInput.disabled = false;
            elements.benchmarkButton.disabled = false;
            setControlsEnabled(true);
            await showFrame(restorePosition);
        }
    }

    function downloadBenchmarkResult() {
        if (!state.benchmarkResult) {
            return;
        }

        const resultBlob = new Blob(
            [JSON.stringify(state.benchmarkResult, null, 2)],
            { type: "application/json;charset=utf-8" }
        );
        const resultUrl = URL.createObjectURL(resultBlob);
        const downloadLink = document.createElement("a");
        const timestamp = state.benchmarkResult.measuredAt.replace(/[:.]/g, "-");
        downloadLink.href = resultUrl;
        downloadLink.download = "boat_capture_benchmark_" + timestamp + ".json";
        downloadLink.click();
        setTimeout(function () {
            URL.revokeObjectURL(resultUrl);
        }, 0);
    }

    elements.folderInput.addEventListener("click", function () {
        // 같은 폴더를 다시 선택해도 change 이벤트가 발생하도록 기존 선택값을 비웁니다.
        elements.folderInput.value = "";
    });

    elements.folderInput.addEventListener("change", function (event) {
        if (event.target.files && event.target.files.length > 0) {
            loadSelectedFolder(event.target.files);
        }
    });

    elements.binaryInput.addEventListener("click", function () {
        // 같은 바이너리를 다시 선택해도 change 이벤트가 발생하도록 기존 선택값을 비웁니다.
        elements.binaryInput.value = "";
    });

    elements.binaryInput.addEventListener("change", function (event) {
        if (event.target.files && event.target.files.length === 1) {
            loadSelectedBinary(event.target.files[0]);
        }
    });

    elements.playButton.addEventListener("click", togglePlayback);
    elements.benchmarkButton.addEventListener("click", runBinaryBenchmark);
    elements.benchmarkDownloadButton.addEventListener("click", downloadBenchmarkResult);
    elements.firstButton.addEventListener("click", function () { navigateTo(0); });
    elements.previousButton.addEventListener("click", function () { navigateTo(state.currentPosition - 1); });
    elements.nextButton.addEventListener("click", function () { navigateTo(state.currentPosition + 1); });
    elements.lastButton.addEventListener("click", function () {
        if (state.session) {
            navigateTo(state.session.frames.length - 1);
        }
    });
    elements.timeline.addEventListener("input", function (event) {
        navigateTo(Number(event.target.value));
    });

    document.addEventListener("keydown", function (event) {
        if (!state.session || event.ctrlKey || event.metaKey || event.altKey) {
            return;
        }

        const tagName = event.target && event.target.tagName;
        if (tagName === "INPUT" || tagName === "BUTTON" || tagName === "TEXTAREA" || tagName === "SELECT") {
            return;
        }

        const keyActions = {
            " ": togglePlayback,
            ArrowLeft: function () { navigateTo(state.currentPosition - 1); },
            ArrowRight: function () { navigateTo(state.currentPosition + 1); },
            Home: function () { navigateTo(0); },
            End: function () { navigateTo(state.session.frames.length - 1); }
        };
        const action = keyActions[event.key];
        if (action) {
            event.preventDefault();
            action();
        }
    });

    document.addEventListener("visibilitychange", function () {
        if (document.hidden && state.isPlaying) {
            stopPlayback("브라우저가 백그라운드로 이동해 재생을 일시 정지했습니다.");
        }
    });

    window.addEventListener("beforeunload", function () {
        stopPlayback();
        clearFramePairCache();
    });
}());
