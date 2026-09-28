/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGHealerGuard.h"

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include "BGTacticArms.h"
#include "Battleground.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
struct Guards
{
    uint32 builtMs = 0;
    std::map<ObjectGuid, ObjectGuid> healerOf;  // bodyguard -> healer
};
std::mutex guardLock;
std::map<std::pair<uint32, uint32>, Guards> teams;  // (instance, team)
std::atomic<uint32> statPairs{0}, statPlans{0}, statFollows{0}, statTargets{0}, statLogMs{0};

bool MeleeDps(Player* p)
{
    return !PlayerbotAI::IsHeal(p, true) && !PlayerbotAI::IsTank(p, true) && !PlayerbotAI::IsRanged(p, true);
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
    uint32 const plans = statPlans.exchange(0);
    if (plans)
        LOG_INFO("module", "HealerGuard (5 min): plans {}, avg pairs {:.1f}, follow picks {}, targets on a healer {}", plans,
                 double(statPairs.exchange(0)) / plans, statFollows.exchange(0), statTargets.exchange(0));
}

// Rebuild the pairs (every 3 s). Needs guardLock.
void Build(Battleground* bg, TeamId team, Guards& g)
{
    std::vector<Player*> healers, melee;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || !p->IsInWorld() || !p->IsAlive() || p->GetTeamId() != team || !GET_PLAYERBOT_AI(p) || p->GetVehicle())
            continue;
        if (PlayerbotAI::IsHeal(p, true))
            healers.push_back(p);
        else if (MeleeDps(p))
            melee.push_back(p);
    }
    std::map<ObjectGuid, ObjectGuid> next;
    std::map<ObjectGuid, bool> guarded, taken;
    // keep pairs whose members are both alive and within 60 yd
    for (auto const& [guard, healer] : g.healerOf)
    {
        Player* gp = ObjectAccessor::FindPlayer(guard);
        Player* hp = ObjectAccessor::FindPlayer(healer);
        if (gp && hp && gp->IsAlive() && hp->IsAlive() && gp->GetTeamId() == team && gp->GetMap() == hp->GetMap() &&
            gp->GetExactDist2d(hp) < 60.0f && !guarded[healer])
        {
            next[guard] = healer;
            guarded[healer] = taken[guard] = true;
        }
    }
    for (Player* h : healers)
    {
        if (guarded[h->GetGUID()])
            continue;
        Player* best = nullptr;
        for (Player* m : melee)
            if (!taken[m->GetGUID()] && m->GetExactDist2d(h) < 60.0f &&
                (!best || m->GetExactDist2d(h) < best->GetExactDist2d(h)))
                best = m;
        if (best)
        {
            next[best->GetGUID()] = h->GetGUID();
            taken[best->GetGUID()] = true;
        }
    }
    g.healerOf = next;
    ++statPlans;
    statPairs += uint32(next.size());
}

Player* HealerFor(Player* bot, Battleground* bg)
{
    if (bg->GetStatus() != STATUS_IN_PROGRESS || !bot->IsAlive() || bot->GetVehicle() ||
        !BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::HealerGuard))
        return nullptr;
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(guardLock);
    std::pair<uint32, uint32> const key{bg->GetInstanceID(), uint32(bot->GetTeamId())};
    if (teams.size() > 64 && !teams.count(key))
        teams.clear();  // ended games
    Guards& g = teams[key];
    if (!g.builtMs || getMSTimeDiff(g.builtMs, now) > 3 * IN_MILLISECONDS)
    {
        Build(bg, bot->GetTeamId(), g);
        g.builtMs = now;
    }
    auto it = g.healerOf.find(bot->GetGUID());
    if (it == g.healerOf.end())
        return nullptr;
    Player* h = ObjectAccessor::FindPlayer(it->second);
    return h && h->IsAlive() && h->GetMap() == bot->GetMap() ? h : nullptr;
}
}  // namespace

bool BGHealerGuard::Hold(PlayerbotAI* botAI, Battleground* bg, Position& out)
{
    Player* bot = botAI->GetBot();
    Player* h = HealerFor(bot, bg);
    if (!h || bot->GetExactDist2d(h) <= 12.0f)
        return false;
    out.Relocate(h->GetPositionX(), h->GetPositionY(), h->GetPositionZ());
    ++statFollows;
    MaybeLog();
    return true;
}

Unit* BGHealerGuard::Target(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return nullptr;
    Player* h = HealerFor(bot, bg);
    if (!h || bot->GetExactDist2d(h) > 30.0f)
        return nullptr;
    Player* onHealer = nullptr;
    Player* nearest = nullptr;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || !p->IsInWorld() || !p->IsAlive() || p->GetTeamId() == bot->GetTeamId() || p->GetExactDist2d(h) > 30.0f ||
            !bot->IsWithinLOSInMap(p) || !bot->CanSeeOrDetect(p))
            continue;
        if (p->GetVictim() == h && (!onHealer || p->GetExactDist2d(h) < onHealer->GetExactDist2d(h)))
            onHealer = p;
        if (p->GetExactDist2d(h) < 10.0f && (!nearest || p->GetExactDist2d(h) < nearest->GetExactDist2d(h)))
            nearest = p;
    }
    Unit* pick = onHealer ? onHealer : nearest;
    if (pick)
        ++statTargets;
    MaybeLog();
    return pick;
}
