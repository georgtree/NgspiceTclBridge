#!/usr/bin/env bash
set -euo pipefail
root=$PWD
prefix="$root/_ci/ngspice"
work="$root/_ci/ngspice-build"
mkdir -p "$work"
cd "$work"
source_url=https://downloads.sourceforge.net/project/ngspice/ng-spice-rework/47/ngspice-47.tar.gz
curl --fail --location --retry 3 --output source.tar.gz "$source_url"
echo '894e649651f1838a14095e5a5439e7d3aa63e87ede14d283173fda4fcdef675f  source.tar.gz' | sha256sum -c -
tar -xzf source.tar.gz
opts=(--with-ngshared --enable-xspice --disable-openmp --without-readline --without-x)
if [[ ${MSYSTEM:-} == UCRT64 ]]; then
    opts+=(--enable-relpath)
    prefix=$(cygpath -m "$prefix")
fi
./ngspice-47/configure --prefix="$prefix" "${opts[@]}"
make -j4
make install
if [[ ${MSYSTEM:-} == UCRT64 ]]; then
    # Libtool names this DLL libngspice-0.dll. The test suite expects ngspice.dll.
    cd "$prefix/bin"
    if [[ ! -f ngspice.dll ]]; then
        dll=(libngspice*.dll)
        cp "${dll[0]}" ngspice.dll
    fi
    # LoadLibraryEx searches the simulator's directory for private dependencies.
    # Include transitive UCRT64 runtime DLLs there for the MSVC runner too.
    dlls=(ngspice.dll)
    for ((i=0; i<${#dlls[@]}; i++)); do
        objdump -p "${dlls[i]}" > "$work/imports.txt"
        while read -r dependency; do
            if [[ -f /ucrt64/bin/$dependency && ! -f $dependency ]]; then
                cp "/ucrt64/bin/$dependency" .
                dlls+=("$dependency")
            fi
        done < <(awk '/DLL Name:/ {print $3}' "$work/imports.txt")
    done
else
    test -f "$prefix/lib/libngspice.so"
fi
