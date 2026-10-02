/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "VehicleActions.h"

#include "BGStrand.h"
#include "BGIsle.h"
#include "BGTacticArms.h"
#include "BattlegroundIC.h"
#include "BattlegroundSA.h"
#include "ItemVisitors.h"
#include "ObjectDefines.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "QuestValues.h"
#include "ServerFacade.h"
#include "Unit.h"
#include "Vehicle.h"

// TODO methods to enter/exit vehicle should be added to BGTactics or MovementAction (so that we can better control
// whether bot is in vehicle, eg: get out of vehicle to cap flag, if we're down to final boss, etc),
// right now they will enter vehicle based only what's available here, then they're stuck in vehicle until they die
// (LeaveVehicleAction doesnt do much seeing as they, or another bot, will get in immediately after exit)
bool EnterVehicleAction::Execute(Event event)
{
    // do not switch vehicles yet
    if (bot->GetVehicle())
        return false;

    Player* master = botAI->GetMaster();
    // Triggered by a chat command
    if (event.getOwner() && master && master->GetTarget())
    {
        Unit* vehicleBase = botAI->GetUnit(master->GetTarget());
        if (!vehicleBase)
            return false;
        Vehicle* veh = vehicleBase->GetVehicleKit();
        if (vehicleBase->IsVehicle() && veh && veh->GetAvailableSeatCount())
        {
            return EnterVehicle(vehicleBase, false);
        }
        return false;
    }

    // SAPassengers (SotA): a passenger boards a driven demolisher nearby first
    Battleground* sabg = bot->GetMapId() == 607 ? bot->GetBattleground() : nullptr;
    if (sabg)
        if (Unit* seat = BGStrand::PassengerSeat(bot, sabg))
        {
            bool const in = EnterVehicle(seat, true);
            if (in && bot->GetVehicle())
                BGStrand::Stat("passenger_board", bot);
            return in;
        }

    GuidVector npcs = AI_VALUE(GuidVector, "nearest vehicles");
    for (GuidVector::iterator i = npcs.begin(); i != npcs.end(); i++)
    {
        Unit* vehicleBase = botAI->GetUnit(*i);
        if (!vehicleBase)
            continue;

        // SAPassengers: melee drive
        if (sabg && BGStrand::LeaveToMelee(bot, sabg, vehicleBase))
            continue;

        // ICSiegeCrew diagnostics: every Siege Engine and turret a bot on foot considers, and why it passes it by
        uint32 const diagEntry = vehicleBase->GetEntry();
        bool const diag = bot->GetMapId() == 628 && (diagEntry == NPC_SIEGE_ENGINE_A || diagEntry == NPC_SIEGE_ENGINE_H ||
                                                     diagEntry == 34777 || diagEntry == 36355 || diagEntry == 34778 ||
                                                     diagEntry == 36356);
        std::string const dk = diag ? std::string(bot->GetTeamId() == TEAM_HORDE ? "H_" : "A_") + std::to_string(diagEntry) : "";
        if (diag)
            BGIsle::CrewCount(dk + "_seen");
        if (diag && (diagEntry == NPC_SIEGE_ENGINE_A || diagEntry == NPC_SIEGE_ENGINE_H) && vehicleBase->GetVehicleKit())
            for (int8 seat : {int8(1), int8(2), int8(7)})
            {
                Unit* acc = vehicleBase->GetVehicleKit()->GetPassenger(seat);
                BGIsle::CrewCount(std::to_string(diagEntry) + "_seat" + std::to_string(seat) + "_" +
                                  (acc ? std::to_string(acc->GetEntry()) + (acc->IsVehicle() ? "" : "_novehicle") +
                                             "_faction" + std::to_string(acc->GetFaction())
                                       : std::string("empty")));
            }
        if (vehicleBase->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        {
            if (diag)
                BGIsle::CrewCount(dk + "_not_selectable");
            continue;
        }

        // dont let them get in the cannons as they'll stay forever and do nothing useful
        // dont let them in catapult they cant use them at all
        if (NPC_KEEP_CANNON == vehicleBase->GetEntry() || NPC_CATAPULT == vehicleBase->GetEntry() ||
            NPC_ANTI_PERSONNAL_CANNON == vehicleBase->GetEntry())
            continue;

        if (!vehicleBase->IsFriendlyTo(bot))
        {
            if (diag)
                BGIsle::CrewCount(dk + "_not_friendly");
            continue;
        }

        if (!vehicleBase->GetVehicleKit()->GetAvailableSeatCount())
            continue;

        // ICSiegeCrew: a Siege Engine counts as in use once a turret has a gunner, so engines whose turrets were boarded
        // first never got a driver. The driver's seat goes first; a turret is boarded only on a driven engine.
        Battleground* crewBg = bot->GetBattleground();
        bool const crew = crewBg && bot->GetMapId() == 628 && BGTacticArms::IsOn(crewBg, bot->GetTeamId(), BGTactic::ICSiegeCrew);
        uint32 const vehEntry = vehicleBase->GetEntry();
        bool const engine = vehEntry == NPC_SIEGE_ENGINE_A || vehEntry == NPC_SIEGE_ENGINE_H;
        bool const turret = vehEntry == 34777 || vehEntry == 36355 || vehEntry == 34778 || vehEntry == 36356;
        if (crew && engine)
        {
            if (vehicleBase->GetCharmerGUID().IsPlayer())
            {
                BGIsle::CrewCount(dk + "_has_driver");
                continue;  // it has its driver
            }
        }
        else if (crew && turret)
        {
            Unit* carrier = vehicleBase->GetVehicleBase();
            if (!carrier || !carrier->GetCharmerGUID().IsPlayer() || vehicleBase->GetVehicleKit()->IsVehicleInUse())
            {
                BGIsle::CrewCount(dk + (!carrier ? "_no_carrier" : !carrier->GetCharmerGUID().IsPlayer() ? "_engine_no_driver"
                                                                                                        : "_has_gunner"));
                continue;  // no driver yet, or this turret has its gunner
            }
        }
        // this will avoid adding passengers (which dont really do much for the IOC vehicles which is the only place
        // this code is used)
        else if (vehicleBase->GetVehicleKit()->IsVehicleInUse())
            continue;

        float const diagDist = bot->GetExactDist2d(vehicleBase);
        bool const entered = EnterVehicle(vehicleBase, true);
        if (diag)
            BGIsle::CrewCount(dk + (diagDist > 40.0f ? "_too_far" : diagDist > INTERACTION_DISTANCE && !bot->IsOnVehicle(vehicleBase)
                                        ? (entered ? "_walking_to" : "_walk_failed")
                                        : bot->IsOnVehicle(vehicleBase) ? "_boarded" : "_click_failed"));
        if (entered)
            return true;
    }

    return false;
}

bool EnterVehicleAction::EnterVehicle(Unit* vehicleBase, bool moveIfFar)
{
    float dist = ServerFacade::instance().GetDistance2d(bot, vehicleBase);
    if (dist > 40.0f)
        return false;

    // ICSiegeCrew: a Siege Engine's center is out of reach on foot (the model is large): board it from up to 15 yd
    Battleground* reachBg = bot->GetBattleground();
    bool const farClick = dist <= 15.0f && reachBg && bot->GetMapId() == 628 &&
                          (vehicleBase->GetEntry() == NPC_SIEGE_ENGINE_A || vehicleBase->GetEntry() == NPC_SIEGE_ENGINE_H) &&
                          BGTacticArms::IsOn(reachBg, bot->GetTeamId(), BGTactic::ICSiegeCrew);
    if (dist > INTERACTION_DISTANCE && !moveIfFar && !farClick)
        return false;

    if (dist > INTERACTION_DISTANCE && !farClick)
        return MoveTo(vehicleBase);
    // Use HandleSpellClick instead of Unit::EnterVehicle to handle special vehicle script (ulduar)
    vehicleBase->HandleSpellClick(bot);

    if (!bot->IsOnVehicle(vehicleBase))
        return false;

    // a siege position is only for the vehicle it was set for: the tactics set one for this vehicle
    PositionMap& posMap = context->GetValue<PositionMap&>("position")->Get();
    PositionInfo siegePos = posMap["bg siege"];
    siegePos.Reset();
    posMap["bg siege"] = siegePos;

    // dismount because bots can enter vehicle on mount
    WorldPacket emptyPacket;
    bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    return true;
}

bool LeaveVehicleAction::Execute(Event /*event*/)
{
    Vehicle* myVehicle = bot->GetVehicle();
    if (!myVehicle)
        return false;

    // ICStayVehicle: in Isle of Conquest the random trigger makes bots leave their siege vehicles (about 5 riding at
    // the peak, no keep breached in 100 games); stay while the game runs
    if (Battleground* bg = bot->GetBattleground())
        if (bg->GetStatus() == STATUS_IN_PROGRESS && BGTacticArms::IsOn(bg, bot->GetTeamId(), BGTactic::ICStayVehicle))
        {
            BattlegroundTypeId type = bg->GetBgTypeID();
            if (type == BATTLEGROUND_RB)
                type = bg->GetBgTypeID(true);
            if (type == BATTLEGROUND_IC)
                return false;
        }

    VehicleSeatEntry const* seat = myVehicle->GetSeatForPassenger(bot);
    if (!seat || !seat->CanEnterOrExit())
        return false;

    WorldPacket p;
    bot->GetSession()->HandleRequestVehicleExit(p);

    return true;
}
