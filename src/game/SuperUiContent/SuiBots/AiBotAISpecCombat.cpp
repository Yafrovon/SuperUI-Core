/*
 * AiBotAISpecCombat.cpp -- typed, talent-aware combat policies for AiBots.
 *
 * A policy is enabled only for a PB_TALENT_PROFILE_USABLE entry.  Every other
 * profile state deliberately falls back to the inherited class AI.  The
 * external LOAD_ROTATION slate is dispatched before this file and remains the
 * absolute override.
 *
 * The nine class methods below each contain their three Vanilla talent-tab
 * policies.  They share only mechanics that benefit every class: rank-chain
 * lookup, add-only CC selection, CC/threat-safe AoE, interrupts, taunts and pet
 * commands.  Engagement, doctrine, movement and target ownership stay in the
 * existing AiBot spine.
 */

#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "AiBotAIMain.h"
#include <map>
#include <mutex>
#include "Config/Config.h"
#include <fstream>
#include <set>
#include "SuiRaidTelemetry.h"
#include "SuiCommanderRaid.h"
#include "SuiRaidPlaybook.h"
#include "AiBotCircuit.h"   // [CIRCUIT] probe macros (CIRCUIT_BOARD.md)
#include "CreatureAI.h"
#include "TemporarySummon.h"
#include "Group.h"
#include "Item.h"
#include "Bag.h"
#include "MotionMaster.h"
#include "PlayerBotMgr.h"
#include "Spell.h"
#include "ObjectMgr.h"
#include "SpellAuras.h"
#include "Totem.h"

#include <cstdio>
#include <list>

// The unit a cast in progress is aimed at, looked up by its guid on `me`'s map; null once it has left the map. Never
// the cast's raw target pointer: a TEMPSUMMON_DEAD_DESPAWN add is deleted the moment it dies, and a member still
// casting or auto-shooting at it keeps that pointer until its spell updates (2026-11-10: mangosd SIGSEGV in
// HasBreakableByDamageCrowdControlAura as Majordomo's last add died, twice - 14:46 and 16:21).
static Unit* CastTarget(Player const* me, Spell const* cast)
{
    ObjectGuid const guid = cast->m_targets.getUnitTargetGuid();
    if (guid.IsEmpty() || !me->IsInWorld())
        return nullptr;
    return const_cast<Player*>(me)->GetMap()->GetUnit(guid);
}

namespace
{
enum SpecSpell : uint32
{
    // Warrior
    SP_BATTLE_STANCE = 2457, SP_DEFENSIVE_STANCE = 71, SP_BERSERKER_STANCE = 2458,
    SP_REVENGE = 6572, SP_TAUNT = 355, SP_SHIELD_BLOCK = 2565,
    SP_SWEEPING_STRIKES = 12292, SP_MORTAL_STRIKE = 12294,
    SP_PIERCING_HOWL = 12323, SP_DEATH_WISH = 12328, SP_BLOODTHIRST = 23881,
    SP_LAST_STAND = 12975, SP_CONCUSSION_BLOW = 12809, SP_SHIELD_SLAM = 23922,

    // Paladin
    SP_HOLY_SHOCK = 20473, SP_DIVINE_FAVOR = 20216, SP_HOLY_SHIELD = 20925,
    SP_REPENTANCE = 20066,
    SP_BLESSING_OF_MIGHT = 19740, SP_BLESSING_OF_WISDOM = 19742,
    SP_BLESSING_OF_LIGHT = 19977, SP_BLESSING_OF_KINGS = 20217,
    SP_BLESSING_OF_SANCTUARY = 20911, SP_BLESSING_OF_SALVATION = 1038,
    SP_BLESSING_OF_PROTECTION = 1022, SP_BLESSING_OF_FREEDOM = 1044,
    SP_BLESSING_OF_SACRIFICE = 6940,

    // Hunter
    SP_AUTO_SHOT = 75, SP_MEND_PET = 136, SP_RAPID_FIRE = 3045,
    SP_INTIMIDATION = 19577, SP_BESTIAL_WRATH = 19574,
    SP_SCATTER_SHOT = 19503, SP_TRUESHOT_AURA = 19506,
    SP_DETERRENCE = 19263, SP_WYVERN_STING = 19386,

    // Rogue
    SP_COLD_BLOOD = 14177, SP_RIPOSTE = 14251, SP_BLADE_FLURRY = 13877,
    SP_ADRENALINE_RUSH = 13750, SP_GHOSTLY_STRIKE = 14278,
    SP_HEMORRHAGE = 16511, SP_PREPARATION = 14185,

    // Priest
    SP_POWER_WORD_SHIELD = 17, SP_WEAKENED_SOUL = 6788,
    SP_INNER_FOCUS = 14751, SP_POWER_INFUSION = 10060,
    SP_HOLY_NOVA = 15237, SP_SHADOWFORM = 15473,
    SP_VAMPIRIC_EMBRACE = 15286, SP_SILENCE = 15487, SP_MIND_FLAY = 15407,

    // Shaman
    SP_ELEMENTAL_MASTERY = 16166, SP_STORMSTRIKE = 17364,
    SP_NATURES_SWIFTNESS_SHAMAN = 16188, SP_MANA_TIDE = 16190,

    // Mage
    SP_ARCANE_MISSILES = 5143, SP_PRESENCE_OF_MIND = 12043,
    SP_ARCANE_POWER = 12042, SP_COMBUSTION = 11129,
    SP_BLAST_WAVE = 11113, SP_COLD_SNAP = 12472, SP_ICE_BARRIER = 11426,

    // Warlock
    SP_DARK_PACT = 18220, SP_FEL_DOMINATION = 18708, SP_SOUL_LINK = 19028,
    SP_SOUL_LINK_AURA = 18814, SP_SOUL_LINK_AURA_COMPAT = 25228,
    SP_DRAIN_SOUL = 1120, SP_HEALTH_FUNNEL = 755,

    // Druid
    SP_NATURES_SWIFTNESS_DRUID = 17116, SP_SWIFTMEND = 18562,
    SP_FERAL_CHARGE = 16979, SP_FAERIE_FIRE_FERAL = 16857,

    // Battleground objective auras: stealth must never hide a flag carrier.
    SP_WARSONG_FLAG = 23333, SP_SILVERWING_FLAG = 23335,
};

bool HasSoulLinkAura(Unit const* master)
{
    return master && (master->HasAura(SP_SOUL_LINK_AURA) ||
                      master->HasAura(SP_SOUL_LINK_AURA_COMPAT));
}

enum SpecBuffReason : uint8
{
    BUFF_PALADIN_TANK = 1,
    BUFF_PALADIN_MANA = 2,
    BUFF_PALADIN_PHYSICAL = 3,
    BUFF_PRIEST_FORTITUDE_SINGLE = 10,
    BUFF_PRIEST_FORTITUDE_GROUP = 11,
    BUFF_PRIEST_SPIRIT_SINGLE = 12,
    BUFF_PRIEST_SPIRIT_GROUP = 13,
    BUFF_MAGE_INTELLECT_SINGLE = 20,
    BUFF_MAGE_INTELLECT_GROUP = 21,
    BUFF_DRUID_WILD_SINGLE = 30,
    BUFF_DRUID_WILD_GROUP = 31,
    BUFF_DRUID_THORNS_TANK_FIRST = 32,
    BUFF_DRUID_INNERVATE_SUPPORT = 33,
};

CombatBotRoles GetSpecBuffTargetRole(Player* target)
{
    if (!target)
        return ROLE_INVALID;   // cb:fold pure recipient classifier, winner probed before cast

    if (AiBotAI* ai = dynamic_cast<AiBotAI*>(target->AI()))
        return ai->GetCombatActiveRole();   // cb:fold pure recipient classifier, winner probed before cast

    if (CombatBotBaseAI* ai = dynamic_cast<CombatBotBaseAI*>(target->AI()))
        return ai->GetRole();   // cb:fold pure recipient classifier, winner probed before cast

    return ROLE_INVALID;
}

bool IsSpecManaUser(Player const* target)
{
    // Max mana is stable while a Druid is shifted; current power type is not.
    return target && target->GetMaxPower(POWER_MANA) > 0;
}

void TraceSpecBuffSelection(AiBotAI* ai, Player* target, SpellEntry const* spell,
                            uint8 reason, bool groupSpell)
{
    if (!ai || !target || !spell)
        return;   // cb:fold defensive probe guard, no decision exists to record

    if (CbCircuit::g_mode)
    {
        char note[24];
        std::snprintf(note, sizeof(note), "%u/%u/%u/%u", target->GetGUIDLow(),
                      uint32(target->GetClass()), uint32(GetSpecBuffTargetRole(target)),
                      uint32(reason));
        CB_HITN(ai->GetBotPlayer()->GetGUIDLow(), "cpp-buff: target/class/role/reason", note);
        if (groupSpell)
            CB_HITV(ai->GetBotPlayer()->GetGUIDLow(), "cpp-buff: group spell selected", spell->Id);
        else
            CB_HITV(ai->GetBotPlayer()->GetGUIDLow(), "cpp-buff: single spell selected", spell->Id);
    }
}
}

uint8 AiBotAI::GetCombatSpecTab() const
{
    if (!botEntry || botEntry->talentProfileState != PB_TALENT_PROFILE_USABLE ||
        botEntry->specTab > 2)
        return 255;   // cb:fold hot per-update detail
    return botEntry->specTab;
}

CombatBotRoles AiBotAI::GetCombatActiveRole() const
{
    if (SuiCommanderRaid::Owns(me)) return GetRole(); // temporary encounter role
    if (m_offTank && me->IsInCombat())
        return ROLE_TANK;   // an off-tank on duty (UpdateOffTankDuty)
    return GetCombatSpecTab() <= 2 ? botEntry->activeRole : GetRole();
}

bool AiBotAI::HasUsableSpecCombat() const
{
    return GetCombatSpecTab() <= 2;
}

bool AiBotAI::HasFastCombatPolicy() const
{
    return !m_rotation.empty() || HasUsableSpecCombat();
}

// [TACTICS] An area spell whose area would reach a creature that is not in the fight wakes it
// (2026-09-24 telemetry: Frost Nova and Arcane Explosion first-struck an idle Molten Giant,
// Destroyer and Annihilator standing next to the pack being fought). Areas from the spell data.
namespace
{
    std::set<uint32> gKnownRisers;
    std::mutex gRisersLock;
    bool gRisersLoaded = false;

    // Learned risers survive a restart (2026-09-25: the first Core Hound pack after every restart
    // was fought unbalanced until one of them fell): "entry" lines beside mangosd.conf.
    std::string RisersPath()
    {
        std::string conf = sConfig.GetFilename();
        size_t slash = conf.find_last_of('/');
        return (slash == std::string::npos ? std::string() : conf.substr(0, slash + 1)) + "sui-autopilot-risers.txt";
    }

    void LoadRisers()
    {
        if (gRisersLoaded)
            return;
        gRisersLoaded = true;
        std::ifstream in(RisersPath());
        uint32 entry = 0;
        while (in >> entry)
            gKnownRisers.insert(entry);
    }
}

void AiBotAI::NoteRiser(uint32 entry)
{
    std::lock_guard<std::mutex> guard(gRisersLock);
    LoadRisers();
    if (gKnownRisers.insert(entry).second)
        std::ofstream(RisersPath(), std::ios::app) << entry << '\n';
}

namespace
{
    std::set<uint32> gRebounders;
    std::map<ObjectGuid, float> gLowestSeen;
    std::mutex gReboundLock;
    bool gReboundLoaded = false;

    std::string ReboundPath()
    {
        std::string conf = sConfig.GetFilename();
        size_t slash = conf.find_last_of('/');
        return (slash == std::string::npos ? std::string() : conf.substr(0, slash + 1)) + "sui-autopilot-rebounders.txt";
    }

    void LoadRebounders()
    {
        if (gReboundLoaded)
            return;
        gReboundLoaded = true;
        std::ifstream in(ReboundPath());
        uint32 entry = 0;
        while (in >> entry)
            gRebounders.insert(entry);
    }

    // A kind whose own spell list holds a heal (a Flamewaker Priest's Dark Mending), from its data only.
    bool KindHeals(Creature const* c)
    {
        static std::mutex lock;
        static std::map<uint32, bool> cache;
        std::lock_guard<std::mutex> guard(lock);
        auto found = cache.find(c->GetEntry());
        if (found != cache.end())
            return found->second;
        std::vector<uint32> spells;
        if (CreatureInfo const* info = c->GetCreatureInfo())
        {
            if (info->spell_list_id)
                if (CreatureSpellsList const* list = sObjectMgr.GetCreatureSpellsList(info->spell_list_id))
                    for (CreatureSpellsEntry const& e : *list)
                        spells.push_back(e.spellId);
            for (uint32 id : info->spells)
                if (id)
                    spells.push_back(id);
        }
        bool heals = false;
        for (uint32 id : spells)
            if (SpellEntry const* spell = sSpellMgr.GetSpellEntry(id))
                for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
                    if (spell->Effect[i] == SPELL_EFFECT_HEAL || spell->Effect[i] == SPELL_EFFECT_HEAL_MAX_HEALTH)
                        heals = true;
        cache.emplace(c->GetEntry(), heals);
        return heals;
    }
}

void AiBotAI::ObserveRebound(Unit const* mob)
{
    if (!mob || !mob->IsCreature() || !mob->IsAlive())
        return;
    // A creature walking home after a wipe heals whole, and a splitting kind comes back fresh: neither
    // rebounds (2026-11-03: Lava Spawn, Onyxia, Shazzrah and Majordomo's healers had all been learned
    // this way and were left for last).
    // A kind that gets back up from the floor stands at full again too (2026-11-03: Core Hound).
    if (const_cast<Creature*>(static_cast<Creature const*>(mob))->IsInEvadeMode() || !mob->GetVictim() ||
        AiBotAI::IsKnownMultiplier(mob->GetEntry()) || AiBotAI::IsKnownRiser(mob->GetEntry()))
    {
        std::lock_guard<std::mutex> guard(gReboundLock);
        gLowestSeen.erase(mob->GetObjectGuid());
        return;
    }
    std::lock_guard<std::mutex> guard(gReboundLock);
    LoadRebounders();
    if (!mob->IsInCombat())
    {
        gLowestSeen.erase(mob->GetObjectGuid());
        return;
    }
    float const hp = mob->GetHealthPercent();
    auto it = gLowestSeen.find(mob->GetObjectGuid());
    if (it == gLowestSeen.end())
    {
        gLowestSeen[mob->GetObjectGuid()] = hp;
        return;
    }
    if (hp < it->second)
        it->second = hp;
    else if (it->second <= 55.0f && hp >= 90.0f && !gRebounders.count(mob->GetEntry()))
    {
        // Healed back by another creature in the fight is not its own rebound: that healer dies first
        // (2026-11-06: Sulfuron went 30 -> 91 % under his priests' Dark Mending and was learned; with
        // the priests learned the same way in week 1, the kill order skipped all five - wipe).
        std::list<Creature*> kin;
        MaNGOS::AnyUnitInObjectRangeCheck check(mob, 40.0f);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(kin, check);
        Cell::VisitGridObjects(mob, searcher, 40.0f);
        for (Creature* k : kin)
            if (k != mob && k->IsAlive() && k->IsInCombat() && !k->IsHostileTo(mob) && KindHeals(k))
            {
                gLowestSeen.erase(mob->GetObjectGuid());
                return;
            }
        // ...nor is a rebound under an aura another creature put on it: Majordomo's script makes his last add
        // his Champion (21090) at full health, the Elite was learned as healing itself whole and left for last,
        // and the raid beat on Majordomo for 110 s beside it (2026-11-09, attempt 57: seven adds dead).
        for (auto const& entry : mob->GetSpellAuraHolderMap())
            if (entry.second && entry.second->GetCasterGuid() != mob->GetObjectGuid() && entry.second->GetCasterGuid().IsCreature())
            {
                gLowestSeen.erase(mob->GetObjectGuid());
                return;
            }
        gRebounders.insert(mob->GetEntry());
        std::ofstream(ReboundPath(), std::ios::app) << mob->GetEntry() << '\n';
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-REBOUND] %s (entry %u) went from %.0f%% back to %.0f%% - its kind is left for last",
            mob->GetName(), mob->GetEntry(), it->second, hp);
    }
}

// Adds the playbook woke on purpose: no mage sheeps them again while the raid kills them.
static std::mutex gReleasedLock;
static std::map<ObjectGuid, uint32> gReleased;   // add -> until ms
static std::map<ObjectGuid, uint32> gReleasedAt; // add -> when first released

static bool RecentlyReleased(Unit const* u)
{
    std::lock_guard<std::mutex> guard(gReleasedLock);
    auto it = gReleased.find(u->GetObjectGuid());
    return it != gReleased.end() && WorldTimer::getMSTime() < it->second;
}

bool AiBotAI::WasPlaybookReleased(Unit const* u)
{
    return u && RecentlyReleased(u);
}

uint32 AiBotAI::PlaybookReleaseAgeMs(Unit const* u)
{
    std::lock_guard<std::mutex> guard(gReleasedLock);
    auto it = u ? gReleasedAt.find(u->GetObjectGuid()) : gReleasedAt.end();
    return it == gReleasedAt.end() ? 0xFFFFFFFFu : WorldTimer::getMSTime() - it->second;
}

// A tank that can force an enemy onto itself (warrior Taunt, druid Growl). A paladin tank cannot.
bool AiBotAI::TankCanTaunt(Player* p)
{
    return p && (p->HasSpell(355) || p->HasSpell(6795));
}

// Off-tank on demand (owner 2026-11-09: "rather than it be a tank, it's a warrior/druid who OTs"): a melee
// damage dealer with a taunt deals damage until the fight has more awake elites on the group than living
// tanks, then tanks - every tank rule counts it while on duty (GetCombatActiveRole). It goes back to damage
// once the other tanks have been enough for 8 s and nothing holds on it. The 6 s before it steps in let the
// opening's sheeps land (Majordomo's nine adds all come at the puller; four are sheeped within ~3 s).
void AiBotAI::UpdateOffTankDuty()
{
    bool const able = botEntry && GetCombatSpecTab() <= 2 && botEntry->activeRole == ROLE_MELEE_DPS &&
        TankCanTaunt(me) && me->IsAlive() && me->IsInCombat() && me->GetGroup() && !SuiCommanderRaid::Owns(me);
    if (!able)
    {
        if (m_offTank)
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-OFFTANK] %s: off duty (fight over)", me->GetName());
        m_offTank = false;
        m_offTankShortSinceMs = m_offTankEnoughSinceMs = m_offTankLooseSinceMs = m_offTankFightMs = 0;
        m_offTankBoss = false;
        return;
    }
    if (!m_offTankFightMs)
        m_offTankFightMs = WorldTimer::getMSTime();
    Group* group = me->GetGroup();
    // A boss whose adds die in an order (playbook killOrder) wants every tank it can get: the off-tank tanks his
    // whole fight as a full tank. With a 6th tank one is free from the pull, so the sheeped Healers die one after
    // another before any Elite, and none is left to wake immune at the 4th death of eight (2026-11-09, Majordomo:
    // attempts 59-72 all collapsed at that wake-up).
    {
        std::list<Unit*> wide;
        MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck wideCheck(me, me, 100.0f);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> wideSearcher(wide, wideCheck);
        Cell::VisitAllObjects(me, wideSearcher, 100.0f);
        bool ordered = false;
        for (Unit* b : wide)
            if (b->IsCreature() && b->IsAlive() && b->IsInCombat() && static_cast<Creature*>(b)->IsWorldBoss())
                for (Unit* a : wide)
                    if (a != b && a->IsCreature() && a->IsAlive() && SuiRaidPlaybook::KillOrder(b->GetEntry(), a->GetEntry()) >= 0)
                    {
                        ordered = true;
                        break;
                    }
        if (ordered)
        {
            if (!m_offTankBoss)
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-OFFTANK] %s: tanks the whole fight - its boss orders its adds", me->GetName());
            m_offTank = m_offTankBoss = true;
            return;
        }
        m_offTankBoss = false;
    }
    // an enemy a member is sheeping right now is not awake (2026-11-09, Majordomo attempt 53: the off-tank
    // stepped in at +4 s, "7 awake for 5 tanks", while four Polymorphs were still in flight)
    std::set<Unit const*> beingControlled;
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        if (Player* m = itr->getSource())
            if (Spell* s = m->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                if (s->m_spellInfo && (s->m_spellInfo->Mechanic == MECHANIC_POLYMORPH || s->m_spellInfo->Mechanic == MECHANIC_BANISH ||
                    s->m_spellInfo->Mechanic == MECHANIC_SHACKLE || s->m_spellInfo->EffectMechanic[0] == MECHANIC_POLYMORPH))
                    if (Unit* t = CastTarget(me, s))
                        beingControlled.insert(t);
    std::list<Unit*> nearby;
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(me, me, 60.0f);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(nearby, check);
    Cell::VisitAllObjects(me, searcher, 60.0f);
    int elites = 0;
    bool held = false, loose = false, looseBoss = false;
    for (Unit* u : nearby)
    {
        if (!u->IsCreature() || !u->IsAlive() || !u->IsInCombat() || u->HasBreakableByDamageCrowdControlAura() ||
            beingControlled.count(u))
            continue;
        Creature const* c = static_cast<Creature const*>(u);
        if (!c->IsElite() && !c->IsWorldBoss())
            continue;
        Unit* v = u->GetVictim();
        Player* vp = v ? v->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!vp || !group->IsMember(vp->GetObjectGuid()))
            continue;
        ++elites;
        if (v == me)
            held = true;
        // ...or one is loose on someone who is not a tank: Majordomo's threat resets with every teleport and
        // he killed two healers while the five tanks were "enough" (2026-11-09, Majordomo attempt 54)
        else if (v == vp && !IsSpecAoETankHolder(vp))
        {
            loose = true;
            looseBoss = looseBoss || c->IsWorldBoss();
        }
    }
    int tanks = 0;   // the others: an off-tank counts itself out, so it stays while the fight needs it
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* p = itr->getSource();
        if (p && p != me && p->IsAlive() && p->IsInMap(me) && p->IsWithinDistInMap(me, 80.0f) && IsSpecAoETankHolder(p))
            ++tanks;
    }
    bool const shortOfTanks = elites > tanks;
    uint32 const now = WorldTimer::getMSTime();
    if (loose)
    {
        if (!m_offTankLooseSinceMs)
            m_offTankLooseSinceMs = now;
    }
    else
        m_offTankLooseSinceMs = 0;
    if (shortOfTanks || loose)
    {
        m_offTankEnoughSinceMs = 0;
        if (!m_offTankShortSinceMs)
            m_offTankShortSinceMs = now;
    }
    else
    {
        m_offTankShortSinceMs = 0;
        if (!m_offTankEnoughSinceMs)
            m_offTankEnoughSinceMs = now;
    }
    // (the pull's first 8 s belong to crowd control: at +3 s the off-tank took a Healer loose on a priest, the
    // mages scored a held one "don't sheep" and the next Polymorph went out at +14 s - attempt 66)
    if (!m_offTank && ((shortOfTanks && now - m_offTankShortSinceMs >= 6000) ||
        (m_offTankLooseSinceMs && now - m_offTankLooseSinceMs >= (looseBoss ? 1000u : 3000u) && now - m_offTankFightMs >= 8000)))
    {
        m_offTank = true;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-OFFTANK] %s: tanks - %d awake elites on the group for %d tanks%s",
            me->GetName(), elites, tanks, loose ? ", one loose on a non-tank" : "");
    }
    else if (m_offTank && !shortOfTanks && !loose && !held && now - m_offTankEnoughSinceMs >= 8000)
    {
        m_offTank = false;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-OFFTANK] %s: back to damage - %d awake elites for %d tanks",
            me->GetName(), elites, tanks);
    }
}

// A paladin's Lay on Hands on a group member under 15 % that an elite is beating on, a tank first - even
// mid-cast: Holy Light ran 2.5 s of every 3 and the rotation's own check never came round while a tank fell
// from 40 % to dead (2026-11-08, Majordomo attempts 39-43: no Lay on Hands in seventeen tank deaths).
bool AiBotAI::TryLayOnHandsSave()
{
    if (me->GetClass() != CLASS_PALADIN || !me->IsInCombat() || !me->GetGroup())
        return false;
    SpellEntry const* loh = nullptr;
    for (uint32 id : {10310u, 2800u, 633u})
        if (me->HasSpell(id))
        {
            if (me->IsSpellReady(id))
                loh = sSpellMgr.GetSpellEntry(id);
            break;
        }
    if (!loh)
        return false;
    Player* best = nullptr;
    bool bestTank = false;
    for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
    {
        Player* p = itr->getSource();
        if (!p || p == me || !p->IsAlive() || !p->IsInMap(me) || p->GetHealthPercent() >= 15.0f ||
            !me->IsWithinDistInMap(p, 38.0f) || !me->IsWithinLOSInMap(p))
            continue;
        bool held = false;
        for (Unit* a : p->GetAttackers())
            if (a && a->IsCreature() && a->GetVictim() == p && static_cast<Creature*>(a)->IsElite())
                held = true;
        if (!held)
            continue;
        bool const tank = IsSpecAoETankHolder(p);
        if (!best || (tank && !bestTank) || (tank == bestTank && p->GetHealthPercent() < best->GetHealthPercent()))
        {
            best = p;
            bestTank = tank;
        }
    }
    if (!best)
        return false;
    // one paladin's Lay on Hands per dying member (attempt 45: two landed on the same priest in one second)
    static std::mutex lohLock;
    static std::map<ObjectGuid, uint32> lohUntil;
    {
        std::lock_guard<std::mutex> guard(lohLock);
        uint32 const now = WorldTimer::getMSTime();
        auto it = lohUntil.find(best->GetObjectGuid());
        if (it != lohUntil.end() && now < it->second)
            return false;
        lohUntil[best->GetObjectGuid()] = now + 8000;
    }
    float const hp = best->GetHealthPercent();
    bool const wasCasting = me->IsNonMeleeSpellCasted(false, false, true);
    if (wasCasting)
        me->InterruptNonMeleeSpells(false);
    if (DoCastSpell(best, loh) != SPELL_CAST_OK)
    {
        std::lock_guard<std::mutex> guard(lohLock);
        lohUntil.erase(best->GetObjectGuid());
        return false;
    }
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HEAL] %s: Lay on Hands on %s at %.0f%%%s",
        me->GetName(), best->GetName(), hp, wasCasting ? " (cast cut short)" : "");
    return true;
}

Unit* AiBotAI::PlaybookReleaseTarget(Player* me, bool* noFreeTank)
{
    if (noFreeTank)
        *noFreeTank = false;
    if (!me || !me->IsInCombat() || !me->GetGroup())
        return nullptr;
    // (100 yd: a member on an add tanked 50 yd from the boss is still in his fight - 2026-11-09, attempt 63)
    std::list<Unit*> nearby;
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(me, me, 100.0f);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(nearby, check);
    Cell::VisitAllObjects(me, searcher, 100.0f);
    uint32 boss = 0;
    Unit* bossUnit = nullptr;
    for (Unit* u : nearby)
        if (u->IsCreature() && u->IsAlive() && u->IsInCombat() && static_cast<Creature*>(u)->IsWorldBoss())
        {
            boss = u->GetEntry();
            bossUnit = u;
            break;
        }
    if (!boss)
        return nullptr;
    // The adds are counted round the boss, the same for every member: from a caster's own 60 yd an awake
    // add out of reach looked dead, and a hunter shot a sheep "released" at 2 s (2026-11-07, Majordomo).
    nearby.clear();
    {
        MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck around(bossUnit, me, 90.0f);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> all(nearby, around);
        Cell::VisitAllObjects(bossUnit, all, 90.0f);
    }
    // 2026-11-06, Majordomo: his Healers turn immune to Polymorph once half his adds are down, so they
    // die first, one at a time, while the others stay sheeped (four woken at once killed 34 in 35 s).
    int minAwake = 1000;
    Unit* best = nullptr;
    int bestK = 1000;
    for (Unit* u : nearby)
    {
        if (!u->IsCreature() || !u->IsAlive() || !u->IsInCombat())
            continue;
        int const k = SuiRaidPlaybook::KillOrder(boss, u->GetEntry());
        if (k < 0)
            continue;
        if (!u->HasBreakableByDamageCrowdControlAura())
            minAwake = std::min(minAwake, k);
        else if (k < bestK || (k == bestK && best && u->GetGUIDLow() < best->GetGUIDLow()))
        {
            bestK = k;
            best = u;
        }
    }
    // A sheep is woken only for a tank with nothing to hold: every awake enemy (the boss too) already has
    // one (2026-11-08, Majordomo with five tanks: four Elites, the boss and the released Healer were six,
    // the Healer went untanked and Shadow Shocked all eight melee dead in 12 s). Until a tank is free the
    // raid kills the next awake kind in the order, and its death frees one.
    int awake = 0;
    for (Unit* u : nearby)
        if (u->IsCreature() && u->IsAlive() && u->IsInCombat() && !u->HasBreakableByDamageCrowdControlAura())
            ++awake;
    int tanks = 0, taunters = 0, freeTaunters = 0;
    for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
    {
        Player* p = itr->getSource();
        if (!p || !p->IsAlive() || !p->IsInMap(bossUnit) || !p->IsWithinDistInMap(bossUnit, 100.0f))
            continue;
        AiBotAI const* ai = dynamic_cast<AiBotAI const*>(p->AI());
        if (!(ai ? ai->GetCombatActiveRole() == ROLE_TANK : IsTankClass(p->GetClass())))
            continue;
        bool holds = false;
        for (Unit* u : nearby)
            if (u->IsAlive() && u->IsInCombat() && !u->HasBreakableByDamageCrowdControlAura() && u->GetVictim() == p)
            {
                holds = true;
                break;
            }
        // An off-tank counts only for what it holds and is never a free tank: on duty for a moment it let a
        // Healer go at +5 s, then went back to damage with nobody for it (2026-11-09, Majordomo attempt 53).
        bool const offTank = ai && ai->IsOffTankOnDuty();
        if (offTank && !holds)
            continue;
        ++tanks;
        if (!TankCanTaunt(p) || offTank)
            continue;
        ++taunters;
        if (!holds)
            ++freeTaunters;
    }
    // ...and that tank can taunt: a paladin tank (no taunt, little threat) kept a Healer he woke on him 2-17 %
    // of its life (2026-11-08, Majordomo attempts 42-43). While every taunting tank holds something the sheep
    // sleeps on; the free paladin takes the boss meanwhile (TankDuty), whose threat resets every 20-30 s.
    bool const noTaunter = taunters > 0 && freeTaunters == 0;
    if (noFreeTank)
        *noFreeTank = tanks <= awake;
    if (!best || bestK >= minAwake)
        return nullptr;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> guard(gReleasedLock);
        static uint32 holdLogMs = 0;
        uint32 const now = WorldTimer::getMSTime();
        auto it = gReleased.find(best->GetObjectGuid());
        bool const already = it != gReleased.end() && now < it->second;
        if ((tanks <= awake || noTaunter) && !already)
        {
            if (now - holdLogMs > 10000)
            {
                holdLogMs = now;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-RELEASE] %s (guid %u) stays sheeped: %d tanks for %d awake, %d of %d taunting tanks free",
                    best->GetName(), best->GetGUIDLow(), tanks, awake, freeTaunters, taunters);
            }
            return nullptr;
        }
        fresh = !already;
        // (10 min, not 60 s: the record is refreshed only while the add sleeps, and a woken Healer that took 117 s to
        // die stopped counting as released at 60 s - the kill order's floor no longer held its Elites, 2026-11-09,
        // Majordomo attempt 77)
        gReleased[best->GetObjectGuid()] = now + 600000;
        if (fresh)
            gReleasedAt[best->GetObjectGuid()] = now;
    }
    if (fresh)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-RELEASE] %s (guid %u) released: %d tanks for %d awake",
            best->GetName(), best->GetGUIDLow(), tanks, awake);
    return best;
}

bool AiBotAI::IsKnownRebounder(uint32 entry)
{
    std::lock_guard<std::mutex> guard(gReboundLock);
    LoadRebounders();
    return gRebounders.count(entry) != 0;
}

namespace
{
    std::set<uint32> gMultipliers;
    std::mutex gMultiplierLock;
    bool gMultipliersLoaded = false;

    std::string MultipliersPath()
    {
        std::string conf = sConfig.GetFilename();
        size_t slash = conf.find_last_of('/');
        return (slash == std::string::npos ? std::string() : conf.substr(0, slash + 1)) + "sui-autopilot-multipliers.txt";
    }

    void LoadMultipliers()
    {
        if (gMultipliersLoaded)
            return;
        gMultipliersLoaded = true;
        std::ifstream in(MultipliersPath());
        uint32 entry = 0;
        while (in >> entry)
            gMultipliers.insert(entry);
    }
}

void AiBotAI::ObserveMultiplier(Unit const* mob)
{
    // 2026-11-03: a Firelord's Lava Spawns split every 10 s in combat; the raid spread 172k damage
    // over 122 of them, killed none, and 175 of them buried it.
    if (!mob || !mob->IsCreature() || !static_cast<Creature const*>(mob)->IsTemporarySummon())
        return;
    ObjectGuid const summoner = static_cast<TemporarySummon const*>(mob)->GetSummonerGuid();
    if (!summoner.IsCreature() || summoner.GetEntry() != mob->GetEntry())
        return;
    std::lock_guard<std::mutex> guard(gMultiplierLock);
    LoadMultipliers();
    if (gMultipliers.insert(mob->GetEntry()).second)
    {
        std::ofstream(MultipliersPath(), std::ios::app) << mob->GetEntry() << '\n';
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-MULTIPLY] %s (entry %u) is summoned by its own kind - it dies first, oldest first",
            mob->GetName(), mob->GetEntry());
    }
}

bool AiBotAI::IsKnownMultiplier(uint32 entry)
{
    std::lock_guard<std::mutex> guard(gMultiplierLock);
    LoadMultipliers();
    return gMultipliers.count(entry) != 0;
}

bool AiBotAI::IsKnownRiser(uint32 entry)
{
    std::lock_guard<std::mutex> guard(gRisersLock);
    LoadRisers();
    return gKnownRisers.count(entry) != 0;
}

// [TACTICS] A pack that gets back up must reach the floor together: an area spell is held while
// it would put a low one down early (its kin still well above it).
static bool AreaWouldFloorRiserEarly(Player* me, Unit* target, SpellEntry const* spell)
{
    bool area = false;
    float radius = 0.0f;
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        uint32 const a = spell->EffectImplicitTargetA[i], b = spell->EffectImplicitTargetB[i];
        if (a == TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC || b == TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC ||
            a == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DEST_LOC || b == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DEST_LOC ||
            a == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DYNOBJ_LOC || b == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DYNOBJ_LOC ||
            a == TARGET_ENUM_UNITS_ENEMY_IN_CONE_24 || b == TARGET_ENUM_UNITS_ENEMY_IN_CONE_24)
        {
            area = true;
            if (SpellRadiusEntry const* r = sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]))
                radius = std::max(radius, Spells::GetSpellRadius(r));
        }
    }
    if (!area)
        return false;
    std::list<Unit*> near;
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(me, me, 45.0f);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(near, check);
    Cell::VisitAllObjects(me, searcher, 45.0f);
    for (Unit* low : near)
    {
        if (!low->IsCreature() || !low->IsAlive() || !low->IsInCombat() || !AiBotAI::IsKnownRiser(low->GetEntry()) ||
            low->GetHealthPercent() > 20.0f || low->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD))
            continue;
        WorldObject const* center = target ? static_cast<WorldObject const*>(target) : me;
        if (radius > 0.0f && !low->IsWithinDist(center, radius + 2.0f, false))
            continue;
        for (Unit* kin : near)
            if (kin != low && kin->IsCreature() && kin->IsAlive() && kin->GetEntry() == low->GetEntry() &&
                !kin->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD) && kin->GetHealthPercent() > 20.0f)
                return true;
    }
    return false;
}

// [TACTICS] No damage-over-time on a pack whose fallen get back up: a DoT keeps ticking after the
// damage dealers moved on and floors that one alone, and the pack's clock starts early.
static bool DotOnRiser(Unit* target, SpellEntry const* spell)
{
    if (!target || !target->IsCreature() || target == nullptr || !AiBotAI::IsKnownRiser(target->GetEntry()))
        return false;
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE ||
            spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_LEECH ||
            spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE_PERCENT)
            return true;
    return false;
}

static bool AreaWouldWakeIdle(Player* me, Unit* target, SpellEntry const* spell)
{
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (!spell->Effect[i])
            continue;
        uint32 const a = spell->EffectImplicitTargetA[i], b = spell->EffectImplicitTargetB[i];
        bool const atSource = a == TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC || b == TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC ||
            a == TARGET_ENUM_UNITS_ENEMY_IN_CONE_24 || b == TARGET_ENUM_UNITS_ENEMY_IN_CONE_24;
        bool const atDest = a == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DEST_LOC || b == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DEST_LOC ||
            a == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DYNOBJ_LOC || b == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DYNOBJ_LOC;
        if (!atSource && !atDest)
            continue;
        SpellRadiusEntry const* radiusEntry = sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]);
        float const radius = radiusEntry ? Spells::GetSpellRadius(radiusEntry) : 0.0f;
        if (radius <= 0.0f)
            continue;
        WorldObject* center = (atDest && target) ? static_cast<WorldObject*>(target) : static_cast<WorldObject*>(me);
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(center, radius + 3.0f);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(center, searcher, radius + 3.0f);
        for (Creature* c : near)
            if (c->IsAlive() && !c->IsInCombat() && !c->IsTotem() && !c->IsCivilian() &&
                c->GetCreatureType() != CREATURE_TYPE_CRITTER && me->IsValidAttackTarget(c))
                return true;
    }
    return false;
}

bool AiBotAI::TrySpecSpell(Unit* target, SpellEntry const* spell)
{
    if (!target)
        return false;   // cb:fold rotation rung, outcome probed at cast
    if (!spell)
    {
        SuiRaidTelemetry::OnCastAttempt(me, 0, -2);
        return false;   // cb:fold rotation rung, outcome probed at cast
    }
    if (!CanTryToCastSpell(target, spell))
    {
        SuiRaidTelemetry::OnCastAttempt(me, spell->Id, -1);
        return false;   // cb:fold rotation rung, outcome probed at cast
    }
    // No magic into an enemy that reflects it: it comes back at me - a Polymorph sheeps the mage
    // (2026-11-06, Majordomo's Magic Reflection on his adds, 10 s of every 30).
    if (target != me && spell->DmgClass == SPELL_DAMAGE_CLASS_MAGIC && me->IsValidAttackTarget(target) &&
        (target->HasAuraType(SPELL_AURA_REFLECT_SPELLS) || target->HasAuraType(SPELL_AURA_REFLECT_SPELLS_SCHOOL)))
    {
        SuiRaidTelemetry::OnCastAttempt(me, spell->Id, -4);
        return false;   // cb:fold rotation rung, outcome probed at cast
    }
    if (me->GetGroup() && (AreaWouldWakeIdle(me, target, spell) || AreaWouldFloorRiserEarly(me, target, spell) ||
        DotOnRiser(target, spell)))
    {
        SuiRaidTelemetry::OnCastAttempt(me, spell->Id, -3);
        return false;   // cb:fold rotation rung, outcome probed at cast
    }
    SpellCastResult const result = DoCastSpell(target, spell);
    SuiRaidTelemetry::OnCastAttempt(me, spell->Id, int32(result == SPELL_CAST_OK ? 0 : result));
    if (result == SPELL_FAILED_LINE_OF_SIGHT)
    {
        m_losRefusedGuid = target->GetObjectGuid();
        m_losRefusedMs = WorldTimer::getMSTime();
    }
    if (result != SPELL_CAST_OK)
        return false;   // cb:fold rotation rung, outcome probed at cast
    CB_HITV(me->GetGUIDLow(), "cpp-spec: spec ladder winner cast", spell->Id);
    return true;
}

bool AiBotAI::TrySpecSpell(Unit* target, uint32 firstRankSpellId)
{
    return TrySpecSpell(target, GetHighestKnownRank(firstRankSpellId));
}

bool AiBotAI::HasAuraFromSpellChain(Unit const* target, uint32 firstRankSpellId) const
{
    if (!target)
        return false;   // cb:fold hot per-update detail
    for (auto const& itr : target->GetSpellAuraHolderMap())
        if (itr.second && itr.second->GetSpellProto() &&
            sSpellMgr.GetFirstSpellInChain(itr.second->GetSpellProto()->Id) == firstRankSpellId)
            return true;   // cb:fold hot per-update detail
    return false;
}

bool AiBotAI::TrySpecAura(Unit* target, uint32 firstRankSpellId)
{
    return target && !HasAuraFromSpellChain(target, firstRankSpellId) &&
           TrySpecSpell(target, firstRankSpellId);
}

bool AiBotAI::TrySpecStackingAura(Unit* target, uint32 firstRankSpellId)
{
    SpellEntry const* spell = GetHighestKnownRank(firstRankSpellId);
    if (!target || !spell || !CanTryToCastStackingSpell(target, spell))
        return false;   // cb:fold rotation rung, outcome probed at cast
    if (DoCastSpell(target, spell) != SPELL_CAST_OK)
        return false;   // cb:fold rotation rung, outcome probed at cast
    CB_HITV(me->GetGUIDLow(), "cpp-spec: stacking aura winner cast", spell->Id);
    return true;
}

// Whether a player holding an enemy is a tank whose aggro a splash must respect.  A bot
// answers with its active role (the encounter's temporary role while one owns it); a
// human-driven body counts as a tank when its class can tank.  Pets and creatures never.
bool AiBotAI::IsSpecAoETankHolder(Unit const* holder) const
{
    Player const* player = holder ? holder->ToPlayer() : nullptr;
    if (!player)
        return false;   // cb:fold hot per-update detail
    if (AiBotAI const* ai = dynamic_cast<AiBotAI const*>(const_cast<Player*>(player)->AI()))
        return ai->GetCombatActiveRole() == ROLE_TANK;   // cb:fold hot per-update detail
    return IsTankClass(player->GetClass());
}

// Raid aggro practice (2026-09-22): a damage dealer splashes an enemy only while it stays
// under kSpecAoEThreatShare of the tank's threat on it; the game moves aggro at 110 % in
// melee and 130 % at range.  An enemy that is already loose on a non-tank has no tank to
// protect and dies best to the splash.  The old margin (tank >= mine + my whole health)
// refused every fresh add and summon, so no AoE ever went out on them.
bool AiBotAI::IsSpecAoEThreatSafe(Unit* enemy) const
{
    static float const kSpecAoEThreatShare = 0.8f;
    Unit* holder = enemy->GetVictim();
    if (!holder || holder == me || !enemy->CanHaveThreatList() || !IsSpecAoETankHolder(holder))
        return true;   // cb:fold hot per-update detail
    float const mine = enemy->GetThreatManager().getThreat(me);
    float const held = enemy->GetThreatManager().getThreat(holder);
    return mine < held * kSpecAoEThreatShare;
}

namespace
{
    float const kSplashCcWander = 6.0f;

    // An enemy a group member is putting to sleep right now (Polymorph, Shackle, Hibernate, Sap, Gouge in
    // flight): a splash cast during that cast lands on the sheep (2026-11-08, Majordomo: one Blizzard at
    // 2 s broke two fresh sheeps - it went out while the Polymorphs were still being cast).
    bool CrowdControlBeingCastNear(Player* me, float x, float y, float radius)
    {
        uint32 const ccMask = (1u << (MECHANIC_POLYMORPH - 1)) | (1u << (MECHANIC_SHACKLE - 1)) |
            (1u << (MECHANIC_SLEEP - 1)) | (1u << (MECHANIC_SAPPED - 1)) | (1u << (MECHANIC_KNOCKOUT - 1));
        Group* group = me->GetGroup();
        if (!group)
            return false;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* m = itr->getSource();
            if (!m || !m->IsAlive() || !m->IsInMap(me))
                continue;
            Spell* cast = m->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!cast || !cast->m_spellInfo || !(cast->m_spellInfo->GetSpellMechanicMask(7) & ccMask))
                continue;
            Unit* t = CastTarget(me, cast);
            if (t && t->IsAlive() && t->GetDistance2d(x, y) < radius + t->GetObjectBoundingRadius())
                return true;
        }
        return false;
    }

    bool CrowdControlledEnemyNear(Player* me, float x, float y, float radius)
    {
        if (CrowdControlBeingCastNear(me, x, y, radius))
            return true;
        std::list<Unit*> enemies;
        me->GetEnemyListInRadiusAround(me, 50.0f, enemies);
        for (Unit* u : enemies)
            if (u && u->IsAlive() && u->HasBreakableByDamageCrowdControlAura() &&
                u->GetDistance2d(x, y) < radius + u->GetObjectBoundingRadius())
                return true;
        return false;
    }

    // A splash already running when a crowd-controlled enemy comes into it: a channelled area
    // (Blizzard, Rain of Fire, Hurricane, Volley) is stopped, a melee splash buff (Blade Flurry,
    // Sweeping Strikes) is dropped. The gate above only sees the moment of the cast; the area runs
    // 8 s and the buff 15 s while the sheep walks about (2026-11-06, Majordomo: a broken Healer
    // runs loose with no tank free, and its Shadow Shock reaches 20 yd).
    void StopSplashNearCrowdControl(Player* me)
    {
        if (Spell* channel = me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            if (SpellEntry const* spell = channel->m_spellInfo)
                if (channel->m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)
                {
                    float radius = 0.0f;
                    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
                        if (SpellRadiusEntry const* entry = sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]))
                            radius = std::max(radius, Spells::GetSpellRadius(entry));
                    if (radius > 0.0f && CrowdControlledEnemyNear(me, channel->m_targets.m_destX,
                            channel->m_targets.m_destY, radius + 2.0f))
                    {
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-CC] %s: stops %s - a crowd-controlled enemy is in it",
                            me->GetName(), spell->SpellName[0].c_str());
                        me->InterruptSpell(CURRENT_CHANNELED_SPELL);
                    }
                }
        for (uint32 buff : { uint32(SP_BLADE_FLURRY), uint32(SP_SWEEPING_STRIKES) })
            if (me->HasAura(buff) && CrowdControlledEnemyNear(me, me->GetPositionX(), me->GetPositionY(),
                    8.0f + me->GetObjectBoundingRadius()))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-CC] %s: drops splash buff %u - a crowd-controlled enemy is in reach",
                    me->GetName(), buff);
                me->RemoveAurasDueToSpell(buff);
            }
    }

    // Consecration lies on the ground for 8 s while a sheep wanders and the tank's mob moves it: the
    // cast-time check (area + 6 yd) is too short (2026-11-08, Majordomo: a Protection paladin's
    // Consecration broke two fresh sheeps 16 s into the pull).
    bool ConsecrationNearSheep(Player* me)
    {
        return CrowdControlledEnemyNear(me, me->GetPositionX(), me->GetPositionY(), 20.0f);
    }

    // Multi-Shot jumps up to 10 yd from each target to the next, three targets: a sheep 20 yd from the
    // first is in reach (2026-11-08, Majordomo: a Multi-Shot broke a sleeping Healer with every splash
    // check at 16 yd passing).
    bool MultiShotNearSheep(Player* me, Unit* target)
    {
        return CrowdControlledEnemyNear(me, target->GetPositionX(), target->GetPositionY(),
            2.0f * CHAIN_SPELL_JUMP_RADIUS + kSplashCcWander);
    }

    // A channelled area of a group member (Blizzard, Rain of Fire, Volley) over this enemy: a sheep
    // landed in it breaks at the next tick, before the channel's own stop sees it (2026-11-07,
    // Majordomo: one Blizzard broke two fresh sheeps at 7 s).
    bool InGroupChannelArea(Player* me, Unit* u)
    {
        Group* group = me->GetGroup();
        if (!group)
            return false;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* m = itr->getSource();
            if (!m || !m->IsAlive() || !m->IsInMap(me))
                continue;
            Spell* channel = m->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            if (!channel || !channel->m_spellInfo || !(channel->m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION))
                continue;
            float radius = 0.0f;
            for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
                if (SpellRadiusEntry const* entry = sSpellRadiusStore.LookupEntry(channel->m_spellInfo->EffectRadiusIndex[i]))
                    radius = std::max(radius, Spells::GetSpellRadius(entry));
            if (radius > 0.0f && u->GetDistance2d(channel->m_targets.m_destX, channel->m_targets.m_destY) <
                    radius + 2.0f + u->GetObjectBoundingRadius())
                return true;
        }
        return false;
    }

    // a spell that hurts its target (a cast of Polymorph on a sheep is its renewal, not a break)
    bool DamagingSpell(SpellEntry const* spell)
    {
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (spell->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE || spell->Effect[i] == SPELL_EFFECT_WEAPON_DAMAGE ||
                spell->Effect[i] == SPELL_EFFECT_NORMALIZED_WEAPON_DMG || spell->Effect[i] == SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL ||
                spell->Effect[i] == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE ||
                (spell->Effect[i] == SPELL_EFFECT_APPLY_AURA &&
                 (spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE ||
                  spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_LEECH)))
                return true;
        return false;
    }
}

bool AiBotAI::CanUseSpecAoE(Unit* center, float radius, uint32 minimumTargets) const
{
    if (!center)
        return false;   // cb:fold hot per-update detail

    std::list<Unit*> enemies;
    // A crowd-controlled enemy is looked for a little past the area: a sheep wanders while it
    // waits (2026-11-06, Majordomo: 68 of 89 broken sheeps were splashes - Blade Flurry, Blizzard,
    // Multi-Shot, Frost Nova, Rain of Fire - mostly a sheep that walked in after the check).
    me->GetEnemyListInRadiusAround(center, radius + kSplashCcWander, enemies);
    bool const tank = GetCombatActiveRole() == ROLE_TANK;
    if (CrowdControlBeingCastNear(me, center->GetPositionX(), center->GetPositionY(), radius + kSplashCcWander))
        return false;   // cb:fold hot per-update detail
    uint32 valid = 0;
    for (Unit* enemy : enemies)
    {
        if (!enemy || !enemy->IsAlive() || !me->IsValidAttackTarget(enemy))
            continue;   // cb:fold hot per-update detail
        // Never splash an assigned/breakable CC target, an uninvolved pack, or a unit the
        // active encounter keeps this member off.
        if (enemy->HasBreakableByDamageCrowdControlAura())
            return false;   // cb:fold hot per-update detail
        if (enemy != center && !center->IsWithinDistInMap(enemy, radius))
            continue;   // cb:fold hot per-update detail
        if (!enemy->IsInCombat() || SuiCommanderRaid::ForbidsSplash(me, enemy))
            return false;   // cb:fold hot per-update detail
        // In combat but not swinging at anyone yet (a fresh summon or split choosing its
        // victim): it neither blocks the splash nor counts toward it.
        if (!enemy->GetVictim())
            continue;   // cb:fold hot per-update detail
        // A kind that heals itself whole is no target for a splash either: the damage is lost
        // (2026-11-05, Golemagg: his two Core Ragers made three targets, and every mage channelled
        // Blizzard and every warlock Rain of Fire for ~80 s of a 199 s fight - 170 dps each).
        if (enemy->IsCreature() && IsKnownRebounder(enemy->GetEntry()))
            continue;   // cb:fold hot per-update detail
        if (!tank && !IsSpecAoEThreatSafe(enemy))
            return false;   // cb:fold hot per-update detail
        ++valid;
    }
    return valid >= minimumTargets;
}

Unit* AiBotAI::SelectSafeSpecAdd(Unit const* primary) const
{
    // The active encounter owns add assignments and crowd-control decisions.
    if (SuiCommanderRaid::Owns(me)) return nullptr;
    Unit* add = SelectAttackerDifferentFrom(primary);
    if (!add || !IsValidHostileTarget(add) || add->HasBreakableByDamageCrowdControlAura())
        return nullptr;   // cb:fold hot per-update detail
    if (add->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE) ||
        add->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT) ||
        add->HasAuraType(SPELL_AURA_PERIODIC_LEECH))
        return nullptr;   // cb:fold hot per-update detail
    if (me->GetGroup() && AreOthersOnSameTarget(add->GetObjectGuid()))
        return nullptr;   // cb:fold hot per-update detail
    return add;
}

bool AiBotAI::TrySpecInterrupt(Unit* target, std::initializer_list<uint32> spellIds)
{
    if (!target || !target->IsNonMeleeSpellCasted())
        return false;   // cb:fold rotation rung, outcome probed at cast
    for (uint32 id : spellIds)
        if (TrySpecSpell(target, id))
            return true;   // cb:fold rotation rung, outcome probed at cast
    return false;
}

// [TACTICS] An interrupt is for any enemy near me casting at the raid, not only my own target: the
// damage dealers all hit one kill target while the tanked adds cast freely (2026-11-08, Majordomo: the
// Elites' Fireball, Fire Blast and Blast Wave did ~130k a fight; a Counterspell locks an Elite's fire
// school for 10 s). One interrupt per cast: the others keep theirs for the next one.
bool AiBotAI::TrySpecInterruptNearby(std::initializer_list<uint32> spellIds, float range)
{
    if (!me->IsInCombat())
        return false;
    bool ready = false;
    for (uint32 id : spellIds)
        if (me->HasSpell(id) && me->IsSpellReady(id))
            ready = true;
    if (!ready)
        return false;
    static std::map<ObjectGuid, uint32> claims;
    static std::mutex claimsLock;
    std::list<Unit*> around;
    me->GetEnemyListInRadiusAround(me, range, around);
    uint32 const now = WorldTimer::getMSTime();
    for (Unit* u : around)
    {
        if (!u || !u->IsAlive() || !u->IsCreature() || !u->IsInCombat() || u->HasBreakableByDamageCrowdControlAura())
            continue;
        Spell* cast = u->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!cast)
            cast = u->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!cast || !cast->m_spellInfo || cast->m_spellInfo->PreventionType != SPELL_PREVENTION_TYPE_SILENCE)
            continue;
        {
            std::lock_guard<std::mutex> guard(claimsLock);
            auto c = claims.find(u->GetObjectGuid());
            if (c != claims.end() && now < c->second)
                continue;
        }
        if (!me->IsWithinLOSInMap(u))
            continue;
        for (uint32 id : spellIds)
            if (TrySpecSpell(u, id))
            {
                {
                    std::lock_guard<std::mutex> guard(claimsLock);
                    claims[u->GetObjectGuid()] = now + 1500;
                }
                if (now >= m_interruptLogMs)
                {
                    m_interruptLogMs = now + 10000;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-INTERRUPT] %s: interrupts %s's %s (%.0f yd)",
                        me->GetName(), u->GetName(), cast->m_spellInfo->SpellName[0].c_str(), me->GetDistance(u));
                }
                return true;
            }
    }
    return false;
}

bool AiBotAI::TrySpecTaunt(Unit* target)
{
    if (!target || GetCombatActiveRole() != ROLE_TANK || target->GetVictim() == me)
        return false;   // cb:fold rotation rung, outcome probed at cast
    // [TACTICS] Never taunt a mob off another tank who is holding it (owner 2026-09-23).
    // A taunt is for a mob on a healer or a caster, or a tank that is going down.
    if (Unit* holder = target->GetVictim())
        if (holder != me && holder->IsAlive() && holder->GetHealthPercent() > 25.0f &&
            IsSpecAoETankHolder(holder))
        {
            // ...unless that tank holds a stack (three or more on it) and this is not its own
            // target: the stack is split (owner 2026-09-23: "unless there's a stack").
            uint32 onHolder = 0;
            for (Unit* a : holder->GetAttackers())
                if (a && a->IsAlive() && a->GetVictim() == holder)
                    ++onHolder;
            if (onHolder < 3 || holder->GetVictim() == target)
                return false;   // cb:fold rotation rung, outcome probed at cast
        }
    for (SpellEntry const* taunt : m_spellListTaunt)
        if (TrySpecSpell(target, taunt))
            return true;   // cb:fold rotation rung, outcome probed at cast
    // (diagnostic 2026-11-10: Majordomo verification V5 - Taunt "gate" 104 times, never cast, while woken Healers turned on
    // the casters; which check refuses it)
    uint32 const nowMs = WorldTimer::getMSTime();
    if (!m_spellListTaunt.empty() && nowMs >= m_tauntWhyLogMs && target->IsCreature())
    {
        m_tauntWhyLogMs = nowMs + 5000;
        SpellEntry const* t = m_spellListTaunt.front();
        Unit* holder = target->GetVictim();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TAUNT] %s: %s on %s (on %s) not cast: ready %d gcd %d form %d immune %d reach %d dist %.1f",
            me->GetName(), t->SpellName[0].c_str(), target->GetName(), holder ? holder->GetName() : "nobody",
            me->IsSpellReady(t->Id), me->HasGCD(t), uint32(t->GetErrorAtShapeshiftedCast(me->GetShapeshiftForm())),
            target->IsImmuneToSpell(t, false), me->CanReachWithMeleeSpellAttack(target), me->GetDistance(target));
    }
    return false;
}

bool AiBotAI::CommandSpecPet(Unit* target, bool mendHunterPet)
{
    Pet* pet = me->GetPet();
    if (!pet || !pet->IsAlive())
        return false;   // cb:fold hot per-update detail
    if (me->GetGroup() && pet->GetCharmInfo() && pet->GetCharmInfo()->GetReactState() == REACT_AGGRESSIVE)
        pet->GetCharmInfo()->SetReactState(REACT_DEFENSIVE);

    if (mendHunterPet && pet->GetHealthPercent() < 55.0f &&
        me->GetHealthPercent() > 35.0f && TrySpecSpell(pet, SP_MEND_PET))
        return true;   // cb:fold hot per-update detail

    if (target && (!pet->GetVictim() || pet->GetVictim() != target) && pet->GetCharmInfo())
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-spec: pet ordered onto target", target->GetEntry());
        pet->GetCharmInfo()->SetIsCommandAttack(true);
        pet->AI()->AttackStart(target);
    }
    return false;
}

// [TACTICS] A raider drinks: a mana potion when the mana runs low, a healing potion when the
// health does (2026-09-24: the mages stood at 180-410 mana through a Core Hound fight, every
// Frostbolt and Blizzard refused). What an item does comes from its spell data.
bool AiBotAI::TryCombatPotion()
{
    uint32 const now = WorldTimer::getMSTime();
    if (now < m_potionNextMs || !me->IsInCombat() || me->IsNonMeleeSpellCasted(false))
        return false;
    m_potionNextMs = now + 1000;
    bool const needMana = me->GetPowerType() == POWER_MANA && me->GetMaxPower(POWER_MANA) > 0 &&
        me->GetPowerPercent(POWER_MANA) < 25.0f;
    bool const needHealth = me->GetHealthPercent() < 30.0f;
    // A protection potion the quartermaster handed out (a school's damage absorbed) goes back up
    // once it has been burned through: from 3.4k health one Fire Blossom kills a caster
    // (2026-09-25, Geddon's room). Health and mana emergencies come first.
    // The potions share one cooldown: a healer's goes to mana, and a caster tops its shield up only while
    // its mana is high (2026-11-07, Majordomo: every Major Mana Potion was refused on cooldown - the
    // Fire Protection top-ups had taken it - and nine healers ran dry with ten potions each).
    bool const manaUser = me->GetPowerType() == POWER_MANA && me->GetMaxPower(POWER_MANA) > 0;
    bool const needShield = !needHealth && !needMana && me->GetHealthPercent() < 85.0f &&
        (!manaUser || (GetCombatActiveRole() != ROLE_HEALER && me->GetPowerPercent(POWER_MANA) >= 60.0f));
    if (!needMana && !needHealth && !needShield)
        return false;
    // (diagnostic 2026-11-07: healers at 1 % mana carried ten Major Mana Potions untouched)
    std::string why;
    auto consider = [&](Item* item) -> bool
    {
        if (!item)
            return false;
        ItemPrototype const* proto = item->GetProto();
        // (runes are trade goods: a Demonic Rune's mana has its own cooldown beside the potions)
        if (!proto || (proto->Class != ITEM_CLASS_CONSUMABLE && proto->Class != ITEM_CLASS_TRADE_GOODS))
            return false;
        for (auto const& s : proto->Spells)
        {
            if (!s.SpellId || s.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;
            SpellEntry const* spell = sSpellMgr.GetSpellEntry(s.SpellId);
            if (!spell)
                continue;
            bool mana = false, heal = false, shield = false, hurts = false;
            for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                if (spell->Effect[i] == SPELL_EFFECT_ENERGIZE && spell->EffectMiscValue[i] == POWER_MANA)
                    mana = true;
                if (spell->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE)
                    hurts = true;   // a rune's price in health
                if (spell->Effect[i] == SPELL_EFFECT_HEAL)
                    heal = true;
                // a school shield, and only for the school that is hurting me right now
                if (spell->Effect[i] == SPELL_EFFECT_APPLY_AURA && spell->EffectApplyAuraName[i] == SPELL_AURA_SCHOOL_ABSORB &&
                    (uint32(spell->EffectMiscValue[i]) & SuiRaidTelemetry::RecentDamageSchools(me)))
                    shield = true;
            }
            if (shield && me->HasAura(spell->Id))
                continue;   // still up
            if (hurts && me->GetHealthPercent() < 60.0f)
                continue;
            if (!((needMana && mana) || (needHealth && heal) || (needShield && shield)))
                continue;
            if (!me->IsSpellReady(*spell, proto))
            {
                if (mana)
                    why += " " + std::to_string(spell->Id) + ":cooldown";
                continue;
            }
            SpellCastResult const res = me->CastSpell(me, spell, false, item);
            if (res == SPELL_CAST_OK)
            {
                m_potionNextMs = now + 3000;
                return true;
            }
            if (mana)
                why += " " + std::to_string(spell->Id) + ":cast" + std::to_string(uint32(res));
        }
        return false;
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (consider(me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            return true;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* b = dynamic_cast<Bag*>(me->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
            for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                if (consider(b->GetItemByPos(uint8(slot))))
                    return true;
    if (needMana && now >= m_potionWhyLogMs)
    {
        m_potionWhyLogMs = now + 10000;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-POTION] %s: mana %.0f%% and nothing drunk:%s",
            me->GetName(), me->GetPowerPercent(POWER_MANA), why.empty() ? " no mana item found" : why.c_str());
    }
    return false;
}

// Called on every 4 Hz combat sub-tick, mid-cast included (the spec rotation itself is not).
bool AiBotAI::StopNearlyFullHeal()
{
    // A direct heal about to land on someone who no longer needs most of it is stopped in its last half second - mana is
    // spent only when a cast completes, so the stop costs a moment, not the spell (2026-11-10, Ragnaros: overheal 40-50 %
    // per healer while the healers spent up to 65 s of a 146 s fight out of mana). The melee about to land on the target
    // counts as missing already, so a tank's heal before the next blow goes through.
    Spell* cast = me->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (cast)
        if (cast->m_spellInfo && cast->getState() == SPELL_STATE_PREPARING && cast->GetCastedTime() > 0 &&
            cast->GetCastedTime() <= 500)
            if (Unit* to = CastTarget(me, cast))
                if (to->IsAlive() && !to->IsHostileTo(me))
                {
                    int32 heal = 0;
                    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
                        if (cast->m_spellInfo->Effect[i] == SPELL_EFFECT_HEAL)
                            heal += cast->m_spellInfo->EffectBasePoints[i] + 1;
                    int32 const missing = int32(to->GetMaxHealth() - to->GetHealth()) + GetIncomingdamage(to) -
                        IncomingGroupHeals(to);
                    if (heal > 0 && missing * 2 < heal)
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-spec: heal stopped, target nearly full");
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HEALSTOP] %s: %s on %s stopped - missing %d of %d",
                            me->GetName(), cast->m_spellInfo->SpellName[0].c_str(), to->GetName(), missing, heal);
                        me->InterruptSpell(CURRENT_GENERIC_SPELL);
                        return true;
                    }
                }
    return false;
}

bool AiBotAI::UpdateSpecCombatAI()
{
    uint8 const spec = GetCombatSpecTab();
    if (spec > 2)
        return false;   // cb:fold hot per-update detail
    if (TryCombatPotion())
        return true;   // cb:fold hot per-update detail

    // A damaging cast on its way at a unit that has been crowd-controlled since it began is stopped,
    // whatever my victim is now (2026-11-08, Majordomo: a warlock's Immolate landed on a Healer two
    // seconds after a mage re-sheeped it).
    for (CurrentSpellTypes type : { CURRENT_AUTOREPEAT_SPELL, CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
        if (Spell* cast = me->GetCurrentSpell(type))
            if (Unit* at = CastTarget(me, cast))
                if (at != me && at->IsAlive() && at->HasBreakableByDamageCrowdControlAura() &&
                    at != PlaybookReleaseTarget(me) && cast->m_spellInfo && DamagingSpell(cast->m_spellInfo))
                    me->InterruptSpell(type);

    // The 250 ms action lane must honor a CC decision immediately rather than
    // waiting for the one-second doctrine tick.  Stop both white swings and the
    // commanded pet; the normal target authority will select a legal target.
    if (Unit* victim = me->GetVictim())
    {   // cb:fold hot per-update detail
        if (victim->HasBreakableByDamageCrowdControlAura() && victim != PlaybookReleaseTarget(me))
        {   // cb:fold hot per-update detail
            CB_HITV(me->GetGUIDLow(), "cpp-spec: halt, victim under breakable cc", victim->GetEntry());
            if (Pet* pet = me->GetPet())
                if (pet->GetVictim() == victim)   // cb:fold hot per-update detail
                    pet->AttackStop();   // cb:fold hot per-update detail
            me->AttackStop(false);
            // ...and the shot or spell on its way at it (2026-11-06: 7 sheeps broken by a wand's
            // Shoot, 4 by Frostbolt - the swing stopped, the auto-repeat did not)
            for (CurrentSpellTypes type : { CURRENT_AUTOREPEAT_SPELL, CURRENT_GENERIC_SPELL })
                if (Spell* cast = me->GetCurrentSpell(type))
                    if (cast->m_targets.getUnitTargetGuid() == victim->GetObjectGuid() &&
                        cast->m_spellInfo && DamagingSpell(cast->m_spellInfo))
                        me->InterruptSpell(type);
            return true;
        }
        // A melee damage dealer waits out an elite's damage shield: every hit comes back (2026-11-08,
        // Majordomo: five rogues on a Flamewaker Healer died within 4 s to Damage Shield 21075 and Shadow Shock).
        // (below 75 % only: the shield is up 10 s of every 30, a third of the melee's time, and they now step out
        // of a bursting kind at 65 % - 2026-11-09, Majordomo: rogues at ~120 dps each)
        if (GetCombatActiveRole() == ROLE_MELEE_DPS && victim->IsCreature() && static_cast<Creature*>(victim)->IsElite() &&
            victim->HasAuraType(SPELL_AURA_DAMAGE_SHIELD) && me->GetHealthPercent() < 75.0f)
        {   // cb:fold hot per-update detail
            uint32 const nowMs = WorldTimer::getMSTime();
            if (nowMs - m_damageShieldLogMs > 10000)
            {
                m_damageShieldLogMs = nowMs;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-MELEE] %s: waits out %s's damage shield",
                    me->GetName(), victim->GetName());
            }
            if (Pet* pet = me->GetPet())
                if (pet->GetVictim() == victim)
                    pet->AttackStop();
            me->AttackStop(false);
            return true;
        }
    }
    StopSplashNearCrowdControl(me);

    switch (me->GetClass())
    {
        case CLASS_WARRIOR: UpdateSpecCombatWarrior(spec); break;   // cb:fold hot per-update detail
        case CLASS_PALADIN: UpdateSpecCombatPaladin(spec); break;   // cb:fold hot per-update detail
        case CLASS_HUNTER:  UpdateSpecCombatHunter(spec); break;   // cb:fold hot per-update detail
        case CLASS_ROGUE:   UpdateSpecCombatRogue(spec); break;   // cb:fold hot per-update detail
        case CLASS_PRIEST:  UpdateSpecCombatPriest(spec); break;   // cb:fold hot per-update detail
        case CLASS_SHAMAN:  UpdateSpecCombatShaman(spec); break;   // cb:fold hot per-update detail
        case CLASS_MAGE:    UpdateSpecCombatMage(spec); break;   // cb:fold hot per-update detail
        case CLASS_WARLOCK: UpdateSpecCombatWarlock(spec); break;   // cb:fold hot per-update detail
        case CLASS_DRUID:   UpdateSpecCombatDruid(spec); break;   // cb:fold hot per-update detail
        default: return false;   // cb:fold hot per-update detail
    }
    return true;
}

bool AiBotAI::UpdateSpecOutOfCombatAI()
{
    uint8 const spec = GetCombatSpecTab();
    if (spec > 2)
        return false;   // cb:fold hot per-update detail
    m_isBuffing = UpdateSpecOutOfCombat(me->GetClass(), spec);
    // The legacy class routines all END with `if (GetVictim()) UpdateInCombatAI_X()`
    // — the bridge that fires a ranged opener at an ARMED victim during the pull
    // window (attack intent set, combat flag not yet). Losing it here parked
    // spec casters at chase distance with nothing ever starting the fight
    // (owner 2026-08-25: "standing in combat pose doing nothing", self-
    // resolving only when the mob wandered into melee reach). Same bridge,
    // spec rotation.
    if (me->GetVictim())
        UpdateSpecCombatAI();   // cb:fold rotation entry, deciders probed inside
    return true;
}

bool AiBotAI::UpdateSpecOutOfCombat(uint8 playerClass, uint8 spec)
{
    auto isReachableSpecBuffTarget = [this](Player* target) -> bool
    {
        return target && target->IsAlive() && !target->IsGameMaster() &&
            me->IsValidHelpfulTarget(target) && me->IsWithinLOSInMap(target) &&
            me->IsWithinDist(target, 30.0f);
    };

    auto evaluateSpecBuffTarget =
        [this, &isReachableSpecBuffTarget](Player* target,
                                           SpellEntry const* singleSpell,
                                           SpellEntry const* groupSpell,
                                           bool manaOnly,
                                           bool& preserveGroupForm) -> bool
    {
        preserveGroupForm = false;
        if (!isReachableSpecBuffTarget(target))
            return false;   // cb:fold pure recipient filter, winner probed before cast
        if (manaOnly && !IsSpecManaUser(target))
            return false;   // cb:fold pure recipient filter, winner probed before cast

        bool const hasSingleForm = singleSpell && HasAuraFromSpellChain(
            target, sSpellMgr.GetFirstSpellInChain(singleSpell->Id));
        bool const hasGroupForm = groupSpell && HasAuraFromSpellChain(
            target, sSpellMgr.GetFirstSpellInChain(groupSpell->Id));
        if (hasGroupForm)
        {   // cb:fold form preservation only, selected spell and cast outcome are probed
            preserveGroupForm = true;
            return IsValidMaintenanceBuffTarget(target, groupSpell);
        }
        if (hasSingleForm)
            return IsValidMaintenanceBuffTarget(target, singleSpell);   // cb:fold form preservation only, winner is probed
        if (singleSpell && !IsValidMaintenanceBuffTarget(target, singleSpell))
            return false;   // cb:fold pure recipient filter, winner probed before cast
        if (groupSpell && !IsValidMaintenanceBuffTarget(target, groupSpell))
            return false;   // cb:fold pure recipient filter, winner probed before cast
        return true;
    };

    auto specBuffRoleScore = [](Player* target, bool manaOnly) -> int32
    {
        CombatBotRoles const role = GetSpecBuffTargetRole(target);
        if (manaOnly)
        {   // cb:fold deterministic scoring only, winner is probed before cast
            if (role == ROLE_HEALER)
                return 500;   // cb:fold deterministic score only, winner probed before cast
            if (role == ROLE_RANGE_DPS)
                return 400;   // cb:fold deterministic score only, winner probed before cast
            if (role == ROLE_TANK)
                return 300;   // cb:fold deterministic score only, winner probed before cast
            if (role == ROLE_MELEE_DPS)
                return 200;   // cb:fold deterministic score only, winner probed before cast
            return 100;
        }

        if (role == ROLE_TANK)
            return 400;   // cb:fold deterministic score only, winner probed before cast
        if (role == ROLE_HEALER)
            return 300;   // cb:fold deterministic score only, winner probed before cast
        if (role == ROLE_MELEE_DPS)
            return 200;   // cb:fold deterministic score only, winner probed before cast
        if (role == ROLE_RANGE_DPS)
            return 100;   // cb:fold deterministic score only, winner probed before cast
        return 0;
    };

    auto selectSpecBuffTarget =
        [this, &evaluateSpecBuffTarget, &specBuffRoleScore](
            SpellEntry const* singleSpell, SpellEntry const* groupSpell,
            bool manaOnly, SpellEntry const*& selectedSpell,
            bool& selectedGroupSpell) -> Player*
    {
        selectedSpell = nullptr;
        selectedGroupSpell = false;
        if (!singleSpell && !groupSpell)
            return nullptr;   // cb:fold no learned spell, no cast decision exists

        Group* party = me->GetGroup();
        if (!party)
        {   // cb:fold solo traversal only, winner is probed before cast
            bool preserveGroupForm = false;
            if (!evaluateSpecBuffTarget(me, singleSpell, groupSpell, manaOnly,
                                        preserveGroupForm))
                return nullptr;   // cb:fold solo recipient already satisfied or ineligible
            selectedGroupSpell = !singleSpell || preserveGroupForm;
            selectedSpell = selectedGroupSpell ? groupSpell : singleSpell;
            return me;   // cb:fold winner probed by TrySpecBuff below
        }

        uint8 missingBySubgroup[MAX_RAID_SUBGROUPS] = {};
        for (GroupReference* itr = party->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->getSource();
            bool preserveGroupForm = false;
            if (!evaluateSpecBuffTarget(member, singleSpell, groupSpell, manaOnly,
                                        preserveGroupForm))
                continue;   // cb:fold rejected candidates summarized by the eventual winner/no-op
            uint8 const subgroup = member->GetSubGroup();
            if (subgroup < MAX_RAID_SUBGROUPS)
                ++missingBySubgroup[subgroup];   // cb:fold bounded raid subgroup census
        }

        Player* best = nullptr;
        int32 bestScore = -1;
        bool bestUsesGroup = false;
        for (GroupReference* itr = party->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->getSource();
            bool preserveGroupForm = false;
            if (!evaluateSpecBuffTarget(member, singleSpell, groupSpell, manaOnly,
                                        preserveGroupForm))
                continue;   // cb:fold rejected candidates summarized by the eventual winner/no-op

            uint8 const subgroup = member->GetSubGroup();
            bool const useGroup = !singleSpell || preserveGroupForm ||
                (groupSpell && subgroup < MAX_RAID_SUBGROUPS &&
                 missingBySubgroup[subgroup] > 1);
            int32 score = specBuffRoleScore(member, manaOnly);
            if (useGroup)
                score += 1000;   // cb:fold group efficiency score, winner probe records group choice
            if (score > bestScore)
            {   // cb:fold deterministic winner bookkeeping, winner is probed before cast
                best = member;
                bestScore = score;
                bestUsesGroup = useGroup;
            }
        }

        if (!best)
            return nullptr;   // cb:fold every reachable recipient already satisfied/ineligible
        selectedSpell = bestUsesGroup ? groupSpell : singleSpell;
        selectedGroupSpell = bestUsesGroup;
        return best;   // cb:fold winner probed by TrySpecBuff below
    };

    auto trySpecBuff = [this](Player* target, SpellEntry const* spell,
                              uint8 reason, bool groupSpell) -> bool
    {
        if (!target || !spell)
            return false;   // cb:fold defensive caller guard, no selection exists

        TraceSpecBuffSelection(this, target, spell, reason, groupSpell);
        if (!CanTryToRefreshAura(target, spell))
        {
            CB_HITV(me->GetGUIDLow(), "cpp-buff: cast precheck rejected", spell->Id);
            return false;
        }

        SpellCastResult const result = DoCastSpell(target, spell);
        if (result != SPELL_CAST_OK)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-buff: cast failed", uint32(result));
            return false;
        }

        CB_HITV(me->GetGUIDLow(), "cpp-buff: cast started", spell->Id);
        return true;
    };

    auto selectTankFirstBuffTarget =
        [this, &evaluateSpecBuffTarget](SpellEntry const* spell) -> Player*
    {
        if (!spell)
            return nullptr;   // cb:fold no learned spell, no cast decision exists

        Player* best = nullptr;
        int32 bestScore = -1;
        auto consider = [this, spell, &evaluateSpecBuffTarget,
                         &best, &bestScore](Player* target)
        {
            bool preserveGroupForm = false;
            if (!evaluateSpecBuffTarget(target, spell, nullptr, false,
                                        preserveGroupForm))
                return;   // cb:fold pure recipient filter, winner probed before cast

            int32 score = 0;
            CombatBotRoles const role = GetSpecBuffTargetRole(target);
            if (role == ROLE_TANK)
                score += 1000;   // cb:fold deterministic tank score
            if (target->GetShapeshiftForm() == FORM_BEAR ||
                target->GetShapeshiftForm() == FORM_DIREBEAR ||
                target->HasAura(25780))
                score += 500;   // cb:fold observable tank-state score
            if (!target->GetAttackers().empty())
                score += 300;   // cb:fold observable aggro score
            if (IsTankClass(target->GetClass()))
                score += 100;   // cb:fold human/non-profile fallback score
            if (role == ROLE_MELEE_DPS)
                score += 25;   // cb:fold melee fallback score
            if (target == me)
                ++score;   // cb:fold stable final tie-breaker

            if (score > bestScore)
            {   // cb:fold deterministic winner bookkeeping, winner is probed before cast
                best = target;
                bestScore = score;
            }
        };

        consider(me);
        if (Group* party = me->GetGroup())
            for (GroupReference* itr = party->GetFirstMember(); itr; itr = itr->next())   // cb:fold deterministic census, winner is probed
                if (Player* member = itr->getSource())
                    if (member != me)   // cb:fold deterministic census, winner is probed
                        consider(member);   // cb:fold deterministic group census, winner probed below
        return best;
    };

    auto selectPaladinBlessingTarget =
        [this, &isReachableSpecBuffTarget](SpellEntry const*& selectedSpell,
                                           uint8& selectedReason) -> Player*
    {
        selectedSpell = nullptr;
        selectedReason = 0;
        SpellEntry const* might = GetHighestKnownRank(SP_BLESSING_OF_MIGHT);
        SpellEntry const* wisdom = GetHighestKnownRank(SP_BLESSING_OF_WISDOM);
        SpellEntry const* light = GetHighestKnownRank(SP_BLESSING_OF_LIGHT);
        SpellEntry const* kings = GetHighestKnownRank(SP_BLESSING_OF_KINGS);
        SpellEntry const* sanctuary = GetHighestKnownRank(SP_BLESSING_OF_SANCTUARY);
        SpellEntry const* salvation = GetHighestKnownRank(SP_BLESSING_OF_SALVATION);

        Player* best = nullptr;
        int32 bestScore = -1;
        auto consider = [this, &isReachableSpecBuffTarget, might, wisdom, light,
                         kings, sanctuary, salvation, &selectedSpell, &selectedReason,
                         &best, &bestScore](Player* target)
        {
            if (!isReachableSpecBuffTarget(target))
                return;   // cb:fold pure recipient filter, winner probed before cast

            CombatBotRoles const role = GetSpecBuffTargetRole(target);
            bool const manaUser = IsSpecManaUser(target);
            SpellEntry const* candidates[6] = {};
            uint8 reason = 0;
            int32 score = 0;

            if (role == ROLE_TANK)
            {   // cb:fold deterministic recipient policy, reason is probed before cast
                candidates[0] = sanctuary;
                candidates[1] = kings;
                candidates[2] = light;
                candidates[3] = might;
                candidates[4] = wisdom;
                reason = BUFF_PALADIN_TANK;
                score = 3000;
            }
            else if (role == ROLE_MELEE_DPS || !manaUser)
            {   // cb:fold deterministic recipient policy, reason is probed before cast
                candidates[0] = might;
                candidates[1] = kings;
                candidates[2] = salvation;
                candidates[3] = sanctuary;
                candidates[4] = wisdom;
                reason = BUFF_PALADIN_PHYSICAL;
                score = 1000;
            }
            else
            {   // cb:fold deterministic recipient policy, reason is probed before cast
                candidates[0] = wisdom;
                candidates[1] = kings;
                candidates[2] = salvation;
                candidates[3] = might;
                reason = BUFF_PALADIN_MANA;
                score = role == ROLE_HEALER ? 2500 : 2000;
            }

            uint32 ownBlessingChain = 0;
            for (auto const& aura : target->GetSpellAuraHolderMap())
            {
                SpellAuraHolder const* holder = aura.second;
                if (!holder || holder->GetCasterGuid() != me->GetObjectGuid())
                    continue;   // cb:fold only this Paladin's assignment controls diversification

                uint32 chain = sSpellMgr.GetFirstSpellInChain(aura.first);
                // A Greater blessing is this caster's existing assignment too.
                // Its native blessing category, family flags and effect identify the same family
                // even when the spell database uses a separate rank chain.
                if (Spells::GetSpellSpecific(aura.first) == SPELL_BLESSING)
                    for (SpellEntry const* candidate : {might, wisdom, light, kings, sanctuary, salvation})
                        if (candidate && holder->GetSpellProto()->SpellFamilyFlags == candidate->SpellFamilyFlags &&
                            holder->GetSpellProto()->EffectApplyAuraName[0] == candidate->EffectApplyAuraName[0] &&
                            holder->GetSpellProto()->EffectMiscValue[0] == candidate->EffectMiscValue[0])
                        {   // cb:fold native blessing-family normalization, selected family is probed
                            chain = sSpellMgr.GetFirstSpellInChain(candidate->Id);
                            break;
                        }
                if (chain == SP_BLESSING_OF_PROTECTION ||
                    chain == SP_BLESSING_OF_FREEDOM ||
                    chain == SP_BLESSING_OF_SACRIFICE)
                    return;   // cb:fold never replace this Paladin's active utility blessing
                if (chain == SP_BLESSING_OF_MIGHT ||
                    chain == SP_BLESSING_OF_WISDOM ||
                    chain == SP_BLESSING_OF_LIGHT ||
                    chain == SP_BLESSING_OF_KINGS ||
                    chain == SP_BLESSING_OF_SANCTUARY ||
                    chain == SP_BLESSING_OF_SALVATION)
                    ownBlessingChain = chain;   // cb:fold at most one normal blessing per caster/target
            }

            SpellEntry const* desired = nullptr;
            if (ownBlessingChain)
            {   // cb:fold stable owner-assignment path, selected family is probed
                for (SpellEntry const* candidate : candidates)
                    if (candidate && sSpellMgr.GetFirstSpellInChain(candidate->Id) == ownBlessingChain)
                    {   // cb:fold bounded family match, refresh winner is probed
                        if (IsValidMaintenanceBuffTarget(target, candidate))
                            desired = candidate;   // cb:fold stable owner refresh/upgrade
                        break;
                    }
                if (!desired)
                    return;   // cb:fold this Paladin's current assignment is still healthy
            }
            else
            {   // cb:fold unassigned-recipient path, selected family is probed
                for (SpellEntry const* candidate : candidates)
                    if (candidate && IsValidMaintenanceBuffTarget(target, candidate))
                    {   // cb:fold bounded priority election, winner is probed
                        desired = candidate;
                        break;
                    }
                if (!desired)
                    return;   // cb:fold every useful family is already supplied/unavailable
            }

            if (target == me)
                ++score;   // cb:fold stable final tie-breaker
            if (score > bestScore)
            {   // cb:fold deterministic winner bookkeeping, winner is probed before cast
                best = target;
                bestScore = score;
                selectedSpell = desired;
                selectedReason = reason;
            }
        };

        consider(me);
        if (Group* party = me->GetGroup())
            for (GroupReference* itr = party->GetFirstMember(); itr; itr = itr->next())   // cb:fold deterministic census, winner is probed
                if (Player* member = itr->getSource())
                    if (member != me)   // cb:fold deterministic census, winner is probed
                        consider(member);   // cb:fold deterministic group census, winner probed below
        return best;
    };

    switch (playerClass)
    {
        case CLASS_WARRIOR:   // cb:fold rotation rung, outcome probed at cast
        {
            Unit* chargeTarget = me->GetVictim();
            float const chargeDistance = chargeTarget ? me->GetCombatDistance(chargeTarget) : 0.0f;
            bool const chargeReady = chargeTarget && m_spells.warrior.pCharge &&
                IsValidHostileTarget(chargeTarget) && me->IsWithinLOSInMap(chargeTarget) &&
                chargeDistance >= 8.0f && chargeDistance <= 25.0f &&
                me->IsSpellReady(m_spells.warrior.pCharge->Id) &&
                !me->HasGCD(m_spells.warrior.pCharge);

            uint32 intendedStance = SP_BATTLE_STANCE;
            if (GetCombatActiveRole() == ROLE_TANK || spec == 2)
                intendedStance = SP_DEFENSIVE_STANCE;   // cb:fold rotation rung, outcome probed at cast
            else if (spec == 1)
                intendedStance = SP_BERSERKER_STANCE;   // cb:fold rotation rung, outcome probed at cast

            // Charge is Battle-Stance-only.  Hold Battle Stance while a real,
            // in-range opener is ready; combat policy restores the profile's
            // intended stance immediately after the charge engages.
            uint32 stance = chargeReady ? SP_BATTLE_STANCE : intendedStance;

            // Low-level Fury may not know Berserker Stance yet.  Fall back once
            // to Battle Stance, but never cascade through multiple known stances
            // and oscillate every OOC GCD.
            if (!GetHighestKnownRank(stance))
                stance = SP_BATTLE_STANCE;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecAura(me, stance)) return true;   // cb:fold rotation rung, outcome probed at cast

            bool const missingShout = m_spells.warrior.pBattleShout &&
                !HasAuraFromSpellChain(me, 6673);
            if (missingShout && TrySpecSpell(me, m_spells.warrior.pBattleShout)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (missingShout && me->GetPower(POWER_RAGE) < 100 &&
                TrySpecSpell(me, m_spells.warrior.pBloodrage)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (chargeReady && TrySpecSpell(chargeTarget, m_spells.warrior.pCharge)) return true;   // cb:fold rotation rung, outcome probed at cast
            return false;
        }

        case CLASS_PALADIN:   // cb:fold class dispatch, decision and cast outcome are probed below
        {
            if (TrySpecSpell(me, m_spells.paladin.pAura)) return true;   // cb:fold rotation rung, outcome probed at cast
            SpellEntry const* blessing = nullptr;
            uint8 blessingReason = 0;
            if (Player* target = selectPaladinBlessingTarget(blessing, blessingReason))
                if (trySpecBuff(target, blessing, blessingReason, false)) return true;   // cb:fold selection and cast outcome are probed
            if (spec == 1 && TrySpecAura(me, 25780)) return true; // Righteous Fury   // cb:fold rotation rung, outcome probed at cast
            if (spec == 0 && FindAndHealInjuredAlly(100.0f, 100.0f)) return true;   // cb:fold rotation rung, outcome probed at cast
            return false;
        }

        case CLASS_HUNTER:   // cb:fold rotation rung, outcome probed at cast
            SummonPetIfNeeded();
            if (Pet* pet = me->GetPet())
                if (pet->IsAlive() && pet->GetHealthPercent() < 80.0f && TrySpecSpell(pet, SP_MEND_PET)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (spec == 1 && TrySpecAura(me, SP_TRUESHOT_AURA)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (!me->GetVictim() && !me->IsMounted() && m_spells.hunter.pAspectOfTheCheetah)
            {   // cb:fold rotation rung, outcome probed at cast
                if (TrySpecAura(me, 5118)) return true; // Aspect of the Cheetah   // cb:fold rotation rung, outcome probed at cast
                return false; // Never replace an active travel aspect with Hawk.
            }
            if ((me->GetVictim() || !m_spells.hunter.pAspectOfTheCheetah) &&
                TrySpecAura(me, 13165)) return true; // Aspect of the Hawk   // cb:fold rotation rung, outcome probed at cast
            return false;

        case CLASS_ROGUE:   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.rogue.pMainHandPoison &&
                CastWeaponBuff(m_spells.rogue.pMainHandPoison, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK) return true;   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.rogue.pOffHandPoison &&
                CastWeaponBuff(m_spells.rogue.pOffHandPoison, EQUIPMENT_SLOT_OFFHAND) == SPELL_CAST_OK) return true;   // cb:fold rotation rung, outcome probed at cast
            {
                Unit* victim = me->GetVictim();
                bool const closeApproach = victim && me->IsWithinDistInMap(victim, 35.0f);
                bool const stealthed = m_spells.rogue.pStealth &&
                    me->HasAura(m_spells.rogue.pStealth->Id);

                // Stealth is an engage/escape tool, never a travel stance.  Keep
                // low-health Vanish stealth intact, but cancel ordinary stale
                // stealth once the target is no longer on final approach.
                if (stealthed && !closeApproach && me->GetHealthPercent() >= 25.0f)
                {   // cb:fold rotation rung, outcome probed at cast
                    CB_HIT(me->GetGUIDLow(), "cpp-spec: stale stealth cancelled for travel");
                    me->RemoveAurasDueToSpellByCancel(m_spells.rogue.pStealth->Id);
                    return true;
                }

                if (closeApproach && !stealthed &&
                    !me->HasAura(SP_WARSONG_FLAG) && !me->HasAura(SP_SILVERWING_FLAG) &&
                    TrySpecSpell(me, m_spells.rogue.pStealth)) return true;   // cb:fold rotation rung, outcome probed at cast
            }
            return false;

        case CLASS_PRIEST:   // cb:fold class dispatch, decision and cast outcome are probed below
        {
            if (TrySpecAura(me, 588)) return true; // Inner Fire   // cb:fold rotation rung, outcome probed at cast
            SpellEntry const* buffSpell = nullptr;
            bool groupSpell = false;
            if (Player* target = selectSpecBuffTarget(
                    m_spells.priest.pPowerWordFortitude,
                    m_spells.priest.pPrayerofFortitude,
                    false, buffSpell, groupSpell))
            {   // cb:fold selector winner is probed by trySpecBuff
                uint8 const reason = groupSpell ? BUFF_PRIEST_FORTITUDE_GROUP :
                    BUFF_PRIEST_FORTITUDE_SINGLE;
                if (trySpecBuff(target, buffSpell, reason, groupSpell)) return true;   // cb:fold selection and cast outcome are probed
            }
            if (Player* target = selectSpecBuffTarget(
                    m_spells.priest.pDivineSpirit,
                    m_spells.priest.pPrayerofSpirit,
                    true, buffSpell, groupSpell))
            {   // cb:fold selector winner is probed by trySpecBuff
                uint8 const reason = groupSpell ? BUFF_PRIEST_SPIRIT_GROUP :
                    BUFF_PRIEST_SPIRIT_SINGLE;
                if (trySpecBuff(target, buffSpell, reason, groupSpell)) return true;   // cb:fold selection and cast outcome are probed
            }
            if (spec == 2 && TrySpecAura(me, SP_SHADOWFORM)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (spec != 2 && FindAndHealInjuredAlly(100.0f, 100.0f)) return true;   // cb:fold rotation rung, outcome probed at cast
            return false;
        }

        case CLASS_SHAMAN:   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecAura(me, 324)) return true; // Lightning Shield   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.shaman.pWeaponBuff &&
                CastWeaponBuff(m_spells.shaman.pWeaponBuff, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK) return true;   // cb:fold rotation rung, outcome probed at cast
            if (spec == 2 && FindAndHealInjuredAlly(100.0f, 100.0f)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (!me->GetVictim() && m_spells.shaman.pGhostWolf &&
                !me->IsMoving() && !me->IsMounted() &&
                (!GetMountSpellId() || me->HasAura(SP_WARSONG_FLAG) || me->HasAura(SP_SILVERWING_FLAG)) &&
                TrySpecSpell(me, m_spells.shaman.pGhostWolf)) return true;   // cb:fold rotation rung, outcome probed at cast
            return false;

        case CLASS_MAGE:   // cb:fold class dispatch, decision and cast outcome are probed below
        {
            SpellEntry const* buffSpell = nullptr;
            bool groupSpell = false;
            if (Player* target = selectSpecBuffTarget(
                    m_spells.mage.pArcaneIntellect,
                    m_spells.mage.pArcaneBrilliance,
                    true, buffSpell, groupSpell))
            {   // cb:fold rotation rung, outcome probed at cast
                uint8 const reason = groupSpell ? BUFF_MAGE_INTELLECT_GROUP :
                    BUFF_MAGE_INTELLECT_SINGLE;
                if (trySpecBuff(target, buffSpell, reason, groupSpell)) return true;   // cb:fold selection and cast outcome are probed
            }
            if (TrySpecSpell(me, m_spells.mage.pIceArmor)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (spec == 2 && TrySpecAura(me, SP_ICE_BARRIER)) return true;   // cb:fold rotation rung, outcome probed at cast
            return false;
        }

        case CLASS_WARLOCK:   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(me, m_spells.warlock.pDemonArmor)) return true;   // cb:fold rotation rung, outcome probed at cast
            SummonPetIfNeeded(); // typed SummonPetIfNeeded chooses a stable pet and never replaces a living one
            if (spec == 1)
                if (Pet* pet = me->GetPet())   // cb:fold rotation rung, outcome probed at cast
                    if (pet->IsAlive() && !HasSoulLinkAura(me) &&   // cb:fold rotation rung, outcome probed at cast
                        TrySpecSpell(pet, SP_SOUL_LINK)) return true;   // cb:fold rotation rung, outcome probed at cast
            return false;

        case CLASS_DRUID:   // cb:fold rotation rung, outcome probed at cast
        {
            auto leaveFormForCast = [this](Unit* target, SpellEntry const* spell) -> bool
            {
                if (!CanTryToCastSpellAfterLeavingForm(target, spell))
                    return false;   // cb:fold rotation rung, outcome probed at cast
                me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
                return true;
            };

            auto leaveFormForBuff = [this](Unit* target, SpellEntry const* spell) -> bool
            {
                if (!CanTryToRefreshAuraAfterLeavingForm(target, spell))
                    return false;   // cb:fold rotation rung, outcome probed at selection/cast
                me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
                return true;
            };

            SpellEntry const* wild = nullptr;
            bool groupWild = false;
            if (Player* target = selectSpecBuffTarget(
                    m_spells.druid.pMarkoftheWild,
                    m_spells.druid.pGiftoftheWild,
                    false, wild, groupWild))
            {   // cb:fold selector winner is probed by form/cast paths below
                uint8 const reason = groupWild ? BUFF_DRUID_WILD_GROUP :
                    BUFF_DRUID_WILD_SINGLE;
                if (leaveFormForBuff(target, wild))
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-buff: left form, cast deferred", wild->Id);
                    TraceSpecBuffSelection(this, target, wild, reason, groupWild);
                    return true;
                }
                if (trySpecBuff(target, wild, reason, groupWild)) return true;   // cb:fold selection and cast outcome are probed
            }

            if (Player* target = selectTankFirstBuffTarget(m_spells.druid.pThorns))
            {   // cb:fold selector winner is probed by form/cast paths below
                if (leaveFormForBuff(target, m_spells.druid.pThorns))
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-buff: left form, cast deferred", m_spells.druid.pThorns->Id);
                    TraceSpecBuffSelection(this, target, m_spells.druid.pThorns,
                                           BUFF_DRUID_THORNS_TANK_FIRST, false);
                    return true;
                }
                if (trySpecBuff(target, m_spells.druid.pThorns,
                                BUFF_DRUID_THORNS_TANK_FIRST, false)) return true;   // cb:fold selection and cast outcome are probed
            }
            if (leaveFormForCast(me, GetHighestKnownRank(16689))) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecAura(me, 16689)) return true; // Nature's Grasp   // cb:fold rotation rung, outcome probed at cast
            if (spec == 2)
            {   // cb:fold rotation rung, outcome probed at cast
                if (Unit* heal = SelectHealTarget(100.0f, 100.0f))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (me->GetShapeshiftForm() != FORM_NONE)
                    {   // cb:fold rotation rung, outcome probed at cast
                        for (SpellEntry const* spell : m_spellListPeriodicHeal)
                            if (leaveFormForCast(heal, spell)) return true;   // cb:fold rotation rung, outcome probed at cast
                        for (SpellEntry const* spell : m_spellListDirectHeal)
                            if (leaveFormForCast(heal, spell)) return true;   // cb:fold rotation rung, outcome probed at cast
                    }
                    if (HealInjuredTarget(heal)) return true;   // cb:fold rotation rung, outcome probed at cast
                }
            }
            if (spec == 1)
            {   // cb:fold rotation rung, outcome probed at cast
                if (leaveFormForCast(me, GetHighestKnownRank(16864))) return true;   // cb:fold rotation rung, outcome probed at cast
                if (TrySpecAura(me, 16864)) return true; // Omen of Clarity   // cb:fold rotation rung, outcome probed at cast
            }

            if (!me->GetVictim() && !me->IsMounted() && m_spells.druid.pTravelForm &&
                (!GetMountSpellId() || me->HasAura(SP_WARSONG_FLAG) || me->HasAura(SP_SILVERWING_FLAG)))
            {   // cb:fold rotation rung, outcome probed at cast
                if (me->GetShapeshiftForm() != FORM_TRAVEL &&
                    TrySpecSpell(me, m_spells.druid.pTravelForm)) return true;   // cb:fold rotation rung, outcome probed at cast
                return false; // Preserve Travel Form until approach/combat restores the role form.
            }

            if (spec == 0 && !me->IsMounted() && TrySpecAura(me, 24858)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (spec == 1 && !me->IsMounted())
            {   // cb:fold rotation rung, outcome probed at cast
                if (GetCombatActiveRole() == ROLE_TANK && me->GetShapeshiftForm() != FORM_BEAR &&
                    me->GetShapeshiftForm() != FORM_DIREBEAR && TrySpecSpell(me, m_spells.druid.pBearForm)) return true;   // cb:fold rotation rung, outcome probed at cast
                if (GetCombatActiveRole() == ROLE_MELEE_DPS && me->GetShapeshiftForm() != FORM_CAT &&
                    TrySpecSpell(me, m_spells.druid.pCatForm)) return true;   // cb:fold rotation rung, outcome probed at cast
            }
            return false;
        }
    }
    return false;
}

bool AiBotAI::UpdateSpecCombatWarrior(uint8 spec)
{
    Unit* victim = me->GetVictim();
    if (!victim)
        return false;   // cb:fold rotation rung, outcome probed at cast

    // Arms and Fury are approved as alternate tank roles.  Tank ownership is
    // evaluated before the Arms weapon-lock/degraded-DPS branch so persisted
    // activeRole remains authoritative and these profiles actually defend the
    // party rather than merely wearing a tank label.
    if (spec != 2 && GetCombatActiveRole() == ROLE_TANK)
    {   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecAura(me, SP_DEFENSIVE_STANCE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecTaunt(victim)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecInterrupt(victim, {72})) return true;   // cb:fold rotation rung, outcome probed at cast

        uint32 onMe = 0;
        for (Unit* a : me->GetAttackers())
            if (a && a->IsAlive() && a->GetVictim() == me)
                ++onMe;
        bool const swamped = onMe >= 4;
        if ((swamped || me->GetHealthPercent() < 35.0f) && IsWearingShield(me) && TrySpecSpell(me, 871)) return true;   // cb:fold rotation rung, outcome probed at cast
        if ((me->GetHealthPercent() < 35.0f || (swamped && me->GetHealthPercent() < 70.0f)) &&
            TrySpecSpell(me, SP_LAST_STAND)) return true;   // cb:fold rotation rung, outcome probed at cast

        if (IsWearingShield(me))
        {   // cb:fold rotation rung, outcome probed at cast
            if (victim->CanReachWithMeleeAutoAttack(me) && TrySpecAura(me, SP_SHIELD_BLOCK)) return true;   // cb:fold rotation rung, outcome probed at cast
        }

        if (TrySpecSpell(victim, SP_REVENGE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecStackingAura(victim, 7386)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (!HasAuraFromSpellChain(victim, 1160) && CanUseSpecAoE(me, 10.0f, 1) &&
            TrySpecSpell(victim, 1160)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetPower(POWER_RAGE) > 500 && CanUseSpecAoE(victim, 8.0f, 2) &&
            TrySpecSpell(victim, 845)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetPower(POWER_RAGE) > 300 && TrySpecSpell(victim, 78)) return true;   // cb:fold rotation rung, outcome probed at cast
        return false;
    }

    if (spec == 0)
    {   // cb:fold rotation rung, outcome probed at cast
        Item* weapon = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        ItemPrototype const* proto = weapon ? weapon->GetProto() : nullptr;
        bool const correctWeapon = proto && proto->IsWeapon() &&
            proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2;
        if (!correctWeapon)
        {   // cb:fold rotation rung, outcome probed at cast
            if (!m_reportedArmsWeaponMismatch)
            {   // cb:fold rotation rung, outcome probed at cast
                CB_HIT(me->GetGUIDLow(), "cpp-spec: arms profile degraded, no two handed axe");
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-SPEC] %s Arms profile degraded: requires a two-handed axe",
                    me->GetName());
                m_reportedArmsWeaponMismatch = true;
            }
            // Safe degraded kit while auto-equip searches inventory: retain
            // stance, interrupts, Rend and a resource-capped white-swing dump;
            // do not pretend the axe-locked policy is operating optimally.
            if (TrySpecAura(me, SP_BATTLE_STANCE)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecInterrupt(victim, {72, 6552})) return true;   // cb:fold rotation rung, outcome probed at cast
            if (victim->GetHealthPercent() > 35.0f && TrySpecAura(victim, 772)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (me->GetPower(POWER_RAGE) > 300 && TrySpecSpell(victim, 78)) return true;   // cb:fold rotation rung, outcome probed at cast
            return false;
        }
        m_reportedArmsWeaponMismatch = false;
    }

    if (spec == 2) // Protection
    {   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecAura(me, SP_DEFENSIVE_STANCE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecTaunt(victim)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecInterrupt(victim, {72, SP_CONCUSSION_BLOW, 6552})) return true;   // cb:fold rotation rung, outcome probed at cast
        uint32 onMe = 0;
        for (Unit* a : me->GetAttackers())
            if (a && a->IsAlive() && a->GetVictim() == me)
                ++onMe;
        bool const swamped = onMe >= 4;
        if ((swamped || me->GetHealthPercent() < 35.0f) && TrySpecSpell(me, 871)) return true; // Shield Wall   // cb:fold rotation rung, outcome probed at cast
        if ((me->GetHealthPercent() < 35.0f || (swamped && me->GetHealthPercent() < 70.0f)) &&
            TrySpecSpell(me, SP_LAST_STAND)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->CanReachWithMeleeAutoAttack(me) && TrySpecAura(me, SP_SHIELD_BLOCK)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, SP_REVENGE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, SP_SHIELD_SLAM)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecStackingAura(victim, 7386)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (!HasAuraFromSpellChain(victim, 1160) && CanUseSpecAoE(me, 10.0f, 1) &&
            TrySpecSpell(victim, 1160)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(victim, 8.0f, 2) && TrySpecSpell(victim, 845)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetPower(POWER_RAGE) > 300 && TrySpecSpell(victim, 78)) return true;   // cb:fold rotation rung, outcome probed at cast
        return false;
    }

    if (spec == 1) // Fury
    {   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecAura(me, SP_BERSERKER_STANCE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecInterrupt(victim, {6552, 72})) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() < 20.0f && TrySpecSpell(victim, 5308)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetHealthPercent() > 70.0f && victim->GetHealthPercent() > 55.0f &&
            TrySpecSpell(me, SP_DEATH_WISH)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, SP_BLOODTHIRST)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(victim, 8.0f, 2) && TrySpecSpell(victim, 1680)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(me, 10.0f, 2) && TrySpecSpell(me, SP_PIERCING_HOWL)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetPower(POWER_RAGE) > 300 && CanUseSpecAoE(victim, 8.0f, 2) && TrySpecSpell(victim, 845)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetPower(POWER_RAGE) > 300 && TrySpecSpell(victim, 78)) return true;   // cb:fold rotation rung, outcome probed at cast
        return false;
    }

    // Arms
    if (TrySpecAura(me, SP_BATTLE_STANCE)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecInterrupt(victim, {72, 6552})) return true;   // cb:fold rotation rung, outcome probed at cast
    if (victim->GetHealthPercent() < 20.0f && TrySpecSpell(victim, 5308)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecSpell(victim, SP_MORTAL_STRIKE)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (CanUseSpecAoE(victim, 8.0f, 2) && TrySpecSpell(me, SP_SWEEPING_STRIKES)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (CanUseSpecAoE(victim, 8.0f, 2) && TrySpecSpell(victim, 1680)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecSpell(victim, 7384)) return true; // Overpower is aura-state gated by DBC   // cb:fold rotation rung, outcome probed at cast
    if (victim->GetHealthPercent() > 35.0f && TrySpecAura(victim, 772)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (me->GetPower(POWER_RAGE) > 300 && CanUseSpecAoE(victim, 8.0f, 2) && TrySpecSpell(victim, 845)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (me->GetPower(POWER_RAGE) > 300 && TrySpecSpell(victim, 78)) return true;   // cb:fold rotation rung, outcome probed at cast
    return false;
}

bool AiBotAI::UpdateSpecCombatPaladin(uint8 spec)
{
    Unit* victim = me->GetVictim();

    // A paladin tank an elite is about to kill shields itself: in this version its mobs keep swinging at the
    // Divine Shield for nothing, 12 s for the healers to catch up, and Lay on Hands itself after that
    // (2026-11-08, Majordomo attempt 49: the paladin tank went from 96 % to dead in 6 s under one Elite and
    // the add cycle lost its fifth tank at 44 s).
    if (spec == 1 && me->IsInCombat() && me->GetHealthPercent() < 25.0f && !me->HasAura(25771))
    {
        bool held = false;
        for (Unit* a : me->GetAttackers())
            if (a && a->IsCreature() && a->GetVictim() == me && static_cast<Creature*>(a)->IsElite())
                held = true;
        if (held)
        {
            float const hp = me->GetHealthPercent();
            if (TrySpecSpell(me, 642))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: Divine Shield at %.0f%%", me->GetName(), hp);
                return true;
            }
            if (hp < 15.0f)
                for (uint32 id : {10310u, 2800u, 633u})
                    if (me->HasSpell(id))
                    {
                        if (me->IsSpellReady(id) && DoCastSpell(me, sSpellMgr.GetSpellEntry(id)) == SPELL_CAST_OK)
                        {
                            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: Lay on Hands on itself at %.0f%%", me->GetName(), hp);
                            return true;
                        }
                        break;
                    }
        }
    }

    // Every Paladin protects allies before dealing damage.  Holy Shock is
    // explicitly evaluated as a heal before its offensive branch.
    Unit* heal = SelectHealTarget(spec == 0 ? 88.0f : 35.0f, spec == 0 ? 82.0f : 28.0f);
    if (!(heal && heal->GetHealthPercent() < 40.0f) &&
        TrySpecValuedDispel(m_spells.paladin.pCleanse, 800.0f)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (heal)
    {   // cb:fold rotation rung, outcome probed at cast
        // A tank about to die gets Lay on Hands: one save an hour, worth all this paladin's mana - a dead tank
        // ends the add cycle (2026-11-08, Majordomo: the first tank died at 27-103 s in most attempts).
        if (heal != me && heal->GetHealthPercent() < 15.0f && heal->IsPlayer())
        {
            bool held = false;
            for (Unit* a : heal->GetAttackers())
                if (a && a->IsCreature() && a->GetVictim() == heal && static_cast<Creature*>(a)->IsElite())
                    held = true;
            if (held && TrySpecSpell(heal, 10310))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HEAL] %s: Lay on Hands on %s at %.0f%%",
                    me->GetName(), heal->GetName(), heal->GetHealthPercent());
                return true;
            }
        }
        if (heal->GetHealthPercent() < 35.0f && TrySpecSpell(me, SP_DIVINE_FAVOR)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (heal->GetHealthPercent() < 65.0f && TrySpecSpell(heal, SP_HOLY_SHOCK)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (spec == 0 || heal->GetHealthPercent() < 28.0f)
            if (HealInjuredTarget(heal)) return true;   // cb:fold rotation rung, outcome probed at cast
    }

    if (TrySpecValuedDispel(m_spells.paladin.pCleanse, 1.0f)) return true;   // cb:fold rotation rung, outcome probed at cast

    if (!victim)
        return false;   // cb:fold rotation rung, outcome probed at cast

    if (TrySpecInterrupt(victim, {853})) return true; // Hammer of Justice   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecSpell(me, m_spells.paladin.pAura)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecSpell(me, m_spells.paladin.pSeal)) return true;   // cb:fold rotation rung, outcome probed at cast

    if (spec == 1) // Protection
    {   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecAura(me, 25780)) return true; // Righteous Fury   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecAura(me, SP_HOLY_SHIELD)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(me, 8.0f, 2) && !ConsecrationNearSheep(me) && TrySpecSpell(me, m_spells.paladin.pConsecration)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.paladin.pJudgement)) return true;   // cb:fold rotation rung, outcome probed at cast
        return false; // never bubble/BoP the active tank automatically
    }

    if (spec == 2) // Retribution
    {   // cb:fold rotation rung, outcome probed at cast
        if (Unit* add = SelectSafeSpecAdd(victim))
            if (TrySpecSpell(add, SP_REPENTANCE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() < 20.0f && TrySpecSpell(victim, m_spells.paladin.pHammerOfWrath)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.paladin.pJudgement)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetPowerPercent(POWER_MANA) > 65.0f && CanUseSpecAoE(me, 8.0f, 2) &&
            !ConsecrationNearSheep(me) && TrySpecSpell(me, m_spells.paladin.pConsecration)) return true;   // cb:fold rotation rung, outcome probed at cast
        return false;
    }

    // Holy: offense only after the healing/cleanse pass above.
    if (me->GetPowerPercent(POWER_MANA) > 55.0f && TrySpecSpell(victim, SP_HOLY_SHOCK)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (me->GetPowerPercent(POWER_MANA) > 70.0f && CanUseSpecAoE(me, 8.0f, 2) &&
        !ConsecrationNearSheep(me) && TrySpecSpell(me, m_spells.paladin.pConsecration)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecSpell(victim, m_spells.paladin.pJudgement)) return true;   // cb:fold rotation rung, outcome probed at cast
    return false;
}

// [TACTICS] An enraged enemy (a buff whose dispel type is Enrage - a frenzy) is calmed by a
// hunter's Tranquilizing Shot. From the aura's own spell data; no creature is named.
bool AiBotAI::TrySpecTranquilize()
{
    uint32 const kTranquilizingShot = 19801;
    if (!me->HasSpell(kTranquilizingShot) || !me->IsSpellReady(kTranquilizingShot) || !me->GetGroup())
        return false;
    std::list<Unit*> nearby;
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(me, me, 35.0f);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(nearby, check);
    Cell::VisitAllObjects(me, searcher, 35.0f);
    for (Unit* mob : nearby)
    {
        if (!mob->IsAlive() || !mob->IsInCombat() || !IsValidAssistTarget(mob))
            continue;
        for (auto const& entry : mob->GetSpellAuraHolderMap())
        {
            SpellEntry const* proto = entry.second ? entry.second->GetSpellProto() : nullptr;
            if (proto && proto->Dispel == DISPEL_ENRAGE && entry.second->IsPositive())
                if (TrySpecSpell(mob, kTranquilizingShot))
                    return true;
        }
    }
    return false;
}

bool AiBotAI::UpdateSpecCombatHunter(uint8 spec)
{
    if (TrySpecTranquilize()) return true;   // cb:fold rotation rung, outcome probed at cast
    Unit* victim = me->GetVictim();
    if (!victim)
    {   // cb:fold rotation rung, outcome probed at cast
        if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);   // cb:fold rotation rung, outcome probed at cast
        return false;
    }

    // Replace the travel aspect before any dead-zone or pet branch can yield;
    // otherwise a Hunter entering melee can remain vulnerable to Cheetah daze.
    if (TrySpecAura(me, 13165)) return true; // Aspect of the Hawk   // cb:fold rotation rung, outcome probed at cast

    // Auto Shot cannot operate in the hunter dead zone.  Stop it on the fast
    // lane immediately instead of waiting for the next one-second spine tick.
    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) &&
        me->GetCombatDistance(victim) < 8.0f)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-spec: hunter dead zone, autoshot stopped");
        me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
    }

    if (CommandSpecPet(victim, true)) return true;   // cb:fold rotation rung, outcome probed at cast

    Pet* pet = me->GetPet();
    bool const petReady = pet && pet->IsAlive() && pet->GetVictim() == victim;
    float const distance = me->GetCombatDistance(victim);

    if (distance < 8.0f)
    {   // cb:fold rotation rung, outcome probed at cast
        if (spec == 2 && GetAttackersInRangeCount(8.0f) > 0 && TrySpecSpell(me, SP_DETERRENCE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.hunter.pWingClip)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.hunter.pMongooseBite)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.hunter.pRaptorStrike)) return true;   // cb:fold rotation rung, outcome probed at cast
        return false;
    }

    if (spec == 1 && TrySpecAura(me, SP_TRUESHOT_AURA)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecAura(victim, 1130)) return true; // Hunter's Mark   // cb:fold rotation rung, outcome probed at cast

    if (!me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) && !me->IsMoving() &&
        me->HasSpell(SP_AUTO_SHOT))
    {   // cb:fold rotation rung, outcome probed at cast
        SpellCastResult result = me->CastSpell(victim, SP_AUTO_SHOT, false);
        if ((result == SPELL_FAILED_NEED_AMMO || result == SPELL_FAILED_NO_AMMO) && !SuiCommanderRaid::Owns(me))
        {   // cb:fold rotation rung, outcome probed at cast
            CB_HIT(me->GetGUIDLow(), "cpp-spec: out of ammo, restocking");
            AddHunterAmmo();
        }
        if (result == SPELL_CAST_OK)
            return true;   // cb:fold rotation rung, outcome probed at cast
    }

    if (spec == 0) // Beast Mastery
    {   // cb:fold rotation rung, outcome probed at cast
        if (petReady && victim->IsNonMeleeSpellCasted() && TrySpecSpell(pet, SP_INTIMIDATION)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (petReady && victim->GetHealthPercent() > 55.0f && TrySpecSpell(pet, SP_BESTIAL_WRATH)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 35.0f && TrySpecAura(victim, 1978)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.hunter.pArcaneShot)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(victim, 10.0f, 2) && !MultiShotNearSheep(me, victim) && TrySpecSpell(victim, m_spells.hunter.pMultiShot)) return true;   // cb:fold rotation rung, outcome probed at cast
    }
    else if (spec == 1) // Marksmanship
    {   // cb:fold rotation rung, outcome probed at cast
        if (Unit* add = SelectSafeSpecAdd(victim))
            if (TrySpecSpell(add, SP_SCATTER_SHOT)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 55.0f && TrySpecSpell(me, SP_RAPID_FIRE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.hunter.pAimedShot)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(victim, 10.0f, 2) && !MultiShotNearSheep(me, victim) && TrySpecSpell(victim, m_spells.hunter.pMultiShot)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.hunter.pArcaneShot)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 35.0f && TrySpecAura(victim, 1978)) return true;   // cb:fold rotation rung, outcome probed at cast
    }
    else // Survival
    {   // cb:fold rotation rung, outcome probed at cast
        if (Unit* add = SelectSafeSpecAdd(victim))
            if (!HasAuraFromSpellChain(add, 1978) && TrySpecSpell(add, SP_WYVERN_STING)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.hunter.pAimedShot)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(victim, 10.0f, 2) && !MultiShotNearSheep(me, victim) && TrySpecSpell(victim, m_spells.hunter.pMultiShot)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 35.0f && TrySpecAura(victim, 1978)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.hunter.pArcaneShot)) return true;   // cb:fold rotation rung, outcome probed at cast
    }

    if (victim->GetVictim() == me && me->GetHealthPercent() < 55.0f &&
        TrySpecSpell(me, m_spells.hunter.pFeignDeath)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (victim->IsMoving() && victim->GetVictim() == me &&
        TrySpecSpell(victim, m_spells.hunter.pConcussiveShot)) return true;   // cb:fold rotation rung, outcome probed at cast
    return false;
}

bool AiBotAI::UpdateSpecCombatRogue(uint8 spec)
{
    Unit* victim = me->GetVictim();
    if (!victim)
        return false;   // cb:fold rotation rung, outcome probed at cast

    bool const durable = victim->GetHealthPercent() > 45.0f;
    if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
    {   // cb:fold rotation rung, outcome probed at cast
        if (spec == 2) // Subtlety: bank combo points without consuming stealth.
        {   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.rogue.pPremeditation)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.rogue.pAmbush)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.rogue.pCheapShot)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.rogue.pGarrote)) return true;   // cb:fold rotation rung, outcome probed at cast
        }
        else if (spec == 0) // Assassination: prefer the durable/caster bleed opener.
        {   // cb:fold rotation rung, outcome probed at cast
            if ((durable || victim->IsCaster()) &&
                TrySpecSpell(victim, m_spells.rogue.pGarrote)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.rogue.pAmbush)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.rogue.pCheapShot)) return true;   // cb:fold rotation rung, outcome probed at cast
        }
        else // Combat: Cheap Shot is weapon-agnostic; use positional fallbacks.
        {   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.rogue.pCheapShot)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.rogue.pGarrote)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.rogue.pAmbush)) return true;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    if (TrySpecInterrupt(victim, {1766, 1776}) || TrySpecInterruptNearby({1766}, 8.0f)) return true; // Kick before Gouge   // cb:fold rotation rung, outcome probed at cast
    if (me->GetHealthPercent() < 35.0f && victim->GetVictim() == me &&
        TrySpecSpell(me, m_spells.rogue.pEvasion)) return true;   // cb:fold rotation rung, outcome probed at cast

    uint8 combo = me->GetComboTargetGuid() == victim->GetObjectGuid() ? me->GetComboPoints() : 0;

    if (combo >= 4)
    {   // cb:fold rotation rung, outcome probed at cast
        if (spec == 0 && TrySpecSpell(me, SP_COLD_BLOOD)) return true;   // cb:fold rotation rung, outcome probed at cast

        // Deterministic, target-bound finishers: establish Slice and Dice on a
        // durable target, use Rupture only when its full duration is plausible,
        // otherwise cash out with Eviscerate.
        if (durable && !HasAuraFromSpellChain(me, 5171) &&
            TrySpecSpell(me, m_spells.rogue.pSliceAndDice)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (durable && victim->GetHealthPercent() > 70.0f &&
            TrySpecAura(victim, 1943)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.rogue.pEviscerate)) return true;   // cb:fold rotation rung, outcome probed at cast
    }

    if (spec == 1) // Combat
    {   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, SP_RIPOSTE)) return true; // DBC aura-state gates the parry reaction   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(victim, 8.0f, 2) && TrySpecSpell(me, SP_BLADE_FLURRY)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (durable && me->GetPower(POWER_ENERGY) < 45 && TrySpecSpell(me, SP_ADRENALINE_RUSH)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.rogue.pSinisterStrike)) return true;   // cb:fold rotation rung, outcome probed at cast
    }
    else if (spec == 2) // Subtlety
    {   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetVictim() == me && TrySpecSpell(victim, SP_GHOSTLY_STRIKE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, SP_HEMORRHAGE)) return true;   // cb:fold rotation rung, outcome probed at cast

        bool const spentReset =
            (m_spells.rogue.pVanish && !me->IsSpellReady(m_spells.rogue.pVanish->Id)) ||
            (m_spells.rogue.pEvasion && !me->IsSpellReady(m_spells.rogue.pEvasion->Id));
        if (me->GetHealthPercent() < 30.0f && spentReset && TrySpecSpell(me, SP_PREPARATION)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.rogue.pSinisterStrike)) return true;   // cb:fold rotation rung, outcome probed at cast
    }
    else // Assassination
    {   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.rogue.pBackstab)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.rogue.pSinisterStrike)) return true;   // cb:fold rotation rung, outcome probed at cast
    }

    if (me->GetCombatDistance(victim) > 8.0f && TrySpecSpell(me, m_spells.rogue.pSprint)) return true;   // cb:fold rotation rung, outcome probed at cast
    return false;
}

bool AiBotAI::UpdateSpecCombatPriest(uint8 spec)
{
    Unit* victim = me->GetVictim();

    if (TrySpecValuedDispel(m_spells.priest.pDispelMagic, 400.0f)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecPurgeEnemy(m_spells.priest.pDispelMagic)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (m_spells.priest.pAbolishDisease)
        if (Unit* friendUnit = SelectDispelTarget(m_spells.priest.pAbolishDisease))   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(friendUnit, m_spells.priest.pAbolishDisease)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecFearWardTank()) return true;   // cb:fold rotation rung, outcome probed at cast

    if (spec != 2) // Discipline / Holy healers
    {   // cb:fold rotation rung, outcome probed at cast
        Unit* heal = SelectHealTarget(88.0f, 82.0f);
        if (heal)
        {   // cb:fold rotation rung, outcome probed at cast
            if (heal->GetHealthPercent() < 42.0f && TrySpecSpell(me, SP_INNER_FOCUS)) return true;   // cb:fold rotation rung, outcome probed at cast

            bool const rageTank = heal->GetClass() == CLASS_WARRIOR ||
                IsTankingForm(heal->GetShapeshiftForm());
            if (heal->GetHealthPercent() < 45.0f && !rageTank &&
                !heal->HasAura(SP_WEAKENED_SOUL) && TrySpecSpell(heal, SP_POWER_WORD_SHIELD)) return true;   // cb:fold rotation rung, outcome probed at cast

            if (spec == 1 && heal->GetHealthPercent() < 45.0f &&
                me->IsWithinDistInMap(heal, 10.0f) && GetAttackersInRangeCount(10.0f) > 1 &&
                CanUseSpecAoE(me, 10.0f, 1) && TrySpecSpell(me, SP_HOLY_NOVA)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (spec == 0 && heal->GetHealthPercent() >= 45.0f &&
                heal->GetHealthPercent() < 60.0f &&
                me->GetPowerPercent(POWER_MANA) > 55.0f &&
                TrySpecSpell(me, SP_POWER_INFUSION)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (HealInjuredTarget(heal)) return true;   // cb:fold rotation rung, outcome probed at cast
        }

        if (TrySpecValuedDispel(m_spells.priest.pDispelMagic, 1.0f)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim && TrySpecInterrupt(victim, {SP_SILENCE})) return true;   // cb:fold rotation rung, outcome probed at cast
        // A raid's healer keeps its mana and its cast bar for heals: a Holy Fire is 3.5 s the tank
        // waits through (2026-11-05, Golemagg: the priests cast 8-20 Smites and Holy Fires each and
        // the healers were dry at 130 s, the boss at 15 %).
        if (victim && me->GetPowerPercent(POWER_MANA) > 55.0f && !(me->GetGroup() && me->GetGroup()->isRaidGroup()))
        {   // cb:fold rotation rung, outcome probed at cast
            if (spec == 0 && victim->GetHealthPercent() > 60.0f &&
                me->GetPowerPercent(POWER_MANA) > 70.0f &&
                TrySpecSpell(me, SP_POWER_INFUSION)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecAura(victim, 589)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.priest.pHolyFire)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.priest.pSmite)) return true;   // cb:fold rotation rung, outcome probed at cast
        }
        return false;
    }

    // Shadow: remain in Shadowform unless a material emergency justifies
    // cancelling it; never bounce forms for incidental chip damage.
    if (me->GetHealthPercent() < 22.0f)
    {   // cb:fold rotation rung, outcome probed at cast
        if (HasAuraFromSpellChain(me, SP_SHADOWFORM))
        {   // cb:fold rotation rung, outcome probed at cast
            CB_HITV(me->GetGUIDLow(), "cpp-spec: shadow emergency, dropping form to heal", me->GetHealthPercent());
            if (SpellEntry const* form = GetHighestKnownRank(SP_SHADOWFORM))
                me->RemoveAurasDueToSpellByCancel(form->Id);   // cb:fold outcome probed above (shadow emergency)
            return true;
        }
        if (HealInjuredTarget(me)) return true;   // cb:fold rotation rung, outcome probed at cast
    }
    if (TrySpecAura(me, SP_SHADOWFORM)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (!victim) return false;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecInterrupt(victim, {SP_SILENCE})) return true;   // cb:fold rotation rung, outcome probed at cast
    if (victim->GetHealthPercent() > 55.0f && TrySpecAura(victim, SP_VAMPIRIC_EMBRACE)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (victim->GetHealthPercent() > 35.0f && TrySpecAura(victim, 589)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecSpell(victim, m_spells.priest.pMindBlast)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (victim->GetHealthPercent() > 18.0f && TrySpecSpell(victim, SP_MIND_FLAY)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (me->HasSpell(AB_SPELL_SHOOT_WAND) && !me->IsMoving() &&
        !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        return me->CastSpell(victim, AB_SPELL_SHOOT_WAND, false) == SPELL_CAST_OK;   // cb:fold rotation rung, outcome probed at cast
    return false;
}

bool AiBotAI::UpdateSpecCombatShaman(uint8 spec)
{
    if (m_spells.shaman.pGhostWolf && me->GetShapeshiftForm() == FORM_GHOSTWOLF)
    {   // cb:fold rotation rung, outcome probed at cast
        CB_HIT(me->GetGUIDLow(), "cpp-spec: ghost wolf dropped for combat");
        me->RemoveAurasDueToSpellByCancel(m_spells.shaman.pGhostWolf->Id);
        return true;
    }

    // Active fire totems pick their own targets.  Suppress only that element
    // when protected breakable CC is within conservative Searing range; air,
    // earth and water utility totems must remain deployable.
    bool allowFireTotem = true;
    std::list<Unit*> nearbyEnemies;
    me->GetEnemyListInRadiusAround(me, 30.0f, nearbyEnemies);
    for (Unit* enemy : nearbyEnemies)
    {
        if (enemy && me->IsValidAttackTarget(enemy) &&
            enemy->HasBreakableByDamageCrowdControlAura())
        {   // cb:fold rotation rung, outcome probed at cast
            allowFireTotem = false;
            break;
        }
    }
    if (!allowFireTotem)
        if (Totem* fireTotem = me->GetTotem(TOTEM_SLOT_FIRE))   // cb:fold verdict probed at unsummon
        {   // cb:fold rotation rung, outcome probed at cast
            CB_HIT(me->GetGUIDLow(), "cpp-spec: fire totem pulled, breakable cc nearby");
            fireTotem->UnSummon();
        }

    Unit* victim = me->GetVictim();

    if (spec == 2) // Restoration
    {   // cb:fold rotation rung, outcome probed at cast
        Unit* heal = SelectHealTarget(90.0f, 85.0f);
        if (heal)
        {   // cb:fold rotation rung, outcome probed at cast
            if (heal->GetHealthPercent() < 35.0f && TrySpecSpell(me, SP_NATURES_SWIFTNESS_SHAMAN)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (HealInjuredTarget(heal)) return true;   // cb:fold rotation rung, outcome probed at cast
        }
        if (me->GetPowerPercent(POWER_MANA) < 60.0f && TrySpecSpell(me, SP_MANA_TIDE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.shaman.pCurePoison)
            if (Unit* friendUnit = SelectDispelTarget(m_spells.shaman.pCurePoison))   // cb:fold rotation rung, outcome probed at cast
                if (TrySpecSpell(friendUnit, m_spells.shaman.pCurePoison)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.shaman.pCureDisease)
            if (Unit* friendUnit = SelectDispelTarget(m_spells.shaman.pCureDisease))   // cb:fold rotation rung, outcome probed at cast
                if (TrySpecSpell(friendUnit, m_spells.shaman.pCureDisease)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim && TrySpecInterrupt(victim, {8042})) return true;   // cb:fold rotation rung, outcome probed at cast
        if (SummonShamanTotems(allowFireTotem)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim && me->GetPowerPercent(POWER_MANA) > 70.0f && TrySpecSpell(victim, m_spells.shaman.pLightningBolt)) return true;   // cb:fold rotation rung, outcome probed at cast
        return false;
    }

    if (!victim)
        return false;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecInterrupt(victim, {8042})) return true;   // cb:fold rotation rung, outcome probed at cast
    if (me->GetHealthPercent() < 32.0f && FindAndHealInjuredAlly(32.0f, 22.0f)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecAura(me, 324)) return true; // Lightning Shield   // cb:fold rotation rung, outcome probed at cast
    if (SummonShamanTotems(allowFireTotem)) return true;   // cb:fold rotation rung, outcome probed at cast

    if (spec == 1) // Enhancement
    {   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, SP_STORMSTRIKE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.shaman.pEarthShock)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 45.0f && TrySpecAura(victim, 8050)) return true;   // cb:fold rotation rung, outcome probed at cast
        return false; // auto attack remains the resource-free filler
    }

    // Elemental
    if (victim->GetHealthPercent() > 50.0f && TrySpecSpell(me, SP_ELEMENTAL_MASTERY)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (CanUseSpecAoE(victim, 10.0f, 2) && TrySpecSpell(victim, m_spells.shaman.pChainLightning)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (victim->GetHealthPercent() > 40.0f && TrySpecAura(victim, 8050)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecSpell(victim, m_spells.shaman.pLightningBolt)) return true;   // cb:fold rotation rung, outcome probed at cast
    return false;
}

namespace
{
    std::mutex gDispelClaimLock;
    std::map<ObjectGuid, uint32> gDispelClaims;   // member -> a dispel just landed there (until ms)
}

float AiBotAI::DispelValue(Player const* member, uint32 dispelMask)
{
    bool const manaUser = member->GetPowerType() == POWER_MANA;
    bool healer = false;
    bool tank = false;
    if (AiBotAI const* ai = dynamic_cast<AiBotAI const*>(const_cast<Player*>(member)->AI()))
    {
        healer = ai->GetCombatActiveRole() == ROLE_HEALER;
        tank = ai->GetCombatActiveRole() == ROLE_TANK;
    }
    else
    {
        uint8 const cls = member->GetClass();
        healer = cls == CLASS_PRIEST || cls == CLASS_DRUID || cls == CLASS_SHAMAN || cls == CLASS_PALADIN;
        tank = cls == CLASS_WARRIOR;
    }

    float total = 0.0f;
    for (auto const& entry : member->GetSpellAuraHolderMap())
    {
        SpellAuraHolder const* holder = entry.second;
        SpellEntry const* proto = holder->GetSpellProto();
        if (!((1 << proto->Dispel) & dispelMask) || holder->IsPositive())
            continue;
        float value = 0.0f;
        bool known = false;
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            Aura const* aura = holder->GetAuraByEffectIndex(SpellEffectIndex(i));
            if (!aura)
                continue;
            Modifier const* mod = aura->GetModifier();
            switch (mod->m_auraname)
            {
                case SPELL_AURA_PERIODIC_DAMAGE:
                case SPELL_AURA_PERIODIC_LEECH:
                {
                    // damage still to land: a 10 s single-tick "doom" is worth its whole hit
                    int32 const period = mod->periodictime > 0 ? mod->periodictime : 1000;
                    int32 const left = aura->GetAuraDuration();
                    int32 const next = aura->GetAuraPeriodicTimer();
                    int32 ticks = left < 0 ? 5 : (left >= next ? 1 + (left - next) / period : 1);
                    ticks = std::min(ticks, 10);
                    value += std::max(0.0f, mod->m_amount) * ticks;
                    known = true;
                    break;
                }
                case SPELL_AURA_MOD_CHARM:
                case SPELL_AURA_MOD_POSSESS:
                case SPELL_AURA_MOD_FEAR:
                case SPELL_AURA_MOD_STUN:
                case SPELL_AURA_MOD_CONFUSE:
                    value += 4000.0f;   // a body lost to the fight
                    known = true;
                    break;
                case SPELL_AURA_MOD_SILENCE:
                case SPELL_AURA_MOD_PACIFY_SILENCE:
                    value += healer ? 2000.0f : (manaUser ? 500.0f : 0.0f);
                    known = true;
                    break;
                case SPELL_AURA_MOD_POWER_COST_SCHOOL_PCT:
                case SPELL_AURA_MOD_POWER_COST_SCHOOL:
                    value += manaUser ? (healer ? 1200.0f : 400.0f) : 0.0f;
                    known = true;
                    break;
                case SPELL_AURA_PERIODIC_MANA_LEECH:
                case SPELL_AURA_POWER_BURN_MANA:
                    value += manaUser ? (healer ? 1000.0f : 400.0f) : 0.0f;
                    known = true;
                    break;
                case SPELL_AURA_MOD_ROOT:
                    value += 300.0f;
                    known = true;
                    break;
                case SPELL_AURA_MOD_DECREASE_SPEED:
                    value += 150.0f;
                    known = true;
                    break;
                case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                    // by how much: Shazzrah's Curse doubles every explosion the raid takes
                    value += mod->m_amount > 0 ? std::max(400.0f, 15.0f * mod->m_amount) : 0.0f;
                    known = true;
                    break;
                case SPELL_AURA_MOD_HEALING_PCT:
                    value += mod->m_amount < 0 ? 400.0f : 0.0f;
                    known = true;
                    break;
                default:
                    break;
            }
        }
        total += known ? value : 50.0f;
    }
    return tank ? total * 1.5f : total;
}

Player* AiBotAI::SelectValuedDispelTarget(SpellEntry const* spell, float minValue) const
{
    if (!spell)
        return nullptr;
    uint32 mask = 0;
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (spell->Effect[i] == SPELL_EFFECT_DISPEL)
            mask |= Spells::GetDispellMask(DispelType(spell->EffectMiscValue[i]));
    if (!mask)
        return nullptr;

    Group* group = me->GetGroup();
    if (!group)
        return DispelValue(me, mask) >= minValue ? me : nullptr;

    uint32 const now = WorldTimer::getMSTime();
    Player* best = nullptr;
    float bestValue = 0.0f;
    std::lock_guard<std::mutex> guard(gDispelClaimLock);
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->getSource();
        if (!m || !m->IsAlive() || !m->IsInWorld() || !m->IsInMap(me) || m->IsGameMaster())
            continue;
        // a member a creature has charmed is hostile for now, and the dispel is what frees it
        bool const charmed = !m->GetCharmerGuid().IsEmpty() && !m->GetCharmerGuid().IsPlayer();
        if (!me->IsWithinDistInMap(m, 30.0f) || !(charmed || me->IsValidHelpfulTarget(m)) || !me->IsWithinLOSInMap(m))
            continue;
        float value = DispelValue(m, mask);
        if (value < minValue)
            continue;
        auto claim = gDispelClaims.find(m->GetObjectGuid());
        if (claim != gDispelClaims.end() && claim->second > now)
            value *= 0.25f;   // another dispeller just worked there
        if (value > bestValue)
        {
            best = m;
            bestValue = value;
        }
    }
    return best;
}

bool AiBotAI::TrySpecValuedDispel(SpellEntry const* spell, float minValue)
{
    Player* target = SelectValuedDispelTarget(spell, minValue);
    if (!target || !TrySpecSpell(target, spell))
        return false;
    std::lock_guard<std::mutex> guard(gDispelClaimLock);
    gDispelClaims[target->GetObjectGuid()] = WorldTimer::getMSTime() + 800;
    return true;
}

bool AiBotAI::TrySpecPurgeEnemy(SpellEntry const* spell)
{
    if (!spell || !me->IsInCombat())
        return false;
    uint32 mask = 0;
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (spell->Effect[i] == SPELL_EFFECT_DISPEL)
            mask |= Spells::GetDispellMask(DispelType(spell->EffectMiscValue[i]));
    if (!mask)
        return false;
    std::list<Unit*> enemies;
    me->GetEnemyListInRadiusAround(me, 30.0f, enemies);
    for (Unit* u : enemies)
    {
        if (!u || !u->IsCreature() || !u->IsAlive() || !u->IsInCombat() || !u->GetVictim() ||
            !me->IsWithinLOSInMap(u))
            continue;
        bool worth = false;
        for (auto const& entry : u->GetSpellAuraHolderMap())
        {
            SpellAuraHolder const* h = entry.second;
            if (!h || !h->IsPositive() || !((1 << h->GetSpellProto()->Dispel) & mask))
                continue;
            for (uint8 i = 0; i < MAX_EFFECT_INDEX && !worth; ++i)
                if (Aura const* a = h->GetAuraByEffectIndex(SpellEffectIndex(i)))
                {
                    Modifier const* m = a->GetModifier();
                    switch (m->m_auraname)
                    {
                        case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN: worth = m->m_amount < 0; break;
                        case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:  worth = m->m_amount > 0; break;
                        case SPELL_AURA_SCHOOL_ABSORB:
                        case SPELL_AURA_MOD_ATTACKSPEED:
                        case SPELL_AURA_MOD_MELEE_HASTE:
                        case SPELL_AURA_REFLECT_SPELLS:
                        case SPELL_AURA_REFLECT_SPELLS_SCHOOL:     worth = true; break;
                        default: break;
                    }
                }
            if (worth)
                break;
        }
        if (worth && TrySpecSpell(u, spell))
            return true;
    }
    return false;
}

// [TACTICS] A priest who has Fear Ward keeps it on a tank that lacks it (a feared tank drops
// the boss on the healers). Ordinary raid practice; no encounter is named.
bool AiBotAI::TrySpecFearWardTank()
{
    uint32 const kFearWard = 6346;
    if (!me->HasSpell(kFearWard) || !me->IsSpellReady(kFearWard) || !me->GetGroup())
        return false;
    for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->getSource();
        if (!m || !m->IsAlive() || m->HasAura(kFearWard) || !m->IsWithinDistInMap(me, 30.0f))
            continue;
        AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI());
        if (!ai || ai->GetCombatActiveRole() != ROLE_TANK)
            continue;
        if (TrySpecSpell(m, kFearWard))
            return true;
    }
    return false;
}

bool AiBotAI::UpdateSpecCombatMage(uint8 spec)
{
    // [TACTICS] A mage is the raid's decurser, not only its own (a curse on the healers doubles
    // their mana costs; one on the tank can kill it).
    if (TrySpecValuedDispel(m_spells.mage.pRemoveLesserCurse, 1.0f)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecPolymorphSpareElite()) return true;   // cb:fold rotation rung, outcome probed at cast

    Unit* victim = me->GetVictim();
    if (!victim)
        return false;   // cb:fold rotation rung, outcome probed at cast

    if (TrySpecInterrupt(victim, {2139}) || TrySpecInterruptNearby({2139}, 30.0f)) return true;   // cb:fold rotation rung, outcome probed at cast

    // Polymorph is strictly add-only.  The legacy target mix-up (select add,
    // cast on primary) cannot occur because the selected add is passed through.
    if (Unit* add = SelectSafeSpecAdd(victim))
        if (TrySpecSpell(add, m_spells.mage.pPolymorph)) return true;   // cb:fold rotation rung, outcome probed at cast

    if (me->GetHealthPercent() < 25.0f && TrySpecSpell(me, m_spells.mage.pIceBlock)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (me->GetPowerPercent(POWER_MANA) < 20.0f &&
        GetAttackersInRangeCount(10.0f) == 0 && TrySpecSpell(me, m_spells.mage.pEvocation)) return true;   // cb:fold rotation rung, outcome probed at cast

    if (spec == 0) // Arcane
    {   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 60.0f && me->GetPowerPercent(POWER_MANA) > 65.0f &&
            TrySpecSpell(me, SP_ARCANE_POWER)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 45.0f && TrySpecSpell(me, SP_PRESENCE_OF_MIND)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (HasAuraFromSpellChain(me, SP_PRESENCE_OF_MIND))
        {   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, 11366)) return true; // Pyroblast if the profile knows it   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.mage.pFireball)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (TrySpecSpell(victim, m_spells.mage.pFrostbolt)) return true;   // cb:fold rotation rung, outcome probed at cast
        }
        if (CanUseSpecAoE(me, 10.0f, 3) && TrySpecSpell(me, m_spells.mage.pArcaneExplosion)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, SP_ARCANE_MISSILES)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.mage.pFrostbolt)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.mage.pFireball)) return true;   // cb:fold rotation rung, outcome probed at cast
    }
    else if (spec == 1) // Fire
    {   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 60.0f && TrySpecSpell(me, SP_COMBUSTION)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(me, 10.0f, 3) && TrySpecSpell(me, SP_BLAST_WAVE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(me, 10.0f, 3) && TrySpecSpell(me, m_spells.mage.pArcaneExplosion)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() < 22.0f && TrySpecSpell(victim, m_spells.mage.pFireBlast)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 25.0f && TrySpecSpell(victim, m_spells.mage.pFireball)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.mage.pScorch)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.mage.pFrostbolt)) return true; // fire-immune fallback   // cb:fold rotation rung, outcome probed at cast
    }
    else // Frost
    {   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecAura(me, SP_ICE_BARRIER)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (GetAttackersInRangeCount(8.0f) > 0 && CanUseSpecAoE(me, 10.0f, 1) &&
            TrySpecSpell(me, m_spells.mage.pFrostNova)) return true;   // cb:fold rotation rung, outcome probed at cast
        // Mana discipline: the area spells are the expensive ones (a Blizzard is 1400 mana).
        if (me->GetPowerPercent(POWER_MANA) > 25.0f &&
            CanUseSpecAoE(me, 10.0f, 3) && TrySpecSpell(me, m_spells.mage.pArcaneExplosion)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetPowerPercent(POWER_MANA) > 30.0f &&
            CanUseSpecAoE(victim, 10.0f, 2) && TrySpecSpell(victim, m_spells.mage.pBlizzard)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetHealthPercent() < 35.0f &&
            !me->IsSpellReady(SP_ICE_BARRIER) && TrySpecSpell(me, SP_COLD_SNAP)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.mage.pFrostbolt)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() < 18.0f && TrySpecSpell(victim, m_spells.mage.pFireBlast)) return true;   // cb:fold rotation rung, outcome probed at cast
    }

    if (me->HasSpell(AB_SPELL_SHOOT_WAND) && !me->IsMoving() &&
        !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        return me->CastSpell(victim, AB_SPELL_SHOOT_WAND, false) == SPELL_CAST_OK;   // cb:fold rotation rung, outcome probed at cast
    return false;
}

static bool IsBanishedUnit(Unit const* u)
{
    for (auto const& entry : u->GetSpellAuraHolderMap())
        if (entry.second && entry.second->HasMechanic(MECHANIC_BANISH))
            return true;
    return false;
}

// The unit this caster already holds under its own control of that mechanic. One per caster in
// this game: a second Banish or Polymorph frees the first (2026-11-03, Garr: four warlocks
// re-banished a new Firesworn every 2-3 s, each cast waking the one before - none stayed out).
static Unit* OwnControlled(Player const* me, std::list<Unit*> const& nearby, Mechanics mechanic)
{
    for (Unit* u : nearby)
        if (u->IsAlive())
            for (auto const& entry : u->GetSpellAuraHolderMap())
                if (entry.second && entry.second->HasMechanic(mechanic) && entry.second->GetCasterGuid() == me->GetObjectGuid())
                    return u;
    return nullptr;
}

// [TACTICS] More elites on the raid than tanks to hold them: a warlock banishes a spare one
// (an elite no tank is holding, never a boss). The spell's own creature-type rule decides what
// can be banished; a banished elite is re-banished when it wakes if the raid is still short.
// A sheep claimed by the mage who started it, for the cast: mages deciding in the same tick do not see
// each other's casts yet (2026-11-06, Majordomo: 12 Polymorphs on four Healers in 7 s, two mages per
// Healer, each new sheep replacing the last - one Healer stayed awake and killed the raid).
static std::mutex gPolyClaimLock;
static std::map<ObjectGuid, std::pair<ObjectGuid, uint32>> gPolyClaims;   // target -> (mage, until ms)

bool AiBotAI::TrySpecPolymorphSpareElite()
{
    SpellEntry const* poly = m_spells.mage.pPolymorph;
    Group* group = me->GetGroup();
    if (!poly || !group || me->IsNonMeleeSpellCasted(false) || !me->IsInCombat())
        return false;
    uint32 tanks = 0;
    std::set<Unit*> castTargets;   // sheep other mages are already casting
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->getSource();
        if (!m || !m->IsAlive() || !m->IsInMap(me))
            continue;
        if (AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI()))
            if (ai->GetCombatActiveRole() == ROLE_TANK)
                ++tanks;
        if (Spell* s = m->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (s->m_spellInfo && s->m_spellInfo->SpellIconID == poly->SpellIconID)
                if (Unit* u = CastTarget(me, s))
                    castTargets.insert(u);
    }
    std::list<Unit*> nearby;
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(me, me, 30.0f);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(nearby, check);
    Cell::VisitAllObjects(me, searcher, 30.0f);
    // (my sheep is looked for further out than I cast: a sheep 30-45 yd away ran out unrenewed and woke
    // 20 s before the last Elite died - 2026-11-07, Majordomo)
    std::list<Unit*> wide;
    {
        MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck wideCheck(me, me, 45.0f);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> wideSearcher(wide, wideCheck);
        Cell::VisitAllObjects(me, wideSearcher, 45.0f);
    }
    if (Unit* mine = OwnControlled(me, wide, MECHANIC_POLYMORPH))
    {
        // My sheep stands; a new one would wake it. It is renewed in its last seconds, not left to
        // run out (2026-11-06, Majordomo: four Healers sheeped in the first 4 s all woke together
        // 50 s later and the raid died inside 40 s).
        int32 left = -1;
        for (auto const& entry : mine->GetSpellAuraHolderMap())
            if (entry.second && entry.second->HasMechanic(MECHANIC_POLYMORPH) && entry.second->GetCasterGuid() == me->GetObjectGuid())
                left = entry.second->GetAuraDuration();
        // (cast straight: the spell gate refuses a target already wearing the aura)
        // 12 s ahead: a 10 s spell reflection on it would otherwise outlast the window
        bool const reflects = mine->HasAuraType(SPELL_AURA_REFLECT_SPELLS) || mine->HasAuraType(SPELL_AURA_REFLECT_SPELLS_SCHOOL);
        if (left >= 0 && left < 12000 && !reflects && mine != PlaybookReleaseTarget(me) &&
            (me->GetDistance(mine) > 28.0f || !me->IsWithinLOSInMap(mine)))
        {
            // out of reach: walk in to renew it
            float dx = me->GetPositionX() - mine->GetPositionX(), dy = me->GetPositionY() - mine->GetPositionY();
            float const len = std::max(0.5f, std::sqrt(dx * dx + dy * dy));
            // In reach but out of sight, a spot 22 yd out on my own line was where I stood: the mage "walked"
            // 12 s without moving and the sheep ran out (2026-11-08, Majordomo attempt 46). Closer in, and
            // round the sheep, to a spot that sees it.
            float const r = len > 28.0f ? 22.0f : std::max(8.0f, std::min(15.0f, len - 4.0f));
            float const base = std::atan2(dy, dx);
            float x = mine->GetPositionX() + dx / len * 6.0f, y = mine->GetPositionY() + dy / len * 6.0f;
            float z = mine->GetPositionZ();
            me->UpdateAllowedPositionZ(x, y, z);
            for (float turn : { 0.0f, 0.524f, -0.524f, 1.047f, -1.047f, 1.571f, -1.571f })
            {
                float cx = mine->GetPositionX() + std::cos(base + turn) * r, cy = mine->GetPositionY() + std::sin(base + turn) * r;
                float cz = mine->GetPositionZ();
                me->UpdateAllowedPositionZ(cx, cy, cz);
                if (std::fabs(cz - mine->GetPositionZ()) > 6.0f ||
                    !me->IsWithinLOSAtPosition(cx, cy, cz, mine->GetPositionX(), mine->GetPositionY(), mine->GetPositionZ(), true, 1.0f))
                    continue;
                x = cx;
                y = cy;
                z = cz;
                break;
            }
            if (!me->IsMoving())
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-CC] %s: walks into reach to renew the polymorph on %s (guid %u, %.0f yd), %d ms left",
                    me->GetName(), mine->GetName(), mine->GetGUIDLow(), me->GetDistance(mine), left);
                me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
            }
            return true;
        }
        if (left >= 0 && left < 12000 && !reflects && mine != PlaybookReleaseTarget(me) && DoCastSpell(mine, poly) == SPELL_CAST_OK)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-CC] %s: renews the polymorph on %s (guid %u), %d ms left",
                me->GetName(), mine->GetName(), mine->GetGUIDLow(), left);
            return true;
        }
        return false;
    }
    uint32 active = 0;
    Unit* spare = nullptr;
    int spareScore = -1;
    uint32 spareHitters = 1000;
    bool spareBursts = false;
    for (Unit* u : nearby)
    {
        if (!u->IsCreature() || !u->IsAlive() || !u->IsInCombat() || !static_cast<Creature*>(u)->IsElite())
            continue;
        Unit* v = u->GetVictim();
        Player* vp = v ? v->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!vp || !group->IsMember(vp->GetObjectGuid()))
            continue;
        if (IsBanishedUnit(u) || castTargets.count(u) || u->HasBreakableByDamageCrowdControlAura())
            continue;   // already controlled
        ++active;
        if (static_cast<Creature*>(u)->IsWorldBoss() || u->IsImmuneToMechanic(MECHANIC_POLYMORPH))
            continue;
        {
            std::lock_guard<std::mutex> guard(gPolyClaimLock);
            auto c = gPolyClaims.find(u->GetObjectGuid());
            if (c != gPolyClaims.end() && c->second.first != me->GetObjectGuid() && WorldTimer::getMSTime() < c->second.second)
                continue;   // another mage is sheeping it
        }
        if (RecentlyReleased(u))
            continue;   // the playbook's next kill, woken on purpose
        if (InGroupChannelArea(me, u))
            continue;
        // a sheep under a damage-over-time (an Immolate, a Rain of Fire it stands in) breaks at the
        // next tick (2026-09-25: 9 of 14 broken sheep on Majordomo's healers)
        if (u->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE) || u->HasAuraType(SPELL_AURA_PERIODIC_LEECH))
            continue;
        uint32 hitters = 0;   // the raid's own damage on it: sheep the one nobody is killing
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* m = itr->getSource())
                if (m->IsAlive() && m->GetVictim() == u)
                    ++hitters;
        // the raid's kill target: a sheep would only stop the kill - once it is being killed; at the
        // pull every damage dealer's first pick still counts as "hitting" (2026-11-06: no sheep for 14 s)
        if (hitters >= 3 && u->GetHealthPercent() < 90.0f)
            continue;
        AiBotAI* holderAi = dynamic_cast<AiBotAI*>(vp->AI());
        // A burster held by a tank still hits everyone around that tank: it is sheeped like a loose one
        // unless the raid is killing it (2026-11-07, Majordomo with five tanks: the fifth tank took the
        // fourth Healer, so it was never sheeped and its Shadow Shocks killed from 12 s on).
        bool const bursts = hitters < 3 && SuiRaidTelemetry::EntryPulseRadius(u->GetEntry()) > 0.0f;
        // (an off-tank's add is loose all the same: the sheep frees it - 2026-11-09, attempt 66)
        bool const onTank = holderAi && holderAi->GetCombatActiveRole() == ROLE_TANK && !holderAi->IsOffTankOnDuty() && !bursts;
        // a healer kind first: it undoes the raid's damage and a tank is better spent on an elite
        // that hits (2026-11-06, Majordomo: one Flamewaker Healer sheeped of four in the first minute)
        int const score = (onTank ? (vp->GetVictim() == u ? 0 : 1) : 2) * 2 +
            (KindHeals(static_cast<Creature const*>(u)) ? 1 : 0);
        if ((score > spareScore || (score == spareScore && hitters < spareHitters)) && CanTryToCastSpell(u, poly))
        {
            spareScore = score;
            spareHitters = hitters;
            spare = u;
            spareBursts = bursts;
        }
    }
    uint32 const cap = std::max<uint32>(1u, (tanks + 1) / 2);
    if (!spare || (active <= cap && !spareBursts) || spareScore / 2 == 0)
        return false;
    if (!TrySpecSpell(spare, poly))
        return false;
    {
        std::lock_guard<std::mutex> guard(gPolyClaimLock);
        gPolyClaims[spare->GetObjectGuid()] = std::make_pair(me->GetObjectGuid(), WorldTimer::getMSTime() + 2500);
    }
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-CC] %s: polymorph %s (guid %u) - %u elites on the raid, %u tanks",
        me->GetName(), spare->GetName(), spare->GetGUIDLow(), active, tanks);
    return true;
}

bool AiBotAI::TrySpecBanishSpareElite()
{
    SpellEntry const* banish = m_spells.warlock.pBanish;
    Group* group = me->GetGroup();
    if (!banish || !group || me->IsNonMeleeSpellCasted(false))
        return false;
    uint32 tanks = 0;
    std::set<Unit*> castTargets;   // banishes other warlocks are already casting
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->getSource();
        if (!m || !m->IsAlive() || !m->IsInMap(me))
            continue;
        if (AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI()))
            if (ai->GetCombatActiveRole() == ROLE_TANK)
                ++tanks;
        if (Spell* s = m->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (s->m_spellInfo && s->m_spellInfo->SpellIconID == banish->SpellIconID)
                if (Unit* u = CastTarget(me, s))
                    castTargets.insert(u);
    }
    std::list<Unit*> nearby;
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(me, me, 35.0f);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(nearby, check);
    Cell::VisitAllObjects(me, searcher, 35.0f);
    if (OwnControlled(me, nearby, MECHANIC_BANISH))
        return false;   // my banish holds; a new one would free it
    uint32 active = 0;
    Unit* spare = nullptr;
    int spareScore = -1;
    for (Unit* u : nearby)
    {
        if (!u->IsCreature() || !u->IsAlive() || !u->IsInCombat() || !static_cast<Creature*>(u)->IsElite())
            continue;
        Unit* v = u->GetVictim();
        Player* vp = v ? v->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!vp || !group->IsMember(vp->GetObjectGuid()))
            continue;
        if (IsBanishedUnit(u) || castTargets.count(u) || u->HasBreakableByDamageCrowdControlAura())
            continue;   // already controlled
        ++active;
        if (static_cast<Creature*>(u)->IsWorldBoss())
            continue;
        // Never the one the raid is killing: a banish makes it immune and sends the damage dealers
        // elsewhere (the loose one the raid peels is exactly the one "no tank holds").
        uint32 hitters = 0;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* m = itr->getSource())
                if (m != me && m->IsAlive() && m->GetVictim() == u)
                    ++hitters;
        if (hitters >= 3 || u->GetHealthPercent() < 80.0f)
            continue;
        AiBotAI* holderAi = dynamic_cast<AiBotAI*>(vp->AI());
        bool const onTank = holderAi && holderAi->GetCombatActiveRole() == ROLE_TANK;
        // Prefer one no tank holds (it is killing someone), then any but a tank's own target.
        int const score = onTank ? (vp->GetVictim() == u ? 0 : 1) : 2;
        if (score > spareScore && CanTryToCastSpell(u, banish))
        {
            spareScore = score;
            spare = u;
        }
    }
    // Two at a time for four tanks: a pack fought whole spreads the tanks and the heals over every
    // elite at once (2026-09-25: Firewalker + Flameguard + two Lava Elementals wiped the raid again
    // and again with five warlocks never banishing - the old rule waited for more elites than tanks).
    uint32 const cap = std::max<uint32>(1u, (tanks + 1) / 2);
    if (!spare || active <= cap || spareScore == 0)
        return false;
    if (!TrySpecSpell(spare, banish))
        return false;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-CC] %s: banish %s (guid %u) - %u elites on the raid, %u tanks",
        me->GetName(), spare->GetName(), spare->GetGUIDLow(), active, tanks);
    return true;
}

bool AiBotAI::UpdateSpecCombatWarlock(uint8 spec)
{
    if (TrySpecBanishSpareElite()) return true;   // cb:fold rotation rung, outcome probed at cast

    Unit* victim = me->GetVictim();
    if (!victim)
        return false;   // cb:fold rotation rung, outcome probed at cast

    if (CommandSpecPet(victim, false)) return true;   // cb:fold rotation rung, outcome probed at cast
    Pet* pet = me->GetPet();

    if (spec == 1 && (!pet || !pet->IsAlive()))
    {   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(me, SP_FEL_DOMINATION)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(me, 691)) return true; // stable Demonology Felhunter   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(me, 697)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(me, 688)) return true;   // cb:fold rotation rung, outcome probed at cast
    }

    if (pet && pet->IsAlive() && pet->GetHealthPercent() < 35.0f &&
        me->GetHealthPercent() > 65.0f && TrySpecSpell(pet, SP_HEALTH_FUNNEL)) return true;   // cb:fold rotation rung, outcome probed at cast
    if ((victim->CanReachWithMeleeAutoAttack(me) || victim->IsNonMeleeSpellCasted()) &&
        TrySpecSpell(victim, m_spells.warlock.pDeathCoil)) return true;   // cb:fold rotation rung, outcome probed at cast

    // Fear is deliberately never applied to the kill target.  It is an add-only
    // peel and SelectSafeSpecAdd rejects targets being damaged by the group.
    if (Unit* add = SelectSafeSpecAdd(victim))
        if (TrySpecSpell(add, m_spells.warlock.pFear)) return true;   // cb:fold rotation rung, outcome probed at cast

    if (spec == 0) // Affliction
    {   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 30.0f && TrySpecAura(victim, 172)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 55.0f && TrySpecAura(victim, 980)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 55.0f && TrySpecSpell(victim, m_spells.warlock.pSiphonLife)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetHealthPercent() < 55.0f && TrySpecSpell(victim, m_spells.warlock.pDrainLife)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (pet && pet->GetPowerPercent(POWER_MANA) > 45.0f &&
            me->GetPowerPercent(POWER_MANA) < 35.0f && TrySpecSpell(pet, SP_DARK_PACT)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() < 10.0f && TrySpecSpell(victim, SP_DRAIN_SOUL)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.warlock.pShadowBolt)) return true;   // cb:fold rotation rung, outcome probed at cast
    }
    else if (spec == 1) // Demonology
    {   // cb:fold rotation rung, outcome probed at cast
        // A living pet and Soul Link are the policy.  Demonic Sacrifice is never
        // automatic: sacrificing and immediately resummoning was a destructive loop.
        if (pet && pet->IsAlive() && !HasSoulLinkAura(me) &&
            TrySpecSpell(pet, SP_SOUL_LINK)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 35.0f && TrySpecAura(victim, 172)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 45.0f && TrySpecAura(victim, 348)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetHealthPercent() < 45.0f && TrySpecSpell(victim, m_spells.warlock.pDrainLife)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.warlock.pShadowBolt)) return true;   // cb:fold rotation rung, outcome probed at cast
    }
    else // Destruction
    {   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() < 12.0f && TrySpecSpell(victim, m_spells.warlock.pShadowburn)) return true;   // cb:fold rotation rung, outcome probed at cast
        bool const immolated = HasAuraFromSpellChain(victim, 348);
        if (!immolated && victim->GetHealthPercent() > 28.0f && TrySpecSpell(victim, m_spells.warlock.pImmolate)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (immolated && victim->GetHealthPercent() < 45.0f && TrySpecSpell(victim, m_spells.warlock.pConflagrate)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(victim, 10.0f, 2) && me->GetHealthPercent() > 70.0f &&
            TrySpecSpell(victim, m_spells.warlock.pRainOfFire)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.warlock.pShadowBolt)) return true;   // cb:fold rotation rung, outcome probed at cast
    }

    if (me->GetPowerPercent(POWER_MANA) < 12.0f && me->GetHealthPercent() > 70.0f &&
        TrySpecSpell(me, m_spells.warlock.pLifeTap)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (me->HasSpell(AB_SPELL_SHOOT_WAND) && !me->IsMoving() &&
        !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        return me->CastSpell(victim, AB_SPELL_SHOOT_WAND, false) == SPELL_CAST_OK;   // cb:fold rotation rung, outcome probed at cast
    return false;
}

bool AiBotAI::UpdateSpecCombatDruid(uint8 spec)
{
    if (me->GetShapeshiftForm() == FORM_TRAVEL)
    {   // cb:fold rotation rung, outcome probed at cast
        CB_HIT(me->GetGUIDLow(), "cpp-spec: travel form dropped for combat");
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        return true;
    }

    Unit* victim = me->GetVictim();

    auto selectInnervateTarget = [this]() -> Player*
    {
        SpellEntry const* spell = m_spells.druid.pInnervate;
        if (!spell)
            return nullptr;   // cb:fold no learned spell, no support decision exists

        Player* best = nullptr;
        int32 bestScore = -1;
        auto consider = [this, spell, &best, &bestScore](Player* target)
        {
            if (!target || !target->IsAlive() || target->IsGameMaster() ||
                !me->IsValidHelpfulTarget(target) ||
                !me->IsWithinLOSInMap(target) || !me->IsWithinDist(target, 30.0f) ||
                !IsValidBuffTarget(target, spell))
                return;   // cb:fold pure recipient filter, winner probed before cast

            uint32 const maxMana = target->GetMaxPower(POWER_MANA);
            if (!maxMana)
                return;   // cb:fold zero-mana recipient cannot benefit
            uint32 const manaPct = uint32(
                (uint64(target->GetPower(POWER_MANA)) * 100u) / maxMana);
            if (manaPct >= 35)
                return;   // cb:fold support threshold not crossed

            int32 score = 100 - int32(manaPct);
            CombatBotRoles const role = GetSpecBuffTargetRole(target);
            if (role == ROLE_HEALER)
                score += 300;   // cb:fold healer-first support score
            else if (role == ROLE_RANGE_DPS)
                score += 50;   // cb:fold caster fallback score
            else if (role == ROLE_MELEE_DPS)
                score += 10;   // cb:fold mana-melee fallback score
            else if (IsHealerClass(target->GetClass()))
                score += 150;   // cb:fold human/non-profile healer fallback
            if (target == me)
                ++score;   // cb:fold stable final tie-breaker

            if (score > bestScore)
            {   // cb:fold deterministic winner bookkeeping, winner is probed before cast
                best = target;
                bestScore = score;
            }
        };

        consider(me);
        if (Group* party = me->GetGroup())
            for (GroupReference* itr = party->GetFirstMember(); itr; itr = itr->next())   // cb:fold deterministic census, winner is probed
                if (Player* member = itr->getSource())
                    if (member != me)   // cb:fold deterministic census, winner is probed
                        consider(member);   // cb:fold deterministic group census, winner probed below
        return best;
    };

    auto trySupportInnervate = [this, &selectInnervateTarget]() -> bool
    {
        Player* target = selectInnervateTarget();
        SpellEntry const* spell = m_spells.druid.pInnervate;
        if (!target || !spell)
            return false;   // cb:fold no eligible depleted mana recipient

        TraceSpecBuffSelection(this, target, spell,
                               BUFF_DRUID_INNERVATE_SUPPORT, false);
        if (CanTryToRefreshAuraAfterLeavingForm(target, spell))
        {
            CB_HITV(me->GetGUIDLow(), "cpp-buff: left form, cast deferred", spell->Id);
            me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
            return true;
        }
        if (!CanTryToRefreshAura(target, spell))
        {
            CB_HITV(me->GetGUIDLow(), "cpp-buff: cast precheck rejected", spell->Id);
            return false;
        }

        SpellCastResult const result = DoCastSpell(target, spell);
        if (result != SPELL_CAST_OK)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-buff: cast failed", uint32(result));
            return false;
        }
        CB_HITV(me->GetGUIDLow(), "cpp-buff: cast started", spell->Id);
        return true;
    };

    if (spec == 2) // Restoration
    {   // cb:fold rotation rung, outcome probed at cast
        if (me->GetShapeshiftForm() != FORM_NONE)
        {   // cb:fold rotation rung, outcome probed at cast
            CB_HIT(me->GetGUIDLow(), "cpp-spec: resto druid leaves form to cast");
            me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
            return true;
        }

        Unit* heal = SelectHealTarget(90.0f, 84.0f);
        if (heal)
        {   // cb:fold rotation rung, outcome probed at cast
            if (heal->GetHealthPercent() < 32.0f && TrySpecSpell(me, SP_NATURES_SWIFTNESS_DRUID)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (heal->GetHealthPercent() < 58.0f &&
                (HasAuraFromSpellChain(heal, 139) || HasAuraFromSpellChain(heal, 8936)) &&
                TrySpecSpell(heal, SP_SWIFTMEND)) return true;   // cb:fold rotation rung, outcome probed at cast
            if (HealInjuredTarget(heal)) return true;   // cb:fold rotation rung, outcome probed at cast
        }
        if (trySupportInnervate()) return true;   // cb:fold target selection and cast/defer outcome are probed
        if (m_spells.druid.pRemoveCurse)
            if (Unit* friendUnit = SelectDispelTarget(m_spells.druid.pRemoveCurse))   // cb:fold rotation rung, outcome probed at cast
                if (TrySpecSpell(friendUnit, m_spells.druid.pRemoveCurse)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (!victim) return false;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 45.0f && TrySpecAura(victim, 5570)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.druid.pWrath)) return true;   // cb:fold rotation rung, outcome probed at cast
        return false;
    }

    if (!victim)
        return false;   // cb:fold rotation rung, outcome probed at cast

    if (spec == 0) // Balance
    {   // cb:fold rotation rung, outcome probed at cast
        if (trySupportInnervate()) return true;   // cb:fold target selection and cast/defer outcome are probed
        if (TrySpecAura(me, 24858)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 45.0f && TrySpecAura(victim, 5570)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 35.0f && TrySpecAura(victim, 8921)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(victim, 10.0f, 2) && TrySpecSpell(victim, m_spells.druid.pHurricane)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 35.0f && TrySpecSpell(victim, m_spells.druid.pStarfire)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.druid.pWrath)) return true;   // cb:fold rotation rung, outcome probed at cast
        return false;
    }

    // Feral explicitly follows the persisted active role.  A tank is always a
    // Bear/Dire Bear; melee DPS is always Cat.  No random or attacker-count form
    // flip is allowed.
    if (GetCombatActiveRole() == ROLE_TANK)
    {   // cb:fold rotation rung, outcome probed at cast
        if (me->GetShapeshiftForm() != FORM_BEAR && me->GetShapeshiftForm() != FORM_DIREBEAR)
            return TrySpecSpell(me, m_spells.druid.pBearForm);   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecTaunt(victim)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecInterrupt(victim, {5211})) return true; // Bash   // cb:fold rotation rung, outcome probed at cast
        if (victim->IsCaster() && victim->GetVictim() != me && TrySpecSpell(victim, SP_FERAL_CHARGE)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, SP_FAERIE_FIRE_FERAL)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (!HasAuraFromSpellChain(victim, 99) && CanUseSpecAoE(me, 10.0f, 1) &&
            TrySpecSpell(victim, m_spells.druid.pDemoralizingRoar)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (CanUseSpecAoE(victim, 8.0f, 2) && TrySpecSpell(victim, m_spells.druid.pSwipe)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetHealthPercent() < 35.0f && TrySpecSpell(me, m_spells.druid.pFrenziedRegeneration)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (me->GetPower(POWER_RAGE) > 500 && TrySpecSpell(victim, m_spells.druid.pMaul)) return true;   // cb:fold rotation rung, outcome probed at cast
        return false;
    }

    if (trySupportInnervate()) return true;   // cb:fold target selection and cast/defer outcome are probed
    if (me->GetShapeshiftForm() != FORM_CAT)
        return TrySpecSpell(me, m_spells.druid.pCatForm);   // cb:fold rotation rung, outcome probed at cast

    uint8 combo = me->GetComboTargetGuid() == victim->GetObjectGuid() ? me->GetComboPoints() : 0;
    if (combo >= 4)
    {   // cb:fold rotation rung, outcome probed at cast
        if (victim->GetHealthPercent() > 45.0f && TrySpecAura(victim, 1079)) return true;   // cb:fold rotation rung, outcome probed at cast
        if (TrySpecSpell(victim, m_spells.druid.pFerociousBite)) return true;   // cb:fold rotation rung, outcome probed at cast
    }
    if (TrySpecSpell(victim, SP_FAERIE_FIRE_FERAL)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (victim->GetHealthPercent() > 40.0f && TrySpecAura(victim, 1822)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecSpell(victim, m_spells.druid.pShred)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (TrySpecSpell(victim, m_spells.druid.pClaw)) return true;   // cb:fold rotation rung, outcome probed at cast
    if (me->GetCombatDistance(victim) > 8.0f && TrySpecSpell(me, m_spells.druid.pDash)) return true;   // cb:fold rotation rung, outcome probed at cast
    return false;
}
