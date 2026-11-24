// Offline test of the item shop's vanilla id table (src/item_shop_ids.hpp):
// vanilla ids (Elden Ring 2.7.2.0 regulation) and ids that only ELDEN RING
// Reforged 2.3.9.1 or The Convergence 3.0.4 add (r162 analysis).
#include "../src/item_shop_ids.hpp"

#include <cstdio>

namespace {
int failures = 0;

void expect(bool got, bool want, const char* what) {
    if (got != want) {
        std::printf("FAIL %s: got %d, want %d\n", what, got ? 1 : 0, want ? 1 : 0);
        ++failures;
    }
}

template <std::size_t N>
bool sorted(const item_shop_ids::VanillaIdRange (&r)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        if (r[i].first > r[i].last) return false;
        if (i && r[i].first <= r[i - 1].last) return false;
    }
    return true;
}
}  // namespace

int main() {
    using item_shop_ids::vanilla_item;
    expect(sorted(item_shop_ids::kVanillaWeapons) && sorted(item_shop_ids::kVanillaProtectors) &&
        sorted(item_shop_ids::kVanillaAccessories) && sorted(item_shop_ids::kVanillaGoods) &&
        sorted(item_shop_ids::kVanillaGems), true, "ranges sorted and apart");
    // weapons (base ids)
    expect(vanilla_item(0, 1000000), true, "Dagger");
    expect(vanilla_item(0, 3560000), true, "Tarnished Pack weapon");
    expect(vanilla_item(0, 1120000), false, "Reforged/Convergence dagger 1120000");
    expect(vanilla_item(0, 64540000), false, "Reforged Rotten Crystal Ringblade");
    expect(vanilla_item(0, 1000100), false, "not a base weapon id");
    // armour
    expect(vanilla_item(1, 40000), true, "Iron Helmet");
    expect(vanilla_item(1, 5340000), true, "Tarnished Pack armour");
    expect(vanilla_item(1, 41000), false, "Reforged Iron Helmet (Altered)");
    // talismans
    expect(vanilla_item(2, 1000), true, "Crimson Amber Medallion");
    expect(vanilla_item(2, 1061), false, "Convergence Valorous Heirloom");
    expect(vanilla_item(2, 2230), false, "Reforged Prodigy Crown Pendant");
    // goods
    expect(vanilla_item(3, 200000), true, "Black Knife Tiche (vanilla id)");
    expect(vanilla_item(3, 2009600), true, "Tarnished Pack goods");
    expect(vanilla_item(3, 8700), true, "Note: Hidden Cave (shop title row)");
    expect(vanilla_item(3, 300000), false, "Reforged Black Knife Tiche");
    expect(vanilla_item(3, 2400000), false, "Convergence Cemetery Shade Ashes");
    expect(vanilla_item(3, 3010000), false, "Reforged Offense: Poisebreaker");
    // Ashes of War
    expect(vanilla_item(4, 10000), true, "Ash of War: Lion's Claw");
    expect(vanilla_item(4, 12500), false, "mod Ash of War 12500");
    expect(vanilla_item(4, 1010010), false, "Reforged Mundane Ash of War");
    expect(vanilla_item(9, 1), true, "unknown type counts as vanilla");
    if (failures) return 1;
    std::printf("item_shop_ids_test: PASSED\n");
    return 0;
}
