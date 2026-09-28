/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BGSTRAND_H
#define _PLAYERBOT_BGSTRAND_H

#include "Position.h"
#include "SharedDefines.h"

class Battleground;
class GameObject;
class Player;

// Strand of the Ancients (SATactics): attackers push the gates in order, defenders hold the gate under attack.
namespace BGStrand
{
    struct Order
    {
        Position move;
        Position siege;  // the gate a driver shells (set with hasSiege)
        bool hasSiege = false;
        GameObject* use = nullptr;  // banner or relic to click at the move point
    };

    // The Titan Relic carries the attackers' faction; TEAM_NEUTRAL while objects respawn.
    TeamId Attackers(Battleground* bg);
    // This bot's objective; false when there is none.
    bool Objective(Player* bot, Battleground* bg, Order& out);
    // Bots never ride the boats and stay at sea: attackers go to the beach, where the core puts late joiners.
    bool GoAshore(Player* bot, Battleground* bg);
}

#endif
