/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BGAOESQUAD_H
#define _PLAYERBOT_BGAOESQUAD_H

class Battleground;
class PlayerbotAI;
struct Position;
class Unit;

// AoE squad at chokepoints (AV and IoC; arms AoESquad2 / AoESquad5 / AoESquadAll). When 5+ enemies come within 70 yd
// of one of the team's chokes (fixed spots from where fights happen, on its own half of the map), the team's
// area-damage bots nearest to it (2, 5 or all: mages, warlocks, hunters, elemental shamans, balance druids) hold a
// point 22 yd behind it on their side, and target the enemy with the most other enemies around it, so their AoE
// spells land on the pack. The squad stands down 15 s after the choke is quiet.
// AoEGeneral (AV): when 5+ enemies are within 50 yd of our general, every area-damage bot goes to him instead.
namespace BGAoeSquad
{
    // A squad member's hold point while its squad is active; false otherwise (the stock objective).
    bool Hold(PlayerbotAI* botAI, Battleground* bg, Position& out);
    // A squad member's target while holding: the enemy with the most enemies within 8 yd; nullptr otherwise.
    Unit* Target(PlayerbotAI* botAI);
}

#endif
