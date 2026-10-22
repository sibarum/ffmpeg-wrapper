# ffmpeg-wrapper

Panama (FFM) bindings for one C ABI over every video FFmpeg can decode, the sound that goes with it, and
AV1 recorded in real time from frames as they are made. Hand-written downcalls over a thirty-five-function
header and a small C shim behind it, with no jextract, no JNI and no code generation.

```java
try (Video video = Ffmpeg.open(path)) {
    VideoInfo info = video.info();     // 1920x1080, 42.0 s, 29.97 fps, "h264", "mov,mp4,...", audio
    video.output(640, 360);            // every frame scaled to this from now on
    for (Frame f; (f = video.next()) != null; ) {
        f.rgba();                      // 640 * 360 * 4 bytes, straight RGBA8, ready to upload
        f.ptsMicros();                 // when it is shown
    }
    video.seek(12_500_000);            // exact: the next frame is the one showing at 12.5 s
}

try (Audio sound = Ffmpeg.openAudio(path)) {
    sound.play();                      // out of the speakers, decoded on a thread of its own
    sound.clockMicros();               // the time being heard now: show the frame due then
    sound.seek(12_500_000);            // exact, playing or paused
    sound.compression(0.5f);           // night mode: quiet raised, loud lowered, peaks held to -6 dBFS
}

try (Recording rec = Ffmpeg.record(Path.of("window.mkv"), 2560, 1440, Recording.Settings.DEFAULT)) {
    rec.frame(rgba, w, h, presentedMicros);  // RGBA8 in, any size on a 2560x1440 canvas; AV1 out
}                                            // the file is whole once this returns
```

## The one idea

**The caller never learns a codec.** Every file opens the same way and every frame comes out as RGBA8
at the size asked for. It has already been converted from whatever YUV layout, matrix and range the
stream used. The codec's name is in `VideoInfo` to be shown, never to be branched on. This is
imagelib-wrapper's rule, applied to video: format knowledge stops at this boundary.

**FFmpeg's structs never cross into Java.** FFmpeg's API is a few hundred structs whose layouts move
between major versions, and binding them from Java would mean regenerating every time FFmpeg does. So a
shim in C (`native/ffwrap/`, about 900 lines) is the only code that knows them. It exposes two
opaque handles and numbers. Moving to a newer FFmpeg is a recompile of the shim, and the Java does not
change.

## Formats

Whatever the bundled FFmpeg decodes, which is nearly everything people keep video in:

| | |
| --- | --- |
| Video codecs | H.264, HEVC, VP8, VP9, AV1 (dav1d), MPEG-1/2/4, ProRes, DNxHD, Theora, MJPEG and FFmpeg's other built-in decoders |
| Containers | MP4/MOV, Matroska/WebM, AVI, MPEG-TS, FLV, Ogg, 3GP, ... |

Pixels are always **straight RGBA8, sRGB, tightly packed, top row first**, which is imagelib's layout.
A video frame can therefore go through everything a picture already goes through. The YUV matrix is
the stream's own. Where the stream says nothing (most files), BT.709 is used at 720 lines and above
and BT.601 below, which is the convention. Full and limited range are both honoured.

Frames come out at the **display** size: the sample aspect ratio is applied to the width, so anamorphic
DVD video is not squashed. A rotation in the file's display matrix (phone video shot upright) is
**not** applied yet.

## Sound

A `Video` reports that its file has sound (`VideoInfo.audio()`) and does not decode it. `Ffmpeg.openAudio`
opens the same file's sound as an `Audio`: a second handle, with its own demuxer and decoder, so a video's
exact, frame-numbered seeking never waits on the sound or the other way round.

**The sound never comes into Java.** The shim decodes it on a thread of its own, about half a second
ahead, resamples it with libswresample to float stereo at the output's own rate (5.1 is mixed down,
mono goes to both sides), and hands it to [miniaudio](https://miniaud.io), which plays it out of the
system's default output (WASAPI on Windows, PulseAudio or ALSA on Linux) and follows that output when it
changes. Java only steers it, with play, pause, seek, volume and compression, and reads its clock back,
and a copy of the last quarter-second given to the device (`heard`) for a meter or a scope. The audio
thread never calls into Java, so a garbage collection cannot make the sound stutter.

**The clock is what a player keeps time by**: `clockMicros()` is the time of the sound being *heard*
now. It counts what the device has taken, takes off what the device holds but has not played yet, and
runs on smoothly between the device's periods (10 ms on WASAPI). It never goes back except by a seek.
It holds while paused. After a seek it says the place sought at once and stays there until the sound from
there starts to be heard. A gap in the stream is filled with silence, so the count stays the stream's
time. Showing each frame when this clock reaches it keeps the picture with the sound, the way players do.

`Audio.Output.NOWHERE` plays to miniaudio's null device, which drains the sound in real time and drops
it. The clock runs as it would with speakers, which is what the tests use on a machine with no sound
card.

### Compression

`compression(amount)` is dynamic range compression on one knob, from 0 (as stored, the default) to 1:
dialogue comes up, explosions come down. The knob is not a wet/dry mix. It moves every setting together:

| | 0.25: the extremes | 0.5: night mode | 1: too much |
| --- | --- | --- | --- |
| Threshold | ±6 LU around −18 LUFS left alone | none: all pulled towards −18 | none |
| Ratio | 1.5:1 | 3:1 | ∞:1, one loudness |
| Quiet raised by at most | 0 dB | 12 dB | 30 dB, room tone and hiss too |
| Peak ceiling | −3 dBFS | −6 dBFS | −12 dBFS |
| Release | 10 dB/s | 20 dB/s | 80 dB/s, levelled by syllable: it pumps |

The top is too much on purpose. Turned all the way up, the effect cannot be missed, and that is how
an ear learns to hear it lower down. Most normalizers are so timid that even fully on nobody can tell
they are there. It still does no harm at any setting: above 0, no sample is played above that
setting's ceiling, which is never above 0 dBFS, and overall loudness tops out at the target.

**Measures, not gains.** `native/ffwrap/level.c` has two halves. The decoder's half gives every sample
three measures on its way into the ring:

- **Slow loudness:** the median of 10 ms K-weighted blocks (ITU-R BS.1770) over 0.31 s. A bang or a
  pause between words does not move it.
- **Quick loudness:** the same over 50 ms, for the top of the knob.
- **Peak:** in dBFS.

The device's half turns those into a gain by the knob as each sample is played, so turning it is heard
at once, mid-drag, with nothing decoded again. The releases run there too, sample by sample. Every
step that shapes a gain can only lower it, which is what keeps the ceiling a guarantee.

**The lookahead is real, and nothing waits for it.** A compressor on a live signal can only see ahead by
holding the signal back, and everything else, the picture included, has to wait with it. A file is not
live, and the decoder is half a second ahead already. So the decoder's half holds samples until about
0.37 s of what follows has been decoded. Each measure is taken at the loudest point near its sample,
which is the same as the least gain near it, so the gain comes down *before* a loud passage or a bang,
not after. Every sample keeps its place in the stream's time, and the clock counts samples played, so
it is exactly what it is without compression. After a seek, the first sound waits for that 0.37 s to be
decoded, which takes about a millisecond.

Measured by `CompressionTest`:

- A tone at 0.01 then 0.7 is 36.9 dB apart as stored. It is 35.2 dB apart at 0.25, 17.4 at 0.5, and 0.0
  at 1.
- A 20 ms bang at 0.95 in a quiet scene plays at −3, −6 and −12 dBFS at 0.25, 0.5 and 1.
- The quiet around that bang comes up 0, 12 and 22.7 dB at the same settings.
- Turning the knob from 0 to 1 mid-play is heard within 80 ms (+25.7 dB).
- 0.4 s after a seek to 1.0 s, the clock reads 1.37–1.40 s.

## Recording

`Ffmpeg.record` goes the other way. It takes RGBA8 frames as they are made and writes **AV1** into
Matroska or WebM, whichever the file's name says (`.mkv`, `.webm`). It is meant for recording a window
while it runs. The encoder and the muxer were already in the pinned build, so recording added no library,
only `native/ffwrap/encode.c` and six functions in the header.

**Every frame brings its own time.** Matroska stamps each frame to the millisecond, so frames need not
come at a steady rate. A window that draws nothing while idle is recorded as it was: the pause lasts as
long as it did, and no frame is repeated. Times must rise. A frame that falls in the last frame's
millisecond is dropped (`frame` returns false), because the file could not tell the two apart. The
rate in `Settings` is a hint for rate control and the keyframe interval (one every five seconds), not
a schedule.

**A canvas, so a window can be resized while it is recorded.** The size given to `record` is a canvas,
and every frame in the file is that size. Each frame handed in carries its own size. A smaller one goes
at the canvas's top left, unscaled, and the rest of the canvas is black. A larger one is cut to the
canvas. The canvas is meant to be the largest a frame will be, such as the screen's size.

AV1 itself can change size from frame to frame. Matroska's track header holds only one size, though, and
players differ on whether they follow the bitstream or the header. A fixed canvas plays everywhere and
keeps every pixel sharp. The black is almost free in the file: a 960x540 picture on a 1920x1080 canvas
made a file 4% larger than the same picture at 960x540. It is not free in time, because the encoder
works on the whole canvas. That same file took about three times the CPU. So a recording costs what its
canvas costs (about 7 ms a frame at 1080p), however small the window.

**Real time.** Two encoders can be asked for:

| `Recording.Encoder` | Encoder | 1080p, on the Core Ultra 7 255HX laptop this was built on |
| --- | --- | --- |
| `SOFTWARE` (the default) | SVT-AV1, in its real-time mode (`rtc=1`: no look-ahead, no reordering), preset 10, or 12 above 1440p | about 7 ms a frame |
| `HARDWARE` | the GPU's: `av1_nvenc`, then `av1_qsv`, then `av1_amf`, whichever opens first, and SVT-AV1 when none does | about 4 ms a frame (NVENC) |

Both times count the conversion from RGBA, so either encoder keeps up with 60 fps with time to spare.
Software is the default because it gives the same file on every machine and has AV1's screen-content
tools (palette and intra block copy), which are what flat UI and text need. Its screen-content detection
is left at SVT-AV1's default (`scm=2`, decide per frame). Whether that switches the tools on for a real
window in real-time mode has not been measured. Forcing them (`scm=1`) cost about a fifth more CPU on a
synthetic test picture. `recording.encoder()` names the encoder that opened.

On Linux, NVENC and Quick Sync are tried the same way. VAAPI, which is how most Intel and AMD GPUs encode
on Linux, needs frames uploaded to the GPU first and is not wired yet, so those machines record with
SVT-AV1.

**Quality** is AV1's quantiser scale, from 0 (best) to 63 (smallest). It is SVT-AV1's `crf`, and each
hardware encoder gets the same place on its own scale. `Settings.TEXT`, 30, keeps an interface's small
text legible.

**Colour.** Frames are converted with libswscale to 4:2:0 using the BT.709 matrix at **full** range, so no
shade of the interface is squeezed into 16 to 235. The stream is tagged with BT.709 primaries and the
sRGB transfer, and the decoder half reads that tagging back. Odd sizes, of the canvas or of a frame,
are kept as they are.

**Straight from the GPU.** `frame(MemorySegment, w, h, Pixels, pts)` takes native memory, such as a
mapped readback buffer, and reads it in place. `Pixels.BGRA` is the order a swapchain and a DXGI back
buffer hold, so a presented frame is encoded with no copy and no channel swap in Java. A `byte[]` frame
is RGBA, as a `Frame` is, and is copied into a native buffer once.

**A still window lasts until the end.** `repeat(pts)` encodes the last frame again at a later time,
which costs almost nothing. A recording stopped while its window has drawn nothing for ten seconds
calls it, so the file lasts until the stop instead of ending at the last change.

**No frame rate on the stream.** The rate in `Settings` goes to the encoder only. Declared on the
stream, Matroska would write it as every frame's default duration, and a variable-rate recording would
contradict it on every frame. Tools that need a rate guess one. `ffmpeg -f null` then complains about
timestamps that are fine, and `-c copy` or `-fps_mode vfr` does not.

**It survives a crash.** Matroska keeps a cluster in memory until it closes, by default up to five
seconds or 5 MB, and the file's own buffer holds more on top. A process killed mid-recording used to
leave a file of 0 bytes. Now a cluster closes every second and every packet goes to the disk as it is
written. A killed recording plays up to about its last second, and is missing only what a clean finish
adds: the keyframe index and the stated duration, which players do without.
`anUnfinishedRecordingPlaysUpToItsLastSecond` reads a file whose writer is still open to check this.

SVT-AV1 prints its configuration to stderr when it opens. The shim sets `SVT_LOG=0` unless the host has
set it, which keeps SVT-AV1 quiet the way FFmpeg's own log is kept quiet.

`frame` encodes before it returns. A caller that must not wait, such as a render loop, hands frames to
a thread that calls it. `vexelray-codec-video`'s `Recorder` is that thread.

## Licence: LGPL, shared

The bundled FFmpeg is BtbN's **LGPL shared** build. It contains FFmpeg's own decoders plus the
permissively licensed libraries BtbN links in (dav1d, libvpx, openh264, ...). Nothing GPL is in it, so
there is no x264, x265 or libpostproc. Decoding needs none of those, and recording uses SVT-AV1, which is
BSD-licensed and linked into BtbN's `avcodec`, or the GPU vendor's own driver. FFmpeg stays a set of separate
libraries beside the shim, which a user can replace, and that is what the LGPL asks for.
`FFMPEG-LICENSE.txt` and `FFMPEG-SOURCE.txt` (which build, and where its source is) travel in the jar
beside the libraries, under `natives/<platform>/`.

miniaudio is compiled into the shim. Its author offers it as public domain or MIT-0, so it asks for no
notice.

## Layout

```
native/include/ffwrap.h      the contract: 33 functions, 3 opaque handles, no struct crosses
native/ffwrap/ffwrap.c       pictures: libavformat, libavcodec, libswscale
native/ffwrap/audio.c        sound: libavformat, libavcodec, libswresample, miniaudio; its thread and clock
native/ffwrap/encode.c       recording: libswscale to YUV, an AV1 encoder, libavformat to Matroska/WebM
native/ffwrap/miniaudio.c    miniaudio's implementation (device layer only, see ma.h)
native/build.sh              fetch the pinned FFmpeg and miniaudio, compile the shim, stage it with FFmpeg into native/dist/bin/<platform>/
src/main/java/sibarum/ffmpeg/
    Ffmpeg.java              open a file, or its sound, or record one; FFmpeg's version
    Video.java               an open file: info, output size, next frame, seek, close
    Audio.java               its sound: play, pause, seek, volume, clock, close
    Recording.java           a file being recorded: frame, encoder, bytes, close
    VideoInfo.java, Frame.java, VideoException.java
    CApi.java                one line per header function
    NativeLib.java           find, extract, load: six libraries, in order
```

`CApi` and `NativeLib` are package-private on purpose: the ABI is an implementation detail of the public
types, not a second API.

## Building

```bash
./native/build.sh    # fetches FFmpeg and miniaudio, stages native/dist/bin/<platform>/
mvn install          # bundles it into the jar; tests need --enable-native-access (surefire is set up)
```

`build.sh` pins one BtbN autobuild (`RELEASE` and `BUILD` at its top, a month's last build, which BtbN
keeps) and checks the download against its sha256. A bump changes those three lines together. miniaudio
is pinned the same way (`MINIAUDIO` and `MINIAUDIO_SHA`), one header fetched into `native/miniaudio/`.
The native side needs a C compiler and nothing else:

- **Windows:** the Visual Studio C++ build tools. `build.sh` finds them through `vswhere`, and runs from
  Git Bash.
- **Linux (x86_64, aarch64):** `cc`. The shim is linked with `$ORIGIN` as its runpath, so it finds FFmpeg
  beside itself wherever the jar is extracted. Written, not yet run.
- **macOS:** BtbN builds no macOS FFmpeg. One from elsewhere (Homebrew's is GPL, so a build configured
  with `--enable-shared --disable-gpl` would be needed) goes in as a new case in `build.sh` and
  `NativeLib.platform()`.

The Windows staging is about 125 MB of DLLs, most of it `avcodec`. The jar compresses it to about
55 MB. If that ever matters, a custom FFmpeg configured with `--disable-everything` plus the decoders and
demuxers wanted is a fraction of the size, and changes nothing above `build.sh`.

At runtime the libraries are extracted from the jar to a temp dir and loaded in the order `libs.txt`
gives (each after the ones it links, so a library loaded by path finds its dependencies already there).
Before extracting, two places are tried: `-Dffmpeg.native.dir=<dir>`, a directory laid out as `build.sh`
stages one, then an `ffmpeg` folder laid out the same way beside the running executable. Callers need
`--enable-native-access=ALL-UNNAMED` (or their module).

**Native images** ship the libraries beside the executable rather than link them in, since FFmpeg is a
set of shared libraries (unlike imagelib). The jar carries the FFM downcall metadata and the flags a
native image needs (`META-INF/native-image/sibarum.ffmpeg/ffmpeg-wrapper/`), and registers none of the
libraries as resources, which would put 125 MB into the executable. The application copies
`natives/<platform>/` out of the jar into an `ffmpeg` folder beside its executable (Pix does it with
`maven-dependency-plugin` and ships the folder as an installer archive). Without it, `Ffmpeg.open` throws
the `LinkageError` of the failed load, so a caller can tell a missing decoder from a bad file.

## Behaviour worth knowing

- **Seeks are exact.** `seek(t)` goes to the keyframe at or before `t`, then decodes and drops frames
  until the one showing at `t`. That costs up to one keyframe interval of decoding, and is what a
  scrubber wants.
- **A damaged packet is skipped**, as a player would skip it. Only running out of memory, or an
  unreadable file, stops `next()` with an exception.
- **FFmpeg's log is kept, not printed.** A library writing to the host's stderr is talking over its
  caller. The last error FFmpeg logged on the thread is appended to the exception's message instead,
  for example `could not open the file: Invalid data found when processing input (moov atom not found)`.
- **One thread per `Video` at a time.** Different videos on different threads do not interact.
  FFmpeg's own frame threads are used inside each decoder (`thread_count = 0`, as many as the machine
  has). An `Audio` may be called from any thread. It brings two threads of its own, its decoder and
  miniaudio's device thread, and `close()` ends both.
- **No sound device is an exception, not a hang.** `openAudio` throws when the output will not open,
  so a player can carry on with the pictures alone.

## Tests

`mvn test` **fails** rather than skips when the native libraries are missing. A green run of a bindings
suite that never crossed the boundary would say nothing at all.

The fixtures are three seconds at 64x48 and 10 fps: one second red, one green, one blue, with a
keyframe every ten frames. The same picture comes in three codecs and containers: `rgb.mp4` is H.264
(openh264) with AAC sound, `rgb.webm` is VP9, and `rgb.mkv` is AV1 (libaom). A solid colour survives any
lossy codec, so the tests can check pixels as well as counts and timestamps. The sound, a 440 Hz tone,
is played to `Audio.Output.NOWHERE`, so the clock tests run in real time without a speaker. The fixtures
were made with the `ffmpeg.exe` that comes with the pinned build:

```bash
FF=native/ffmpeg/ffmpeg-n8.1.3-9-g29e619e767-win64-lgpl-shared-8.1/bin/ffmpeg.exe
SRC='color=c=red:s=64x48:r=10:d=1,format=yuv420p[a];color=c=lime:s=64x48:r=10:d=1,format=yuv420p[b];color=c=blue:s=64x48:r=10:d=1,format=yuv420p[c];[a][b][c]concat=n=3:v=1:a=0[v]'
$FF -filter_complex "$SRC" -f lavfi -i "sine=f=440:d=3" -map "[v]" -map 0:a -c:v libopenh264 -g 10 -b:v 200k -c:a aac -b:a 32k -shortest rgb.mp4
$FF -filter_complex "$SRC" -map "[v]" -c:v libvpx-vp9 -g 10 -b:v 100k rgb.webm
$FF -filter_complex "$SRC" -map "[v]" -c:v libaom-av1 -g 10 -cpu-used 8 -b:v 100k rgb.mkv
```

The `tone.*` fixtures are files that are only sound: three seconds of the same tone, mono, in each format
people keep sound in. `AudioTest` opens and plays each one to its end. The MP3 carries a cover picture,
which FFmpeg counts as a video stream. It is made in two passes because the MP3 muxer stops the sound
where a one-frame picture ends:

```bash
T='sine=f=440:d=3:r=48000'
$FF -f lavfi -i 'sine=f=440:d=3:r=16000' -c:a pcm_s16le tone.wav
$FF -f lavfi -i "$T" -c:a flac tone.flac
$FF -f lavfi -i "$T" -c:a libvorbis -q:a 0 tone.ogg
$FF -f lavfi -i "$T" -c:a libopus -b:a 24k tone.opus
$FF -f lavfi -i "$T" -c:a aac -b:a 32k tone.m4a
$FF -f lavfi -i "$T" -c:a libmp3lame -b:a 32k bare.mp3
$FF -f lavfi -i color=c=red:s=32x32 -frames:v 1 cover.jpg
$FF -i bare.mp3 -i cover.jpg -map 0:a -map 1:v -c copy -disposition:v attached_pic -id3v2_version 3 \
    -metadata title=Tone -metadata artist=Sine tone.mp3
```

`CompressionTest` measures what the device was given (`Audio.heard`), with two fixtures that have
dynamics in them:

```bash
$FF -f lavfi -i "aevalsrc='if(lt(t,1.5),0.01,0.7)*sin(2*PI*440*t)':s=48000:d=3" -c:a flac quiet-loud.flac
E='if(between(t,1.5,1.52),0.95,0.01)*sin(2*PI*440*t)'      # stereo, so the bang reaches the device at 0.95
$FF -f lavfi -i "aevalsrc='$E|$E':s=48000:d=3" -c:a flac bang.flac
```

`RecordingTest` needs no fixture. It records solid colours with gaps between them, at odd sizes and
through both encoders, and reads every file back through `Ffmpeg.open`, checking the codec, the size,
each frame's time to the millisecond, and the colour. It also times 1080p and fails if a frame takes
longer than 60 fps allows on the machine running the build.
