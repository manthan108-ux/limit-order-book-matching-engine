# Progress

| Phase | Status |
|---|---|
| 1 - Foundation and architecture | Complete |
| 2 - Domain models (Order, Trade, PriceLevel) | Not started |
| 3 - Order book | Not started |
| 4 - Matching engine | Not started |
| 5 - Order lifecycle (cancel / modify) | Not started |
| 6 - CLI | Not started |
| 7 - Testing and reliability | Not started |
| 8 - Benchmarking | Not started |
| 9 - Documentation and finalization | Not started |

## Phase 1

Verified: configure, build with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`, and `ctest` (1/1 suite, 8 tests) under GCC 16.2 (WinLibs, MinGW-w64, Ninja).

## Known limitations

- Only GCC has been tried. MSVC and Clang are not yet verified.
- `lob_core` is header-only for now. `.cpp` files are added as phases introduce them.
- `benchmarks/` is empty. Its CMake hook activates once `benchmarks/CMakeLists.txt` exists.
- No benchmark numbers exist yet.
