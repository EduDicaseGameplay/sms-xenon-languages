#!/usr/bin/env python3
"""Run BSE's fixed-up stage music start against a song that is still stopping.

Usage: python3 tools/mods/test_stage_music.py BUILD/eclipse-src/bse
Pass --compiler with the clang++ path used to build Eclipse if needed.

Leaving Eclipse's character select (Peach's Castle's paintings, once Luigi or
Piantissimo is unlocked) stops its song with a one-second fade. A console is
still reading the stage from the disc when the fade ends; the port can start
the stage first. This compiles AudioStreamer's stop/clear handling and
initStageMusic from the build's sources, on both host widths, and checks that
a stage started during that fade (or during the fade of leaving a stage)
plays its own music: a sequence for Delfino Square (BGM 0x80010001), its
stream for Battle Fort (stream 40). It also builds initStageMusic as it was,
calling finishLeavingStage only, and expects that to fail.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

from test_stage_transition import function

HARNESS = r'''
#include <cassert>
#include <cstddef>
#include <cstdio>
using u8 = unsigned char;
using u32 = unsigned int;
using s32 = int;
using f32 = float;
void OSReport(const char *, ...) {}
template <typename A, typename B> A Max(A a, B b) { return a > b ? a : A(b); }
template <typename A, typename B> A Min(A a, B b) { return a < b ? a : A(b); }
template <typename T> struct P { T v; T get() const { return v; } };
struct TAtomicGuard {};
struct DVDCommandBlock {};
int streamCancels = 0, stageBGMs = 0;
void AISetStreamVolLeft(u8) {}
void AISetStreamVolRight(u8) {}
void AISetStreamPlayState(bool) {}
void DVDCancelStream(DVDCommandBlock *) { ++streamCancels; }
struct TMarDirector { u8 mAreaID = 77, mEpisodeID = 8; } director, *gpMarDirector = &director;
void startStageBGM__10MSMainProcFUcUc(u8, u8) { ++stageBGMs; }
struct JAISound {};
JAISound *handle0 = nullptr;
struct MSBgm { static JAISound *getHandle(u8 track) { return track == 0 ? handle0 : nullptr; } };
u32 gStageBGM;
f32 gAudioSpeed, gAudioPitch, gAudioVolume;
static bool _mIsPlaying, _mIsPaused, _mIsBootOut;
namespace Stage {
struct TStageParams {
    P<bool> mMusicSetCustom{true}, mMusicEnabled{true}, mIsExStage{true};
    P<u32> mMusicID{0x428};
    P<f32> mMusicSpeed{1}, mMusicPitch{1}, mMusicVolume{0.75f};
    P<s32> mStreamLoopStart{-1}, mStreamLoopEnd{-1};
} config;
TStageParams *getStageConfiguration() { return &config; }
}  // namespace Stage
namespace Music {
constexpr size_t AudioQueueSize = 4;
constexpr u8 AudioVolumeDefault = 0x7F;
struct AudioPacket {
    u32 id = 0xFFFFFFFF;
    const char *name = nullptr;
    AudioPacket() {}
    AudioPacket(u32 i) : id(i) {}
    AudioPacket(const char *n) : id(0x12345678), name(n) {}
    u32 getID() const { return id; }
    void setLoopPoint(s32, s32) {}
};
struct AudioStreamer {
    bool mRequestedClear = false, mRequestedStop = false, mRequestedPause = false,
         mRequestedNext = false, mRequestedPlay = false;
    f32 _mDelayedTime = 0, _mFadeTime = 0;
    u8 _mTargetVolume = 0, _mVolLeft = 0, _mVolRight = 0, _mPreservedVolLeft = 0,
       _mPreservedVolRight = 0;
    u32 _mAudioIndex = 0, mStreamPos = 0;
    AudioPacket _mAudioQueue[AudioQueueSize];
    DVDCommandBlock mStopBlock;
    int plays = 0, nexts = 0;
    static AudioStreamer *getInstance() { static AudioStreamer s; return &s; }
    static void cbForCancelStreamOnStopAsync_(u32, DVDCommandBlock *) { _mIsPlaying = _mIsPaused = false; }
    bool isPlaying() const { return _mIsPlaying; }
    bool isPaused() const { return _mIsPaused; }
    AudioPacket &getCurrentAudio() { return _mAudioQueue[_mAudioIndex]; }
    void setVolumeLR(u8, u8) {}
    void setLooping(bool) {}
    void play() { ++plays; }
    void next(f32) { ++nexts; }
    void nextTrack_();
    void finishLeavingStage();
    void finishStoppingForStage();
    bool queueAudio(const AudioPacket &packet);
};
}  // namespace Music
using namespace Music;
'''

SCENARIOS = r'''
static AudioStreamer &fresh() {
    AudioStreamer &s = *AudioStreamer::getInstance();
    s = AudioStreamer();
    _mIsPlaying = _mIsPaused = false;
    streamCancels = stageBGMs = 0;
    return s;
}
// The character select's song playing, then its stopSong(1.0f): a next() with
// a fade, whose stop (and move to the next queue slot) has not run yet.
static AudioStreamer &characterSelectFading() {
    AudioStreamer &s = fresh();
    s._mAudioQueue[0] = AudioPacket("character_select");
    _mIsPlaying = true;
    s.mRequestedNext = s.mRequestedStop = true;
    s._mTargetVolume = 0;
    s._mVolLeft = s._mVolRight = 90;
    return s;
}
int main() {
    // Delfino Square: a sequence (BGM 0x80010001) in a castle stage (ex).
    {
        AudioStreamer &s = characterSelectFading();
        Stage::config.mMusicID.v = 0x1;
        initStageMusic();
        if (stageBGMs != 1) { std::puts("Delfino Square: no stage sequence started"); return 1; }
        assert(!s.isPlaying() && !s.mRequestedStop && streamCancels == 1);
    }
    // Battle Fort: stream 40 (0x428).
    {
        AudioStreamer &s = characterSelectFading();
        Stage::config.mMusicID.v = 0x428;
        initStageMusic();
        if (s.getCurrentAudio().getID() != 40 || s.mRequestedStop || s.nexts) {
            std::puts("Battle Fort: its stream is not the one to play next");
            return 1;
        }
        assert(s.plays == 1 && stageBGMs == 0);
    }
    // Leaving a stage (stopMusicOnExitStage: stop and clear) as before: the
    // castle's Floor 1 to Floor 2, the same stream 28.
    {
        AudioStreamer &s = fresh();
        s._mAudioQueue[0] = AudioPacket(28u);
        _mIsPlaying = true;
        s.mRequestedStop = s.mRequestedClear = true;
        Stage::config.mMusicID.v = 0x41C;
        Stage::config.mIsExStage.v = false;
        initStageMusic();
        assert(s.getCurrentAudio().getID() == 28 && s.plays == 1 && !s.mRequestedClear);
        Stage::config.mIsExStage.v = true;
    }
    // Nothing stopping: a stage whose stream is already current only plays it.
    {
        AudioStreamer &s = fresh();
        s._mAudioQueue[0] = AudioPacket(40u);
        Stage::config.mMusicID.v = 0x428;
        initStageMusic();
        assert(s.getCurrentAudio().getID() == 40 && s.plays == 1 && streamCancels == 0);
    }
    std::puts("ok");
}
'''


def build_and_run(compiler, code, folder, name):
    source = Path(folder) / (name + '.cpp')
    source.write_text(code)
    results = []
    for arch in ('32', '64'):
        binary = Path(folder) / (name + arch)
        subprocess.run([compiler, '-std=c++17', '-m' + arch, '-O2', '-w', str(source), '-o', str(binary)],
                       check=True)
        results.append(subprocess.run([str(binary)], capture_output=True, text=True))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bse', type=Path)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    music = (args.bse / 'src/music.cpp').read_text(encoding='utf-8-sig')
    parts = [function(music, sig) for sig in (
        'void AudioStreamer::nextTrack_()',
        'void AudioStreamer::finishLeavingStage()',
        'void AudioStreamer::finishStoppingForStage()',
        'bool AudioStreamer::queueAudio(const AudioPacket &packet)',
        'static void initStageMusic()')]
    fixed = HARNESS + '\n'.join(parts) + SCENARIOS
    old_init = parts[-1].replace('streamer->finishStoppingForStage();', 'streamer->finishLeavingStage();')
    if old_init == parts[-1]:
        sys.exit('stage-music: initStageMusic does not call finishStoppingForStage')
    old = HARNESS + '\n'.join(parts[:-1] + [old_init]) + SCENARIOS
    with tempfile.TemporaryDirectory(prefix='sms-stage-music-') as folder:
        for arch, r in zip(('32', '64'), build_and_run(args.compiler, fixed, folder, 'fixed')):
            if r.returncode != 0:
                sys.exit('stage-music: %s-bit failed: %s%s' % (arch, r.stdout, r.stderr))
            print('stage-music: %s-bit sequence and stream after a fading song passed' % arch)
        for arch, r in zip(('32', '64'), build_and_run(args.compiler, old, folder, 'old')):
            if r.returncode == 0:
                sys.exit('stage-music: %s-bit: the old initStageMusic passed; the test checks nothing' % arch)
            print('stage-music: %s-bit old initStageMusic rejected (%s)' % (arch, r.stdout.strip()))


if __name__ == '__main__':
    main()
