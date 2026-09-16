# Drives an AV1 round trip through the CMAF muxer and decoder.
#
# AV1 was the codec the pipeline claimed to carry and had never been made to
# prove: `cmaf` and `cmaf_hevc` round-tripped the other two, and nothing in the
# tests mentioned av1 at all. That is the difference between a codec that
# happens to work and one that is supported — and it is the gap an AV1 encoder
# setting lives in, because "we carry it" was true by construction (the muxer is
# FFmpeg's) and exercised by nothing.
#
# Skips cleanly when this machine has no AV1 encoder or decoder. It does NOT
# skip when one is present and the fixture still fails to build: that is the
# arguments below being wrong, and the first draft of this file hid exactly that
# behind a cheerful "skipping", leaving two green tests that proved nothing.

find_program(FFMPEG_BIN ffmpeg)
if(NOT FFMPEG_BIN)
    message(STATUS "ffmpeg CLI not found — skipping AV1 test")
    return()
endif()

execute_process(COMMAND ${FFMPEG_BIN} -hide_banner -encoders
                OUTPUT_VARIABLE encoders ERROR_QUIET)

# Decoding is a separate capability from encoding. An ffmpeg built with
# libsvtav1 and no AV1 decoder would mux a stream nothing here could read back,
# and the point of the test is this muxer's output rather than the runner's
# decoder, so that is a genuine skip.
execute_process(COMMAND ${FFMPEG_BIN} -hide_banner -decoders
                OUTPUT_VARIABLE decoders ERROR_QUIET)
string(FIND "${decoders}" "av1" av1_dec_pos)
if(av1_dec_pos EQUAL -1)
    message(STATUS "no AV1 decoder in this ffmpeg build — skipping AV1 test")
    return()
endif()

# Software first: those are guaranteed to run anywhere, and a failure from one
# of them means something. A GPU encoder can be compiled in and unusable (no
# card, no driver), which is an environment gap rather than a fault.
set(AV1_SOFTWARE libsvtav1 libaom-av1)
set(AV1_CANDIDATES libsvtav1 libaom-av1 av1_nvenc av1_qsv av1_amf)

set(FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/av1_src.mp4")
set(OUTDIR  "${CMAKE_CURRENT_BINARY_DIR}/av1_out")
file(REMOVE "${FIXTURE}")
file(REMOVE_RECURSE "${OUTDIR}")

set(VENC "")
set(SOFTWARE_TRIED "")
set(LAST_ERR "")

foreach(cand ${AV1_CANDIDATES})
    string(FIND "${encoders}" "${cand}" pos)
    if(pos EQUAL -1)
        continue()
    endif()

    # Each encoder's own private options, and only its own: passing libaom's to
    # libsvtav1 is an "Unrecognized option" error, which is precisely how the
    # first draft of this file managed to skip without saying so.
    if(cand STREQUAL "libsvtav1")
        # Keyframes on the segment interval and no scene-cut, matching what the
        # encoder settings do in production — the muxer's job depends on
        # keyframe alignment. 180 frames at 30fps is the 6s default segment.
        #
        # crf rather than qp: SVT-AV1 rejects a qp alongside CRF mode
        # ("bad parameter"), which is the second thing this file got wrong
        # before it could run at all.
        set(PARAMS -svtav1-params "keyint=180:crf=40")
    elseif(cand STREQUAL "libaom-av1")
        set(PARAMS -aom-params "cpu-used=8:crf=40:kf-max-dist=180")
    else()
        set(PARAMS "")
    endif()

    # `IN_LIST` would be tidier and is deliberately not used: it needs policy
    # CMP0057 set to NEW, and a script run with `cmake -P` does not inherit the
    # project's policy version. This passed on a CMake 4 machine and failed on
    # the CI runner's 3.x with "Unknown arguments specified" — list(FIND) has no
    # such dependency on how the script is run.
    list(FIND AV1_SOFTWARE "${cand}" _soft_idx)
    if(_soft_idx GREATER -1)
        list(APPEND SOFTWARE_TRIED "${cand}")
    endif()

    message(STATUS "AV1 test trying encoder: ${cand}")
    # Three tracks — video plus two audio — matching the other two fixtures:
    # the muxer's job here is to carry a multi-track production feed, so a
    # single-track fixture would not exercise it.
    execute_process(
        COMMAND ${FFMPEG_BIN} -y -v error
                -f lavfi -i testsrc2=size=1280x720:rate=30:duration=8
                -f lavfi -i sine=frequency=440:duration=8
                -f lavfi -i sine=frequency=880:duration=8
                -map 0:v -map 1:a -map 2:a
                -c:v ${cand} ${PARAMS}
                -c:a aac -shortest "${FIXTURE}"
        RESULT_VARIABLE gen_rc ERROR_VARIABLE gen_err)
    if(gen_rc EQUAL 0)
        set(VENC "${cand}")
        break()
    endif()
    set(LAST_ERR "${cand}: ${gen_err}")
endforeach()

if(VENC STREQUAL "")
    if(SOFTWARE_TRIED)
        # A software encoder is present and could not do the work. That is this
        # file's fault, not the runner's, and it must not read as a skip.
        message(FATAL_ERROR
            "the AV1 fixture could not be built with ${SOFTWARE_TRIED}, which "
            "this ffmpeg has: ${LAST_ERR}")
    endif()
    message(STATUS "no usable AV1 encoder on this machine — skipping (${LAST_ERR})")
    return()
endif()

message(STATUS "AV1 test using encoder: ${VENC}")

execute_process(COMMAND ${TEST_EXE} "${FIXTURE}" "${OUTDIR}"
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "AV1 CMAF test failed (exit ${rc})")
endif()

# The muxer's output has to exist, or the decode test would "skip" over a muxer
# that quietly wrote nothing — the same trap as above in different clothes.
if(NOT EXISTS "${OUTDIR}/init.mp4" OR NOT EXISTS "${OUTDIR}/seg_000.m4s")
    message(FATAL_ERROR
        "the AV1 muxer reported success but wrote no init.mp4/seg_000.m4s "
        "into ${OUTDIR}")
endif()
