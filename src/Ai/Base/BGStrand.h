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
        float portalSaving = 0.0f;     // yd the portal saves over walking (straight lines)
        Position jump;                 // SASortie: jump off the wall to this point outside (set with hasJump)
        bool hasJump = false;
        GameObject* disarm = nullptr;  // SADefense: a planted enemy charge to disarm at the move point
        bool urgent = false;           // SADefense: move above combat (going out, or to a charge)
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
    // SASortie: a sortie bot still inside while attackers mass at its gate (moves like a flag carrier, above combat).
    bool SortieMoving(Player* bot, Battleground* bg);
    // SADefense: this defender's place from the allocator; false when SADefense is off or not defending.
    bool DefenseObjective(Player* bot, Battleground* bg, Order& out);
    // SADefense: a defender on its way out of the gate or to a charge (moves like a flag carrier).
    bool DefenseMoving(Player* bot, Battleground* bg);
    // SAEscort: an escort more than 25 yd from its demolisher (moves like a flag carrier, above combat).
    bool EscortMoving(Player* bot, Battleground* bg);
    // SASiegeSupply: a fetcher more than 40 yd from its idle demolisher (moves like a flag carrier, above combat).
    bool SupplyMoving(Player* bot, Battleground* bg);
    // SADriveFire: a driver more than 50 yd from its gate attacks enemy players on the way (call every move).
    void DriveFire(Player* bot, Battleground* bg, Position const& gate);
    // SAWarmup: an allocator defender during the warmup (the round has not started: attackers are on the boats).
    bool WarmupDefender(Player* bot, Battleground* bg);
    // SAWarmup diagnostics: kind 0 a warmup move was triggered, 1 it ran (Server.log per 5 min, by 20 s steps).
    void WarmupProbe(Battleground* bg, uint32 kind);
    // SAWarmup diagnostics: how a warmup move ended (Server.log per 5 min, first and second warmup minute).
    void WarmupExit(Battleground* bg, uint32 exit);
    // SAPassengers: a friendly driven demolisher within 30 yd with a free seat for this bot to ride in; nullptr otherwise.
    Unit* PassengerSeat(Player* bot, Battleground* bg);
    // SAPassengers: a ranged bot or healer leaves an empty demolisher to a melee teammate on foot within 40 yd of it.
    bool LeaveToMelee(Player* bot, Battleground* bg, Unit* vehicle);
    // SotA tactic counters (Server.log per 5 min): an event by this bot (counted once per bot per 10 s), and a death to
    // classify (a dismounted driver or an attacker near the relic, tagged by whether the defenders had the package).
    void Stat(char const* name, Player* bot);
    void KillStat(Player* victim);
    // portal diagnostics (Server.log per 5 min): a defender took a Defender's Portal saving this many yd
    void PortalUsed(Player* bot, float saving);
    // passenger casting diagnostics (Server.log per 5 min): a step of a passenger's spell casting, and its state (5 s)
    void PaxCast(Player* bot, std::string const& what);
    void PaxSample(PlayerbotAI* botAI);
    // Ram diagnostics (Server.log per 5 min): one step of a demolisher's Ram
    void RamCount(std::string const& key);
    // a driven siege vehicle died (demo_kill / demo_kill_courtyard, tagged like KillStat)
    void SiegeKill(Unit* vehicle);
    // SADefense diagnostics: a disarm was sent (defused: the charge went away).
    void Disarmed(bool defused);
    // SASlows: a driven enemy siege vehicle within 30 yd in line of sight that is not rooted or slowed.
    Unit* SlowTarget(Player* bot, Battleground* bg);
    // SASortie diagnostics: a jump was made (the rest of the stages are counted in Objective).
    void SortieJumped(Player* bot);
}

#endif
