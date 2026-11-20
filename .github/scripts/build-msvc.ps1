# Run from the repository root in an x64 Visual Studio developer PowerShell.
# Tcl and Tcllib source trees must be at _ci/tcl and _ci/tcllib.
$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
$prefix = Join-Path $root '_ci/runtime'
function Invoke-Nmake {
    & nmake /nologo /f makefile.vc @args
    if ($LASTEXITCODE -ne 0) { throw "nmake failed with exit code $LASTEXITCODE" }
}
if ($env:RUNTIME_CACHE_HIT -eq 'true') {
    Write-Host "Using cached Tcl runtime"
} else {
    Push-Location _ci/tcl/win
    try {
        Invoke-Nmake "INSTALLDIR=$prefix" release
        Invoke-Nmake "INSTALLDIR=$prefix" install
    } finally { Pop-Location }
}

$out = Join-Path $root '_ci/bridge-build'
$env:Path = "$prefix\bin;" + $env:Path
$env:NGSPICE_DLL = Join-Path $root '_ci/ngspice/bin'
$env:NGSPICE_CODEMODEL_DIR = Join-Path $root '_ci/ngspice/lib/ngspice'
$env:SPICE_SCRIPTS = Join-Path $root '_ci/ngspice/share/ngspice/scripts'
$paths = @($out)
foreach ($dep in 'argparse','rbc') {
    $pkg = (Get-Content "$root/_ci/$dep/package-dir.txt" -Raw).Trim()
    $paths += "$root/_ci/$dep/lib/$pkg"
}
$paths += "$root/_ci/tcllib/modules"
$env:TCLLIBPATH = ($paths | ForEach-Object { '{' + $_.Replace('\','/') + '}' }) -join ' '
$buildArgs = @("INSTALLDIR=$prefix", "TCLDIR=$prefix", "RBC_DIR=$root\_ci\rbc", "OUT_DIR=$out", "TMP_DIR=$out\obj")
Push-Location win
try {
    Invoke-Nmake @buildArgs
    & "$prefix/bin/tclsh91.exe" "$root/.github/scripts/require-vectors.tcl"
    if ($LASTEXITCODE -ne 0) { throw "RBC vector smoke test failed" }
    $testLog = Join-Path $out 'tcltest.log'
    Invoke-Nmake @buildArgs test | Tee-Object -FilePath $testLog
    if (!(Select-String -Path $testLog -Quiet -Pattern '^all\.tcl:\s+Total\s+\d+\s+Passed\s+[1-9]\d*\s+Skipped\s+\d+\s+Failed\s+0\s*$')) {
        throw 'MSVC test target did not report a successful Tcltest run'
    }
    Invoke-Nmake @buildArgs test-fixtures test-events-real
    if ($env:MAKE_DIST -ne 'true') { return }
    Invoke-Nmake @buildArgs "DIST_ROOT=$root\_ci\dist" dist-zip
} finally { Pop-Location }

$archive = Get-Item "$root/_ci/dist/*.zip"
New-Item -ItemType Directory -Force "$root/_ci/package" | Out-Null
Copy-Item $archive.FullName "$root/_ci/package/$($archive.BaseName)-windows-msvc.zip"
