/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MOUNTLEVELS_H
#define _PLAYERBOT_MOUNTLEVELS_H

#include "Define.h"

// The levels at which bots learn and use riding: by default the server's own trainer levels for Apprentice,
// Journeyman, Expert and Artisan Riding (trainer_spell.ReqLevel), so a server that lowers or raises them (e.g.
// Apprentice Riding at 15) gets bots that follow. AiPlayerbot.MountLevelsFromTrainers = 0 keeps the fixed
// AiPlayerbot.Use*MountAtMinLevel values; a riding spell missing from trainer_spell also falls back to them.
namespace MountLevels
{
    uint32 Ground();
    uint32 FastGround();
    uint32 Fly();
    uint32 FastFly();
}

#endif
