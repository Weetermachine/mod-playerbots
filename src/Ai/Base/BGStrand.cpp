/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGStrand.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "AiObjectContext.h"
#include "BGTacticArms.h"
#include "Config.h"
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
    auto it = focusLanes.find(bg->GetInstanceID());
    if (it == focusLanes.end())
        it = focusLanes
                 .emplace(bg->GetInstanceID(),
                          std::make_pair(BGTacticArms::IsOn(bg, attackers, BGTactic::SAFocusFix) ? TEAM_NEUTRAL : TEAM_ALLIANCE, 0u))
                 .first;
    auto& f = it->second;
    if (f.first != attackers)
        f = {attackers, urand(0, 1)};
    return f.second;
}

// Drivers seen in a siege vehicle, by guid: when (the dismounted-driver kill counter)
std::mutex driverLock;
std::unordered_map<ObjectGuid::LowType, uint32> lastDrove;

// The siege vehicles attackers drive (workshop demolishers are not in the BG's demolisher slots).
std::vector<Unit*> DrivenSiege(Battleground* bg)
{
    std::vector<Unit*> out;
    TeamId const attackers = BGStrand::Attackers(bg);
    uint32 const now = getMSTime();
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
        if (Player* p = ref.GetSource(); p && p->IsAlive() && p->GetTeamId() == attackers)
            if (Unit* v = p->GetVehicleBase(); v && v->IsAlive() && v->GetEntry() != NPC_ANTI_PERSONNAL_CANNON)
            {
                {
                    std::lock_guard<std::mutex> guard(driverLock);
                    lastDrove[p->GetGUID().GetCounter()] = now;
                }
                if (std::find(out.begin(), out.end(), v) == out.end())
                    out.push_back(v);
            }
    return out;
}

// An attacker that was in a siege vehicle within the last 5 s and is on foot now (its demolisher just died)
bool JustDismounted(Player* p)
{
    std::lock_guard<std::mutex> guard(driverLock);
    auto const it = lastDrove.find(p->GetGUID().GetCounter());
    return it != lastDrove.end() && !p->GetVehicle() && getMSTimeDiff(it->second, getMSTime()) < 5000;
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

// SACannons: a gun's draw: attackers on foot in its range (10-70 yd); SACannonsFollow: a driven demolisher in range counts
// 3, one closing in (70-100 yd) 1
uint32 GunDraw(Battleground* bg, Unit* gun, bool follow)
{
    uint32 n = FoesInRange(bg, gun);
    if (follow)
        for (Unit* v : DrivenSiege(bg))
        {
            float const d = gun->GetExactDist2d(v);
            n += d >= 10.0f && d <= 70.0f ? 3 : d > 70.0f && d <= 100.0f ? 1 : 0;
        }
    return n;
}

// SACannons: the free cannon with the most draw (2+), if any.
Creature* BusiestGun(Battleground* bg, bool follow)
{
    Creature* best = nullptr;
    uint32 bestN = 1;
    for (uint32 i = BG_SA_GUN_1; i <= BG_SA_GUN_10; ++i)
        if (Creature* gun = bg->GetBGCreature(i); gun && gun->IsAlive() && gun->GetVehicleKit() &&
                                                  !gun->GetVehicleKit()->IsVehicleInUse())
            if (uint32 const n = GunDraw(bg, gun, follow); n > bestN)
            {
                bestN = n;
                best = gun;
            }
    return best;
}

// SACannons: a quarter of the non-healer defenders (not sortie bots) crew the cannons.
bool GunCrew(Player* bot, Battleground* bg)
{
    return !PlayerbotAI::IsHeal(bot) && bot->GetGUID().GetCounter() % 4 == 1 &&
           !(BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SASortie) && SortieRole(bot));
}

// SACannonCrewPlus: a second quarter of the non-healers (not sortie bots) crews while a free cannon has a driven
// demolisher in its range (10-70 yd)
bool ExtraCrew(Player* bot, Battleground* bg)
{
    if (!BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACannonCrewPlus) || PlayerbotAI::IsHeal(bot) ||
        bot->GetGUID().GetCounter() % 4 != 3 || (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SASortie) && SortieRole(bot)))
        return false;
    std::vector<Unit*> const siege = DrivenSiege(bg);
    for (uint32 i = BG_SA_GUN_1; i <= BG_SA_GUN_10; ++i)
        if (Creature* gun = bg->GetBGCreature(i); gun && gun->IsAlive() && gun->GetVehicleKit() &&
                                                  !gun->GetVehicleKit()->IsVehicleInUse())
            for (Unit* v : siege)
                if (float const d = gun->GetExactDist2d(v); d >= 10.0f && d <= 70.0f)
                    return true;
    return false;
}

constexpr uint32 ITEM_MASSIVE_SEAFORIUM_CHARGE = 39213;
constexpr uint32 GO_PLANTED_CHARGE = 190752;

// SACharges: the nearest bomb pile (spawned) within 150 yd; SAChargesOnWay: only one that adds at most 30 yd to the way to the gate.
GameObject* NearestBombPile(Player* bot, Battleground* bg, GameObject* gate, bool onWay, float maxDist = 150.0f)
{
    GameObject* best = nullptr;
    float bestDist = maxDist;
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
GameObject* PortalToward(Player* bot, Battleground* bg, Position const& dest, float* saving = nullptr)
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
    if (best && saving)
        *saving = bestSaving;
    return best;
}

// SAWallPortals: the Defender's Portal that lands on this gate's wall spot (SOTADefPortalDest[gateIdx]); the portals
// only hop about 30 yd, from just inside a gate up to its own wall
GameObject* WallPortal(Battleground* bg, uint32 gateIdx)
{
    static uint32 const portalOf[5] = {1, 2, 0, 4, 3};  // dest (gate order) -> portal (blue, green, yellow, purple, red)
    return gateIdx < 5 ? bg->GetBGObject(BG_SA_PORTAL_DEFFENDER_BLUE + portalOf[gateIdx]) : nullptr;
}

// SAWallPortals: a defender off its gate's wall spot and within 80 yd of the gate's portal takes it up there
bool WallPortalUp(Player* bot, Battleground* bg, uint32 gateIdx, BGStrand::Order& out)
{
    if (!BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAWallPortals) || gateIdx >= 5)
        return false;
    float const* wall = SOTADefPortalDest[gateIdx];
    Position const top(wall[0], wall[1], wall[2]);
    GameObject* portal = WallPortal(bg, gateIdx);
    if (!portal || (bot->GetExactDist2d(&top) < 10.0f && std::fabs(bot->GetPositionZ() - wall[2]) < 6.0f) ||
        bot->GetExactDist2d(portal) > 80.0f)
        return false;
    out.move = portal->GetPosition();
    out.portal = portal;
    out.portalSaving = 0.0f;  // it saves the stairs, not straight-line distance
    BGStrand::Stat("wall_portal_chosen", bot);
    return true;
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
    if (bg->GetMapId() != 607)  // BG_SA_TITAN_RELIC is a SotA slot: other maps' object vectors differ or are shorter
        return TEAM_NEUTRAL;
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
// SADefense diagnostics (Server.log, per 5 min): distinct defenders per role and outside their gate, disarms
std::mutex defLock;
std::unordered_set<ObjectGuid::LowType> defSeen[6];  // out, outside, hold, graveyard, relic door, disarming
uint32 defDisarms = 0, defDefused = 0, defLogMs = 0;

void DefenseStage(Player* bot, uint32 role)
{
    std::lock_guard<std::mutex> guard(defLock);
    defSeen[role].insert(bot->GetGUID().GetCounter());
    uint32 const now = getMSTime();
    if (!defLogMs)
        defLogMs = now;
    else if (getMSTimeDiff(defLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        LOG_INFO("module", "Defense roles (5 min, distinct bots): out {} (outside {}), hold {}, graveyard {}, relic door {}, going to a charge {}, disarms {} (defused {})",
                 defSeen[0].size(), defSeen[1].size(), defSeen[2].size(), defSeen[3].size(), defSeen[4].size(),
                 defSeen[5].size(), defDisarms, defDefused);
        for (auto& d : defSeen)
            d.clear();
        defDisarms = defDefused = 0;
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
    if (WallPortalUp(bot, bg, gateIdx, out))
        return false;
    if (GameObject* portal = PortalToward(bot, bg, top, &out.portalSaving))
    {
        BGStrand::Stat("portal_chosen", bot);
        out.move = portal->GetPosition();
        out.portal = portal;
    }
    return false;
}

void HoldAt(Player* bot, Battleground* bg, Position const& pos, Order& out);

// SAWallRanged: every ranged dps defender of this gate (holders and out front) takes the wall above it while a driven
// demolisher is within 70 yd of it
bool WallRanged(Player* bot, Battleground* bg, GameObject* gate, uint32 gateIdx, BGStrand::Order& out)
{
    if (!BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAWallRanged) || PlayerbotAI::IsHeal(bot) ||
        !PlayerbotAI::IsRanged(bot) || gateIdx >= 5 || !SiegeAtGate(bg, gate, 70.0f))
        return false;
    float const* wall = SOTADefPortalDest[gateIdx];
    uint32 const n = bot->GetGUID().GetCounter();
    HoldAt(bot, bg, Position(wall[0] + float(n % 9) - 4.0f, wall[1] + float(n * 7 % 5) - 2.0f, wall[2]), out);
    WallPortalUp(bot, bg, gateIdx, out);
    BGStrand::Stat("wall_ranged", bot);
    return true;
}

// SAAllHands, also part of the defender package (SADefPack)
bool AllHands(Battleground* bg, Player* bot)
{
    return BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAAllHands) ||
           BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADefPack);
}

// SAPreDamage: the idle demolisher above 10% nearest to a post, within radius of it
Creature* IdleDemolisherNear(Player* bot, WorldObject const* post, float radius)
{
    std::list<Creature*> demos;
    bot->GetCreatureListWithEntryInGrid(demos, NPC_DEMOLISHER_SA, radius + 150.0f);
    bot->GetCreatureListWithEntryInGrid(demos, 32796, radius + 150.0f);
    Creature* nearest = nullptr;
    for (Creature* d : demos)
        if (d->IsAlive() && d->GetHealthPct() > 10.0f && d->GetVehicleKit() && !d->GetVehicleKit()->IsVehicleInUse() &&
            d->GetExactDist2d(post) < radius && (!nearest || post->GetExactDist2d(d) < post->GetExactDist2d(nearest)))
            nearest = d;
    return nearest;
}

// SAWorkshopGuard / SAWorkshopKill: the workshop banner this defender holds: the first 4 non-healers (by guid), two
// per workshop the attackers hold (its demolishers have spawned), for the rest of the round; nullptr otherwise
GameObject* GuardedWorkshop(Player* bot, Battleground* bg)
{
    TeamId const team = bot->GetTeamId();
    if (!(BGTacticArms::IsOn(bg, team, BGTactic::SAWorkshopGuard) || BGTacticArms::IsOn(bg, team, BGTactic::SAWorkshopKill)) ||
        Attackers(bg) == TEAM_NEUTRAL || team == Attackers(bg) || !bot->IsAlive() || PlayerbotAI::IsHeal(bot))
        return nullptr;
    auto spawned = [&](uint32 a, uint32 b)
    { return bg->GetBgMap()->GetCreature(bg->BgCreatures[a]) || bg->GetBgMap()->GetCreature(bg->BgCreatures[b]); };
    bool const left = spawned(BG_SA_DEMOLISHER_7, BG_SA_DEMOLISHER_8), right = spawned(BG_SA_DEMOLISHER_5, BG_SA_DEMOLISHER_6);
    if (!left && !right)
        return nullptr;
    std::vector<ObjectGuid::LowType> mates;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
        if (Player* p = ref.GetSource(); p && p->GetTeamId() == team && !PlayerbotAI::IsHeal(p))
            mates.push_back(p->GetGUID().GetCounter());
    std::sort(mates.begin(), mates.end());
    uint32 const idx = std::find(mates.begin(), mates.end(), bot->GetGUID().GetCounter()) - mates.begin();
    if (idx >= 4)
        return nullptr;
    return bg->GetBGObject(left && (!right || idx % 2 == 0) ? BG_SA_LEFT_FLAG : BG_SA_RIGHT_FLAG);
}

// SAAllOut: a healer holds the wall above its gate (the Defender's Portal spot) and heals the fight out front from there
void WallHeal(Player* bot, Battleground* bg, uint32 gateIdx, Order& out)
{
    float const* wall = SOTADefPortalDest[gateIdx];
    HoldAt(bot, bg, Position(wall[0] + float(bot->GetGUID().GetCounter() % 7) - 3.0f, wall[1], wall[2]), out);
    WallPortalUp(bot, bg, gateIdx, out);
    BGStrand::Stat("wall_healer", bot);
}

void HoldAt(Player* bot, Battleground* bg, Position const& pos, Order& out)
{
    out.move = pos;
    if (GameObject* portal = PortalToward(bot, bg, pos, &out.portalSaving))  // fall back through the portals
    {
        BGStrand::Stat("portal_chosen", bot);
        out.move = portal->GetPosition();
        out.portal = portal;
    }
}
}  // namespace

void Disarmed(bool defused)
{
    std::lock_guard<std::mutex> guard(defLock);
    ++defDisarms;
    defDefused += defused ? 1 : 0;
}

// SABeachAmbush: per defender, the round (instance and attacking team) whose ambush it has finished; per round, its start
std::mutex ambushLock;
std::unordered_map<ObjectGuid::LowType, uint64> ambushOver;
std::unordered_map<uint64, uint32> ambushStart;

uint64 AmbushRound(Battleground* bg) { return (uint64(bg->GetInstanceID()) << 2) | uint64(Attackers(bg)); }

void ForgetAmbush(uint32 instanceId)
{
    std::lock_guard<std::mutex> guard(ambushLock);
    for (uint64 team = 0; team < 3; ++team)
        ambushStart.erase((uint64(instanceId) << 2) | team);
}

void AmbushDied(Player* victim)
{
    Battleground* bg = victim->GetBattleground();
    if (!bg || Attackers(bg) == TEAM_NEUTRAL || victim->GetTeamId() == Attackers(bg) ||
        !BGTacticArms::IsOn(bg, victim->GetTeamId(), BGTactic::SABeachAmbush))
        return;
    {
        std::lock_guard<std::mutex> guard(ambushLock);
        auto& over = ambushOver[victim->GetGUID().GetCounter()];
        if (over == AmbushRound(bg))
            return;
        over = AmbushRound(bg);
    }
    Stat("ambush_over_death", victim);
}

// SABeachAmbush: in the warmup a defender (not cannon crew) waits a little inland of the beach demolishers on its side (by
// guid) and fights the landing there, until its first death in the round or 75 s into it; then the usual positions
bool BeachAmbush(Player* bot, Battleground* bg, Order& out)
{
    if (!BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SABeachAmbush) || Attackers(bg) == TEAM_NEUTRAL ||
        bot->GetTeamId() == Attackers(bg) || bot->GetVehicle() || !bot->IsAlive() || GunCrew(bot, bg))
        return false;
    uint64 const round = AmbushRound(bg);
    bool timeUp = false;
    {
        std::lock_guard<std::mutex> guard(ambushLock);
        auto it = ambushOver.find(bot->GetGUID().GetCounter());
        if (it != ambushOver.end() && it->second == round)
            return false;
        if (bg->GetStatus() == STATUS_IN_PROGRESS)
        {
            uint32& start = ambushStart[round];
            if (!start)
                start = getMSTime();
            if (getMSTimeDiff(start, getMSTime()) > 75000)
            {
                ambushOver[bot->GetGUID().GetCounter()] = round;
                timeUp = true;
            }
        }
    }
    if (timeUp)
    {
        Stat("ambush_over_time", bot);
        return false;
    }
    uint32 const n = bot->GetGUID().GetCounter();
    bool const west = n % 2;
    float const* a = BG_SA_NpcSpawnlocs[west ? BG_SA_DEMOLISHER_3 : BG_SA_DEMOLISHER_1];
    float const* b = BG_SA_NpcSpawnlocs[west ? BG_SA_DEMOLISHER_4 : BG_SA_DEMOLISHER_2];
    float const x = (a[0] + b[0]) / 2.0f - 15.0f + float(n * 37 % 9) - 4.0f;
    float const y = (a[1] + b[1]) / 2.0f + float(n * 53 % 9) - 4.0f;
    float const z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, a[2] + 10.0f);
    out.move = Position(x, y, z > INVALID_HEIGHT ? z : a[2]);
    bool const fighting = bg->GetStatus() == STATUS_IN_PROGRESS;
    out.urgent = fighting && bot->GetExactDist2d(&out.move) > 30.0f;
    Stat(fighting ? "ambush_fight" : "ambush_wait", bot);
    return true;
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
    bool const allOut = BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAAllOut);
    bool const outRole = allOut ? !heal : heal ? idx % 2 == 0 : idx % 3 != 2;

    // SACannons: the cannon crew boards the busiest free cannon before anything else
    bool const extraCrew = !GunCrew(bot, bg) && ExtraCrew(bot, bg);
    bool const relicPhase = Destroyed(bg, BG_SA_ANCIENT_GATE) && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SARelicCrew);
    if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACannons) && (GunCrew(bot, bg) || extraCrew) && !relicPhase)
        if (Creature* gun = BusiestGun(bg, BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACannonsFollow)))
        {
            out.move = gun->GetPosition();
            out.board = gun;
            Stat(extraCrew ? "cannon_board_extra" : "cannon_board", bot);
            return true;
        }

    if (BeachAmbush(bot, bg, out))
        return true;

    // disarm a planted charge nearby first
    if (GameObject* charge = bot->FindNearestGameObject(GO_PLANTED_CHARGE, 30.0f, true))
    {
        out.move = charge->GetPosition();
        out.disarm = charge;
        out.urgent = true;
        DefenseStage(bot, 5);
        return true;
    }

    // SADefPack: the relic door is down: everyone to the relic (they had stayed ~110 yd back at their fall-back spots)
    if (Destroyed(bg, BG_SA_ANCIENT_GATE) && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADefPack))
        if (GameObject* relic = bg->GetBGObject(BG_SA_TITAN_RELIC))
        {
            Position const floor = Floor(relic);
            HoldAt(bot, bg, Position(floor.GetPositionX() + float(bot->GetGUID().GetCounter() % 9) - 4.0f,
                                     floor.GetPositionY() + float(bot->GetGUID().GetCounter() * 7 % 9) - 4.0f,
                                     floor.GetPositionZ()), out);
            out.urgent = bot->GetExactDist2d(relic) > 30.0f;  // at the relic: fight (the relic hunt picks targets)
            Stat(out.urgent ? "relic_phase_on_way" : "relic_phase_at_relic", bot);
            return true;
        }

    // SAWorkshopGuard / SAWorkshopKill: hold the attackers' workshop for the rest of the round (above combat while far)
    if (GameObject* ws = GuardedWorkshop(bot, bg))
    {
        HoldAt(bot, bg, ws->GetPosition(), out);
        out.urgent = bot->GetExactDist2d(ws) > 60.0f;
        Stat(out.urgent ? "ws_guard_on_way" : "ws_guard", bot);
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
        // SADefPack: emergency fallback to the door above any fight on the way, except to hit a demolisher in reach
        if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADefPack))
        {
            bool demo = false;
            for (Unit* v : DrivenSiege(bg))
                demo = demo || (bot->GetExactDist2d(v) < 40.0f && bot->IsWithinLOSInMap(v));
            out.urgent = !demo && bot->GetExactDist2d(door) > 30.0f;
            Stat(demo ? "fallback_intercept" : out.urgent ? "fallback_on_way" : "fallback_at_door", bot);
        }
        DefenseStage(bot, 4);
        return true;
    }
    if (purple || red)
    {
        // fall back to Yellow: a third just outside it, the rest inside
        GameObject* yellow = bg->GetBGObject(BG_SA_YELLOW_GATE);
        if (!yellow)
            return false;
        bool const allHands = AllHands(bg, bot) && SiegeAtGate(bg, yellow, 80.0f);
        if (allOut && heal)
        {
            WallHeal(bot, bg, BG_SA_YELLOW_GATE, out);
            return true;
        }
        if (WallRanged(bot, bg, yellow, BG_SA_YELLOW_GATE, out))
        {
            DefenseStage(bot, 2);
            return true;
        }
        if ((outRole && (allOut || idx % 3 == 0)) || allHands)
        {
            if (allHands && !(outRole && idx % 3 == 0))
                BGStrand::Stat("allhands_out", bot);
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
        uint32 const guards = BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADefPack) ? 4 : 2;
        if (!heal && idx < guards)
        {
            uint32 const flag = green && blue ? (idx % 2 == 0 ? BG_SA_LEFT_FLAG : BG_SA_RIGHT_FLAG)
                                              : (green ? BG_SA_LEFT_FLAG : BG_SA_RIGHT_FLAG);
            if (GameObject* banner = bg->GetBGObject(flag))
            {
                HoldAt(bot, bg, banner->GetPosition(), out);
                BGStrand::Stat("gy_guard", bot);
                // SAPreDamage: the workshop by the graveyard spawns demolishers; wear idle ones down
                if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAPreDamage))
                    if (Creature* idle = IdleDemolisherNear(bot, banner, 100.0f))
                    {
                        out.move = idle->GetPosition();
                        BGStrand::Stat("predamage_move", bot);
                    }
                // SAAllHands: a driven demolisher passing the graveyard
                if (AllHands(bg, bot))
                    for (Unit* d : DrivenSiege(bg))
                        if (d->GetExactDist2d(banner) < 60.0f)
                        {
                            out.move = d->GetPosition();
                            BGStrand::Stat("gy_guard_engage", bot);
                            break;
                        }
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
    if (WallRanged(bot, bg, gate, gateIdx, out))
    {
        DefenseStage(bot, 2);
        return true;
    }
    // SAAllHands: a driven demolisher within 80 yd of this gate: everyone of this gate goes out and fights it
    if (!goOut && AllHands(bg, bot) && SiegeAtGate(bg, gate, 80.0f))
    {
        goOut = true;
        BGStrand::Stat("allhands_out", bot);
    }
    if (goOut)
    {
        bool const outside = GoOut(bot, bg, gate, gateIdx, front, out);
        // SAPreDamage: once an outer gate fell, go on to the nearest idle workshop demolisher above 10% (the beach ones
        // are boarded within seconds of unlocking)
        if (outside && (green || blue) && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAPreDamage))
            if (Creature* idle = IdleDemolisherNear(bot, gate, 100.0f))
            {
                out.move = idle->GetPosition();
                BGStrand::Stat("predamage_move", bot);
            }
        DefenseStage(bot, outside ? 1 : 0);
        return true;
    }
    if (allOut && heal)
    {
        WallHeal(bot, bg, gateIdx, out);
        return true;
    }
    HoldAt(bot, bg, Near(gate, -10.0f, bot), out);
    DefenseStage(bot, 2);
    return true;
}

Unit* SlowTarget(Player* bot, Battleground* bg)
{
    TeamId const attackers = Attackers(bg);
    if (attackers == TEAM_NEUTRAL || bot->GetTeamId() == attackers)
        return nullptr;
    Unit* best = nullptr;
    float bestDist = 30.0f;
    for (Unit* d : DrivenSiege(bg))
    {
        if (d->HasAuraType(SPELL_AURA_MOD_ROOT) || d->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
            !bot->IsWithinLOSInMap(d) || !bot->IsValidAttackTarget(d))
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
std::mutex warmLock;
uint32 warmSeen[2][9] = {}, warmLogMs = 0;  // [triggered, ran][20 s step of battleground time]
}

void WarmupProbe(Battleground* bg, uint32 kind)
{
    uint32 const step = std::min<uint32>(bg->GetStartTime() / 20000, 8);
    std::lock_guard<std::mutex> guard(warmLock);
    ++warmSeen[kind][step];
    uint32 const now = getMSTime();
    if (!warmLogMs)
        warmLogMs = now;
    else if (getMSTimeDiff(warmLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        std::ostringstream o;
        for (uint32 k = 0; k < 2; ++k)
        {
            o << (k ? " | moves run:" : " move triggers:");
            for (uint32 i = 0; i < 9; ++i)
                o << ' ' << warmSeen[k][i];
        }
        LOG_INFO("module", "Warmup probe (5 min, by 20 s of battleground time):{}", o.str());
        std::memset(warmSeen, 0, sizeof(warmSeen));
        warmLogMs = now;
    }
}

namespace
{
char const* const exitNames[] = {"no objective", "used portal or pile", "jumped", "already moving", "within 4 yd",
                                 "in combat", "route moved", "route idle", "MoveNear ok", "MoveNear failed", "other"};
uint32 exits[11][2] = {}, exitLogMs = 0;
}

void WarmupExit(Battleground* bg, uint32 exit)
{
    uint32 const minute = bg->GetStartTime() < 60000 ? 0 : 1;
    std::lock_guard<std::mutex> guard(warmLock);
    ++exits[std::min<uint32>(exit, 10)][minute];
    uint32 const now = getMSTime();
    if (!exitLogMs)
        exitLogMs = now;
    else if (getMSTimeDiff(exitLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        std::ostringstream o;
        for (uint32 m = 0; m < 2; ++m)
        {
            o << (m ? " | 60-120 s:" : " 0-60 s:");
            for (uint32 i = 0; i < 11; ++i)
                if (exits[i][m])
                    o << ' ' << exitNames[i] << ' ' << exits[i][m] << ',';
        }
        LOG_INFO("module", "Warmup exits (5 min):{}", o.str());
        std::memset(exits, 0, sizeof(exits));
        exitLogMs = now;
    }
}

namespace
{
// SAPassengers: this attacker rides as a passenger this round: ranged or healer, half of them (by guid and round)
bool PassengerRole(Player* bot, Battleground* bg)
{
    if (!BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAPassengers) || bot->GetTeamId() != BGStrand::Attackers(bg))
        return false;
    if (!PlayerbotAI::IsHeal(bot) && !PlayerbotAI::IsRanged(bot))
        return false;
    uint32 const round = bg->GetInstanceID() * 2 + (BGStrand::Attackers(bg) == TEAM_HORDE ? 1 : 0);
    return (bot->GetGUID().GetCounter() * 2654435761u + round * 40503u) % 2 == 0;
}
}

Unit* PassengerSeat(Player* bot, Battleground* bg)
{
    if (bot->GetVehicle() || !PassengerRole(bot, bg))
        return nullptr;
    Unit* best = nullptr;
    for (Unit* d : DrivenSiege(bg))
        if (d->GetVehicleKit() && d->GetVehicleKit()->GetAvailableSeatCount() > 0 && d->GetExactDist2d(bot) < 30.0f &&
            (!best || d->GetExactDist2d(bot) < best->GetExactDist2d(bot)))
            best = d;
    return best;
}

bool LeaveToMelee(Player* bot, Battleground* bg, Unit* vehicle)
{
    if (!BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAPassengers) ||
        (!PlayerbotAI::IsHeal(bot) && !PlayerbotAI::IsRanged(bot)))
        return false;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
        if (Player* p = ref.GetSource(); p && p != bot && p->IsAlive() && p->GetTeamId() == bot->GetTeamId() &&
                                         !p->GetVehicle() && !PlayerbotAI::IsHeal(p) && !PlayerbotAI::IsRanged(p) &&
                                         p->GetExactDist2d(vehicle) < 40.0f)
            return true;
    return false;
}

namespace
{
std::mutex statLock;
std::map<std::string, uint32> statCount;
std::unordered_map<std::string, uint32> statLast;  // name + guid -> last count time
uint32 statLogMs = 0;

void StatAdd(std::string const& name, std::string const& key)
{
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(statLock);
    if (!key.empty())
    {
        auto it = statLast.find(key);
        if (it != statLast.end() && getMSTimeDiff(it->second, now) < 10000)
            return;
        statLast[key] = now;
    }
    ++statCount[name];
    if (!statLogMs)
        statLogMs = now;
    else if (getMSTimeDiff(statLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        std::ostringstream o;
        for (auto const& [n, c] : statCount)
            o << ' ' << n << '=' << c;
        LOG_INFO("module", "SA tactic counters (5 min):{}", o.str());
        statCount.clear();
        if (statLast.size() > 20000)
            statLast.clear();
        statLogMs = now;
    }
}
}

void Stat(char const* name, Player* bot)
{
    StatAdd(name, std::string(name) + std::to_string(bot->GetGUID().GetCounter()));
}

void KillStat(Player* victim)
{
    AmbushDied(victim);
    Battleground* bg = victim->GetBattleground();
    if (!bg || victim->GetMapId() != 607 || Attackers(bg) == TEAM_NEUTRAL || victim->GetTeamId() != Attackers(bg))
        return;
    TeamId const defenders = Attackers(bg) == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    std::string const tag = BGTacticArms::IsOn(bg, defenders, BGTactic::SADefPack) ? "" : "_ctrl";
    if (JustDismounted(victim))
    {
        StatAdd("driver_kill" + tag, "");
        return;
    }
    if (Destroyed(bg, BG_SA_ANCIENT_GATE))
        if (GameObject* relic = bg->GetBGObject(BG_SA_TITAN_RELIC); relic && victim->GetExactDist2d(relic) < 25.0f)
            StatAdd("relic_kill" + tag, "");
}

void SiegeKill(Unit* vehicle)
{
    Map* map = vehicle->GetMap();
    Battleground* bg = map && map->IsBattleground() ? ((BattlegroundMap*)map)->GetBG() : nullptr;
    if (!bg || Attackers(bg) == TEAM_NEUTRAL)
        return;
    TeamId const defenders = Attackers(bg) == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    std::string tag = BGTacticArms::IsOn(bg, defenders, BGTactic::SADefPack) ? "" : "_ctrl";
    if (BGTacticArms::IsOn(bg, Attackers(bg), BGTactic::SAEscort))
        tag += "_esc";
    bool const courtyard = Destroyed(bg, BG_SA_YELLOW_GATE) && !Destroyed(bg, BG_SA_ANCIENT_GATE);
    StatAdd(std::string(courtyard ? "demo_kill_courtyard" : "demo_kill") + tag, "");
}

// Passenger casting diagnostics: a bot in a SotA demolisher's passenger seat
namespace
{
std::mutex paxLock;
std::map<std::string, uint32> paxCounts;
std::unordered_map<ObjectGuid::LowType, uint32> paxSampled;
uint32 paxLogMs = 0;

bool IsPassenger(Player* bot)
{
    if (!bot || bot->GetMapId() != 607)
        return false;
    Vehicle* v = bot->GetVehicle();
    if (!v || !v->GetBase() || (v->GetBase()->GetEntry() != NPC_DEMOLISHER_SA && v->GetBase()->GetEntry() != 32796))
        return false;
    VehicleSeatEntry const* seat = v->GetSeatForPassenger(bot);
    return seat && !seat->CanControl();
}
}  // namespace

void PaxCast(Player* bot, std::string const& what)
{
    if (!IsPassenger(bot))
        return;
    std::lock_guard<std::mutex> guard(paxLock);
    ++paxCounts[what];
    uint32 const now = getMSTime();
    if (!paxLogMs)
        paxLogMs = now;
    else if (getMSTimeDiff(paxLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        std::ostringstream o;
        for (auto const& [k, n] : paxCounts)
            o << ' ' << k << '=' << n;
        LOG_INFO("module", "SA passenger casting (5 min):{}", o.str());
        paxCounts.clear();
        paxLogMs = now;
    }
}

// Ram diagnostics (Server.log per 5 min): steps of a demolisher's Ram on SotA, by key
namespace
{
std::mutex ramLock;
std::map<std::string, uint32> ramCounts;
uint32 ramLogMs = 0;
}  // namespace

void CannonDestroyed() { StatAdd("cannon_destroyed", ""); }

void RamCount(std::string const& key)
{
    std::lock_guard<std::mutex> guard(ramLock);
    ++ramCounts[key];
    uint32 const now = getMSTime();
    if (!ramLogMs)
        ramLogMs = now;
    else if (getMSTimeDiff(ramLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        std::ostringstream o;
        for (auto const& [k, n] : ramCounts)
            o << ' ' << k << '=' << n;
        LOG_INFO("module", "SA ram (5 min):{}", o.str());
        ramCounts.clear();
        ramLogMs = now;
    }
}

void PaxSample(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!IsPassenger(bot))
        return;
    {
        std::lock_guard<std::mutex> guard(paxLock);
        uint32& last = paxSampled[bot->GetGUID().GetCounter()];
        if (last && getMSTimeDiff(last, getMSTime()) < 5000)
            return;
        last = getMSTime();
    }
    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    Unit* enemy = botAI->GetAiObjectContext()->GetValue<Unit*>("enemy player target")->Get();
    BotState const state = botAI->GetState();
    PaxCast(bot, state == BOT_STATE_COMBAT ? "state_combat" : state == BOT_STATE_NON_COMBAT ? "state_noncombat" : "state_dead");
    PaxCast(bot, bot->IsInCombat() ? "incombat_yes" : "incombat_no");
    float const d = target ? bot->GetExactDist(target) : 0.0f;
    PaxCast(bot, !target ? "target_none" : d < 30.0f ? "target_lt30" : d < 40.0f ? "target_30_40" : "target_40plus");
    PaxCast(bot, enemy ? "enemy_player_yes" : "enemy_player_no");
    PaxCast(bot, bot->isMoving() ? "moving_yes" : "moving_no");
}

namespace
{
std::mutex portalLock;
uint32 portalUses = 0, portalLogMs = 0;
float portalYards = 0.0f;
std::unordered_set<ObjectGuid::LowType> portalBots;
}  // namespace

void PortalUsed(Player* bot, float saving)
{
    std::lock_guard<std::mutex> guard(portalLock);
    ++portalUses;
    portalYards += saving;
    portalBots.insert(bot->GetGUID().GetCounter());
    uint32 const now = getMSTime();
    if (!portalLogMs)
        portalLogMs = now;
    else if (getMSTimeDiff(portalLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        LOG_INFO("module", "SA portals (5 min): uses {} by {} defenders, {:.0f} yd saved ({:.0f} yd per use, about {:.0f} s of running)",
                 portalUses, portalBots.size(), portalYards, portalUses ? portalYards / portalUses : 0.0f,
                 portalUses ? portalYards / portalUses / 7.0f : 0.0f);
        portalUses = 0;
        portalYards = 0.0f;
        portalBots.clear();
        portalLogMs = now;
    }
}

bool WarmupDefender(Player* bot, Battleground* bg)
{
    TeamId const attackers = Attackers(bg);
    return bg->GetStatus() == STATUS_WAIT_JOIN && bot->IsAlive() && attackers != TEAM_NEUTRAL &&
           bot->GetTeamId() != attackers && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAWarmup) &&
           BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADefense);
}

bool DefenseMoving(Player* bot, Battleground* bg)
{
    Order o;
    if (!DefenseObjective(bot, bg, o))
        return false;
    // SAWarmup: during the warmup every defender walks to its place (the round's move trigger is not active yet)
    if (WarmupDefender(bot, bg))
    {
        bool const go = o.urgent || o.hasJump || bot->GetExactDist2d(&o.move) > 8.0f;
        if (go)
            WarmupProbe(bg, 0);
        return go;
    }
    return o.urgent;
}

// SAEscort: the driven demolisher this attacker escorts (spread over them by guid); none for charge carriers, a
// quarter left free (graveyard banners, the gate) and while nothing is driven
Unit* EscortedSiege(Player* bot, Battleground* bg)
{
    uint32 const n = bot->GetGUID().GetCounter();
    if (!BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAEscort) || bot->GetTeamId() != Attackers(bg) ||
        !bot->IsAlive() || bot->GetVehicle() || bot->HasItemCount(ITEM_MASSIVE_SEAFORIUM_CHARGE, 1) || n % 4 == 3)
        return nullptr;
    if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACharges) && n % 3 == 0 && !PlayerbotAI::IsHeal(bot))
        return nullptr;
    std::vector<Unit*> siege = DrivenSiege(bg);
    if (siege.empty())
        return nullptr;
    std::sort(siege.begin(), siege.end(), [](Unit* a, Unit* b) { return a->GetGUID() < b->GetGUID(); });
    return siege[n % siege.size()];
}

// SAEscort: a few yards behind the escorted demolisher (beach side); above combat only while more than 25 yd off
bool EscortObjective(Player* bot, Battleground* bg, Order& out)
{
    Unit* v = EscortedSiege(bot, bg);
    if (!v)
        return false;
    uint32 const n = bot->GetGUID().GetCounter();
    float const a = v->GetAngle(BEACH_X, BEACH_Y);
    float const x = v->GetPositionX() + 6.0f * std::cos(a) + float(n * 37 % 9) - 4.0f;
    float const y = v->GetPositionY() + 6.0f * std::sin(a) + float(n * 53 % 9) - 4.0f;
    float const z = v->GetMap()->GetHeight(v->GetPhaseMask(), x, y, v->GetPositionZ() + 10.0f);
    out.move = Position(x, y, z > INVALID_HEIGHT ? z : v->GetPositionZ());
    out.urgent = bot->GetExactDist2d(v) > 25.0f;
    Stat(out.urgent ? "escort_catch_up" : "escort_with", bot);
    return true;
}

// SASiegeSupply: whether this attacker claims idle demolishers in this life: a roll at
// AiPlayerbot.BGTactics.SA.SupplyClaimPct (default 100) when alive and on foot, kept until it dies or boards one
std::mutex supplyLock;
std::unordered_map<ObjectGuid::LowType, std::pair<bool, bool>> supplyRoll;  // guid -> (rolled, claims)

bool SupplyClaims(Player* p)
{
    static int32 const pct = sConfigMgr->GetOption<int32>("AiPlayerbot.BGTactics.SA.SupplyClaimPct", 100, false);
    std::lock_guard<std::mutex> guard(supplyLock);
    auto& roll = supplyRoll[p->GetGUID().GetCounter()];
    if (!p->IsAlive() || p->GetVehicle())
    {
        roll.first = false;
        return false;
    }
    if (!roll.first)
        roll = {true, pct >= 100 || (pct > 0 && int32(urand(1, 100)) <= pct)};
    return roll.second;
}

// SASiegeSupply: the idle demolisher this attacker fetches: each idle one goes to the nearest free attacker on foot
// (not healers or charge holders; closest pairs first), however far
Unit* SupplySiege(Player* bot, Battleground* bg)
{
    TeamId const team = bot->GetTeamId();
    if (!BGTacticArms::IsOn(bg, team, BGTactic::SASiegeSupply) || team != Attackers(bg) || !bot->IsAlive() ||
        bot->GetVehicle() || PlayerbotAI::IsHeal(bot) || bot->HasItemCount(ITEM_MASSIVE_SEAFORIUM_CHARGE, 1))
        return nullptr;
    std::vector<Unit*> idle;
    for (uint32 i = BG_SA_DEMOLISHER_1; i <= BG_SA_DEMOLISHER_8; ++i)
        if (Creature* d = bg->GetBgMap()->GetCreature(bg->BgCreatures[i]);  // GetBGCreature logs every empty slot
            d && d->IsAlive() && d->IsVisible() && d->IsFriendlyTo(bot) &&
            !d->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) && d->GetVehicleKit() &&
            !d->GetVehicleKit()->IsVehicleInUse())
            idle.push_back(d);
    if (idle.empty())
        return nullptr;
    std::vector<std::tuple<float, Player*, Unit*>> pairs;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
        if (Player* p = ref.GetSource(); p && p->GetTeamId() == team && SupplyClaims(p) && !PlayerbotAI::IsHeal(p) &&
                                         !p->HasItemCount(ITEM_MASSIVE_SEAFORIUM_CHARGE, 1))
            for (Unit* d : idle)
                pairs.emplace_back(p->GetExactDist2d(d), p, d);
    std::sort(pairs.begin(), pairs.end(), [](auto const& a, auto const& b) { return std::get<0>(a) < std::get<0>(b); });
    std::vector<Player*> busy;
    std::vector<Unit*> taken;
    for (auto const& [dist, p, d] : pairs)
    {
        if (std::find(busy.begin(), busy.end(), p) != busy.end() || std::find(taken.begin(), taken.end(), d) != taken.end())
            continue;
        if (p == bot)
            return d;
        busy.push_back(p);
        taken.push_back(d);
    }
    return nullptr;
}

bool SupplyMoving(Player* bot, Battleground* bg)
{
    Unit* d = SupplySiege(bot, bg);
    return d && bot->GetExactDist2d(d) > 40.0f;
}

// SADriveFire: on the way to the gate (more than 50 yd off) a driver throws a boulder (2999 to players within 10 yd of
// the impact) at the enemy player in front with the most enemies around it, and rams (3000 + knockback) one right ahead
void DriveFire(Player* bot, Battleground* bg, Position const& gate)
{
    Unit* base = bot->GetVehicleBase();
    if (!base || !BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADriveFire) || bot->GetTeamId() != Attackers(bg) ||
        (base->GetEntry() != NPC_DEMOLISHER_SA && base->GetEntry() != 32796) || base->GetExactDist2d(&gate) <= 50.0f)
        return;
    VehicleSeatEntry const* seat = bot->GetVehicle()->GetSeatForPassenger(bot);
    if (!seat || !seat->CanControl())
        return;
    if (!base->HasSpellCooldown(60206))
        for (auto const& ref : bg->GetBgMap()->GetPlayers())
            if (Player* p = ref.GetSource(); p && p->IsAlive() && p->GetTeamId() != bot->GetTeamId() &&
                                             base->GetExactDist2d(p) < 10.0f && base->HasInArc(float(M_PI) / 2.0f, p) &&
                                             base->IsValidAttackTarget(p))
            {
                if (base->CastSpell(base, 60206, false) == SPELL_CAST_OK)
                    Stat("drivefire_ram", bot);
                return;
            }
    if (base->HasSpellCooldown(52338))
        return;
    Player* best = nullptr;
    uint32 bestN = 0;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || !p->IsAlive() || p->GetTeamId() == bot->GetTeamId() || base->GetExactDist2d(p) > 45.0f ||
            !base->HasInArc(float(M_PI), p) || !base->IsValidAttackTarget(p) || !base->IsWithinLOSInMap(p))
            continue;
        uint32 n = 0;
        for (auto const& ref2 : bg->GetBgMap()->GetPlayers())
            if (Player* o = ref2.GetSource(); o && o->IsAlive() && o->GetTeamId() == p->GetTeamId() && o->GetExactDist2d(p) < 10.0f)
                ++n;
        if (n > bestN)
        {
            bestN = n;
            best = p;
        }
    }
    if (best && base->CastSpell(best->GetPositionX(), best->GetPositionY(), best->GetPositionZ(), 52338, false) == SPELL_CAST_OK)
        Stat(bestN >= 2 ? "drivefire_boulder_group" : "drivefire_boulder", bot);
}

bool EscortMoving(Player* bot, Battleground* bg)
{
    Order o;
    return EscortObjective(bot, bg, o) && o.urgent;
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
    // SAAttack: 1 driver (2 once 4+ drive) pressures the other gate
    if (attacking && bot->GetVehicle() && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAAttack))
    {
        std::vector<ObjectGuid::LowType> drivers;
        for (auto const& ref : bg->GetBgMap()->GetPlayers())
            if (Player* p = ref.GetSource(); p && p->IsAlive() && p->GetTeamId() == attackers)
                if (Unit* v = p->GetVehicleBase(); v && v->GetEntry() != NPC_ANTI_PERSONNAL_CANNON)
                    drivers.push_back(p->GetGUID().GetCounter());
        std::sort(drivers.begin(), drivers.end());
        size_t const pressure = drivers.size() >= 4 ? 2 : drivers.size() >= 2 ? 1 : 0;
        auto const it = std::find(drivers.begin(), drivers.end(), bot->GetGUID().GetCounter());
        if (it != drivers.end() && size_t(it - drivers.begin()) < pressure)
            lane = 1 - lane;
    }
    if (!attacking)
        lane = DefenderLane(bot, bg);
    uint32 const target = NextGate(bg, lane);
    GameObject* go = bg->GetBGObject(target);
    if (!go)
        return false;

    if (!attacking)
    {
        if ((target != BG_SA_TITAN_RELIC || BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADefPack)) &&
            DefenseObjective(bot, bg, out))
            return true;
        if (target == BG_SA_TITAN_RELIC)
        {
            out.move = Floor(go);
            if (Unit* in = bot->GetVehicleBase(); in && in->GetEntry() == NPC_ANTI_PERSONNAL_CANNON &&
                                                   BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SARelicCrew))
                out.leave = true;
            return true;
        }
        if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACannons))
        {
            Unit* in = bot->GetVehicleBase();
            if (in && in->GetEntry() == NPC_ANTI_PERSONNAL_CANNON)
            {
                // nobody in its range: free the crew for another gun
                out.leave = GunDraw(bg, in, BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACannonsFollow)) == 0;
                out.move = in->GetPosition();
                return true;
            }
            if (Creature* gun = GunCrew(bot, bg) ? BusiestGun(bg, BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACannonsFollow))
                                                 : nullptr)
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
                if (GameObject* portal = PortalToward(bot, bg, top, &out.portalSaving))
                {
                    BGStrand::Stat("portal_chosen", bot);
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
            if (GameObject* portal = PortalToward(bot, bg, out.move, &out.portalSaving))
            {
                BGStrand::Stat("portal_chosen", bot);
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

    if (Vehicle* veh = bot->GetVehicle(); veh && veh->GetSeatForPassenger(bot) && !veh->GetSeatForPassenger(bot)->CanControl())
    {
        // SAPassengers: a passenger fights from its seat; with a charge, it gets out at the gate to plant it
        if (bot->HasItemCount(ITEM_MASSIVE_SEAFORIUM_CHARGE, 1) && bot->GetVehicleBase()->GetExactDist2d(go) < 12.0f)
        {
            Stat("passenger_exit_plant", bot);
            out.move = bot->GetPosition();
            out.leave = true;
            return true;
        }
        return false;
    }
    if (bot->GetVehicle())
    {
        // SARam: to the wall (Ram hits 3 yd ahead within 3 yd)
        out.move = Near(go, BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SARam) ? 5.0f : 12.0f, bot);
        out.siege = go->GetPosition();
        out.hasSiege = true;
        return true;
    }

    // SASiegeSupply: an idle demolisher, however far (above combat while more than 40 yd off)
    if (Unit* demo = SupplySiege(bot, bg))
    {
        out.move = demo->GetPosition();
        out.urgent = bot->GetExactDist2d(demo) > 40.0f;
        Stat("supply_fetch", bot);
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
        GameObject* pile = NearestBombPile(bot, bg, go, BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAChargesOnWay));
        // SASiegeSupply: nothing on the way: go back to the nearest pile within 250 yd
        if (!pile && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SASiegeSupply) &&
            (pile = NearestBombPile(bot, bg, go, false, 250.0f)))
            Stat("supply_refill", bot);
        if (pile)
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
    // SASiegeSupply: not the central graveyard: deaths in the courtyard then respawn at the workshops' graveyards
    if (!banner && (Destroyed(bg, BG_SA_PURPLE_GATE) || Destroyed(bg, BG_SA_RED_GATE)) &&
        !BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SASiegeSupply))
        banner = EnemyBanner(bg, BG_SA_CENTRAL_FLAG, attackers);
    if (EscortObjective(bot, bg, out))
        return true;
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
    if (!bg || bg->GetMapId() != 607 || !bot->IsAlive() || bot->GetVehicle() || PlayerbotAI::IsHeal(bot))
        return nullptr;
    // SAAttack: an enemy at one of our planted charges is disarming it
    if (bot->GetTeamId() == Attackers(bg))
    {
        if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAAttack))
        {
            GameObject* charge = bot->FindNearestGameObject(GO_PLANTED_CHARGE, 30.0f, true);
            Unit* best = nullptr;
            float bestDist = 8.0f;
            if (charge)
                for (auto const& ref : bg->GetBgMap()->GetPlayers())
                    if (Player* p = ref.GetSource(); p && p->IsAlive() && p->GetTeamId() != bot->GetTeamId() &&
                                                     bot->IsValidAttackTarget(p) && bot->IsWithinLOSInMap(p))
                        if (float const d = p->GetExactDist2d(charge); d < bestDist)
                        {
                            bestDist = d;
                            best = p;
                        }
            if (best)
                return best;
        }
        // SACannonKill: a ranged attacker destroys the nearest manned cannon within 40 yd (cannons deal about a third of
        // all demolisher damage)
        if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACannonKill) && PlayerbotAI::IsRanged(bot))
        {
            Unit* gun = nullptr;
            float gunDist = 40.0f;
            for (uint32 i = BG_SA_GUN_1; i <= BG_SA_GUN_10; ++i)
                if (Creature* c = bg->GetBgMap()->GetCreature(bg->BgCreatures[i]);
                    c && c->IsAlive() && c->GetVehicleKit() && c->GetVehicleKit()->IsVehicleInUse() &&
                    bot->GetExactDist2d(c) < gunDist && bot->IsValidAttackTarget(c) && bot->IsWithinLOSInMap(c))
                {
                    gunDist = bot->GetExactDist2d(c);
                    gun = c;
                }
            if (gun)
            {
                Stat("cannon_target", bot);
                return gun;
            }
        }
        // SAEscort: an enemy hitting the escorted demolisher (within 40 yd of it), else the nearest within 15 yd
        if (Unit* v = EscortedSiege(bot, bg))
        {
            Unit* pick = nullptr;
            float pickDist = 1000.0f;
            bool onIt = false;
            for (auto const& ref : bg->GetBgMap()->GetPlayers())
            {
                Player* p = ref.GetSource();
                if (!p || !p->IsAlive() || p->GetTeamId() == bot->GetTeamId())
                    continue;
                bool const hits = p->GetVictim() == v;
                float const d = p->GetExactDist2d(v);
                if (d > (hits ? 40.0f : 15.0f) || (onIt && !hits) || !bot->IsValidAttackTarget(p) ||
                    !bot->IsWithinLOSInMap(p))
                    continue;
                if ((hits && !onIt) || d < pickDist)
                {
                    pick = p;
                    pickDist = d;
                    onIt = hits;
                }
            }
            if (pick)
            {
                Stat(onIt ? "escort_pick_hitter" : "escort_pick_near", bot);
                return pick;
            }
        }
        return nullptr;
    }
    // SADefPack: an attacker just thrown out of a destroyed demolisher, then (once the relic door is down) the attacker
    // nearest the relic
    if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADefPack) && Attackers(bg) != TEAM_NEUTRAL &&
        bot->GetTeamId() != Attackers(bg))
    {
        Unit* driver = nullptr;
        float best = 60.0f;
        for (auto const& ref : bg->GetBgMap()->GetPlayers())
            if (Player* p = ref.GetSource(); p && p->IsAlive() && p->GetTeamId() == Attackers(bg) && JustDismounted(p) &&
                                             bot->IsValidAttackTarget(p) && bot->GetExactDist2d(p) < best &&
                                             bot->IsWithinLOSInMap(p))
            {
                best = bot->GetExactDist2d(p);
                driver = p;
            }
        if (driver)
        {
            Stat("driver_pick", bot);
            return driver;
        }
        if (Destroyed(bg, BG_SA_ANCIENT_GATE))
            if (GameObject* relic = bg->GetBGObject(BG_SA_TITAN_RELIC))
            {
                Unit* nearest = nullptr;
                float nd = 25.0f;
                for (auto const& ref : bg->GetBgMap()->GetPlayers())
                    if (Player* p = ref.GetSource(); p && p->IsAlive() && p->GetTeamId() == Attackers(bg) &&
                                                     p->GetExactDist2d(relic) < nd && bot->IsValidAttackTarget(p))
                    {
                        nd = p->GetExactDist2d(relic);
                        nearest = p;
                    }
                if (nearest)
                {
                    Stat("relic_pick", bot);
                    return nearest;
                }
            }
    }
    if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SADefense) && Attackers(bg) != TEAM_NEUTRAL &&
        bot->GetTeamId() != Attackers(bg))
    {
        bool const focus = BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAFocusSiege);
        Unit* best = nullptr;
        // SAWallRanged: ranged dps reach 60 yd (from the wall a demolisher shelling from 45-50 yd is in sight)
        float const reach = BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAWallRanged) && PlayerbotAI::IsRanged(bot)
                                ? 60.0f : 45.0f;
        float bestDist = reach, bestPct = 101.0f;
        for (Unit* d : DrivenSiege(bg))
        {
            float const dist = bot->GetExactDist2d(d);
            if (dist >= reach || !bot->IsValidAttackTarget(d) || !bot->IsWithinLOSInMap(d))
                continue;
            // SAFocusSiege: the weakest in reach, so everyone out front works on the same one
            if (focus ? d->GetHealthPct() < bestPct : dist < bestDist)
            {
                bestDist = dist;
                bestPct = d->GetHealthPct();
                best = d;
            }
        }
        if (best)
            return best;
        // SAPreDamage: wear parked demolishers down to 10% but leave them alive (a killed one respawns at full health);
        // workshop guards do the same there (SAWorkshopKill: destroy them instead, 30 s respawn)
        bool const wsGuard = GuardedWorkshop(bot, bg) != nullptr;
        bool const wsKill = wsGuard && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAWorkshopKill);
        if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAPreDamage) || wsGuard)
        {
            auto parked = [](Unit* u)
            {
                return u && (u->GetEntry() == NPC_DEMOLISHER_SA || u->GetEntry() == 32796) && u->IsAlive() &&
                       u->GetVehicleKit() && !u->GetVehicleKit()->IsVehicleInUse();
            };
            if (Unit* victim = bot->GetVictim(); !wsKill && parked(victim) && victim->GetHealthPct() <= 10.0f)
                bot->AttackStop();  // low enough: stop before it dies
            std::list<Creature*> demos;
            bot->GetCreatureListWithEntryInGrid(demos, NPC_DEMOLISHER_SA, 40.0f);
            bot->GetCreatureListWithEntryInGrid(demos, 32796, 40.0f);
            Unit* target = nullptr;
            float nearest = 40.0f;
            for (Creature* d : demos)
                if (parked(d) && (wsKill || d->GetHealthPct() > 10.0f) && bot->IsValidAttackTarget(d) && bot->IsWithinLOSInMap(d))
                    if (float const dist = bot->GetExactDist2d(d); dist < nearest)
                    {
                        nearest = dist;
                        target = d;
                    }
            if (target)
            {
                if (wsGuard)
                    Stat(wsKill ? "ws_kill_idle" : "ws_wear_idle", bot);
                return target;
            }
        }
    }
    bool const bombs = BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SABombHunt);
    bool const sortie = BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SASortie) && SortieRole(bot);
    if (!bombs && !sortie && !BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SAHunt))
        return nullptr;
    TeamId const attackers = Attackers(bg);
    if (attackers == TEAM_NEUTRAL || bot->GetTeamId() == attackers)
        return nullptr;
    GameObject* gate = bg->GetBGObject(NextGate(bg, DefenderLane(bot, bg)));
    if (bombs && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SABombHuntGate))
        for (uint32 lane : {0u, 1u})
            if (GameObject* g = bg->GetBGObject(NextGate(bg, lane)); g && (!gate || bot->GetExactDist2d(g) < bot->GetExactDist2d(gate)))
                gate = g;
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
    // Rocket Blast hits every unit within 8 yd (10-70 yd, not buildings): SACannonSiege: a driven demolisher in range first
    // (the guides: two turrets kill one before it reaches the wall); else the enemy on foot with the most enemies around
    if (BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::SACannonSiege))
    {
        Unit* siege = nullptr;
        float bestPct = 101.0f;
        for (Unit* d : DrivenSiege(bg))
        {
            float const dist = gun->GetExactDist2d(d);
            if (dist < 10.0f || dist > 70.0f || d->GetHealthPct() >= bestPct ||
                (!gun->IsWithinLOSInMap(d) &&
                 !gun->GetMap()->isInLineOfSight(gun->GetPositionX(), gun->GetPositionY(), gun->GetPositionZ() + 4.0f,
                                                 d->GetPositionX(), d->GetPositionY(), d->GetPositionZ() + 2.0f,
                                                 gun->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing)))
                continue;
            bestPct = d->GetHealthPct();
            siege = d;
        }
        if (siege)
            return siege;
    }
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

void BGStrand::Forget(Battleground* bg)
{
    uint32 const id = bg->GetInstanceID();
    {
        std::lock_guard<std::mutex> guard(laneLock);
        lanes.erase(id);
        focusLanes.erase(id);
        defendLanes.erase(id);
    }
    ForgetAmbush(id);
}

void BGStrand::RelicCannon(Player* bot, Battleground* bg, bool target, bool shot)
{
    if (!Destroyed(bg, BG_SA_ANCIENT_GATE))
        return;
    Stat("relic_gunner", bot);  // distinct gunners, at most once per 10 s each
    StatAdd(shot ? "relic_shot" : target ? "relic_not_cast" : "relic_no_target", "");
}
