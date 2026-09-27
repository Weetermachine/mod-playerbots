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

    // Baseline (AiPlayerbot.BGTactics.<WSG|AB|EY>.Baseline, e.g. "FCEscort, FCChase"): accepted tactics, on for
    // both teams in every game of that BG; arms add their tactics on top. A game keeps the baseline it started
    // with. The experiment hot reload sets it.
    void SetBaseline(BattlegroundTypeId type, std::string const& tactics);
}

#endif
