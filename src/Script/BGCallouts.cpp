/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Battleground callouts (AiPlayerbot.BGCallouts): when enemies close in on a node a team holds (AB bases, EotS towers),
// the team's bot nearest it says "4 inc BS" in battleground chat, as players call incoming attacks. At most one callout
// per node and team every 45 s. Only chat: no bot behavior changes.

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Battleground.h"
#include "BattlegroundAB.h"
#include "BattlegroundEY.h"
#include "Chat.h"
#include "Config.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Timer.h"

namespace
{
constexpr uint32 CHECK_MS = 5000;
constexpr uint32 REPEAT_MS = 45000;
constexpr float INCOMING_RANGE = 60.0f;  // enemies this close to the node count as incoming
constexpr float DEFENSE_RANGE = 30.0f;   // our players this close count as defending it

struct Node
{
    char const* name;
    float x, y;
    TeamId owner;
};

struct CalloutState
{
    uint32 lastCheckMs = 0;
    std::unordered_map<uint32, uint32> lastCallMs;  // (node << 1 | team) -> last callout
};

std::mutex calloutLock;
std::unordered_map<uint32, CalloutState> states;  // by BG instance id

bool Enabled() { return sConfigMgr->GetOption<bool>("AiPlayerbot.BGCallouts", false, false); }  // read per call: no missing-key log

// The held nodes of an AB or EotS game (owner TEAM_NEUTRAL: nobody holds it); empty for other BGs.
std::vector<Node> Nodes(Battleground* bg)
{
    std::vector<Node> out;
    if (BattlegroundAB* ab = dynamic_cast<BattlegroundAB*>(bg))
    {
        static char const* names[BG_AB_DYNAMIC_NODES_COUNT] = {"ST", "BS", "Farm", "LM", "GM"};
        for (uint32 i = 0; i < BG_AB_DYNAMIC_NODES_COUNT; ++i)
        {
            uint8 const st = ab->GetCapturePointInfo(i)._state;
            TeamId const owner = st == BG_AB_NODE_STATE_ALLY_OCCUPIED ? TEAM_ALLIANCE
                               : st == BG_AB_NODE_STATE_HORDE_OCCUPIED ? TEAM_HORDE : TEAM_NEUTRAL;
            out.push_back({names[i], BG_AB_NodePositions[i][0], BG_AB_NodePositions[i][1], owner});
        }
    }
    else if (BattlegroundEY* ey = dynamic_cast<BattlegroundEY*>(bg))
    {
        static char const* names[EY_POINTS_MAX] = {"FR", "BE", "DR", "MT"};
        for (uint32 i = 0; i < EY_POINTS_MAX; ++i)
            out.push_back({names[i], BG_EY_TriggerPositions[i][0], BG_EY_TriggerPositions[i][1],
                           ey->GetCapturePointInfo(i)._ownerTeamId});
    }
    return out;
}

void Check(Battleground* bg, uint32 now)
{
    std::vector<Node> const nodes = Nodes(bg);
    for (uint32 n = 0; n < nodes.size(); ++n)
    {
        Node const& node = nodes[n];
        if (node.owner != TEAM_ALLIANCE && node.owner != TEAM_HORDE)
            continue;
        uint32 incoming = 0, defending = 0;
        Player* speaker = nullptr;
        float speakerDist = 100.0f;
        for (auto const& ref : bg->GetBgMap()->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || !p->IsAlive())
                continue;
            float const d = p->GetDistance2d(node.x, node.y);
            if (p->GetBgTeamId() != node.owner)
                incoming += d < INCOMING_RANGE;
            else
            {
                defending += d < DEFENSE_RANGE;
                if (d < speakerDist && GET_PLAYERBOT_AI(p))
                {
                    speakerDist = d;
                    speaker = p;
                }
            }
        }
        if (incoming < 2 || incoming <= defending || !speaker)
            continue;
        {
            std::lock_guard<std::mutex> guard(calloutLock);
            uint32& last = states[bg->GetInstanceID()].lastCallMs[n << 1 | node.owner];
            if (last && getMSTimeDiff(last, now) < REPEAT_MS)
                continue;
            last = now;
        }
        std::string const text = std::to_string(incoming) + " inc " + node.name;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_BATTLEGROUND, LANG_UNIVERSAL, speaker, nullptr, text);
        bg->SendPacketToTeam(node.owner, &data);
        LOG_INFO("module", "BG callout: {} says \"{}\" (bg {})", speaker->GetName(), text, bg->GetInstanceID());
    }
}
}  // namespace

class PlayerbotsBGCalloutScript : public BGScript
{
public:
    PlayerbotsBGCalloutScript()
        : BGScript("PlayerbotsBGCalloutScript", {ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_UPDATE,
                                                 ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_DESTROY})
    {
    }

    void OnBattlegroundUpdate(Battleground* bg, uint32 /*diff*/) override
    {
        if (bg->GetStatus() != STATUS_IN_PROGRESS || !Enabled())
            return;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> guard(calloutLock);
            CalloutState& st = states[bg->GetInstanceID()];
            if (st.lastCheckMs && getMSTimeDiff(st.lastCheckMs, now) < CHECK_MS)
                return;
            st.lastCheckMs = now;
        }
        Check(bg, now);
    }

    void OnBattlegroundDestroy(Battleground* bg) override
    {
        std::lock_guard<std::mutex> guard(calloutLock);
        states.erase(bg->GetInstanceID());
    }
};

void AddPlayerbotsBGCalloutScripts() { new PlayerbotsBGCalloutScript(); }
