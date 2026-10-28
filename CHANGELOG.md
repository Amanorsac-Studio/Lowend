# Low End — what changed

## 1.1.0 (beta), Beta Test 3

**A real pedalboard**
- Pedals look like pedals: our own enclosures in powder coat, hammertone, brushed metal, sparkle and wood, with knobs you turn on the board itself.
- Drag a pedal to move it, across the amp too, or onto the bin to remove it. A full board goes onto two tiers.
- A pedal browser opens over the amp: drag a pedal from it onto the board.

**Presets**
- 115 factory presets in 13 styles: Worship, Clean, Drive, Rock, Funk, Slap, Metal, Gospel, Reggae, Jazz, Synth, Ambient and Strings. An update adds new ones without bringing back any you deleted.
- Make presets in the Library: the rig moves in, so you can build a sound without leaving the page.
- Preset packs: Library › Make a pack saves presets as one .lepack file to share. Drop a pack on the app to install it; every pack is checked first and holds sounds only.
- Tap a preset in the preset list to see its pedals before you load it.

**Strings**
- The Bow pedal turns the bass into a bowed string: notes swell in, hold, and take a player's vibrato. Cello, double bass or viola.
- Section mode bows whatever you play; Solo mode follows a single line and plays it on the string.

**Your Amp**
- The Your Amp pedal plays your own Neural Amp Modeler captures (.nam). Drop a .nam file on the app to add it. Each capture is checked, and timed on your computer, before it is used. It switches the built-in amp off, and the cab too if you want.

**Fixes**
- The Library no longer shakes while it is open.
- Library › Captures › Use on this rig now picks Your IR (it picked the 8×10).

## 1.1.0 (beta)

**Sound**
- Bypass All, on a button, a footswitch and the X key: every pedal, the amp and the cabinet out at once, so you hear the bass as it is.
- Pedals now sound one after another, as on a real board. Drives that blend in the clean signal keep it in time with the driven one.
- Silence stays silent: the noise gate follows the noise your input actually has. A hum filter (50 or 60 Hz) is in Settings › Audio › Noise.

**Presets**
- An edit is kept when you change preset, and kept if Low End closes. When you close it, Low End asks whether to save each edit to the preset or to the set only, or to discard it.
- Library › Make a pack saves the presets you choose as one .lepack file, to send back to us or share with other players.

**Audio**
- Every audio interface on every driver in one list, refreshed when one is plugged in or out.
- The input channel, the output pair, mono or stereo, and a separate pair for the click.
- Lowest safe latency picks your interface's own driver and never a generic one that cannot reach it, and says so when another program is using the interface.

**Looper**
- REC waits for your first note and the loop starts on that bar, so the silence before you come in is not recorded.
- Loop length: free, or 1, 2, 4, 8 or 16 bars. A set length closes the loop by itself.
- The loop keeps to the click: it starts on the click's bar, and PLAY after STOP comes back in on the next "1".
- STOP during a take finishes the bar, then stops.
- Time signatures from 2/4 to 12/8. The "1" of the bar is an octave higher than the other beats.

**Learn**
- Chords are shown per bar or per beat.
- Song timing: set where the song starts and ends, mark a free-time intro and tempo changes, and drag the audio under the beat lines.
- The fretboard shows what you play, at one position, and fades it after you let go.
- YouTube now shows in the Learn tab.

**Cover**
- A new tab: play along to a song, record yourself over it with the whole rig, punch in on a section, and export the mix, your bass and your clean DI as WAV files.

**Fixed**
- Low End no longer freezes when another Amanorsac app opens at the same time.
- A loop stopped just after the bar line stays in time with the click.


## 1.0.0

The first release.

**The rig**
- Four bass amps, five cabinets with microphone choice, and your own impulse responses.
- A pedalboard you build yourself: gate, compressors, overdrive, fuzz, filters, chorus, delay, reverb and more, in any order, before or after the amp.
- A synth voice that follows the note you are playing.
- A four-layer looper with undo.
- Presets, and sets that step through them on stage. Changing preset never cuts the sound.
- A new preset starts with a bare board: amp, cabinet, nothing else.

**On stage**
- The Stage page shows your set, the looper, the output meters, and the board of whichever preset is live.
- Footswitches over MIDI, and MIDI learn on every control.
- A tuner that holds its reading after the note dies away.

**Learn**
- Import a song or a video and Low End writes out the chords, the key and the notes, on a piano roll and a bass fretboard.
- Stem separation: split the song into drums, bass, vocals and the rest, on your own computer. Mute the bass and play along, or listen to one part alone.
- Slow a song down without changing its pitch, and loop any section.
- A YouTube browser with the same speed and loop controls.

**Settings**
- Audio device settings inside the app: driver, input, output, sample rate and buffer, with the latency shown in milliseconds, and a Lowest latency button.
- About, with the licence, privacy and third-party notices.

**Formats**
- Windows: standalone application and VST3.
- macOS: standalone application, VST3 and Audio Unit.
