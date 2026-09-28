/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BGTACTICARMS_H
#define PLAYERBOTS_BGTACTICARMS_H

#include <string>

#include "SharedDefines.h"

class Battleground;

// Battleground tactics that can be switched per faction, for A/B tests.
enum class BGTactic : uint8
{
    FCEscort = 0,   // WSG: escorts regroup on our flag carrier
    FCChase = 1,    // WSG: bots near the enemy flag carrier target it
    NodeGuard = 2,  // AB/EotS: defenders hold an assigned node
    Adaptive = 3,   // AB/EotS: roles (guard / flag / attack) follow how many nodes the team holds
    FCEscort2 = 4,  // WSG: escort v2 - also guards the carrier in standoffs, peels, healers heal the carrier first
    NodeGuard2 = 5, // AB: node guards v2 - node guards plus interrupting enemies channeling a banner capture
    FlagRoom = 6,   // WSG: defenders hold the flag room while our flag is home
    DirectPath = 7, // WSG/AB/EotS: move straight to the objective (no waypoint-path detours); EotS also snaps
                    // objective points to the ground correctly (arm name "EYPath" is an alias)
    Allocator = 8,  // WSG/AB/EotS: a team plan assigns bots to objectives (headcounts, nearest first, sticky)
    AllocRoles = 9, // with Allocator: healers kept off solo slots, one per group (always on in EotS)
    AllocGuard2 = 10,    // with Allocator (AB): 2 guards per held node instead of 1
    AllocOneGroup = 11,  // with Allocator (AB): one attack group of 7 instead of 5 + 4
    AllocFlagFloor = 12, // with Allocator (EotS): flag team >= 5, enemy carrier chase >= 3, escort >= 3
    AllocFlagValue = 13, // with Allocator (EotS): flag, carrier chase and escort jobs worth 2.5x
    ChaseStagger = 14,   // with FCChase (WSG): one slow/stun at a time on the enemy carrier, the next near its end
    FCEvade = 15,        // WSG/EotS: our flag carrier takes a danger-weighted route; escorts walk ahead on it
    EscortStick = 16,    // WSG/EotS: escorts keep up with our carrier through fights not aimed at it
    DirectTime = 17,     // with DirectPath: routes minimise travel time (indoor stretches, no mount, cost 2x)
    SmoothMove = 18,     // with DirectPath: the next hop is sent before the current one ends (no stops)
    DirectSafe = 19,     // with DirectPath: routes mildly avoid enemies ahead
    GroupMove = 20,      // with Allocator: attack/defend groups gather and travel together
    ChaseV3 = 21,        // with FCChase: everyone tries; per-kind checks (stuns over slows); cast-time CC cancelled
    FocusFire = 22,      // WSG/AB/EotS: bots in a fight converge on one enemy (carrier, healers, low health first)
    FocusPartial = 23,   // focus fire with at most 3 of our bots per target (no overkill)
    TowerRush = 24,      // with Allocator (EotS): no flag jobs, every bot on towers (4 towers: enemy cannot capture)
    EYPlan = 25,         // with Allocator (EotS): hold 2 home towers, 7 on the flag, 4 rotators trade towers
    ClassRoles = 26,     // with Allocator (WSG): speed classes on attack, stun classes on defense and the chase
    HealerHunt = 27,     // WSG: up to 2 bots go after the enemy carrier's healer (interrupt/stun first)
    AB3Cap = 28,         // with Allocator (AB): take a compact 3, then hold them instead of attacking on
    Rebuff = 29,         // retired: bots stay 98-100% buffed without it; re-adding +buff every tick froze the team
    JobFocus = 30,       // all BGs: bots pick new fights only in their way (12 yd) or at their objective (25 yd)
    GYWave = 31,         // all BGs: after a respawn, wait for 3 teammates nearby or 10 s before leaving
    BackOff = 32,        // WSG/AB/EotS: 2+ extra enemies within 20 yd and under 50% health: retreat toward help
    WPGraph = 33,        // WSG/AB/EotS: waypoint routes as one road network, shortest path (no backtracking)
    Comeback = 34,       // WSG/AB (allocator), EotS (stock): a team that is behind takes more risk
    Intercept = 35,      // WSG (allocator): 2 chase the enemy carrier, the rest cut it off on its way home
    EscortHealer = 36,   // WSG (allocator): one healer always in the escort of our carrier
    StandoffBreak = 37,  // WSG (allocator): both flags held 90 s+: all but the escort go for their carrier
    Reinforce = 38,      // AB (allocator): 3+ enemies heading for a node we hold raise its defense early
    Retake = 39,         // AB (allocator): an enemy banner on a node is retaken in force (their bots + 2)
    NoSolo = 40,         // WSG/AB/EotS: no new fight alone (no friend within 20 yd) against 2+ enemies
    FinishKill = 41,     // WSG/AB/EotS: switch to an enemy under 30% health in reach
    EYRetake = 42,       // EotS (stock): first 5 min, a lost near tower is retaken by the 5 nearest bots first
    DirectCarrier = 43,  // WSG/EotS, with DirectPath: flag carriers take direct routes too (v3 keeps them on waypoints)
    AVHoldTower = 44,    // AV: a tower/bunker we assaulted keeps up to 4 bots on it until it burns
    AVBossForce = 45,    // AV: the general only in force (20+ of us near, or his last graveyard ours); else wait
    AVCaptainHome = 46,  // AV: defenders skip the rush to the enemy captain
    ICGuardFix = 47,     // IoC: Alliance idle bots guard nodes not yet ours (stock checks the Horde state)
    ICStayVehicle = 48,  // IoC: bots do not leave a vehicle at random
    AoESquad2 = 49,      // AV/IoC: 2 area-damage bots hold behind a busy choke and hit the densest pack (BGAoeSquad)
    AoESquad5 = 50,      // AV/IoC: the same with 5
    AoESquadAll = 51,    // AV/IoC: the same with every area-damage bot
    AoEGeneral = 52,     // AV: 5+ enemies at our general: every area-damage bot goes to him and hits the packs
    BossRaid = 53,       // AV: the enemy general as a raid boss: gather, tanks pull, guards first, healers on tanks
    Disrupt = 54,        // AV: 5 bots hit the enemies fighting our general/captain (healers, their tank first)
    AVMines = 55,        // AV: 5 bots (not 1) go for the mine boss
    ICSiegeEscort = 56,  // IoC: up to 5 bots escort each of our demolishers / siege engines
    AVGraveyards = 57,   // AV: attackers take the nearest capturable graveyard before towers
    HealerGuard = 58,    // any BG without the allocator: a melee bodyguard per healer
    EYSafeTower = 59,    // EotS: the carrier runs to the owned tower with the fewest enemies, not the nearest
};

// Per-game A/B arms. With AiPlayerbot.BGTactics.<BG>.Arms set (e.g.
// "FCEscort.Alliance, FCEscort.Horde, FCChase.Alliance+FCChase.Horde, stock"), each game of that BG
// is given the next arm in the list when it is first looked up, so several experiments and both
// side assignments run at once. Without it, the per-faction switches
// (AiPlayerbot.BGTactics.<BG>.<Tactic>.<Faction>) apply to every game.
namespace BGTacticArms
{
    // Is the tactic on for this team in this game?
    bool IsOn(Battleground* bg, TeamId team, BGTactic tactic);

    // The game's arm, as written in the config ("" when its BG has no arms), for logs.
    std::string ArmName(Battleground* bg);

    // Drop a destroyed game's assignment.
    void Forget(Battleground* bg);

    // Eye of the Storm arms (AiPlayerbot.BGTactics.EY.Arms) live here rather than in PlayerbotAIConfig;
    // the experiment hot reload sets them.
    void SetEYArms(std::string const& arms);

    // Arms of the BGs kept here (EY, AV, IC: AiPlayerbot.BGTactics.<EY|AV|IC>.Arms), set by the hot reload.
    void SetArms(BattlegroundTypeId type, std::string const& arms);

    // Baseline (AiPlayerbot.BGTactics.<WSG|AB|EY>.Baseline, e.g. "FCEscort, FCChase"): accepted tactics, on for
    // both teams in every game of that BG; arms add their tactics on top. A game keeps the baseline it started
    // with. The experiment hot reload sets it.
    void SetBaseline(BattlegroundTypeId type, std::string const& tactics);
}

#endif
