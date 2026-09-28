/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BGHEALERGUARD_H
#define _PLAYERBOT_BGHEALERGUARD_H

class Battleground;
class PlayerbotAI;
class Unit;
struct Position;

// Healer bodyguards (arm HealerGuard, any BG without the allocator): each healer bot of the team gets one melee
// damage dealer (the nearest free one within 60 yd, kept while both live). The bodyguard stays within 12 yd of its
// healer and attacks whoever is on the healer (an enemy whose target is the healer, else the nearest enemy within
// 10 yd of the healer).
namespace BGHealerGuard
{
    // A bodyguard more than 12 yd from its healer: go to the healer. false otherwise (the normal objective).
    bool Hold(PlayerbotAI* botAI, Battleground* bg, Position& out);
    // A bodyguard's target: an enemy on its healer; nullptr otherwise.
    Unit* Target(PlayerbotAI* botAI);
}

#endif
