# Contributing to Villen

Villen is early, and small focused PRs are welcome.

Good contribution areas:

* Documentation and screenshots
* Demo scripts and screencast instructions
* Steam Deck / Linux packaging notes
* Browser client polish
* Admin UI polish
* QR/join flow improvements
* Tests for chess rules and session behavior
* A second small sample game, such as Connect Four

Before starting larger architectural work, open an issue first so the direction
can be discussed.

## Development

Use the README build instructions. For engine-only work, prefer:

```bash
cmake -S . -B build -DVILLEN_BUILD_HOST=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```
