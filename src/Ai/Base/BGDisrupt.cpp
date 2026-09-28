/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGDisrupt.h"

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
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
constexpr uint32 DISRUPTORS = 5;

struct Site
{
    uint32 builtMs = 0;
    ObjectGuid npc;            // the NPC being fought (our general or captain); empty: stood down
    uint32 activeUntilMs = 0;
    std::set<ObjectGuid> members;
};
std::mutex siteLock;
std::map<std::pair<uint32, uint32>, Site> sites;  // (instance, defending team)
std::atomic<uint32> statPlans{0}, statActive{0}, statGeneral{0}, statHolds{0}, statTargets[3], statLogMs{0};

bool IsAV(Battleground* bg)
{
    BattlegroundTypeId type = bg->GetBgTypeID();
    if (type == BATTLEGROUND_RB)
        type = bg->GetBgTypeID(true);
    return type == BATTLEGROUND_AV;
}

bool MeleeDps(Player* p)
{
    if (PlayerbotAI::IsHeal(p, true) || PlayerbotAI::IsTank(p, true))
        return false;
    return !PlayerbotAI::IsRanged(p, true);
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
        LOG_INFO("module", "Disrupt AV (5 min): plans {}, active {} (at the general {}), hold picks {}, targets: healers "
                 "{}, the NPC's victim {}, others {}",
                 plans, statActive.exchange(0), statGeneral.exchange(0), statHolds.exchange(0), statTargets[0].exchange(0),
                 statTargets[1].exchange(0), statTargets[2].exchange(0));
}

// Rebuild (every 3 s). Needs siteLock.
void Build(Battleground* bg, TeamId team, Site& s, uint32 now)
{
    ++statPlans;
    BattlegroundAV* av = static_cast<BattlegroundAV*>(bg);
    Creature* general = bg->GetBGCreature(team == TEAM_ALLIANCE ? AV_CPLACE_A_BOSS : AV_CPLACE_H_BOSS);
    Creature* captain = av->IsCaptainAlive(team)
                            ? bg->GetBGCreature(team == TEAM_ALLIANCE ? AV_CPLACE_TRIGGER16 : AV_CPLACE_TRIGGER18)
                            : nullptr;
    std::vector<Player*> enemies, ours;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || !p->IsInWorld() || !p->IsAlive())
            continue;
        if (p->GetTeamId() != team)
            enemies.push_back(p);
        else if (GET_PLAYERBOT_AI(p) && !p->GetVehicle() && !PlayerbotAI::IsHeal(p, true))
            ours.push_back(p);
    }
    Creature* best = nullptr;
    uint32 bestN = 0;
    for (Creature* c : {general, captain})
    {
        if (!c || !c->IsAlive())
            continue;
        uint32 n = 0;
        for (Player* e : enemies)
            n += e->GetExactDist2d(c) < 50.0f;
        if (n > bestN)
        {
            bestN = n;
            best = c;
        }
    }
    if (best && bestN >= 5)
    {
        s.npc = best->GetGUID();
        s.activeUntilMs = now + 15 * IN_MILLISECONDS;
    }
    else if (now > s.activeUntilMs)
        s.npc.Clear();
    s.members.clear();
    Creature* npc = s.npc.IsEmpty() ? nullptr : bg->GetBgMap()->GetCreature(s.npc);
    if (!npc || !npc->IsAlive())
    {
        s.npc.Clear();
        return;
    }
    ++statActive;
    if (npc == general)
        ++statGeneral;
    // melee damage dealers first (DisruptTank: rogues before them), then the others; nearest first
    bool const rogues = BGTacticArms::IsOn(bg, team, BGTactic::DisruptTank);
    std::sort(ours.begin(), ours.end(),
              [&](Player* a, Player* b)
              {
                  if (rogues)
                  {
                      bool const ra = a->getClass() == CLASS_ROGUE, rb = b->getClass() == CLASS_ROGUE;
                      if (ra != rb)
                          return ra;
                  }
                  bool const ma = MeleeDps(a), mb = MeleeDps(b);
                  if (ma != mb)
                      return ma;
                  return a->GetExactDist2d(npc) < b->GetExactDist2d(npc);
              });
    for (Player* p : ours)
    {
        if (s.members.size() >= DISRUPTORS)
            break;
        if (!PlayerbotAI::IsTank(p, true) && p->GetExactDist2d(npc) < 500.0f)
            s.members.insert(p->GetGUID());
    }
}

Creature* SiteNpc(Player* bot, Battleground* bg)
{
    if (!IsAV(bg) || bg->GetStatus() != STATUS_IN_PROGRESS || !bot->IsAlive() || bot->GetVehicle())
        return nullptr;
    TeamId const team = bot->GetTeamId();
    if (!BGTacticArms::IsOn(bg, team, BGTactic::Disrupt) && !BGTacticArms::IsOn(bg, team, BGTactic::DisruptTank))
        return nullptr;
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(siteLock);
    std::pair<uint32, uint32> const key{bg->GetInstanceID(), uint32(team)};
    if (sites.size() > 64 && !sites.count(key))
        sites.clear();  // ended games
    Site& s = sites[key];
    if (!s.builtMs || getMSTimeDiff(s.builtMs, now) > 3 * IN_MILLISECONDS)
    {
        Build(bg, team, s, now);
        s.builtMs = now;
    }
    if (s.npc.IsEmpty() || !s.members.count(bot->GetGUID()))
        return nullptr;
    return bg->GetBgMap()->GetCreature(s.npc);
}
}  // namespace

bool BGDisrupt::Hold(PlayerbotAI* botAI, Battleground* bg, Position& out)
{
    Player* bot = botAI->GetBot();
    Creature* npc = SiteNpc(bot, bg);
    if (!npc)
        return false;
    uint32 const c = bot->GetGUID().GetCounter();
    float const angle = float(c) * 2.39996f;
    float const r = 5.0f + float((c * 7919u) % 100u) / 100.0f * 8.0f;
    out.Relocate(npc->GetPositionX() + std::cos(angle) * r, npc->GetPositionY() + std::sin(angle) * r, npc->GetPositionZ());
    ++statHolds;
    MaybeLog();
    return true;
}

Unit* BGDisrupt::Target(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return nullptr;
    Creature* npc = SiteNpc(bot, bg);
    if (!npc || bot->GetExactDist2d(npc) > 60.0f)
        return nullptr;
    Unit* victim = npc->GetVictim();
    Player* healer = nullptr;
    Player* victimP = nullptr;
    Player* low = nullptr;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || !p->IsInWorld() || !p->IsAlive() || p->GetTeamId() == bot->GetTeamId() || p->GetExactDist2d(npc) > 40.0f ||
            !bot->IsWithinDistInMap(p, 40.0f) || !bot->IsWithinLOSInMap(p) || !bot->CanSeeOrDetect(p))
            continue;
        if (PlayerbotAI::IsHeal(p, true) && (!healer || bot->GetDistance(p) < bot->GetDistance(healer)))
            healer = p;
        if (p == victim)
            victimP = p;
        if (!low || p->GetHealthPct() < low->GetHealthPct())
            low = p;
    }
    // DisruptTank: their tank first (with it dead the NPC tears through the rest), then healers
    bool const tankFirst = BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::DisruptTank);
    Unit* pick = tankFirst ? (victimP ? victimP : healer ? healer : low) : (healer ? healer : victimP ? victimP : low);
    if (pick)
        ++statTargets[pick == healer ? 0 : pick == victimP ? 1 : 2];
    MaybeLog();
    return pick;
}
