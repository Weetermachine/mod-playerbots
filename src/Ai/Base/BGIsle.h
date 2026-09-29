/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BGISLE_H
#define _PLAYERBOT_BGISLE_H

#include "Position.h"

class Battleground;
class GameObject;
class Player;
class Unit;

// Isle of Conquest extras: ICBombs (carry Seaforium bombs to the enemy gate), ICGunship (drop into the enemy keep).
namespace BGIsle
{
    struct Order
    {
        Position move;
        GameObject* use = nullptr;      // bomb pile or gunship portal to click at the move point
        GameObject* plantAt = nullptr;  // the enemy gate to plant the carried bomb at
        uint32 item = 0;                // the carried bomb item and its placing spell
        uint32 spell = 0;
        Unit* board = nullptr;          // ICCannons: the keep cannon to board at the move point
        bool leave = false;             // ICCannons: get out of this cannon (no siege near the keep, or not crew)
    };

    // This bot's bomb or gunship errand; false: the stock objective.
    bool Objective(Player* bot, Battleground* bg, Order& out);
    // ICGunship: a bot on its gunship jumps when it is over the enemy keep; true while it rides or falls.
    bool Drop(Player* bot, Battleground* bg);
    // ICCannons: what a keep cannon gunner shoots: enemy siege vehicles first, then players; nullptr otherwise.
    Unit* GunnerTarget(Player* bot, Battleground* bg);
}

#endif
