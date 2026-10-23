// stemplayer.js — the Learn tab's song player, as an AudioWorklet.
//
// WHY A WORKLET. Playing separated stems with mute, solo and level is easy;
// playing them SLOWED DOWN without changing pitch, and keeping them in sample
// lock with each other, is not. Several <audio> elements each time-stretching
// independently drift apart within seconds, and a flam between the bass and the
// drums makes a practice tool useless. An AudioBufferSourceNode's playbackRate
// would keep them locked but drops the pitch with the speed, which is worse.
//
// So every stem is stretched HERE, by one process, from ONE time pointer:
//
//   * the alignment search (WSOLA) runs on the summed mix, so the stretch
//     follows the song's own transients;
//   * the window and the read position it finds are then applied IDENTICALLY to
//     every stem, which makes relative drift impossible by construction;
//   * the per-stem gains are applied as the stems are read, so mute, solo and
//     level take effect on the next 128-sample block — no re-render, no click.
//
// A song that has not been separated is loaded as ONE "stem" (the whole mix),
// so speed, pitch, the click and the count-in work on every song.
//
// PITCH. Transposing is stretching plus resampling: the song is stretched by
// the pitch ratio as well as the speed, and the result is then read back
// `ratio` samples per output sample. The two cancel in time and leave only the
// pitch change.
//
// THE CLICK. Clicks are placed on the song's own beats (found by analyzer.js),
// counted on a clock that runs at the playback speed, so they slow down with
// the song. A count-in plays the song's tempo for N beats before it starts.
//
// At exactly 100 % speed and no transposition the stretcher is bypassed and the
// stems are copied straight out, so ordinary playback is bit-exact.
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
        this.ratio = 1;         // pitch: 2^(semitones/12)
        this.inPos = 0;         // read position in the song, in samples
        this.loop = null;       // {a, b} in samples

        this.win = hann (N);
        this.acc = [new Float32Array (N * 4), new Float32Array (N * 4)];
        this.accRead = 0;       // samples already handed out of acc (integer part)
        this.frac = 0;          // fractional read position when transposing
        this.accFill = 0;       // samples of finished output in acc
        this.tail = new Float32Array (HS);   // previous frame's join, mix domain
        this.hasTail = false;
        this.report = 0;

        // click / count-in
        this.beats = [];        // seconds
        this.downbeat = 0;      // which beat-of-four is "one"
        this.tempo = 0;
        this.clickOn = false;
        this.clickLevel = 0.5;
        this.songT = 0;         // the song time being heard now, in seconds
        this.nextBeat = 0;      // index into beats
        this.preroll = 0;       // samples of count-in left
        this.prerollBeat = 0;   // samples to the next count-in click
        this.prerollCount = 0;
        this.voice = 0;         // samples left on the click voice
        this.voiceHz = 1500;
        this.voicePh = 0;
        this.voiceAmp = 0;

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
                this.gain = this.stems.map (() => 1);
                this.mix = new Float32Array (this.frames);
                for (const s of this.stems)
                    for (let i = 0; i < this.frames; i++) this.mix[i] += 0.5 * (s[0][i] + s[1][i]);
                this.reset (0);
                this.port.postMessage ({ type: 'ready', frames: this.frames });
                break;
            case 'gains': this.gain = m.gain.slice (0, this.stems.length || m.gain.length); break;
            case 'rate':  this.rate = Math.max (0.25, Math.min (2, m.rate)); break;
            case 'pitch': this.ratio = Math.pow (2, Math.max (-12, Math.min (12, m.semitones || 0)) / 12); break;
            case 'beats': this.beats = m.beats || []; this.downbeat = m.downbeat || 0; this.tempo = m.tempo || 0; this.accents = m.accents || null; this.seekBeat(); break;
            case 'click': this.clickOn = !! m.on; this.clickLevel = m.level != null ? m.level : this.clickLevel; break;
            case 'loop':  this.loop = m.on ? { a: Math.round (m.a * sampleRate), b: Math.round (m.b * sampleRate) } : null; break;
            case 'play':
                if (m.at !== undefined) this.reset (Math.round (m.at * sampleRate));
                // A count-in: N clicks at the song's tempo (as slowed), then the song.
                if (m.countIn > 0 && this.tempo > 0)
                {
                    const gap = Math.round (sampleRate * 60 / this.tempo / this.rate);
                    this.preroll = gap * m.countIn;
                    this.prerollBeat = 0;
                    this.prerollCount = 0;
                    this.prerollGap = gap;
                }
                this.playing = this.frames > 0; break;
            case 'pause': this.playing = false; this.preroll = 0; break;
            case 'seek':  this.reset (Math.round (m.t * sampleRate)); break;
            case 'clear': this.playing = false; this.stems = []; this.mix = null; this.frames = 0; break;
        }
    }

    reset (pos)
    {
        this.inPos = Math.max (0, Math.min (this.frames, pos || 0));
        this.acc[0].fill (0); this.acc[1].fill (0);
        this.accRead = 0; this.accFill = 0; this.frac = 0;
        this.hasTail = false;
        this.songT = this.inPos / sampleRate;
        this.seekBeat();
    }

    /// The first beat at or after the song time now being heard.
    seekBeat ()
    {
        const b = this.beats; let lo = 0, hi = b.length;
        while (lo < hi) { const mid = (lo + hi) >> 1; if (b[mid] < this.songT - 1e-4) lo = mid + 1; else hi = mid; }
        this.nextBeat = lo;
    }

    trigger (accent)
    {
        this.voice = Math.round (sampleRate * 0.05);
        this.voiceHz = accent ? 2000 : 1500;
        this.voiceAmp = this.clickLevel * (accent ? 1 : 0.7);
        this.voicePh = 0;
    }

    clickSample ()
    {
        if (this.voice <= 0) return 0;
        const age = Math.round (sampleRate * 0.05) - this.voice;
        this.voice--;
        this.voicePh += 2 * Math.PI * this.voiceHz / sampleRate;
        return Math.sin (this.voicePh) * Math.exp (-age / (sampleRate * 0.009)) * Math.min (1, age / (sampleRate * 0.0005)) * this.voiceAmp;
    }

    /// Where, in the mix, the next frame joins most smoothly onto the last one.
    /// Plain normalised cross-correlation over the join length; with nothing to
    /// stretch there is nothing to search for, and the caller does not get here.
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

        // The stretch has to cover the speed AND the pitch ratio, which the
        // resampling on the way out takes back out of the timing.
        const stretch = this.rate / this.ratio;
        const stretching = Math.abs (stretch - 1) > 1e-4;
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
                if (! g) continue;
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
            this.inPos += HS * stretch;
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

        // Count-in: clicks only, the song waits.
        let done = 0;
        if (this.preroll > 0)
        {
            const take = Math.min (need, this.preroll);
            for (let i = 0; i < take; i++)
            {
                if (this.prerollBeat <= 0) { this.trigger (this.prerollCount === 0); this.prerollCount++; this.prerollBeat = this.prerollGap; }
                this.prerollBeat--;
                const c = this.clickSample ();
                out[0][i] = c; out[1][i] = c;
            }
            this.preroll -= take;
            done = take;
            if (done >= need) return true;
        }

        const transposing = Math.abs (this.ratio - 1) > 1e-4;
        const start = done;
        while (done < need)
        {
            const have = this.accFill - this.accRead;
            if (have <= (transposing ? 2 : 0))
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
            if (! transposing)
            {
                const take = Math.min (need - done, have);
                for (let c = 0; c < 2; c++)
                    out[c].set (this.acc[c].subarray (this.accRead, this.accRead + take), done);
                this.accRead += take;
                done += take;
            }
            else
            {
                // Read `ratio` samples of the stretched signal per output sample.
                while (done < need && this.accRead + 1 < this.accFill)
                {
                    const i = this.accRead, f = this.frac;
                    out[0][done] = this.acc[0][i] * (1 - f) + this.acc[0][i + 1] * f;
                    out[1][done] = this.acc[1][i] * (1 - f) + this.acc[1][i + 1] * f;
                    done++;
                    this.frac += this.ratio;
                    const whole = Math.floor (this.frac);
                    this.accRead += whole;
                    this.frac -= whole;
                }
            }
        }

        // The song clock and its clicks, sample by sample over what was output.
        const dt = this.rate / sampleRate;
        for (let i = start; i < done; i++)
        {
            this.songT += dt;
            if (this.clickOn && this.nextBeat < this.beats.length && this.songT >= this.beats[this.nextBeat])
            {
                this.trigger (this.accents ? !! this.accents[this.nextBeat] : (this.nextBeat % 4) === this.downbeat);
                this.nextBeat++;
            }
            else if (this.nextBeat < this.beats.length && this.songT >= this.beats[this.nextBeat]) this.nextBeat++;
            if (this.voice > 0) { const c = this.clickSample (); out[0][i] += c; out[1][i] += c; }
        }

        // Tell the page where we are, about 25 times a second.
        this.report += need;
        if (this.report >= sampleRate / 25)
        {
            this.report = 0;
            this.port.postMessage ({ type: 'pos', t: this.songT });
        }
        return true;
    }
}

registerProcessor ('stem-player', StemPlayer);
