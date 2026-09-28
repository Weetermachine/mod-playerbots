/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MountLevels.h"

#include <mutex>
#include <unordered_map>

#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"

namespace
{
constexpr uint32 APPRENTICE_RIDING = 33388;
constexpr uint32 JOURNEYMAN_RIDING = 33391;
constexpr uint32 EXPERT_RIDING = 34090;
constexpr uint32 ARTISAN_RIDING = 34091;

std::once_flag loaded;
std::unordered_map<uint32, uint32> trainerLevel;  // riding spell -> lowest trainer level

void Load()
{
    if (!sConfigMgr->GetOption<bool>("AiPlayerbot.MountLevelsFromTrainers", true))
        return;
    QueryResult result = WorldDatabase.Query(
        "SELECT SpellId, MIN(ReqLevel) FROM trainer_spell WHERE SpellId IN ({}, {}, {}, {}) AND ReqLevel > 0 "
        "GROUP BY SpellId",
        APPRENTICE_RIDING, JOURNEYMAN_RIDING, EXPERT_RIDING, ARTISAN_RIDING);
    if (!result)
        return;
    do
    {
        Field* fields = result->Fetch();
        trainerLevel[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
    } while (result->NextRow());
    LOG_INFO("playerbots", "Bot riding levels from trainers: ground {}, fast ground {}, flying {}, fast flying {}",
             MountLevels::Ground(), MountLevels::FastGround(), MountLevels::Fly(), MountLevels::FastFly());
}

uint32 Level(uint32 spell, uint32 fallback)
{
    std::call_once(loaded, Load);
    auto it = trainerLevel.find(spell);
    return it == trainerLevel.end() ? fallback : it->second;
}
}  // namespace

uint32 MountLevels::Ground() { return Level(APPRENTICE_RIDING, sPlayerbotAIConfig.useGroundMountAtMinLevel); }
uint32 MountLevels::FastGround() { return Level(JOURNEYMAN_RIDING, sPlayerbotAIConfig.useFastGroundMountAtMinLevel); }
uint32 MountLevels::Fly() { return Level(EXPERT_RIDING, sPlayerbotAIConfig.useFlyMountAtMinLevel); }
uint32 MountLevels::FastFly() { return Level(ARTISAN_RIDING, sPlayerbotAIConfig.useFastFlyMountAtMinLevel); }
