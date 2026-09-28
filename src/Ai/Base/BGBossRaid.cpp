/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGBossRaid.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

#include "BGTacticArms.h"
#include "Battleground.h"
#include "BattlegroundAV.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
enum Phase : uint8
{
    GATHER,
    PULL,
    ENGAGE
};
char const* const phaseNames[3] = {"gather", "pull", "engage"};

struct Raid
{
    uint32 builtMs = 0;
    Phase phase = GATHER;
    uint32 phaseMs = 0;
    uint32 outOfCombatMs = 0;
    ObjectGuid boss;
    std::vector<ObjectGuid> tanks;              // [0] on the general, [1] on the guards
    std::map<ObjectGuid, ObjectGuid> healerOf;  // healer -> tank
    std::vector<ObjectGuid> adds;               // the general's guards, alive
};
std::mutex raidLock;
std::map<std::pair<uint32, uint32>, Raid> raids;  // (instance, attacking team)
std::atomic<uint32> statPhase[3], statPulls{0}, statVictim[2], statLogMs{0};  // statVictim: [samples, on a tank]

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
    uint32 const g = statPhase[0].exchange(0), p = statPhase[1].exchange(0), e = statPhase[2].exchange(0);
    uint32 const vs = statVictim[0].exchange(0), vt = statVictim[1].exchange(0);
    if (g + p + e)
        LOG_INFO("module", "BossRaid AV (5 min): raid plans gather {}, pull {}, engage {}; pulls started {}; general "
                 "hitting a tank {:.0f}% of engage samples ({})",
                 g, p, e, statPulls.exchange(0), vs ? 100.0 * vt / vs : 0.0, vs);
}

// Rebuild the raid (every 2 s). Needs raidLock.
void Build(Battleground* bg, TeamId team, Raid& r, Creature* boss, Position const& waitPos, Player* bot, uint32 now)
{
    r.boss = boss->GetGUID();
    std::vector<Player*> ours;
    uint32 gathered = 0, near = 0;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || !p->IsInWorld() || !p->IsAlive() || p->GetTeamId() != team)
            continue;
        float const dw = p->GetExactDist2d(waitPos.GetPositionX(), waitPos.GetPositionY());
        float const db = p->GetExactDist2d(boss);
        gathered += dw < 50.0f || db < 60.0f;
        near += db < 80.0f;
        if (GET_PLAYERBOT_AI(p) && (dw < 120.0f || db < 120.0f))
            ours.push_back(p);
    }
    // the general's guards: hostile creatures alive within 35 yd of him
    r.adds.clear();
    for (ObjectGuid const& guid : bg->BgCreatures)
        if (Creature* c = guid.IsEmpty() ? nullptr : bg->GetBgMap()->GetCreature(guid))
            if (c != boss && c->IsAlive() && !c->IsCritter() && c->IsHostileTo(bot) && c->GetExactDist2d(boss) < 35.0f)
                r.adds.push_back(guid);

    // tanks: keep the current ones while alive and near; fill up to 2 with the tank-spec bots nearest the general
    std::vector<ObjectGuid> tanks;
    for (ObjectGuid const& t : r.tanks)
        if (Player* p = ObjectAccessor::FindPlayer(t);
            p && p->IsAlive() && std::find(ours.begin(), ours.end(), p) != ours.end())
            tanks.push_back(t);
    std::sort(ours.begin(), ours.end(), [&](Player* a, Player* b) { return a->GetExactDist2d(boss) < b->GetExactDist2d(boss); });
    for (Player* p : ours)
        if (tanks.size() < 2 && PlayerbotAI::IsTank(p, true) &&
            std::find(tanks.begin(), tanks.end(), p->GetGUID()) == tanks.end())
            tanks.push_back(p->GetGUID());
    r.tanks = tanks;
    // healers: up to 3, round robin over the tanks
    r.healerOf.clear();
    if (!tanks.empty())
        for (Player* p : ours)
            if (r.healerOf.size() < 3 && PlayerbotAI::IsHeal(p, true))
                r.healerOf[p->GetGUID()] = tanks[r.healerOf.size() % tanks.size()];

    Unit* victim = boss->IsInCombat() ? boss->GetVictim() : nullptr;
    bool const onTank = victim && std::find(tanks.begin(), tanks.end(), victim->GetGUID()) != tanks.end();
    if (boss->IsInCombat())
        r.outOfCombatMs = 0;
    else if (!r.outOfCombatMs)
        r.outOfCombatMs = now;

    switch (r.phase)
    {
        case GATHER:
            if (gathered >= 20)
            {
                r.phase = tanks.empty() ? ENGAGE : PULL;  // no tank: all in together, at least as a group
                r.phaseMs = now;
                ++statPulls;
            }
            break;
        case PULL:
            if (onTank || getMSTimeDiff(r.phaseMs, now) > 5 * IN_MILLISECONDS)
            {
                r.phase = ENGAGE;
                r.phaseMs = now;
            }
            break;
        case ENGAGE:
            if ((near < 8 && getMSTimeDiff(r.phaseMs, now) > 10 * IN_MILLISECONDS) ||
                (r.outOfCombatMs && getMSTimeDiff(r.outOfCombatMs, now) > 10 * IN_MILLISECONDS))
            {
                r.phase = GATHER;
                r.phaseMs = now;
            }
            break;
    }
    ++statPhase[r.phase];
    if (r.phase == ENGAGE && victim)
    {
        ++statVictim[0];
        if (onTank)
            ++statVictim[1];
    }
}

// The team's raid, rebuilt when stale; nullptr when the arm is off. Needs raidLock.
Raid* Get(Battleground* bg, Player* bot, Creature* boss, Position const* waitPos)
{
    TeamId const team = bot->GetTeamId();
    if (!BGTacticArms::IsOn(bg, team, BGTactic::BossRaid))
        return nullptr;
    std::pair<uint32, uint32> const key{bg->GetInstanceID(), uint32(team)};
    if (raids.size() > 64)  // ended games: drop entries idle for 10 minutes (live games rebuild every few s)
        for (auto it = raids.begin(); it != raids.end();)
            it = it->second.builtMs && getMSTimeDiff(it->second.builtMs, getMSTime()) > 10 * MINUTE * IN_MILLISECONDS
                     ? raids.erase(it)
                     : std::next(it);
    auto it = raids.find(key);
    if (!boss || !waitPos)  // lookups from targeting/healing: only an existing raid
        return it == raids.end() ? nullptr : &it->second;
    Raid& r = raids[key];
    uint32 const now = getMSTime();
    if (!r.builtMs || getMSTimeDiff(r.builtMs, now) > 2 * IN_MILLISECONDS)
    {
        Build(bg, team, r, boss, *waitPos, bot, now);
        r.builtMs = now;
    }
    return &r;
}

void Spot(Player* bot, Position const& center, float rmin, float rmax, Position& out)
{
    uint32 const c = bot->GetGUID().GetCounter();
    float const angle = float(c) * 2.39996f;
    float const r = rmin + float((c * 7919u) % 100u) / 100.0f * (rmax - rmin);
    out.Relocate(center.GetPositionX() + std::cos(angle) * r, center.GetPositionY() + std::sin(angle) * r,
                 center.GetPositionZ());
}
}  // namespace

int BGBossRaid::Plan(PlayerbotAI* botAI, Battleground* bg, Creature* boss, Position const& waitPos, Position& out)
{
    Player* bot = botAI->GetBot();
    std::lock_guard<std::mutex> guard(raidLock);
    Raid* r = Get(bg, bot, boss, &waitPos);
    if (!r)
        return 1;
    MaybeLog();
    bool const tank = std::find(r->tanks.begin(), r->tanks.end(), bot->GetGUID()) != r->tanks.end();
    bool const healer = r->healerOf.count(bot->GetGUID()) > 0;
    if (r->phase == GATHER || (r->phase == PULL && !tank))
    {
        Spot(bot, waitPos, 3.0f, 10.0f, out);
        return 2;
    }
    if (healer)  // 18 yd from the general, on the wait point's side
    {
        float dx = waitPos.GetPositionX() - boss->GetPositionX(), dy = waitPos.GetPositionY() - boss->GetPositionY();
        float const d = std::max(1.0f, std::hypot(dx, dy));
        Position const c(boss->GetPositionX() + dx / d * 18.0f, boss->GetPositionY() + dy / d * 18.0f, boss->GetPositionZ());
        Spot(bot, c, 1.0f, 4.0f, out);
        return 2;
    }
    return 1;
}

Unit* BGBossRaid::Target(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Battleground* bg = bot->GetBattleground();
    if (!bg || PlayerbotAI::IsHeal(bot, true))
        return nullptr;
    std::lock_guard<std::mutex> guard(raidLock);
    Raid* r = Get(bg, bot, nullptr, nullptr);
    if (!r || r->phase == GATHER)
        return nullptr;
    Creature* boss = bg->GetBgMap()->GetCreature(r->boss);
    if (!boss || !boss->IsAlive() || bot->GetExactDist2d(boss) > 50.0f)
        return nullptr;
    auto tankIt = std::find(r->tanks.begin(), r->tanks.end(), bot->GetGUID());
    std::vector<Creature*> adds;
    for (ObjectGuid const& g : r->adds)
        if (Creature* c = bg->GetBgMap()->GetCreature(g); c && c->IsAlive())
            adds.push_back(c);
    if (tankIt != r->tanks.end())
    {
        if (tankIt == r->tanks.begin() || adds.empty())
            return boss;
        // tank 2: the guard hitting someone else (or the nearest)
        Creature* best = nullptr;
        for (Creature* c : adds)
            if (!best || (c->GetVictim() != bot && best->GetVictim() == bot) ||
                bot->GetExactDist2d(c) < bot->GetExactDist2d(best))
                best = c;
        return best;
    }
    if (r->phase != ENGAGE)
        return nullptr;
    // damage dealers: the guards first (lowest health), then the general
    Creature* best = nullptr;
    for (Creature* c : adds)
        if (!best || c->GetHealthPct() < best->GetHealthPct())
            best = c;
    return best ? best : boss;
}

Unit* BGBossRaid::HealTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return nullptr;
    std::lock_guard<std::mutex> guard(raidLock);
    Raid* r = Get(bg, bot, nullptr, nullptr);
    if (!r || r->phase == GATHER)
        return nullptr;
    auto it = r->healerOf.find(bot->GetGUID());
    if (it == r->healerOf.end())
        return nullptr;
    Player* tank = ObjectAccessor::FindPlayer(it->second);
    return tank && tank->IsAlive() ? tank : nullptr;
}
