# Commander raid supplemental Core patches

> **2026-09-19 cleanup:** the incremental `.patch` files this log refers to were removed from this repo; every one
> of them is applied in the SuperUI-Core checkout on 192.168.0.2, whose git history is now the single source of
> truth. What stays here: this log, the canonical copies the QA harness fingerprints (`SuiCommanderRaid.cpp`,
> `RaidQaCheckpoint.cpp`), the two newest units (`SuiRaidPrepare.*`, `SuiRaidSupply.*`) and the quartermaster
> policy `sui-raid-supply.json` (installed template: Core `src/mangosd/sui-raid-supply.json.dist`).


The existing `/home/wowvmangos/vmangos` source checkout already contains these changes. Do not apply them again there. Nico alone builds, installs and restarts Core.

- `commander-raid.patch`: original protocol-v3 encounter system (previously deployed).
- `commander-raid-live-fix.patch`: logout/instance-transfer broadcaster null guard (previously deployed).
- `commander-raid-roster-batch.patch`: one chain/roster publication per affected group; deployed by Nico on 2026-09-06 at 23:59.
- `commander-raid-role-policy.patch`: reusable spec execution under encounter arbitration, heal rank/target handling, Flash of Light, threat holds and ammunition/CC constraints; deployed by Nico on 2026-09-06 at 23:59.

The two new supplemental diffs pass `git apply --reverse --check` against the edited checkout. Files use LF endings. Core source contracts pass 33 checks; they do not compile or execute C++. Client Debug/Release and 98 pure regressions pass. No real Onyxia clear is claimed. See `shared_docs/COMMANDER_RAID_HANDOFF.md` and the final role-competence section in `shared_docs/COMMANDER_RAID_PLAN.md` for recovery, evidence, guide sources and remaining gaps.

`commander-raid-geometry.patch` is now applied to the existing Core SOURCE and awaits Nico's build/install/restart. It moves cast-start observation out of the client cast-bar path so triggered windups are observed, rejects undersized point-hazard radii, and searches closer reachable melee flank positions near walls. The supplemental patch passes reverse-apply checking and the updated 37 source contracts. It has not been compiled or tested live. The corresponding client/data changes pass Debug/Release and 98 raid checks. The best real pull entered air with all forty alive, reached 47%, then wiped to the previously unobserved/undersized Deep Breath. No clear is claimed.


### 2026-09-07 owner build correction: spell radius namespace

Nico's Core build caught an unqualified `GetSpellRadius` call at `SuiCommanderRaid.cpp:453`. Corrected the already-applied source to `Spells::GetSpellRadius(radius)` and updated the source assertion and portable `commander-raid-geometry.patch`. The portable patch reverse-check and all 37 Core source contracts pass. No Core build/install/restart was performed by the agent; Nico can rerun his incremental build. This corrects the reported compiler error but is not a claim that the full Core build has passed.


### 2026-09-07: geometry deployed; generic guidance requires next owner build

Nico deployed the geometry patch including the namespace correction (binary 08:18:31 EDT). The subsequent real attempt reached 43%; no clear occurred. `commander-raid-guidance.patch` is now ALREADY APPLIED to Core source and requires Nico's next build/install/restart. It upgrades the paired raid wire to v4, exports authoritative boss/member state and non-controlling manual hazard advice, and shares a shortest complete-path escape solver with bot execution. Actual warning time is separate from effect lifetime, and late escape remains best effort. No boss-specific executor branch is added.

Reverse-apply verification and 43 Core source contracts pass. Client Debug/Release, 122 raid regressions, 18 gameplay contracts, possession/UI/docs checks and source/DB audit fixtures pass. Core compilation and the new live behavior are unverified. Do not use the new v4 client for live raid Apply/Arm until Nico deploys this source. The exact forty characters were recovered outside and the local QA client was closed normally.


### 2026-09-07: guidance deployed; tank acquisition awaits owner build

Nico deployed v4 guidance at 08:59:37 EDT. Actual Apply/Arm, authoritative life state and main-death observation worked. A cast-start spread data retry reached landing and 38.72% before a failed backup-tank acquisition and wipe. `commander-raid-tank-acquisition.patch` is now ALREADY APPLIED TO SOURCE: tanks use ordinary chase/rotation to establish contact and threat before returning to the hold anchor. No protocol change or boss-specific branch. Reverse-check and 44 source contracts pass; Core compilation/live behavior await Nico's build/install/restart. Current client Debug/Release and 123 raid checks pass. All forty were recovered outside and the local QA client closed.


### 2026-09-07: tank acquisition deployed; cone positioning awaits owner build

Tank acquisition was deployed at 09:48:52 EDT and worked in the next real attempt, which reached 34.43% and wiped. `commander-raid-cone-positioning.patch` is ALREADY APPLIED TO CORE SOURCE. It adds generic loaded-spell cone constraints and shared path search, prevents unsafe station returns, and keeps normal manual guidance non-controlling. Reverse-check and 52 source contracts pass. Client Debug/Release, 129 raid checks, 19 gameplay contracts, 12 Python fixtures plus actual loaded-data integration and possession/UI/docs laws pass. Core compilation and cone behavior require Nico's build/install/restart and the next live test. The wire remains v4, but the older Core rejects the new authored action. No clear is claimed; exact recovered state and limitations are in the latest handoff.


Cone positioning was deployed by Nico at 10:22:28 EDT and passed real Apply/Arm. The next opening exposed an independent opt-in human QA driver contact defect, corrected client-side. No further Core build is required for that driver change. No Onyxia clear yet.


### 2026-09-07: healing range recovery awaits owner build

`commander-raid-healing-range.patch` is ALREADY APPLIED TO CORE SOURCE. It repairs the observed role failure where out-of-range healers remained near staging patients while their assigned tank died: restore living primary-patient range/LOS before opportunistic healing, with a living tank fallback and cone-constrained approach. Reverse-check and 55 source contracts pass. Client Debug/Release, 133 raid checks, 23 gameplay contracts and possession/UI/docs laws pass. Nico must build/install/restart; Core compilation and live rescue behavior are unverified. The subsequent partial air attempt was interrupted by a confirmed same-account login, not a raid clear. Read the latest handoff before live recovery/testing.


### 2026-09-18: ORDER_PREPARE (pre-pull preparation as a gameplay verb) - APPLIED TO THE 192.168.0.2 SOURCE, awaits Nico's build/install/restart

`SuiRaidPrepare.h/.cpp` (canonical copies here; live in `src/game/SuperUiContent/SuiWorld/CRPG/`) plus four one-line hooks:
`SuiPossess.h` adds `ORDER_PREPARE = 15` to the CMSG_SUI_ORDER order types; `SuiPossess.cpp` `HandleOrder` dispatches it
to `SuiRaidPrepare::Begin` and does NOT set the RTS hold or yield an applied raid-plan row for it; `AiBotAIMain.cpp`
`UpdateAI` gives `SuiRaidPrepare::Tick` the tick right after `SuiCommanderRaid::Tick`; `src/game/CMakeLists.txt` lists the
new unit. All three touched units pass `-fsyntax-only` with the project's own compile flags; nothing was built, installed
or restarted.

What the order does, per ordered bot, in order, standing still: (1) buff round - every active spell in its own spellbook
that is positive, aura-only, targets a friend and lasts >= 5 min, highest rank first, cast on each group member the AI's
ordinary buff-target rules say lacks it; (2) elixirs/flasks - each carried consumable whose use is a positive aura-only
spell lasting >= 15 min on a short item category (not the potion class) and whose aura it lacks; (3) one school-absorb
potion (long absorb aura on the potion cooldown class) - the order's `x` carries the expected school mask, and with 0 a
single carried absorb school is drunk while several are left alone; (4) eat - carried food whose use is the seated
regeneration aura with a periodic trigger (the trigger IS the well-fed buff), held seated until that buff lands (20 s
cap); (5) feed a hunter pet below Happy through its own Feed Pet with carried food in its diet. Combat, death,
possession, freeze or teleport interrupts it; 150 s total cap. Every use goes through the ordinary session cast / item
paths (costs, reagents, cooldowns, consumption). No item, spell, class or boss identity in code. When the batch a
commander ordered finishes, the commander gets one chat receipt (counts + who is not well fed + who was interrupted);
`[SUI][raid-prepare]` lines in Server.log carry the per-bot trail.

Client side (same day): "Prepare raid" button in the Rotations & Raid Plan window sends ORDER_PREPARE to every commandable
member; the LIVE DUTY column reads pre-pull readiness ("Fed - 2 elixirs - 3 buffs") from visible auras until the plan is
armed (`CommanderRaidReadinessLaw`). The client wire was returned to **protocol 4** with a schema-1 definition projection
(`CommanderEncounterWire`) so a client built from source talks to the deployed Core again.

### 2026-09-18 (same day): ORDER_SUPPLY - the raid quartermaster - APPLIED TO THE 192.168.0.2 SOURCE, awaits the same build

`SuiRaidSupply.h/.cpp` (canonical copies here) + `ORDER_SUPPLY = 16` in `SuiPossess.h`, a `HandleOrder` case (logistics
orders share Prepare's no-hold/no-yield rule) and a `SuiRaidSupply::Flush` after the subject loop, + the CMake line. Passes
`-fsyntax-only`. Policy file `sui-raid-supply.json` (canonical copy here; installed at `run/etc/sui-raid-supply.json`,
override with `SuiRaidSupply.Policy=<path>` in mangosd.conf) is re-read on every order, so the owner edits it live.

Semantics: the order's `x` is the encounter role the client assigned (1 tank, 2 add tank, 3 healer, 4 melee, 5 ranged,
0 = derive). Buckets: `everyone`; `mana` for members whose power type is mana; `roles.tank|healer|physical|caster`
(ranged/unassigned resolve to physical when a bow, gun or crossbow is equipped, else caster when the power is mana);
`ammo` tops up the member's CURRENT ammunition and only grants the policy arrows/bullets to a shooter with none;
`petFood` grants the first policy food in the hunter pet's diet; `repair` repairs for free. All counts are top-up
targets across all bags (never the checkpoint's backpack-only path); nothing is ever removed. Gates: the commander
must be the group leader; a dead or in-combat member is skipped and named. One chat receipt per order (members, items,
repairs, refusals). `[SUI][raid-supply]` log lines per grant. Client: "Supply raid" button sends one order per role
bucket taken from the plan's assignment, else from the member's learned abilities. Supply, then Prepare, then Apply/Arm.
