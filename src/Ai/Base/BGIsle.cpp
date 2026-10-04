/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGIsle.h"

#include <cmath>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

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
#include "Vehicle.h"

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
GameObject* NearestOwn(Player* bot, Battleground* bg, uint32 from, uint32 to, uint32 faction, float maxDist)
{
    GameObject* best = nullptr;
    for (uint32 i = from; i <= to; ++i)
    {
        GameObject* go = bg->GetBgMap()->GetGameObject(bg->BgObjects[i]);  // GetBGObject logs every empty slot
        if (!go || !go->isSpawned() || go->GetUInt32Value(GAMEOBJECT_FACTION) != faction ||
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
// Our keep gates (standing or not): the keep a threat is measured against.
bool NearOurKeep(Battleground* bg, TeamId team, Unit* u, float dist)
{
    uint32 const first = team == TEAM_ALLIANCE ? BG_IC_GO_ALLIANCE_GATE_1 : BG_IC_GO_HORDE_GATE_1;
    for (uint32 g = 0; g < 3; ++g)
        if (GameObject* gate = bg->GetBGObject(first + g); gate && u->GetExactDist2d(gate) < dist)
            return true;
    return false;
}

bool SiegeVehicle(uint32 entry)
{
    switch (entry)
    {
        case NPC_DEMOLISHER:
        case NPC_SIEGE_ENGINE_A: case NPC_SIEGE_ENGINE_H:
        case NPC_GLAIVE_THROWER_A: case NPC_GLAIVE_THROWER_H:
            return true;
        default:
            return false;
    }
}

// An enemy siege vehicle within 110 yd of our keep (the siege spots are 59-65 yd out).
bool SiegeThreat(Battleground* bg, TeamId team)
{
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        Unit* veh = p && p->IsAlive() && p->GetBgTeamId() != team ? p->GetVehicleBase() : nullptr;
        if (veh && veh->IsAlive() && SiegeVehicle(veh->GetEntry()) && NearOurKeep(bg, team, veh, 110.0f))
            return true;
    }
    return false;
}

bool KeepCannon(Unit* u) { return u && u->GetEntry() == NPC_KEEP_CANNON; }
}  // namespace

namespace BGIsle
{
bool Objective(Player* bot, Battleground* bg, Order& out)
{
    TeamId const team = bot->GetBgTeamId();
    uint32 const n = bot->GetGUID().GetCounter();
    if (PlayerbotAI::IsHeal(bot))
        return false;

    // ICCannons: crew (a fifth of our non-healers) mans our keep cannons while enemy siege is near the keep
    if (BGTacticArms::IsOn(bg, team, BGTactic::ICCannons))
    {
        bool const crew = n % 5 == 2, threat = crew && SiegeThreat(bg, team);
        Unit* in = bot->GetVehicleBase();
        if (KeepCannon(in))
        {
            out.move = in->GetPosition();
            out.leave = !threat;
            return true;
        }
        if (threat && !bot->GetVehicle())
        {
            Creature* best = nullptr;
            for (uint32 i = BG_IC_NPC_KEEP_CANNON_1; i <= BG_IC_NPC_KEEP_CANNON_25; ++i)
            {
                Creature* c = bg->GetBGCreature(i);
                if (!c || !c->IsAlive() || !KeepCannon(c) || c->GetFaction() != BG_IC_Factions[team] ||
                    !c->GetVehicleKit() || c->GetVehicleKit()->IsVehicleInUse())
                    continue;
                if (!best || bot->GetExactDist2d(c) < bot->GetExactDist2d(best))
                    best = c;
            }
            if (best && bot->GetExactDist2d(best) < 250.0f)
            {
                out.move = best->GetPosition();
                out.board = best;
                return true;
            }
        }
    }
    if (bot->GetVehicle())
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
            GameObject* pile =
                NearestOwn(bot, bg, BG_IC_GO_SEAFORIUM_BOMBS_1, BG_IC_GO_SEAFORIUM_BOMBS_2, BG_IC_Factions[team], 400.0f);
            // ICHugeBombs: our usable huge piles are in the enemy keep (faction 1997 for us as Alliance, 1995 as Horde),
            // reachable once one of its gates is down (without it the old lookup never matched: wrong keep and faction)
            GameObject* huge = nullptr;
            if (BGTacticArms::IsOn(bg, team, BGTactic::ICHugeBombs) && EnemyGateDown(bg, team))
            {
                uint32 const hugeFirst =
                    team == TEAM_ALLIANCE ? BG_IC_GO_HUGE_SEAFORIUM_BOMBS_H_1 : BG_IC_GO_HUGE_SEAFORIUM_BOMBS_A_1;
                huge = NearestOwn(bot, bg, hugeFirst, hugeFirst + 3, team == TEAM_ALLIANCE ? 1997 : 1995, 400.0f);
            }
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
        if (GameObject* portal =
                NearestOwn(bot, bg, BG_IC_GO_HANGAR_TELEPORTER_1, BG_IC_GO_HANGAR_TELEPORTER_3, BG_IC_Factions[team], 500.0f))
        {
            out.move = portal->GetPosition();
            out.use = portal;
            return true;
        }
    return false;
}

Unit* GunnerTarget(Player* bot, Battleground* bg)
{
    Unit* gun = bot->GetVehicleBase();
    if (!KeepCannon(gun))
        return nullptr;
    Unit* best = nullptr;
    float bestDist = 100.0f;
    for (bool siege : {true, false})  // enemy siege vehicles first, then players
    {
        for (auto const& ref : bg->GetBgMap()->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || !p->IsAlive() || p->GetBgTeamId() == bot->GetBgTeamId())
                continue;
            Unit* t = siege ? p->GetVehicleBase() : (p->GetVehicle() ? nullptr : p);
            if (!t || !t->IsAlive() || (siege && !SiegeVehicle(t->GetEntry())))
                continue;
            float const dist = gun->GetExactDist2d(t);
            if (dist < bestDist && gun->IsWithinLOSInMap(t) && bot->CanSeeOrDetect(t))
            {
                bestDist = dist;
                best = t;
            }
        }
        if (best)
            return best;
    }
    return nullptr;
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

namespace
{
std::mutex crewLock;
std::map<std::string, uint32> crewCounts;
uint32 crewLogMs = 0;
}  // namespace

void BGIsle::CrewCount(std::string const& key)
{
    std::lock_guard<std::mutex> guard(crewLock);
    ++crewCounts[key];
    uint32 const now = getMSTime();
    if (!crewLogMs)
        crewLogMs = now;
    else if (getMSTimeDiff(crewLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        std::ostringstream o;
        for (auto const& [k, n] : crewCounts)
            o << ' ' << k << '=' << n;
        LOG_INFO("module", "IC siege crew (5 min):{}", o.str());
        crewCounts.clear();
        crewLogMs = now;
    }
}
