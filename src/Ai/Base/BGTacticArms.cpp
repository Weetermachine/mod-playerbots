/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGTacticArms.h"

#include <mutex>
#include <unordered_map>
#include <vector>

#include "Battleground.h"
#include "Config.h"
#include "PlayerbotAIConfig.h"

namespace
{
struct Assignment
{
    std::string name;
    uint8 mask[2] = {0, 0};  // bit per BGTactic, indexed by TeamId
};

// Games of different BGs update on different map threads, so all access is locked.
std::mutex armsLock;
std::unordered_map<uint32, Assignment> games;  // by BG instance id
std::unordered_map<uint32, uint32> nextArm;    // by BG type id
std::string eyArms;
bool eyArmsLoaded = false;

BattlegroundTypeId RealType(Battleground* bg)
{
    BattlegroundTypeId type = bg->GetBgTypeID();
    return type == BATTLEGROUND_RB ? bg->GetBgTypeID(true) : type;
}

std::string const& ArmsConfig(BattlegroundTypeId type)
{
    static std::string const none;
    switch (type)
    {
        case BATTLEGROUND_WS:
            return sPlayerbotAIConfig.wsgTacticArms;
        case BATTLEGROUND_AB:
            return sPlayerbotAIConfig.abTacticArms;
        case BATTLEGROUND_EY:
            if (!eyArmsLoaded)
            {
                eyArms = sConfigMgr->GetOption<std::string>("AiPlayerbot.BGTactics.EY.Arms", "");
                eyArmsLoaded = true;
            }
            return eyArms;
        default:
            return none;
    }
}

// Split on sep, dropping whitespace and empty parts.
std::vector<std::string> Split(std::string const& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == sep)
        {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        }
        else if (!isspace(static_cast<unsigned char>(c)))
            cur += c;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

// "FCEscort.Alliance+FCChase.Horde" -> masks. Unknown parts (e.g. "stock") switch nothing on.
Assignment ParseArm(std::string const& arm)
{
    Assignment a;
    a.name = arm;
    for (std::string const& part : Split(arm, '+'))
    {
        size_t dot = part.find('.');
        if (dot == std::string::npos)
            continue;

        std::string const tactic = part.substr(0, dot);
        std::string const faction = part.substr(dot + 1);
        int bit = tactic == "FCEscort" ? int(BGTactic::FCEscort)
                : tactic == "FCChase" ? int(BGTactic::FCChase)
                : tactic == "NodeGuard" ? int(BGTactic::NodeGuard) : -1;
        if (bit < 0)
            continue;

        if (faction == "Alliance")
            a.mask[TEAM_ALLIANCE] |= 1 << bit;
        else if (faction == "Horde")
            a.mask[TEAM_HORDE] |= 1 << bit;
    }
    return a;
}

// The game's assignment, made on first lookup; nullptr when its BG has no arms. Needs armsLock.
Assignment const* Assign(Battleground* bg)
{
    auto itr = games.find(bg->GetInstanceID());
    if (itr != games.end())
        return &itr->second;

    BattlegroundTypeId type = RealType(bg);
    std::vector<std::string> arms = Split(ArmsConfig(type), ',');
    if (arms.empty())
        return nullptr;

    uint32 index = nextArm[type]++ % arms.size();
    return &(games[bg->GetInstanceID()] = ParseArm(arms[index]));
}

bool GlobalSwitch(BGTactic tactic, TeamId team, BattlegroundTypeId type)
{
    if (type == BATTLEGROUND_EY)
        return false;  // EotS tactics only run as arms

    switch (tactic)
    {
        case BGTactic::FCEscort:
            return sPlayerbotAIConfig.wsgFCEscort[team];
        case BGTactic::FCChase:
            return sPlayerbotAIConfig.wsgFCChase[team];
        case BGTactic::NodeGuard:
            return sPlayerbotAIConfig.abNodeGuard[team];
    }
    return false;
}
}  // namespace

bool BGTacticArms::IsOn(Battleground* bg, TeamId team, BGTactic tactic)
{
    if (!bg || (team != TEAM_ALLIANCE && team != TEAM_HORDE))
        return false;

    {
        std::lock_guard<std::mutex> guard(armsLock);
        if (Assignment const* a = Assign(bg))
            return a->mask[team] & (1 << uint8(tactic));
    }

    return GlobalSwitch(tactic, team, RealType(bg));
}

std::string BGTacticArms::ArmName(Battleground* bg)
{
    if (!bg)
        return "";

    std::lock_guard<std::mutex> guard(armsLock);
    Assignment const* a = Assign(bg);
    return a ? a->name : "";
}

void BGTacticArms::SetEYArms(std::string const& arms)
{
    std::lock_guard<std::mutex> guard(armsLock);
    eyArms = arms;
    eyArmsLoaded = true;
}

void BGTacticArms::Forget(Battleground* bg)
{
    std::lock_guard<std::mutex> guard(armsLock);
    games.erase(bg->GetInstanceID());
}
