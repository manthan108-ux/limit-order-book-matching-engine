# Progress

| Phase | Status |
|---|---|
| 1 - Foundation and architecture | Complete |
| 2 - Domain models (Order, Trade, PriceLevel) | Complete |
| 3 - Order book | Not started |
| 4 - Matching engine | Not started |
| 5 - Order lifecycle (cancel / modify) | Not started |
| 6 - CLI | Not started |
| 7 - Testing and reliability | Not started |
| 8 - Benchmarking | Not started |
| 9 - Documentation and finalization | Not started |

## Phase 1

Build system, `OrderTypes.h` (types, enums, transition rules, validation helpers), test harness, and docs.

## Phase 2

Implemented `Order`, `Trade` and `PriceLevel` (see [ARCHITECTURE.md](ARCHITECTURE.md)). `lob_core` is now a static library. Phase 1 changes were limited to:
- adding `RejectReason::InvalidOrderId` and its string;
- adding a `CHECK_THROWS` macro to the test harness.

Verified from a clean build directory under GCC 16.2 (WinLibs, MinGW-w64, Ninja) with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`: zero warnings, `ctest` 4/4 suites pass.

| Suite | Tests |
|---|---|
| OrderTypesTests (Phase 1) | 8 |
| OrderTests | 13 |
| TradeTests | 8 |
| PriceLevelTests | 18 |
| **Total** | **47** |

## Known limitations

- Only GCC has been tried. MSVC and Clang are not yet verified.
- `PriceLevel` cannot detect a handle that belongs to another level or a handle that was already invalidated. The OrderBook (Phase 3) must own that guarantee.
- `PriceLevel` does not check order id uniqueness. The OrderBook's id map does.
- `Order` has no in-place quantity-reduction operation yet. It will be added with Modify in Phase 5.
- The CLI is still a placeholder, and nothing is matched yet.
- `benchmarks/` is empty. No benchmark numbers exist yet.
