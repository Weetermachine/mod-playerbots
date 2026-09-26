/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChooseTargetActions.h"

#include <mutex>
#include <unordered_map>

#include "BGTacticArms.h"
#include "BattleGroundTactics.h"

#include "ChooseRpgTargetAction.h"
#include "Event.h"
#include "LootObjectStack.h"
#include "NewRpgStrategy.h"
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
}  // namespace

Unit* FindFocusTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Battleground* bg = bot->GetBattleground();
    if (!bg || !BGTacticArms::IsOn(bg, bot->GetBgTeamId(), BGTactic::FocusFire) || PlayerbotAI::IsHeal(bot) ||
        !bot->IsInCombat())
        return nullptr;
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
        if (auto it = picks.find(p->GetGUID()); it != picks.end())
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

bool FocusFireAction::Execute(Event /*event*/)
{
    Unit* target = FindFocusTarget(botAI);
    if (!target || !Attack(target))
        return false;
    std::lock_guard<std::mutex> guard(focusLock);
    focusPicks[FocusKey(bot->GetBattleground(), bot->GetBgTeamId())].emplace_back(target->GetGUID(), getMSTime());
    return true;
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
