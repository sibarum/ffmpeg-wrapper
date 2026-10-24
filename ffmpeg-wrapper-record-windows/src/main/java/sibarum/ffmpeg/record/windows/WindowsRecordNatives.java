package sibarum.ffmpeg.record.windows;

import sibarum.ffmpeg.spi.FfmpegRecordNatives;

import java.util.Locale;

/**
 * Where this module bundles the encoding FFmpeg for Windows: {@code /natives/record/<platform>/}, as native/build.sh
 * stages it.
 */
public final class WindowsRecordNatives implements FfmpegRecordNatives {

    private static final String ARCH = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);

    @Override
    public String platform() {
        return "windows-x86_64";
    }

    /** Only the architectures native/build.sh builds the encoding FFmpeg for; anything else has no bundled build. */
    @Override
    public String resourceRoot() {
        return (ARCH.equals("amd64") || ARCH.equals("x86_64")) ? "/natives/record/" + platform() + "/" : null;
    }
}
