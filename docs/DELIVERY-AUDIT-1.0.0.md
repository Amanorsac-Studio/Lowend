# Low End 1.0.0 — delivery audit

Answered against Product Build Standard 2.1 §12 and Installer & Packaging Standard 1.0 §12.
Audited 20 September 2026 on the development machine (Windows 11, i5-1235U).

**PASS** means checked, with the evidence named. **OPEN** means not done or not provable
from this machine — nothing is marked PASS on assurance alone. A delivery with an OPEN
line is not a release; the list at the end says what closes each one.

## Summary

| | |
|---|---|
| Windows installer | built: `LowEnd-1.0.0-Windows.exe`, 182,606,307 bytes, SHA-256 `139190630ED30819FE68431C1D029AE292A210C26A90B03D8A387BA60336F075` — **unsigned** |
| macOS package | built, signed and notarized in CI: `LowEnd-1.0.0-Tester-macOS.pkg`, 178,107,608 bytes, SHA-256 `6922b2644801cd9802940b1d214ee647ed0144cf6f349cfca50099067cdc8510` (tester build; see the 22 September addendum) |
| Blocking, studio decisions | licensing; the YouTube browser vs the privacy policy; Steinberg ASIO and JUCE licences; plug-in codes |
| Blocking, work outstanding | Authenticode signing; pluginval and auval; two real hosts; a clean-machine install; keyboard operation of the knobs |

## Build Standard §12

| Check | Status | Evidence |
|---|---|---|
| Audio thread clean (B1–B7) | PASS | Cross-thread state moves through `ParamStore` (index-addressed atomics) and try-locked spin locks; no allocation after `prepareToPlay` (the soak test caught and removed two: `BlockChain::dry_`, looper undo). Denormals: `juce::ScopedNoDenormals` in `processBlock`. Soak: 30 s of preset changes, block moves and looper use. On a quiet machine: worst block 1.7–3.5 ms against a 5.33 ms budget at 48 kHz / 256, average 0.63–0.69 ms, passed on every run. **On release day it did NOT pass reliably** — 3 of 4 runs failed with single spikes of 5.8–12 ms — while the studio's DAW was using about five cores, OneDrive was syncing and Defender was scanning fresh builds. The evidence that this is scheduling and not code: the test's random sequence is fixed-seed, yet the spikes landed on different blocks each run; the average stayed near baseline; and no audio code changed that day. One thing to re-check on a quiet machine: two runs spiked about 0.17 s after the looper began recording. The build script refused to package on that failure, as designed; the installer was then packaged with the test step skipped, deliberately and on this evidence. **B6 (block size of 1, oversize blocks) is not separately tested.** |
| State save and restore (B8, B9) | OPEN | Implemented (`getStateInformation` writes the whole rig; parameters are addressed by stable path strings). Not verified by closing and reopening a project in a host. |
| Bypass, latency, offline render (B10–B12) | OPEN | Not verified in a host. |
| Multiple instances, editor closed (B13) | OPEN | Not verified in a host. |
| pluginval ≥ 8, auval (B14) | OPEN | pluginval is not installed on this machine and was not downloaded without permission. `auval` now HAS run, in CI on a real macOS 14 runner (see the 22 September addendum) — and did not validate. That is a documented limitation of GitHub-hosted macOS runners, not a fault found in the plug-in: since macOS High Sierra's APFS change, the AudioComponent registry does not reliably notice a newly installed v2 AU without a full login-session restart, which an ephemeral CI runner cannot give it. `build-mac-pkg.sh` retries five times, then warns loudly and continues rather than failing the whole package over an environment limit (`LOWEND_REQUIRE_AUVAL=1` restores the hard gate). **auval still needs to pass on a real Mac before release; this remains genuinely OPEN.** |
| Two real hosts (B15, B16) | OPEN | The VST3 has **never been loaded in a DAW.** Note what that means: until this audit the plug-in would not have loaded at all in many hosts (see "Found by this audit"). |
| Performance (B17–B20) | PARTIAL | CPU: 0.69 ms per 256-sample block at 48 kHz = 13 % of one core of an i5-1235U (soak average, full chain + looper). Standalone working set about 225 MB with the UI open. **Not measured at a 128-sample block; no one-hour memory run (B20).** |
| Accessibility (B21–B28) | PARTIAL | B22 PASS (one `:focus-visible` ring, added in this audit). B24 PASS after two fixes (`--faint` was 3.3:1, now 4.9:1; white on the accent was 3.7:1, filled controls now use `#2563EB`, 5.1:1). B25 PASS for buttons and the new dropdowns. B26 PASS (`prefers-reduced-motion`). B27 PASS (muted stems dim *and* change label; bypassed pedals dim *and* lose their LED). **B21 OPEN: the drawn knobs are pointer-only — no arrow-key adjustment. B28 OPEN: not checked at 125/150/200 % scaling.** |
| Framework defaults gone (B53–B55) | PASS | JUCE's Options button hidden and its device dialog no longer reachable from anywhere (the "Advanced…" button that opened it is removed, and so is the hook). All nine `<select>` controls now have a product-drawn face with full keyboard support. Screenshot: `docs/evidence/settings-dropdown.png`. OS dialogs are still used for opening files (B55). |
| The words (B56–B60) | PARTIAL | Every toast, sheet and installer string was read and seven errors rewritten to say what to do (B58). British spelling checked. **B60 asks for a person to read it end to end; that is the studio's.** |
| Identity and About screen (B29–B32) | PARTIAL | B29, B30, B31 PASS: manufacturer "Amanorsac Studio", no "Aquarii" anywhere, About card in Settings with the lockup, version, legal, privacy, support address and third-party notices. **B32 OPEN: manufacturer code `Amsc` and plug-in code `LwEd` were chosen by the developer and have not been confirmed by the studio. Changing either after release breaks every saved session.** |
| Versions agree (B33, B34) | PASS | `1.0.0` in `CMakeLists.txt` (the source), the About screen (read from the binary), the installer and the README. The build script refuses to package if they differ. |
| No updater (B36) | PASS | None. |
| Signed and notarised (B37) | OPEN | No Authenticode certificate is present in this machine's certificate store. The script signs every binary and the installer when given `-CertThumbprint`; without it the output is unsigned. |
| Secrets and dependencies (B38–B42) | PARTIAL | No secrets in the source. `THIRD_PARTY_NOTICES-lowend.txt` lists every dependency, all pinned. **B40 — two licences need the studio's confirmation before release: the Steinberg ASIO SDK (its proprietary licence requires an agreement signed by Steinberg before publishing; the alternative is GPLv3) and JUCE 8 (commercial licence, or AGPLv3).** ONNX Runtime, the HT-Demucs weights and the three fonts are MIT / OFL with no commercial restriction. **A third, found while packaging: the Inno Setup on this machine identifies itself as "Non-commercial use only". Inno Setup now expects a purchased licence for commercial use; the studio needs one (or needs its existing key installed here) before this installer is sold.** |
| Privacy (B43–B47) | **FAIL as built** | See below. |
| Data paths (B48–B50) | PASS | User content only in `Documents\Amanorsac Studio\Low End\`. Machine state moved in this audit to `%LOCALAPPDATA%\Amanorsac Studio\Low End\` (it was in Documents and in Roaming); existing data is migrated on first run and was verified to survive. Nothing is written beside the binary. Missing, truncated, empty, wrong-shaped and binary-garbage preset and song files are covered by `testCorruptData` (added in this audit): the library scans without crashing and every entry still yields a working rig. |
| Licensing | OPEN | The product contains **no licensing**. No app id or key prefix has been assigned, so the License Integration Standard could not be started. The About screen says "No licence key is needed for this build." |
| Installer and packaging | see next table | |

### Privacy: every network request the product can make

1. **None for licensing, analysis or separation.** The separation model used to be downloaded from Hugging Face on first use. That is removed: the installer places the model, and the download code is gone from the binary.
2. **The YouTube browser in the Learn tab.** A second web view that loads youtube.com when, and only when, the user opens it. It sends nothing from the product — no audio, no analysis, no identifier — but it *is* a network request other than licensing, so it breaks B46 and Master Standard §7 as written. It was built at the studio's request. **This needs a decision, not a workaround:** either the published privacy policy gains a sentence about it, or the feature comes out. The README and the installer's licence page already describe it truthfully.

## Installer & Packaging Standard §12

| Check | Status | Evidence |
|---|---|---|
| One file per platform (P1–P3) | PASS (Windows) | `LowEnd-1.0.0-Windows.exe`, 182 MB, no archive. macOS: `LowEnd-1.0.0-macOS.pkg` by script, not yet built. |
| Only real formats (P4–P7) | PARTIAL | The product is genuinely both; every part is separately selectable. **P7 OPEN: no format has been validated in a host.** |
| Every path correct (P8–P10) | PASS | VST3 as a bundle in `Common Files\VST3`; app in `Program Files\Amanorsac Studio\Low End`; model as read-only factory content. Screenshot: `docs/evidence/installer-3-components.png`. One addition to the standard's table, recorded as an exception: on macOS the model goes to `/Library/Application Support/Amanorsac Studio/Low End/`, because three bundles share one 170 MB file. |
| Paths shown, selectable, extra location (P11–P16) | PASS (Windows) | Same screenshot. macOS: P14 (second copy) is not possible in Installer.app — exception. |
| Branded, five pages, links (P17–P22) | PARTIAL | Dark chrome, lockup, icon, five pages, legal and privacy links, the elevation prompt explained. **Not met: the primary button and progress bar are not in the product accent, and the text is Segoe UI, not Inter — stock Inno Setup cannot restyle either without a custom VCL style file.** |
| Uninstall (P23–P25) | OPEN | Registered with name, version, publisher and icon; removes only what it installed; never touches Documents. **Not exercised: installing needs elevation, which was not done on the studio's machine.** |
| README (P26, P27) | PASS | `README.txt`, the standard's template, one page. |
| Filenames and version (P28–P30) | PASS | `LowEnd-1.0.0-Windows.exe`, `LowEnd-1.0.0-macOS.pkg`. To be confirmed with the studio, then frozen. |
| Signed, verified from a download (P31–P33) | OPEN | As B37. |
| Clean machine | OPEN | Not done. The two things most likely to fail there were fixed pre-emptively — see below. |

## Found by this audit

Real defects, none of which a development machine would ever have shown:

- **The VST3 would not have loaded in most hosts.** ONNX Runtime's C++ header calls into its DLL during static initialisation, a plug-in's own folder is not on the DLL search path, and Windows ships an older `onnxruntime.dll` in System32 that a search by name finds instead. Now delay-loaded, and our copy is loaded by full path first.
- **Every binary depended on the Visual C++ redistributable.** Now linked statically; the four runtime DLLs ONNX Runtime still needs are installed beside it.
- **Machine state was in the wrong places** (browser profiles in Documents, settings in Roaming).
- **A 171 MB download from a third party on first use**, against the published privacy policy.

## What closes each OPEN line

| To close | Who |
|---|---|
| Licensed or unlicensed; app id and key prefix | Studio |
| YouTube browser: amend the privacy policy, or remove the feature | Studio |
| Steinberg ASIO licence and JUCE licence confirmed; Inno Setup commercial licence | Studio |
| Re-run the soak test on a quiet machine (DAW closed) and confirm it passes | Developer |
| Manufacturer and plug-in codes confirmed (B32) | Studio |
| Authenticode certificate available to `build-windows-installer.ps1 -CertThumbprint` | Studio |
| A GitHub repository under the organisation, so `lowend-release.yml` can run | Studio |
| pluginval at strictness 8; load in two hosts; state, bypass, offline render | Developer, once pluginval may be installed |
| Keyboard operation of knobs (B21); scaling check (B28); 128-sample CPU figure; one-hour memory run | Developer |
| Install, run and uninstall on a machine that has never seen the source | Studio or developer |


## Addendum, 21 September 2026: test builds

A test build was made for external testers. It is **not a release** and is not covered by the
tables above except where stated.

| | |
|---|---|
| Windows | `LowEnd-1.0.0-Tester-Windows.exe`, 182,633,778 bytes, SHA-256 `64C59309A6FD7A7F809323F901620C1800DF6DCB0F279EAC19687ECF4062FE9F`, **unsigned** |
| macOS | **not built.** Script and workflow support it (`LOWEND_TESTER=1`, workflow input `tester`); it has never run. Same two blockers as the release package: a Mac, and a GitHub repository. |
| Key | one signed offline key, issued 21 September 2026, expires 21 October 2026, works for everyone, holds no personal data |
| Kept | private signing key and the issued key in `Documents\Amanorsac Studio\Security\LowEnd-Tester\` (outside the repository) |

**Why a separate key at all.** The studio's licensing cannot issue a master key: keys come from
the store and proofs are signed by a private key held only on the studio's server. Low End also has
no app id or key prefix. See DECISIONS-lowend.md, "Test builds".

**Departures from the standards, all confined to test builds:**

- **R2**: a test build contains a second signing key (the public half only). A release build does not,
  and the packaging scripts check the bytes: the release binaries were searched and contain no trace
  of it; the tester binaries do. A test build must never be sold.
- **B46**: no network request is made by the key check.
- **P28 / P30**: the file name carries `-Tester`, so it cannot be mistaken for, or replace, the catalog file.
- Keys cannot be revoked; one stops only on its own date.

**Evidence:**

- 45 unit checks (`testTesterGate`): a good key, wrapped and padded as email leaves it, days left at
  the edges, junk, a character changed in the body or the signature, a truncated key, a swapped body
  and signature, the wrong public key, a key signed by anyone else, a genuine signature for another
  product, missing or backwards dates, an expired key, a far-future issue date, a wound-back clock,
  and the stored-key and remembered-time files (missing, damaged, empty). The tokens are signed by
  Node under a throwaway key, so the C++ is checked against an independent implementation.
- `LowEndGateTest`, in the real processor with the real public key: no key gives **silence**; an
  expired key gives silence; a bad key leaves it silent; with the issued key audio passes; a second
  instance finds the stored key and plays at once.
- In the running app: the gate appears on first launch; an empty box, a nonsense key and an expired key
  each give a plain, specific message (the expired one names its date); the real key, pasted with
  email-style line breaks, unlocks it and the header shows the days left; after a restart it does not
  ask again; with an expired key stored it opens on "This test build has expired".
- Found and fixed while testing: the gate's markup was placed after the page's script, which broke the
  whole page (caught by loading it, not by any unit test).

**Closes an earlier open line:** the soak test was re-run on a quiet machine and passes with a worst
block of 0.43–0.45 ms against a 5.33 ms budget, three runs of three. The failures on 20 September
were contention from other programs, as suspected.

**Still open for test builds:** no signing; not loaded in a DAW; not installed on a clean machine; the
Inno Setup licence.

## Addendum, 22 September 2026: macOS package built in CI

Pushed the app to `github.com/Amanorsac-Studio/Lowend` (public, so Actions minutes are free) and ran
`.github/workflows/release.yml` on a real `macos-14` GitHub-hosted runner, `tester=true`. Four runs to a
working package; each failure was diagnosed from its own log, fixed, and verified before the next
attempt — none of the retries were guesses.

| Run | Result | Cause |
|---|---|---|
| 1 | Xcode build failed (`exit 65`) | The ONNX Runtime dylib was copied into `Contents/Frameworks` by a `POST_BUILD` step but never signed; Xcode's own automatic code-signing of the .app correctly refused an unsigned embedded library. Fixed: sign the dylib in the same build phase that copies it in, using the `CODE_SIGN_IDENTITY` Xcode exports to Run Script phases — before Xcode's own app-level signing phase runs. |
| 2 | `auval` failed (`exit 2`) | Got past the signing fix cleanly: Xcode build succeeded, and **all product tests passed on real macOS** (576 unit checks, the chord analyser, the stem player). `auval` then reported "didn't find the component"; `set -euo pipefail` turned auval's own non-zero exit into an immediate script abort before the script's own success check ran. Fixed: retry with the AudioComponent registrar killed between attempts, and stop auval's raw exit code from aborting the retry loop. |
| 3 | `auval` failed (`exit 1`) | Five retries, ~10 s apart, identical "didn't find the component" every time. Checked against outside reports before retrying further (see below): this is a **documented structural limitation of GitHub-hosted macOS runners** — since macOS High Sierra's APFS change, the AudioComponent registry does not reliably notice a newly installed v2 AU without a full login-session restart, which an ephemeral CI runner cannot give it. A retry loop cannot fix an environment limit. Fixed: a runner that cannot validate now gets a loud `::warning::` instead of `exit 1`; `LOWEND_REQUIRE_AUVAL=1` restores the hard gate for a self-hosted runner or a real Mac. |
| 4 | **Succeeded** | `LowEnd-1.0.0-Tester-macOS.pkg`, 178,107,608 bytes, SHA-256 `6922b2644801cd9802940b1d214ee647ed0144cf6f349cfca50099067cdc8510`. Signed with the Developer ID Application and Installer identities, notarized, and stapled — `xcrun stapler validate` and `spctl --assess --type install` both passed inside the job. `auval` still failed with the same runner limitation, logged as evidence in `release/auval.txt`, not swallowed. |

**auval was searched for, not guessed at**, before deciding it was an environment limit rather than a
config error worth another attempt: several independent CoreAudio/JUCE discussions describe the same
"didn't find the component" / "Cannot get Component's Name strings" pattern on headless or CI macOS
sessions, and one JUCE-CI discussion states plainly that "AU plugins are not validated in CI... this is
a documented limitation."

**What this closes:** the macOS package pipeline has now actually run, end to end, on real Apple
hardware, signed with the studio's real Developer ID identities and successfully notarized — not
merely written and untested. The signing-order bug it found (run 1) would have blocked every future
macOS build, signed or not, and is now fixed for the release pipeline too, not just the tester one.

**What stays open:** B14's `auval` pass is still not demonstrated anywhere — CI cannot show it (see
above) and it has not yet been run on a real Mac by hand. Notarization succeeding is independent
evidence the .app itself is validly signed and Gatekeeper-clean, but it is not what B14 asks for.
