// Placeholder entry point. The interactive CLI arrives in Phase 6; until then
// this only confirms the project builds and links against lob_core.
#include <iostream>

#include "OrderTypes.h"

int main() {
    std::cout << "Limit Order Book & Matching Engine (scaffold)\n"
              << "Sides: " << lob::toString(lob::Side::Buy) << ", "
              << lob::toString(lob::Side::Sell) << "\n";
    return 0;
}
