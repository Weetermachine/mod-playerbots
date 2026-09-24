/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Battleground roster event log, for diagnosing uneven teams and mid-game departures.
// Enabled with AiPlayerbot.BGEventLog = 1. Writes one key=value line per event to the
// "playerbots.bgevents" logger; route it to its own file in worldserver.conf, e.g.
//   Appender.BGEvents=2,4,0,BGEvents.log,a
//   Logger.playerbots.bgevents=4,BGEvents

#include "Battleground.h"
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
}  // namespace

class PlayerbotsBGEventLogScript : public BGScript
{
public:
    PlayerbotsBGEventLogScript()
        : BGScript("PlayerbotsBGEventLogScript",
                   {ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_START, ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_ADD_PLAYER,
                    ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_REMOVE_PLAYER_AT_LEAVE, ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END})
    {
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
                       {PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_BEFORE_TELEPORT})
    {
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
};

void AddPlayerbotsBGEventLogScripts()
{
    new PlayerbotsBGEventLogScript();
    new PlayerbotsBGEventLogPlayerScript();
}
