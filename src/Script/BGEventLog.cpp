/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Battleground roster event log, for diagnosing uneven teams and mid-game departures.
// Enabled with AiPlayerbot.BGEventLog = 1. Writes one key=value line per event to the
// "playerbots.bgevents" logger; route it to its own file in worldserver.conf, e.g.
//   Appender.BGEvents=2,4,1,BGEvents.log,a      (flags 1 = timestamp prefix)
//   Logger.playerbots.bgevents=4,BGEvents

#include <algorithm>
#include <map>
#include <mutex>
#include <cmath>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

#include "Battleground.h"
#include "BattlegroundAB.h"
#include "BattlegroundAV.h"
#include "BattlegroundEY.h"
#include "BattlegroundIC.h"
#include "BattlegroundSA.h"
#include "BattlegroundWS.h"
#include "BGStrand.h"
#include "BGTacticArms.h"
#include "Config.h"
#include "Creature.h"
#include "Map.h"
#include "Log.h"
#include "Player.h"
#include "Transport.h"
#include "Vehicle.h"
#include "PlayerScript.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "GameObject.h"
#include "SpellInfo.h"
#include "Timer.h"

namespace
{
bool BGEventLogEnabled() { return sConfigMgr->GetOption<bool>("AiPlayerbot.BGEventLog", false); }

char const* StatusName(BattlegroundStatus status)
{
    switch (status)
    {
        case STATUS_WAIT_QUEUE:
            return "wait_queue";
        case STATUS_WAIT_JOIN:
            return "wait_join";
        case STATUS_IN_PROGRESS:
            return "in_progress";
        case STATUS_WAIT_LEAVE:
            return "wait_leave";
        default:
            return "none";
    }
}

char const* PlayerKind(Player* player)
{
    if (!GET_PLAYERBOT_AI(player))
        return "human";
    return sRandomPlayerbotMgr.IsRandomBot(player) ? "rndbot" : "altbot";
}

char const* TeamName(TeamId team) { return team == TEAM_ALLIANCE ? "A" : team == TEAM_HORDE ? "H" : "N"; }

// " last_action=<name> last_action_ago_ms=<n>" for bots, empty for humans.
std::string LastAction(Player* player)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI || botAI->lastBGAction.empty())
        return "";
    return " last_action=\"" + botAI->lastBGAction +
           "\" last_action_ago_ms=" + std::to_string(GetMSTimeDiffToNow(botAI->lastBGActionTime));
}

void LogBG(char const* event, Battleground* bg)
{
    std::string const arm = BGTacticArms::ArmName(bg);
    LOG_INFO("playerbots.bgevents", "event={} bg={} type={} name=\"{}\" levels={}-{} status={} bg_ms={} A={} H={}{}",
             event, bg->GetInstanceID(), uint32(bg->GetBgTypeID()), bg->GetName(), uint32(bg->GetMinLevel()),
             uint32(bg->GetMaxLevel()), StatusName(bg->GetStatus()), bg->GetStartTime(),
             bg->GetPlayersCountByTeam(TEAM_ALLIANCE), bg->GetPlayersCountByTeam(TEAM_HORDE),
             arm.empty() ? "" : " arm=" + arm);
}

void LogPlayer(char const* event, Battleground* bg, Player* player, std::string const& extra = "")
{
    LOG_INFO("playerbots.bgevents",
             "event={} bg={} type={} status={} bg_ms={} guid={} name={} kind={} team={} level={} alive={} A={} H={}{}",
             event, bg->GetInstanceID(), uint32(bg->GetBgTypeID()), StatusName(bg->GetStatus()), bg->GetStartTime(),
             player->GetGUID().GetCounter(), player->GetName(), PlayerKind(player), TeamName(player->GetBgTeamId()),
             uint32(player->GetLevel()), player->IsAlive() ? 1 : 0, bg->GetPlayersCountByTeam(TEAM_ALLIANCE),
             bg->GetPlayersCountByTeam(TEAM_HORDE), extra);
}
// Last logged WSG objective state per BG instance. BG updates run on their map's thread, so
// different instances can update concurrently; the lock only guards this small map.
struct WSGState
{
    // Indexed by the flag's owning team: flagState[TEAM_ALLIANCE] is the Alliance flag, and
    // keeper[TEAM_ALLIANCE] is the (Horde) player carrying it. Matches BattlegroundWS internals.
    uint8 flagState[2] = {0, 0};  // BG_WS_FLAG_STATE_*
    uint32 keeper[2] = {0, 0};    // carrier's low guid, 0 if none
    uint32 score[2] = {0, 0};
    bool operator!=(WSGState const& o) const
    {
        return flagState[0] != o.flagState[0] || flagState[1] != o.flagState[1] || keeper[0] != o.keeper[0] ||
               keeper[1] != o.keeper[1] || score[0] != o.score[0] || score[1] != o.score[1];
    }
};
std::mutex wsgStateLock;
std::unordered_map<uint32, WSGState> wsgStates;

char const* WSFlagStateName(uint8 state)
{
    switch (state)
    {
        case BG_WS_FLAG_STATE_ON_BASE:
            return "base";
        case BG_WS_FLAG_STATE_ON_PLAYER:
            return "carried";
        case BG_WS_FLAG_STATE_ON_GROUND:
            return "ground";
        default:
            return "wait";  // respawning after a capture
    }
}
}  // namespace

class PlayerbotsBGEventLogScript : public BGScript
{
public:
    PlayerbotsBGEventLogScript()
        : BGScript("PlayerbotsBGEventLogScript",
                   {ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_START, ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_ADD_PLAYER,
                    ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_REMOVE_PLAYER_AT_LEAVE, ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END,
                    ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_UPDATE, ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_DESTROY})
    {
    }

    // Objective events. WSG: a "wsg" line whenever either flag's state, either carrier, or the score
    // changes (pickups, drops, returns, captures). Other BGs: an "obj" line whenever a node, flag or
    // carrier changes, plus a score heartbeat every 30 s. Only compares a few fields per tick.
    void OnBattlegroundUpdate(Battleground* bg, uint32 /*diff*/) override
    {
        // SotA: its warmups (boats sailing) are logged too
        bool const saWarmup = bg->GetBgTypeID() == BATTLEGROUND_SA && bg->GetStatus() == STATUS_WAIT_JOIN;
        if ((bg->GetStatus() != STATUS_IN_PROGRESS && !saWarmup) || !BGEventLogEnabled())
            return;
        if (bg->GetBgTypeID() != BATTLEGROUND_WS)
        {
            LogObjectives(bg);
            LogPositions(bg);
            return;
        }
        BattlegroundWS* ws = dynamic_cast<BattlegroundWS*>(bg);
        if (!ws)
            return;

        WSGState now;
        for (TeamId team : {TEAM_ALLIANCE, TEAM_HORDE})
        {
            now.flagState[team] = ws->GetFlagState(team);
            now.keeper[team] = ws->GetFlagPickerGUID(team).GetCounter();
            now.score[team] = ws->GetTeamScore(team);
        }

        {
            std::lock_guard<std::mutex> guard(wsgStateLock);
            WSGState& last = wsgStates[bg->GetInstanceID()];
            if (!(now != last))
                return;
            last = now;
        }

        LOG_INFO("playerbots.bgevents",
                 "event=wsg bg={} type={} bg_ms={} a_flag={} h_flag={} a_keeper={} h_keeper={} score_a={} score_h={} "
                 "A={} H={}",
                 bg->GetInstanceID(), uint32(bg->GetBgTypeID()), bg->GetStartTime(), WSFlagStateName(now.flagState[0]),
                 WSFlagStateName(now.flagState[1]), now.keeper[0], now.keeper[1], now.score[0], now.score[1],
                 bg->GetPlayersCountByTeam(TEAM_ALLIANCE), bg->GetPlayersCountByTeam(TEAM_HORDE));
    }

    void OnBattlegroundDestroy(Battleground* bg) override
    {
        std::lock_guard<std::mutex> guard(wsgStateLock);
        wsgStates.erase(bg->GetInstanceID());
        objStates.erase(bg->GetInstanceID());
    }

private:
    struct ObjState
    {
        std::string objectives;  // node/flag/carrier part; any change is logged at once
        int32 score[2] = {-1, -1};
        uint32 lastLogMs = 0;
        uint32 lastSampleMs = 0;     // AV generals / IoC vehicles sampling
        std::string lastSample;      // last logged sample (without time fields)
        uint32 lastSampleLogMs = 0;
        uint32 lastPosMs = 0;        // AV/IoC position snapshot
        ObjectGuid general[2];       // AV: Vanndar (Alliance), Drek'Thar (Horde)
    };

    static constexpr uint32 SAMPLE_MS = 5000;
    static constexpr uint32 SAMPLE_HEARTBEAT_MS = 60000;
    static constexpr float GENERAL_RANGE = 50.0f;
    static constexpr uint32 AV_GENERAL_ENTRY[2] = {11948, 11946};  // Vanndar Stormpike, Drek'Thar

    // Final objective state at game end (the deciding capture happens after the last update).
    static void LogFinalState(Battleground* bg)
    {
        if (BattlegroundWS* ws = dynamic_cast<BattlegroundWS*>(bg))
        {
            LOG_INFO("playerbots.bgevents",
                     "event=wsg bg={} type={} bg_ms={} a_flag={} h_flag={} a_keeper={} h_keeper={} score_a={} "
                     "score_h={} A={} H={} final=1",
                     bg->GetInstanceID(), uint32(bg->GetBgTypeID()), bg->GetStartTime(),
                     WSFlagStateName(ws->GetFlagState(TEAM_ALLIANCE)), WSFlagStateName(ws->GetFlagState(TEAM_HORDE)),
                     ws->GetFlagPickerGUID(TEAM_ALLIANCE).GetCounter(), ws->GetFlagPickerGUID(TEAM_HORDE).GetCounter(),
                     ws->GetTeamScore(TEAM_ALLIANCE), ws->GetTeamScore(TEAM_HORDE),
                     bg->GetPlayersCountByTeam(TEAM_ALLIANCE), bg->GetPlayersCountByTeam(TEAM_HORDE));
            return;
        }
        std::string objectives;
        int32 score[2] = {0, 0};
        if (!DescribeObjectives(bg, objectives, score))
            return;
        LOG_INFO("playerbots.bgevents", "event=obj bg={} type={} bg_ms={} score_a={} score_h={}{} A={} H={} final=1",
                 bg->GetInstanceID(), uint32(bg->GetBgTypeID()), bg->GetStartTime(), score[0], score[1], objectives,
                 bg->GetPlayersCountByTeam(TEAM_ALLIANCE), bg->GetPlayersCountByTeam(TEAM_HORDE));
    }

    // AV: each general's health, combat state, and enemy/own players within GENERAL_RANGE.
    // Shows when each side first reaches the enemy general and whether anyone defends their own.
    static std::string SampleAVGenerals(Battleground* bg, ObjState& state)
    {
        Map* map = bg->GetBgMap();
        if (!map)
            return "";
        Creature* gen[2] = {nullptr, nullptr};
        for (int side = 0; side < 2; ++side)
        {
            if (!state.general[side].IsEmpty())
                gen[side] = map->GetCreature(state.general[side]);
            if (!gen[side])
                for (ObjectGuid const& guid : bg->BgCreatures)
                    if (Creature* c = guid.IsEmpty() ? nullptr : map->GetCreature(guid))
                        if (c->GetEntry() == AV_GENERAL_ENTRY[side])
                        {
                            gen[side] = c;
                            state.general[side] = guid;
                            break;
                        }
        }
        uint32 attackers[2] = {0, 0}, defenders[2] = {0, 0}, tanks[2] = {0, 0};  // tanks: tank-spec attackers in range
        for (auto const& [guid, player] : bg->GetPlayers())
        {
            if (!player || !player->IsAlive() || player->GetMapId() != bg->GetMapId())
                continue;
            TeamId team = player->GetBgTeamId();
            for (int side = 0; side < 2; ++side)
                if (gen[side] && gen[side]->IsAlive() && player->IsWithinDist(gen[side], GENERAL_RANGE))
                {
                    (team == TeamId(side) ? defenders : attackers)[side]++;
                    if (team != TeamId(side) && PlayerbotAI::IsTank(player, true))
                        tanks[side]++;
                }
        }
        std::ostringstream s;
        for (int side = 0; side < 2; ++side)
        {
            char const* k = side == 0 ? "a" : "h";
            uint32 hp = gen[side] && gen[side]->IsAlive() ? uint32(gen[side]->GetHealthPct() + 0.5f) : 0;
            s << " gen_" << k << "_hp=" << hp << " gen_" << k << "_combat=" << (gen[side] && gen[side]->IsInCombat() ? 1 : 0)
              << " gen_" << k << "_attackers=" << attackers[side] << " gen_" << k << "_defenders=" << defenders[side]
              << " gen_" << k << "_tanks=" << tanks[side];
            // who the general is hitting: class id and 1 if a tank spec (0 class = nobody)
            Unit* v = gen[side] && gen[side]->IsAlive() && gen[side]->IsInCombat() ? gen[side]->GetVictim() : nullptr;
            Player* vp = v ? v->ToPlayer() : nullptr;
            s << " gen_" << k << "_victim_class=" << (vp ? uint32(vp->getClass()) : 0) << " gen_" << k
              << "_victim_tank=" << (vp && PlayerbotAI::IsTank(vp, true) ? 1 : 0);
        }
        return s.str();
    }

    // IoC: players riding vehicles, per faction, by vehicle entry.
    static std::string SampleICVehicles(Battleground* bg)
    {
        std::map<uint32, uint32> byEntry[2];
        uint32 riding[2] = {0, 0};
        std::string hp[2];  // health % of each driven vehicle
        for (auto const& [guid, player] : bg->GetPlayers())
        {
            if (!player || player->GetMapId() != bg->GetMapId())
                continue;
            if (Unit* base = player->GetVehicleBase())
            {
                int side = player->GetBgTeamId() == TEAM_ALLIANCE ? 0 : 1;
                riding[side]++;
                byEntry[side][base->GetEntry()]++;
                if (Vehicle* kit = base->GetVehicleKit(); kit && kit->GetSeatForPassenger(player) &&
                                                          kit->GetSeatForPassenger(player)->CanControl())
                    hp[side] += (hp[side].empty() ? "" : ",") + std::to_string(uint32(base->GetHealthPct()));
            }
        }
        std::ostringstream s;
        for (int side = 0; side < 2; ++side)
        {
            char const* k = side == 0 ? "a" : "h";
            // gate health (building hit points; 0 = destroyed), left/right/front for the Alliance, front/left/right for the Horde
            s << ' ' << k << "_gates=";
            for (int g = 0; g < 3; ++g)
            {
                uint32 const idx = (side == 0 ? BG_IC_GO_ALLIANCE_GATE_1 : BG_IC_GO_HORDE_GATE_1) + g;
                GameObject* go = bg->GetBGObject(idx);
                s << (g ? "," : "") << (go ? go->GetGOValue()->Building.Health : 0);
            }
            s << ' ' << k << "_vhp=" << (hp[side].empty() ? "-" : hp[side]);
            s << ' ' << k << "_riding=" << riding[side] << ' ' << k << "_vehicles=";
            if (byEntry[side].empty())
                s << '-';
            bool first = true;
            for (auto const& [entry, n] : byEntry[side])
            {
                s << (first ? "" : ",") << entry << 'x' << n;
                first = false;
            }
        }
        return s.str();
    }

    // SotA: the attacking team, gate health (Green, Yellow, Blue, Red, Purple, Chamber) and each boat's path progress /
    // pause time / GO state, to see whether gates take damage and when the boats reach the dock
    static std::string SampleSA(Battleground* bg)
    {
        std::ostringstream s;
        TeamId const att = BGStrand::Attackers(bg);
        s << " attackers=" << (att == TEAM_ALLIANCE ? 'A' : att == TEAM_HORDE ? 'H' : 'N') << " gates=";
        for (uint32 g = BG_SA_GREEN_GATE; g <= BG_SA_ANCIENT_GATE; ++g)
        {
            GameObject* go = bg->GetBGObject(g);
            s << (g != BG_SA_GREEN_GATE ? "," : "") << (go ? go->GetGOValue()->Building.Health : 0);
        }
        std::string demo;
        for (uint32 i = BG_SA_DEMOLISHER_1; i <= BG_SA_DEMOLISHER_8; ++i)
            if (Creature* d = bg->GetBGCreature(i); d && d->IsAlive() && d->IsVisible())
                demo += (demo.empty() ? "" : ",") + std::to_string(uint32(d->GetHealthPct()));
        s << " demo=" << (demo.empty() ? "-" : demo);
        std::string drv;  // attacker-driven siege (workshop demolishers are not in the slots above)
        std::vector<Unit*> seen;
        for (auto const& ref : bg->GetBgMap()->GetPlayers())
            if (Player* p = ref.GetSource(); p && p->IsAlive())
                if (Unit* v = p->GetVehicleBase(); v && v->IsAlive() && v->GetEntry() != NPC_ANTI_PERSONNAL_CANNON &&
                                                   std::find(seen.begin(), seen.end(), v) == seen.end())
                {
                    seen.push_back(v);
                    drv += (drv.empty() ? "" : ",") + std::to_string(uint32(v->GetHealthPct()));
                }
        s << " drv=" << (drv.empty() ? "-" : drv);
        for (uint32 b : {BG_SA_BOAT_ONE, BG_SA_BOAT_TWO})
        {
            GameObject* go = bg->GetBGObject(b);
            StaticTransport* t = go ? go->ToStaticTransport() : nullptr;
            s << " boat" << (b - BG_SA_BOAT_ONE + 1) << '=';
            if (t)
                s << t->GetPathProgress() << '/' << t->GetPauseTime() << '/' << uint32(t->GetGoState()) << '@'
                  << int32(t->GetPositionX()) << ',' << int32(t->GetPositionY()) << 'p' << t->GetPassengers().size();
            else
                s << '-';
        }
        return s.str();
    }

    // Position snapshot (AV/IoC), every POS_MS: one line per game, every player as guid,team,x,y,state
    // (state: a alive, d dead, v driving/riding a vehicle), to see where bots spend their time (e.g. idle guards at a
    // base nobody attacks, which never show up in death positions).
    static constexpr uint32 POS_MS = 30000;
    void LogPositions(Battleground* bg)
    {
        BattlegroundTypeId const type = bg->GetBgTypeID();
        if (type != BATTLEGROUND_AV && type != BATTLEGROUND_IC && type != BATTLEGROUND_SA)
            return;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> guard(wsgStateLock);
            ObjState& st = objStates[bg->GetInstanceID()];
            if (st.lastPosMs && getMSTimeDiff(st.lastPosMs, now) < POS_MS)
                return;
            st.lastPosMs = now;
        }
        std::ostringstream pts;
        bool first = true;
        for (auto const& ref : bg->GetBgMap()->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || !p->IsInWorld())
                continue;
            char const state = !p->IsAlive() ? 'd' : p->GetVehicle() ? 'v' : p->GetTransport() ? 'b' : 'a';
            pts << (first ? "" : ";") << p->GetGUID().GetCounter() << ',' << (p->GetTeamId() == TEAM_HORDE ? 'H' : 'A')
                << ',' << int32(p->GetPositionX()) << ',' << int32(p->GetPositionY()) << ',' << state;
            first = false;
        }
        LOG_INFO("playerbots.bgevents", "event=pos bg={} type={} bg_ms={} pts={}", bg->GetInstanceID(), uint32(type),
                 bg->GetStartTime(), pts.str());
    }

    // Periodic samples for AV/IoC: logged when changed, or every SAMPLE_HEARTBEAT_MS.
    void LogSamples(Battleground* bg, uint32 now)
    {
        char const* event = nullptr;
        BattlegroundTypeId type = bg->GetBgTypeID();
        if (type == BATTLEGROUND_AV)
            event = "avgen";
        else if (type == BATTLEGROUND_IC)
            event = "veh";
        else if (type == BATTLEGROUND_SA)
            event = "sa";
        else
            return;

        std::string sample;
        {
            std::lock_guard<std::mutex> guard(wsgStateLock);
            ObjState& st = objStates[bg->GetInstanceID()];
            if (st.lastSampleMs && getMSTimeDiff(st.lastSampleMs, now) < SAMPLE_MS)
                return;
            st.lastSampleMs = now;
            sample = type == BATTLEGROUND_AV ? SampleAVGenerals(bg, st)
                   : type == BATTLEGROUND_IC ? SampleICVehicles(bg) : SampleSA(bg);
            if (sample.empty())
                return;
            bool changed = sample != st.lastSample;
            if (!changed && st.lastSampleLogMs && getMSTimeDiff(st.lastSampleLogMs, now) < SAMPLE_HEARTBEAT_MS)
                return;
            st.lastSample = sample;
            st.lastSampleLogMs = now;
        }
        LOG_INFO("playerbots.bgevents", "event={} bg={} type={} bg_ms={}{} A={} H={}", event, bg->GetInstanceID(),
                 uint32(type), bg->GetStartTime(), sample, bg->GetPlayersCountByTeam(TEAM_ALLIANCE),
                 bg->GetPlayersCountByTeam(TEAM_HORDE));
    }
    std::unordered_map<uint32, ObjState> objStates;  // guarded by wsgStateLock

    static constexpr uint32 SCORE_HEARTBEAT_MS = 30000;

    static char Owner(TeamId team) { return team == TEAM_ALLIANCE ? 'A' : team == TEAM_HORDE ? 'H' : 'N'; }

    // Builds " key=value ..." for the BG's objectives and fills score[] (resources/reinforcements).
    // Returns false for BG types without objective logging.
    static bool DescribeObjectives(Battleground* bg, std::string& out, int32 score[2])
    {
        std::ostringstream s;
        switch (bg->GetBgTypeID())
        {
            case BATTLEGROUND_AB:
            {
                BattlegroundAB* ab = dynamic_cast<BattlegroundAB*>(bg);
                if (!ab)
                    return false;
                // Node state: N neutral, A/H occupied, a/h assaulted (contested) by that faction.
                static char const* names[BG_AB_DYNAMIC_NODES_COUNT] = {"stables", "blacksmith", "farm", "lumbermill",
                                                                        "goldmine"};
                static char const codes[] = {'N', 'A', 'H', 'a', 'h'};
                for (uint32 i = 0; i < BG_AB_DYNAMIC_NODES_COUNT; ++i)
                {
                    uint8 st = ab->GetCapturePointInfo(i)._state;
                    s << ' ' << names[i] << '=' << (st < sizeof(codes) ? codes[st] : '?');
                }
                score[0] = bg->GetTeamScore(TEAM_ALLIANCE);
                score[1] = bg->GetTeamScore(TEAM_HORDE);
                break;
            }
            case BATTLEGROUND_EY:
            {
                BattlegroundEY* ey = dynamic_cast<BattlegroundEY*>(bg);
                if (!ey)
                    return false;
                static char const* names[EY_POINTS_MAX] = {"felreaver", "bloodelf", "draenei", "magetower"};
                for (uint32 i = 0; i < EY_POINTS_MAX; ++i)
                    s << ' ' << names[i] << '=' << Owner(ey->GetCapturePointInfo(i)._ownerTeamId);
                uint8 fs = ey->GetFlagState();
                s << " flag="
                  << (fs == BG_EY_FLAG_STATE_ON_BASE     ? "base"
                      : fs == BG_EY_FLAG_STATE_ON_PLAYER ? "carried"
                      : fs == BG_EY_FLAG_STATE_ON_GROUND ? "ground"
                                                         : "wait")
                  << " keeper=" << ey->GetFlagPickerGUID().GetCounter();
                score[0] = bg->GetTeamScore(TEAM_ALLIANCE);
                score[1] = bg->GetTeamScore(TEAM_HORDE);
                break;
            }
            case BATTLEGROUND_AV:
            {
                BattlegroundAV* av = dynamic_cast<BattlegroundAV*>(bg);
                if (!av)
                    return false;
                // Node: owner letter + state digit (0 neutral, 1 assaulted, 2 destroyed, 3 controlled).
                static char const* names[BG_AV_NODES_MAX] = {"aidstation", "stormpikegy", "stoneheartgy", "snowfallgy",
                                                              "icebloodgy", "frostwolfgy", "frostwolfhut", "dbsouth",
                                                              "dbnorth", "icewing", "stoneheart", "icebloodtower",
                                                              "towerpoint", "fwEast", "fwWest"};
                for (uint32 i = 0; i < BG_AV_NODES_MAX; ++i)
                {
                    BG_AV_NodeInfo const& n = av->GetAVNodeInfo(i);
                    s << ' ' << names[i] << '=' << Owner(n.OwnerId) << uint32(n.State);
                }
                s << " captain_a=" << (av->IsCaptainAlive(TEAM_ALLIANCE) ? 1 : 0)
                  << " captain_h=" << (av->IsCaptainAlive(TEAM_HORDE) ? 1 : 0)
                  << " mine_n=" << Owner(av->GetMineOwner(0)) << " mine_s=" << Owner(av->GetMineOwner(1));
                score[0] = av->GetTeamReinforcements(TEAM_ALLIANCE);
                score[1] = av->GetTeamReinforcements(TEAM_HORDE);
                break;
            }
            case BATTLEGROUND_IC:
            {
                BattlegroundIC* ic = dynamic_cast<BattlegroundIC*>(bg);
                if (!ic)
                    return false;
                // Node: N uncontrolled, a/h in conflict (being taken by), A/H controlled.
                static char const* names[MAX_NODE_TYPES] = {"refinery", "quarry", "docks", "hangar", "workshop",
                                                             "gy_alliance", "gy_horde"};
                static char const codes[] = {'N', 'a', 'h', 'A', 'H'};
                for (uint8 i = 0; i < MAX_NODE_TYPES; ++i)
                {
                    uint32 st = ic->GetNodeState(i);
                    s << ' ' << names[i] << '=' << (st < sizeof(codes) ? codes[st] : '?');
                }
                score[0] = ic->GetTeamReinforcements(TEAM_ALLIANCE);
                score[1] = ic->GetTeamReinforcements(TEAM_HORDE);
                break;
            }
            default:
                return false;
        }
        out = s.str();
        return true;
    }

    void LogObjectives(Battleground* bg)
    {
        LogSamples(bg, getMSTime());

        std::string objectives;
        int32 score[2] = {0, 0};
        if (!DescribeObjectives(bg, objectives, score))
            return;

        uint32 now = getMSTime();
        {
            std::lock_guard<std::mutex> guard(wsgStateLock);
            ObjState& last = objStates[bg->GetInstanceID()];
            bool changed = objectives != last.objectives;
            bool scoreDue = (score[0] != last.score[0] || score[1] != last.score[1]) &&
                            getMSTimeDiff(last.lastLogMs, now) >= SCORE_HEARTBEAT_MS;
            if (!changed && !scoreDue && last.lastLogMs != 0)
                return;
            last.objectives = objectives;
            last.score[0] = score[0];
            last.score[1] = score[1];
            last.lastLogMs = now;
        }

        LOG_INFO("playerbots.bgevents", "event=obj bg={} type={} bg_ms={} score_a={} score_h={}{} A={} H={}",
                 bg->GetInstanceID(), uint32(bg->GetBgTypeID()), bg->GetStartTime(), score[0], score[1], objectives,
                 bg->GetPlayersCountByTeam(TEAM_ALLIANCE), bg->GetPlayersCountByTeam(TEAM_HORDE));
    }

public:
    void OnBattlegroundStart(Battleground* bg) override
    {
        if (BGEventLogEnabled())
            LogBG("start", bg);
    }

    void OnBattlegroundAddPlayer(Battleground* bg, Player* player) override
    {
        if (BGEventLogEnabled())
            LogPlayer("join", bg, player);
    }

    // Fires for every departure, including the normal exodus after the game ends
    // (status=wait_leave). Departures with status=in_progress are the interesting ones.
    void OnBattlegroundRemovePlayerAtLeave(Battleground* bg, Player* player) override
    {
        if (BGEventLogEnabled())
            LogPlayer("leave", bg, player, LastAction(player));
    }

    void OnBattlegroundEnd(Battleground* bg, TeamId winnerTeam) override
    {
        if (!BGEventLogEnabled())
            return;
        // The deciding capture/score change happens after the last in-progress update, so write
        // the final objective state here (final=1) before the end line.
        LogFinalState(bg);
        LOG_INFO("playerbots.bgevents", "event=end bg={} type={} bg_ms={} winner={} A={} H={}",
                     bg->GetInstanceID(), uint32(bg->GetBgTypeID()), bg->GetStartTime(), TeamName(winnerTeam),
                     bg->GetPlayersCountByTeam(TEAM_ALLIANCE), bg->GetPlayersCountByTeam(TEAM_HORDE));
    }
};

// Level changes, logouts and teleports off the BG map: catches bots being reset (e.g. by
// mod-player-bot-reset), logged out, or teleported out (which removes them from the BG) mid-game.
// SotA gate damage by source (Server.log, per 5 min): charge blasts, boulders, rams, other; the attacker's distance
class PlayerbotsSAGateDamageScript : public AllGameObjectScript
{
public:
    PlayerbotsSAGateDamageScript() : AllGameObjectScript("PlayerbotsSAGateDamageScript") {}

    void OnGameObjectModifyHealth(GameObject* go, Unit* attacker, int32& change, SpellInfo const* spellInfo) override
    {
        if (change >= 0 || !go || go->GetMapId() != 607 || go->GetGoType() != GAMEOBJECT_TYPE_DESTRUCTIBLE_BUILDING)
            return;
        uint32 const spell = spellInfo ? spellInfo->Id : 0;
        uint32 const src = spell == 52408 ? 0 : spell == 52339 ? 1 : spell == 60206 ? 2 : 3;  // charge, boulder, ram, other
        float const dist = attacker ? attacker->GetExactDist2d(go) : -1.0f;
        uint32 const band = dist < 0.0f ? 4 : dist < 30.0f ? 0 : dist < 70.0f ? 1 : dist < 120.0f ? 2 : 3;
        std::lock_guard<std::mutex> guard(lock);
        dmg[src] += uint64(-change);
        hits[src][band]++;
        if (src == 3)
            other[spell]++;
        uint32 const now = getMSTime();
        if (!logMs)
            logMs = now;
        else if (getMSTimeDiff(logMs, now) > 5 * MINUTE * IN_MILLISECONDS)
        {
            std::ostringstream o;
            char const* names[4] = {"charges", "boulders", "rams", "other"};
            for (uint32 i = 0; i < 4; ++i)
                o << ' ' << names[i] << ' ' << dmg[i] << " (hits by attacker distance <30/30-70/70-120/120+/none: " << hits[i][0]
                  << '/' << hits[i][1] << '/' << hits[i][2] << '/' << hits[i][3] << '/' << hits[i][4] << ')';
            for (auto const& [id, n] : other)
                o << " spell" << id << "x" << n;
            LOG_INFO("module", "SA gate damage (5 min):{}", o.str());
            std::fill(std::begin(dmg), std::end(dmg), 0);
            for (auto& h : hits)
                std::fill(std::begin(h), std::end(h), 0);
            other.clear();
            logMs = now;
        }
    }

private:
    std::mutex lock;
    uint64 dmg[4] = {};
    uint32 hits[4][5] = {};
    std::map<uint32, uint32> other;
    uint32 logMs = 0;
};

// SotA damage to driven siege vehicles (Server.log, per 5 min): hits, distinct hitters, damage as a share of max health,
// kills, and how far the vehicle was from the nearest gate when hit
class PlayerbotsSASiegeDamageScript : public UnitScript
{
public:
    PlayerbotsSASiegeDamageScript() : UnitScript("PlayerbotsSASiegeDamageScript") {}

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!Driven(victim) || !attacker || damage == 0)
            return;
        Player* p = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!p)
            return;
        uint32 const k = attacker->IsVehicle() ? 1 : 0;  // 0: players on foot, 1: from a cannon
        // on foot: 0 melee, 1 ranged from the ground, 2 ranged from above (5+ yd higher than the vehicle)
        uint32 const r = PlayerbotAI::IsRanged(p) ? (p->GetPositionZ() > victim->GetPositionZ() + 5.0f ? 2 : 1) : 0;
        std::lock_guard<std::mutex> guard(lock);
        if (!k)
        {
            ++roleHits[r];
            roleDmg[r] += damage;
        }
        ++hits;
        ++srcHits[k];
        srcDmg[k] += damage;
        dmg += damage;
        maxHp = victim->GetMaxHealth();
        lastDriven[victim->GetGUID().GetCounter()] = getMSTime();
        hitters.insert(p->GetGUID().GetCounter());
        victims.insert(victim->GetGUID().GetCounter());
        Log();
    }

    // the driver is out by the time it dies: a vehicle counts if it was hit while driven in the last 10 s
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (!unit || unit->GetMapId() != 607 || !unit->IsVehicle())
            return;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const it = lastDriven.find(unit->GetGUID().GetCounter());
            if (it == lastDriven.end() || getMSTimeDiff(it->second, getMSTime()) > 10000)
                return;
            lastDriven.erase(it);
            ++kills;
            Log();
        }
        BGStrand::SiegeKill(unit);
    }

private:
    // a siege vehicle someone drives on SotA (not the defenders' cannons)
    static bool Driven(Unit* u)
    {
        return u && u->GetMapId() == 607 && u->IsVehicle() && u->GetEntry() != 27894 && u->GetVehicleKit() &&
               u->GetVehicleKit()->IsVehicleInUse();
    }

    void Log()
    {
        uint32 const now = getMSTime();
        if (!logMs)
            logMs = now;
        else if (getMSTimeDiff(logMs, now) > 5 * MINUTE * IN_MILLISECONDS)
        {
            LOG_INFO("module", "SA siege damage (5 min): hits {} by {} players on {} vehicles, damage {} ({:.1f} vehicle max healths of {}), kills {} | on foot hits {} damage {} | cannons hits {} damage {}",
                     hits, hitters.size(), victims.size(), dmg, maxHp ? double(dmg) / maxHp : 0.0, maxHp, kills,
                     srcHits[0], srcDmg[0], srcHits[1], srcDmg[1]);
            LOG_INFO("module", "SA siege damage on foot (5 min): melee hits {} damage {} | ranged ground hits {} damage {} | ranged above hits {} damage {}",
                     roleHits[0], roleDmg[0], roleHits[1], roleDmg[1], roleHits[2], roleDmg[2]);
            roleHits[0] = roleHits[1] = roleHits[2] = 0;
            roleDmg[0] = roleDmg[1] = roleDmg[2] = 0;
            hits = kills = srcHits[0] = srcHits[1] = 0;
            dmg = srcDmg[0] = srcDmg[1] = 0;
            for (auto it = lastDriven.begin(); it != lastDriven.end();)
                it = getMSTimeDiff(it->second, now) > 60000 ? lastDriven.erase(it) : std::next(it);
            hitters.clear();
            victims.clear();
            logMs = now;
        }
    }

    std::mutex lock;
    uint32 hits = 0, kills = 0, maxHp = 0, logMs = 0, srcHits[2] = {}, roleHits[3] = {};
    uint64 dmg = 0, srcDmg[2] = {}, roleDmg[3] = {};
    std::set<ObjectGuid::LowType> hitters, victims;
    std::unordered_map<ObjectGuid::LowType, uint32> lastDriven;
};

// SotA: snares and roots that land on driven siege vehicles (Server.log, per 5 min, by spell)
class PlayerbotsSASlowAuraScript : public UnitScript
{
public:
    PlayerbotsSASlowAuraScript() : UnitScript("PlayerbotsSASlowAuraScript") {}

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!unit || !aura || unit->GetMapId() != 607 || !unit->IsVehicle() || unit->GetEntry() == 27894 ||
            !unit->GetVehicleKit() || !unit->GetVehicleKit()->IsVehicleInUse())
            return;
        SpellInfo const* info = aura->GetSpellInfo();
        if (!info || !(info->HasAura(SPELL_AURA_MOD_DECREASE_SPEED) || info->HasAura(SPELL_AURA_MOD_ROOT) ||
                       info->HasAura(SPELL_AURA_MOD_STUN)))
            return;
        std::lock_guard<std::mutex> guard(lock);
        ++applied[info->SpellName[0]];
        uint32 const now = getMSTime();
        if (!logMs)
            logMs = now;
        else if (getMSTimeDiff(logMs, now) > 5 * MINUTE * IN_MILLISECONDS)
        {
            std::ostringstream o;
            for (auto const& [name, n] : applied)
                o << ' ' << name << ' ' << n;
            LOG_INFO("module", "SA siege slowed (5 min, landed):{}", o.str());
            applied.clear();
            logMs = now;
        }
    }

private:
    std::mutex lock;
    std::map<std::string, uint32> applied;
    uint32 logMs = 0;
};

// SotA anti-personnel cannon damage (Server.log, per 5 min): hits, damage and kills on players and on vehicles
class PlayerbotsSACannonDamageScript : public UnitScript
{
public:
    PlayerbotsSACannonDamageScript() : UnitScript("PlayerbotsSACannonDamageScript") {}

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!attacker || !victim || damage == 0 || victim->GetMapId() != 607 || attacker->GetEntry() != 27894)
            return;
        uint32 const k = victim->IsVehicle() ? 1 : 0;  // 0: players (and others on foot), 1: vehicles
        std::lock_guard<std::mutex> guard(lock);
        ++hits[k];
        dmg[k] += damage;
        if (damage >= victim->GetHealth())
            ++kills[k];
        uint32 const now = getMSTime();
        if (!logMs)
            logMs = now;
        else if (getMSTimeDiff(logMs, now) > 5 * MINUTE * IN_MILLISECONDS)
        {
            LOG_INFO("module", "SA cannon damage (5 min): on foot hits {} damage {} kills {} | vehicles hits {} damage {} kills {}",
                     hits[0], dmg[0], kills[0], hits[1], dmg[1], kills[1]);
            hits[0] = hits[1] = kills[0] = kills[1] = 0;
            dmg[0] = dmg[1] = 0;
            logMs = now;
        }
    }

private:
    std::mutex lock;
    uint32 hits[2] = {}, kills[2] = {}, logMs = 0;
    uint64 dmg[2] = {};
};

// SotA demolisher passengers (Server.log, per 5 min): what bots in a demolisher's passenger seats do, by distinct riders
class PlayerbotsSAPassengerScript : public UnitScript
{
public:
    PlayerbotsSAPassengerScript() : UnitScript("PlayerbotsSAPassengerScript") {}

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        Player* p = Rider(attacker);
        if (!p || !victim || damage == 0)
            return;
        uint32 const k = victim->IsVehicle() ? 1 : 0;  // 0: on foot, 1: vehicles
        std::lock_guard<std::mutex> guard(lock);
        ++hits[k];
        dmg[k] += damage;
        kills[k] += damage >= victim->GetHealth() ? 1 : 0;
        riders.insert(p->GetGUID().GetCounter());
        Log();
    }

    void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override
    {
        Player* p = Rider(healer);
        if (!p || !receiver || gain == 0)
            return;
        uint32 const k = receiver->IsVehicle() ? 1 : 0;
        std::lock_guard<std::mutex> guard(lock);
        ++heals[k];
        healed[k] += gain;
        healers.insert(p->GetGUID().GetCounter());
        Log();
    }

private:
    // a player in a SotA demolisher's passenger seat (not the driver)
    static Player* Rider(Unit* u)
    {
        Player* p = u ? u->ToPlayer() : nullptr;
        if (!p || p->GetMapId() != 607)
            return nullptr;
        Vehicle* v = p->GetVehicle();
        if (!v || !v->GetBase() || (v->GetBase()->GetEntry() != 28781 && v->GetBase()->GetEntry() != 32796))
            return nullptr;
        VehicleSeatEntry const* seat = v->GetSeatForPassenger(p);
        return seat && !seat->CanControl() ? p : nullptr;
    }

    void Log()
    {
        uint32 const now = getMSTime();
        if (!logMs)
            logMs = now;
        else if (getMSTimeDiff(logMs, now) > 5 * MINUTE * IN_MILLISECONDS)
        {
            LOG_INFO("module", "SA passengers (5 min): damage by {} riders: on foot hits {} damage {} kills {} | vehicles hits {} damage {} kills {} || healing by {} riders: others {} ({}) | vehicles {} ({})",
                     riders.size(), hits[0], dmg[0], kills[0], hits[1], dmg[1], kills[1], healers.size(), heals[0], healed[0],
                     heals[1], healed[1]);
            hits[0] = hits[1] = kills[0] = kills[1] = heals[0] = heals[1] = 0;
            dmg[0] = dmg[1] = healed[0] = healed[1] = 0;
            riders.clear();
            healers.clear();
            logMs = now;
        }
    }

    std::mutex lock;
    uint32 hits[2] = {}, kills[2] = {}, heals[2] = {}, logMs = 0;
    uint64 dmg[2] = {}, healed[2] = {};
    std::set<ObjectGuid::LowType> riders, healers;
};

// SotA: damage demolishers deal to players (Server.log, per 5 min): boulders and rams on the way in and at the gates
class PlayerbotsSADemolisherHitsScript : public UnitScript
{
public:
    PlayerbotsSADemolisherHitsScript() : UnitScript("PlayerbotsSADemolisherHitsScript") {}

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!attacker || !victim || damage == 0 || victim->GetMapId() != 607 || !victim->IsPlayer() ||
            (attacker->GetEntry() != 28781 && attacker->GetEntry() != 32796))
            return;
        std::lock_guard<std::mutex> guard(lock);
        ++hits;
        dmg += damage;
        kills += damage >= victim->GetHealth() ? 1 : 0;
        uint32 const now = getMSTime();
        if (!logMs)
            logMs = now;
        else if (getMSTimeDiff(logMs, now) > 5 * MINUTE * IN_MILLISECONDS)
        {
            LOG_INFO("module", "SA demolisher hits on players (5 min): hits {} damage {} kills {}", hits, dmg, kills);
            hits = kills = 0;
            dmg = 0;
            logMs = now;
        }
    }

private:
    std::mutex lock;
    uint32 hits = 0, kills = 0, logMs = 0;
    uint64 dmg = 0;
};

// SotA: damage by ranged players standing on a gate's wall spot (within 8 yd of a SOTADefPortalDest, 4 yd in height),
// to demolishers and to players (Server.log, per 5 min)
class PlayerbotsSAWallShotsScript : public UnitScript
{
public:
    PlayerbotsSAWallShotsScript() : UnitScript("PlayerbotsSAWallShotsScript") {}

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        Player* p = attacker ? attacker->ToPlayer() : nullptr;
        if (!p || !victim || damage == 0 || p->GetMapId() != 607 || p->GetVehicle() || !PlayerbotAI::IsRanged(p))
            return;
        bool onWall = false;
        for (auto const& w : SOTADefPortalDest)
            onWall = onWall || (std::hypot(p->GetPositionX() - w[0], p->GetPositionY() - w[1]) < 8.0f &&
                                std::fabs(p->GetPositionZ() - w[2]) < 4.0f);
        if (!onWall)
            return;
        uint32 const k = victim->IsPlayer() ? 1 : victim->IsVehicle() && victim->GetEntry() != 27894 ? 0 : 2;
        if (k == 2)
            return;
        std::lock_guard<std::mutex> guard(lock);
        ++hits[k];
        dmg[k] += damage;
        shooters.insert(p->GetGUID().GetCounter());
        uint32 const now = getMSTime();
        if (!logMs)
            logMs = now;
        else if (getMSTimeDiff(logMs, now) > 5 * MINUTE * IN_MILLISECONDS)
        {
            LOG_INFO("module", "SA wall shooters (5 min): {} ranged on wall spots | vs demolishers hits {} damage {} | vs players hits {} damage {}",
                     shooters.size(), hits[0], dmg[0], hits[1], dmg[1]);
            hits[0] = hits[1] = 0;
            dmg[0] = dmg[1] = 0;
            shooters.clear();
            logMs = now;
        }
    }

private:
    std::mutex lock;
    uint32 hits[2] = {}, logMs = 0;
    uint64 dmg[2] = {};
    std::set<ObjectGuid::LowType> shooters;
};

class PlayerbotsBGEventLogPlayerScript : public PlayerScript
{
public:
    PlayerbotsBGEventLogPlayerScript()
        : PlayerScript("PlayerbotsBGEventLogPlayerScript",
                       {PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_BEFORE_TELEPORT,
                        PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_ON_PLAYER_RELEASED_GHOST,
                        PLAYERHOOK_ON_PLAYER_RESURRECT})
    {
    }

    // Death -> release -> revive, with positions, so time spent dead (and where players die)
    // can be measured per game.
    void OnPlayerJustDied(Player* player) override
    {
        BGStrand::KillStat(player);
        LogLifecycle("death", player);
    }
    void OnPlayerReleasedGhost(Player* player) override { LogLifecycle("release", player); }
    void OnPlayerResurrect(Player* player, float /*restore_percent*/, bool& /*applySickness*/) override
    {
        if (player->GetBattleground())
            BGTacticArms::NoteRevive(player->GetGUID());  // graveyard wave (GYWave)
        LogLifecycle("revive", player);
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float x, float y, float z, float /*orientation*/,
                                uint32 /*options*/, Unit* /*target*/) override
    {
        if (!BGEventLogEnabled())
            return true;
        Battleground* bg = player->GetBattleground();
        if (bg && mapid != bg->GetMapId())
            LogPlayer("teleport_out", bg, player,
                      " to_map=" + std::to_string(mapid) + " to_xyz=" + std::to_string(int32(x)) + "," +
                          std::to_string(int32(y)) + "," + std::to_string(int32(z)) + LastAction(player));
        return true;
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (!BGEventLogEnabled())
            return;
        if (Battleground* bg = player->GetBattleground())
            LogPlayer("level", bg, player, " old_level=" + std::to_string(oldLevel));
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!BGEventLogEnabled())
            return;
        if (Battleground* bg = player->GetBattleground())
            LogPlayer("logout", bg, player);
    }

private:
    static void LogLifecycle(char const* event, Player* player)
    {
        if (!BGEventLogEnabled())
            return;
        Battleground* bg = player->GetBattleground();
        if (!bg || bg->GetMapId() != player->GetMapId())
            return;
        LogPlayer(event, bg, player,
                  " xyz=" + std::to_string(int32(player->GetPositionX())) + "," +
                      std::to_string(int32(player->GetPositionY())) + "," +
                      std::to_string(int32(player->GetPositionZ())));
    }
};

void AddPlayerbotsBGEventLogScripts()
{
    new PlayerbotsBGEventLogScript();
    new PlayerbotsBGEventLogPlayerScript();
    new PlayerbotsSAGateDamageScript();
    new PlayerbotsSASiegeDamageScript();
    new PlayerbotsSACannonDamageScript();
    new PlayerbotsSAPassengerScript();
    new PlayerbotsSADemolisherHitsScript();
    new PlayerbotsSAWallShotsScript();
    new PlayerbotsSASlowAuraScript();
}
