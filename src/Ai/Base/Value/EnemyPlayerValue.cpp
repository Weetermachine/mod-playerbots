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

// StealthFix: the stock checks asked whether the ENEMY can see the bot (so a stealthed bot dropped every target); the
// bot's own view of the enemy is what matters
static bool StealthFixOn(Player* bot)
{
    Battleground* bg = bot->GetBattleground();
    return bg && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::StealthFix);
}

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
        /*!enemy->HasStealthAura() && !enemy->HasInvisibilityAura()*/ (StealthFixOn(bot) ? bot->CanSeeOrDetect(enemy) : enemy->CanSeeOrDetect(bot)) &&
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
        PositionInfo obj = botAI->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get()["bg objective"];
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

// No solo fights (arm NoSolo): in a battleground, a bot with no teammate within 20 yd does not pick a new fight
// (step 2) with an enemy that has another enemy within 15 yd; it goes on with its job. Fighting back (step 1) is
// unchanged. Log every 5 minutes: new fights skipped.
namespace
{
std::atomic<uint32> soloSkipped{0}, soloPicked{0}, soloLogMs{0};

bool NoSoloAllows(Player* bot, Unit* target)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg || !BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::NoSolo))
        return true;
    bool friendNear = false;
    uint32 enemies = 0;
    for (auto const& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || p == bot || !p->IsAlive())
            continue;
        if (p->GetTeamId() == bot->GetTeamId())
            friendNear |= bot->GetExactDist2d(p) < 20.0f;
        else
            enemies += target->GetExactDist2d(p) < 15.0f;
    }
    bool const allowed = friendNear || enemies < 2;  // enemies counts the target itself
    ++(allowed ? soloPicked : soloSkipped);
    uint32 const now = getMSTime();
    uint32 last = soloLogMs.load();
    if (!last)
        soloLogMs.compare_exchange_strong(last, now);
    else if (getMSTimeDiff(last, now) > 5 * MINUTE * IN_MILLISECONDS && soloLogMs.compare_exchange_strong(last, now))
        LOG_INFO("module", "NoSolo (5 min): new fights picked {}, skipped (alone against 2+) {}", soloPicked.exchange(0),
                 soloSkipped.exchange(0));
    return allowed;
}
}  // namespace

Unit* EnemyPlayerValue::Calculate()
{
    bool controllingCannon = false;
    bool controllingVehicle = false;
    bool passenger = false;
    if (Vehicle* vehicle = bot->GetVehicle())
    {
        VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
        if (!seat || (!seat->CanControl() && !(seat->m_flags & VEHICLE_SEAT_FLAG_CAN_ATTACK)))
            return nullptr;  // a passenger seat that can't attack
        VehicleEntry const* vi = vehicle->GetVehicleInfo();
        if (!seat->CanControl())
            passenger = true;  // fights from its seat: it can't close in
        else if (vi && vi->m_flags & VEHICLE_FLAG_FIXED_POSITION)
            controllingCannon = true;
        else
            controllingVehicle = true;
        // ICTurretAim: a Siege Engine turret's seat counts as driving a mobile vehicle (5 yd); it can't move, so it
        // picks enemies as a cannon does
        uint32 const baseEntry = vehicle->GetBase()->GetEntry();
        if (controllingVehicle && bot->GetMapId() == 628 && bot->GetBattleground() &&
            (baseEntry == 34777 || baseEntry == 36355 || baseEntry == 34778 || baseEntry == 36356) &&
            BGTacticArms::IsOn(bot->GetBattleground(), bot->GetTeamId(), BGTactic::ICTurretAim))
        {
            controllingVehicle = false;
            controllingCannon = true;
        }
    }

    // 1. Check units we are currently in PvP combat with.
    std::vector<Unit*> targets;
    Unit* pVictim = bot->GetVictim();
    for (auto const& [guid, combatRef] : bot->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* pTarget = combatRef->GetOther(bot);
        if (!pTarget || pTarget == pVictim || !pTarget->IsPlayer() ||
            (StealthFixOn(bot) ? !bot->CanSeeOrDetect(pTarget) : !pTarget->CanSeeOrDetect(bot)) ||
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
        uint32 const aggroDistance = passenger                                                        ? 30.0f
                                     : controllingVehicle                                             ? 5.0f
                                     : (controllingCannon || bot->GetHealth() > pTarget->GetHealth()) ? maxAggroDistance
                                                                                                      : 20.0f;
        if (!bot->IsWithinDist(pTarget, aggroDistance))
            continue;

        if (bot->IsWithinLOSInMap(pTarget) &&
            (controllingCannon || (fabs(bot->GetPositionZ() - pTarget->GetPositionZ()) < 30.0f)) &&
            JobFocusAllows(botAI, bot, pTarget) && NoSoloAllows(bot, pTarget))
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
                        bot->IsWithinLOSInMap(pAttacker) && pAttacker != pVictim &&
                        (StealthFixOn(bot) ? bot->CanSeeOrDetect(pAttacker) : pAttacker->CanSeeOrDetect(bot)))
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
