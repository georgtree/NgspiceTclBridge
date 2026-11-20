#!/usr/bin/env bash
set -euo pipefail
root=$PWD
prefix="$root/_ci/runtime"
platform=unix
toolchain=linux-gcc
native_path() { printf '%s' "$1"; }
if [[ ${MSYSTEM:-} == UCRT64 ]]; then
    platform=win
    toolchain=windows-ucrt64
    native_path() { cygpath -m "$1"; }
fi
if [[ ${RUNTIME_CACHE_HIT:-false} != true ]]; then
    mkdir -p _ci/tcl-build
    cd _ci/tcl-build
    "$root/_ci/tcl/$platform/configure" --prefix="$prefix"
    make -j2
    make install
else
    echo "Using cached Tcl runtime"
fi
export PATH="$prefix/bin:$PATH"
export LD_LIBRARY_PATH="$prefix/lib:${LD_LIBRARY_PATH:-}"
build="$root/_ci/bridge-build"
mkdir -p "$build"
cd "$build"
bash "$root/configure" --with-tcl="$prefix/lib" --with-rbc="$root/_ci/rbc" --prefix="$prefix" --exec-prefix="$prefix"
make -j2
# pkgIndex.tcl loads the compiled library and sources ngspicetclbridge.tcl beside it.
cp "$root/ngspicetclbridge.tcl" .
export TCLLIBPATH="{$(native_path "$build")}"
for dep in argparse rbc; do
    pkg=$(cat "$root/_ci/$dep/package-dir.txt")
    TCLLIBPATH+=" {$(native_path "$root/_ci/$dep/lib/$pkg")}"
done
TCLLIBPATH+=" {$(native_path "$root/_ci/tcllib/modules")}"
tclsh="$prefix/bin/tclsh9.1"
if [[ $platform == win ]]; then tclsh="$prefix/bin/tclsh91.exe"; fi
spice="$root/_ci/ngspice/lib"
if [[ $platform == win ]]; then spice="$root/_ci/ngspice/bin"; fi
export NGSPICE_CODEMODEL_DIR="$(native_path "$root/_ci/ngspice/lib/ngspice")"
export SPICE_SCRIPTS="$(native_path "$root/_ci/ngspice/share/ngspice/scripts")"
export NGSPICE_DLL="$(native_path "$spice")"
"$tclsh" "$(native_path "$root/.github/scripts/require-vectors.tcl")"
make test test-rbc test-events test-events-real TCLLIBPATH="$TCLLIBPATH"
if [[ ${MAKE_DIST:-false} == true ]]; then
    ext=tar.gz
    target=dist
    if [[ $platform == win ]]; then ext=zip; target=dist-zip; fi
    make "$target" DIST_ROOT="$root/_ci/dist"
    archive=("$root"/_ci/dist/*."$ext")
    name=$(basename "${archive[0]}" ".$ext")
    mkdir -p "$root/_ci/package"
    cp "${archive[0]}" "$root/_ci/package/$name-$toolchain.$ext"
fi
