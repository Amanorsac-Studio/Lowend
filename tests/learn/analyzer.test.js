// analyzer.test.js — checks the Learn tab's song analyser against progressions
// whose answer is known, because they are synthesized here.
//
//   node apps/lowend/tests/learn/analyzer.test.js
//
// Notes are rendered as harmonic stacks with a pluck envelope, which is closer
// to a real instrument than sine waves and — deliberately — gives every note an
// overtone series for the overtone suppression and the chord model to cope with.
'use strict';
const path = require('path');
const { analyze, SR } = require(path.join(__dirname, '..', '..', 'resources', 'ui', 'analyzer.js'));

let checks = 0, failures = 0;
const check = (ok, what) => { checks++; if (!ok) { failures++; console.log('  FAIL  ' + what); } };
const hz = p => 440 * Math.pow(2, (p - 69) / 12);

/** Render [{p, s, e, a}] (MIDI pitch, start s, end s, amplitude) to mono PCM. */
function render (events, seconds, seed = 1) {
  const out = new Float32Array(Math.ceil(seconds * SR));
  let rnd = seed;
  const noise = () => { rnd = (rnd * 16807) % 2147483647; return rnd / 2147483647 - 0.5; };
  for (const ev of events) {
    const f0 = hz(ev.p), a = ev.a || 0.2;
    const s0 = Math.floor(ev.s * SR), s1 = Math.min(out.length, Math.floor(ev.e * SR));
    for (let h = 1; h <= 8; h++) {
      const fh = f0 * h;
      if (fh > SR / 2 - 200) break;
      const ah = a / Math.pow(h, 1.1);
      const ph = noise() * 6;
      for (let i = s0; i < s1; i++) {
        const t = (i - s0) / SR;
        const env = Math.min(1, t / 0.01) * Math.exp(-t * (0.9 + 0.35 * h)) * Math.min(1, (s1 - i) / (0.02 * SR));
        out[i] += ah * env * Math.sin(2 * Math.PI * fh * t + ph);
      }
    }
  }
  for (let i = 0; i < out.length; i++) out[i] += 0.002 * noise();   // a little room noise
  return out;
}

/** A chord: bass note + three voicing notes, re-struck every beat for `len` s. */
function chord (bassP, voicing, s, len, beat = 0.5) {
  const ev = [];
  for (let t = s; t < s + len - 0.01; t += beat) {
    ev.push({ p: bassP, s: t, e: t + beat, a: 0.35 });
    for (const p of voicing) ev.push({ p, s: t, e: t + beat, a: 0.16 });
  }
  return ev;
}

const nameAt = (res, t) => { const c = res.chords.find(c => t >= c.s && t < c.e); return c ? c.name : '—'; };

//------------------------------------------------------------------ 1. I–vi–IV–V in C
{
  console.log('I–vi–IV–V in C');
  const ev = [
    ...chord(36, [60, 64, 67], 0, 2),    // C   : C2 | C4 E4 G4
    ...chord(33, [57, 60, 64], 2, 2),    // Am  : A1 | A3 C4 E4
    ...chord(41, [57, 60, 65], 4, 2),    // F   : F2 | A3 C4 F4
    ...chord(43, [59, 62, 67], 6, 2),    // G   : G2 | B3 D4 G4
  ];
  const t0 = Date.now();
  const res = analyze(render(ev, 8));
  console.log('  ' + ((Date.now() - t0) / 1000).toFixed(2) + ' s for 8 s of audio; chords: ' + res.chords.map(c => c.name + '@' + c.s.toFixed(1)).join('  '));
  check(nameAt(res, 1.0) === 'C', 'C at 1.0 s (got ' + nameAt(res, 1.0) + ')');
  check(nameAt(res, 3.0) === 'Am', 'Am at 3.0 s (got ' + nameAt(res, 3.0) + ')');
  check(nameAt(res, 5.0) === 'F', 'F at 5.0 s (got ' + nameAt(res, 5.0) + ')');
  check(nameAt(res, 7.0) === 'G', 'G at 7.0 s (got ' + nameAt(res, 7.0) + ')');
  check(res.chords.length <= 6, 'the chord line does not flicker (' + res.chords.length + ' segments)');
  check(res.key.root === 0 && res.key.mode === 'major', 'key is C major (got ' + res.key.name + ')');

  // The bass line: each chord's root should be found as a bass-role note.
  const bassAt = t => res.notes.filter(n => n.role === 'bass' && t >= n.s && t < n.e).map(n => n.p);
  check(bassAt(1.2).includes(36), 'bass C2 under the C (got ' + bassAt(1.2) + ')');
  check(bassAt(3.2).includes(33), 'bass A1 under the Am (got ' + bassAt(3.2) + ')');
  check(bassAt(5.2).includes(41), 'bass F2 under the F (got ' + bassAt(5.2) + ')');
  check(bassAt(7.2).includes(43), 'bass G2 under the G (got ' + bassAt(7.2) + ')');
}

//------------------------------------------------------------------ 2. minor key, sevenths, flats
{
  console.log('iv–V–i in G minor');
  const ev = [
    ...chord(36, [60, 63, 67], 0, 2),    // Cm  : C2 | C4 Eb4 G4
    ...chord(38, [60, 66, 69], 2, 2),    // D7  : D2 | C4 F#4 A4
    ...chord(43, [58, 62, 67], 4, 3),    // Gm  : G2 | Bb3 D4 G4
  ];
  const res = analyze(render(ev, 7, 7));
  console.log('  chords: ' + res.chords.map(c => c.name + '@' + c.s.toFixed(1)).join('  ') + '   key: ' + res.key.name);
  check(nameAt(res, 1.0) === 'Cm', 'Cm at 1.0 s (got ' + nameAt(res, 1.0) + ')');
  check(/^D7?$/.test(nameAt(res, 3.0)), 'D or D7 at 3.0 s (got ' + nameAt(res, 3.0) + ')');
  check(nameAt(res, 5.5) === 'Gm', 'Gm at 5.5 s (got ' + nameAt(res, 5.5) + ')');
  check(res.key.root === 7 && res.key.mode === 'minor', 'key is G minor (got ' + res.key.name + ')');
}

//------------------------------------------------------------------ 3. a melody over chords
{
  console.log('Melody over held chords');
  const ev = [
    ...chord(36, [55, 60, 64], 0, 3, 1.5),               // C, voiced low
    { p: 76, s: 0.0, e: 0.7, a: 0.3 },                    // E5
    { p: 74, s: 0.75, e: 1.45, a: 0.3 },                  // D5
    { p: 72, s: 1.5, e: 2.2, a: 0.3 },                    // C5
    { p: 79, s: 2.25, e: 2.95, a: 0.3 },                  // G5
  ];
  const res = analyze(render(ev, 3, 3));
  const melAt = t => res.notes.filter(n => n.role === 'melody' && t >= n.s && t < n.e).map(n => n.p);
  check(melAt(0.35).includes(76), 'melody E5 at 0.35 s (got ' + melAt(0.35) + ')');
  check(melAt(1.1).includes(74), 'melody D5 at 1.1 s (got ' + melAt(1.1) + ')');
  check(melAt(1.85).includes(72), 'melody C5 at 1.85 s (got ' + melAt(1.85) + ')');
  check(melAt(2.6).includes(79), 'melody G5 at 2.6 s (got ' + melAt(2.6) + ')');
}

//------------------------------------------------------------------ 4. solo bass line (a stem)
{
  console.log('Solo bass line, eighth notes at 120 bpm');
  const line = [28, 28, 31, 33, 35, 33, 31, 28, 40, 38, 36, 35];   // E1 E1 G1 A1 B1 A1 G1 E1 E2 D2 C2 B1
  const ev = line.map((p, i) => ({ p, s: i * 0.25, e: i * 0.25 + 0.24, a: 0.5 }));
  const res = analyze(render(ev, 3.2, 11));
  let hit = 0;
  line.forEach((p, i) => {
    const t = i * 0.25 + 0.14;
    if (res.notes.some(n => n.p === p && t >= n.s && t < n.e)) hit++;
  });
  console.log('  ' + hit + ' / ' + line.length + ' notes found at their time');
  check(hit >= line.length - 2, 'a solo bass line is transcribed note for note (' + hit + '/' + line.length + ')');
}

//------------------------------------------------------------------ 4b. with drums
{
  console.log('I–vi–IV–V in C with a drum kit on top');
  const ev = [
    ...chord(36, [60, 64, 67], 0, 2), ...chord(33, [57, 60, 64], 2, 2),
    ...chord(41, [57, 60, 65], 4, 2), ...chord(43, [59, 62, 67], 6, 2),
  ];
  const pcm = render(ev, 8, 21);
  // Kick on every beat (a fast downward sweep), closed hi-hat on every eighth
  // (a short burst of bright noise) — broadband energy the chord model must ignore.
  let rnd = 99;
  const noise = () => { rnd = (rnd * 16807) % 2147483647; return rnd / 2147483647 - 0.5; };
  for (let t = 0; t < 8; t += 0.25) {
    const s0 = Math.floor(t * SR);
    for (let i = 0; i < 0.04 * SR && s0 + i < pcm.length; i++) pcm[s0 + i] += 0.25 * noise() * Math.exp(-i / (0.008 * SR));
    if (Math.abs((t * 2) % 1) < 1e-9) {
      let ph = 0;
      for (let i = 0; i < 0.25 * SR && s0 + i < pcm.length; i++) {
        const f = 50 + 110 * Math.exp(-i / (0.03 * SR));
        ph += 2 * Math.PI * f / SR;
        pcm[s0 + i] += 0.6 * Math.sin(ph) * Math.exp(-i / (0.09 * SR));
      }
    }
  }
  const res = analyze(pcm);
  console.log('  chords: ' + res.chords.map(c => c.name + '@' + c.s.toFixed(1)).join('  ') + '   key: ' + res.key.name);
  const ok = ['C', 'Am', 'F', 'G'].filter((n, i) => nameAt(res, 2 * i + 1) === n).length;
  check(ok >= 3, 'at least 3 of 4 chords survive a drum kit (' + ok + '/4)');
  check(res.key.root === 0 && res.key.mode === 'major', 'key is still C major with drums (got ' + res.key.name + ')');
}

//------------------------------------------------------------------ 5. silence
{
  console.log('Silence');
  const res = analyze(new Float32Array(SR * 2));
  check(res.chords.length === 0, 'silence has no chords');
  check(res.notes.length === 0, 'silence has no notes');
}

//------------------------------------------------------------------ 6. speed
{
  console.log('Speed');
  const ev = [];
  for (let k = 0; k < 15; k++) ev.push(...chord(36 + (k % 5), [60, 64, 67], k * 4, 4));
  const t0 = Date.now();
  analyze(render(ev, 60, 5));
  const s = (Date.now() - t0) / 1000;
  console.log('  60 s of audio analysed in ' + s.toFixed(1) + ' s');
  check(s < 30, 'a minute of audio analyses in well under a minute (' + s.toFixed(1) + ' s)');
}

console.log('\n' + checks + ' checks, ' + failures + ' failures');
process.exit(failures ? 1 : 0);
