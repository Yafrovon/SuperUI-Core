/* SuperUI RTS boot latch and module facade. */

#ifndef MANGOS_SUI_RTS_H
#define MANGOS_SUI_RTS_H

#include "Common.h"

#include <string>

class Player;
class Unit;
class WorldSession;

namespace SuiRts
{
    // Called once during boot. The web world-profile owns schema creation and
    // migration; the core only reads pre-created RTS tables.
    void LoadRuleset();
    // Re-applies the RTS world's rate.* overrides after .reload config re-read
    // mangosd.conf. No-op outside an RTS boot.
    void ReapplyRateOverrides();
    void Tick(uint32 diff);
    void Shutdown();

    bool HonorEnabled();
    bool HeroesEnabled();
    bool TerritoryEnabled();
    bool DungeonsEnabled();
    bool FactionControlEnabled();

    std::string GetKV(std::string const& key, std::string const& def);
    float GetKVFloat(std::string const& key, float def);
    int64 GetKVInt(std::string const& key, int64 def);

    int64 HonorPool(uint8 teamIdx);
    void AddHonor(uint8 teamIdx, int64 amount);
    bool TrySpendHonor(uint8 teamIdx, int64 amount, int64* poolAfter = nullptr);
    void RefundHonor(uint8 teamIdx, int64 amount, int64* poolAfter = nullptr);

    int64 BotCap(uint8 teamIdx);

    // Minimal core seam dispatchers. Every implementation returns immediately
    // unless the immutable boot mode and the owning module gate are active.
    void OnUnitKill(Unit* killer, Unit* victim);
    void OnPlayerWorldEnter(Player* player);

    void HandleRtsState(WorldSession* session, uint8 flags);
    void HandleRtsAction(WorldSession* session, uint8 action, uint64 subjectGuid);
}

#endif
