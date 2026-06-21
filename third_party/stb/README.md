# stb (vendored single-headers)

Public-domain single-header libraries from <https://github.com/nothings/stb>,
vendored the same way as the nlohmann single-header (DESIGN-villen §8): an
explicit, reviewable copy rather than a configure-time fetch — the Steam Deck
never builds (steamdeck-debugging §2).

Used by the `filter` engine (DESIGN-filter §12) for per-frame JPEG decode/encode
of the camera media path. No system image-codec dependency.

- `stb_image.h` — v2.30 — JPEG/PNG/… decode (`stbi_load_from_memory`)
- `stb_image_write.h` — v1.16 — JPEG encode (`stbi_write_jpg_to_func`)

The implementations are compiled in exactly one translation unit:
`host/src/engines/filter/stb_impl.cpp`.
