/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Hot reload of the battleground experiment settings, for A/B testing without restarts.
// With AiPlayerbot.ExperimentHotReload = 1, playerbots.conf is checked every few seconds and, when it
// changed, these keys are re-read (nothing else):
//   AiPlayerbot.BGTactics.<WSG|AB>.Arms, the per-faction BGTactics switches, and
//   AiPlayerbot.RandomBotAutoJoinBG<WS|AB|AV|EY|IC>Count (0 drains a BG: running games finish, no new ones).
// Running games keep their arm; new games use the new settings. The file is parsed here directly
// instead of reloading ConfigMgr, so nothing else changes under running code.

#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <sys/stat.h>

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

        if (!sConfigMgr->GetOption<bool>("AiPlayerbot.ExperimentHotReload", false))
            return;

        std::string const path = sConfigMgr->GetConfigPath() + "modules/playerbots.conf";
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return;
        if (_mtime == 0)
        {
            _mtime = st.st_mtime;  // the startup load already has these values
            return;
        }
        if (st.st_mtime == _mtime)
            return;
        _mtime = st.st_mtime;
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
            {"AiPlayerbot.BGTactics.WSG.Arms", text(c.wsgTacticArms)},
            {"AiPlayerbot.BGTactics.AB.Arms", text(c.abTacticArms)},
            {"AiPlayerbot.BGTactics.WSG.FCEscort.Alliance", flag(c.wsgFCEscort[TEAM_ALLIANCE])},
            {"AiPlayerbot.BGTactics.WSG.FCEscort.Horde", flag(c.wsgFCEscort[TEAM_HORDE])},
            {"AiPlayerbot.BGTactics.WSG.FCChase.Alliance", flag(c.wsgFCChase[TEAM_ALLIANCE])},
            {"AiPlayerbot.BGTactics.WSG.FCChase.Horde", flag(c.wsgFCChase[TEAM_HORDE])},
            {"AiPlayerbot.BGTactics.AB.NodeGuard.Alliance", flag(c.abNodeGuard[TEAM_ALLIANCE])},
            {"AiPlayerbot.BGTactics.AB.NodeGuard.Horde", flag(c.abNodeGuard[TEAM_HORDE])},
            {"AiPlayerbot.RandomBotAutoJoinBGWSCount", count(c.randomBotAutoJoinBGWSCount)},
            {"AiPlayerbot.RandomBotAutoJoinBGABCount", count(c.randomBotAutoJoinBGABCount)},
            {"AiPlayerbot.RandomBotAutoJoinBGAVCount", count(c.randomBotAutoJoinBGAVCount)},
            {"AiPlayerbot.RandomBotAutoJoinBGEYCount", count(c.randomBotAutoJoinBGEYCount)},
            {"AiPlayerbot.RandomBotAutoJoinBGICCount", count(c.randomBotAutoJoinBGICCount)},
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
                 "Experiment reload: {} settings from {} (WSG arms \"{}\", AB arms \"{}\", auto-join WS {} AB {} AV {} EY {} IC {})",
                 applied, path, c.wsgTacticArms, c.abTacticArms, c.randomBotAutoJoinBGWSCount,
                 c.randomBotAutoJoinBGABCount, c.randomBotAutoJoinBGAVCount, c.randomBotAutoJoinBGEYCount,
                 c.randomBotAutoJoinBGICCount);
    }

    uint32 _timer = 0;
    time_t _mtime = 0;
};
}  // namespace

void AddPlayerbotsExperimentReloadScripts() { new PlayerbotsExperimentReloadScript(); }
