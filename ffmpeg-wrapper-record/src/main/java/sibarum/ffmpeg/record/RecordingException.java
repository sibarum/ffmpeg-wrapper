package sibarum.ffmpeg.record;

/**
 * A recording that could not be started, fed or finished, or an encoder that would not load — carrying FFmpeg's own
 * message where there is one.
 *
 * <p>Unchecked, as {@code ffmpeg-wrapper}'s {@code VideoException} is: a full disk or a missing encoder is shown, or
 * ends the recording, one level up.
 */
public class RecordingException extends RuntimeException {

    private static final long serialVersionUID = 1L;

    public RecordingException(String message) {
        super(message);
    }

    public RecordingException(String message, Throwable cause) {
        super(message, cause);
    }
}
