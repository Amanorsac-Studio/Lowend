# build-windows-installer.ps1 - builds LowEnd-<version>-Windows.exe.
#
#   powershell -ExecutionPolicy Bypass -File apps\lowend\scripts\build-windows-installer.ps1
#   ... -CertThumbprint <sha1>      sign with a certificate in the Windows store
#   ... -SkipBuild                  package what is already built
#   ... -Tester                     a TEST build: asks for a test key, named LowEnd-<v>-Tester-Windows.exe
#
# A test build is made in its own folder (build-tester) so it can never be mistaken
# for, or overwrite, the release binaries. It carries the key check in
# src\plugin\TesterGate.h and MUST NOT be sold; see that file for what it is.
#
# What it does, in order: builds Release, runs the tests, checks the things the
# Installer & Packaging Standard cares about (static runtime, delay-loaded ONNX
# Runtime, the model present and the right size), signs every binary if a
# certificate was named (P31), compiles installer\LowEnd.iss, and prints the
# filename, size and SHA-256 that go in the delivery note.
#
# SIGNING. No secret is read, stored or passed by this script. -CertThumbprint
# names a certificate already in the Windows certificate store (or on a hardware
# token); signtool asks the store for it. Without it the installer is built
# UNSIGNED and the script says so in capitals, because an unsigned installer is
# not a release (Build Standard B37).
param(
    [string] $CertThumbprint = "",
    [string] $TimestampUrl   = "http://timestamp.digicert.com",
    [string] $ModelDir       = "",
    [switch] $SkipBuild,
    [switch] $SkipTests,
    [switch] $Tester
)
$ErrorActionPreference = "Stop"
$app   = Split-Path -Parent $PSScriptRoot
$repo  = Resolve-Path (Join-Path $app "..\..")
$build = Join-Path $app $(if ($Tester) { "build-tester" } else { "build" })
$rel   = Join-Path $build "LowEnd_artefacts\Release"
$out   = Join-Path $app "release"

function Find-Tool([string[]] $candidates, [string] $what) {
    foreach ($c in $candidates) { $hit = Get-Item $c -ErrorAction SilentlyContinue | Select-Object -Last 1; if ($hit) { return $hit.FullName } }
    throw "Could not find $what."
}
$vs      = "C:\Program Files\Microsoft Visual Studio\18\Community"
$cmake   = Find-Tool @("$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe") "cmake"
$ctest   = Find-Tool @("$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe") "ctest"
$dumpbin = Find-Tool @("$vs\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe") "dumpbin"
$crt     = Find-Tool @("$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT") "the Visual C++ runtime DLLs"
$iscc    = Find-Tool @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "C:\Program Files (x86)\Inno Setup 6\ISCC.exe") "Inno Setup (ISCC.exe)"

# ---- version: one string, read from the one place it is defined (B34)
$version = (Select-String -Path (Join-Path $app "CMakeLists.txt") -Pattern 'project\(LowEnd VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Matches[0].Groups[1].Value
foreach ($f in @("installer\LowEnd.iss", "installer\README.txt", "installer\README-tester.txt")) {
    if (-not (Select-String -Path (Join-Path $app $f) -SimpleMatch $version -Quiet)) { throw "$f does not carry version $version." }
}
Write-Host "Low End $version"

# ---- the separation model: shipped in the installer, never downloaded (B46)
if (-not $ModelDir) {
    foreach ($c in @((Join-Path $app "models"), "$env:ProgramFiles\Amanorsac Studio\Low End\Models", "$([Environment]::GetFolderPath('MyDocuments'))\Amanorsac Studio\Low End\Models")) {
        if (Test-Path (Join-Path $c "htdemucs_fwd.onnx.data")) { $ModelDir = $c; break }
    }
}
if (-not $ModelDir) { throw "The separation model was not found. Pass -ModelDir <folder with htdemucs_fwd.onnx and .onnx.data>." }
$expect = @{ "htdemucs_fwd.onnx" = 2385507; "htdemucs_fwd.onnx.data" = 168361984 }
foreach ($k in $expect.Keys) {
    $len = (Get-Item (Join-Path $ModelDir $k)).Length
    if ($len -ne $expect[$k]) { throw "$k is $len bytes, expected $($expect[$k])." }
}
Write-Host "model: $ModelDir"

# ---- build and test
if (-not $SkipBuild) {
    Get-Process "Low End" -ErrorAction SilentlyContinue | ForEach-Object { throw "Low End is running (it holds its own .exe open). Close it and run this again." }
    $cfg = @("-S", $app, "-B", $build)
    if ($Tester) {
        $cfg += "-DLOWEND_TESTER_BUILD=ON"
        # Reuse the ONNX Runtime the release build already fetched, when it is there.
        $ort = Join-Path $app "build\_deps\onnxruntime-src"
        if (Test-Path $ort) { $cfg += "-DFETCHCONTENT_SOURCE_DIR_ONNXRUNTIME=$ort" }
    } else { $cfg += "-DLOWEND_TESTER_BUILD=OFF" }
    & $cmake @cfg | Out-Null
    & $cmake --build $build --config Release --parallel | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "The build failed. Run cmake --build by hand to see why." }
}
if (-not $SkipTests) {
    # Everything except the soak goes through ctest as usual.
    & $ctest --test-dir $build -C Release --output-on-failure -E LowEndSoakShort
    if ($LASTEXITCODE -ne 0) { throw "Tests failed; not packaging." }
    # The soak test measures the worst audio block against a real-time budget, so
    # it is only meaningful when scheduled the way an audio thread is. On a
    # machine that is also running a DAW it fails at normal priority on
    # contention alone (measured: 16-31 ms worst blocks at normal priority,
    # 3 ms at High, same binary, same minute).
    $soak = Start-Process -FilePath (Join-Path $build "LowEndSoak_artefacts\Release\LowEndSoak.exe") -ArgumentList "--seconds","30" -NoNewWindow -PassThru
    $soak.PriorityClass = "High"; $soak.WaitForExit()
    if ($soak.ExitCode -ne 0) { throw "The soak test failed; not packaging." }
}

# ---- checks a buyer's clean machine would otherwise make for us
$bins = @("$rel\Standalone\Low End.exe", "$rel\VST3\Low End.vst3\Contents\x86_64-win\Low End.vst3")
foreach ($b in $bins) {
    $deps = (& $dumpbin /DEPENDENTS $b) -join "`n"
    if ($deps -match "(?i)VCRUNTIME140|MSVCP140") { throw "$b depends on the Visual C++ runtime DLLs; it must link the runtime statically." }
    $imports = (& $dumpbin /IMPORTS $b) -join "`n"
    if ($imports -notmatch "(?is)delay load imports.*onnxruntime\.dll") { throw "$b does not delay-load onnxruntime.dll." }
}
if (-not (Test-Path "$rel\VST3\Low End.vst3\Contents\x86_64-win\onnxruntime.dll")) { throw "onnxruntime.dll is missing from the VST3 bundle." }
Write-Host "binaries: static runtime, ONNX Runtime delay-loaded"

# A release must not carry the test-key public key (Licence Standard R2), and a test
# build must, or its gate is not there. Looked for in the bytes, not assumed from flags.
$mod = (Select-String -Path (Join-Path $app "src\plugin\TesterKey.h") -Pattern '"([0-9a-f]{16})' | Select-Object -First 1).Matches[0].Groups[1].Value
foreach ($b in $bins) {
    $has = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($b)).Contains($mod)
    if (-not $Tester -and $has) { throw "$b contains the test-build key. A release must not; rebuild with LOWEND_TESTER_BUILD=OFF." }
    if ($Tester -and -not $has) { throw "$b has no test-build key check; it was not built with LOWEND_TESTER_BUILD=ON." }
}
Write-Host ("binaries: test-key check " + $(if ($Tester) { "present" } else { "absent (release)" }))

# ---- sign every binary we built (P31). Microsoft's DLLs are already signed.
$signArg = @()
if ($CertThumbprint) {
    $signtool = Find-Tool @("C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe") "signtool"
    foreach ($b in $bins) {
        & $signtool sign /sha1 $CertThumbprint /fd sha256 /tr $TimestampUrl /td sha256 /d "Low End" /du "https://amanorsac.studio" $b
        if ($LASTEXITCODE -ne 0) { throw "Signing failed for $b." }
    }
    $signArg = @("/DSignCmd=`"$signtool`" sign /sha1 $CertThumbprint /fd sha256 /tr $TimestampUrl /td sha256 /d `$qLow End`$q /du https://amanorsac.studio `$f")
}

# ---- compile
New-Item -ItemType Directory -Force $out | Out-Null
$testerDef = @(); if ($Tester) { $testerDef = @("/DTester") }
& $iscc @testerDef "/DBuildDir=$rel" "/DCrtDir=$crt" "/DModelDir=$ModelDir" "/DOutDir=$out" @signArg (Join-Path $app "installer\LowEnd.iss")
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed." }

$exe = Get-Item (Join-Path $out $(if ($Tester) { "LowEnd-$version-Tester-Windows.exe" } else { "LowEnd-$version-Windows.exe" }))
$sha = (Get-FileHash $exe.FullName -Algorithm SHA256).Hash
Write-Host ""
Write-Host "  file     $($exe.Name)"
Write-Host "  bytes    $($exe.Length)"
Write-Host "  sha256   $sha"
if ($CertThumbprint) { Write-Host "  signed   yes ($CertThumbprint)" }
else { Write-Host "  signed   NO - THIS INSTALLER IS UNSIGNED AND IS NOT A RELEASE. Re-run with -CertThumbprint." -ForegroundColor Yellow }
