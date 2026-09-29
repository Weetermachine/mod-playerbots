/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AttackersValue.h"

#include <algorithm>

#include "BGTacticArms.h"
#include "Battleground.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Playerbots.h"
#include "ReputationMgr.h"
#include "ServerFacade.h"

GuidVector AttackersValue::Calculate()
{
    std::unordered_set<Unit*> targets;

    GuidVector result;
    if (!botAI->AllowActivity(ALL_ACTIVITY))
        return result;

    AddAttackersOf(bot, targets);

    if (Group* group = bot->GetGroup())
        AddAttackersOf(group, targets);

    RemoveNonThreating(targets);

    // prioritized target
    GuidVector prioritizedTargets = AI_VALUE(GuidVector, "prioritized targets");
    for (ObjectGuid target : prioritizedTargets)
    {
        Unit* unit = botAI->GetUnit(target);
        if (unit && IsValidTarget(unit, bot))
            targets.insert(unit);
    }
    if (Group* group = bot->GetGroup())
    {
        ObjectGuid skullGuid = group->GetTargetIcon(7);
        Unit* skullTarget = botAI->GetUnit(skullGuid);
        if (skullTarget && IsValidTarget(skullTarget, bot))
            targets.insert(skullTarget);
    }

    for (Unit* unit : targets)
        result.push_back(unit->GetGUID());

    if (bot->duel && bot->duel->Opponent)
        result.push_back(bot->duel->Opponent->GetGUID());

    // PvPAttackers (BG arm): threat lists hold only NPCs, so enemy players never became "attackers" and every rule
    // built on them (enemy healer interrupts, snares, CC, multi-DoT, dps/tank assist) was dead in battlegrounds.
    // With the arm, valid enemy players among the possible targets join, as in arenas.
    if (Battleground* bg = bot->GetBattleground())
        if (bg->GetStatus() == STATUS_IN_PROGRESS && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::PvPAttackers))
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible targets");
            for (ObjectGuid const guid : possibleTargets)
            {
                Unit* unit = botAI->GetUnit(guid);
                if (unit && unit->IsPlayer() && IsValidTarget(unit, bot) &&
                    std::find(result.begin(), result.end(), guid) == result.end())
                    result.push_back(guid);
            }
        }

    // workaround for bots of same faction not fighting in arena
    if (bot->InArena())
    {
        GuidVector possibleTargets = AI_VALUE(GuidVector, "possible targets");
        for (ObjectGuid const guid : possibleTargets)
        {
            Unit* unit = botAI->GetUnit(guid);
            if (unit && unit->IsPlayer() && IsValidTarget(unit, bot))
                result.push_back(unit->GetGUID());
        }
    }

    return result;
}

void AttackersValue::AddAttackersOf(Group* group, std::unordered_set<Unit*>& targets)
{
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member || !member->IsAlive() || member == bot || member->GetMapId() != bot->GetMapId() ||
            ServerFacade::instance().GetDistance2d(bot, member) > sPlayerbotAIConfig.sightDistance)
            continue;

        AddAttackersOf(member, targets);
    }
}

struct AddGuardiansHelper
{
    explicit AddGuardiansHelper(std::vector<Unit*>& units) : units(units) {}

    void operator()(Unit* target) const { units.push_back(target); }

    std::vector<Unit*>& units;
};

void AttackersValue::AddAttackersOf(Player* player, std::unordered_set<Unit*>& targets)
{
    if (!player || !player->IsInWorld() || player->IsBeingTeleported())
        return;

    for (auto const& [guid, ref] : player->GetThreatMgr().GetThreatenedByMeList())
    {
        Unit* attacker = ref->GetOwner();
        if (!attacker)
            continue;

        if (player->IsValidAttackTarget(attacker) &&
            player->GetDistance2d(attacker) < sPlayerbotAIConfig.sightDistance)
            targets.insert(attacker);
    }
}

void AttackersValue::RemoveNonThreating(std::unordered_set<Unit*>& targets)
{
    for (std::unordered_set<Unit*>::iterator tIter = targets.begin(); tIter != targets.end();)
    {
        Unit* unit = *tIter;
        if (bot->GetMapId() != unit->GetMapId() || !hasRealThreat(unit) || !IsValidTarget(unit, bot))
        {
            std::unordered_set<Unit*>::iterator tIter2 = tIter;
            ++tIter;
            targets.erase(tIter2);
        }
        else
            ++tIter;
    }
}

bool AttackersValue::hasRealThreat(Unit* attacker)
{
    return attacker && attacker->IsInWorld() && attacker->IsAlive() && !attacker->IsPolymorphed() &&
           // !attacker->isInRoots() &&
           !attacker->IsFriendlyTo(bot);
}

bool AttackersValue::IsPossibleTarget(Unit* attacker, Player* bot, float /*range*/)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    // Basic check
    if (!attacker)
        return false;

    // bool inCannon = botAI->IsInVehicle(false, true);
    // bool enemy = botAI->GetAiObjectContext()->GetValue<Unit*>("enemy player target")->Get();

    // Validity checks
    if (!attacker->IsVisible() || !attacker->IsInWorld() || attacker->GetMapId() != bot->GetMapId())
        return false;

    if (attacker->isDead() || attacker->HasSpiritOfRedemptionAura())
        return false;

    // Flag checks
    if (attacker->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NON_ATTACKABLE_2))
        return false;

    if (attacker->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC) || attacker->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        return false;

    // Skip targets that are immune to all damage (e.g., Ice Block, Divine Shield)
    if (attacker->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) &&
        attacker->IsImmunedToDamage(SPELL_SCHOOL_MASK_MAGIC))
        return false;

    // Relationship checks
    if (attacker->IsFriendlyTo(bot))
        return false;

    // Critter exception
    if (attacker->GetCreatureType() == CREATURE_TYPE_CRITTER && !attacker->IsInCombat())
        return false;

    // Visibility check
    if (!bot->CanSeeOrDetect(attacker))
        return false;

    // PvP prohibition checks (skip for duels)
    if ((attacker->GetGUID().IsPlayer() || attacker->GetGUID().IsPet()) &&
        (!bot->duel || bot->duel->Opponent != attacker) &&
        (sPlayerbotAIConfig.IsPvpProhibited(attacker->GetZoneId(), attacker->GetAreaId()) ||
        sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId())))
    {
        // This will stop aggresive pets from starting an attack.
        // This will stop currently attacking pets from continuing their attack.
        // This will first require the bot to change from a combat strat. It will
        // not be reached if the bot only switches targets, including NPC targets.
        for (Unit::ControlSet::const_iterator itr = bot->m_Controlled.begin();
            itr != bot->m_Controlled.end(); ++itr)
        {
            Creature* creature = dynamic_cast<Creature*>(*itr);
            if (creature && creature->GetVictim() == attacker)
            {
                creature->AttackStop();
                if (CharmInfo* charmInfo = creature->GetCharmInfo())
                    charmInfo->SetIsCommandAttack(false);
            }
        }

        return false;
    }

    // Unflagged player check
    if (attacker->IsPlayer() && !attacker->IsPvP() && !attacker->IsFFAPvP() &&
        (!bot->duel || bot->duel->Opponent != attacker))
        return false;

    // Creature-specific checks
    Creature* c = attacker->ToCreature();
    // SAHunt / ICHunt: enemy siege vehicles are fair game (the tap rule below, for world mobs, dropped every one of
    // them: a driven vehicle always has one of our players as its victim)
    bool const huntVehicle = c && c->IsVehicle() && bot->InBattleground() && bot->GetBattleground() &&
                             (BGTacticArms::IsOn(bot->GetBattleground(), bot->GetBgTeamId(), BGTactic::SAHunt) ||
                              BGTacticArms::IsOn(bot->GetBattleground(), bot->GetBgTeamId(), BGTactic::ICHunt) ||
                              BGTacticArms::IsOn(bot->GetBattleground(), bot->GetBgTeamId(), BGTactic::VehicleTargetFix));
    if (c && !huntVehicle)
    {
        if (c->IsInEvadeMode())
            return false;

        bool leaderHasThreat = false;
        if (bot->GetGroup() && botAI->GetMaster())
            leaderHasThreat = attacker->GetThreatMgr().GetThreat(botAI->GetMaster());

        bool isMemberBotGroup = false;
        if (bot->GetGroup() && botAI->GetMaster())
        {
            PlayerbotAI* masterBotAI = GET_PLAYERBOT_AI(botAI->GetMaster());
            if (masterBotAI && !masterBotAI->IsRealPlayer())
                isMemberBotGroup = true;
        }

        bool canAttack = (!isMemberBotGroup && botAI->HasStrategy("attack tagged", BOT_STATE_NON_COMBAT)) ||
            leaderHasThreat ||
            (!c->hasLootRecipient() &&
                (!c->GetVictim() ||
                    (c->GetVictim() &&
                        ((!c->GetVictim()->IsPlayer() || bot->IsInSameGroupWith(c->GetVictim()->ToPlayer())) ||
                            (botAI->GetMaster() && c->GetVictim() == botAI->GetMaster()))))) ||
            c->isTappedBy(bot);

        if (!canAttack)
            return false;
    }

    return true;
}

bool AttackersValue::IsValidTarget(Unit* attacker, Player* bot)
{
    return IsPossibleTarget(attacker, bot) && bot->IsWithinLOSInMap(attacker);
}

bool PossibleAddsValue::Calculate()
{
    GuidVector possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();

    for (ObjectGuid const guid : possible)
    {
        if (find(attackers.begin(), attackers.end(), guid) != attackers.end())
            continue;
        Unit* add = botAI->GetUnit(guid);
        if (!add || !add->IsInWorld() || add->IsDuringRemoveFromWorld())
            continue;

        if (!add->GetTarget() && !add->GetThreatMgr().GetLastVictim() && add->IsHostileTo(bot))
        {
            for (ObjectGuid const attackerGUID : attackers)
            {
                Unit* attacker = botAI->GetUnit(attackerGUID);
                if (!attacker)
                    continue;

                float dist = ServerFacade::instance().GetDistance2d(attacker, add);
                if (ServerFacade::instance().IsDistanceLessOrEqualThan(dist, sPlayerbotAIConfig.aoeRadius * 1.5f))
                    continue;

                if (ServerFacade::instance().IsDistanceLessOrEqualThan(dist, sPlayerbotAIConfig.aggroDistance))
                    return true;
            }
        }
    }

    return false;
}
