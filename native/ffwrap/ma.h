/*
 * miniaudio as the shim uses it: the device layer only. FFmpeg decodes, so miniaudio's decoders, encoders,
 * engine and the rest are left out. Every file that includes miniaudio does it through here, so the
 * implementation (miniaudio.c) and its users agree on what was left out. build.sh fetches the pinned
 * header into ../miniaudio/.
 */
#ifndef FFWRAP_MA_H
#define FFWRAP_MA_H

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#include "miniaudio.h"

#endif /* FFWRAP_MA_H */
