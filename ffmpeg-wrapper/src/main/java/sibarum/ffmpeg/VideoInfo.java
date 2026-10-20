package sibarum.ffmpeg;

/**
 * What a file says about itself, read once on {@link Ffmpeg#open}.
 *
 * @param width          px a frame is shown at: the coded width with the sample aspect applied
 * @param height         px
 * @param durationMicros the whole file's length, or -1 where the container does not say
 * @param frameRate      frames per second as the stream declares it, or 0 where it does not
 * @param codec          the video codec's short name: "h264", "hevc", "vp9", "av1"
 * @param container      the container's short name: "matroska,webm", "mov,mp4,m4a,3gp,3g2,mj2"
 * @param audio          whether the file also holds sound, which nothing here decodes
 */
public record VideoInfo(int width, int height, long durationMicros, double frameRate, String codec,
                        String container, boolean audio) {

    /** Whether the length is known. */
    public boolean timed() {
        return durationMicros >= 0;
    }
}
