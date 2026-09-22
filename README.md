# Low End

A bass amp, pedalboard and looper for the stage, from **Amanorsac Studio** — plus a
Learn tab that listens to a song and shows its chords, key and notes, and can split
it into stems (drums / bass / vocals / other) to mute the bass and play along.

Ships as a standalone application and a VST3 / Audio Unit plug-in, for Windows and
macOS. See `docs/LOWEND-BUILD-SPEC.md` for the full engineering spec and
`DECISIONS-lowend.md` for why things are built the way they are.

## Building

Requires CMake 3.24+ and a C++20 compiler. JUCE 8 is fetched automatically if not
found beside this checkout.

```bash
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release
```

On Windows, the WebView2 SDK is needed at configure time:

```bash
nuget install Microsoft.Web.WebView2 -Version 1.0.2903.40 -OutputDirectory <dir>
```

### Windows installer

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-windows-installer.ps1
```

Add `-CertThumbprint <sha1>` to sign it, or `-Tester` to build a key-gated test
build instead of a release (see `src/plugin/TesterGate.h`).

### macOS package

```bash
scripts/build-mac-pkg.sh
```

Signs, notarises and staples when `APPLE_TEAM_ID`, `ASC_KEY_PATH`, `ASC_KEY_ID` and
`ASC_ISSUER_ID` are set (see `docs/DELIVERY-AUDIT-1.0.0.md` for what each requires);
otherwise it produces an unsigned `.pkg` for local testing. Set `LOWEND_TESTER=1`
for a test build. Needs the separation model in `models/` first — see
`.github/workflows/release.yml` for how CI fetches it, or place it there by hand
(`htdemucs_fwd.onnx` + `htdemucs_fwd.onnx.data` from
[Intel/demucs-openvino](https://huggingface.co/Intel/demucs-openvino), MIT licence).

The macOS release workflow (`.github/workflows/release.yml`) runs on a `v*` tag or
by hand from the Actions tab, and needs the organisation's signing secrets to
produce a signed package — see `docs/DELIVERY-AUDIT-1.0.0.md`.

## Status

Not yet released. See `docs/DELIVERY-AUDIT-1.0.0.md` for the current line-by-line
state against the Amanorsac Studio build and packaging standards.

## Licence

© Amanorsac Studio. Third-party components are listed in `THIRD_PARTY_NOTICES.txt`.
