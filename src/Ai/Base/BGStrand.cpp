/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGStrand.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
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

// The siege vehicles attackers drive (workshop demolishers are not in the BG's demolisher slots).
std::vector<Unit*> DrivenSiege(Battleground* bg)
{
    std::vector<Unit*> out;
    TeamId const attackers = BGStrand::Attackers(bg);
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
        if (Player* p = ref.GetSource(); p && p->IsAlive() && p->GetTeamId() == attackers)
            if (Unit* v = p->GetVehicleBase(); v && v->IsAlive() && v->GetEntry() != NPC_ANTI_PERSONNAL_CANNON &&
                                               std::find(out.begin(), out.end(), v) == out.end())
                out.push_back(v);
    return out;
}

// SAHunt / SASortie: an attacker-driven siege vehicle within dist yd of the gate.
bool SiegeAtGate(Battleground* bg, GameObject* gate, float dist = 50.0f)
{
    for (Unit* v : DrivenSiege(bg))
        if (v->GetExactDist2d(gate) < dist)
            return true;
    return false;
}

// SADefendLane: the lane whose gate has more living attackers within 60 yd; switched for 3+ more, at most every 15 s.
std::unordered_map<uint32, LaneChoice> defendLanes;  // by BG instance id

uint32 DefendLane(Battleground* bg, TeamId attackers)
{
    bool const green = Destroyed(bg, BG_SA_GREEN_GATE), blue = Destroyed(bg, BG_SA_BLUE_GATE);
    if (green != blue)
        return green ? 0 : 1;  // one outer gate open: only the gate behind it can be reached
    uint32 cand[2] = {BG_SA_GREEN_GATE, BG_SA_BLUE_GATE};
    if (green && blue)
    {
        cand[0] = BG_SA_PURPLE_GATE;
        cand[1] = BG_SA_RED_GATE;
    }
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(laneLock);
    LaneChoice& c = defendLanes[bg->GetInstanceID()];
    if (c.attackers != attackers)
        c = LaneChoice{attackers, 0, 0};
    if (c.ms && getMSTimeDiff(c.ms, now) < 15000)
        return c.lane;
    uint32 att[2];
    for (uint32 l = 0; l < 2; ++l)
    {
        GameObject* g = bg->GetBGObject(cand[l]);
        att[l] = g ? TeamNear(bg, attackers, g->GetPosition(), 60.0f) : 0;
    }
    uint32 const other = 1 - c.lane;
    if (!c.ms ? att[other] > att[c.lane] : att[other] >= att[c.lane] + 3)
        c.lane = other;
    c.ms = now;
    return c.lane;
}

// SASortie diagnostics (Server.log, per 5 min): distinct bots per stage; how far short the ones never at the wall were
std::mutex sortieLock;
std::unordered_set<ObjectGuid::LowType> sortieSeen[4];  // sent, at the wall, jumped, outside
std::unordered_map<ObjectGuid::LowType, std::pair<float, float>> sortieShort;  // last 2D distance and height gap
uint32 sortieLogMs = 0;

void SortieStage(Player* bot, uint32 stage, float dist = 0.0f, float dz = 0.0f)
{
    std::lock_guard<std::mutex> guard(sortieLock);
    ObjectGuid::LowType const id = bot->GetGUID().GetCounter();
    sortieSeen[stage].insert(id);
    if (stage == 0)
        sortieShort[id] = {dist, dz};
    else
        sortieShort.erase(id);
    uint32 const now = getMSTime();
    if (!sortieLogMs)
        sortieLogMs = now;
    else if (getMSTimeDiff(sortieLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        uint32 near = 0, mid = 0, far = 0, high = 0;
        for (auto const& [guid, d] : sortieShort)
        {
            (d.first < 10.0f ? near : d.first < 30.0f ? mid : far)++;
            high += d.second >= 6.0f ? 1 : 0;
        }
        LOG_INFO("module", "Sortie stages (5 min, distinct bots): sent {}, at the wall {}, jumped {}, outside {}; never at the wall: within 10 yd {}, 10-30 {}, 30+ {} (6+ yd height gap {})",
                 sortieSeen[0].size(), sortieSeen[1].size(), sortieSeen[2].size(), sortieSeen[3].size(), near, mid, far, high);
        for (auto& s : sortieSeen)
            s.clear();
        sortieShort.clear();
        sortieLogMs = now;
    }
}

// SACannons diagnostics (Server.log, per 5 min): enemies on foot per gunner look: all, within 70 yd, 10-70 yd, shootable
std::mutex sightLock;
uint64 sight[5] = {};  // looks, all, within 70, in range, shootable
uint32 sightLogMs = 0;

void GunnerSight(uint32 all, uint32 in70, uint32 inRange, uint32 shootable)
{
    std::lock_guard<std::mutex> guard(sightLock);
    ++sight[0];
    sight[1] += all;
    sight[2] += in70;
    sight[3] += inRange;
    sight[4] += shootable;
    uint32 const now = getMSTime();
    if (!sightLogMs)
        sightLogMs = now;
    else if (getMSTimeDiff(sightLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        float const n = float(sight[0]);
        LOG_INFO("module", "Gunner sight SA (5 min): looks {}, enemies on foot per look {:.1f}, within 70 yd {:.1f}, 10-70 yd {:.1f}, in line of sight {:.1f}",
                 sight[0], sight[1] / n, sight[2] / n, sight[3] / n, sight[4] / n);
        std::fill(std::begin(sight), std::end(sight), 0);
        sightLogMs = now;
    }
}

// SASortie diagnostics (Server.log, per 5 min): defender objective calls, sortie bots, siege near their gate or any gate
std::mutex probeLock;
uint64 probe[6] = {};  // calls, sortie role, siege at own gate (70 yd), siege at any gate (70 yd), driven siege seen, arm on
uint32 probeLogMs = 0;

void SortieProbe(bool role, bool own, bool any, bool driven, bool arm)
{
    std::lock_guard<std::mutex> guard(probeLock);
    ++probe[0];
    probe[1] += role;
    probe[2] += own;
    probe[3] += any;
    probe[4] += driven;
    probe[5] += arm;
    uint32 const now = getMSTime();
    if (!probeLogMs)
        probeLogMs = now;
    else if (getMSTimeDiff(probeLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        LOG_INFO("module", "Sortie probe (5 min): defender calls {}, arm on {}, sortie role {}, driven siege anywhere {}, near any gate {}, near own gate {}",
                 probe[0], probe[5], probe[1], probe[4], probe[3], probe[2]);
        std::fill(std::begin(probe), std::end(probe), 0);
        probeLogMs = now;
    }
}

// SASortie: half the non-healer defenders.
bool SortieRole(Player* bot) { return !PlayerbotAI::IsHeal(bot) && (bot->GetGUID().GetCounter() / 3) % 2 == 0; }

// Outside a gate: nearer the beach than the gate.
bool Outside(Player* bot, GameObject* gate)
{
    return bot->GetExactDist2d(BEACH_X, BEACH_Y) + 4.0f < gate->GetExactDist2d(BEACH_X, BEACH_Y);
}

uint32 DefenderLane(Player* bot, Battleground* bg);

// SASortie: 3+ attackers on foot within 40 yd outside the gate (charges take the gates, not siege).
bool Massing(Battleground* bg, GameObject* gate)
{
    uint32 n = 0;
    TeamId const attackers = BGStrand::Attackers(bg);
    float const gateToBeach = gate->GetExactDist2d(BEACH_X, BEACH_Y);
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
        if (Player* p = ref.GetSource(); p && p->IsAlive() && p->GetTeamId() == attackers && !p->GetVehicle() &&
                                         p->GetExactDist2d(gate) < 40.0f && p->GetExactDist2d(BEACH_X, BEACH_Y) < gateToBeach)
            ++n;
    return n >= 3;
}

// SACannons: attackers on foot within a cannon's Rocket Blast range (10-70 yd).
uint32 FoesInRange(Battleground* bg, Unit* gun)
{
    uint32 n = 0;
    TeamId const attackers = BGStrand::Attackers(bg);
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
        if (Player* p = ref.GetSource(); p && p->IsAlive() && p->GetTeamId() == attackers && !p->GetVehicle())
            if (float const d = gun->GetExactDist2d(p); d >= 10.0f && d <= 70.0f)
                ++n;
    return n;
}

// SACannons: the free cannon with the most attackers in range (2+), if any.
Creature* BusiestGun(Battleground* bg)
{
    Creature* best = nullptr;
    uint32 bestN = 1;
    for (uint32 i = BG_SA_GUN_1; i <= BG_SA_GUN_10; ++i)
        if (Creature* gun = bg->GetBGCreature(i); gun && gun->IsAlive() && gun->GetVehicleKit() &&
                                                  !gun->GetVehicleKit()->IsVehicleInUse())
            if (uint32 const n = FoesInRange(bg, gun); n > bestN)
            {
                bestN = n;
                best = gun;
            }
    return best;
}

bool SortieRole(Player* bot);

// SACannons: a quarter of the non-healer defenders (not sortie bots) crew the cannons.
bool GunCrew(Player* bot, Battleground* bg)
{
    return !PlayerbotAI::IsHeal(bot) && bot->GetGUID().GetCounter() % 4 == 1 &&
           !(BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SASortie) && SortieRole(bot));
}

constexpr uint32 ITEM_MASSIVE_SEAFORIUM_CHARGE = 39213;

// SACharges: the nearest bomb pile (spawned) within 150 yd; SAChargesOnWay: only one that adds at most 30 yd to the way to the gate.
GameObject* NearestBombPile(Player* bot, Battleground* bg, GameObject* gate, bool onWay)
{
    GameObject* best = nullptr;
    float bestDist = 150.0f;
    float const direct = bot->GetExactDist2d(gate);
    for (uint32 i = BG_SA_BOMB; i < BG_SA_MAXOBJ; ++i)
        if (GameObject* go = bg->GetBGObject(i); go && go->isSpawned())
            if (float const d = bot->GetExactDist2d(go); d < bestDist && (!onWay || d + go->GetExactDist2d(gate) <= direct + 30.0f))
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
}  // namespace BGStrand

namespace
{
uint32 DefenderLane(Player* bot, Battleground* bg)
{
    if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADefendLane))
        if (TeamId const attackers = BGStrand::Attackers(bg); attackers != TEAM_NEUTRAL && bot->GetTeamId() != attackers)
            return DefendLane(bg, attackers);
    return bot->GetGUID().GetCounter() % 2;
}
}  // namespace

namespace BGStrand
{

namespace
{
constexpr uint32 GO_PLANTED_CHARGE = 190752;

// SADefense diagnostics (Server.log, per 5 min): distinct defenders per role and outside their gate, disarms
std::mutex defLock;
std::unordered_set<ObjectGuid::LowType> defSeen[6];  // out, outside, hold, graveyard, relic door, disarming
uint32 defDisarms = 0, defLogMs = 0;

void DefenseStage(Player* bot, uint32 role)
{
    std::lock_guard<std::mutex> guard(defLock);
    defSeen[role].insert(bot->GetGUID().GetCounter());
    uint32 const now = getMSTime();
    if (!defLogMs)
        defLogMs = now;
    else if (getMSTimeDiff(defLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        LOG_INFO("module", "Defense roles (5 min, distinct bots): out {} (outside {}), hold {}, graveyard {}, relic door {}, going to a charge {}, disarms {}",
                 defSeen[0].size(), defSeen[1].size(), defSeen[2].size(), defSeen[3].size(), defSeen[4].size(),
                 defSeen[5].size(), defDisarms);
        for (auto& d : defSeen)
            d.clear();
        defDisarms = 0;
        defLogMs = now;
    }
}

// The wall spot above a gate (SOTADefPortalDest shares the gate order), for jumping down outside it.
bool GoOut(Player* bot, Battleground* bg, GameObject* gate, uint32 gateIdx, float front, Order& out)
{
    if (Outside(bot, gate))
    {
        out.move = Near(gate, front, bot);
        return true;
    }
    out.urgent = true;
    float const* wall = SOTADefPortalDest[gateIdx];
    Position const top(wall[0], wall[1], wall[2]);
    if (bot->GetExactDist2d(&top) < 10.0f && std::fabs(bot->GetPositionZ() - wall[2]) < 6.0f)
    {
        float const a = gate->GetAngle(BEACH_X, BEACH_Y);
        float const x = wall[0] + 14.0f * std::cos(a), y = wall[1] + 14.0f * std::sin(a);
        float const z = gate->GetMap()->GetHeight(gate->GetPhaseMask(), x, y, wall[2]);
        out.jump = Position(x, y, z > INVALID_HEIGHT ? z : gate->GetPositionZ());
        out.hasJump = true;
    }
    out.move = top;
    if (GameObject* portal = PortalToward(bot, bg, top))
    {
        out.move = portal->GetPosition();
        out.portal = portal;
    }
    return false;
}

void HoldAt(Player* bot, Battleground* bg, Position const& pos, Order& out)
{
    out.move = pos;
    if (GameObject* portal = PortalToward(bot, bg, pos))  // fall back through the portals
    {
        out.move = portal->GetPosition();
        out.portal = portal;
    }
}
}  // namespace

void Disarmed()
{
    std::lock_guard<std::mutex> guard(defLock);
    ++defDisarms;
}

bool DefenseObjective(Player* bot, Battleground* bg, Order& out)
{
    TeamId const attackers = Attackers(bg);
    if (!BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADefense) || attackers == TEAM_NEUTRAL ||
        bot->GetTeamId() == attackers || bot->GetVehicle())
        return false;
    // this bot's rank among its team's healers or non-healers (by guid): roles and sides stay fixed
    bool const heal = PlayerbotAI::IsHeal(bot);
    std::vector<ObjectGuid::LowType> mates;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
        if (Player* p = ref.GetSource(); p && p->GetTeamId() == bot->GetTeamId() && PlayerbotAI::IsHeal(p) == heal)
            mates.push_back(p->GetGUID().GetCounter());
    std::sort(mates.begin(), mates.end());
    uint32 const idx = std::find(mates.begin(), mates.end(), bot->GetGUID().GetCounter()) - mates.begin();
    uint32 const side = heal ? (idx / 2) % 2 : idx % 2;  // 0: west (Green, Purple), 1: east (Blue, Red)
    bool const outRole = heal ? idx % 2 == 0 : idx % 3 != 2;

    // disarm a planted charge nearby first
    if (GameObject* charge = bot->FindNearestGameObject(GO_PLANTED_CHARGE, 30.0f, true))
    {
        out.move = charge->GetPosition();
        out.disarm = charge;
        out.urgent = true;
        DefenseStage(bot, 5);
        return true;
    }

    bool const green = Destroyed(bg, BG_SA_GREEN_GATE), blue = Destroyed(bg, BG_SA_BLUE_GATE);
    bool const purple = Destroyed(bg, BG_SA_PURPLE_GATE), red = Destroyed(bg, BG_SA_RED_GATE);
    if (Destroyed(bg, BG_SA_YELLOW_GATE))
    {
        // courtyard: everyone in front of the relic door, stopping bombers
        GameObject* door = bg->GetBGObject(BG_SA_ANCIENT_GATE);
        if (!door || Destroyed(bg, BG_SA_ANCIENT_GATE))
            return false;
        HoldAt(bot, bg, Near(door, 8.0f, bot), out);
        DefenseStage(bot, 4);
        return true;
    }
    if (purple || red)
    {
        // fall back to Yellow: a third just outside it, the rest inside
        GameObject* yellow = bg->GetBGObject(BG_SA_YELLOW_GATE);
        if (!yellow)
            return false;
        if (outRole && idx % 3 == 0)
        {
            DefenseStage(bot, GoOut(bot, bg, yellow, BG_SA_YELLOW_GATE, 15.0f, out) ? 1 : 0);
            return true;
        }
        HoldAt(bot, bg, Near(yellow, -8.0f, bot), out);
        DefenseStage(bot, 2);
        return true;
    }
    uint32 gateIdx = side ? BG_SA_BLUE_GATE : BG_SA_GREEN_GATE;
    bool goOut = outRole;
    float front = 30.0f;
    if (green || blue)
    {
        // an outer gate fell: two guard the graveyards behind it, the other side keeps its holders
        if (!heal && idx < 2)
        {
            uint32 const flag = green && blue ? (idx == 0 ? BG_SA_LEFT_FLAG : BG_SA_RIGHT_FLAG)
                                              : (green ? BG_SA_LEFT_FLAG : BG_SA_RIGHT_FLAG);
            if (GameObject* banner = bg->GetBGObject(flag))
            {
                HoldAt(bot, bg, banner->GetPosition(), out);
                DefenseStage(bot, 3);
                return true;
            }
        }
        bool const ownFell = side ? blue : green;
        if (ownFell || outRole)
        {
            // the breach: fight out front of the middle gate behind it
            bool const breachWest = green && (!blue || side == 0);
            gateIdx = breachWest ? BG_SA_PURPLE_GATE : BG_SA_RED_GATE;
            front = 20.0f;
        }
        else
            goOut = false;  // watch the other side from inside its gate
    }
    GameObject* gate = bg->GetBGObject(gateIdx);
    if (!gate)
        return false;
    if (goOut)
    {
        DefenseStage(bot, GoOut(bot, bg, gate, gateIdx, front, out) ? 1 : 0);
        return true;
    }
    HoldAt(bot, bg, Near(gate, -10.0f, bot), out);
    DefenseStage(bot, 2);
    return true;
}

bool DefenseMoving(Player* bot, Battleground* bg)
{
    Order o;
    return DefenseObjective(bot, bg, o) && o.urgent;
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
    if (!attacking)
        lane = DefenderLane(bot, bg);
    uint32 const target = NextGate(bg, lane);
    GameObject* go = bg->GetBGObject(target);
    if (!go)
        return false;

    if (!attacking)
    {
        if (target != BG_SA_TITAN_RELIC && DefenseObjective(bot, bg, out))
            return true;
        if (target == BG_SA_TITAN_RELIC)
        {
            out.move = Floor(go);
            return true;
        }
        if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACannons))
        {
            Unit* in = bot->GetVehicleBase();
            if (in && in->GetEntry() == NPC_ANTI_PERSONNAL_CANNON)
            {
                out.leave = FoesInRange(bg, in) == 0;  // nobody in its range: free the crew for another gun
                out.move = in->GetPosition();
                return true;
            }
            if (Creature* gun = GunCrew(bot, bg) ? BusiestGun(bg) : nullptr)
            {
                out.move = gun->GetPosition();
                out.board = gun;
                return true;
            }
        }
        out.move = Near(go, -10.0f, bot);
        {
            bool anyGate = false;
            for (uint32 g : {BG_SA_GREEN_GATE, BG_SA_YELLOW_GATE, BG_SA_BLUE_GATE, BG_SA_RED_GATE, BG_SA_PURPLE_GATE})
                if (GameObject* gg = bg->GetBGObject(g); gg && SiegeAtGate(bg, gg, 70.0f))
                    anyGate = true;
            SortieProbe(SortieRole(bot), SiegeAtGate(bg, go, 70.0f), anyGate, !DrivenSiege(bg).empty(),
                        BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SASortie));
        }
        // SASortie: while attackers mass outside the gate, go up the wall above it and jump down outside; outside, fight there
        if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SASortie) && SortieRole(bot) && target <= BG_SA_PURPLE_GATE &&
            Massing(bg, go) && !Destroyed(bg, target))
        {
            if (Outside(bot, go))
            {
                out.move = Near(go, 8.0f, bot);
                SortieStage(bot, 3);
            }
            else
            {
                float const* wall = SOTADefPortalDest[target];
                Position const top(wall[0], wall[1], wall[2]);
                float const dist = bot->GetExactDist2d(&top), dz = std::fabs(bot->GetPositionZ() - wall[2]);
                bool const atWall = dist < 10.0f && dz < 6.0f;
                SortieStage(bot, atWall ? 1 : 0, dist, dz);
                if (atWall)
                {
                    float const a = go->GetAngle(BEACH_X, BEACH_Y);
                    float const x = wall[0] + 14.0f * std::cos(a), y = wall[1] + 14.0f * std::sin(a);
                    float const z = go->GetMap()->GetHeight(go->GetPhaseMask(), x, y, wall[2]);
                    out.jump = Position(x, y, z > INVALID_HEIGHT ? z : go->GetPositionZ());
                    out.hasJump = true;
                }
                out.move = top;
                if (GameObject* portal = PortalToward(bot, bg, top))
                {
                    out.move = portal->GetPosition();
                    out.portal = portal;
                }
            }
            return true;
        }
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
        if (GameObject* pile = NearestBombPile(bot, bg, go, BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAChargesOnWay)))
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
    if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADefense) && Attackers(bg) != TEAM_NEUTRAL &&
        bot->GetTeamId() != Attackers(bg))
    {
        Unit* best = nullptr;
        float bestDist = 45.0f;
        for (Unit* d : DrivenSiege(bg))
            if (float const dist = bot->GetExactDist2d(d); dist < bestDist && bot->IsValidAttackTarget(d) &&
                                                           bot->IsWithinLOSInMap(d))
            {
                bestDist = dist;
                best = d;
            }
        if (best)
            return best;
    }
    bool const bombs = BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SABombHunt);
    bool const sortie = BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SASortie) && SortieRole(bot);
    if (!bombs && !sortie && !BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAHunt))
        return nullptr;
    TeamId const attackers = Attackers(bg);
    if (attackers == TEAM_NEUTRAL || bot->GetTeamId() == attackers)
        return nullptr;
    GameObject* gate = bg->GetBGObject(NextGate(bg, DefenderLane(bot, bg)));
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
    if (!BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAHunt) && !(sortie && Outside(bot, gate)))
        return nullptr;
    Unit* best = nullptr;
    float bestDist = 60.0f;
    for (Unit* d : DrivenSiege(bg))
    {
        if (d->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE) || d->GetExactDist2d(gate) > 50.0f ||
            !bot->IsValidAttackTarget(d) || !bot->IsWithinLOSInMap(d))
            continue;
        if (float const dist = bot->GetExactDist2d(d); dist < bestDist)
        {
            bestDist = dist;
            best = d;
        }
    }
    return best;
}

namespace
{
struct Carry
{
    uint32 ms;
    TeamId attackers;
    Position last;  // a carrier that died reappears at a graveyard
};
std::mutex carryLock;
std::unordered_map<ObjectGuid::LowType, Carry> carries;
uint32 carryOut[5] = {}, carryPlantMs = 0, carryLogMs = 0;  // picked up, planted, died carrying, round ended, lost otherwise
}

void CarryTrack(Player* bot, Battleground* bg)
{
    uint32 const now = getMSTime();
    bool const has = bot->HasItemCount(ITEM_MASSIVE_SEAFORIUM_CHARGE, 1);
    std::lock_guard<std::mutex> guard(carryLock);
    auto it = carries.find(bot->GetGUID().GetCounter());
    if (it == carries.end())
    {
        if (has)
        {
            carries[bot->GetGUID().GetCounter()] = {now, Attackers(bg), bot->GetPosition()};
            ++carryOut[0];
        }
    }
    else if (it->second.attackers != Attackers(bg))
    {
        ++carryOut[3];
        carries.erase(it);
    }
    else if (!has)
    {
        ++carryOut[bot->GetExactDist2d(&it->second.last) > 40.0f ? 2 : 4];
        carries.erase(it);
    }
    else
        it->second.last = bot->GetPosition();
    if (!carryLogMs)
        carryLogMs = now;
    else if (getMSTimeDiff(carryLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        LOG_INFO("module", "Charge carries (5 min): picked up {}, planted {} (avg {} s after pickup), died carrying {}, round ended carrying {}, lost otherwise {}, carrying now {}",
                 carryOut[0], carryOut[1], carryOut[1] ? carryPlantMs / carryOut[1] / 1000 : 0, carryOut[2], carryOut[3], carryOut[4],
                 carries.size());
        std::fill(std::begin(carryOut), std::end(carryOut), 0);
        carryPlantMs = 0;
        carryLogMs = now;
        for (auto c = carries.begin(); c != carries.end();)  // games that ended
            c = getMSTimeDiff(c->second.ms, now) > 20 * MINUTE * IN_MILLISECONDS ? carries.erase(c) : std::next(c);
    }
}

bool PushingCharge(Player* bot, Battleground* bg)
{
    return BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAChargesPush) && bot->GetTeamId() == Attackers(bg) &&
           !bot->GetVehicle() && bot->HasItemCount(ITEM_MASSIVE_SEAFORIUM_CHARGE, 1);
}

void SortieJumped(Player* bot) { SortieStage(bot, 2); }

bool SortieMoving(Player* bot, Battleground* bg)
{
    TeamId const attackers = Attackers(bg);
    if (!BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SASortie) || !SortieRole(bot) || attackers == TEAM_NEUTRAL ||
        bot->GetTeamId() == attackers || bot->GetVehicle())
        return false;
    uint32 const target = NextGate(bg, DefenderLane(bot, bg));
    GameObject* go = target <= BG_SA_PURPLE_GATE ? bg->GetBGObject(target) : nullptr;
    return go && Massing(bg, go) && !Destroyed(bg, target) && !Outside(bot, go);
}

void CarryPlanted(Player* bot)
{
    std::lock_guard<std::mutex> guard(carryLock);
    auto it = carries.find(bot->GetGUID().GetCounter());
    if (it == carries.end())
        return;
    ++carryOut[1];
    carryPlantMs += getMSTimeDiff(it->second.ms, getMSTime());
    carries.erase(it);
}

Unit* GunnerTarget(Player* bot, Battleground* bg)
{
    Unit* gun = bot->GetVehicleBase();
    if (!gun || gun->GetEntry() != NPC_ANTI_PERSONNAL_CANNON)
        return nullptr;
    // Rocket Blast hits every unit within 8 yd for 4000 (10-70 yd, not buildings; demolishers have ~15x health): the enemy
    // on foot with the most enemies around, a charge carrier counting 3
    std::vector<Player*> foes;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
        if (Player* p = ref.GetSource(); p && p->IsAlive() && p->GetTeamId() != bot->GetTeamId() && !p->GetVehicle())
            foes.push_back(p);
    Unit* best = nullptr;
    uint32 bestScore = 0, in70 = 0, inRange = 0, shootable = 0;
    for (Player* p : foes)
    {
        float const dist = gun->GetExactDist2d(p);
        in70 += dist <= 70.0f ? 1 : 0;
        inRange += dist >= 10.0f && dist <= 70.0f ? 1 : 0;
        // line of sight from the gunner's height (the cannon's base sits behind its own wall)
        if (dist < 10.0f || dist > 70.0f || !bot->CanSeeOrDetect(p) ||
            (!gun->IsWithinLOSInMap(p) &&
             !gun->GetMap()->isInLineOfSight(gun->GetPositionX(), gun->GetPositionY(), gun->GetPositionZ() + 4.0f,
                                             p->GetPositionX(), p->GetPositionY(), p->GetPositionZ() + 2.0f, gun->GetPhaseMask(),
                                             LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing)))
            continue;
        ++shootable;
        uint32 score = 0;
        for (Player* q : foes)
            if (q->GetExactDist2d(p) < 8.0f)
                score += q->HasItemCount(ITEM_MASSIVE_SEAFORIUM_CHARGE, 1) ? 3 : 1;
        if (score > bestScore)
        {
            bestScore = score;
            best = p;
        }
    }
    GunnerSight(foes.size(), in70, inRange, shootable);
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
