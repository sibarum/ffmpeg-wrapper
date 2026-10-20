package sibarum.ffmpeg.spi;

/**
 * Where this operating system's build of the ffwrap shim and its FFmpeg libraries is bundled: the one fact
 * {@code ffmpeg-wrapper} needs that differs per OS. An OS module ({@code ffmpeg-wrapper-windows},
 * {@code -linux}) provides it through {@link java.util.ServiceLoader}, and the build puts exactly one of them on
 * the class path.
 */
public interface FfmpegNatives {

    /** What this is for in a message: {@code windows-x86_64}, {@code linux-x86_64}. */
    String platform();

    /**
     * The class-path folder holding this machine's build, ending in {@code /}: a {@code libs.txt} listing the
     * libraries in load order, and the libraries. Null when the module has no build for this architecture.
     */
    String resourceRoot();
}
