/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGTacticArms.h"

#include <bitset>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Battleground.h"
#include "Config.h"
#include "PlayerbotAIConfig.h"
#include "Timer.h"

namespace
{
constexpr size_t TACTIC_BITS = 128;  // BGTactic values must stay below this (static_assert in the header)

struct Assignment
{
    std::string name;
    std::bitset<TACTIC_BITS> mask[2];  // bit per BGTactic, indexed by TeamId
};

// Games of different BGs update on different map threads, so all access is locked.
std::mutex armsLock;
std::unordered_map<uint32, Assignment> games;  // by BG instance id
std::unordered_map<uint32, uint32> nextArm;    // by BG type id
std::unordered_map<uint32, std::string> localArms;  // EY, AV, IC arms by BG type id; loaded from the config on first use
std::unordered_map<uint32, std::string> baselines;  // by BG type id; loaded from the config on first use

BattlegroundTypeId RealType(Battleground* bg)
{
    BattlegroundTypeId type = bg->GetBgTypeID();
    return type == BATTLEGROUND_RB ? bg->GetBgTypeID(true) : type;
}

char const* BGName(BattlegroundTypeId type);

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
        case BATTLEGROUND_AV:
        case BATTLEGROUND_IC:
        case BATTLEGROUND_SA:
        {
            auto itr = localArms.find(type);
            if (itr == localArms.end())
            {
                char const* key = BGName(type);
                itr = localArms.emplace(type, sConfigMgr->GetOption<std::string>(
                                                  std::string("AiPlayerbot.BGTactics.") + key + ".Arms", "")).first;
            }
            return itr->second;
        }
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

int TacticBit(std::string const& tactic)
{
    return tactic == "FCEscort" ? int(BGTactic::FCEscort)
         : tactic == "FCChase" ? int(BGTactic::FCChase)
         : tactic == "NodeGuard" ? int(BGTactic::NodeGuard)
         : tactic == "Adaptive" ? int(BGTactic::Adaptive)
         : tactic == "FCEscort2" ? int(BGTactic::FCEscort2)
         : tactic == "NodeGuard2" ? int(BGTactic::NodeGuard2)
         : tactic == "FlagRoom" ? int(BGTactic::FlagRoom)
         : tactic == "DirectPath" || tactic == "EYPath" ? int(BGTactic::DirectPath)
         : tactic == "Allocator" ? int(BGTactic::Allocator)
         : tactic == "AllocRoles" ? int(BGTactic::AllocRoles)
         : tactic == "AllocGuard2" ? int(BGTactic::AllocGuard2)
         : tactic == "AllocOneGroup" ? int(BGTactic::AllocOneGroup)
         : tactic == "AllocFlagFloor" ? int(BGTactic::AllocFlagFloor)
         : tactic == "AllocFlagValue" ? int(BGTactic::AllocFlagValue)
         : tactic == "ChaseStagger" ? int(BGTactic::ChaseStagger)
         : tactic == "FCEvade" ? int(BGTactic::FCEvade)
         : tactic == "EscortStick" ? int(BGTactic::EscortStick)
         : tactic == "DirectTime" ? int(BGTactic::DirectTime)
         : tactic == "SmoothMove" ? int(BGTactic::SmoothMove)
         : tactic == "DirectSafe" ? int(BGTactic::DirectSafe)
         : tactic == "GroupMove" ? int(BGTactic::GroupMove)
         : tactic == "ChaseV3" ? int(BGTactic::ChaseV3)
         : tactic == "FocusFire" ? int(BGTactic::FocusFire)
         : tactic == "FocusPartial" ? int(BGTactic::FocusPartial)
         : tactic == "TowerRush" ? int(BGTactic::TowerRush)
         : tactic == "EYPlan" ? int(BGTactic::EYPlan)
         : tactic == "ClassRoles" ? int(BGTactic::ClassRoles)
         : tactic == "HealerHunt" ? int(BGTactic::HealerHunt)
         : tactic == "AB3Cap" ? int(BGTactic::AB3Cap)
         : tactic == "Rebuff" ? int(BGTactic::Rebuff)
         : tactic == "JobFocus" ? int(BGTactic::JobFocus)
         : tactic == "GYWave" ? int(BGTactic::GYWave)
         : tactic == "BackOff" ? int(BGTactic::BackOff)
         : tactic == "WPGraph" ? int(BGTactic::WPGraph)
         : tactic == "Comeback" ? int(BGTactic::Comeback)
         : tactic == "Intercept" ? int(BGTactic::Intercept)
         : tactic == "EscortHealer" ? int(BGTactic::EscortHealer)
         : tactic == "StandoffBreak" ? int(BGTactic::StandoffBreak)
         : tactic == "Reinforce" ? int(BGTactic::Reinforce)
         : tactic == "Retake" ? int(BGTactic::Retake)
         : tactic == "NoSolo" ? int(BGTactic::NoSolo)
         : tactic == "FinishKill" ? int(BGTactic::FinishKill)
         : tactic == "EYRetake" ? int(BGTactic::EYRetake)
         : tactic == "DirectCarrier" ? int(BGTactic::DirectCarrier)
         : tactic == "AVHoldTower" ? int(BGTactic::AVHoldTower)
         : tactic == "AVBossForce" ? int(BGTactic::AVBossForce)
         : tactic == "AVCaptainHome" ? int(BGTactic::AVCaptainHome)
         : tactic == "ICGuardFix" ? int(BGTactic::ICGuardFix)
         : tactic == "ICStayVehicle" ? int(BGTactic::ICStayVehicle)
         : tactic == "AoESquad2" ? int(BGTactic::AoESquad2)
         : tactic == "AoESquad5" ? int(BGTactic::AoESquad5)
         : tactic == "AoESquadAll" ? int(BGTactic::AoESquadAll)
         : tactic == "AoEGeneral" ? int(BGTactic::AoEGeneral)
         : tactic == "BossRaid" ? int(BGTactic::BossRaid)
         : tactic == "Disrupt" ? int(BGTactic::Disrupt)
         : tactic == "AVMines" ? int(BGTactic::AVMines)
         : tactic == "ICSiegeEscort" ? int(BGTactic::ICSiegeEscort)
         : tactic == "AVGraveyards" ? int(BGTactic::AVGraveyards)
         : tactic == "HealerGuard" ? int(BGTactic::HealerGuard)
         : tactic == "EYSafeTower" ? int(BGTactic::EYSafeTower)
         : tactic == "AVNoMines" ? int(BGTactic::AVNoMines)
         : tactic == "AVBurnFirst" ? int(BGTactic::AVBurnFirst)
         : tactic == "DisruptTank" ? int(BGTactic::DisruptTank)
         : tactic == "AVBothMines" ? int(BGTactic::AVBothMines)
         : tactic == "PvPAoE" ? int(BGTactic::PvPAoE)
         : tactic == "EYFelReaverFix" ? int(BGTactic::EYFelReaverFix)
         : tactic == "PvPAttackers" ? int(BGTactic::PvPAttackers)
         : tactic == "StealthFix" ? int(BGTactic::StealthFix)
         : tactic == "PvPThreatFix" ? int(BGTactic::PvPThreatFix)
         : tactic == "SpiritNearest" ? int(BGTactic::SpiritNearest)
         : tactic == "WSGFixes" ? int(BGTactic::WSGFixes)
         : tactic == "ABStealBack" ? int(BGTactic::ABStealBack)
         : tactic == "GroundZFix" ? int(BGTactic::GroundZFix)
         : tactic == "AVFixes" ? int(BGTactic::AVFixes)
         : tactic == "StockEscort" ? int(BGTactic::StockEscort)
         : tactic == "ICSiegeFix" ? int(BGTactic::ICSiegeFix)
         : tactic == "EYRouteFix" ? int(BGTactic::EYRouteFix)
         : tactic == "SATactics" ? int(BGTactic::SATactics)
         : tactic == "EYJunctionDirect" ? int(BGTactic::EYJunctionDirect) : -1;
}

char const* BGName(BattlegroundTypeId type)
{
    return type == BATTLEGROUND_WS ? "WSG" : type == BATTLEGROUND_AB ? "AB" : type == BATTLEGROUND_EY ? "EY"
         : type == BATTLEGROUND_AV ? "AV" : type == BATTLEGROUND_IC ? "IC"
         : type == BATTLEGROUND_SA ? "SA" : nullptr;
}

// Baseline tactics of a BG type as a mask (both teams). Needs armsLock.
std::bitset<TACTIC_BITS> BaselineMask(BattlegroundTypeId type)
{
    auto itr = baselines.find(type);
    if (itr == baselines.end())
    {
        char const* name = BGName(type);
        std::string const cfg =
            name ? sConfigMgr->GetOption<std::string>(std::string("AiPlayerbot.BGTactics.") + name + ".Baseline", "") : "";
        itr = baselines.emplace(type, cfg).first;
    }
    std::bitset<TACTIC_BITS> mask;
    for (std::string const& group : Split(itr->second, ','))
        for (std::string const& part : Split(group, '+'))
        {
            int const bit = TacticBit(part.substr(0, part.find('.')));  // a faction suffix is ignored
            if (bit >= 0)
                mask.set(bit);
        }
    return mask;
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
        int bit = TacticBit(tactic);
        if (bit < 0)
            continue;

        if (faction == "Alliance")
            a.mask[TEAM_ALLIANCE].set(bit);
        else if (faction == "Horde")
            a.mask[TEAM_HORDE].set(bit);
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
    Assignment a = ParseArm(arms[index]);
    std::bitset<TACTIC_BITS> const base = BaselineMask(type);  // fixed for the game's lifetime
    a.mask[TEAM_ALLIANCE] |= base;
    a.mask[TEAM_HORDE] |= base;
    return &(games[bg->GetInstanceID()] = a);
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
        case BGTactic::Adaptive:
        case BGTactic::FCEscort2:
        case BGTactic::NodeGuard2:
        case BGTactic::FlagRoom:
        case BGTactic::DirectPath:
        case BGTactic::Allocator:
        case BGTactic::AllocRoles:
        case BGTactic::AllocGuard2:
        case BGTactic::AllocOneGroup:
        case BGTactic::AllocFlagFloor:
        case BGTactic::AllocFlagValue:
        case BGTactic::ChaseStagger:
        case BGTactic::FCEvade:
        case BGTactic::EscortStick:
        case BGTactic::DirectTime:
        case BGTactic::SmoothMove:
        case BGTactic::DirectSafe:
        case BGTactic::GroupMove:
        case BGTactic::ChaseV3:
        case BGTactic::FocusFire:
        case BGTactic::FocusPartial:
        case BGTactic::TowerRush:
        case BGTactic::EYPlan:
        case BGTactic::ClassRoles:
        case BGTactic::HealerHunt:
        case BGTactic::AB3Cap:
        case BGTactic::Rebuff:
        case BGTactic::JobFocus:
        case BGTactic::GYWave:
        case BGTactic::BackOff:
        case BGTactic::WPGraph:
        case BGTactic::Comeback:
        case BGTactic::Intercept:
        case BGTactic::EscortHealer:
        case BGTactic::StandoffBreak:
        case BGTactic::Reinforce:
        case BGTactic::Retake:
        case BGTactic::NoSolo:
        case BGTactic::FinishKill:
        case BGTactic::EYRetake:
        case BGTactic::DirectCarrier:
        case BGTactic::AVHoldTower:
        case BGTactic::AVBossForce:
        case BGTactic::AVCaptainHome:
        case BGTactic::ICGuardFix:
        case BGTactic::ICStayVehicle:
        case BGTactic::AoESquad2:
        case BGTactic::AoESquad5:
        case BGTactic::AoESquadAll:
        case BGTactic::AoEGeneral:
        case BGTactic::BossRaid:
        case BGTactic::Disrupt:
        case BGTactic::AVMines:
        case BGTactic::ICSiegeEscort:
        case BGTactic::AVGraveyards:
        case BGTactic::HealerGuard:
        case BGTactic::EYSafeTower:
        case BGTactic::AVNoMines:
        case BGTactic::AVBurnFirst:
        case BGTactic::DisruptTank:
        case BGTactic::AVBothMines:
        case BGTactic::PvPAoE:
        case BGTactic::EYFelReaverFix:
        case BGTactic::PvPAttackers:
        case BGTactic::StealthFix:
        case BGTactic::PvPThreatFix:
        case BGTactic::SpiritNearest:
        case BGTactic::WSGFixes:
        case BGTactic::ABStealBack:
        case BGTactic::GroundZFix:
        case BGTactic::AVFixes:
        case BGTactic::StockEscort:
        case BGTactic::ICSiegeFix:
        case BGTactic::EYRouteFix:
        case BGTactic::SATactics:
        case BGTactic::EYJunctionDirect:
        case BGTactic::TacticCount:
            return false;  // arms only
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
            return a->mask[team].test(uint32(tactic));
        if (BaselineMask(RealType(bg)).test(uint32(tactic)))
            return true;
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
    SetArms(BATTLEGROUND_EY, arms);
}

void BGTacticArms::SetArms(BattlegroundTypeId type, std::string const& arms)
{
    std::lock_guard<std::mutex> guard(armsLock);
    localArms[type] = arms;
}

namespace
{
std::mutex reviveLock;
std::unordered_map<ObjectGuid, uint32> reviveMs;
}  // namespace

void BGTacticArms::NoteRevive(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(reviveLock);
    if (reviveMs.size() > 20000)
        reviveMs.clear();
    reviveMs[guid] = getMSTime();
}

uint32 BGTacticArms::LastReviveMs(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(reviveLock);
    auto it = reviveMs.find(guid);
    return it == reviveMs.end() ? 0 : it->second;
}

void BGTacticArms::SetBaseline(BattlegroundTypeId type, std::string const& tactics)
{
    std::lock_guard<std::mutex> guard(armsLock);
    baselines[type] = tactics;
}

void BGTacticArms::Forget(Battleground* bg)
{
    std::lock_guard<std::mutex> guard(armsLock);
    games.erase(bg->GetInstanceID());
}
