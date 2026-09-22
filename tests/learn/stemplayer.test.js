// stemplayer.test.js — checks the Learn tab's stem player.
//
//   node apps/lowend/tests/learn/stemplayer.test.js
//
// The player is an AudioWorklet, so the three globals a worklet is given
// (AudioWorkletProcessor, registerProcessor, sampleRate) are stubbed here and
// the real resources/ui/stemplayer.js is run against them. Nothing is mocked
// below that level: this drives the actual process() in 128-sample blocks, the
// way the browser does.
//
// What matters about this code is not that it makes sound, it is that:
//   * at 100 % speed it is a bit-exact mix, so ordinary playback is untouched;
//   * mute and level are honoured immediately;
//   * slowing down does NOT drop the pitch (the whole point of the stretcher);
//   * and the stems cannot drift apart. The drift test is the strong one: two
//     stems holding the same signal with opposite sign must cancel to silence
//     at ANY speed, which is only true if both are read from one time pointer.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const SR = 44100;
let checks = 0, failures = 0;
const check = (ok, what) => { checks++; if (!ok) { failures++; console.log('  FAIL  ' + what); } };
const close = (a, b, tol, what) => check(Math.abs(a - b) <= tol, `${what} (${a.toFixed(4)} vs ${b.toFixed(4)})`);

/** Load the worklet the way the browser would, and return its processor class. */
function loadWorklet () {
  const src = fs.readFileSync(path.join(__dirname, '..', '..', 'resources', 'ui', 'stemplayer.js'), 'utf8');
  let registered = null;
  const sandbox = {
    sampleRate: SR,
    registerProcessor: (name, cls) => { registered = { name, cls }; },
    AudioWorkletProcessor: class { constructor () { this.port = { onmessage: null, postMessage: () => {} }; } },
    Math, Float32Array, console,
  };
  vm.createContext(sandbox);
  vm.runInContext(src, sandbox, { filename: 'stemplayer.js' });
  if (!registered) throw new Error('stemplayer.js registered no processor');
  return registered;
}

const { name, cls: StemPlayer } = loadWorklet();

/** A player loaded with the given stems (arrays of mono Float32Array). */
function make (monoStems) {
  const p = new StemPlayer();
  const sent = [];
  p.port.postMessage = (m) => sent.push(m);
  p.port.onmessage({ data: { type: 'load', stems: monoStems.map(m => [m, m]) } });
  return { p, sent };
}

/** Pull `frames` samples of output, in 128-sample blocks. */
function pull (p, frames) {
  const L = new Float32Array(frames), R = new Float32Array(frames);
  const out = [new Float32Array(128), new Float32Array(128)];
  for (let done = 0; done < frames; done += 128) {
    out[0].fill(0); out[1].fill(0);
    p.process([], [out]);
    const n = Math.min(128, frames - done);
    L.set(out[0].subarray(0, n), done);
    R.set(out[1].subarray(0, n), done);
  }
  return [L, R];
}

const sine = (n, f, a = 0.5) => {
  const v = new Float32Array(n);
  for (let i = 0; i < n; i++) v[i] = a * Math.sin(2 * Math.PI * f * i / SR);
  return v;
};

/** Dominant frequency from zero crossings of a steady tone. */
function freqOf (x, from, to) {
  let crossings = 0, first = -1, last = -1;
  for (let i = from + 1; i < to; i++)
    if ((x[i - 1] < 0) !== (x[i] < 0)) { crossings++; if (first < 0) first = i; last = i; }
  if (crossings < 4) return 0;
  return (crossings - 1) * SR / (2 * (last - first));
}

const rms = (x, from = 0, to = x.length) => {
  let s = 0;
  for (let i = from; i < to; i++) s += x[i] * x[i];
  return Math.sqrt(s / Math.max(1, to - from));
};

console.log('Stem player\n===========');
check(name === 'stem-player', 'registers as stem-player');

//--------------------------------------------------------------- normal speed
{
  const a = sine(SR * 2, 110), b = sine(SR * 2, 330, 0.25);
  const { p } = make([a, b, new Float32Array(SR * 2), new Float32Array(SR * 2)]);
  p.port.onmessage({ data: { type: 'play', at: 0 } });
  const [L] = pull(p, SR);
  let worst = 0;
  for (let i = 0; i < SR; i++) worst = Math.max(worst, Math.abs(L[i] - (a[i] + b[i])));
  check(worst < 1e-6, `100 % speed is a bit-exact mix (worst error ${worst.toExponential(1)})`);
}

//------------------------------------------------------------- mute and level
{
  const a = sine(SR, 110), b = sine(SR, 330);
  const { p } = make([a, b, new Float32Array(SR), new Float32Array(SR)]);
  p.port.onmessage({ data: { type: 'gains', gain: [1, 0, 0, 0] } });
  p.port.onmessage({ data: { type: 'play', at: 0 } });
  const [L] = pull(p, 4096);
  let worst = 0;
  for (let i = 0; i < 4096; i++) worst = Math.max(worst, Math.abs(L[i] - a[i]));
  check(worst < 1e-6, 'a muted stem contributes nothing');

  // Level change mid-flight must apply to the very next block.
  p.port.onmessage({ data: { type: 'gains', gain: [0.5, 0, 0, 0] } });
  const [L2] = pull(p, 512);
  const expect = rms(a.subarray(4096, 4608)) * 0.5;
  close(rms(L2), expect, expect * 0.02, 'a level change lands on the next block');
}

//-------------------------------------------------------------- pitch at half
{
  // Two seconds of a steady tone, played at half speed: four seconds of output,
  // and the same note. A naive resampler would answer 110 Hz.
  const a = sine(SR * 3, 220);
  const { p } = make([a, new Float32Array(SR * 3), new Float32Array(SR * 3), new Float32Array(SR * 3)]);
  p.port.onmessage({ data: { type: 'rate', rate: 0.5 } });
  p.port.onmessage({ data: { type: 'play', at: 0 } });
  const [L] = pull(p, SR * 2);
  const f = freqOf(L, SR / 2, SR * 3 / 2);
  close(f, 220, 6, 'half speed keeps the pitch');
  close(p.inPos / SR, 1.0, 0.05, 'half speed consumes half as much song');
  const level = rms(L, SR / 2, SR * 3 / 2);
  close(level, rms(a) , rms(a) * 0.15, 'the stretcher keeps the level');
}

//--------------------------------------------------------- no drift, any speed
for (const rate of [1, 0.75, 0.5, 1.25]) {
  // Stems 0 and 1 hold the same music with opposite sign; stem 2 holds
  // something else so the alignment search has a real signal to work on. If the
  // two stems were read even one sample apart, they would stop cancelling.
  const music = sine(SR * 2, 147, 0.6), other = sine(SR * 2, 523, 0.3);
  const neg = new Float32Array(music.length);
  for (let i = 0; i < music.length; i++) neg[i] = -music[i];
  const { p } = make([music, neg, other, new Float32Array(SR * 2)]);
  p.port.onmessage({ data: { type: 'rate', rate } });
  p.port.onmessage({ data: { type: 'play', at: 0 } });
  const [L] = pull(p, Math.floor(SR * 1.5));

  // Whatever is left must be stem 2 alone: compare against the same signal
  // played on its own at the same rate.
  const solo = make([new Float32Array(SR * 2), new Float32Array(SR * 2), other, new Float32Array(SR * 2)]);
  solo.p.port.onmessage({ data: { type: 'rate', rate } });
  solo.p.port.onmessage({ data: { type: 'play', at: 0 } });
  const [S] = pull(solo.p, Math.floor(SR * 1.5));
  let worst = 0;
  for (let i = 0; i < L.length; i++) worst = Math.max(worst, Math.abs(L[i] - S[i]));
  check(worst < 1e-5, `stems stay sample-locked at ${Math.round(rate * 100)} % (worst error ${worst.toExponential(1)})`);
}

//--------------------------------------------------------------------- looping
{
  const a = sine(SR * 4, 110);
  const { p } = make([a, new Float32Array(SR * 4), new Float32Array(SR * 4), new Float32Array(SR * 4)]);
  p.port.onmessage({ data: { type: 'loop', on: true, a: 1.0, b: 2.0 } });
  p.port.onmessage({ data: { type: 'play', at: 1.0 } });
  pull(p, SR * 3);
  check(p.inPos / SR >= 1.0 && p.inPos / SR <= 2.05, `a loop keeps the position inside it (at ${(p.inPos / SR).toFixed(2)} s)`);
}

//----------------------------------------------------------------- end of song
{
  const a = sine(Math.floor(SR * 0.5), 110);
  const { p, sent } = make([a, new Float32Array(a.length), new Float32Array(a.length), new Float32Array(a.length)]);
  p.port.onmessage({ data: { type: 'play', at: 0 } });
  pull(p, SR);
  check(sent.some(m => m.type === 'ended'), 'the end of the song is reported');
  check(p.playing === false, 'and playback stops');
}

console.log(`\n${checks} checks, ${failures} failures`);
process.exit(failures === 0 ? 0 : 1);
