cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_LIST_DIR}/ObsHelpers.cmake")

alpha_recorder_require(REPO_ROOT)
alpha_recorder_require(BUILD_DIR)

set(STAGE_DIR "${BUILD_DIR}/Testing/Temporary/obs-app-stage" CACHE PATH "Portable OBS app E2E stage directory")
set(OBS_ROOT "$ENV{OBS_ROOT}" CACHE PATH "OBS runtime root")
set(CONFIGURATION "RelWithDebInfo" CACHE STRING "Build configuration")
set(PORT 0 CACHE STRING "obs-websocket port, or 0 for an ephemeral port")
set(SYNC_RECORD_SECONDS 2 CACHE STRING "Short recording duration for sync-correctness attempts")
set(SYNC_ATTEMPTS 5 CACHE STRING "Number of short sync-correctness recording attempts")
set(DURABILITY_RECORD_SECONDS 30 CACHE STRING "Sustained recording duration for durability validation")
set(WIDTH 1920 CACHE STRING "Canvas width")
set(HEIGHT 1080 CACHE STRING "Canvas height")
set(FPS 60 CACHE STRING "Recording FPS")
set(FPS_NUM "" CACHE STRING "Recording FPS numerator; defaults to FPS")
set(FPS_DEN 1 CACHE STRING "Recording FPS denominator")
set(RECORD_FORMAT "mkv" CACHE STRING "OBS recording container format")
set(ARTIFACT_BASE "" CACHE PATH "Optional OBS app E2E artifact base directory")
set(OUTPUT_MODE "simple" CACHE STRING "OBS output mode for E2E: simple or advanced-standard")
set(RECORD_AUDIO_ENCODER "aac" CACHE STRING "OBS recording audio encoder for E2E: aac or pcm_s16le")
set(WITH_AUDIO OFF CACHE BOOL "Add a deterministic audio source to the OBS app E2E scene")
set(RGB_ENCODER "software" CACHE STRING "RGB recording encoder profile: software, apple_hevc, nvenc_hevc, or amd_hevc")
set(FINALIZATION_FORMAT "mask_png_mov" CACHE STRING "Alpha Recorder finalization format")
set(HEVC_QUALITY_PROFILE "high_quality" CACHE STRING "Alpha Recorder HEVC quality profile")
set(HEVC_QUALITY_CQ 19 CACHE STRING "Alpha Recorder HEVC CQ value")
set(HEVC_PRESET "nvenc_p3" CACHE STRING "Alpha Recorder HEVC preset")
set(HEVC_NVENC_TUNE "hq" CACHE STRING "Alpha Recorder NVENC tune")
set(HEVC_GOP_SIZE 0 CACHE STRING "Alpha Recorder HEVC GOP size in frames, or 0 for auto")
set(SKIP_BUILD OFF CACHE BOOL "Skip plugin build before OBS app E2E")
set(SKIP_STAGE OFF CACHE BOOL "Skip staging before OBS app E2E")
set(KEEP_OBS_OPEN OFF CACHE BOOL "Keep OBS open after the E2E run")
set(ALLOW_OVERLOAD OFF CACHE BOOL "Continue OBS app E2E after OBS reports overload")
set(VERIFY_NLE_TIMELINE OFF CACHE BOOL "Verify alpha ISO BMFF output uses an NLE-friendly exact-CFR timeline")
set(STRICT_ALL_FRAMES OFF CACHE BOOL "Require every shared decoded RGB/alpha frame to match")
set(PHASE_SWEEP_STEPS 0 CACHE STRING "Number of graphics-cadence phase buckets to sweep")
set(REQUIRE_MAIN_PTS_GAP OFF CACHE BOOL "Fail unless a main packet PTS gap is observed")
set(REQUIRE_PACKET_REORDER OFF CACHE BOOL "Fail unless packet presentation reorder is observed")
set(REQUIRE_REPLAY_UNDERFLOW OFF CACHE BOOL "Fail unless a replay queue underflow is observed")
set(REQUIRE_REPLAY_CATCHUP OFF CACHE BOOL "Fail unless stale replay evidence is skipped during recovery")
set(REQUIRE_OVERLOAD OFF CACHE BOOL "Fail unless OBS overload is observed")
set(REQUIRE_TAIL_REPEAT OFF CACHE BOOL "Fail unless a replay tail-repeat slot is observed")
set(PAUSE_AT_MS -1 CACHE STRING "Pause the recording at active-recording milliseconds, or -1")
set(PAUSE_DURATION_MS 0 CACHE STRING "Pause duration in milliseconds")
set(SPLIT_AT_MS -1 CACHE STRING "Request a recording file split at active-recording milliseconds, or -1")
set(OVERLOAD_PULSE_AT_MS -1 CACHE STRING "Start an E2E render-delay overload pulse at active-recording milliseconds")
set(OVERLOAD_PULSE_DURATION_MS 0 CACHE STRING "E2E render-delay overload pulse duration")
set(OVERLOAD_PULSE_DELAY_MS 0 CACHE STRING "Per-frame E2E source render delay during the overload pulse")
set(BEST_EFFORT_SYNC OFF CACHE BOOL "Publish a best-effort alpha output when sync proof fails")
set(TEST_FAULT "" CACHE STRING "E2E-only injected fault name")
set(FAULT_SEGMENT 0 CACHE STRING "One-based segment index for lifecycle fault injection, or 0 for all")
set(REPLAY_GAP_PACKETS 24 CACHE STRING "Number of replay evidence packets held by the E2E replay-gap fault")
set(EXPECT_RESULT "normal" CACHE STRING "Expected alpha result: normal, sync-invalid, no-alpha, or temp-preserved")

alpha_recorder_abs_path(REPO_ROOT "${REPO_ROOT}" "${CMAKE_CURRENT_LIST_DIR}")
alpha_recorder_abs_path(BUILD_DIR "${BUILD_DIR}" "${REPO_ROOT}")
alpha_recorder_abs_path(STAGE_DIR "${STAGE_DIR}" "${REPO_ROOT}")
if(NOT OBS_ROOT STREQUAL "")
    alpha_recorder_abs_path(OBS_ROOT "${OBS_ROOT}" "${REPO_ROOT}")
endif()
if(NOT ARTIFACT_BASE STREQUAL "")
    alpha_recorder_abs_path(ARTIFACT_BASE "${ARTIFACT_BASE}" "${REPO_ROOT}")
endif()

if(CONFIGURATION STREQUAL "")
    set(CONFIGURATION "RelWithDebInfo")
endif()

if(NOT SKIP_BUILD)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIGURATION}" --target alpha_recorder_plugin alpha_recorder_obs_app_e2e_source
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()
if(FPS_NUM STREQUAL "")
    set(FPS_NUM "${FPS}")
endif()

if(NOT SKIP_STAGE)
    set(stage_args "-DINCLUDE_TEST_SOURCE=ON")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DREPO_ROOT=${REPO_ROOT}"
            "-DBUILD_DIR=${BUILD_DIR}"
            "-DSTAGE_DIR=${STAGE_DIR}"
            "-DOBS_ROOT=${OBS_ROOT}"
            "-DCONFIGURATION=${CONFIGURATION}"
            ${stage_args}
            -P "${CMAKE_CURRENT_LIST_DIR}/StageObsTree.cmake"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

unset(BUN_EXECUTABLE)
if(WIN32 AND DEFINED ENV{USERPROFILE})
    file(GLOB _alpha_recorder_scoop_bun_paths LIST_DIRECTORIES false "$ENV{USERPROFILE}/scoop/apps/bun/*/bun.exe")
    list(FILTER _alpha_recorder_scoop_bun_paths EXCLUDE REGEX "/current/")
    list(SORT _alpha_recorder_scoop_bun_paths)
    list(REVERSE _alpha_recorder_scoop_bun_paths)
    foreach(_alpha_recorder_scoop_bun_path IN LISTS _alpha_recorder_scoop_bun_paths)
        if(EXISTS "${_alpha_recorder_scoop_bun_path}")
            set(BUN_EXECUTABLE "${_alpha_recorder_scoop_bun_path}")
            break()
        endif()
    endforeach()
endif()

if(NOT BUN_EXECUTABLE AND DEFINED ENV{HOME} AND EXISTS "$ENV{HOME}/.bun/bin/bun")
    set(BUN_EXECUTABLE "$ENV{HOME}/.bun/bin/bun")
endif()

if(NOT BUN_EXECUTABLE)
    find_program(BUN_EXECUTABLE bun REQUIRED)
endif()

set(args
    "${CMAKE_CURRENT_LIST_DIR}/obs_app_e2e.ts"
    "--repo-root" "${REPO_ROOT}"
    "--build-dir" "${BUILD_DIR}"
    "--stage-dir" "${STAGE_DIR}"
    "--configuration" "${CONFIGURATION}"
    "--port" "${PORT}"
    "--sync-record-seconds" "${SYNC_RECORD_SECONDS}"
    "--sync-attempts" "${SYNC_ATTEMPTS}"
    "--durability-record-seconds" "${DURABILITY_RECORD_SECONDS}"
    "--width" "${WIDTH}"
    "--height" "${HEIGHT}"
    "--fps-num" "${FPS_NUM}"
    "--fps-den" "${FPS_DEN}"
    "--record-format" "${RECORD_FORMAT}"
    "--output-mode" "${OUTPUT_MODE}"
    "--record-audio-encoder" "${RECORD_AUDIO_ENCODER}"
    "--rgb-encoder" "${RGB_ENCODER}"
    "--finalization-format" "${FINALIZATION_FORMAT}"
    "--hevc-quality-profile" "${HEVC_QUALITY_PROFILE}"
    "--hevc-quality-cq" "${HEVC_QUALITY_CQ}"
    "--hevc-preset" "${HEVC_PRESET}"
    "--hevc-nvenc-tune" "${HEVC_NVENC_TUNE}"
    "--hevc-gop-size" "${HEVC_GOP_SIZE}"
    "--phase-sweep-steps" "${PHASE_SWEEP_STEPS}"
    "--pause-at-ms" "${PAUSE_AT_MS}"
    "--pause-duration-ms" "${PAUSE_DURATION_MS}"
    "--split-at-ms" "${SPLIT_AT_MS}"
    "--overload-pulse-at-ms" "${OVERLOAD_PULSE_AT_MS}"
    "--overload-pulse-duration-ms" "${OVERLOAD_PULSE_DURATION_MS}"
    "--overload-pulse-delay-ms" "${OVERLOAD_PULSE_DELAY_MS}"
    "--fault-segment" "${FAULT_SEGMENT}"
    "--replay-gap-packets" "${REPLAY_GAP_PACKETS}"
    "--expect-result" "${EXPECT_RESULT}"
)
if(NOT TEST_FAULT STREQUAL "")
    list(APPEND args "--test-fault" "${TEST_FAULT}")
endif()
if(BEST_EFFORT_SYNC)
    list(APPEND args "--best-effort-sync")
endif()
if(KEEP_OBS_OPEN)
    list(APPEND args "--keep-obs-open")
endif()
if(ALLOW_OVERLOAD)
    list(APPEND args "--allow-overload")
endif()
if(VERIFY_NLE_TIMELINE)
    list(APPEND args "--verify-nle-timeline")
endif()
if(STRICT_ALL_FRAMES)
    list(APPEND args "--strict-all-frames")
endif()
if(REQUIRE_MAIN_PTS_GAP)
    list(APPEND args "--require-main-pts-gap")
endif()
if(REQUIRE_PACKET_REORDER)
    list(APPEND args "--require-packet-reorder")
endif()
if(REQUIRE_REPLAY_UNDERFLOW)
    list(APPEND args "--require-replay-underflow")
endif()
if(REQUIRE_REPLAY_CATCHUP)
    list(APPEND args "--require-replay-catchup")
endif()
if(REQUIRE_OVERLOAD)
    list(APPEND args "--require-overload")
endif()
if(REQUIRE_TAIL_REPEAT)
    list(APPEND args "--require-tail-repeat")
endif()
if(WITH_AUDIO)
    list(APPEND args "--with-audio")
endif()
if(NOT ARTIFACT_BASE STREQUAL "")
    list(APPEND args "--artifact-base" "${ARTIFACT_BASE}")
endif()

if(APPLE)
    execute_process(
        COMMAND /bin/sh -c "exec \"$0\" \"$@\"" "${BUN_EXECUTABLE}" ${args}
        RESULT_VARIABLE run_result
    )
else()
    execute_process(
        COMMAND "${BUN_EXECUTABLE}" ${args}
        RESULT_VARIABLE run_result
    )
endif()
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "OBS app E2E failed with exit code ${run_result}")
endif()
