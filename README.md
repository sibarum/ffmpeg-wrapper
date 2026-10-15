# ffmpeg-wrapper

Panama (FFM) bindings for one C ABI over every video FFmpeg can decode. Hand-written downcalls over a
sixteen-function header and a small C shim behind it, with no jextract, no JNI and no code generation.

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
```

## The one idea

**The caller never learns a codec.** Every file opens the same way and every frame comes out as RGBA8
at the size asked for. It has already been converted from whatever YUV layout, matrix and range the
stream used. The codec's name is in `VideoInfo` to be shown, never to be branched on. This is
imagelib-wrapper's rule, applied to video: format knowledge stops at this boundary.

**FFmpeg's structs never cross into Java.** FFmpeg's API is a few hundred structs whose layouts move
between major versions, and binding them from Java would mean regenerating every time FFmpeg does. So a
shim in C (`native/ffwrap/ffwrap.c`, about 400 lines) is the only code that knows them. It exposes an
opaque handle and numbers. Moving to a newer FFmpeg is a recompile of the shim, and the Java does not
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

**Video only.** A file's sound is reported (`VideoInfo.audio()`) and not decoded: the stack has nowhere
to play it yet.

## Licence: LGPL, shared

The bundled FFmpeg is BtbN's **LGPL shared** build. It contains FFmpeg's own decoders plus the
permissively licensed libraries BtbN links in (dav1d, libvpx, openh264, ...). Nothing GPL is in it, so
there is no x264, x265 or libpostproc. Decoding needs none of those. FFmpeg stays a set of separate
libraries beside the shim, which a user can replace, and that is what the LGPL asks for.
`FFMPEG-LICENSE.txt` and `FFMPEG-SOURCE.txt` (which build, and where its source is) travel in the jar
beside the libraries, under `natives/<platform>/`.

## Layout

```
native/include/ffwrap.h      the contract: 16 functions, 1 opaque handle, no struct crosses
native/ffwrap/ffwrap.c       the C behind it: libavformat, libavcodec, libswscale
native/build.sh              fetch the pinned FFmpeg, compile the shim, stage both into native/dist/bin/<platform>/
src/main/java/sibarum/ffmpeg/
    Ffmpeg.java              open a file; FFmpeg's version
    Video.java               an open file: info, output size, next frame, seek, close
    VideoInfo.java, Frame.java, VideoException.java
    CApi.java                one line per header function
    NativeLib.java           find, extract, load: six libraries, in order
```

`CApi` and `NativeLib` are package-private on purpose: the ABI is an implementation detail of the public
types, not a second API.

## Building

```bash
./native/build.sh    # fetches FFmpeg into native/ffmpeg/, stages native/dist/bin/<platform>/
mvn install          # bundles it into the jar; tests need --enable-native-access (surefire is set up)
```

`build.sh` pins one BtbN autobuild (`RELEASE` and `BUILD` at its top, a month's last build, which BtbN
keeps) and checks the download against its sha256. A bump changes those three lines together. The
native side needs a C compiler and nothing else:

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
Override with `-Dffmpeg.native.dir=<dir>`, a directory laid out as `build.sh` stages one. Callers need
`--enable-native-access=ALL-UNNAMED` (or their module).

There is no native-image story yet. Unlike imagelib, FFmpeg is a set of shared libraries, so a native
image would ship them beside the executable rather than link them in.

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
  has).

## Tests

`mvn test` **fails** rather than skips when the native libraries are missing. A green run of a bindings
suite that never crossed the boundary would say nothing at all.

The fixtures are three seconds at 64x48 and 10 fps: one second red, one green, one blue, with a
keyframe every ten frames. The same picture comes in three codecs and containers: `rgb.mp4` is H.264
(openh264) with AAC sound, `rgb.webm` is VP9, and `rgb.mkv` is AV1 (libaom). A solid colour survives any
lossy codec, so the tests can check pixels as well as counts and timestamps. They were made with the
`ffmpeg.exe` that comes with the pinned build:

```bash
FF=native/ffmpeg/ffmpeg-n8.1.3-9-g29e619e767-win64-lgpl-shared-8.1/bin/ffmpeg.exe
SRC='color=c=red:s=64x48:r=10:d=1,format=yuv420p[a];color=c=lime:s=64x48:r=10:d=1,format=yuv420p[b];color=c=blue:s=64x48:r=10:d=1,format=yuv420p[c];[a][b][c]concat=n=3:v=1:a=0[v]'
$FF -filter_complex "$SRC" -f lavfi -i "sine=f=440:d=3" -map "[v]" -map 0:a -c:v libopenh264 -g 10 -b:v 200k -c:a aac -b:a 32k -shortest rgb.mp4
$FF -filter_complex "$SRC" -map "[v]" -c:v libvpx-vp9 -g 10 -b:v 100k rgb.webm
$FF -filter_complex "$SRC" -map "[v]" -c:v libaom-av1 -g 10 -cpu-used 8 -b:v 100k rgb.mkv
```
