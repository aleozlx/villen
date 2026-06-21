// The single translation unit that compiles the vendored stb implementations
// (DESIGN-filter §12). Every other file includes the stb headers declaration-
// only; the code lives here once.
//
// JPEG decode is all stb_image needs to pull in; disabling the other formats and
// the stdio path trims the object and the attack surface — the media path only
// ever hands stb an in-memory JPEG.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"
