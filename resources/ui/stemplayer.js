// stemplayer.js — the Learn tab's stem player, as an AudioWorklet.
//
// WHY A WORKLET. Playing four separated stems with mute, solo and level is easy;
// playing them SLOWED DOWN without changing pitch, and keeping them in sample
// lock with each other, is not. Four <audio> elements each time-stretching
// independently drift apart within seconds, and a flam between the bass and the
// drums makes a practice tool useless. An AudioBufferSourceNode's playbackRate
// would keep them locked but drops the pitch with the speed, which is worse.
//
// So all four stems are stretched HERE, by one process, from ONE time pointer:
//
//   * the alignment search (WSOLA) runs on the summed mix, so the stretch
//     follows the song's own transients;
//   * the window and the read position it finds are then applied IDENTICALLY to
//     all four stems, which makes relative drift impossible by construction;
//   * the per-stem gains are applied as the stems are read, so mute, solo and
//     level take effect on the next 128-sample block — no re-render, no click.
//
// At exactly 100 % speed the stretcher is bypassed altogether and the stems are
// copied straight out, so ordinary playback is bit-exact.
//
// The audio the worklet plays is the user's own imported file, separated on
// their machine. Nothing here records, uploads or retains it.

const N = 2048;              // analysis/synthesis frame
const HS = N / 4;            // synthesis hop — hann at 75 % overlap sums to 2
const SEARCH = 256;          // +/- samples WSOLA may slide to find a better join
const OLA_GAIN = 0.5;        // undoes the window overlap sum

function hann (n)
{
    const w = new Float32Array (n);
    for (let i = 0; i < n; i++) w[i] = 0.5 * (1 - Math.cos (2 * Math.PI * i / n));
    return w;
}

class StemPlayer extends AudioWorkletProcessor
{
    constructor ()
    {
        super();
        this.stems = [];        // [[L,R], ...] one pair per stem
        this.mix = null;        // mono sum, only for the alignment search
        this.frames = 0;
        this.gain = [1, 1, 1, 1];
        this.playing = false;
        this.rate = 1;
        this.inPos = 0;         // read position in the song, in samples
        this.loop = null;       // {a, b} in samples

        this.win = hann (N);
        this.acc = [new Float32Array (N * 2), new Float32Array (N * 2)];
        this.accRead = 0;       // samples already handed out of acc
        this.accFill = 0;       // samples of finished output in acc
        this.tail = new Float32Array (HS);   // previous frame's join, mix domain
        this.hasTail = false;
        this.report = 0;

        this.port.onmessage = (e) => this.onMessage (e.data);
    }

    onMessage (m)
    {
        switch (m.type)
        {
            case 'load':
                // Float32Arrays arrive transferred, so nothing is copied.
                this.stems = m.stems.map (s => [new Float32Array (s[0]), new Float32Array (s[1])]);
                this.frames = this.stems.length ? this.stems[0][0].length : 0;
                this.mix = new Float32Array (this.frames);
                for (const s of this.stems)
                    for (let i = 0; i < this.frames; i++) this.mix[i] += 0.5 * (s[0][i] + s[1][i]);
                this.reset (0);
                this.port.postMessage ({ type: 'ready', frames: this.frames });
                break;
            case 'gains': this.gain = m.gain.slice (0, 4); break;
            case 'rate':  this.rate = Math.max (0.25, Math.min (2, m.rate)); break;
            case 'loop':  this.loop = m.on ? { a: Math.round (m.a * sampleRate), b: Math.round (m.b * sampleRate) } : null; break;
            case 'play':  if (m.at !== undefined) this.reset (Math.round (m.at * sampleRate));
                          this.playing = this.frames > 0; break;
            case 'pause': this.playing = false; break;
            case 'seek':  this.reset (Math.round (m.t * sampleRate)); break;
            case 'clear': this.playing = false; this.stems = []; this.mix = null; this.frames = 0; break;
        }
    }

    reset (pos)
    {
        this.inPos = Math.max (0, Math.min (this.frames, pos || 0));
        this.acc[0].fill (0); this.acc[1].fill (0);
        this.accRead = 0; this.accFill = 0;
        this.hasTail = false;
    }

    /// Where, in the mix, the next frame joins most smoothly onto the last one.
    /// Plain normalised cross-correlation over the join length; at rate 1 there
    /// is nothing to search for, and the caller does not get here.
    bestOffset (p)
    {
        if (! this.hasTail) return 0;
        let best = 0, bestScore = -Infinity;
        const lo = Math.max (-SEARCH, -p), hi = Math.min (SEARCH, this.frames - p - HS);
        for (let d = lo; d <= hi; d += 4)        // step 4: a sample-exact search
        {                                        // costs 4x for no audible gain
            let num = 0, den = 1e-9;
            for (let i = 0; i < HS; i += 2)
            {
                const v = this.mix[p + d + i];
                num += v * this.tail[i];
                den += v * v;
            }
            const score = num / Math.sqrt (den);
            if (score > bestScore) { bestScore = score; best = d; }
        }
        return best;
    }

    /// Overlap-add one synthesis frame into `acc`, advancing the song position
    /// by the analysis hop. Returns false at the end of the song.
    produce ()
    {
        if (this.inPos >= this.frames) return false;

        // Make room: slide the accumulator down by what has been consumed.
        if (this.accRead > 0)
        {
            for (const c of this.acc) c.copyWithin (0, this.accRead);
            const keep = this.acc[0].length - this.accRead;
            for (const c of this.acc) c.fill (0, keep);
            this.accFill = Math.max (0, this.accFill - this.accRead);
            this.accRead = 0;
        }

        const stretching = Math.abs (this.rate - 1) > 1e-4;
        const p = Math.max (0, Math.min (this.frames - 1,
                    Math.round (this.inPos) + (stretching ? this.bestOffset (Math.round (this.inPos)) : 0)));
        const n = Math.min (N, this.frames - p);

        for (let i = 0; i < n; i++)
        {
            const w = stretching ? this.win[i] * OLA_GAIN : 1;
            let l = 0, r = 0;
            for (let s = 0; s < this.stems.length; s++)
            {
                const g = this.gain[s];
                if (g === 0) continue;
                l += this.stems[s][0][p + i] * g;
                r += this.stems[s][1][p + i] * g;
            }
            const o = this.accFill + i;
            if (o < this.acc[0].length) { this.acc[0][o] += l * w; this.acc[1][o] += r * w; }
        }

        if (stretching)
        {
            // Remember the join for the next frame's alignment search.
            for (let i = 0; i < HS; i++)
            {
                const j = p + HS + i;
                this.tail[i] = j < this.frames ? this.mix[j] : 0;
            }
            this.hasTail = true;
            this.accFill += HS;
            this.inPos += HS * this.rate;
        }
        else
        {
            this.accFill += n;
            this.inPos += n;
        }

        if (this.loop && this.inPos >= this.loop.b) { this.reset (this.loop.a); }
        return true;
    }

    process (_inputs, outputs)
    {
        const out = outputs[0];
        const need = out[0].length;

        if (! this.playing || this.frames === 0)
        {
            out[0].fill (0); out[1].fill (0);
            return true;
        }

        let done = 0;
        while (done < need)
        {
            const have = this.accFill - this.accRead;
            if (have <= 0)
            {
                if (! this.produce())
                {
                    out[0].fill (0, done); out[1].fill (0, done);
                    this.playing = false;
                    this.port.postMessage ({ type: 'ended' });
                    break;
                }
                continue;
            }
            const take = Math.min (need - done, have);
            for (let c = 0; c < 2; c++)
                out[c].set (this.acc[c].subarray (this.accRead, this.accRead + take), done);
            this.accRead += take;
            done += take;
        }

        // Tell the page where we are, about 25 times a second.
        this.report += need;
        if (this.report >= sampleRate / 25)
        {
            this.report = 0;
            this.port.postMessage ({ type: 'pos', t: this.inPos / sampleRate });
        }
        return true;
    }
}

registerProcessor ('stem-player', StemPlayer);
