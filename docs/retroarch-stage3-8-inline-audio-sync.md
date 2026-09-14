# RetroArch H3531 Stage 3.8 — inline AO sync diagnosis

Stage 3.7 physically proved that the H3531 AO transport can output FCEUmm audio, but enabling RetroArch's threaded audio pipeline caused unstable pacing: gameplay/audio ran fast at first and the video PERF window later fell below 60 FPS while AO stayed nearly full.

Pinned RetroArch 1.22.2 documents `audio_driver_t::wait_writable` as the capability that allows a driver to host the threaded pipeline. H3531 AO does not need a separate frontend consumer thread: its blocking `write()` can naturally pace on the hardware queue.

Stage 3.8 therefore keeps the physically proven Stage 3.6 video renderer and Stage 3.7 AO transport, but:

- removes the `wait_writable` callback from the H3531 audio vtable;
- keeps blocking `write()` as the standard RetroArch inline audio path;
- explicitly enables `audio_sync`;
- disables `audio_rate_control` for this hardware-clock validation build so RetroArch does not alter pitch/rate while AO is the master clock;
- keeps AO5/ch0, 48 kHz, S16 mono, 30x160 and the proven 320-byte frame ABI unchanged.

Acceptance on hardware: normal pitch, normal gameplay speed, stable video around 60 FPS, no AO send/query failures.
