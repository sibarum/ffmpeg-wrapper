package sibarum.ffmpeg.spi;

/**
 * Where this operating system's build of the ffrecord shim and its encoding-only FFmpeg is bundled: what
 * {@code ffmpeg-wrapper-record} needs that differs per OS, as {@link FfmpegNatives} is for the decoding half. An OS
 * module ({@code ffmpeg-wrapper-record-windows}, {@code -linux}) provides it through {@link java.util.ServiceLoader}.
 *
 * <p>A service of its own, not an {@link FfmpegNatives}, because an application that plays and records has both OS
 * modules on its class path, and each half must find its own build.
 */
public interface FfmpegRecordNatives extends FfmpegNatives {
}
