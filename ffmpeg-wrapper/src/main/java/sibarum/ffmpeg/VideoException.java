package sibarum.ffmpeg;

/**
 * An open, a decode, a seek or a load that did not happen — carrying FFmpeg's own message where there
 * is one.
 *
 * <p>Unchecked, because there is nothing a caller can usefully do about a damaged file at the call site
 * that it could not do better one level up: show why, or skip the file.
 */
public class VideoException extends RuntimeException {

    private static final long serialVersionUID = 1L;

    public VideoException(String message) {
        super(message);
    }

    public VideoException(String message, Throwable cause) {
        super(message, cause);
    }
}
