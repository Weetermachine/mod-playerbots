/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BGCastLog.h"

#include <atomic>
#include <sstream>

#include "Battleground.h"
#include "Log.h"
#include "Player.h"
#include "SpellInfo.h"
#include "Timer.h"

namespace
{
constexpr uint32 BGS = 5;       // WS, AB, EY, AV, IC
constexpr uint32 CLASSES = 12;  // class ids 1-11
char const* const bgNames[BGS] = {"WS", "AB", "EY", "AV", "IC"};
char const* const classNames[CLASSES] = {"", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "DK",
                                         "Shaman", "Mage", "Warlock", "", "Druid"};
std::atomic<uint32> casts[BGS][CLASSES][2];  // [bg][class][0 single-target, 1 area]
std::atomic<uint32> checks[BGS][2][2];       // [bg][light, medium+][0 checked, 1 active]
std::atomic<uint32> logMs{0};

int32 BgIndex(Player* bot)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
        return -1;
    BattlegroundTypeId type = bg->GetBgTypeID();
    if (type == BATTLEGROUND_RB)
        type = bg->GetBgTypeID(true);
    switch (type)
    {
        case BATTLEGROUND_WS: return 0;
        case BATTLEGROUND_AB: return 1;
        case BATTLEGROUND_EY: return 2;
        case BATTLEGROUND_AV: return 3;
        case BATTLEGROUND_IC: return 4;
        default: return -1;
    }
}

void MaybeLog()
{
    uint32 const now = getMSTime();
    uint32 last = logMs.load();
    if (!last)
    {
        logMs.compare_exchange_strong(last, now);
        return;
    }
    if (getMSTimeDiff(last, now) <= 5 * MINUTE * IN_MILLISECONDS || !logMs.compare_exchange_strong(last, now))
        return;
    for (uint32 b = 0; b < BGS; ++b)
    {
        uint32 single = 0, area = 0;
        std::ostringstream per;
        for (uint32 c = 0; c < CLASSES; ++c)
        {
            uint32 const s = casts[b][c][0].exchange(0), a = casts[b][c][1].exchange(0);
            single += s;
            area += a;
            if (a + s)
                per << " " << classNames[c] << " " << a << "/" << (a + s);
        }
        uint32 lc = checks[b][0][0].exchange(0), la = checks[b][0][1].exchange(0);
        uint32 mc = checks[b][1][0].exchange(0), ma = checks[b][1][1].exchange(0);
        if (!single && !area && !lc && !mc)
            continue;
        LOG_INFO("module", "AoE {} (5 min): hostile casts {}, area {} ({:.1f}%); area/all by class:{}; trigger 2+ near target "
                 "true {:.1f}% of {} checks, 3+ {:.1f}% of {}",
                 bgNames[b], single + area, area, single + area ? 100.0 * area / (single + area) : 0.0, per.str(),
                 lc ? 100.0 * la / lc : 0.0, lc, mc ? 100.0 * ma / mc : 0.0, mc);
    }
}
}  // namespace

void BGCastLog::Cast(Player* bot, SpellInfo const* spell)
{
    if (!bot || !spell || spell->IsPositive())
        return;
    int32 const b = BgIndex(bot);
    if (b < 0)
        return;
    uint8 const cls = bot->getClass();
    if (cls >= CLASSES)
        return;
    ++casts[b][cls][spell->IsAffectingArea() ? 1 : 0];
    MaybeLog();
}

void BGCastLog::AoeCheck(Player* bot, uint32 amount, bool active)
{
    int32 const b = BgIndex(bot);
    if (b < 0)
        return;
    uint32 const k = amount <= 2 ? 0 : 1;
    ++checks[b][k][0];
    if (active)
        ++checks[b][k][1];
    MaybeLog();
}
