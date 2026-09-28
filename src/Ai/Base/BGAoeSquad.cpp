/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGAoeSquad.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "BGTacticArms.h"
#include "Battleground.h"
#include "BattlegroundAV.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
// Chokes: centers of the 40-yd cells with the most deaths in the stock games (121 AV, 100 IoC), fight lines only
// (not the generals' rooms)
struct Choke
{
    float x, y, z;
};
std::vector<Choke> const avChokes = {{-343, -377, 10}, {-555, -320, 40}, {-760, -347, 66}, {380, -389, 1}, {653, -308, 33}};
std::vector<Choke> const icChokes = {{790, -805, 6}, {1005, -1035, 16}, {732, -370, 17}, {880, -880, 8}};

struct Squad
{
    uint32 builtMs = 0;
    int32 choke = -1;              // active choke index, -1 none
    uint32 activeUntilMs = 0;      // stays up this long after the choke was last busy
    float hx = 0, hy = 0, hz = 0;  // hold point
    std::set<ObjectGuid> members;
};
std::mutex squadLock;
std::map<std::pair<uint32, uint32>, Squad> squads;  // (instance, team)
constexpr int32 GENERAL = 100;  // Squad::choke while defending our AV general
std::atomic<uint32> statPlans[2][2], statHolds[2], statTargets[2], statCluster[2], statGeneral{0}, statLogMs{0};

bool AoeClass(Player* p)
{
    switch (p->getClass())
    {
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        case CLASS_HUNTER:
            return true;
        case CLASS_SHAMAN:
        case CLASS_DRUID:
            return PlayerbotAI::IsRanged(p, true) && !PlayerbotAI::IsHeal(p, true);
        default:
            return false;
    }
}

uint32 SquadSize(Battleground* bg, TeamId team)
{
    if (BGTacticArms::IsOn(bg, team, BGTactic::AoESquadAll))
        return 99;
    if (BGTacticArms::IsOn(bg, team, BGTactic::AoESquad5))
        return 5;
    if (BGTacticArms::IsOn(bg, team, BGTactic::AoESquad2))
        return 2;
    return 0;
}

int32 MapIndex(Battleground* bg)
{
    BattlegroundTypeId type = bg->GetBgTypeID();
    if (type == BATTLEGROUND_RB)
        type = bg->GetBgTypeID(true);
    return type == BATTLEGROUND_AV ? 0 : type == BATTLEGROUND_IC ? 1 : -1;
}

void MaybeLog()
{
    uint32 const now = getMSTime();
    uint32 last = statLogMs.load();
    if (!last)
    {
        statLogMs.compare_exchange_strong(last, now);
        return;
    }
    if (getMSTimeDiff(last, now) <= 5 * MINUTE * IN_MILLISECONDS || !statLogMs.compare_exchange_strong(last, now))
        return;
    static char const* const names[2] = {"AV", "IC"};
    for (uint32 m = 0; m < 2; ++m)
    {
        uint32 const plans = statPlans[m][0].exchange(0), active = statPlans[m][1].exchange(0);
        uint32 const holds = statHolds[m].exchange(0), targets = statTargets[m].exchange(0), cl = statCluster[m].exchange(0);
        if (plans)
            LOG_INFO("module", "AoE squad {} (5 min): plans {}, active {} ({:.0f}%; defending the general {}), hold picks {}, "
                     "pack targets {} (avg {:.1f} enemies within 8 yd of the target, itself included)",
                     names[m], plans, active, 100.0 * active / plans, m == 0 ? statGeneral.exchange(0) : 0, holds, targets,
                     targets ? double(cl) / targets : 0.0);
    }
}

bool GeneralOn(Battleground* bg, TeamId team)
{
    return MapIndex(bg) == 0 && BGTacticArms::IsOn(bg, team, BGTactic::AoEGeneral);
}

// Rebuild the team's squad (every 3 s). Needs squadLock.
void Build(Battleground* bg, TeamId team, Squad& sq, uint32 now)
{
    int32 const m = MapIndex(bg);
    std::vector<Choke> const& chokes = m == 0 ? avChokes : icChokes;
    Position const* ours = bg->GetTeamStartPosition(team);
    Position const* theirs = bg->GetTeamStartPosition(team == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE);
    std::vector<Player*> enemies, aoe;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || !p->IsInWorld() || !p->IsAlive())
            continue;
        if (p->GetTeamId() != team)
            enemies.push_back(p);
        else if (GET_PLAYERBOT_AI(p) && !p->GetVehicle() && AoeClass(p))
            aoe.push_back(p);
    }
    ++statPlans[m][0];

    // General defense (AV, arm AoEGeneral): 5+ enemies within 50 yd of our general: every area-damage bot, wherever
    // it is, goes to him (attackers die there in packs: median 13-22 deaths per wave, 1-2 s apart). Takes precedence
    // over the chokes.
    if (GeneralOn(bg, team))
    {
        Creature* general = bg->GetBGCreature(team == TEAM_ALLIANCE ? AV_CPLACE_A_BOSS : AV_CPLACE_H_BOSS);
        if (general && general->IsAlive())
        {
            uint32 n = 0;
            for (Player* e : enemies)
                n += e->GetExactDist2d(general) < 50.0f;
            if (n >= 5 || (sq.choke == GENERAL && now <= sq.activeUntilMs))
            {
                if (n >= 5)
                    sq.activeUntilMs = now + 15 * IN_MILLISECONDS;
                if (sq.choke != GENERAL)
                {
                    float dx = 0, dy = 0;
                    if (ours)
                    {
                        dx = ours->GetPositionX() - general->GetPositionX();
                        dy = ours->GetPositionY() - general->GetPositionY();
                        float const d = std::max(1.0f, std::hypot(dx, dy));
                        dx /= d;
                        dy /= d;
                    }
                    sq.hx = general->GetPositionX() + dx * 10.0f;
                    sq.hy = general->GetPositionY() + dy * 10.0f;
                    sq.hz = general->GetPositionZ();
                    float const h = bg->GetBgMap()->GetHeight(sq.hx, sq.hy, sq.hz + 5.0f);
                    if (h > INVALID_HEIGHT && std::abs(h - sq.hz) < 8.0f)
                        sq.hz = h;
                }
                sq.choke = GENERAL;
                sq.members.clear();
                for (Player* p : aoe)
                    sq.members.insert(p->GetGUID());
                ++statPlans[m][1];
                ++statGeneral;
                return;
            }
        }
        if (sq.choke == GENERAL)
            sq.choke = -1;
    }
    if (!SquadSize(bg, team))
    {
        sq.choke = -1;
        sq.members.clear();
        return;
    }
    // the busiest of our chokes (on our half: not farther from our start than 150 yd beyond theirs)
    int32 best = -1;
    uint32 bestN = 0;
    for (size_t i = 0; i < chokes.size(); ++i)
    {
        Choke const& c = chokes[i];
        if (ours && theirs &&
            std::hypot(c.x - ours->GetPositionX(), c.y - ours->GetPositionY()) >
                std::hypot(c.x - theirs->GetPositionX(), c.y - theirs->GetPositionY()) + 150.0f)
            continue;
        uint32 n = 0;
        for (Player* e : enemies)
            n += std::hypot(e->GetPositionX() - c.x, e->GetPositionY() - c.y) < 70.0f;
        if (n > bestN)
        {
            bestN = n;
            best = int32(i);
        }
    }
    if (best >= 0 && bestN >= 5)
    {
        if (sq.choke != best || now > sq.activeUntilMs)
        {
            Choke const& c = chokes[best];
            float dx = 0, dy = 0;
            if (ours)
            {
                dx = ours->GetPositionX() - c.x;
                dy = ours->GetPositionY() - c.y;
                float const d = std::max(1.0f, std::hypot(dx, dy));
                dx /= d;
                dy /= d;
            }
            sq.hx = c.x + dx * 22.0f;
            sq.hy = c.y + dy * 22.0f;
            sq.hz = c.z;
            float const h = bg->GetBgMap()->GetHeight(sq.hx, sq.hy, c.z + 10.0f);
            if (h > INVALID_HEIGHT && std::abs(h - c.z) < 15.0f)
                sq.hz = h;
        }
        sq.choke = best;
        sq.activeUntilMs = now + 15 * IN_MILLISECONDS;
    }
    else if (now > sq.activeUntilMs)
        sq.choke = -1;

    sq.members.clear();
    if (sq.choke >= 0)
    {
        std::sort(aoe.begin(), aoe.end(),
                  [&](Player* a, Player* b)
                  {
                      return std::hypot(a->GetPositionX() - sq.hx, a->GetPositionY() - sq.hy) <
                             std::hypot(b->GetPositionX() - sq.hx, b->GetPositionY() - sq.hy);
                  });
        uint32 const n = SquadSize(bg, team);
        for (Player* p : aoe)
        {
            if (sq.members.size() >= n)
                break;
            if (std::hypot(p->GetPositionX() - sq.hx, p->GetPositionY() - sq.hy) < 350.0f)
                sq.members.insert(p->GetGUID());
        }
    }
    if (sq.choke >= 0)
        ++statPlans[m][1];
}

bool Member(Player* bot, Battleground* bg, Squad* out)
{
    int32 const m = MapIndex(bg);
    if (m < 0 || bg->GetStatus() != STATUS_IN_PROGRESS || !bot->IsAlive() || bot->GetVehicle())
        return false;
    TeamId const team = bot->GetTeamId();
    if (!SquadSize(bg, team) && !GeneralOn(bg, team))
        return false;
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(squadLock);
    std::pair<uint32, uint32> const key{bg->GetInstanceID(), uint32(team)};
    if (squads.size() > 64)  // ended games: drop entries idle for 10 minutes (live games rebuild every few s)
        for (auto it = squads.begin(); it != squads.end();)
            it = it->second.builtMs && getMSTimeDiff(it->second.builtMs, getMSTime()) > 10 * MINUTE * IN_MILLISECONDS
                     ? squads.erase(it)
                     : std::next(it);
    Squad& sq = squads[key];
    if (!sq.builtMs || getMSTimeDiff(sq.builtMs, now) > 3 * IN_MILLISECONDS)
    {
        Build(bg, team, sq, now);
        sq.builtMs = now;
    }
    if (sq.choke < 0 || !sq.members.count(bot->GetGUID()))
        return false;
    if (out)
        *out = sq;
    return true;
}
}  // namespace

bool BGAoeSquad::Hold(PlayerbotAI* botAI, Battleground* bg, Position& out)
{
    Player* bot = botAI->GetBot();
    Squad sq;
    if (!Member(bot, bg, &sq))
        return false;
    // a fixed spot per bot around the hold point, so the squad does not stack on one spot
    uint32 const c = bot->GetGUID().GetCounter();
    float const angle = float(c) * 2.39996f;
    float const r = 3.0f + float((c * 7919u) % 100u) / 100.0f * 5.0f;
    out.Relocate(sq.hx + std::cos(angle) * r, sq.hy + std::sin(angle) * r, sq.hz);
    ++statHolds[MapIndex(bg)];
    MaybeLog();
    return true;
}

Unit* BGAoeSquad::Target(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Battleground* bg = bot->GetBattleground();
    if (!bg || !bot->IsInCombat() || !Member(bot, bg, nullptr))
        return nullptr;
    std::vector<Player*> enemies;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (p && p->IsInWorld() && p->IsAlive() && p->GetTeamId() != bot->GetTeamId())
            enemies.push_back(p);
    }
    Player* best = nullptr;
    uint32 bestN = 0;
    for (Player* e : enemies)
    {
        if (!bot->IsWithinDistInMap(e, 36.0f) || !bot->IsWithinLOSInMap(e) || !bot->CanSeeOrDetect(e))
            continue;
        uint32 n = 0;
        for (Player* o : enemies)
            n += o != e && e->GetExactDist2d(o) < 8.0f;
        if (n > bestN || (n == bestN && best && e->GetHealthPct() < best->GetHealthPct()))
        {
            bestN = n;
            best = e;
        }
    }
    if (!best || bestN < 1)
        return nullptr;  // no pack: the normal target choice
    int32 const m = MapIndex(bg);
    ++statTargets[m];
    statCluster[m] += bestN + 1;
    MaybeLog();
    return best;
}
