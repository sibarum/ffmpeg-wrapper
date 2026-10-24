package sibarum.ffmpeg;

import sibarum.ffmpeg.spi.FfmpegNatives;
import sibarum.ffmpeg.spi.NativeLibraries;

import java.lang.foreign.SymbolLookup;
import java.util.List;

/**
 * Loads the {@code ffwrap} shim and the decoding FFmpeg under it, through {@link NativeLibraries}: from
 * {@code -Dffmpeg.native.dir}, then an {@value #BESIDE} folder beside the running executable (how a native image ships
 * them), then the classpath resources the OS module ({@link FfmpegNatives}) names. Loading is idempotent and
 * thread-safe.
 */
final class NativeLib {

    /** The folder beside an executable that the libraries are looked for in. */
    static final String BESIDE = "ffmpeg";

    private static final NativeLibraries LIBRARIES =
            new NativeLibraries(FfmpegNatives.class, "ffmpeg.native.dir", BESIDE, "ffmpeg-wrapper");

    private static volatile SymbolLookup lookup;

    static synchronized SymbolLookup load() {
        if (lookup != null) {
            return lookup;
        }
        try {
            LIBRARIES.load();
        } catch (IllegalStateException e) {
            throw new VideoException(e.getMessage(), e.getCause());
        }
        lookup = SymbolLookup.loaderLookup();
        return lookup;
    }

    /** The file names in {@code libs.txt}, in its order: blank lines and surrounding space ignored. */
    static List<String> libraries(String list) {
        return NativeLibraries.libraries(list);
    }

    private NativeLib() {
    }
}
