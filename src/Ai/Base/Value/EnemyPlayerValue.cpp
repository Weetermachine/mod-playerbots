/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EnemyPlayerValue.h"

#include <atomic>

#include "BGTacticArms.h"
#include "Battleground.h"
#include "PositionValue.h"

#include "CombatManager.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Vehicle.h"

bool NearestEnemyPlayersValue::AcceptUnit(Unit* unit)
{
    // Apply parent's filtering first (includes level difference checks)
    if (!PossibleTargetsValue::AcceptUnit(unit))
        return false;

    bool inCannon = botAI->IsInVehicle(false, true);
    Player* enemy = dynamic_cast<Player*>(unit);
    if (enemy && botAI->IsOpposing(enemy) && enemy->IsPvP() &&
        !sPlayerbotAIConfig.IsPvpProhibited(enemy->GetZoneId(), enemy->GetAreaId()) &&
        !enemy->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NON_ATTACKABLE_2) &&
        ((inCannon || !enemy->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE))) &&
        /*!enemy->HasStealthAura() && !enemy->HasInvisibilityAura()*/ enemy->CanSeeOrDetect(bot) &&
        !(enemy->HasSpiritOfRedemptionAura()))
    {
        // If with master, only attack if master is PvP flagged
        Player* master = botAI->GetMaster();
        if (master && !master->IsPvP() && !master->IsFFAPvP())
            return false;

        return true;
    }

    return false;
}

// Objective-only fighting (arm JobFocus): in a battleground, a bot picks a new fight (step 2 below) only with an enemy
// in its way (12 yd) or at its objective (25 yd of its "bg objective": its node, flag room, tower, or the carrier it
// escorts), not with any enemy within 40 yd it might beat (the stock kill-chasing). Fighting back (step 1) and helping
// a teammate under attack (step 3) are unchanged. Log every 5 minutes: new fights picked and skipped.
namespace
{
std::atomic<uint32> jobPicked{0}, jobSkipped{0}, jobLogMs{0};

bool JobFocusAllows(PlayerbotAI* botAI, Player* bot, Unit* target)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg || !BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::JobFocus))
        return true;
    bool allowed = bot->IsWithinDist(target, 12.0f);
    if (!allowed)
    {
        PositionInfo const obj = botAI->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get()["bg objective"];
        allowed = obj.isSet() && target->GetExactDist2d(obj.x, obj.y) < 25.0f;
    }
    ++(allowed ? jobPicked : jobSkipped);
    uint32 const now = getMSTime();
    uint32 last = jobLogMs.load();
    if (!last)
        jobLogMs.compare_exchange_strong(last, now);
    else if (getMSTimeDiff(last, now) > 5 * MINUTE * IN_MILLISECONDS && jobLogMs.compare_exchange_strong(last, now))
        LOG_INFO("module", "JobFocus (5 min): new fights picked {}, skipped (not in the way or at the objective) {}",
                 jobPicked.exchange(0), jobSkipped.exchange(0));
    return allowed;
}
}  // namespace

Unit* EnemyPlayerValue::Calculate()
{
    bool controllingCannon = false;
    bool controllingVehicle = false;
    if (Vehicle* vehicle = bot->GetVehicle())
    {
        VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
        if (!seat || !seat->CanControl())  // not in control of vehicle so cant attack anyone
            return nullptr;
        VehicleEntry const* vi = vehicle->GetVehicleInfo();
        if (vi && vi->m_flags & VEHICLE_FLAG_FIXED_POSITION)
            controllingCannon = true;
        else
            controllingVehicle = true;
    }

    // 1. Check units we are currently in PvP combat with.
    std::vector<Unit*> targets;
    Unit* pVictim = bot->GetVictim();
    for (auto const& [guid, combatRef] : bot->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* pTarget = combatRef->GetOther(bot);
        if (!pTarget || pTarget == pVictim || !pTarget->IsPlayer() || !pTarget->CanSeeOrDetect(bot) ||
            !bot->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL))
            continue;

        if ((bot->GetTeamId() == TEAM_HORDE && pTarget->HasAura(23333)) ||
            (bot->GetTeamId() == TEAM_ALLIANCE && pTarget->HasAura(23335)))
            return pTarget;

        targets.push_back(pTarget);
    }

    if (!targets.empty())
    {
        std::sort(targets.begin(), targets.end(),
                  [&](Unit const* pUnit1, Unit const* pUnit2)
                  { return bot->GetDistance(pUnit1) < bot->GetDistance(pUnit2); });

        return *targets.begin();
    }

    // 2. Find enemy player in range.

    GuidVector players = AI_VALUE(GuidVector, "nearest enemy players");
    float const maxAggroDistance = GetMaxAttackDistance();
    for (auto const& gTarget : players)
    {
        Unit* pUnit = botAI->GetUnit(gTarget);
        if (!pUnit)
            continue;

        Player* pTarget = dynamic_cast<Player*>(pUnit);
        if (!pTarget)
            continue;

        if (pTarget == pVictim)
            continue;

        if (bot->GetTeamId() == TEAM_HORDE)
        {
            if (pTarget->HasAura(23333))
                return pTarget;
        }
        else
        {
            if (pTarget->HasAura(23335))
                return pTarget;
        }

        // Aggro weak enemies from further away.
        // If controlling mobile vehicle only agro close enemies (otherwise will never reach objective)
        uint32 const aggroDistance = controllingVehicle                                               ? 5.0f
                                     : (controllingCannon || bot->GetHealth() > pTarget->GetHealth()) ? maxAggroDistance
                                                                                                      : 20.0f;
        if (!bot->IsWithinDist(pTarget, aggroDistance))
            continue;

        if (bot->IsWithinLOSInMap(pTarget) &&
            (controllingCannon || (fabs(bot->GetPositionZ() - pTarget->GetPositionZ()) < 30.0f)) &&
            JobFocusAllows(botAI, bot, pTarget))
            return pTarget;
    }

    // 3. Check party attackers.

    if (Group* pGroup = bot->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->GetSource())
            {
                if (pMember == bot)
                    continue;

                if (ServerFacade::instance().GetDistance2d(bot, pMember) > 30.0f)
                    continue;

                if (Unit* pAttacker = pMember->getAttackerForHelper())
                    if (pAttacker->IsPlayer() && bot->IsWithinDist(pAttacker, maxAggroDistance * 2.0f) &&
                        bot->IsWithinLOSInMap(pAttacker) && pAttacker != pVictim && pAttacker->CanSeeOrDetect(bot))
                        return pAttacker;
            }
        }
    }

    return nullptr;
}

float EnemyPlayerValue::GetMaxAttackDistance()
{
    if (!bot->GetBattleground())
        return 60.0f;

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return 40.0f;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    if (bgType == BATTLEGROUND_IC)
    {
        if (botAI->IsInVehicle(false, true))
            return 120.0f;
    }

    return 40.0f;
}
