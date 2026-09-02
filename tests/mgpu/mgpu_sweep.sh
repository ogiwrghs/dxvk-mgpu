#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# mgpu_sweep.sh
#
# Walks the testbed's feature axes one at a time and records, for each
# configuration, whether GPU1's output actually reached the composite.
#
# One axis per run rather than a full cross product: with nine axes the
# product is hundreds of runs, and a failure in a combination tells you less
# than a failure in a single axis does. The combinations at the end exist only
# to catch interactions between axes that each pass alone.
#
# The decisive column is READBACK. The composite's debug readback samples the
# worker image every thirty frames; the first sample of any run is always
# [0,0,0,0] because the composite reads before GPU1's first write lands, so
# the script ignores sample one and judges the rest.
#
# Usage:
#   cd ~/testbed && ./mgpu_sweep.sh 2>&1 | tee sweep_console.txt
#
# Results land in sweep_results.txt.
# ---------------------------------------------------------------------------

set -u

EXE=./d3d11_testbed.exe
OUT=sweep_results.txt
FRAMES=${FRAMES:-180}
BASE="-w 1280 -h 720 -frames $FRAMES -psload 16 -draws 16 -instances 64 -logcalls 0 -quiet"

if [ ! -x "$EXE" ]; then
    echo "cannot find $EXE - run this from the testbed directory"
    exit 1
fi

: > "$OUT"

{
    echo "mgpu sweep"
    echo "date:    $(date)"
    echo "frames:  $FRAMES"
    echo "base:    $BASE"
    echo "dll:     $(ls -la d3d11.dll 2>/dev/null | awk '{print $6, $7, $8}')"
    echo "env:     $(env | grep -i '^DXVK' | tr '\n' ' ')"
    echo
    printf "%-28s %-8s %-10s %-9s %s\n" "CONFIG" "FPS" "READBACK" "SAMPLES" "NOTES"
    printf -- "---------------------------------------------------------------------------\n"
} >> "$OUT"

run_case() {
    local name="$1"; shift
    local args="$*"

    rm -f d3d11_testbed.log d3d11_testbed_d3d11.log d3d11_testbed_dxgi.log

    DXVK_MGPU_DEBUG_DRAW=1 $EXE $BASE $args -tag "$name" > /dev/null 2>&1
    local rc=$?

    local fps="-"
    local readback="-"
    local samples=0
    local notes=""

    if [ $rc -ne 0 ]; then
        notes="exit=$rc"
    fi

    if [ -f d3d11_testbed.log ]; then
        fps=$(grep -o 'fps=[0-9.]*' d3d11_testbed.log | tail -1 | cut -d= -f2)
        [ -z "$fps" ] && fps="-"

        if grep -q '^\[.*\] \[error\]' d3d11_testbed.log; then
            notes="${notes} app-error"
        fi
    fi

    local dxlog=d3d11_testbed_d3d11.log

    if [ -f "$dxlog" ]; then
        # Skip the first sample: it is warm-up and always reads as untouched.
        local all
        all=$(grep 'worker image .* centre' "$dxlog" | tail -n +2)
        samples=$(printf '%s\n' "$all" | grep -c 'centre' || true)

        if [ "$samples" -eq 0 ]; then
            readback="NOSAMPLE"
        else
            local nonzero
            nonzero=$(printf '%s\n' "$all" | grep -v 'centre \[0,0,0,0\]' | grep -c 'centre' || true)

            if [ "$nonzero" -eq 0 ]; then
                readback="EMPTY"
            elif [ "$nonzero" -eq "$samples" ]; then
                readback="OK"
            else
                readback="PARTIAL($nonzero/$samples)"
            fi
        fi

        local errs
        errs=$(grep -c '^err' "$dxlog" || true)
        [ "$errs" -gt 0 ] && notes="${notes} dxvk-err=$errs"

        if grep -q 'extent mismatch' "$dxlog"; then
            notes="${notes} extent-mismatch"
        fi
        if grep -q 'no worker image' "$dxlog"; then
            notes="${notes} no-worker-image"
        fi
        if grep -q 'SRVNULL' "$dxlog"; then
            notes="${notes} srv-null"
        fi
    else
        readback="NOLOG"
    fi

    printf "%-28s %-8s %-10s %-9s %s\n" "$name" "$fps" "$readback" "$samples" "$notes" >> "$OUT"
    printf "%-28s %-8s %-10s %-9s %s\n" "$name" "$fps" "$readback" "$samples" "$notes"

    # Keep the DXVK log of anything that failed, so a bad case can be read
    # afterwards without reproducing it.
    if [ "$readback" != "OK" ] && [ -f "$dxlog" ]; then
        cp "$dxlog" "sweep_fail_${name}.log" 2>/dev/null
    fi
}

echo "=== baselines ==="
run_case single-gpu-baseline ""   # note: run manually with DXVK_MGPU_DISABLE=1 to compare
run_case plain                ""
run_case clearonly            "-clearonly"

echo
echo "=== one axis at a time ==="
run_case renderscale-1.5      "-renderscale 1.5"
run_case renderscale-0.5      "-renderscale 0.5"
run_case passes-1             "-passes 1"
run_case passes-3             "-passes 3"
run_case pyramid-2            "-pyramid 2"
run_case pyramid-4            "-pyramid 4"
run_case msaa-4               "-msaa 4"
run_case depthread            "-depthread"
run_case compute              "-compute"
run_case copyres              "-copyres"
run_case copyregion           "-copyregion"
run_case updatesub            "-updatesub"

echo
echo "=== superposition-shaped combinations ==="
run_case superposition-lite   "-renderscale 1.5 -pyramid 3 -passes 2"
run_case superposition-full   "-renderscale 1.5 -pyramid 3 -passes 2 -compute -depthread"
run_case unmirrored-all       "-compute -copyres -copyregion -msaa 4"
run_case heavy                "-renderscale 1.5 -pyramid 4 -passes 3 -compute -depthread -copyres"

{
    echo
    echo "READBACK column:"
    echo "  OK        every sample after the first showed pixels from GPU1"
    echo "  EMPTY     every sample was [0,0,0,0] - GPU1 contributed nothing"
    echo "  PARTIAL   some samples had pixels, some did not"
    echo "  NOSAMPLE  the composite never ran, or never got past its extent check"
    echo "  NOLOG     no DXVK log was produced at all"
    echo
    echo "Any case that is not OK has its DXVK log kept as sweep_fail_<name>.log"
} >> "$OUT"

echo
echo "done - results in $OUT"
