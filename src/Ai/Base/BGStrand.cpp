/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGStrand.h"

#include <cmath>

#include "BattlegroundSA.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Player.h"
#include "Transport.h"

namespace
{
constexpr float BEACH_X = 1600.381f;
constexpr float BEACH_Y = -106.263f;
constexpr float BEACH_Z = 8.8745f;

bool Destroyed(Battleground* bg, uint32 gate)
{
    GameObject* go = bg->GetBGObject(gate);
    return go && go->GetDestructibleState() == GO_DESTRUCTIBLE_DESTROYED;
}

// The gate the attackers of this lane work on next (lane 0: Green/Purple, 1: Blue/Red), or the relic.
uint32 NextGate(Battleground* bg, uint32 lane)
{
    bool const green = Destroyed(bg, BG_SA_GREEN_GATE);
    bool const blue = Destroyed(bg, BG_SA_BLUE_GATE);
    if (!green && !blue)
        return lane ? BG_SA_BLUE_GATE : BG_SA_GREEN_GATE;
    if (!Destroyed(bg, BG_SA_PURPLE_GATE) && !Destroyed(bg, BG_SA_RED_GATE))
    {
        if (green && blue)
            return lane ? BG_SA_RED_GATE : BG_SA_PURPLE_GATE;
        return green ? BG_SA_PURPLE_GATE : BG_SA_RED_GATE;  // only the gate behind the open one can be reached
    }
    if (!Destroyed(bg, BG_SA_YELLOW_GATE))
        return BG_SA_YELLOW_GATE;
    if (!Destroyed(bg, BG_SA_ANCIENT_GATE))
        return BG_SA_ANCIENT_GATE;
    return BG_SA_TITAN_RELIC;
}

// A point dist yd from the object toward the beach (outside a gate; negative: inside), spread per bot.
Position Near(GameObject* go, float dist, Player* bot)
{
    float const a = go->GetAngle(BEACH_X, BEACH_Y);
    uint32 const n = bot->GetGUID().GetCounter();
    float const x = go->GetPositionX() + dist * std::cos(a) + float(n * 37 % 9) - 4.0f;
    float const y = go->GetPositionY() + dist * std::sin(a) + float(n * 53 % 9) - 4.0f;
    float z = go->GetMap()->GetHeight(go->GetPhaseMask(), x, y, go->GetPositionZ() + 10.0f);
    if (z <= INVALID_HEIGHT)
        z = go->GetPositionZ();
    return Position(x, y, z);
}

// The graveyard banner if the enemy holds it (Alliance banners have even entries).
GameObject* EnemyBanner(Battleground* bg, uint32 flag, TeamId team)
{
    GameObject* go = bg->GetBGObject(flag);
    if (!go || (go->GetEntry() % 2 == 0) == (team == TEAM_ALLIANCE))
        return nullptr;
    return go;
}
}  // namespace

namespace BGStrand
{
TeamId Attackers(Battleground* bg)
{
    GameObject* relic = bg->GetBGObject(BG_SA_TITAN_RELIC);
    if (!relic)
        return TEAM_NEUTRAL;
    return relic->GetUInt32Value(GAMEOBJECT_FACTION) == BG_SA_Factions[TEAM_ALLIANCE] ? TEAM_ALLIANCE : TEAM_HORDE;
}

bool Objective(Player* bot, Battleground* bg, Order& out)
{
    TeamId const attackers = Attackers(bg);
    if (attackers == TEAM_NEUTRAL)
        return false;
    uint32 const lane = bot->GetGUID().GetCounter() % 2;
    uint32 const target = NextGate(bg, lane);
    GameObject* go = bg->GetBGObject(target);
    if (!go)
        return false;

    if (bot->GetTeamId() != attackers)
    {
        out.move = Near(go, target == BG_SA_TITAN_RELIC ? 0.0f : -10.0f, bot);
        return true;
    }

    if (target == BG_SA_TITAN_RELIC)
    {
        out.move = go->GetPosition();
        out.use = go;
        return true;
    }

    if (bot->GetVehicle())
    {
        out.move = Near(go, 12.0f, bot);
        out.siege = go->GetPosition();
        out.hasSiege = true;
        return true;
    }

    // take the graveyards behind the open outer gates, then the central one behind Purple/Red
    bool const green = Destroyed(bg, BG_SA_GREEN_GATE);
    bool const blue = Destroyed(bg, BG_SA_BLUE_GATE);
    GameObject* banner = nullptr;
    if (green && (lane == 0 || !blue))
        banner = EnemyBanner(bg, BG_SA_LEFT_FLAG, attackers);
    if (!banner && blue)
        banner = EnemyBanner(bg, BG_SA_RIGHT_FLAG, attackers);
    if (!banner && (Destroyed(bg, BG_SA_PURPLE_GATE) || Destroyed(bg, BG_SA_RED_GATE)))
        banner = EnemyBanner(bg, BG_SA_CENTRAL_FLAG, attackers);
    if (banner)
    {
        out.move = banner->GetPosition();
        out.use = banner;
        return true;
    }

    out.move = Near(go, 20.0f, bot);
    return true;
}

bool GoAshore(Player* bot, Battleground* bg)
{
    if (bot->GetPositionX() < 1700.0f || !bot->IsAlive() || bot->GetTeamId() != Attackers(bg))
        return false;
    if (Transport* boat = bot->GetTransport())
    {
        boat->RemovePassenger(bot, true);
        LOG_INFO("module", "SA boat: {} still at sea after the start, teleported (bg {})", bot->GetName(),
                 bg->GetInstanceID());
    }
    uint32 const n = bot->GetGUID().GetCounter();
    return bot->TeleportTo(bg->GetMapId(), BEACH_X + float(n * 37 % 9) - 4.0f, BEACH_Y + float(n * 53 % 9) - 4.0f,
                           BEACH_Z, 3.78f);
}
bool Board(Player* bot, Battleground* bg)
{
    if (bot->GetTransport() || bot->GetPositionX() < 1700.0f || !bot->IsAlive() || bot->GetTeamId() != Attackers(bg))
        return false;
    StaticTransport* boat = nullptr;
    for (uint32 i : {BG_SA_BOAT_ONE, BG_SA_BOAT_TWO})
        if (GameObject* go = bg->GetBGObject(i))
            if (StaticTransport* t = go->ToStaticTransport(); t && bot->GetExactDist2d(t) < 60.0f &&
                                                              (!boat || bot->GetExactDist2d(t) < bot->GetExactDist2d(boat)))
                boat = t;
    if (!boat)
        return false;

    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
    boat->AddPassenger(bot, true);
    // the core drops attackers above the deck with slow fall; set them down on it (the deck is the boat's model)
    float const deck = boat->GetMap()->GetHeight(boat->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(),
                                                 bot->GetPositionZ() + 5.0f, true, 30.0f);
    if (deck > boat->GetPositionZ() && deck < bot->GetPositionZ())
        bot->m_movementInfo.transport.pos.m_positionZ -= bot->GetPositionZ() - deck;
    LOG_INFO("module", "SA boat: {} boarded {} (deck {:.1f}, bg {})", bot->GetName(), boat->GetEntry(), deck,
             bg->GetInstanceID());
    return true;
}

bool Ride(Player* bot, Battleground* bg)
{
    Transport* t = bot->GetTransport();
    StaticTransport* boat = t ? t->ToStaticTransport() : nullptr;
    if (!boat)
        return false;
    if (boat->GetPauseTime() && (boat->GetGoState() != GO_STATE_ACTIVE || boat->GetPathProgress() != boat->GetPauseTime()))
        return true;  // sailing: the dock is the pause point of its path

    boat->RemovePassenger(bot, true);
    // land is toward lower x: the first dry ground 15-45 yd off the side
    Map* map = bot->GetMap();
    for (float d = 15.0f; d <= 45.0f; d += 10.0f)
    {
        float const x = bot->GetPositionX() - d;
        float const y = bot->GetPositionY();
        float const z = map->GetHeight(x, y, bot->GetPositionZ() + 20.0f);
        if (z > INVALID_HEIGHT && !map->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
        {
            bot->GetMotionMaster()->MoveJump(x, y, z, 15.0f, 8.0f);
            LOG_INFO("module", "SA boat: {} jumped ashore from {} at {:.0f} {:.0f} to {:.0f} {:.0f} {:.1f} (bg {})",
                     bot->GetName(), boat->GetEntry(), bot->GetPositionX(), bot->GetPositionY(), x, y, z,
                     bg->GetInstanceID());
            return true;
        }
    }
    LOG_INFO("module", "SA boat: {} found no ground off {} at {:.0f} {:.0f}, teleported (bg {})", bot->GetName(),
             boat->GetEntry(), bot->GetPositionX(), bot->GetPositionY(), bg->GetInstanceID());
    uint32 const n = bot->GetGUID().GetCounter();
    bot->TeleportTo(bg->GetMapId(), BEACH_X + float(n * 37 % 9) - 4.0f, BEACH_Y + float(n * 53 % 9) - 4.0f, BEACH_Z,
                    3.78f);
    return true;
}
}  // namespace BGStrand
