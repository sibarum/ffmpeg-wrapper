/*
 * What ffwrap.c and audio.c share, and nothing outside the shim sees: the error and log plumbing behind
 * ffw_last_error.
 */
#ifndef FFWRAP_INTERNAL_H
#define FFWRAP_INTERNAL_H

/* Say why the call in hand failed, for ffw_last_error on this thread. */
void ffwi_fail(const char *fmt, ...);

/* The same, in FFmpeg's words for `code`, after what this was doing, with the last error FFmpeg logged. */
void ffwi_fail_av(const char *doing, int code);

/* Keep FFmpeg's log from here on rather than print it, and forget what this thread kept before. */
void ffwi_listen(void);

#endif /* FFWRAP_INTERNAL_H */
