// analyzer.js — Low End "Learn": listens to a song and writes down what it hears.
//
// Input : mono PCM (Float32Array) at 22 050 Hz.
// Output: { duration, key, chords:[{s,e,name,root,q,bass}], notes:[{s,e,p,v,role}] }
//
//   chords — segments named from a chromagram against chord templates, smoothed
//            with a Viterbi pass so the name does not flicker on every beat.
//   notes  — an approximate transcription: per-frame multi-pitch salience by
//            harmonic summation, overtone-suppressed, peak-picked and tracked
//            into notes. Each note is tagged "bass" (the lowest line), "melody"
//            (the top line) or "other".
//   key    — Krumhansl–Kessler profile correlation over the whole song.
//
// It runs in a Web Worker inside the app (so the UI never stalls) and under
// Node for the tests (tests/learn/analyzer.test.js). No dependencies.
//
// HONEST LIMITS. Chord templates on a chromagram are the classic method: good
// on clear recordings, confused by heavy distortion, dense arrangements and
// jazz voicings. Polyphonic note transcription from a full mix is an open
// research problem; what this produces is a useful guide to follow along
// with, not a score. Both get much better on an isolated stem.
'use strict';

const SR = 22050;
const HOP = 1024;          // 46.4 ms
const NH = 4096;           // 186 ms — mids and highs
const NL = 8192;           // 372 ms — separates low bass notes on their harmonics, without smearing a fast line
const P_LO = 28, P_HI = 96; // E1 .. C7
const SPLIT = 52;          // pitches below this read the long window
const BASS_TOP = 55;       // G3, the 12th fret of a bass's G string: the bass register ends here

//------------------------------------------------------------------ FFT
function makeFFT (n) {
  const levels = Math.log2(n) | 0;
  const cos = new Float64Array(n / 2), sin = new Float64Array(n / 2);
  for (let i = 0; i < n / 2; i++) { cos[i] = Math.cos(2 * Math.PI * i / n); sin[i] = Math.sin(2 * Math.PI * i / n); }
  const rev = new Uint32Array(n);
  for (let i = 0; i < n; i++) {
    let x = i, r = 0;
    for (let b = 0; b < levels; b++) { r = (r << 1) | (x & 1); x >>>= 1; }
    rev[i] = r;
  }
  const win = new Float64Array(n);
  for (let i = 0; i < n; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (n - 1));
  const re = new Float64Array(n), im = new Float64Array(n);
  const mag = new Float64Array(n / 2);
  // Magnitude spectrum of pcm[start .. start+n), Hann-windowed; zero-padded at the ends.
  return function spectrum (pcm, start) {
    for (let i = 0; i < n; i++) {
      const j = start + i;
      const s = (j >= 0 && j < pcm.length) ? pcm[j] : 0;
      const k = rev[i];
      re[k] = s * win[i]; im[k] = 0;
    }
    for (let size = 2; size <= n; size <<= 1) {
      const half = size >> 1, step = n / size;
      for (let i = 0; i < n; i += size) {
        for (let j = i, k = 0; j < i + half; j++, k += step) {
          const tr = re[j + half] * cos[k] + im[j + half] * sin[k];
          const ti = -re[j + half] * sin[k] + im[j + half] * cos[k];
          re[j + half] = re[j] - tr; im[j + half] = im[j] - ti;
          re[j] += tr; im[j] += ti;
        }
      }
    }
    for (let i = 0; i < n / 2; i++) mag[i] = Math.sqrt(re[i] * re[i] + im[i] * im[i]);
    return mag;
  };
}

const hz = p => 440 * Math.pow(2, (p - 69) / 12);

/** Peak magnitude within ±half a semitone of `f`, from a spectrum of size n. */
function peakAt (mag, n, f) {
  const lo = Math.max(1, Math.floor(f * Math.pow(2, -1 / 24) * n / SR));
  const hi = Math.min(mag.length - 1, Math.ceil(f * Math.pow(2, 1 / 24) * n / SR));
  let m = 0;
  for (let b = lo; b <= hi; b++) if (mag[b] > m) m = mag[b];
  return m;
}

//------------------------------------------------------------ chord model
const QUAL = [
  { q: 'maj',  iv: [0, 4, 7],     suffix: '',     prior: 0 },
  { q: 'min',  iv: [0, 3, 7],     suffix: 'm',    prior: 0 },
  { q: '7',    iv: [0, 4, 7, 10], suffix: '7',    prior: -0.025 },
  { q: 'maj7', iv: [0, 4, 7, 11], suffix: 'maj7', prior: -0.025 },
  { q: 'm7',   iv: [0, 3, 7, 10], suffix: 'm7',   prior: -0.025 },
  { q: 'sus4', iv: [0, 5, 7],     suffix: 'sus4', prior: -0.07 },   // kicks and toms leak into the 4th
  { q: 'sus2', iv: [0, 2, 7],     suffix: 'sus2', prior: -0.07 },
  { q: 'dim',  iv: [0, 3, 6],     suffix: 'dim',  prior: -0.05 },
  { q: 'aug',  iv: [0, 4, 8],     suffix: 'aug',  prior: -0.06 },
];
const SHARP = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const FLAT  = ['C', 'Db', 'D', 'Eb', 'E', 'F', 'Gb', 'G', 'Ab', 'A', 'Bb', 'B'];

const KK_MAJOR = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88];
const KK_MINOR = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17];

/** Flat-side keys spell with flats (F major writes Bb, not A#). */
function usesFlats (root, mode) {
  const majorFlat = [5, 10, 3, 8, 1, 6];    // F Bb Eb Ab Db Gb
  const minorFlat = [2, 7, 0, 5, 10, 3];    // Dm Gm Cm Fm Bbm Ebm
  return mode === 'minor' ? minorFlat.includes(root) : majorFlat.includes(root);
}

function pearson (a, b) {
  let ma = 0, mb = 0;
  for (let i = 0; i < 12; i++) { ma += a[i]; mb += b[i]; }
  ma /= 12; mb /= 12;
  let n = 0, da = 0, db = 0;
  for (let i = 0; i < 12; i++) { const x = a[i] - ma, y = b[i] - mb; n += x * y; da += x * x; db += y * y; }
  return da > 0 && db > 0 ? n / Math.sqrt(da * db) : 0;
}

function detectKey (chromaSum) {
  let best = { root: 0, mode: 'major', score: -2 };
  for (let r = 0; r < 12; r++) {
    const rot = i => chromaSum[(i + r) % 12];
    const v = Array.from({ length: 12 }, (_, i) => rot(i));
    const sM = pearson(v, KK_MAJOR), sm = pearson(v, KK_MINOR);
    if (sM > best.score) best = { root: r, mode: 'major', score: sM };
    if (sm > best.score) best = { root: r, mode: 'minor', score: sm };
  }
  const names = usesFlats(best.root, best.mode) ? FLAT : SHARP;
  return { root: best.root, mode: best.mode, name: names[best.root] + (best.mode === 'minor' ? ' minor' : ' major'),
           confidence: Math.max(0, Math.min(1, best.score)) };
}

const sameChord = (a, b) => a.root === b.root && a.qi === b.qi && a.bass === b.bass;

/**
 * Key from the chord progression, with the chroma profile as a tie-breaker.
 *
 * A pitch-class profile on its own is easily fooled by overtones — every note
 * adds its own fifth and major third — and it cannot tell a major key from its
 * relative minor, which share every note. The chords can: C–Am–F–G is diatonic
 * to both C major and A minor, but it STARTS on C and C gets the time, and that
 * is what a musician hears as the key. So each key is scored by how much of the
 * song's time is spent on its diatonic chords, with extra weight for time on
 * its tonic and for opening or closing on it.
 */
function detectKeyFromChords (chords, chromaSum) {
  const family = q => (q === 'min' || q === 'm7') ? 'min' : (q === 'dim') ? 'dim' : (q === 'sus4' || q === 'sus2') ? 'sus' : 'maj';
  const fits = (deg, fam, mode) => {
    // Diatonic triads by scale degree (semitones above the tonic). The minor
    // side includes the harmonic-minor V, because that is what minor songs use.
    const M = { 0: 'maj', 2: 'min', 4: 'min', 5: 'maj', 7: 'maj', 9: 'min', 11: 'dim' };
    const m = { 0: 'min', 2: 'dim', 3: 'maj', 5: 'min', 7: ['min', 'maj'], 8: 'maj', 10: 'maj' };
    const t = (mode === 'major' ? M : m)[deg];
    if (t === undefined) return false;
    if (fam === 'sus') return true;
    return Array.isArray(t) ? t.includes(fam) : t === fam;
  };
  const total = chords.reduce((s, c) => s + (c.e - c.s), 0) || 1;
  let best = null;
  for (let r = 0; r < 12; r++) {
    for (const mode of ['major', 'minor']) {
      let s = 0;
      chords.forEach((c, i) => {
        const d = c.e - c.s, deg = (c.root - r + 12) % 12, fam = family(c.q);
        if (!fits(deg, fam, mode)) return;
        s += d;
        const tonic = deg === 0 && fam === (mode === 'major' ? 'maj' : 'min');
        if (tonic) { s += 0.8 * d; if (i === 0) s += 0.5 * total / 8; if (i === chords.length - 1) s += 0.3 * total / 8; }
      });
      // Profile correlation as a small tie-breaker.
      const v = Array.from({ length: 12 }, (_, i) => chromaSum[(i + r) % 12]);
      s = s / total + 0.15 * pearson(v, mode === 'major' ? KK_MAJOR : KK_MINOR);
      if (!best || s > best.score) best = { root: r, mode, score: s };
    }
  }
  if (!chords.length) return detectKey(chromaSum);
  const names = usesFlats(best.root, best.mode) ? FLAT : SHARP;
  return { root: best.root, mode: best.mode, name: names[best.root] + (best.mode === 'minor' ? ' minor' : ' major'),
           confidence: Math.max(0, Math.min(1, best.score / 2)) };
}

//----------------------------------------------------------------- analyse
function analyze (pcm, onProgress) {
  const nFrames = Math.max(0, Math.floor((pcm.length - 1) / HOP) + 1);
  const specH = makeFFT(NH), specL = makeFFT(NL);
  const P = P_HI - P_LO + 1;

  // Harmonic weights and per-frame working arrays.
  const HW = [1, 0.62, 0.46, 0.36, 0.3];
  // Spectrum bin ranges for every (pitch, harmonic), computed once instead of
  // once per frame — the inner loop is then just a max over a few bins.
  const NHARM = HW.length;
  const binLo = new Int32Array(P * NHARM), binHi = new Int32Array(P * NHARM), binL = new Uint8Array(P * NHARM);
  for (let i = 0; i < P; i++) {
    const p = P_LO + i, f0 = hz(p), useL = p < SPLIT;
    for (let h = 1; h <= NHARM; h++) {
      const fh = f0 * h, k = i * NHARM + h - 1;
      if (fh > 5000) { binLo[k] = 1; binHi[k] = 0; continue; }       // empty range
      const long = useL && fh < 400, n = long ? NL : NH;
      binL[k] = long ? 1 : 0;
      binLo[k] = Math.max(1, Math.floor(fh * Math.pow(2, -1 / 24) * n / SR));
      binHi[k] = Math.min(n / 2 - 1, Math.ceil(fh * Math.pow(2, 1 / 24) * n / SR));
    }
  }
  const hpk = new Float64Array(NHARM);
  const sal = new Float32Array(nFrames * P);      // overtone-suppressed salience
  const chroma = new Float32Array(nFrames * 12);
  const bass = new Float32Array(nFrames * 12);
  const energy = new Float32Array(nFrames);
  const frameMax = new Float32Array(nFrames);
  let lowMag = null;
  const lowCopy = new Float64Array(NL / 2);
  const raw = new Float64Array(P);

  for (let f = 0; f < nFrames; f++) {
    const centre = f * HOP;
    const mh = specH(pcm, centre - NH / 2);
    // The long window changes slowly; computing it on every other frame halves
    // the cost and is invisible in the result.
    if (f % 2 === 0 || !lowMag) { lowMag = specL(pcm, centre - NL / 2); lowCopy.set(lowMag); }
    const ml = lowCopy;

    let e = 0;
    for (let b = 2; b < mh.length; b++) e += mh[b];
    energy[f] = e;

    // Harmonic-sum salience, magnitudes compressed with a 0.6 power.
    for (let i = 0; i < P; i++) {
      let s = 0, hmax = 0;
      for (let h = 0; h < NHARM; h++) {
        const k = i * NHARM + h, src = binL[k] ? ml : mh;
        let m = 0;
        for (let b = binLo[k]; b <= binHi[k]; b++) if (src[b] > m) m = src[b];
        hpk[h] = m;
        if (m > hmax) hmax = m;
        s += HW[h] * Math.pow(m, 0.6);
      }
      // Fundamental gate. A pitch an octave BELOW a real note collects that
      // note's whole harmonic series on its even harmonics and outscores it —
      // the classic octave-down error. A real note has energy at its own
      // fundamental (even a bass, whose fundamental is weaker than its second
      // harmonic, has some), so a candidate whose fundamental is nearly absent
      // is heavily discounted.
      if (hpk[0] < 0.18 * hmax) s *= 0.2;
      raw[i] = s;
    }
    // Overtone suppression: a note's octave and twelfth collect its own
    // harmonics, so take back a share of what the pitches below already claim.
    let fm = 0;
    for (let i = 0; i < P; i++) {
      let v = raw[i];
      if (i >= 12) v -= 0.45 * sal[f * P + i - 12];
      if (i >= 19) v -= 0.35 * sal[f * P + i - 19];
      if (i >= 24) v -= 0.2 * sal[f * P + i - 24];
      v = Math.max(0, v);
      sal[f * P + i] = v;
      if (v > fm) fm = v;
    }
    frameMax[f] = fm;

    if (onProgress && (f % 64 === 0)) onProgress(0.9 * f / nFrames);
  }

  //----------------------------------------------------------- silence floor
  const sortedE = Array.from(energy).sort((a, b) => a - b);
  const e90 = sortedE[Math.floor(0.9 * (sortedE.length - 1))] || 0;
  const silent = f => energy[f] < e90 * 0.03 || frameMax[f] <= 0;
  const sortedM = Array.from(frameMax).sort((a, b) => a - b);
  const m95 = sortedM[Math.floor(0.95 * (sortedM.length - 1))] || 1;

  //------------------------------------------------------------ candidates
  // Per frame: the pitches that are really sounding, with overtones removed.
  // Chords AND notes are built from these, so the overtone clean-up that stops
  // the melody vanishing also stops a triad being read as a seventh chord.
  const floor = 0.12 * m95;
  const frameCands = new Array(nFrames);
  const fmLoA = new Float32Array(nFrames), fmHiA = new Float32Array(nFrames);
  // Each harmonic can only explain so much: a pitch is dropped as an overtone
  // only when it is no louder than that harmonic of a lower note would be. A
  // chord tone that coincides with a bass harmonic (G and C over a C bass —
  // most of a root-position chord) is louder than that, and stays. The octave
  // is the strictest case: melodies double chord tones an octave up all the
  // time, and the lower note's score is itself inflated by the upper one.
  const HARM = { 12: 0.45, 19: 0.32, 24: 0.26, 28: 0.22, 31: 0.2, 34: 0.18, 36: 0.16 };
  for (let f = 0; f < nFrames; f++) {
    const cands = [];
    frameCands[f] = cands;
    if (silent(f)) continue;
    // Two references, not one: the bass register usually owns the frame
    // maximum, and measuring the upper voices against it made quiet chord
    // tones and the melody disappear under a loud bass note.
    let fmLo = 0, fmHi = 0;
    for (let i = 0; i < P; i++) { const v = sal[f * P + i]; if (P_LO + i < BASS_TOP) fmLo = Math.max(fmLo, v); else fmHi = Math.max(fmHi, v); }
    fmLoA[f] = fmLo; fmHiA[f] = fmHi;
    const all = [];
    for (let i = 0; i < P; i++) {
      const v = sal[f * P + i];
      const ref = P_LO + i < BASS_TOP ? fmLo : fmHi;
      if (v < floor || v < 0.3 * ref) continue;
      const l = i > 0 ? sal[f * P + i - 1] : 0, r = i < P - 1 ? sal[f * P + i + 1] : 0;
      if (v >= l && v >= r) all.push({ p: P_LO + i, v });
    }
    all.sort((a, b) => b.v - a.v);
    for (const c of all) {
      const overtone = cands.some(k => k.p < c.p && HARM[c.p - k.p] !== undefined && c.v < HARM[c.p - k.p] * k.v);
      if (!overtone) cands.push(c);
      if (cands.length === 6) break;
    }
    // Chroma from the kept pitches (E2..C6), bass chroma from E1..E3, with a
    // little of the raw salience underneath so a sparse frame still has a shape.
    // The melody register counts for less: a passing tone in the tune (a D
    // sung over a C chord) should not rename the chord underneath it.
    for (const c of cands) {
      if (c.p >= 40 && c.p <= 84) chroma[f * 12 + (c.p % 12)] += c.v * (c.p > 71 ? 0.4 : 1);
      if (c.p <= 52) bass[f * 12 + (c.p % 12)] += c.v;
    }
    for (let i = 0; i < P; i++) {
      const p = P_LO + i, v = 0.15 * sal[f * P + i];
      if (p >= 40 && p <= 84) chroma[f * 12 + (p % 12)] += v;
      if (p <= 52) bass[f * 12 + (p % 12)] += v;
    }
  }

  //------------------------------------------------------------------ key
  const chromaSum = new Float64Array(12);
  for (let f = 0; f < nFrames; f++) {
    if (silent(f)) continue;
    let n = 0;
    for (let c = 0; c < 12; c++) n += chroma[f * 12 + c] * chroma[f * 12 + c];
    n = Math.sqrt(n) || 1;
    for (let c = 0; c < 12; c++) chromaSum[c] += chroma[f * 12 + c] / n;
  }

  //---------------------------------------------------------------- chords
  const S = 12 * QUAL.length + 1, N_STATE = S - 1;
  const emis = new Float32Array(nFrames * S);
  const TEMP = 18;
  for (let f = 0; f < nFrames; f++) {
    const base = f * S;
    if (silent(f)) { for (let s = 0; s < S; s++) emis[base + s] = 0; emis[base + N_STATE] = TEMP; continue; }
    let n = 0, bm = 0;
    for (let c = 0; c < 12; c++) { n += chroma[f * 12 + c] ** 2; bm = Math.max(bm, bass[f * 12 + c]); }
    n = Math.sqrt(n) || 1; bm = bm || 1;
    for (let r = 0; r < 12; r++) {
      for (let qi = 0; qi < QUAL.length; qi++) {
        const Q = QUAL[qi];
        let dot = 0;
        for (const iv of Q.iv) dot += chroma[f * 12 + (r + iv) % 12];
        const cos = dot / (n * Math.sqrt(Q.iv.length));
        let score = cos + 0.2 * (bass[f * 12 + r] / bm) + Q.prior;   // the bass usually plays the root
        // A seventh has to be PLAYED, not implied: a triad's own third harmonic
        // lands a major seventh above the third (A → E over F; B → F# over G),
        // so without this every major triad drifts to maj7. The added tone must
        // be a real share of the triad's energy.
        if (Q.iv.length === 4) {
          const triad = (chroma[f * 12 + r] + chroma[f * 12 + (r + Q.iv[1]) % 12] + chroma[f * 12 + (r + 7) % 12]) / 3;
          if (chroma[f * 12 + (r + Q.iv[3]) % 12] < 0.7 * triad) score -= 0.15;
        }
        emis[base + qi * 12 + r] = score * TEMP;
      }
    }
    emis[base + N_STATE] = 0.5 * TEMP;
  }
  // Viterbi with a single switch penalty — O(frames × states), not × states².
  const SWITCH = 7;
  const back = new Int16Array(nFrames * S);
  let prev = new Float32Array(S), cur = new Float32Array(S);
  for (let s = 0; s < S; s++) prev[s] = emis[s];
  for (let f = 1; f < nFrames; f++) {
    let pm = -Infinity, pa = 0;
    for (let s = 0; s < S; s++) if (prev[s] > pm) { pm = prev[s]; pa = s; }
    for (let s = 0; s < S; s++) {
      const stay = prev[s], move = pm - SWITCH;
      if (stay >= move) { cur[s] = stay + emis[f * S + s]; back[f * S + s] = s; }
      else { cur[s] = move + emis[f * S + s]; back[f * S + s] = pa; }
    }
    const t = prev; prev = cur; cur = t;
  }
  const path = new Int16Array(nFrames);
  if (nFrames > 0) {
    let bs = 0;
    for (let s = 1; s < S; s++) if (prev[s] > prev[bs]) bs = s;
    path[nFrames - 1] = bs;
    for (let f = nFrames - 1; f > 0; f--) path[f - 1] = back[f * S + path[f]];
  }
  const tOf = f => Math.max(0, f * HOP / SR);
  const chords = [];
  let segStart = 0;
  for (let f = 1; f <= nFrames; f++) {
    if (f < nFrames && path[f] === path[segStart]) continue;
    const st = path[segStart];
    if (st !== N_STATE) {
      const qi = Math.floor(st / 12), r = st % 12;
      // Slash chord if a different chord tone clearly owns the bass.
      const bsum = new Float64Array(12);
      for (let g = segStart; g < f; g++) for (let c = 0; c < 12; c++) bsum[c] += bass[g * 12 + c];
      let bb = 0; for (let c = 1; c < 12; c++) if (bsum[c] > bsum[bb]) bb = c;
      const tones = QUAL[qi].iv.map(iv => (r + iv) % 12);
      const slash = bb !== r && tones.includes(bb) && bsum[bb] > 1.4 * bsum[r];
      chords.push({ s: tOf(segStart), e: tOf(f), root: r, q: QUAL[qi].q, bass: slash ? bb : r,
                    qi, slash });
    }
    segStart = f;
  }
  // Absorb blips shorter than 0.25 s into their neighbour.
  for (let i = chords.length - 1; i >= 0; i--) {
    const c = chords[i];
    if (c.e - c.s >= 0.25 || chords.length === 1) continue;
    if (i > 0 && Math.abs(chords[i - 1].e - c.s) < 0.06) { chords[i - 1].e = c.e; chords.splice(i, 1); }
    else if (i + 1 < chords.length && Math.abs(chords[i + 1].s - c.e) < 0.06) { chords[i + 1].s = c.s; chords.splice(i, 1); }
  }
  // Merge neighbours that ended up with the same name.
  for (let i = chords.length - 1; i > 0; i--)
    if (sameChord (chords[i], chords[i - 1]) && Math.abs(chords[i - 1].e - chords[i].s) < 0.06) { chords[i - 1].e = chords[i].e; chords.splice(i, 1); }

  // Key, now that the chords are known — then spell every chord in it.
  const key = detectKeyFromChords(chords, chromaSum);
  const names = usesFlats(key.root, key.mode) ? FLAT : SHARP;
  for (const c of chords)
    c.name = names[c.root] + QUAL[c.qi].suffix + (c.slash ? '/' + names[c.bass] : '');

  //----------------------------------------------------------------- notes
  const notes = [];
  const active = new Map();
  // A two-frame note has to be strong to count. The long bass window straddles
  // two notes for ~90 ms at every change and invents weak in-between pitches;
  // a real short note (a fast bass sixteenth) is loud.
  const close = (p, a) => { if (a.n >= 3 || (a.n === 2 && a.sum / 2 >= 0.35 * m95)) notes.push({ p, s: tOf(a.start), e: tOf(a.last + 1), sum: a.sum, n: a.n, bv: a.bv, mv: a.mv }); };
  for (let f = 0; f < nFrames; f++) {
    const cands = frameCands[f];
    // Per-frame roles: the lowest strong pitch is the bass, the highest the melody.
    let bassP = -1, melP = -1;
    for (const c of cands) {
      if (c.p <= 55 && c.v >= 0.35 * fmLoA[f] && (bassP < 0 || c.p < bassP)) bassP = c.p;
      if (c.p >= 55 && c.v >= 0.45 * fmHiA[f] && c.p > melP) melP = c.p;
    }
    const seen = new Set();
    for (const c of cands) {
      seen.add(c.p);
      let a = active.get(c.p);
      const prevV = f > 0 ? sal[(f - 1) * P + (c.p - P_LO)] : 0;
      // A sharp rise on a pitch that is already sounding is a re-attack.
      if (a && prevV > 0 && c.v > 1.9 * prevV && f - a.start >= 2) { close(c.p, a); a = null; }
      if (a && f - a.last <= 2) { a.last = f; a.sum += c.v; a.n++; }
      else { if (a) close(c.p, a); a = { start: f, last: f, sum: c.v, n: 1, bv: 0, mv: 0 }; active.set(c.p, a); }
      if (c.p === bassP) a.bv++;
      if (c.p === melP) a.mv++;
    }
    for (const [p, a] of active) if (!seen.has(p) && f - a.last > 2) { close(p, a); active.delete(p); }
  }
  for (const [p, a] of active) close(p, a);
  notes.sort((a, b) => a.s - b.s || a.p - b.p);
  const out = notes.slice(0, 8000).map(n => ({
    s: +n.s.toFixed(3), e: +n.e.toFixed(3), p: n.p,
    v: +Math.min(1, (n.sum / n.n) / m95).toFixed(2),
    role: n.bv >= 0.5 * n.n ? 'bass' : n.mv >= 0.5 * n.n ? 'melody' : 'other',
  }));

  if (onProgress) onProgress(1);
  return { duration: pcm.length / SR, key,
           chords: chords.map(c => ({ s: +c.s.toFixed(3), e: +c.e.toFixed(3), name: c.name, root: c.root, q: c.q, bass: c.bass })),
           notes: out };
}

//------------------------------------------------------------ worker glue
if (typeof self !== 'undefined' && typeof importScripts === 'function') {
  self.onmessage = e => {
    const { pcm } = e.data;
    try {
      const result = analyze(pcm, p => self.postMessage({ progress: p }));
      self.postMessage({ result });
    } catch (err) {
      self.postMessage({ error: String(err && err.message || err) });
    }
  };
}
if (typeof module !== 'undefined') module.exports = { analyze, SR, detectKey };
