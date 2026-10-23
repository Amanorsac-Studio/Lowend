# Low End beta — for testers

Thank you for testing Low End 1.1.0. This is a test build: it asks for the test
key you were sent, and it stops working when that key expires.

## Install

Download from the **Low End 1.1.0 (beta)** release on this repository:

| Computer | File |
|---|---|
| Windows | `LowEnd-1.1.0-Tester-Windows.exe` |
| macOS | `LowEnd-1.1.0-Tester-macOS.pkg` |

The first time Low End opens it asks for your key: paste the whole key and choose
Unlock. The key is checked on your computer; Low End never connects to the internet.

The Windows installer of this beta is not code-signed yet, so Windows may show
"Windows protected your PC". Choose **More info › Run anyway**.

## Use one audio app at a time

Your audio interface's own driver (ASIO on Windows) works with one program at a
time. Close other music software before opening Low End. In **Settings › Audio**,
**Lowest safe latency** picks your interface's own driver.

## What to try

- The rig: build a board, change amps and cabinets, save presets and sets.
- The looper: REC waits for your first note; try a 4-bar loop with the click on,
  STOP, then PLAY, and check it comes back in on the "1".
- Learn: import a song, separate it, play along.
- Cover: record yourself over a song and export it.

## Report a problem

Open an issue with the **Bug report** form. Say what you did, what you expected
and what happened, and which computer and interface you use. You can also write
to hello@amanorsac.studio.

## Send us your presets

In Low End, open **Library › Share presets**, choose the presets and add your name
if you want it credited. They are copied to:

- Windows: `Documents\Amanorsac Studio\Low End\Beta Presets\`
- macOS: `~/Documents/Amanorsac Studio/Low End/Beta Presets/`

Attach the files to a **Preset submission** issue, or email them to
hello@amanorsac.studio. Nothing is sent from Low End for you.

## What Low End keeps on your computer

Your presets, sets, songs, loops and recordings are in
`Documents/Amanorsac Studio/Low End/`. Uninstalling does not remove them.
