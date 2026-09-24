/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Battleground roster event log, for diagnosing uneven teams and mid-game departures.
// Enabled with AiPlayerbot.BGEventLog = 1. Writes one key=value line per event to the
// "playerbots.bgevents" logger; route it to its own file in worldserver.conf, e.g.
//   Appender.BGEvents=2,4,1,BGEvents.log,a      (flags 1 = timestamp prefix)
//   Logger.playerbots.bgevents=4,BGEvents

#include <mutex>
#include <unordered_map>

#include "Battleground.h"
#include "BattlegroundWS.h"
#include "Config.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
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
    LOG_INFO("playerbots.bgevents", "event={} bg={} type={} name=\"{}\" levels={}-{} status={} bg_ms={} A={} H={}",
             event, bg->GetInstanceID(), uint32(bg->GetBgTypeID()), bg->GetName(), uint32(bg->GetMinLevel()),
             uint32(bg->GetMaxLevel()), StatusName(bg->GetStatus()), bg->GetStartTime(),
             bg->GetPlayersCountByTeam(TEAM_ALLIANCE), bg->GetPlayersCountByTeam(TEAM_HORDE));
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

    // WSG objective events: logs a "wsg" line whenever either flag's state, either carrier, or
    // the score changes (pickups, drops, returns, captures). Only compares a few fields per tick.
    void OnBattlegroundUpdate(Battleground* bg, uint32 /*diff*/) override
    {
        if (bg->GetBgTypeID() != BATTLEGROUND_WS || bg->GetStatus() != STATUS_IN_PROGRESS || !BGEventLogEnabled())
            return;
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
    }

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
        if (BGEventLogEnabled())
            LOG_INFO("playerbots.bgevents", "event=end bg={} type={} bg_ms={} winner={} A={} H={}",
                     bg->GetInstanceID(), uint32(bg->GetBgTypeID()), bg->GetStartTime(), TeamName(winnerTeam),
                     bg->GetPlayersCountByTeam(TEAM_ALLIANCE), bg->GetPlayersCountByTeam(TEAM_HORDE));
    }
};

// Level changes, logouts and teleports off the BG map: catches bots being reset (e.g. by
// mod-player-bot-reset), logged out, or teleported out (which removes them from the BG) mid-game.
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
    void OnPlayerJustDied(Player* player) override { LogLifecycle("death", player); }
    void OnPlayerReleasedGhost(Player* player) override { LogLifecycle("release", player); }
    void OnPlayerResurrect(Player* player, float /*restore_percent*/, bool& /*applySickness*/) override
    {
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
}
