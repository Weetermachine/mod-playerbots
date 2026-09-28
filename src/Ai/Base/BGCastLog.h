/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BGCASTLOG_H
#define _PLAYERBOT_BGCASTLOG_H

#include "Define.h"

class Player;
class SpellInfo;

// Battleground cast log (tactics measurement): per BG, every 5 minutes, bots' hostile casts split into area and
// single-target spells per class, and how often the AoE triggers ("light aoe": 2+ attackers within 8 yd of the
// current target, "medium aoe": 3+) are true when checked.
namespace BGCastLog
{
    void Cast(Player* bot, SpellInfo const* spell);          // a bot's cast was started
    void AoeCheck(Player* bot, uint32 amount, bool active);  // an AoE trigger was checked
}

#endif
