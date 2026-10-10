class GP32AudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];
    this.queued = 0;
    this.started = false;
    this.preroll = Math.floor(sampleRate * 0.12);
    this.lowWater = Math.floor(sampleRate * 0.05);
    this.maxQueue = Math.floor(sampleRate * 0.45);
    this.lastL = 0;
    this.lastR = 0;
    /* What the speaker last received, and the level a gap fades out from. */
    this.outL = 0;
    this.outR = 0;
    this.tailL = 0;
    this.tailR = 0;
    this.fade = 0;
    /* About 1 ms glide from the last output onto resumed or trimmed audio. */
    this.rampLen = Math.max(16, Math.round(sampleRate / 1000));
    this.ramp = 0;
    this.rampFromL = 0;
    this.rampFromR = 0;
    this.trimmed = true; /* the first audio fades in from silence */
    this.gen = 0;
    this.consumedSincePost = 0;
    this.underruns = 0;
    this.port.onmessage = (ev) => {
      const m = ev.data || {};
      if (m.type === 'reset') {
        /* Echo the caller's generation, including resets before node creation. */
        this.gen = typeof m.gen === 'number' ? m.gen : this.gen + 1;
        this.queue.length = 0;
        this.queued = 0;
        this.started = false;
        this.lastL = this.lastR = 0;
        /* Fade out whatever was playing instead of cutting it off. */
        this.tailL = this.outL;
        this.tailR = this.outR;
        this.fade = 0;
        this.ramp = 0;
        this.trimmed = true;
        this.consumedSincePost = 0;
        this.underruns = 0;
      } else if (m.type === 'config') {
        if (m.preroll) this.preroll = m.preroll | 0;
        if (m.lowWater) this.lowWater = m.lowWater | 0;
        if (m.maxQueue) this.maxQueue = m.maxQueue | 0;
      } else if (m.type === 'audio' && m.data) {
        const data = m.data instanceof Int16Array ? m.data : new Int16Array(m.data);
        const frames = (m.frames | 0) || (data.length >> 1);
        if (frames > 0) {
          this.queue.push({ data, frames, pos: 0 });
          this.queued += frames;
          while (this.queued > this.maxQueue && this.queue.length > 2) {
            const h = this.queue.shift();
            this.queued -= h.frames - h.pos;
            this.trimmed = true;
          }
        }
      }
    };
  }

  _popSample() {
    while (this.queue.length) {
      const h = this.queue[0];
      if (h.pos < h.frames) {
        const i = h.pos++ << 1;
        this.queued--;
        this.consumedSincePost++;
        if (h.pos >= h.frames) this.queue.shift();
        this.lastL = h.data[i] / 32768.0;
        this.lastR = h.data[i + 1] / 32768.0;
        return 1;
      }
      this.queue.shift();
    }
    return 0;
  }

  /* Gap output: a short linear fade from the level the speaker last had. */
  _gapSample() {
    if (this.fade === 0) {
      this.tailL = this.outL;
      this.tailR = this.outR;
    }
    const gain = this.fade < 96 ? (1.0 - this.fade / 96.0) : 0.0;
    if (this.fade < 0x7fffffff) this.fade++;
    this.outL = this.tailL * gain;
    this.outR = this.tailR * gain;
  }

  _postStat() {
    this.port.postMessage({ type: 'stat', gen: this.gen, queued: this.queued, consumed: this.consumedSincePost, underruns: this.underruns, started: this.started });
    this.consumedSincePost = 0;
    this.underruns = 0;
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const l = out[0];
    const r = out[1] || out[0];
    if (!this.started) {
      if (this.queued >= this.preroll) this.started = true;
      else {
        for (let i = 0; i < l.length; ++i) {
          this._gapSample();
          l[i] = this.outL;
          r[i] = this.outR;
        }
        if (this.consumedSincePost || this.underruns) this._postStat();
        return true;
      }
    }

    for (let i = 0; i < l.length; ++i) {
      if (this._popSample()) {
        let vl = this.lastL, vr = this.lastR;
        if (this.fade > 0 || this.trimmed) {
          /* Join the new audio from what the speaker just played. */
          this.ramp = this.rampLen;
          this.rampFromL = this.outL;
          this.rampFromR = this.outR;
          this.trimmed = false;
        }
        if (this.ramp > 0) {
          const k = (this.rampLen - this.ramp + 1) / (this.rampLen + 1);
          vl = this.rampFromL + (vl - this.rampFromL) * k;
          vr = this.rampFromR + (vr - this.rampFromR) * k;
          this.ramp--;
        }
        this.fade = 0;
        this.outL = vl;
        this.outR = vr;
      } else {
        if (this.started) this.underruns++;
        /* Do not hard-cut to zero.  A short tail is less audible and avoids
           the click caused by a single late main-thread/WASM batch. */
        this._gapSample();
        if (this.fade > 1024 && this.queued < this.lowWater) this.started = false;
      }
      l[i] = this.outL;
      r[i] = this.outR;
    }

    if (this.consumedSincePost >= 1024 || this.underruns) this._postStat();
    return true;
  }
}
registerProcessor('gp32-audio-processor', GP32AudioProcessor);
