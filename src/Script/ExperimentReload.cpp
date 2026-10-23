/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Hot reload of the battleground experiment settings, for A/B testing without restarts.
// With AiPlayerbot.ExperimentHotReload = 1, playerbots.conf is checked every few seconds and, when it
// changed, these keys are re-read (nothing else):
//   AiPlayerbot.BGTactics.<WSG|AB|EY|AV|IC|SA>.Arms and .Baseline, the per-faction BGTactics switches, and
//   AiPlayerbot.RandomBotAutoJoinBG<WS|AB|AV|EY|IC|SA>Count (0 drains a BG: running games finish, no new ones).
// Running games keep their arm; new games use the new settings. The file is parsed here directly
// instead of reloading ConfigMgr, so nothing else changes under running code.

#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <sys/stat.h>

#include "BGTacticArms.h"
#include "Config.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"
#include "ScriptMgr.h"

namespace
{
std::string Trim(std::string s)
{
    size_t b = s.find_first_not_of(" \t\r\"");
    size_t e = s.find_last_not_of(" \t\r\"");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

class PlayerbotsExperimentReloadScript : public WorldScript
{
public:
    PlayerbotsExperimentReloadScript() : WorldScript("PlayerbotsExperimentReloadScript") {}

    void OnUpdate(uint32 diff) override
    {
        _timer += diff;
        if (_timer < 5000)
            return;
        _timer = 0;

        if (!sConfigMgr->GetOption<bool>("Playerbots.ExperimentHotReload", false, false))
            return;

        std::string const path = sConfigMgr->GetConfigPath() + "modules/playerbots.conf";
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return;
        // nanosecond mtime and size: a second write within the same second still counts; the first check applies the
        // file, which catches edits made while the server was starting
        if (st.st_mtim.tv_sec == _sec && st.st_mtim.tv_nsec == _nsec && st.st_size == _size)
            return;
        _sec = st.st_mtim.tv_sec;
        _nsec = st.st_mtim.tv_nsec;
        _size = st.st_size;
        Reload(path);
    }

private:
    void Reload(std::string const& path)
    {
        auto& c = sPlayerbotAIConfig;
        auto flag = [](bool& v) { return [&v](std::string const& s) { v = s == "1" || s == "true"; }; };
        auto count = [](uint32& v) { return [&v](std::string const& s) { v = std::stoul(s); }; };
        auto text = [](std::string& v) { return [&v](std::string const& s) { v = s; }; };
        std::map<std::string, std::function<void(std::string const&)>> const keys = {
            {"Playerbots.BGTactics.WSG.Arms", text(c.wsgTacticArms)},
            {"Playerbots.BGTactics.AB.Arms", text(c.abTacticArms)},
            {"Playerbots.BGTactics.EY.Arms", [](std::string const& s) { BGTacticArms::SetArms(BATTLEGROUND_EY, s); }},
            {"Playerbots.BGTactics.WSG.Baseline", [](std::string const& s) { BGTacticArms::SetBaseline(BATTLEGROUND_WS, s); }},
            {"Playerbots.BGTactics.AB.Baseline", [](std::string const& s) { BGTacticArms::SetBaseline(BATTLEGROUND_AB, s); }},
            {"Playerbots.BGTactics.EY.Baseline", [](std::string const& s) { BGTacticArms::SetBaseline(BATTLEGROUND_EY, s); }},
            {"Playerbots.BGTactics.AV.Arms", [](std::string const& s) { BGTacticArms::SetArms(BATTLEGROUND_AV, s); }},
            {"Playerbots.BGTactics.IC.Arms", [](std::string const& s) { BGTacticArms::SetArms(BATTLEGROUND_IC, s); }},
            {"Playerbots.BGTactics.AV.Baseline", [](std::string const& s) { BGTacticArms::SetBaseline(BATTLEGROUND_AV, s); }},
            {"Playerbots.BGTactics.IC.Baseline", [](std::string const& s) { BGTacticArms::SetBaseline(BATTLEGROUND_IC, s); }},
            {"Playerbots.BGTactics.SA.Arms", [](std::string const& s) { BGTacticArms::SetArms(BATTLEGROUND_SA, s); }},
            {"Playerbots.BGTactics.SA.Baseline", [](std::string const& s) { BGTacticArms::SetBaseline(BATTLEGROUND_SA, s); }},
            {"Playerbots.RandomBotAutoJoinBGSACount", count(c.RandomBotAutoJoinBGSACount)},
            {"Playerbots.BGTactics.WSG.FCEscort.Alliance", flag(c.wsgFCEscort[TEAM_ALLIANCE])},
            {"Playerbots.BGTactics.WSG.FCEscort.Horde", flag(c.wsgFCEscort[TEAM_HORDE])},
            {"Playerbots.BGTactics.WSG.FCChase.Alliance", flag(c.wsgFCChase[TEAM_ALLIANCE])},
            {"Playerbots.BGTactics.WSG.FCChase.Horde", flag(c.wsgFCChase[TEAM_HORDE])},
            {"Playerbots.BGTactics.AB.NodeGuard.Alliance", flag(c.abNodeGuard[TEAM_ALLIANCE])},
            {"Playerbots.BGTactics.AB.NodeGuard.Horde", flag(c.abNodeGuard[TEAM_HORDE])},
            {"Playerbots.RandomBotAutoJoinBGWSCount", count(c.RandomBotAutoJoinBGWSCount)},
            {"Playerbots.RandomBotAutoJoinBGABCount", count(c.RandomBotAutoJoinBGABCount)},
            {"Playerbots.RandomBotAutoJoinBGAVCount", count(c.RandomBotAutoJoinBGAVCount)},
            {"Playerbots.RandomBotAutoJoinBGEYCount", count(c.RandomBotAutoJoinBGEYCount)},
            {"Playerbots.RandomBotAutoJoinBGICCount", count(c.RandomBotAutoJoinBGICCount)},
        };

        std::ifstream in(path);
        std::string line;
        uint32 applied = 0;
        while (std::getline(in, line))
        {
            size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos)
                continue;
            auto it = keys.find(Trim(line.substr(0, eq)));
            if (it == keys.end())
                continue;
            try
            {
                it->second(Trim(line.substr(eq + 1)));
                ++applied;
            }
            catch (std::exception const&)
            {
                LOG_ERROR("server", "Experiment reload: bad value in line '{}'", line);
            }
        }

        LOG_INFO("server",
                 "Experiment reload: {} settings from {} (WSG arms \"{}\", AB arms \"{}\", auto-join WS {} AB {} AV {} EY {} IC {} SA {})",
                 applied, path, c.wsgTacticArms, c.abTacticArms, c.RandomBotAutoJoinBGWSCount,
                 c.RandomBotAutoJoinBGABCount, c.RandomBotAutoJoinBGAVCount, c.RandomBotAutoJoinBGEYCount,
                 c.RandomBotAutoJoinBGICCount, c.RandomBotAutoJoinBGSACount);
    }

    uint32 _timer = 0;
    time_t _sec = 0;
    long _nsec = 0;
    off_t _size = 0;
};
}  // namespace

void AddPlayerbotsExperimentReloadScripts() { new PlayerbotsExperimentReloadScript(); }
