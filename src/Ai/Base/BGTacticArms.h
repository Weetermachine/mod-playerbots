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
}

#endif
