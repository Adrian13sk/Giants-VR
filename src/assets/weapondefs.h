// weapondefs.bin (loose in Bin/): the weapon and inventory-item tables.
//   header: u32 version=1, u32 fileSize
//   records: {u32 key, u32 type, (type-2) x u32 value}. type 2 = flag (no value), 3 = one value,
//   4 = two, 6 = four. {key 2, type 2} ends a record. Strings are NUL-terminated at file offsets
//   (the string pool at the end of the file).
//   The weapon records come first (key 1 = weapon id); the last record is the item list, one item
//   per run starting at key 36 (item id) and ending with key 37 (flag).
// Key meanings are read from the values (see FORMATS.md, "weapondefs.bin"): the item ids agree
// with the missions' item lists (6 = IconSnipergun in story3_bunker_3, whose briefing names the
// SNIPER GUN; 10 = IconPopup in prison_7; 20 = IconBush in 2b) and key 39 is the weapon's object
// type (21 mc_gun_l0, 22 mc_shotgun_l0, 787 mc_snipergun_l0 ... in GameObj.bin). The weapon keys
// (10 damage, 13 seconds between shots, 24 projectile speed, 34 range) are our reading.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gvr {

struct WeaponDef {
    int id = -1;
    int projectileType = -1;  // key 3: object type of the projectile (793 mc_sniper_bullit, 800 mc_rocket)
    float damage = 0;         // key 10 (game units of health)
    float interval = 0;       // key 13: s between shots
    float speed = 0;          // key 24: projectile speed, game units/s (0 = not given)
    float range = 0;          // key 34: game units
    float splash[4] = {0, 0, 0, 0};  // key 9 (blast weapons)
    std::string fxProjectile, fxImpact, fxSplash, fxNozzle;  // keys 22, 23, 53, 29
};

struct ItemDef {
    int id = -1;              // key 36: the id the missions' item lists use
    int objectType = -1;      // key 39: the weapon/gadget object (GameObj type)
    int pickupType = -1;      // key 41: the pickup object
    int ammoItem = -1;        // key 42
    int maxAmmo = -1;         // key 45
    int ammoPerPickup = -1;   // key 46
    int category = -1;        // key 47: 1 weapon, 2 thrown, 3 gadget/spell, 4 ammo, 5 consumable, 6 building
    int race = -1;            // key 54: 1 any, 2 Meccaryn, 3 Reaper
    std::string icon;         // key 48 ("IconSnipergun")
};

struct WeaponDefs {
    std::vector<WeaponDef> weapons;
    std::vector<ItemDef> items;
    const WeaponDef* weapon(int id) const;
    const ItemDef* item(int id) const;
    // The weapon whose projectile / effect matches (the item table does not name its weapon).
    const WeaponDef* weaponByFx(const char* fxProjectile, float interval = -1.0f) const;
};

bool parseWeaponDefs(const uint8_t* data, size_t size, WeaponDefs& out, std::string* err = nullptr);

}  // namespace gvr
