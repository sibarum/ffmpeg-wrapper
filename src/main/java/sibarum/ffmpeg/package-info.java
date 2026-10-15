/**
 * Frames, from any video — see {@link sibarum.ffmpeg.Ffmpeg}.
 *
 * <p>Six public types: {@code Ffmpeg} (open a file), {@link sibarum.ffmpeg.Video} (an open file,
 * decoded forward and sought), {@link sibarum.ffmpeg.VideoInfo}, {@link sibarum.ffmpeg.Frame},
 * {@link sibarum.ffmpeg.VideoException}, and nothing that names a codec in a type.
 *
 * <p>The boundary itself is {@code native/include/ffwrap.h}: sixteen C functions, one opaque handle,
 * no struct crossing it. FFmpeg's structs change layout between major versions, so they stay behind
 * the shim. {@code CApi} binds the functions one line each and {@code NativeLib} loads the shim and
 * FFmpeg under it; both are package-private, because the ABI is an implementation detail of these
 * types rather than a second API.
 */
package sibarum.ffmpeg;
