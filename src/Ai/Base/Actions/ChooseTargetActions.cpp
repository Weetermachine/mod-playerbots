/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChooseTargetActions.h"

#include <atomic>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "BGAoeSquad.h"
#include "BGBossRaid.h"
#include "BGDisrupt.h"
#include "BGHealerGuard.h"
#include "BGStrand.h"
#include "BattlegroundIC.h"
#include "BGTacticArms.h"
#include "BattleGroundTactics.h"

#include "ChooseRpgTargetAction.h"
#include "Event.h"
#include "LootObjectStack.h"
#include "NewRpgStrategy.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "RtiTargetValue.h"
#include "PossibleRpgTargetsValue.h"
#include "PvpTriggers.h"
#include "ServerFacade.h"
#include "Spell.h"
#include "SpellInfo.h"

bool AttackEnemyPlayerAction::isUseful()
{
    if (PlayerHasFlag::IsCapturingFlag(bot))
        return false;

    return !sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId());
}

bool AttackEnemyFlagCarrierAction::isUseful()
{
    Unit* target = context->GetValue<Unit*>("enemy flag carrier")->Get();
    if (!target ||
        !ServerFacade::instance().IsDistanceLessOrEqualThan(ServerFacade::instance().GetDistance2d(bot, target), 100.0f))
        return false;

    if (PlayerHasFlag::IsCapturingFlag(bot))
        return true;

    // WSG FC chase: other bots on this team switch to the enemy FC only when they can actually reach
    // it. This runs above every heal and attack, and switching whenever the FC was near made melee
    // drop fights they were winning to run after a carrier they couldn't catch (v1 went 4-13).
    // Stock target selection already prefers the FC when a bot picks a new target.
    if ((bot->GetBattlegroundTypeId() != BATTLEGROUND_WS && bot->GetBattlegroundTypeId() != BATTLEGROUND_EY) ||
        !BGTacticArms::IsOn(bot->GetBattleground(), bot->GetBgTeamId(), BGTactic::FCChase))
        return false;

    float const dist = ServerFacade::instance().GetDistance2d(bot, target);
    if (dist > 40.0f)
        return false;

    // healers: only to finish off a nearly dead FC
    if (PlayerbotAI::IsHeal(bot))
        return target->GetHealthPct() <= 10.0f;

    // ranged: switching costs nothing when the FC is in range and sight
    if (PlayerbotAI::IsRanged(bot))
        return dist <= 35.0f && bot->IsWithinLOSInMap(target);

    // melee: finish a weak FC nearby, or go when a pull, gap closer or sprint is ready
    if (dist <= 20.0f && target->GetHealthPct() <= 35.0f)
        return true;

    return BGTactics::CanCatchEnemyFC(botAI, target);
}

bool AggressiveTargetAction::isUseful()
{
    if (bot->IsInCombat())
        return false;

    return true;
}

bool DropTargetAction::Execute(Event /*event*/)
{
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    if (target && target->isDead())
    {
        ObjectGuid guid = target->GetGUID();
        if (guid)
            context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);
    }

    // ObjectGuid pullTarget = context->GetValue<ObjectGuid>("pull target")->Get();
    // GuidVector possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();

    // if (pullTarget && find(possible.begin(), possible.end(), pullTarget) == possible.end())
    // {
    //     context->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
    // }

    context->GetValue<Unit*>("current target")->Set(nullptr);

    bot->SetTarget(ObjectGuid::Empty);
    bot->SetSelection(ObjectGuid());
    botAI->ChangeEngine(BOT_STATE_NON_COMBAT);
    if (bot->getClass() == CLASS_HUNTER) // Check for Hunter Class
    {
        Spell const* spell = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL); // Get the current spell being cast by the bot
        if (spell && spell->m_spellInfo->Id == 75) //Check spell is not nullptr before accessing m_spellInfo
            bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL); // Interrupt Auto Shot
    }
    bot->AttackStop();

    // if (Pet* pet = bot->GetPet())
    // {
    //     if (CreatureAI* creatureAI = ((Creature*)pet)->AI())
    //     {
    //         pet->SetReactState(REACT_PASSIVE);
    //         pet->GetCharmInfo()->SetCommandState(COMMAND_FOLLOW);
    //         pet->GetCharmInfo()->SetIsCommandFollow(true);
    //         pet->AttackStop();
    //         pet->GetCharmInfo()->IsReturning();
    //         pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
    //     }
    // }

    return true;
}

bool AttackAnythingAction::Execute(Event event)
{
    bool result = AttackAction::Execute(event);
    if (result)
    {
        if (Unit* grindTarget = GetTarget())
        {
            if (char const* grindName = grindTarget->GetName().c_str())
            {
                context->GetValue<ObjectGuid>("pull target")->Set(grindTarget->GetGUID());
                bot->GetMotionMaster()->Clear();
                // bot->StopMoving();
            }
        }
    }

    return result;
}

bool AttackAnythingAction::isUseful()
{
    if (!bot || !botAI)  // Prevents invalid accesses
        return false;

    if (!botAI->AllowActivity(GRIND_ACTIVITY))  // Bot cannot be active
        return false;

    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return false;

    if (bot->IsInCombat())
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsInWorld())  // Checks if the target is valid and in the world
        return false;

    std::string const name = std::string(target->GetName());
    if (!name.empty() &&
        (name.find("Dummy") != std::string::npos ||
         name.find("Charge Target") != std::string::npos ||
         name.find("Melee Target") != std::string::npos ||
         name.find("Ranged Target") != std::string::npos))
    {
        return false;
    }

    return true;
}

bool AttackAnythingAction::isPossible() { return GetTarget() && AttackAction::isPossible(); }

bool DpsAssistAction::isUseful()
{
    if (PlayerHasFlag::IsCapturingFlag(bot))
        return false;

    return true;
}

bool AttackRtiTargetAction::Execute(Event /*event*/)
{
    Unit* rtiTarget = AI_VALUE(Unit*, "rti target");

    // Fallback: if the "rti target" value did not resolve a valid unit yet,
    // try to resolve the raid icon directly from the group.
    if (!rtiTarget)
    {
        if (Group* group = bot->GetGroup())
        {
            std::string const rti = AI_VALUE(std::string, "rti");
            int32 const index = RtiTargetValue::GetRtiIndex(rti);
            if (index >= 0)
            {
                ObjectGuid const guid = group->GetTargetIcon(index);
                if (!guid.IsEmpty())
                    rtiTarget = botAI->GetUnit(guid);
            }
        }
    }

    if (rtiTarget && rtiTarget->IsInWorld() && rtiTarget->GetMapId() == bot->GetMapId())
    {
        botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({rtiTarget->GetGUID()});
        bool result = Attack(botAI->GetUnit(rtiTarget->GetGUID()));
        if (result)
        {
            context->GetValue<ObjectGuid>("pull target")->Set(rtiTarget->GetGUID());
            return true;
        }
    }
    else
        botAI->TellError("I dont see my rti attack target");

    return false;
}

bool AttackRtiTargetAction::isUseful()
{
    if (botAI->ContainsStrategy(STRATEGY_TYPE_HEAL))
        return false;

    return true;
}

// WSG escort v2 (peel). Spells that slow or stop a pursuer; unknown ones fail CanCastSpell's spellbook lookup.
static char const* const PEEL_SLOWS[] = {"hamstring", "chains of ice", "wing clip", "frost shock", "concussive shot",
                                         "kidney shot", "hammer of justice", "frost nova", "slow"};

Unit* AttackFCAttackerAction::FindAttacker()
{
    Unit* carrier = AI_VALUE(Unit*, "team flag carrier");
    if (!carrier || carrier == bot || !carrier->IsAlive() || !bot->IsWithinDistInMap(carrier, 30.0f))
        return nullptr;

    Unit* best = nullptr;
    float bestDist = 30.0f;
    Unit::AttackerSet const attackers(carrier->getAttackers());  // copy: the set changes as fights change
    for (Unit* attacker : attackers)
    {
        if (!attacker || !attacker->IsPlayer() || !attacker->IsAlive())
            continue;
        float dist = bot->GetDistance(attacker);
        if (dist < bestDist && bot->IsWithinLOSInMap(attacker))
        {
            bestDist = dist;
            best = attacker;
        }
    }
    return best;
}

bool AttackFCAttackerAction::isUseful()
{
    return (bot->GetBattlegroundTypeId() == BATTLEGROUND_WS || bot->GetBattlegroundTypeId() == BATTLEGROUND_EY) &&
           !PlayerbotAI::IsHeal(bot) &&
           BGTacticArms::IsOn(bot->GetBattleground(), bot->GetBgTeamId(), BGTactic::FCEscort2) && FindAttacker();
}

bool AttackFCAttackerAction::Execute(Event /*event*/)
{
    Unit* attacker = FindAttacker();
    if (!attacker)
        return false;

    // switch to it once (Attack returns false when already on it), then slow it unless something already does
    if (Attack(attacker))
        return true;

    if (attacker->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) || attacker->HasAuraType(SPELL_AURA_MOD_STUN) ||
        attacker->HasAuraType(SPELL_AURA_MOD_ROOT))
        return false;

    for (char const* spell : PEEL_SLOWS)
        if (botAI->CanCastSpell(spell, attacker))
            return botAI->CastSpell(spell, attacker);

    return false;
}

// AB node guards v2: interrupts and stuns that end a banner capture channel (a hit alone also ends it).
static char const* const CAPPER_INTERRUPTS[] = {"kick", "pummel", "counterspell", "wind shear", "mind freeze",
                                                "shield bash", "silence", "strangulate", "hammer of justice",
                                                "kidney shot", "cheap shot", "gouge", "war stomp", "arcane torrent",
                                                "psychic scream", "concussion blow", "frost nova"};

Unit* FindBannerCapper(PlayerbotAI* botAI, float range)
{
    Player* bot = botAI->GetBot();
    Unit* best = nullptr;
    float bestDist = range;
    for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest enemy players")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;
        Spell* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!spell || !spell->m_spellInfo || spell->m_spellInfo->Id != SPELL_CAPTURE_BANNER)
            spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!spell || !spell->m_spellInfo || spell->m_spellInfo->Id != SPELL_CAPTURE_BANNER)
            continue;
        float dist = bot->GetDistance(unit);
        if (dist < bestDist && bot->IsWithinLOSInMap(unit))
        {
            bestDist = dist;
            best = unit;
        }
    }
    return best;
}

// Focus fire (arm FocusFire): in a battleground fight, bots pick the same enemy instead of each its own. Candidates
// are living enemy players within 35 yd in sight; each is scored: the enemy flag carrier x4, a healer x2, missing
// health up to x2, nearer is better (1 / (1 + distance / 25)), melee bots discount targets beyond 8 yd, and every
// teammate who picked it in the last 3 s adds half (up to x3), which is what makes a fight converge. The current
// target keeps a 30% bonus so bots do not flip between near-equal targets. Healers keep healing.
namespace
{
std::mutex focusLock;
std::unordered_map<uint64, std::vector<std::pair<ObjectGuid, uint32>>> focusPicks;  // instance << 1 | team

uint64 FocusKey(Battleground* bg, TeamId team) { return (uint64(bg->GetInstanceID()) << 1) | uint64(team == TEAM_HORDE); }

// Focus log (per 5 minutes, full / partial): switches made; switches whose target was replaced by something else
// within 3 s (the bot's normal targeting pulling it away again); samples of bots in a fight, how many teammates
// within 40 yd share their target, and how often their target is the focus target.
struct FocusSwitch
{
    ObjectGuid target;
    uint32 ms = 0;
    bool checked = false;
};
std::unordered_map<ObjectGuid, FocusSwitch> focusSwitches;  // by bot
uint32 focusStat[2][5];  // [full, partial][switches, reverted, samples, sharing (sum), on focus]
uint32 focusLogMs = 0;

void FocusLog(uint32 now)  // needs focusLock
{
    if (!focusLogMs)
    {
        focusLogMs = now;
        return;
    }
    if (getMSTimeDiff(focusLogMs, now) <= 5 * MINUTE * IN_MILLISECONDS)
        return;
    focusLogMs = now;
    for (uint32 m = 0; m < 2; ++m)
    {
        uint32 const* v = focusStat[m];
        if (v[0] || v[2])
            LOG_INFO("module", "Focus {} (5 min): switches {}, reverted within 3 s {:.0f}%, fight samples {}, teammates on "
                     "the same target {:.2f} avg, on the focus target {:.0f}%", m ? "partial" : "full", v[0],
                     v[0] ? 100.0 * v[1] / v[0] : 0.0, v[2], v[2] ? double(v[3]) / v[2] : 0.0,
                     v[2] ? 100.0 * v[4] / v[2] : 0.0);
        for (uint32 k = 0; k < 5; ++k)
            focusStat[m][k] = 0;
    }
}
}  // namespace

// Finish kills (arm FinishKill, without focus fire): in combat, switch to the enemy with the lowest health under 30%
// within reach (8 yd melee, 30 yd ranged), unless the current target is itself under 30%. Healers don't switch.
namespace
{
std::atomic<uint32> finishSwitches{0}, finishLogMs{0};
}

Unit* FindFinishTarget(PlayerbotAI* botAI, Battleground* bg)
{
    Player* bot = botAI->GetBot();
    if (PlayerbotAI::IsHeal(bot) || !bot->IsInCombat())
        return nullptr;
    Unit* current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (current && current->IsAlive() && current->GetHealthPct() < 30.0f)
        return nullptr;
    float const reach = botAI->IsMelee(bot) ? 8.0f : 30.0f;
    Unit* best = nullptr;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || !p->IsAlive() || p->GetBgTeamId() == bot->GetBgTeamId() || p->GetHealthPct() >= 30.0f ||
            !bot->IsWithinDistInMap(p, reach) || !bot->IsWithinLOSInMap(p) || !bot->CanSeeOrDetect(p))
            continue;
        if (!best || p->GetHealthPct() < best->GetHealthPct())
            best = p;
    }
    if (best && best != current)
    {
        ++finishSwitches;
        uint32 const now = getMSTime();
        uint32 last = finishLogMs.load();
        if (!last)
            finishLogMs.compare_exchange_strong(last, now);
        else if (getMSTimeDiff(last, now) > 5 * MINUTE * IN_MILLISECONDS && finishLogMs.compare_exchange_strong(last, now))
            LOG_INFO("module", "FinishKill (5 min): switches to a low-health enemy {}", finishSwitches.exchange(0));
    }
    return best;
}

// ICHunt (IoC): the nearest enemy demolisher, siege engine or glaive thrower within 60 yd of this bot and 70 yd of
// one of its keep's gates (the siege spots are 59-65 yd out); healers keep healing
static Unit* FindICSiegeTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Battleground* bg = bot->GetBattleground();
    if (!bg || !bot->IsAlive() || bot->GetVehicle() || PlayerbotAI::IsHeal(bot) ||
        !BGTacticArms::IsOn(bg, bot->GetBgTeamId(), BGTactic::ICHunt))
        return nullptr;
    uint32 const firstGate = bot->GetBgTeamId() == TEAM_ALLIANCE ? BG_IC_GO_ALLIANCE_GATE_1 : BG_IC_GO_HORDE_GATE_1;
    GameObject* gates[3];
    for (uint32 g = 0; g < 3; ++g)
        gates[g] = bg->GetBGObject(firstGate + g);
    Unit* best = nullptr;
    float bestDist = 60.0f;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        Unit* veh = p && p->IsAlive() && p->GetBgTeamId() != bot->GetBgTeamId() ? p->GetVehicleBase() : nullptr;
        if (!veh || !veh->IsAlive() || !bot->IsValidAttackTarget(veh))
            continue;
        switch (veh->GetEntry())
        {
            case NPC_DEMOLISHER: case 35415:                           // Demolisher (1)
            case NPC_SIEGE_ENGINE_A: case NPC_SIEGE_ENGINE_H: case 35431: case 35433:  // Siege Engine (1)
            case NPC_GLAIVE_THROWER_A: case NPC_GLAIVE_THROWER_H: case 35419:  // Glaive Thrower (1)
                break;
            default:
                continue;
        }
        bool nearGate = false;
        for (GameObject* g : gates)
            nearGate = nearGate || (g && veh->GetExactDist2d(g) < 70.0f);
        if (!nearGate)
            continue;
        if (float const dist = bot->GetExactDist2d(veh); dist < bestDist)
        {
            bestDist = dist;
            best = veh;
        }
    }
    return best;
}

Unit* FindFocusTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Battleground* bg = bot->GetBattleground();
    if (Unit* demolisher = BGStrand::HuntTarget(botAI))  // SotA defenders: the enemy demolishers at their gate
        return demolisher;
    if (Unit* siege = FindICSiegeTarget(botAI))  // IoC: enemy siege vehicles at our keep
        return siege;
    if (Unit* raid = BGBossRaid::Target(botAI))  // raid boss (AV): tanks on the general/guards, then guards, general
        return raid;
    if (Unit* hit = BGDisrupt::Target(botAI))  // disruption (AV): the enemies fighting our general or captain
        return hit;
    if (Unit* onHealer = BGHealerGuard::Target(botAI))  // healer bodyguard: whoever is on our healer
        return onHealer;
    if (Unit* pack = BGAoeSquad::Target(botAI))  // AoE squad (AV/IoC): the densest enemy pack in range
        return pack;
    if (bg && BGTacticArms::IsOn(bg, bot->GetBgTeamId(), BGTactic::FinishKill) &&
        !BGTacticArms::IsOn(bg, bot->GetBgTeamId(), BGTactic::FocusFire) &&
        !BGTacticArms::IsOn(bg, bot->GetBgTeamId(), BGTactic::FocusPartial))
        return FindFinishTarget(botAI, bg);
    bool const partial = bg && BGTacticArms::IsOn(bg, bot->GetBgTeamId(), BGTactic::FocusPartial);
    if (!bg || !(partial || BGTacticArms::IsOn(bg, bot->GetBgTeamId(), BGTactic::FocusFire)) || PlayerbotAI::IsHeal(bot) ||
        !bot->IsInCombat())
        return nullptr;
    // Partial focus: how many of our other bots are on each enemy right now (their current target)
    std::unordered_map<ObjectGuid, uint32> attackers;
    if (partial)
        for (auto const& ref : bg->GetBgMap()->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || p == bot || !p->IsAlive() || p->GetBgTeamId() != bot->GetBgTeamId())
                continue;
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                if (Unit* t = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get())
                    ++attackers[t->GetGUID()];
        }
    uint32 const now = getMSTime();
    std::unordered_map<ObjectGuid, uint32> picks;
    {
        std::lock_guard<std::mutex> guard(focusLock);
        auto& v = focusPicks[FocusKey(bg, bot->GetBgTeamId())];
        v.erase(std::remove_if(v.begin(), v.end(), [&](auto const& p) { return getMSTimeDiff(p.second, now) > 3000; }), v.end());
        for (auto const& p : v)
            ++picks[p.first];
    }
    Unit* current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    Unit* enemyFC = botAI->GetAiObjectContext()->GetValue<Unit*>("enemy flag carrier")->Get();
    bool const melee = botAI->IsMelee(bot);
    Unit* best = nullptr;
    float bestScore = 0.0f;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || !p->IsAlive() || p->GetBgTeamId() == bot->GetBgTeamId() || !bot->IsWithinDistInMap(p, 35.0f) ||
            !bot->IsWithinLOSInMap(p) || !bot->CanSeeOrDetect(p))
            continue;
        float const dist = bot->GetDistance(p);
        float score = (p == enemyFC ? 4.0f : 1.0f) * (PlayerbotAI::IsHeal(p) ? 2.0f : 1.0f) *
                      (2.0f - p->GetHealthPct() / 100.0f) / (1.0f + dist / 25.0f);
        if (melee && dist > 8.0f)
            score *= 0.6f;
        uint32 const on = partial ? (attackers.count(p->GetGUID()) ? attackers[p->GetGUID()] : 0) : 0;
        if (partial && on >= 3 && p != current)
            score *= 0.25f;  // already has its 3: take the next-best target
        else if (auto it = picks.find(p->GetGUID()); it != picks.end())
            score *= std::min(3.0f, 1.0f + 0.5f * it->second);
        if (p == current)
            score *= 1.3f;
        if (score > bestScore)
        {
            bestScore = score;
            best = p;
        }
    }
    return best;
}

bool FocusFireAction::isUseful()
{
    Unit* target = FindFocusTarget(botAI);
    return target && target != AI_VALUE(Unit*, "current target");
}

// SAHunt / ICHunt diagnostics: vehicle focus picks and attacks that went through, per 5 minutes (Server.log)
namespace
{
std::atomic<uint32> huntPicks{0}, huntAttacks{0}, huntLogMs{0};
void HuntCount(Unit* target, bool attacked)
{
    if (!target || !target->IsVehicle())
        return;
    ++huntPicks;
    huntAttacks += attacked ? 1 : 0;
    uint32 const now = getMSTime();
    uint32 last = huntLogMs.load();
    if (!last)
        huntLogMs = now;
    else if (getMSTimeDiff(last, now) > 5 * MINUTE * IN_MILLISECONDS && huntLogMs.compare_exchange_strong(last, now))
        LOG_INFO("module", "Hunt (5 min): vehicle focus picks {}, attacks started {}", huntPicks.exchange(0),
                 huntAttacks.exchange(0));
}
}

// Hunt diagnostics: why a vehicle focus pick was refused (Attack's checks, in its order), logged with the counter
namespace
{
std::mutex refusalLock;
std::map<std::string, uint32> refusals;
uint32 refusalLogMs = 0;
void HuntRefusal(PlayerbotAI* botAI, Player* bot, Unit* target)
{
    std::string why = !target->IsInWorld() ? "not in world"
        : bot->IsFriendlyTo(target) ? "friendly"
        : target->isDead() ? "dead"
        : !bot->IsWithinLOSInMap(target) ? "no LOS"
        : botAI->IsInVehicle() ? "bot in vehicle"
        : bot->GetVictim() == target ? "already attacking"
        : !bot->IsValidAttackTarget(target) ? "invalid target"
        : "other";
    float const dist = bot->GetExactDist(target);
    why += std::string(botAI->IsMelee(bot) ? " melee" : " ranged") + (dist < 10 ? " <10yd" : dist < 30 ? " 10-30yd" : dist < 45 ? " 30-45yd" : " 45+yd");
    std::lock_guard<std::mutex> guard(refusalLock);
    ++refusals[why];
    uint32 const now = getMSTime();
    if (!refusalLogMs)
        refusalLogMs = now;
    else if (getMSTimeDiff(refusalLogMs, now) > 5 * MINUTE * IN_MILLISECONDS)
    {
        std::ostringstream out;
        for (auto const& [k, n] : refusals)
            out << " [" << k << "]=" << n;
        LOG_INFO("module", "Hunt refusals (5 min):{}", out.str());
        refusals.clear();
        refusalLogMs = now;
    }
}
}

bool FocusFireAction::Execute(Event /*event*/)
{
    Unit* target = FindFocusTarget(botAI);
    bool const attacked = target && Attack(target);
    HuntCount(target, attacked);
    if (target && target->IsVehicle() && !attacked)
        HuntRefusal(botAI, bot, target);
    if (!attacked)
        return false;
    uint32 const now = getMSTime();
    uint32 const m = BGTacticArms::IsOn(bot->GetBattleground(), bot->GetBgTeamId(), BGTactic::FocusPartial) ? 1 : 0;
    std::lock_guard<std::mutex> guard(focusLock);
    focusPicks[FocusKey(bot->GetBattleground(), bot->GetBgTeamId())].emplace_back(target->GetGUID(), now);
    focusSwitches[bot->GetGUID()] = {target->GetGUID(), now, false};
    ++focusStat[m][0];
    return true;
}

// Focus log sample (called from the trigger, for bots in a fight with focus fire on)
void FocusSample(PlayerbotAI* botAI, Unit* focus)
{
    Player* bot = botAI->GetBot();
    Battleground* bg = bot->GetBattleground();
    Unit* current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    uint32 sharing = 0;
    if (current)
        for (auto const& ref : bg->GetBgMap()->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || p == bot || !p->IsAlive() || p->GetBgTeamId() != bot->GetBgTeamId() || bot->GetExactDist2d(p) > 40.0f)
                continue;
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                sharing += ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get() == current;
        }
    uint32 const now = getMSTime();
    uint32 const m = BGTacticArms::IsOn(bg, bot->GetBgTeamId(), BGTactic::FocusPartial) ? 1 : 0;
    std::lock_guard<std::mutex> guard(focusLock);
    ++focusStat[m][2];
    focusStat[m][3] += sharing;
    focusStat[m][4] += current && current == focus;
    auto it = focusSwitches.find(bot->GetGUID());
    if (it != focusSwitches.end() && !it->second.checked && getMSTimeDiff(it->second.ms, now) >= 1000)
    {
        uint32 const age = getMSTimeDiff(it->second.ms, now);
        bool const still = current && current->GetGUID() == it->second.target;
        bool const targetDead = !ObjectAccessor::GetUnit(*bot, it->second.target) ||
                                !ObjectAccessor::GetUnit(*bot, it->second.target)->IsAlive();
        if (!still && !targetDead && age <= 3000)
        {
            ++focusStat[m][1];  // pulled away while the focus target lived
            it->second.checked = true;
        }
        else if (age > 3000)
            it->second.checked = true;
    }
    if (focusSwitches.size() > 5000)
        focusSwitches.clear();
    FocusLog(now);
}

// Healer hunt (arm HealerHunt, WSG): the enemy carrier's healer (an enemy healer within 40 yd of it, nearest to this
// bot within 35 yd) gets up to 2 of our bots: an interrupt or stun first, then attacks. Bots that can reach the
// carrier keep attacking it ("attack enemy flag carrier" is above this).
Unit* FindCarrierHealer(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Battleground* bg = bot->GetBattleground();
    if (!bg || !BGTacticArms::IsOn(bg, bot->GetBgTeamId(), BGTactic::HealerHunt) || PlayerbotAI::IsHeal(bot))
        return nullptr;
    Unit* fc = botAI->GetAiObjectContext()->GetValue<Unit*>("enemy flag carrier")->Get();
    if (!fc || !fc->IsAlive() || fc == bot)
        return nullptr;
    Player* best = nullptr;
    float bestDist = 35.0f;
    std::unordered_map<ObjectGuid, uint32> on;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || !p->IsAlive() || p == bot)
            continue;
        if (p->GetBgTeamId() == bot->GetBgTeamId())
        {
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                if (Unit* t = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get())
                    ++on[t->GetGUID()];
            continue;
        }
        if (p == fc || !PlayerbotAI::IsHeal(p) || p->GetDistance(fc) > 40.0f)
            continue;
        float const d = bot->GetDistance(p);
        if (d < bestDist && bot->IsWithinLOSInMap(p))
        {
            bestDist = d;
            best = p;
        }
    }
    if (!best)
        return nullptr;
    Unit* current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    uint32 const already = on.count(best->GetGUID()) ? on[best->GetGUID()] : 0;
    return current == best || already < 2 ? best : nullptr;
}

bool AttackCarrierHealerAction::isUseful()
{
    Unit* healer = FindCarrierHealer(botAI);
    return healer && healer != AI_VALUE(Unit*, "current target");
}

bool AttackCarrierHealerAction::Execute(Event /*event*/)
{
    Unit* healer = FindCarrierHealer(botAI);
    if (!healer)
        return false;
    if (healer->IsNonMeleeSpellCast(false))
        for (char const* spell : CAPPER_INTERRUPTS)
            if (botAI->CanCastSpell(spell, healer))
                return botAI->CastSpell(spell, healer);
    return Attack(healer);
}

bool AttackBannerCapperAction::isUseful()
{
    return bot->GetBattlegroundTypeId() == BATTLEGROUND_AB &&
           BGTacticArms::IsOn(bot->GetBattleground(), bot->GetBgTeamId(), BGTactic::NodeGuard2) &&
           FindBannerCapper(botAI, 30.0f);
}

bool AttackBannerCapperAction::Execute(Event /*event*/)
{
    Unit* capper = FindBannerCapper(botAI, 30.0f);
    if (!capper)
        return false;

    // an interrupt/stun first if one is ready, else switch to it (the first hit ends the channel);
    // healers only use the interrupt, they keep their own target
    for (char const* spell : CAPPER_INTERRUPTS)
        if (botAI->CanCastSpell(spell, capper))
            return botAI->CastSpell(spell, capper);

    return !PlayerbotAI::IsHeal(bot) && Attack(capper);
}
