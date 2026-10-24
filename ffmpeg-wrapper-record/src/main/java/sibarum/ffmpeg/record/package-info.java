/**
 * AV1 recorded from frames as they are made, and nothing else — see {@link sibarum.ffmpeg.record.Recording}.
 *
 * <p>The recording half of ffmpeg-wrapper, apart from the decoding half ({@code sibarum.ffmpeg}) down to its own
 * FFmpeg: an application that records its windows carries the AV1 encoders and the Matroska and WebM muxers, and no
 * decoder. Two public types: {@code Recording} (frames in, AV1 out) and
 * {@link sibarum.ffmpeg.record.RecordingException}.
 *
 * <p>The boundary is {@code native/record/ffrecord.h}: eight C functions, one opaque handle. {@code RecordApi} binds
 * them one line each and loads the shim and its FFmpeg; it is package-private, because the ABI is an implementation
 * detail of {@code Recording} rather than a second API.
 */
package sibarum.ffmpeg.record;
