/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGStrand.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "BGTacticArms.h"
#include "BattlegroundSA.h"
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

// The floor under an object (the relic floats over its dais; routes to it never complete).
Position Floor(GameObject* go)
{
    float const z = go->GetMap()->GetHeight(go->GetPhaseMask(), go->GetPositionX(), go->GetPositionY(),
                                            go->GetPositionZ(), true, 30.0f);
    return Position(go->GetPositionX(), go->GetPositionY(), z > INVALID_HEIGHT ? z : go->GetPositionZ());
}

// Living players of a team within dist yd of a point.
uint32 TeamNear(Battleground* bg, TeamId team, Position const& p, float dist)
{
    uint32 n = 0;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
        if (Player* pl = ref.GetSource(); pl && pl->IsAlive() && pl->GetTeamId() == team && pl->GetExactDist2d(&p) < dist)
            ++n;
    return n;
}

// SALane: the lane all attackers push, per game and round. Chosen by fewer defenders at the two candidate gates;
// changed only for a clear difference (4+), at most every 30 s.
struct LaneChoice
{
    TeamId attackers = TEAM_NEUTRAL;
    uint32 lane = 0;
    uint32 ms = 0;
};
std::mutex laneLock;
std::unordered_map<uint32, LaneChoice> lanes;  // by BG instance id

uint32 AttackLane(Battleground* bg, TeamId attackers)
{
    bool const green = Destroyed(bg, BG_SA_GREEN_GATE), blue = Destroyed(bg, BG_SA_BLUE_GATE);
    uint32 cand[2] = {BG_SA_GREEN_GATE, BG_SA_BLUE_GATE};
    if (green && blue)
    {
        cand[0] = BG_SA_PURPLE_GATE;
        cand[1] = BG_SA_RED_GATE;
    }
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(laneLock);
    LaneChoice& c = lanes[bg->GetInstanceID()];
    if (c.attackers != attackers)
        c = LaneChoice{attackers, 0, 0};
    // one outer gate open: its lane (the gate behind it is the only one reachable)
    if (green != blue)
        return c.lane = green ? 0 : 1;
    if (c.ms && getMSTimeDiff(c.ms, now) < 30000)
        return c.lane;
    TeamId const defenders = attackers == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    uint32 def[2];
    for (uint32 l = 0; l < 2; ++l)
    {
        GameObject* g = bg->GetBGObject(cand[l]);
        def[l] = g ? TeamNear(bg, defenders, g->GetPosition(), 45.0f) : 0;
    }
    uint32 const other = 1 - c.lane;
    if (!c.ms ? def[other] < def[c.lane] : def[other] + 4 <= def[c.lane])
        c.lane = other;
    c.ms = now;
    return c.lane;
}

// SAGateFocus: one random lane per round for every attacker, kept until its gate falls (then NextGate moves on).
std::unordered_map<uint32, std::pair<TeamId, uint32>> focusLanes;  // by BG instance id: round's attackers, lane

uint32 FocusLane(Battleground* bg, TeamId attackers)
{
    bool const green = Destroyed(bg, BG_SA_GREEN_GATE), blue = Destroyed(bg, BG_SA_BLUE_GATE);
    if (green != blue)
        return green ? 0 : 1;  // one outer gate open: its lane (the gate behind it is the only one reachable)
    std::lock_guard<std::mutex> guard(laneLock);
    auto& f = focusLanes[bg->GetInstanceID()];
    if (f.first != attackers)
        f = {attackers, urand(0, 1)};
    return f.second;
}

// SAHunt: an enemy-driven demolisher within 50 yd of the gate.
bool SiegeAtGate(Battleground* bg, GameObject* gate)
{
    for (uint32 i = BG_SA_DEMOLISHER_1; i <= BG_SA_DEMOLISHER_8; ++i)
        if (Creature* d = bg->GetBGCreature(i); d && d->IsAlive() && d->GetVehicleKit() &&
                                                d->GetVehicleKit()->IsVehicleInUse() && d->GetExactDist2d(gate) < 50.0f)
            return true;
    return false;
}

// SACannons: the anti-personnel cannons within 45 yd of a gate.
std::vector<Creature*> GunsAt(Battleground* bg, GameObject* gate)
{
    std::vector<Creature*> out;
    for (uint32 i = BG_SA_GUN_1; i <= BG_SA_GUN_10; ++i)
        if (Creature* gun = bg->GetBGCreature(i); gun && gun->IsAlive() && gun->GetExactDist2d(gate) < 45.0f)
            out.push_back(gun);
    return out;
}

// SACannons: this defender's cannon at its gate, if it is one of the first (by guid) non-healer defenders of its lane
// alive, one per cannon; nullptr otherwise.
Creature* GunFor(Player* bot, Battleground* bg, GameObject* gate, uint32 lane)
{
    std::vector<Creature*> guns = GunsAt(bg, gate);
    if (guns.empty() || PlayerbotAI::IsHeal(bot))
        return nullptr;
    std::vector<uint32> crew;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
        if (Player* p = ref.GetSource(); p && p->IsAlive() && p->GetTeamId() == bot->GetTeamId() &&
                                         p->GetGUID().GetCounter() % 2 == lane && !PlayerbotAI::IsHeal(p))
            crew.push_back(p->GetGUID().GetCounter());
    std::sort(crew.begin(), crew.end());
    auto const it = std::find(crew.begin(), crew.end(), bot->GetGUID().GetCounter());
    size_t const idx = it - crew.begin();
    if (it == crew.end() || idx >= std::min<size_t>(guns.size(), 2))
        return nullptr;
    std::sort(guns.begin(), guns.end(), [](Creature* a, Creature* b) { return a->GetGUID().GetCounter() < b->GetGUID().GetCounter(); });
    return guns[idx];
}

constexpr uint32 ITEM_MASSIVE_SEAFORIUM_CHARGE = 39213;

// SACharges: the nearest bomb pile (spawned) within 150 yd.
GameObject* NearestBombPile(Player* bot, Battleground* bg)
{
    GameObject* best = nullptr;
    float bestDist = 150.0f;
    for (uint32 i = BG_SA_BOMB; i < BG_SA_MAXOBJ; ++i)
        if (GameObject* go = bg->GetBGObject(i); go && go->isSpawned())
            if (float const d = bot->GetExactDist2d(go); d < bestDist)
            {
                bestDist = d;
                best = go;
            }
    return best;
}

// SAPortals: the Defender's Portal whose destination saves this bot 40+ yd on its way to dest (straight lines), if any.
// Portal order in BG_SA_Objects: blue, green, yellow, purple, red; SOTADefPortalDest: green, yellow, blue, red, purple.
GameObject* PortalToward(Player* bot, Battleground* bg, Position const& dest)
{
    static uint32 const destOf[5] = {2, 0, 1, 4, 3};
    float const direct = bot->GetExactDist2d(&dest);
    GameObject* best = nullptr;
    float bestSaving = 40.0f;
    for (uint32 i = 0; i < 5; ++i)
    {
        GameObject* portal = bg->GetBGObject(BG_SA_PORTAL_DEFFENDER_BLUE + i);
        if (!portal)
            continue;
        float const* d = SOTADefPortalDest[destOf[i]];
        float const via = bot->GetExactDist2d(portal) + std::hypot(d[0] - dest.GetPositionX(), d[1] - dest.GetPositionY());
        if (direct - via > bestSaving)
        {
            bestSaving = direct - via;
            best = portal;
        }
    }
    return best;
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
    bool const attacking = bot->GetTeamId() == attackers;
    uint32 lane = bot->GetGUID().GetCounter() % 2;
    if (attacking && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SALane))
        lane = AttackLane(bg, attackers);
    if (attacking && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAGateFocus))
        lane = FocusLane(bg, attackers);
    uint32 const target = NextGate(bg, lane);
    GameObject* go = bg->GetBGObject(target);
    if (!go)
        return false;

    if (!attacking)
    {
        if (target == BG_SA_TITAN_RELIC)
        {
            out.move = Floor(go);
            return true;
        }
        if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACannons))
        {
            Creature* gun = GunFor(bot, bg, go, lane);
            Unit* in = bot->GetVehicleBase();
            if (in && in->GetEntry() == NPC_ANTI_PERSONNAL_CANNON)
            {
                out.leave = in != gun;  // stay in our gate's cannon only
                out.move = in->GetPosition();
                return true;
            }
            if (gun && !gun->GetVehicleKit()->IsVehicleInUse())
            {
                out.move = gun->GetPosition();
                out.board = gun;
                return true;
            }
        }
        out.move = Near(go, -10.0f, bot);
        // SAHunt: while enemy siege is at the gate, hunters step outside it (no line of sight from inside or the wall)
        if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAHunt) && !PlayerbotAI::IsHeal(bot) &&
            SiegeAtGate(bg, go))
            out.move = Near(go, 6.0f, bot);
        if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAPortals))
            if (GameObject* portal = PortalToward(bot, bg, out.move))
            {
                out.move = portal->GetPosition();
                out.portal = portal;
            }
        return true;
    }

    if (target == BG_SA_TITAN_RELIC)
    {
        out.move = Floor(go);
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

    // SACharges: a third of the attackers on foot (not healers) carry charges from the piles to the gate
    if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACharges) && bot->GetGUID().GetCounter() % 3 == 0 &&
        !PlayerbotAI::IsHeal(bot))
    {
        if (bot->HasItemCount(ITEM_MASSIVE_SEAFORIUM_CHARGE, 1))
        {
            out.move = Near(go, 2.0f, bot);
            out.plantAt = go;
            return true;
        }
        if (GameObject* pile = NearestBombPile(bot, bg))
        {
            out.move = pile->GetPosition();
            out.pickup = pile;
            return true;
        }
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
Unit* HuntTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Battleground* bg = bot->GetBattleground();
    if (!bg || !bot->IsAlive() || bot->GetVehicle() || PlayerbotAI::IsHeal(bot))
        return nullptr;
    bool const bombs = BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SABombHunt);
    if (!bombs && !BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAHunt))
        return nullptr;
    TeamId const attackers = Attackers(bg);
    if (attackers == TEAM_NEUTRAL || bot->GetTeamId() == attackers)
        return nullptr;
    GameObject* gate = bg->GetBGObject(NextGate(bg, bot->GetGUID().GetCounter() % 2));
    if (!gate)
        return nullptr;
    // SABombHunt: an enemy carrying a charge near our gate is about to plant it
    if (bombs)
    {
        Unit* carrier = nullptr;
        float carrierDist = 60.0f;
        for (auto const& ref : bg->GetBgMap()->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || !p->IsAlive() || p->GetTeamId() != attackers || !p->HasItemCount(ITEM_MASSIVE_SEAFORIUM_CHARGE, 1) ||
                p->GetExactDist2d(gate) > 45.0f || !bot->IsValidAttackTarget(p) || !bot->CanSeeOrDetect(p))
                continue;
            if (float const dist = bot->GetExactDist2d(p); dist < carrierDist)
            {
                carrierDist = dist;
                carrier = p;
            }
        }
        if (carrier)
            return carrier;
    }
    if (!BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAHunt))
        return nullptr;
    Unit* best = nullptr;
    float bestDist = 60.0f;
    for (uint32 i = BG_SA_DEMOLISHER_1; i <= BG_SA_DEMOLISHER_8; ++i)
    {
        Creature* d = bg->GetBGCreature(i);
        if (!d || !d->IsAlive() || !d->IsVisible() || d->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE) ||
            d->GetExactDist2d(gate) > 50.0f || !bot->IsValidAttackTarget(d) || !bot->IsWithinLOSInMap(d))
            continue;
        if (float const dist = bot->GetExactDist2d(d); dist < bestDist)
        {
            bestDist = dist;
            best = d;
        }
    }
    return best;
}

Unit* GunnerTarget(Player* bot, Battleground* bg)
{
    Unit* gun = bot->GetVehicleBase();
    if (!gun || gun->GetEntry() != NPC_ANTI_PERSONNAL_CANNON)
        return nullptr;
    // enemy demolishers first: they are what takes the gate
    Unit* best = nullptr;
    float bestDist = 70.0f;
    for (uint32 i = BG_SA_DEMOLISHER_1; i <= BG_SA_DEMOLISHER_8; ++i)
        if (Creature* d = bg->GetBGCreature(i); d && d->IsAlive() && d->IsVisible() && d->GetVehicleKit() &&
                                                d->GetVehicleKit()->IsVehicleInUse() && gun->IsWithinLOSInMap(d))
            if (float const dist = gun->GetExactDist2d(d); dist < bestDist)
            {
                bestDist = dist;
                best = d;
            }
    if (best)
        return best;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || !p->IsAlive() || p->GetTeamId() == bot->GetTeamId() || !gun->IsWithinLOSInMap(p) ||
            !bot->CanSeeOrDetect(p))
            continue;
        if (float const dist = gun->GetExactDist2d(p); dist < bestDist)
        {
            bestDist = dist;
            best = p;
        }
    }
    return best;
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
