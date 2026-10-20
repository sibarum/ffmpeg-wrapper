package sibarum.ffmpeg.windows;

import sibarum.ffmpeg.spi.FfmpegNatives;

import java.util.Locale;

/** Where this module bundles FFmpeg for Windows: {@code /natives/<platform>/}, as native/build.sh stages it. */
public final class WindowsFfmpegNatives implements FfmpegNatives {

    private static final String ARCH = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);

    @Override
    public String platform() {
        return "windows-x86_64";
    }

    /** Only the architectures native/build.sh has a pinned FFmpeg for; anything else has no bundled build. */
    @Override
    public String resourceRoot() {
        return (ARCH.equals("amd64") || ARCH.equals("x86_64")) ? "/natives/" + platform() + "/" : null;
    }
}
