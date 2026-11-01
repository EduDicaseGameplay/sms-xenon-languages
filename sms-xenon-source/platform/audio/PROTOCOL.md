# JAudio ↔ DSP protocol (as SMS uses it)

This is what the game's CPU side expects of the audio DSP task, and how `platform/audio` answers it.
Every fact here comes from the decomp's own source (file names below), the linker map, or the disc data.
Where the microcode's behaviour cannot be read from the CPU side, the choice made is marked **assumed**, with the evidence used to pick it.

Sources: `libs/JSystem/src/dsptask.c` (DspBoot, DSPSendCommands2, DspStartWork/DspFinishWork), `libs/JSystem/src/dspproc.c` (DsetupTable, DsyncFrame, DSPReleaseHalt), `libs/JSystem/src/osdsp_task.c` (__DSPHandler, DsyncFrame2), `JASAudioThread.cpp`, `JASDSPBuf.cpp`, `JASAiCtrl.cpp`, `JASDSPInterface.{hpp,cpp}`, `JASDSPChannel.cpp`, `JASChannel.cpp`, `JASDriverIF.cpp`, `JASDriverTables.cpp`, `JASRate.cpp`, `JAIGFrameStream.cpp`, `JAIData.cpp`.

## Timing

`JASRate.cpp`: DAC rate 32028.5 Hz (the AI's 32 kHz mode, `AISetDSPSampleRate(0)`), a frame of 0x230 = 560 samples, split into 7 subframes of 80 samples.
The AI DMA block is 0x460 halfwords = 560 stereo s16 pairs (17.5 ms).

## Mailbox framing

`DSPSendCommands2(words, n, cb)`: wait for `Dsp_Running_Check`, send mail `n`, `DSPAssertInt()`, then `n` words (`n == 0` means 2 words).
If `cb` is set, `DspStartWork(words[0], cb)` queues `words[0] >> 16` as the id to acknowledge.

Replies come through the DSP interrupt.
`__DSPHandler` reads a first mail; `0xDCD10004` calls the audio task's `req_cb` (`AudioThread::syncDSP`), which reads a second mail:

| Second mail | Meaning | CPU action |
| --- | --- | --- |
| `0xF355FF00` | subframe rendered | message 1 to the audio thread: next subframe, or finish the frame |
| `0xF355xxxx`, other | command acknowledged | `DspFinishWork(xxxx)`: runs the queued callback if `xxxx` matches |

`0xF355` is `DSPInterface::JAS_DSP_PREFIX`.
Boot: the task start mail `0xDCD10000` makes the handler call `init_cb` (`DspHandShake`), which waits for one more mail and then sets `Dsp_Running_Start`.

## Commands

| Words | Sender | Meaning |
| --- | --- | --- |
| `0x81000000 \| n`, `CH_BUF`, `DSPRES_FILTER`, `DSPADPCM_FILTER`, `FX_BUF` | `DsetupTable` (from `DSPInterface::setupBuffer`) | voice count and table addresses; the CPU spins until it is acknowledged (`0xF3558100`) |
| `0x82000000 \| subframes << 16 \| level`, `outA`, `outB` | `DsyncFrame` (from `DSPBuf::finishDSPFrame`) | start a frame: `subframes` × (frame/`subframes`) samples into `outA` and `outB`, which are the two halves of one `dsp_buf` (so frame = `(outB − outA) / 2` samples); `level` = `DSP_MIXERLEVEL`. No acknowledgement expected |
| (count 0) map hi, map lo | `DSPReleaseHalt` (from `DSPBuf::updateDSP`) | the 64-bit enabled-voice map (voice 0 = MSB); the DSP renders the next subframe of the current frame and replies `0xF355FF00` |

A frame therefore runs: `0x82` → release (CPU has just run `TDSPChannel::updateAll`) → subframe 0 → `FF00` → CPU updates channels → release → subframe 1 … → the 7th `FF00` makes the CPU finish the frame and, if a triple-buffer slot is free, start the next (`DSPBuf::process`).

## Output path

`Kernel::vframeWork` (on each AI DMA interrupt) takes the DSP's finished buffer and interleaves it with `Calc::imixcopy(outB-half, outA-half, dac)`, so DMA pairs are (outB, outA).
It then adds CPU-mixed extras through `extMixCallback` (THP movie audio, which `audio-05` scales by the music volume) and hands the block to `AIInitDMA`.
The DMA callback (`syncAudio`) is the whole audio clock: each block's start wakes the audio thread (message 0 → `Kernel::updateDac`).

## Voice block (`DSPInterface::DSPBuffer`, 0x180 bytes × 64)

Written by the CPU field by field, so on the PC port they are in host byte order.
Fields the DSP writes back are marked ←.

| Offset | Field | Meaning |
| --- | --- | --- |
| 0x000 | `enabled` | set by `playStart`, cleared by the CPU (`replyFinishRequest`, `stop`) |
| 0x002 | `done` ← | the voice has finished: one-shot end, or stop request completed. `TDSPChannel::updateAll` then calls back and clears `done`/`enabled` |
| 0x004 | `resamplingRatio` | Q12 step (0x1000 = one source sample per output sample); `4096 × pitch × waveRate / 32028.5`, capped 0x7FFF. Streams use `(rate << 12) / 32000` |
| 0x006 | (port) volume class | not a DSP field: padding that `TChannel::playLogicalChannel` writes (`decomp-patches/audio-05`) when it starts the voice; 1 = a BGM sequence's voice (follows Music Volume), 0 = anything else (SFX Volume). Streams (source 0x21) are music whatever it says. README.md, Music and SFX volume |
| 0x008 | `resetVpb` | set by `playStart`: restart decoding (the DSP clears it) |
| 0x00A | `endReached` ← | set with `done` at a one-shot's end |
| 0x00C | `useConstantSample` | pause flag: no output, no advance |
| 0x00E | `samplesToKeepCount` | init delay max (surround delay; **ignored**) |
| 0x010 | `mixChannels[6]` {bus, target, current ←, level} | six sends: bus address (below), Q15 target volume, current volume (ramps to target over one subframe), delay bytes (**ignored**) |
| 0x050–0x058 | auto mixer: position (pan << 8 \| dolby), fx << 8, volume current ←/target, enable | used instead of `mixChannels` when the channel's mix config is 0xFFFF (`updateAutoMixer`); SMS uses this for most sound effects |
| 0x060, 0x068 | position fraction ←, position ← | for streams: `currentPosition >> 16` is read by `Get_DirectPCM_Counter` |
| 0x06C | `samplesBeforeLoop` ← | streams: samples to the ring's end, 16.16 (`Get_DirectPCM_LoopRemain`) |
| 0x074 | `remainingLength` ↔ | streams: samples still to play (0xFFFFFFFF = endless); `Get_DirectPCM_Remain` |
| 0x100 | `samplesSourceType` | 9 = ADPCM4, 5 = ADPCM2, 8 = PCM8, 16 = PCM16 (from `setWaveInfo`'s `COMP_BLOCKBYTES`), 0x21 = direct PCM (streams); other values are oscillators (`setOscInfo`, below) |
| 0x102 | `isLooping` | |
| 0x104/0x106 | `loopYN1`/`loopYN2` | ADPCM history for the loop |
| 0x108 | `filterMode` | bit 5: biquad (IIR) on, low 5 bits: FIR8 length (the FIR8 is **not applied**) |
| 0x10A | `endRequested` | `TDSPChannel::forceStop`: halve current volumes each subframe until zero, then `done` |
| 0x10C | `unk10C` | CPU-owned age counter (voice stealing) |
| 0x110 | `loopAddress` | loop start, a **sample index** (stored through an `s16*`); streams: ring start |
| 0x114 | `endPosition` | loop end, or sample count; streams: ring length << 16 |
| 0x118 | `baseAddress` | ARAM byte address of the wave; streams: main-memory `s16*` ring |
| 0x11C | `sampleCount` | |
| 0x120, 0x148, 0x150 | FIR8, IIR (biquad), distance low-pass coefficients | biquad and low-pass applied (Voice filters, below); FIR8 **not applied** |

Bus addresses (`setBusConnect`'s `connect_table`, index = the mix config's high byte): 1 = 0x0D00, 2 = 0x0D60, 3 = 0x0DC0, 4 = 0x0E20, 5 = 0x0E80, 6 = 0x0EE0, 7 = 0x0CA0, 8 = 0x0F40, 9 = 0x0FA0, 10 = 0x0B00, 11 = 0x09A0.
The default mix config (`JASChannelMgr`: 0x150, 0x210, 0x352, 0x412) sends vol·sin(1−pan) to bus 1, vol·sin(pan) to bus 2, and the same times fxmix to buses 3 and 4.
Streams send voice *i* to bus *i*+1 at 0x7FFF.
So bus 1 is left and bus 2 right (pan 0 → bus 1), `outA` carries bus 1 and `outB` bus 2 (**assumed**: the only reading consistent with the pan law and the stream routing), and DMA pairs are (right, left).

## FX lines (`DSPInterface::FXBuffer`, 0x20 bytes × 4)

`setFXLine` from `JAIData` with the AAF's FX scene table (cmd 7): mode (0x00), delay length in 80-sample subframes (0x02), a main-memory delay buffer (0x04), bus/gain pairs `SEND_TABLE[cfg.unk2]`/`cfg.unk4` (0x08/0x0A) and `SEND_TABLE[cfg.unk6]`/`cfg.unk8` (0x0C/0x0E), and 8 FIR taps (0x10).
SMS's scenes: lines 0/1 return to buses 1/2 (gain 4096 or 2047), second target bus 8, delay 48 subframes (120 ms), taps `0 0 846 846 4715 8343 13421 −4096`.
Each line reads its circular buffer in main memory before voices are mixed. It
prepends the previous eight raw samples, in chronological order, and applies
an eight-tap Q15 FIR over that forward window. Mode bit 0 filters before
returning the signal; mode bit 1 filters after the returns, for feedback.
Both return gains are Q15 (`4096` = 1/8), and either return can target any
supported bus. The resulting feedback occupies bus 3+*i*; new voice sends are
added before that bus is written back into the game's circular buffer.
Using the real buffer also respects the CPU's `setFXLine` clears.

Sunshine's early Zelda DSP uses a Q16 auto-mixer reverb factor: `fx << 8`
is divided by 65536, so the game's maximum `127 << 8` is just under 0.5.
Treating the high byte as a fraction of 127 doubled footstep wet sends;
the boot jingle's zero reverb could not reveal that error. The dry volume
and master calibration remain unchanged. The auto mixer sends front wet
audio to buses 5/6 (lines 2/3), while explicit
music mix configurations send to buses 3/4 (lines 0/1). Stop requests halve
current volumes each 80-sample subframe, rather than cutting a voice off in
one subframe. These protocol rules are documented by the development oracle
and [Dolphin's Zelda DSP implementation](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp).
`tests/mixer_regression.cc` checks impulse gain, both filter modes, history,
RAM clears, cross-bus returns, Q16 wet gain/routing, unchanged dry levels, and release
through complete silence without using disc assets.

## Voice filters

The microcode filters a voice's resampled samples before any send: first the
one-pole distance low-pass (`lowPassCoeff`, 0x150, applied when non-zero), then
the biquad (`biquadFilterCoeffs`, 0x148, when `filterMode` bit 5 is set and the
coefficients are not the identity `0x7FFF, 0, 0, 0` that `initFilter` writes).
Both follow Dolphin's Zelda HLE sample for sample:

- low-pass: `y[n] = y[n-1] + ((x[n] - x[n-1]) * c >> 7)`, history (y, x) at 0x0D0, cleared on `resetVpb`;
- biquad: `y[n] = (b1 x[n-1] + b2 x[n-2] + a1 y[n-1] + a2 y[n-2]) >> 15`, history (x1, x2, y1, y2) at 0x0A8, which `playStart` clears;
- every output saturates to s16.

JAudio fills them from a track's four IIR parameters (`__UpdateJcToDSP`:
`setIIRFilterParam`, `setFilterMode`, `setDistFilter`). A sequence picks a row
of `Player::CUTOFF_TO_IIR_TABLE` (128 rows from a resonant low-pass at 0 to the
identity at 127) with its IIR cutoff command. Sunshine's sound effects ask
`MSSeCallBack::setParameterSeqSync` (sync 12 and 13) for that row, which
answers `0x7F` minus `MSSeCallBack::smWaterFilter`. `TMap` sets the filter to
0x78 while the camera is below the water surface (`setWaterCameraFir`), so
effects then play through row 7: about +12 dB at 50 Hz, -1 dB at 1 kHz,
-13 dB at 4 kHz, -18 dB at 8 kHz. That is the under-water muffle; before the
port applied these filters, it was missing. Case 12 steps the level by 0x14
per call, and the sequence turns that into rows 8, 28, ... 108 on the way in
and 28, ... 108, 128 on the way out, so the muffle fades in and out. Row 128
is one past the table; the console reads the next `.data` (`s_key_table`)
there, and `decomp-patches/bounds-08` gives the host those bytes. They leave
a mild low-pass on the sounds that were playing as the camera left the water,
until they end; sounds started after that get row 127 (sync 13).
`SMS_AUDIO_FILTERS=0` bypasses both filters (A/B checks), and `SMS_AUDIO_TRACE=1`
logs each change of a voice's filter settings.
The FIR8 (`filterMode` bits 0–4, coefficients at 0x120) is still not applied;
Dolphin does not implement it either, and a voice that asks for it logs once.

## Levels (measured against retail)

Reference: Dolphin (the dolphin-oracle build) running the retail disc with **DSP LLE**, i.e. the game's own microcode, with `DumpAudio` capturing the AI DMA stream.
Boot → title → file select from `movies/title.dtm`, in `runs/audio-retail-lle` of the oracle tree.
Dolphin's DSP HLE gave the same levels (the boot jingle peaks at 13760 in both).

- **Master** (`DSP_MIXERLEVEL`, SMS sets 0x5000): **Q15**, i.e. 0x5000 = 0.625×.
  The boot jingle (an auto-mixer voice) then matches retail sample for sample: correlation 1.000 and gain 0.994–1.001 on both channels (Q14 gave 0.497).
- **Auto-mixer volume** (0x056): Q15 with a sine/cosine pan law (0.707 each at centre); fixed by the same jingle match.
- **`mixChannels` volumes**: **Q13**, so JAudio's cap `MAX_MIXERLEVEL = gain × 16384` is 2.0. This is where the music goes: BGM channels use explicit mix configs, sound effects mostly the auto mixer.
  Evidence (2026-10-25): `movies/title.dtm` (boot → title → file select) under Dolphin with DSP LLE and with DSP HLE gives the same whole-mix level, second by second within about 1 dB and −25.7/−25.9 dBFS over 20–66 s, where music voices dominate (a port trace of the same inputs starts 1316 `mixChannels` music voices and 143 auto-mixer SFX voices). Dolphin's Zelda HLE mixes these sends as `vol × sample >> 16` and applies the master as 4.12 (`level >> 12`), which is Q13 against this mixer's Q15 master; its auto-mixer path works out to this mixer's Q15 exactly (the boot jingle peaks at 13760 in all three). The port with the same inputs, which cannot follow the movie frame for frame, measures −25.73 dBFS over 20–66 s at Q13 and −28.24 at Q14, and its 6 s windows aligned on the envelope sit within about ±2 dB of retail at Q13 and 3–6.7 dB under it at Q14.
  The earlier choice, Q14, came from replaying field-rate snapshots of retail's voice blocks (`CH_BUF` over 600 fields of file-select music), which still came out 1.2–1.3× quieter than retail's dump. Snapshots miss what changes between fields, so that method read low. At Q14 the music played 6 dB under the sound effects (sms-launcher#127).
- These three scales cannot be told apart from "`mixChannels` Q15, auto-mixer at quarter, master Q13": every path gives the same output.
  Streams (0x7FFF sends) therefore play at 2.5×, as in Dolphin's HLE, and their loudest peaks can saturate. No retail code starts one.
- `SMS_AUDIO_MASTER_SHIFT` and `SMS_AUDIO_SLOT_SHIFT` override the two shifts.

## Bus → channel

In retail file select, every voice uses buses 1–4 (0x0D00/0x0D60/0x0DC0/0x0E20), with no FX sends; the auto-mixer voices' `mixChannels` hold garbage bus words, as on the port.
Bus 1 comes out as the dump's left channel: the replay's bus 1 is louder like retail's L (L/R rms 1.13 replay, 1.33 retail), and envelope correlation is bus1–L 0.60 against bus2–L 0.38.
The port's (right, left) DMA order plus the swap for SDL/WAV output is therefore right.

## Timing

Two port-side problems made the music irregular and thin; both are fixed:

- **AI pacing.** DMA blocks were consumed as fast as interrupts could be delivered after a stall, before the audio thread had rendered the next DSP frame, so JAudio's `DSPBuf` repeated stale samples (gaps) while the sequencer fell behind.
  The AI now starts a block only when no DSP frame is pending (`port_audio_dsp_frame_pending`).
- **Voice stealing.** `TDSPChannel::updateAll` measures DSP load with `OSGetTick` gaps between subframe updates and calls `breakLowerActive`.
  On the port those gaps measure host scheduling, and it cut most notes (median life 20 subframes against retail's 7 fields ≈ 47).
  `decomp-patches/audio-03` skips the guard on TARGET_PC.
  Afterwards the port averages 11–13 simultaneous voices at file select (retail 10.9), and the music floor is continuous (quietest-quartile energy 0.6–0.8 of the mean; retail 0.5–0.65, before the fix 0.04–0.18).

## Surround (Dolby Pro Logic II)

In stereo output mode (the IPL default, and what the port's `OSGetSoundMode` returns) JAudio routes only to buses 1–4 and remaps bus 8 to 11.
Retail used nothing else in the traced segment.
Surround mode (output mode 2, from the in-game option menu) uses buses 8/9 and the auto mixer's dolby byte; the microcode's matrix encoding into the stereo pair is **not verified** (buses 8/9 are currently dropped).
An oracle run that selects surround in the options menu would settle it.

## Wave data

`.aw` files hold the waves at the WSYS table's offsets; `JASWaveArcLoader` copies them into ARAM, so `baseAddress` is an ARAM address and `port_aram_ptr` resolves it.

- **ADPCM4** (AFC, 9-byte frames of 16 samples): header byte = scale exponent (high nibble) and predictor index (low nibble) into `DSPADPCM_FILTER` (16 × {c1, c2}, Q11); then 16 signed nibbles, high nibble first.
  `y = sat16(((n << scale) << 11) + c1·y₋₁ + c2·y₋₂) >> 11)`.
- **ADPCM2** (5-byte frames): 16 signed 2-bit values, high bits first, scaled as `n × 4` on the 4-bit scale above.
- **PCM8**: signed 8-bit. **PCM16**: big-endian s16.
- **Loops**: the stored `loopYN1/YN2` are y₋₁/y₋₂ of the ADPCM frame that contains the loop start.
  The decoder resumes at that frame with that history and decodes forward to the loop start.
  Looped waves are stored only up to the loop end, so the table's sample count can exceed the data.

`tests/audio_test` checks the decoders against the disc: decoding every looped wave from its start reproduces the stored loop history exactly for all 449 ADPCM4 and 3 ADPCM2 loops (which confirms the nibble order, the scale and 2-bit rules and the predictor table).

## Oscillators

`TChannel::playLogicalChannel` calls `setOscInfo(n)` for a note whose program is 0xF0 + *n* (`JASBankMgr`'s `noteOnOsc`): base address 0 and source type *n*.
There is no wave; the DSP synthesises the voice until the CPU stops it.
The Gelato Beach Dune Buds (`TSandBomb`) use types 4 and 7 for their four filling sounds (`MSD_SE_OBJ_SANDBOMB_WATER_1`..`_4`) and their deflating sound.

`noteOnOsc` sets the pitch as `16736.016 / dacRate × C5BASE_PITCHTABLE[key]`, so a ratio of 0x1000 must be one period every 16736.016 / 523 = 32 output samples.
The voice output is not resampled.

| Type | Output |
| --- | --- |
| 0 | square, 50 % duty, ±0x4000 (first half positive) |
| 1 | saw: the period's 16-bit phase read as s16 |
| 3 | square, 25 % duty, ±0x4000 |
| 7, 4, 11, 12 | patterns 0–3: 64 s16 samples each, the second 0x200 bytes of the setup table (`DSPRES_FILTER` halfwords 0x100–0x1FF) |
| 10 | pattern 0 with a step modulated by another buffer; played as plain pattern 0 (**assumed**: unused by SMS as far as traced) |

The game's patterns 0 and 1 are a sine and a triangle; 2 and 3 are silent except for a two-sample click at the end.
The source numbering, the pattern location and the duty cycles follow [Dolphin's Zelda DSP HLE](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp), which treats SMS's microcode as one of that family; the 32-sample period agrees with JAudio's constant.
`tests/mixer_regression.cc` checks the period, the pattern lookup and phase across subframes, the duty cycles, and that the fields left by a previous wave do not end the voice.

## Resampling

`DSPRES_FILTER`'s first 256 halfwords are 64 phases × 4 taps (Q15, each phase sums to 0x8000), applied to x[n−1], x[n], x[n+1], x[n+2] with phase = fraction >> 10.
The position advances by `ratio << 4` in 16.16 per output sample.
Without the game's table (the WAV test) a Catmull-Rom table is built instead.

## Not implemented

- Oscillator type 10's modulated step (played as plain pattern 0).
- The per-voice FIR8 filter and the surround delay bytes.
- Buses 5–11 other than as FX inputs (Dolby surround output mode).
- DVD audio streaming (DTK `AISetStreamPlayState`, `JASHardStream`): SMS's streams go through DirectPCM voices instead (the retail dump's DTK track was silent throughout).
- Stream voices (DirectPCM, source 0x21) are not exercised by boot/title/file select: `title.afc` is not the title music (correlation ≤ 0.2 against the decoded file), so they remain untested against retail.
