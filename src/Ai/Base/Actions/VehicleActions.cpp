/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "VehicleActions.h"

#include "BGStrand.h"
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

        if (vehicleBase->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
            continue;

        // dont let them get in the cannons as they'll stay forever and do nothing useful
        // dont let them in catapult they cant use them at all
        if (NPC_KEEP_CANNON == vehicleBase->GetEntry() || NPC_CATAPULT == vehicleBase->GetEntry() ||
            NPC_ANTI_PERSONNAL_CANNON == vehicleBase->GetEntry())
            continue;

        if (!vehicleBase->IsFriendlyTo(bot))
            continue;

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
                continue;  // it has its driver
        }
        else if (crew && turret)
        {
            Unit* carrier = vehicleBase->GetVehicleBase();
            if (!carrier || !carrier->GetCharmerGUID().IsPlayer() || vehicleBase->GetVehicleKit()->IsVehicleInUse())
                continue;  // no driver yet, or this turret has its gunner
        }
        // this will avoid adding passengers (which dont really do much for the IOC vehicles which is the only place
        // this code is used)
        else if (vehicleBase->GetVehicleKit()->IsVehicleInUse())
            continue;

        if (EnterVehicle(vehicleBase, true))
            return true;
    }

    return false;
}

bool EnterVehicleAction::EnterVehicle(Unit* vehicleBase, bool moveIfFar)
{
    float dist = ServerFacade::instance().GetDistance2d(bot, vehicleBase);
    if (dist > 40.0f)
        return false;

    if (dist > INTERACTION_DISTANCE && !moveIfFar)
        return false;

    if (dist > INTERACTION_DISTANCE)
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
