package sibarum.ffmpeg;

/**
 * One decoded frame: straight RGBA8, sRGB, tightly packed, top row first — {@code width * height * 4}
 * bytes, the layout imagelib gives a picture, so a frame uploads the way a picture does.
 *
 * <p>The array is the caller's: each {@link Video#next} copies into a new one, since the native buffer
 * is rewritten by the next decode.
 *
 * @param width     px, the output size the video was set to
 * @param height    px
 * @param ptsMicros when it is shown, from the stream's start
 * @param rgba      {@code width * height * 4} bytes
 */
public record Frame(int width, int height, long ptsMicros, byte[] rgba) {

    @Override
    public String toString() {
        return "Frame[" + width + "x" + height + " at " + ptsMicros + "us]";
    }
}
