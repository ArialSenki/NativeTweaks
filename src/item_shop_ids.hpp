#pragma once
// Item shop (F11): the vanilla regulation's item ids, so that the items an
// overhaul or other mod adds get a section of their own (r162).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace item_shop_ids {

struct VanillaIdRange { std::int32_t first, last; };
#include "item_shop_vanilla_ids.inc"

// Ranges are sorted and do not overlap; weapons hold base ids only
// (id % 10000 == 0), in steps of 10000.
template <std::size_t N>
bool in_ranges(const VanillaIdRange (&ranges)[N], std::int32_t id) noexcept {
    const auto it = std::upper_bound(std::begin(ranges), std::end(ranges), id,
        [](std::int32_t value, const VanillaIdRange& r) { return value < r.first; });
    return it != std::begin(ranges) && id <= std::prev(it)->last;
}

// equip type as in ShopLineupParam: 0 weapon, 1 armour, 2 talisman, 3 goods, 4 Ash of War
inline bool vanilla_item(std::uint8_t equip_type, std::int32_t id) noexcept {
    switch (equip_type) {
    case 0: return id % 10000 == 0 && in_ranges(kVanillaWeapons, id);
    case 1: return in_ranges(kVanillaProtectors, id);
    case 2: return in_ranges(kVanillaAccessories, id);
    case 3: return in_ranges(kVanillaGoods, id);
    case 4: return in_ranges(kVanillaGems, id);
    default: return true;
    }
}

}  // namespace item_shop_ids
