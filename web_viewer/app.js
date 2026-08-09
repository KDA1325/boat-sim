(function () {
    "use strict";

    const core = window.BoatViewerCore;
    const MAX_CACHED_FRAME_PAIRS = 6;

    const elements = {
        folderInput: document.getElementById("folder-input"),
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
        metaFailed: document.getElementById("meta-failed")
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
        framePairCache: new Map()
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
        const colorUrl = URL.createObjectURL(frame.colorFile);
        const depthUrl = URL.createObjectURL(frame.depthFile);
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
        elements.metaVersion.textContent = "v" + metadata.version;
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

    async function loadSelectedFolder(fileList) {
        stopPlayback();
        state.renderRequestToken += 1;
        clearFramePairCache();
        state.session = null;
        state.currentPosition = 0;
        state.displayedPosition = -1;
        state.requestedPosition = -1;
        setControlsEnabled(false);
        setStatus("캡처 세션을 확인하고 첫 프레임을 준비하고 있습니다.", "info");

        try {
            const session = await core.FolderSequenceLoader.load(fileList);
            state.session = session;
            renderSessionMetadata(session);
            elements.emptyState.hidden = true;
            elements.viewer.hidden = false;
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
            clearFramePairCache();
            setControlsEnabled(false);
            setStatus(error.message, "error");
        }
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

    elements.playButton.addEventListener("click", togglePlayback);
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
