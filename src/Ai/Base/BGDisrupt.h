/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BGDISRUPT_H
#define _PLAYERBOT_BGDISRUPT_H

class Battleground;
class PlayerbotAI;
class Unit;
struct Position;

// Disruption (AV, arm Disrupt): while 5+ enemies fight our general or our captain, 5 of our bots (melee damage
// dealers first, then other damage dealers; nearest first, within 500 yd) go there and hit the enemies fighting him:
// their healers first, then whoever he is hitting (their tank: without it he tears through the rest), then the lowest
// health. The enemy is busy with the NPC, so its healers and tank are exposed. Stands down 15 s after it ends.
namespace BGDisrupt
{
    // A disruptor's destination while its site is busy; false otherwise (the stock objective).
    bool Hold(PlayerbotAI* botAI, Battleground* bg, Position& out);
    // A disruptor's target near the site; nullptr otherwise.
    Unit* Target(PlayerbotAI* botAI);
}

#endif
