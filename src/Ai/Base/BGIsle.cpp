/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGIsle.h"

#include <cmath>

#include "BGTacticArms.h"
#include "BattlegroundIC.h"
#include "Creature.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Transport.h"

namespace
{
constexpr uint32 ITEM_SEAFORIUM_BOMBS = 46847;       // placed with 66268
constexpr uint32 ITEM_HUGE_SEAFORIUM_BOMBS = 47030;  // placed with 66674

bool GateDown(GameObject* gate) { return !gate || gate->GetGOValue()->Building.Health == 0; }

// The enemy keep's standing gate with the least health (the one the siege is working on).
GameObject* WeakestEnemyGate(Battleground* bg, TeamId team)
{
    uint32 const first = team == TEAM_ALLIANCE ? BG_IC_GO_HORDE_GATE_1 : BG_IC_GO_ALLIANCE_GATE_1;
    GameObject* best = nullptr;
    for (uint32 g = 0; g < 3; ++g)
    {
        GameObject* gate = bg->GetBGObject(first + g);
        if (!GateDown(gate) && (!best || gate->GetGOValue()->Building.Health < best->GetGOValue()->Building.Health))
            best = gate;
    }
    return best;
}

bool EnemyGateDown(Battleground* bg, TeamId team)
{
    uint32 const first = team == TEAM_ALLIANCE ? BG_IC_GO_HORDE_GATE_1 : BG_IC_GO_ALLIANCE_GATE_1;
    for (uint32 g = 0; g < 3; ++g)
        if (GateDown(bg->GetBGObject(first + g)))
            return true;
    return false;
}

// The nearest spawned object among BG object slots [from, to] with this team's faction (bomb piles, gunship portals).
GameObject* NearestOwn(Player* bot, Battleground* bg, uint32 from, uint32 to, TeamId team, float maxDist)
{
    GameObject* best = nullptr;
    for (uint32 i = from; i <= to; ++i)
    {
        GameObject* go = bg->GetBGObject(i);
        if (!go || !go->isSpawned() || go->GetUInt32Value(GAMEOBJECT_FACTION) != BG_IC_Factions[team] ||
            go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE))
            continue;
        if (float const d = bot->GetExactDist2d(go); d < maxDist)
        {
            maxDist = d;
            best = go;
        }
    }
    return best;
}
}  // namespace

namespace BGIsle
{
bool Objective(Player* bot, Battleground* bg, Order& out)
{
    TeamId const team = bot->GetBgTeamId();
    uint32 const n = bot->GetGUID().GetCounter();
    if (PlayerbotAI::IsHeal(bot) || bot->GetVehicle())
        return false;

    if (n % 5 == 0 && BGTacticArms::IsOn(bg, team, BGTactic::ICBombs))
    {
        uint32 const item = bot->HasItemCount(ITEM_HUGE_SEAFORIUM_BOMBS, 1) ? ITEM_HUGE_SEAFORIUM_BOMBS
                          : bot->HasItemCount(ITEM_SEAFORIUM_BOMBS, 1)      ? ITEM_SEAFORIUM_BOMBS : 0;
        if (item)
        {
            GameObject* gate = WeakestEnemyGate(bg, team);
            if (!gate)
                return false;
            // right in front of the gate, on the bot's side
            float const a = gate->GetAngle(bot);
            float const x = gate->GetPositionX() + 4.0f * std::cos(a), y = gate->GetPositionY() + 4.0f * std::sin(a);
            float z = gate->GetMap()->GetHeight(gate->GetPhaseMask(), x, y, gate->GetPositionZ() + 10.0f);
            out.move = Position(x, y, z > INVALID_HEIGHT ? z : gate->GetPositionZ());
            out.plantAt = gate;
            out.item = item;
            out.spell = item == ITEM_HUGE_SEAFORIUM_BOMBS ? 66674 : 66268;
            return true;
        }
        if (WeakestEnemyGate(bg, team))
        {
            uint32 const hugeFirst = team == TEAM_ALLIANCE ? BG_IC_GO_HUGE_SEAFORIUM_BOMBS_A_1 : BG_IC_GO_HUGE_SEAFORIUM_BOMBS_H_1;
            GameObject* pile = NearestOwn(bot, bg, BG_IC_GO_SEAFORIUM_BOMBS_1, BG_IC_GO_SEAFORIUM_BOMBS_2, team, 400.0f);
            GameObject* huge = NearestOwn(bot, bg, hugeFirst, hugeFirst + 3, team, 400.0f);
            if (huge && (!pile || bot->GetExactDist2d(huge) < bot->GetExactDist2d(pile) + 100.0f))
                pile = huge;  // the huge bombs do far more damage: worth a detour
            if (pile)
            {
                out.move = pile->GetPosition();
                out.use = pile;
                return true;
            }
        }
    }

    if (n % 5 == 1 && BGTacticArms::IsOn(bg, team, BGTactic::ICGunship) && EnemyGateDown(bg, team))
        if (GameObject* portal = NearestOwn(bot, bg, BG_IC_GO_HANGAR_TELEPORTER_1, BG_IC_GO_HANGAR_TELEPORTER_3, team, 500.0f))
        {
            out.move = portal->GetPosition();
            out.use = portal;
            return true;
        }
    return false;
}

bool Drop(Player* bot, Battleground* bg)
{
    Transport* ship = bot->GetTransport();
    if (!ship || !ship->ToMotionTransport() || !BGTacticArms::IsOn(bg, bot->GetBgTeamId(), BGTactic::ICGunship))
        return false;
    Creature* boss = bg->GetBGCreature(bot->GetBgTeamId() == TEAM_ALLIANCE ? BG_IC_NPC_OVERLORD_AGMAR
                                                                              : BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE);
    if (!boss || bot->GetExactDist2d(boss) > 90.0f)
        return true;  // riding
    ship->RemovePassenger(bot, true);
    bot->GetMotionMaster()->MoveFall();  // the gunship's parachute drop
    LOG_INFO("module", "IC gunship: {} jumped over the enemy keep at {:.0f} {:.0f} {:.0f} (bg {})", bot->GetName(),
             bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bg->GetInstanceID());
    return true;
}
}  // namespace BGIsle
