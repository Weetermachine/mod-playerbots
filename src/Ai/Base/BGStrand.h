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
class PlayerbotAI;
class Unit;

// Strand of the Ancients (SATactics): attackers push the gates in order, defenders hold the gate under attack.
namespace BGStrand
{
    struct Order
    {
        Position move;
        Position siege;  // the gate a driver shells (set with hasSiege)
        bool hasSiege = false;
        GameObject* use = nullptr;  // banner or relic to click at the move point
        Unit* board = nullptr;      // SACannons: the cannon to board at the move point
        bool leave = false;         // SACannons: get out of this cannon (its gate fell or it is not ours to man)
        GameObject* pickup = nullptr;  // SACharges: the bomb pile to take a charge from
        GameObject* plantAt = nullptr; // SACharges: the gate to plant the carried charge at
        GameObject* portal = nullptr;  // SAPortals: the Defender's Portal to take
        Position jump;                 // SASortie: jump off the wall to this point outside (set with hasJump)
        bool hasJump = false;
    };

    // The Titan Relic carries the attackers' faction; TEAM_NEUTRAL while objects respawn.
    TeamId Attackers(Battleground* bg);
    // This bot's objective; false when there is none.
    bool Objective(Player* bot, Battleground* bg, Order& out);
    // Bots never ride the boats and stay at sea: attackers go to the beach, where the core puts late joiners.
    bool GoAshore(Player* bot, Battleground* bg);
    // SABoatRide: during the warmup an attacker at sea boards the nearest boat as a passenger.
    bool Board(Player* bot, Battleground* bg);
    // SABoatRide: a passenger jumps ashore once its boat waits at the dock; true while it still rides.
    bool Ride(Player* bot, Battleground* bg);
    // SAHunt: the enemy demolisher for this defender to attack (near the gate it defends); nullptr otherwise.
    Unit* HuntTarget(PlayerbotAI* botAI);
    // SACannons: the enemy player a cannon gunner shoots at; nullptr otherwise.
    Unit* GunnerTarget(Player* bot, Battleground* bg);
    // SACharges diagnostics: follows each picked-up charge to its outcome (call every move); Planted when one is placed.
    void CarryTrack(Player* bot, Battleground* bg);
    void CarryPlanted(Player* bot);
    // SAChargesPush: an attacker holding a charge (treated like a flag carrier).
    bool PushingCharge(Player* bot, Battleground* bg);
}

#endif
