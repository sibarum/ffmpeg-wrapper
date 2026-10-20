package sibarum.ffmpeg.linux;

import sibarum.ffmpeg.spi.FfmpegNatives;

import java.util.Locale;

/** Where this module bundles FFmpeg for Linux: {@code /natives/<platform>/}, as native/build.sh stages it. */
public final class LinuxFfmpegNatives implements FfmpegNatives {

    private static final String ARCH = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);

    @Override
    public String platform() {
        return ARCH.equals("aarch64") ? "linux-aarch64" : "linux-x86_64";
    }

    /** Only the architectures native/build.sh has a pinned FFmpeg for; anything else has no bundled build. */
    @Override
    public String resourceRoot() {
        return (ARCH.equals("amd64") || ARCH.equals("x86_64") || ARCH.equals("aarch64")) ? "/natives/" + platform() + "/" : null;
    }
}
