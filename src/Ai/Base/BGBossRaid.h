/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BGBOSSRAID_H
#define _PLAYERBOT_BGBOSSRAID_H

class Battleground;
class Creature;
class PlayerbotAI;
class Unit;
struct Position;

// Raid-boss treatment for the AV generals (arm BossRaid). Stock bots walk into the general whenever they have
// nothing else to do and die in waves (median 13-22 deaths 1-2 s apart; one game fed him 354 in a row). With the
// arm, a bot heading for the enemy general:
//   gather  waits at the boss wait point until 12+ of its team are there (or 5+ after 90 s);
//   pull    up to 2 tank-spec bots go in first (tank 1 on the general, tank 2 on his guards), the rest hold;
//   engage  after 5 s, or once the general is hitting a tank: damage dealers kill the guards first, then the general;
//           up to 3 healers are assigned to the tanks and stand 18 yd back.
// It falls back to gather when fewer than 8 of the team are left near him or he leaves combat for 10 s.
namespace BGBossRaid
{
    // For a bot whose objective would be the enemy general: 1 = go for him, 2 = go to `out` instead.
    int Plan(PlayerbotAI* botAI, Battleground* bg, Creature* boss, Position const& waitPos, Position& out);
    // Target for tanks and damage dealers while the raid pulls or fights; nullptr = the normal choice.
    Unit* Target(PlayerbotAI* botAI);
    // The tank a healer is assigned to (alive, in the raid); nullptr otherwise.
    Unit* HealTarget(PlayerbotAI* botAI);
}

#endif
