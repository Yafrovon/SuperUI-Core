# SUI Wire Protocol — CRPG/RTS Control Extension

Custom opcodes between **MSUIClient** and **SuperUI-Core**, layered on the stock
1.12 (build 5875) world protocol. A vanilla client never emits opcodes above
827, and the custom SMSGs are only sent to sessions that have spoken a
`CMSG_SUI_*` first (`WorldSession::IsSuiCapable`), so stock clients never see
them. All integers little-endian; guids are raw `uint64`, never packed.

Server implementation: `src/game/SuperUiContent/SuiWorld/Shared/SuiWorldState.{h,cpp}`,
`src/game/SuperUiContent/SuiWorld/CRPG/SuiPossess.{h,cpp}`,
`src/game/SuperUiContent/SuiWorld/RTS/SuiFactionControl.{h,cpp}`,
`SuiRts.{h,cpp}`, `SuiHonor.{h,cpp}`, `SuiHero.{h,cpp}`, and
`src/game/SuperUiContent/SuiWorld/Bridge/SuiPortal.{h,cpp}`, packet structs in
`src/game/Server/Packets/SuiControl.h` and `SuiPortalPackets.h`.
Client implementation: MSUIClient `GameLoop/Scene/GameLoop.Control.cs`,
`GameLoop/Scene/GameLoop.CommanderMap.cs`, `Net/RtsWire.cs`, and the
`Net/NetworkClient.cs` / `Net/WorldSession.cs` facade paths.

| Opcode | Value | Direction |
|---|---|---|
| `CMSG_SUI_CONTROL_REQUEST` | 828 (0x033C) | C → S |
| `CMSG_SUI_CONTROL_RELEASE` | 829 (0x033D) | C → S |
| `CMSG_SUI_ORDER`           | 830 (0x033E) | C → S |
| `SMSG_SUI_CONTROL_ROSTER`  | 831 (0x033F) | S → C |
| `SMSG_SUI_CONTROL_ACK`     | 832 (0x0340) | S → C |
| `SMSG_SUI_PROXY`           | 833 (0x0341) | S → C |
| `SMSG_SUI_SNAPSHOT`        | 834 (0x0342) | S → C |
| `CMSG_SUI_CAM`             | 835 (0x0343) | C → S |
| `CMSG_SUI_ZONE_INTEL`      | 836 (0x0344) | C → S |
| `SMSG_SUI_ZONE_INTEL`      | 837 (0x0345) | S → C |
| `CMSG_SUI_RTS_STATE`       | 838 (0x0346) | C → S |
| `SMSG_SUI_RTS_STATE`       | 839 (0x0347) | S → C |
| `CMSG_SUI_RTS_ACTION`      | 840 (0x0348) | C → S |
| `SMSG_SUI_RTS_ACTION_RESULT` | 841 (0x0349) | S → C |
| `CMSG_SUI_FORCE_ROSTER`    | 842 (0x034A) | C → S |
| `SMSG_SUI_FORCE_ROSTER`    | 843 (0x034B) | S → C |

### Real-portal opcode block

| Opcode | Value | Direction |
|---|---|---|
| `CMSG_SUI_PORTAL_PREPARE`  | 844 (0x034C) | C -> S |
| `SMSG_SUI_PORTAL_DESCRIPTOR` | 845 (0x034D) | S -> C |
| `CMSG_SUI_PORTAL_READY`    | 846 (0x034E) | C -> S |
| `SMSG_SUI_PORTAL_STATE`    | 847 (0x034F) | S -> C |

### Party-member-facts opcode block

| Opcode | Value | Direction |
|---|---|---|
| `CMSG_SUI_MEMBER_FACTS`    | 850 (0x0352) | C -> S |
| `SMSG_SUI_MEMBER_SPELLS`   | 851 (0x0353) | S -> C |
| `CMSG_SUI_MEMBER_ITEM_MOVE` | 852 (0x0354) | C -> S |
| `SMSG_SUI_MEMBER_ITEM_MOVE_RESULT` | 853 (0x0355) | S -> C |

### Party-quest-facts opcode block (PLAN_20 P1)

| Opcode | Value | Direction |
|---|---|---|
| `CMSG_SUI_QUEST_FACTS`     | 854 (0x0356) | C -> S |
| `SMSG_SUI_QUEST_LOG`       | 855 (0x0357) | S -> C |
| `CMSG_SUI_PARTY_QUEST`     | 856 (0x0358) | C -> S |
| `SMSG_SUI_PARTY_QUEST_RESULT` | 857 (0x0359) | S -> C |

858-859 stay RESERVED for the PLAN_20 P4 vendor pair, so a half-deployed server
can never renumber them out from under a client. `NUM_MSG_TYPES` is 874 (see
the tactical-freeze section at the end).

Values 835-843 are the camera, zone-intel, and RTS extensions.
The portal values are intentionally fixed above that range so those branches
can merge without renumbering wire traffic. 848/849 are provisionally spoken
for by the dynamic-combat rotation pair (MSUIClient
`docs/plans/DYNAMIC_COMBAT_RULES_AND_ENCOUNTER_INTELLIGENCE.md`), so the
member-facts pair starts at 850.

## CMSG_SUI_CONTROL_REQUEST
Ask to possess a bot. MMO boot keeps the original party/raid-only rule. An RTS
boot with `control.faction_bots=1` additionally authorizes any in-world
same-faction AiBot; this is the only party-membership bypass.

| Field | Type | Notes |
|---|---|---|
| targetGuid | u64 | the bot to possess |

Server validation (deny codes in parentheses): requester is a real session and
self-mover, alive, not on taxi/transport, possessing nothing (6); target exists
and is in world (1); target session is a bot with AiBotAI (2); same group, or
the boot-latched RTS faction-control bypass described above (3); not already
possessed (4); target alive, not on taxi/transport, not teleporting (5).

An authorized RTS faction bot already on the same map+instance must be visible.
For any non-visible outdoor, non-instance target (distant on the same map or on
a different map), the server first retires any source free-camera eye and
accepts an ordinary owner-body `TeleportTo` to the bot's position, then replies
7 (`ACK_RELOCATING`). The client waits for ordinary movement/world and entity
streaming, then retries this same request after the bot is resident. Any
instance target not already same-map, same-instance, and visible replies 8 and
is never entered. The owner body remains at that destination after later release.

On grant, in order: bot AI suspended (`AiBotAI::m_possessed`, bridge commands
gated, brain sees `possessed:1` STATE), bot loot force-released,
`Camera::SetView(bot)`, `SetPossessorGuid`, `SetMover(bot)`,
`SetClientControl(bot, 1)`, then `SMSG_SUI_CONTROL_ACK(OK)`.
**The client must answer the grant with `CMSG_SET_ACTIVE_MOVER(botGuid)`** —
guaranteed to be accepted because the server set the mover first — then stream
`MSG_MOVE_*` from the ACK position.

## CMSG_SUI_CONTROL_RELEASE
| Field | Type | Notes |
|---|---|---|
| mode | u8 | 0 = back to own character, 1 = free view (own char stays autonomous) |

Safe to send when possessing nothing (freecam enter/leave from the own
character); always answered with an ACK. The client should flush a
`MSG_MOVE_STOP` at the bot's position **before** sending this, then park its
movement stream until the ACK.

## CMSG_SUI_ORDER (M6)
| Field | Type | Notes |
|---|---|---|
| orderType | u8 | 0 move, 1 attack, 2 stop |
| subjectCount | u8 | 0 = every controllable bot in the group |
| subjects | u64 × count | |
| targetGuid | u64 | attack orders; 0 otherwise |
| x, y, z | f32 × 3 | move orders |

## SMSG_SUI_CONTROL_ROSTER
Pushed on every group roster change (`Group::SendUpdate`) and on possession
begin/end. Lists **all** current group members.

| Field | Type | Notes |
|---|---|---|
| count | u8 | |
| per member: guid | u64 | |
| per member: flags | u8 | 0x01 controllable (AiBot), 0x02 currently possessed, 0x04 conscripted, 0x08 your companion |
| per member: chain | u8 | row v2 (2026-09-03): 0 linked · 1 unlinked by the human (holds until re-linked) · 2 world hold (landed alone / human hopped far / boss flew off; clears when the anchor is back in range) |
| per member: anchor | u64 | row v2: the body this member's formation keys on (`FindEscortBoss`), 0 for a real player |

Rows are 18 bytes since 2026-09-03 (were 9). Re-pushed on every chain edge
(`SuiPossess::NotifyChainChanged`: hold set/cleared, ORDER_LINK) so the client's
chain UI is server truth. ORDER_LINK with x >= 0.5 also lifts a world hold.

## SMSG_SUI_CONTROL_ACK
Answers requests/releases AND arrives **unsolicited** on forced release — the
client must accept it in any control state.

| Field | Type | Notes |
|---|---|---|
| guid | u64 | granted bot, or own character on release/deny |
| result | u8 | see below |
| x, y, z, o | f32 × 4 | authoritative position of the unit the client now drives |

Results: 0 OK · 1 DENY_NOT_FOUND · 2 DENY_NOT_BOT · 3 DENY_NOT_IN_GROUP ·
4 DENY_BUSY · 5 DENY_TARGET_STATE · 6 DENY_REQUESTER_STATE ·
7 ACK_RELOCATING · 8 DENY_CROSS_INSTANCE ·
16 RELEASED · 17 RELEASED_FREECAM · 18 RELEASED_DEATH · 19 RELEASED_TELEPORT ·
20 RELEASED_GROUP · 21 RELEASED_LOGOUT

On any release result the client teleports its controller to the carried
position (the own character may have moved under its AI). Server-initiated
releases open a 1 s movement drain window on the session so in-flight
bot-coordinate `MSG_MOVE_*` are discarded, not misattributed.

The fixed ACK prefix remains 25 bytes. Current cores append an optional
eight-byte capability trailer: `u32 0x31495553` (`SUI1` on the little-endian
wire), then a `u32` capability mask. Bit 0 advertises REAL_PORTALS version 1;
bit 1 advertises the server-authored cast-prewarm catalog described below;
bit 2 advertises FACTION_CONTROL_GROUPS version 1 (census + non-party
possession + RTS orders); bit 3 advertises PARTY_MEMBER_FACTS version 1
(party/raid AiBot bags + known spells without possession — the client must
not send `CMSG_SUI_MEMBER_FACTS` until bit 3 is observed); bit 4 advertises
PARTY_ITEM_MOVE version 1 (instant bag-item moves between party members —
the client must not send `CMSG_SUI_MEMBER_ITEM_MOVE` until bit 4 is
observed); bit 5 advertises PARTY_QUEST_FACTS version 1 (party/raid quest logs
without possession, and the requester's own quests held past the twenty
update-field slots — the client must not send `CMSG_SUI_QUEST_FACTS` until bit
5 is observed); bit 6 advertises PARTY_QUEST_ACTS version 1 (accept / turn in /
abandon on behalf of party members, and the id-addressed abandon -- the client
must not send `CMSG_SUI_PARTY_QUEST` until bit 6 is observed).
Older clients may ignore the suffix and any catalog bytes after it. A current
client probes safely with a zero-guid `CMSG_SUI_CONTROL_REQUEST`: an older core
returns its ordinary denial without a trailer, while a portal-capable core
advertises bit 0. The client must not send portal opcodes until that bit is
observed.

When bit 1 is present, the mask is followed by `u8 catalogVersion = 1`, `u8
rowCount = 6`, `u16 rowBytes = 32`, then six fixed-stride rows. Each row is
`u32 summonSpellId`, `u32 portalEntry`, `u32 teleportSpellId`, `u32 previewMapId`,
and `f32 previewX, previewY, previewZ, previewOrientation`. The rows cover only
Mage portal summon spells 10059 and 11416-11420. Destinations are resolved from
the live server `spell_target_position` table; the client never hard-codes city
coordinates. A new client may begin cancellable destination-only work when its
own `SMSG_SPELL_START` matches a row. This hint has no GameObject GUID, ticket,
lease, use permission, or teleport authority: visual publication and READY still
require the later ordinary portal descriptor for the exact spawned object.

## SMSG_SUI_PROXY (M3)
Owner-only packets of the possessed bot, re-wrapped into the possessor's
session (the bot's session is socket-less; its `SendPacket` also forwards a
whitelist here — never *instead of* the bot AI's own `OnPacketReceived`).

| Field | Type | Notes |
|---|---|---|
| sourceGuid | u64 | the bot the inner packet describes |
| innerOpcode | u16 | e.g. SMSG_ACTION_BUTTONS, SMSG_INITIAL_SPELLS, SMSG_SPELL_COOLDOWN, SMSG_CAST_RESULT |
| innerBody | rest | verbatim inner packet body |

The client routes the inner body through its normal parser into the per-guid
store and **drops proxies whose sourceGuid ≠ its current controlled guid**
(possession-boundary stragglers).

Mirrored families (the `MirrorOwnerPacket` whitelist), each paired with the
handler family that runs as `GetSuiActor()`:

| Family | Inner opcodes |
|---|---|
| spells / bars / cooldowns | SMSG_ACTION_BUTTONS, SMSG_INITIAL_SPELLS, SMSG_LEARNED_SPELL, SMSG_SUPERCEDED_SPELL, SMSG_REMOVED_SPELL, SMSG_SPELL_COOLDOWN, SMSG_COOLDOWN_EVENT, SMSG_CLEAR_COOLDOWN, SMSG_CAST_RESULT |
| quests / gossip | SMSG_GOSSIP_MESSAGE, SMSG_GOSSIP_COMPLETE, SMSG_QUESTGIVER_* |
| vendor / trainer | SMSG_LIST_INVENTORY, SMSG_SELL_ITEM, SMSG_BUY_ITEM, SMSG_BUY_FAILED, SMSG_TRAINER_LIST, SMSG_TRAINER_BUY_SUCCEEDED, SMSG_TRAINER_BUY_FAILED |
| trade | SMSG_TRADE_STATUS, SMSG_TRADE_STATUS_EXTENDED |
| mail | SMSG_RECEIVED_MAIL, MSG_QUERY_NEXT_MAIL_TIME |
| loot (2026-09-03) | SMSG_LOOT_RESPONSE, SMSG_LOOT_RELEASE_RESPONSE, SMSG_LOOT_REMOVED, SMSG_LOOT_CLEAR_MONEY, SMSG_LOOT_MONEY_NOTIFY, SMSG_ITEM_PUSH_RESULT |
| pet (2026-09-03) | SMSG_PET_SPELLS, SMSG_PET_MODE, SMSG_PET_ACTION_FEEDBACK, SMSG_PET_CAST_FAILED |
| taxi (2026-09-03) | SMSG_ACTIVATETAXIREPLY, SMSG_SHOWTAXINODES, SMSG_TAXINODE_STATUS, SMSG_NEW_TAXI_PATH (the last three reach the bot's session through a gossip "I need a ride") |
| gossip-answered (2026-09-03) | SMSG_SHOW_BANK, MSG_LIST_STABLED_PETS, MSG_TALENT_WIPE_CONFIRM, SMSG_BINDER_CONFIRM, SMSG_PLAYERBOUND, MSG_AUCTION_HELLO — Player::OnGossipSelect runs as the bot and answers these on its session; stable master, innkeeper bind and talent wipe handlers run as GetSuiActor() too. SMSG_BINDPOINTUPDATE is not mirrored (the client keeps one hearth). |

Loot notes: `Player::SendNewItem`'s group broadcast skips the bot's possessor,
so the mirrored copy is the commander's only SMSG_ITEM_PUSH_RESULT. Group loot
rolls are NOT mirrored: the driven bot's AI keeps auto-passing its own roll and
the commander rolls with its own character's copy. A possessed bot's loot
window is force-released on grant AND on release. The driven body's pet bar
(SMSG_PET_SPELLS) is pushed after the grant snapshot; the own character's pet
bar is re-sent after the release ack.

## SMSG_SUI_SNAPSHOT (M4)
Read-only bags/talents for the possessed bot, pushed once after a grant.
Layout finalized in M4. Item rows are `u8 bag, u8 slot, u64 guid, u32 entry,
u32 stack, u8 bagSlots`; bag 255 = character-held with the contiguous
PLAYER_FIELD_INV_SLOT numbering (equipment 0-18, bag slots 19-22, backpack
23-38, **bank items 39-62 and bank bags 63-68 since 2026-09-03**, keyring 81+);
any other bag value is the slot of the equipped/bank bag the row sits in. Bag
rows precede their contents. Under PARTY_MEMBER_FACTS (capability bit 3) the same
byte-identical packet is also pushed for every party/raid AiBot member — no
possession required — on every roster edge and in answer to
`CMSG_SUI_MEMBER_FACTS` (see below).

## CMSG_SUI_MEMBER_FACTS / SMSG_SUI_MEMBER_SPELLS (party member facts)

Owner rule: **party = full facts, faction = orders.** Party/raid AiBot
members' bags and known spells are available to the party's real SUI clients
without possession; non-party faction bots stay command-only (faction-control
authority is deliberately NOT sufficient). Gated behind capability bit 3.

`CMSG_SUI_MEMBER_FACTS` (exact length required: 2 + 8×count):

| Field | Type | Notes |
|---|---|---|
| flags | u8 | reserved, 0 |
| count | u8 | 0 = every AiBot in the requester's group |
| subjects | u64 × count | raw guids |

Authorization per subject: requester is a real player session; the subject is
an AiBot (`AiBotAI` attached, bot session) in the SAME group/raid as the
requester. Each authorized subject is answered with its `SMSG_SUI_SNAPSHOT`
plus one `SMSG_SUI_MEMBER_SPELLS`. Pulls are rate-limited per session (1/s),
independent of movement. The server also pushes both packets unsolicited for
every party AiBot on every roster edge (`SuiPossess::BroadcastRoster`).

`SMSG_SUI_MEMBER_SPELLS`:

| Field | Type | Notes |
|---|---|---|
| guid | u64 | the member |
| count | u16 | |
| spellIds | u32 × count | active spellbook, same filter as `SMSG_INITIAL_SPELLS` |

Cooldowns and other live facts remain possession-only (the M3 proxy wire);
inventory dirty-hooks are deferred — clients stamp snapshot age and re-pull
when a panel opens.

## CMSG_SUI_QUEST_FACTS / SMSG_SUI_QUEST_LOG (party quest facts, PLAN_20 P1)

Owner decision 2026-08-25: **real per-character quest logs, merged in the
client's view.** The member-facts law extends from bags and spells to quest
logs. Gated behind capability bit 5.

`CMSG_SUI_QUEST_FACTS` (exact length required: 2 + 8*count):

| Field | Type | Notes |
|---|---|---|
| flags | u8 | `0x01` requests absolute Unix quest deadlines in the response entries |
| count | u8 | 0 = the whole group AND the requester's own character |
| subjects | u64 x count | raw guids |

Authorization per subject: the requester's OWN character always qualifies —
that is the only way a client can learn about quests it holds without an
update-field slot — and everyone else must clear the same party-line predicate
the bag/spell facts use (`IsMemberFactsSubject`: an AiBot in the same
group/raid). Faction-control authority is deliberately NOT sufficient. Pulls
are rate-limited per session (1/s) INDEPENDENTLY of the member-facts pull, so a
quest panel and a bag panel cannot starve each other. The server also pushes
unsolicited on every roster edge (`SuiPossess::BroadcastRoster`).

`SMSG_SUI_QUEST_LOG` (13-byte header + 19- or 23-byte fixed-stride entries):

| Field | Type | Notes |
|---|---|---|
| subject | u64 | whose log this is |
| flags | u8 | `0x01` = each entry includes the optional deadline field |
| heldCap | u16 | server `Quests.MaxHeld` -- how many quests this character may hold; 0 = not stated |
| count | u16 | entries following |
| questId | u32 | per entry |
| status | u8 | vanilla QUEST_STATUS_* (1 COMPLETE, 3 INCOMPLETE, 5 FAILED) |
| entryFlags | u8 | 0x01 complete, 0x02 failed, 0x04 held without a log slot |
| slot | u8 | update-field log slot, or 255 when held without one |
| objectives | u8 x 4 | `m_creatureOrGOcount`, clamped to a byte |
| items | u16 x 4 | `m_itemcount`, clamped to a u16 |
| deadline | u32 | only with flag `0x01`; absolute Unix seconds, 0 = untimed |

Timer extension `0x01` is request-negotiated and remembered for the session, so
legacy clients that send flags `0` continue receiving the original 19-byte entry.
The absolute deadline matches the player's `QUEST_TIME_OFFSET` update field;
clients subtract their synchronized server time locally, so countdowns keep
moving between quest-facts polls.

Entries are gated exactly as the bridge `questBlob` is: **`m_rewarded` is
skipped**, because VMaNGOS leaves a turned-in quest at `QUEST_STATUS_COMPLETE`
forever and only that bit means "done". FAILED is included here (the blob drops
it) so the client can show a failed quest honestly rather than as absent.

The counters are the SERVER-side truth, never the packed update-field mirror.
Two reasons, both load-bearing: a party member's quest-log fields are owner-only
and were never streamed to this client, and vanilla tracks required-ITEM
progress by counting the player's own bags — structurally unavailable for
anyone else. The `slot`/overflow sentinel is on the wire from day one so no
second packet shape is needed when PLAN_20 P2 lifts the 20-quest cap.

**P2 (held-quest cap) is in.** `Quests.MaxHeld` (default 100, floor 20) caps
how many quests a character may HOLD; `MAX_QUEST_LOG_SIZE` stays 20 as the
update-field slot count. Quests past the slots carry `slot = 255` and the
overflow entry flag, and are the reason a client addresses ITSELF in
`CMSG_SUI_QUEST_FACTS` -- its own update fields cannot show them. `heldCap`
lets the client print an honest "n/100" instead of the vanilla "n/20".

## CMSG_SUI_PARTY_QUEST / SMSG_SUI_PARTY_QUEST_RESULT (party quest acts, PLAN_20 P3)

Owner decision 2026-08-25: accept and turn in for the whole party in one
gesture, with the **reward chosen per bot by the player** -- every member's
picker visible at once. Gated behind capability bit 6.

`CMSG_SUI_PARTY_QUEST` (exact length required: 14 + 9*count):

| Field | Type | Notes |
|---|---|---|
| action | u8 | 1 accept, 2 turn-in, 3 abandon |
| questId | u32 | |
| npcGuid | u64 | questgiver; ignored for abandon |
| count | u8 | **there is no whole-party shorthand** |
| subjects | {u64 guid, u8 rewardChoice} x count | 255 = let the server choose |

Unlike `CMSG_SUI_QUEST_FACTS`, an empty subject list is NOT a shorthand for the
whole group and is refused. Reading a party member's log is harmless; acting on
their behalf is not, and who is about to act must always be visible to the
player who ordered it.

Authorization is per subject and reuses the quest-facts party line (own
character, or an AiBot in the same group). Range is the interesting part: the
requester answers to the ordinary `INTERACTION_DISTANCE` because they are the
one talking to the NPC, while companions must be within `QUEST_SHARE_DISTANCE`
(14 yd) **of the requester** -- which is precisely vanilla's own rule for "this
party member is close enough to be shared with". Measuring five companions
against a 5-yard interaction radius on one NPC would make the feature unusable.

Accept mirrors the real handler rather than the bot bridge: `CanTakeQuest` ->
`CanAddQuest` -> `AddQuest(pQuest, giver)` (the real giver object, so quest
accept scripts fire) -> `CompleteQuest` if already complete -> **the quest's
source spell is cast**, which the bridge path omits. Turn-in resolves the
reward per subject (explicit index, or the fleet's own spec-aware
`ChooseQuestReward` when the client sent 255), bounds-checks it against
`QUEST_REWARD_CHOICES_COUNT` because `RewardQuest` indexes unchecked, then
`CanRewardQuest` -> `RewardQuest`. "Auto" for the requester's OWN character is
refused (`NEEDS_CHOICE`): there is no chooser for a real player, and the client
always has a picker for itself.

`SMSG_SUI_PARTY_QUEST_RESULT` (6 + 9*count): u8 action, u32 quest, u8 count,
then {u64 guid, u8 result} per subject. Results: 0 OK, 1 DENIED, 2 REQUIREMENTS,
3 LOG_FULL, 4 NO_QUEST, 5 TOO_FAR, 6 BAD_REWARD, 7 CANNOT_REWARD,
8 ALREADY_HELD, 9 ALREADY_REWARDED, 10 NEEDS_CHOICE, 11 CANNOT_ABANDON. The
vocabulary is fine-grained on purpose: a party act must be able to say WHICH
member was refused and WHY. Every subject whose log actually moved is followed
by a fresh `SMSG_SUI_QUEST_LOG`, rather than making the client re-pull against
its own one-per-second limit.

**Action 3 addressed at your own character is the id-keyed abandon** that
`CMSG_QUESTLOG_REMOVE_QUEST` cannot express, since that opcode names a slot.
It is what reaches a quest held past the twenty update-field slots (P2).

## Shared quests to AiBot companions (PLAN_20 P3)

Ordinary vanilla `CMSG_PUSHQUESTTOPARTY`, not a SuperUI extension. A socket-less
bot session already receives every SMSG -- `WorldSession::SendPacket` diverts
into `GetBot()->ai->OnPacketReceived` -- but nothing in the bot AI answered a
quest offer, so a shared quest died silently and left the sharer waiting.

`AiBotAI::OnPacketReceived` now answers `SMSG_QUESTGIVER_QUEST_DETAILS` when the
giver guid is a PLAYER (a creature giver means the bridge is driving this bot
through a normal questgiver -- leave that alone) and that player is a real,
non-bot member of the same group: accept via a queued
`CMSG_QUESTGIVER_ACCEPT_QUEST`, or answer `MSG_QUEST_PUSH_RESULT` with
DECLINE when it cannot take the quest. The reply is QUEUED, never a direct
handler call: the send is synchronous inside `HandlePushQuestToParty`, which
sets the share info AFTER sending, so accepting inline would kill the sharer's
confirmation and strand the bot at BUSY for every later share.
`SMSG_QUEST_CONFIRM_ACCEPT` gets the same treatment for escort quests.

## CMSG_SUI_MEMBER_ITEM_MOVE / SMSG_SUI_MEMBER_ITEM_MOVE_RESULT (Phase C v1)

The CRPG shared backpack (owner 2026-08-25): a real SUI player moves one bag
item between two party endpoints — its own character or a party AiBot — with
no trade window. Gated behind capability bit 4.

`CMSG_SUI_MEMBER_ITEM_MOVE` (exact length 19):

| Field | Type | Notes |
|---|---|---|
| flags | u8 | reserved, 0 |
| from | u64 | source endpoint guid |
| to | u64 | destination endpoint guid |
| bag | u8 | 255 = character-held (backpack 23-38, keyring 81+), 19-22 = equipped bag |
| slot | u8 | slot within that bag |

Endpoint authorization: each of from/to is the requester's own character OR an
AiBot in the requester's group (the party line; faction authority never
suffices), from != to, same map, neither endpoint mid-trade. Binding does NOT
gate the move (party logistics, not the auction house); conjured items are
refused. Mechanics are the trade-completion sequence: `CanStoreItem` →
`MoveItemFromInventory` → `MoveItemToInventory` (auto-stored at the first free
slot). No distance gate in v1 — deliberate, BG3-style.

`SMSG_SUI_MEMBER_ITEM_MOVE_RESULT` (17 bytes): u8 result, u64 from, u64 to.
Results: 0 OK · 1 DENIED · 2 NO_ITEM · 3 TARGET_FULL · 4 UNAVAILABLE ·
5 REFUSED_ITEM. After an accepted move both endpoints' `SMSG_SUI_SNAPSHOT` (+
member spells) re-push to every real SUI member of the group — clients update
from those pushes, never from optimism.

## Bridge STATE addition
`AiBotAI::BridgeSendState` gained `"possessed": 0|1`. The C# brain must hold
all planners/driver ticks for a possessed bot (mirror of the `pparty`
stand-down). Mutating bridge commands sent anyway are dropped with an
`EVENT { event: "POSSESSED_DROP", data: "<command>" }`.

## GM commands (stock-client testable path)
`.sui possess <botname>` (or with the bot selected) and `.sui release` drive
the same core without any custom SMSGs — control still works because the grant
uses stock `SMSG_CLIENT_CONTROL_UPDATE` + `CMSG_SET_ACTIVE_MOVER`. This makes
the whole server milestone testable from an unmodified 1.12 client.

## CMSG_SUI_CAM

Free-view camera position; the server keeps the streaming eye under it
(heartbeat: every 2 s or >5 yd of rig travel). The trailing ACTIVE byte is the
free view’s on/off signal — optional on the wire, absent reads as “up”.

| Field | Type | Notes |
|---|---|---|
| x, y, z | 3×f32 | rig position, raw map coords |
| active | u8 (optional) | 0 = the free view came down |

## CMSG_SUI_ZONE_INTEL

The commander map’s census request. Client-driven polling: sent on map open and
every ~5 s while it stays open. Marks the session SuiCapable.

| Field | Type | Notes |
|---|---|---|
| flags | u8 (optional) | reserved, send 0 |

## SMSG_SUI_ZONE_INTEL

Answered only to the asker. Two blocks, each with an explicit ROW STRIDE so a
future server can append per-row facts while an older client skips the extra
bytes (`skip(stride - known)`) — the classic optional-trailing idiom cannot
express “every row grew”. Per-PACKET additions still go after the last block.

| Field | Type | Notes |
|---|---|---|
| zoneCount | u16 | sparse: only zones with a nonzero census, all maps |
| zoneRowBytes | u8 | row stride, currently 9 (v1) |
| zoneCount × rows: | | |
|  zoneId | u32 | `Player::GetCachedZoneId` (≤ one zone-tick stale) |
|  bots | u16 | sessions with a `PlayerBotEntry` (`Player::IsBot`) |
|  players | u16 | everyone else — unattended real characters and the asker included |
|  controller | u8 | zone controller: 0 none / 1 alliance / 2 horde, bit 0x80 = contested (always 0 until territory R3) |
| unitCount | u8 | the asker’s own forces: self + in-world group members |
| unitRowBytes | u8 | row stride, currently 29 |
| unitCount × rows: | | |
|  guid | u64 | raw |
|  mapId | u32 | |
|  zoneId | u32 | |
|  x, y, z | 3×f32 | live position (the client cannot see unstreamed members) |
|  flags | u8 | bit0 alive, bit1 bot |

Notes: GM-invisible characters are counted (no filtering in v1). Zone ids the
client has no WorldMapArea entry for (dungeons, raids) appear in the census and
are aggregated by the client as “elsewhere”.

## CMSG_SUI_RTS_STATE / SMSG_SUI_RTS_STATE

Tier-2 RTS worldstate snapshot; answered only to the asker, marks the session
SuiCapable. Client polls on the commander-map cadence (~5 s). In the vanilla
worldstate the reply is mode=0, moduleFlags=0 and all-zero blocks.

Request: `u8 flags` (optional, reserved).

Reply (all blocks stride-versioned — future servers grow rows, old clients skip):

| Field | Type | Notes |
|---|---|---|
| mode | u8 | 0 vanilla, 1 RTS match |
| moduleFlags | u8 | bit0 honor, bit1 heroes, bit2 territory, bit3 dungeons, bit4 faction control |
| factionRowStride | u8 | currently 26; ALWAYS 2 rows (alliance, horde) |
|  honorPool | i64 | faction honor pool (R2 feeds it) |
|  ore, skins, herbs | 3×i32 | standing supply from held zones (R3) |
|  controlledZones | u16 | R3 |
|  heroesFielded, heroSlotCap | 2×u16 | R2/R3 |
| heroCount, heroRowStride(12) | 2×u8 | rows: u64 guid, u8 team, u8 heroLevel, u8 dead, u8 pad |
| dungeonCount, dungeonRowStride(7) | 2×u8 | rows: u32 mapId, u8 controller, u8 liveRunFlags(bit0 A run, bit1 H run), u8 pad |

## CMSG_SUI_RTS_ACTION / SMSG_SUI_RTS_ACTION_RESULT

Request: `u8 action` (1 heroDeclare, 2 heroUpgrade, 3 heroRevive), `u64 subjectGuid`.
Result: `u8 action, u8 result, u64 subjectGuid, i64 poolAfter`.
Result codes: 0 ok, 1 insufficient honor, 2 no free slot, 3 bad subject,
4 unsupported/disabled.

Heroes are unconditionally AiBot-only. `hero.slots_fixed` configures 1..127
slots per faction and defaults to four.
Declare/upgrade costs are selected by the target level (20/40/80/160/320) and
revive costs by the current level (10/20/40/80/160). A dead hero stays in the
AiBot dead path until a successful paid revive, which returns it at the normal
graveyard with full resources. Hero levels use native passive, permanent world
spells 51001..51005. Each has `MOD_SCALE` and `MOD_DAMAGE_PERCENT_DONE` for a
total 120/140/160/180/200 percent; the server validates the spell definition
against the save-bound `scale_percent` and `damage_percent` rule columns before
enabling heroes. No generic `Unit` damage/scale override is involved.

## CMSG_SUI_FORCE_ROSTER / SMSG_SUI_FORCE_ROSTER

RTS-only paged discovery of every in-world same-faction AiBot. An MMO boot, or
an RTS save without `control.faction_bots=1`, returns an empty page. Names are
not duplicated here; the client resolves the full player GUID with the stock
name-query path. The request body is exactly 14 bytes. Reserved request flags
must be zero and requestId must be nonzero. A wrong-size or zero-id request is
dropped without a roster scan; reserved flags with a usable id receive a
terminal empty page.

Request (14 bytes):

| Field | Type | Notes |
|---|---|---|
| flags | u8 | reserved, must be 0 |
| requestId | u32 | nonzero client generation; echoed |
| zoneId | u32 | 0 = every zone, otherwise exact cached zone |
| afterGuidLow | u32 | exclusive cursor; 0 starts a scan |
| limit | u8 | 0 means 200; otherwise clamped to 1..200 |

Reply header (16 bytes), followed by `count` fixed rows:

| Field | Type | Notes |
|---|---|---|
| requestId | u32 | echoed |
| zoneId | u32 | echoed |
| nextGuidLow | u32 | 0=end; otherwise exactly the last emitted GUID low |
| total | u16 | saturating size of this live scan |
| count | u8 | rows in this page, at most 200 |
| rowStride | u8 | 32 |

Rows are sorted by player GUID low and are exactly:
`u64 fullPlayerGuid, u32 mapId, u32 zoneId, f32 x, f32 y, f32 z, u8 race,
u8 class, u8 level, u8 flags`. Row flags are 0x01 alive, 0x02 busy/possessed,
0x04 control-eligible-now, 0x08 same map+instance, 0x10 hero, 0x20 hero dead
or pending, 0x40 instanceable, and 0x80 reserved.

Pagination is a sequence of live scans, not a snapshot: `total` may change
between pages. Clients must not reject a generation solely because it changes.

## RTS ruleset key list (shared with the SuperUI web app registry)

Scalars in `characters.superui_worldstate` (key/value), read ONCE at boot by
`SuiRts::LoadRuleset()`. Absent key = default. This list is the single source
both the core and the web app's RtsRulesetRegistry mirror.

| Key | Default | Phase | Meaning |
|---|---|---|---|
| mode | (absent = vanilla) | — | `rts` flips the worldstate |
| honor.enabled | 0 | R2 | explicit Honor module gate |
| hero.enabled | 0 | R2 | explicit hero gate; fails closed unless Honor is enabled and all five rules/spells validate |
| control.faction_bots | 0 | R2 | faction-wide roster/control gate |
| state.flush_ms | 30000 | R2 | write-behind cadence for faction state |
| rate.xp_kill / rate.xp_kill_elite / rate.xp_quest | conf | R2 | sWorld rate overrides |
| rate.drop_money / rate.drop_item_poor..artifact / rate.drop_item_referenced | conf | R2 | sWorld rate overrides |
| bots.cap.alliance / bots.cap.horde | -1 (uncapped) | R2 | per-faction bot population cap |
| honor.weight.player / .bot / .npc / .npc_elite | 10 / 5 / 1 / 3 | R2 | enemy-player, enemy-bot, opposing-faction NPC, and opposing-faction elite kill weights |
| honor.suppress_bot_hk | 1 | R2 | skip vanilla HK recording for bot-vs-bot |
| hero.slots_fixed | 4 | R2 | per-faction hero cap until territory lands; clamped 1..127 so both faction rosters fit the u8 packet count |
| territory.zones_per_hero_slot | 2 | R3 | AoE-housing slot ratio |
| territory.flip_cooldown_ms | 30000 | R3 | debounce hub re-flips |
| dungeon.run_timeout_min | 120 | R4 | stale live-run safety net |

List config tables (rows ship inside the RTS save): `superui_rules_zone`,
`superui_rules_hub`, `superui_rules_hero`, `superui_rules_dungeon`. Runtime
state: `superui_faction`, `superui_heroes`, `superui_zone_control`,
`superui_dungeon_control`. The MangosSuperUI world-profile workflow owns schema,
seed rows, and the five world `spell_template` rows. Core boot is read-only for
configuration: it never creates, alters, seeds, or hot-reloads RTS schema.
## Real portal preparation/readiness (version 1)

This side channel lets each live MSUIClient session preload a destination and
render it only after a complete frame is available. It does not invoke
`GameObject::Use`, authorize a crossing, or replace the stock click handler.
Every client packet first marks the session SUI-capable; no portal SMSG is sent
to a session which has not opted in with a `CMSG_SUI_*`.

All four payloads are fixed-size. Reserved fields and trailing bytes must be
zero/absent. Unknown versions are rejected rather than guessed.

### CMSG_SUI_PORTAL_PREPARE (16 bytes)

| Field | Type | Notes |
|---|---|---|
| version | u8 | 1 |
| reserved | u8 | 0 |
| flags | u16 | 0 in version 1 |
| requestId | u32 | echoed in the descriptor |
| portalGuid | u64 | raw game-object guid |

The server validates that the object is spawned, visible for use, inside the
150-yard preparation radius, has no `GO_FLAG_NO_INTERACT`, passes
`PlayerCanUse`, and passes the spellcaster party-only rule. The authoritative
classifier accepts only type-22 entries 176296, 176497, 176498, 176499, 176500,
and 176501 with their matching `gameobject_template.data0` use spells 17334,
17607, 17608, 17609, 17610, and 17611. Destination coordinates come from
`SpellMgr::GetSpellTargetPosition`; client-side portal-name inference is never
trusted.

The six matching summoned GameObjects are forced into the core's bounded
200-yard large-object visibility tier, so a client can actually discover an
existing portal before the 150-yard preparation boundary. This does not change
the stock click/use distance.

### SMSG_SUI_PORTAL_DESCRIPTOR (92 bytes)

| Field | Type | Notes |
|---|---|---|
| version, result | u8, u8 | version 1; result below |
| flags | u16 | bit 0 ONE_WAY, bit 1 PARTY_ONLY, bit 2 CLICK_FALLBACK, bit 3 SAME_MAP_HINT; bit 4 BIDIRECTIONAL is reserved |
| requestId | u32 | correlation with PREPARE |
| portalGuid | u64 | raw game-object guid |
| generation, revision | u32, u32 | per-session generation; descriptor revision |
| ticket | u64 | readiness correlation only; not teleport authority |
| entry, teleportSpellId | u32, u32 | authoritative classifier inputs |
| remainingLifetimeMs | u32 | `0xFFFFFFFF` means no known expiry |
| sourceX, sourceY, sourceZ, sourceYaw | f32 x 4 | source frame center and facing |
| halfWidth, halfHeight | f32, f32 | 3.0 and 4.0: a 6 x 8 yard aperture |
| crossingEpsilon | f32 | 0.35 yard geometry tolerance |
| destinationMapId | u32 | preview map |
| destinationX, destinationY, destinationZ, destinationO | f32 x 4 | preview pose |

Descriptor results: 0 OK, 1 Denied, 2 Unsupported, 3 Expired, 4 Failed.
Denied/error replies keep the same 92-byte layout and zero fields which are not
valid, so parsing never depends on a result-specific packet length.

### CMSG_SUI_PORTAL_READY (28 bytes)

| Field | Type | Notes |
|---|---|---|
| version, loadResult | u8, u8 | version 1; 0 Ready, 1 Failed |
| reserved | u16 | 0 |
| portalGuid | u64 | descriptor key |
| generation, revision | u32, u32 | descriptor key |
| ticket | u64 | descriptor key |

The server rejects stale/mismatched keys, expired preparation leases, despawned
objects, range changes, and lost use/party eligibility. It re-runs validation;
a client assertion is never treated as proof.

### SMSG_SUI_PORTAL_STATE (32 bytes)

| Field | Type | Notes |
|---|---|---|
| version, state, reason, reserved | u8 x 4 | reserved is 0 |
| portalGuid | u64 | correlated object |
| generation, revision | u32, u32 | correlated descriptor |
| ticket | u64 | correlated descriptor |
| leaseMs | u32 | nonzero only when Ready |

States: 0 Ready, 1 Revoked, 2 Blocked, 3 Entering, 4 Expired, 5 Failed.
Version 1 grants a 5-second Ready lease. A portal that remains open is renewed
by another PREPARE/DESCRIPTOR/READY cycle; the generation/ticket may be reused
while the previous correlation is still live. This lease still does not bypass
the stock `CMSG_GAMEOBJ_USE` range and eligibility checks.

## Companions — CMSG_SUI_COMPANION / SMSG_SUI_COMPANION (owner decision 2026-09-02)

| Opcode | Value | Direction |
|---|---|---|
| `CMSG_SUI_COMPANION` | 866 (0x0362) | C → S |
| `SMSG_SUI_COMPANION` | 867 (0x0363) | S → C |

`NUM_MSG_TYPES` is 874 (see the tactical-freeze section below). Advertised by capability bit 7 (`COMPANIONS_V1`) in the
`SUI1` trailer; the client must not send `CMSG_SUI_COMPANION` until it has seen it.

A **companion** is one of the requester's OWN characters (same account, verified
server-side from the character cache — never from anything the client claims)
logged in on a socket-less AiBot session for the length of the owner's session.
The session keeps the **real account id**; only the World session-map key is
synthetic (`WorldSession::GetSessionKey`), so the owner's live session and the
companion coexist and `SaveToDB` never re-stamps `characters.account`. It is the
single owner-verified exception to the real-account wall in
`AiBotAI::OnSessionLoaded`; it never opens the brain bridge and is never written
to `characters.playerbot`. Server implementation:
`src/game/SuperUiContent/SuiWorld/CRPG/SuiCompanion.{h,cpp}`.

**Authority law:** a companion counts as a bot ONLY for its owner. To every
other human it is a real player — no possession (`DENY_NOT_BOT`), no orders,
no bag/spell/quest facts, no item moves, no faction control, no force-roster
row. The same closure now applies to an unattended own character: real
sessions other than the actor's own are never commandable
(`SuiCompanion::MayCommand`, the one predicate every site funnels through).

**Follow law (multi-human):** every unit keeps formation on *the driven body of
its human* — the bot that human possesses, else that human's own character.
A bound unit (companion → owner, unattended own character → its own session,
conscript → conscriptor) follows nobody else and HOLDS when its human is
outside the group or drives nothing else. Shared fleet bots keep the group
rules (real leader, else the deterministic split / follow override) and then
follow that human's driven body.

### CMSG_SUI_COMPANION (9 bytes)

| Field | Type | Notes |
|---|---|---|
| action | u8 | 1 summon · 2 dismiss · 3 list |
| guid | u64 | character guid; 0 for list |

Summon requires the requester in the world, alive, outdoors (non-instanceable
map), not on a taxi/transport, not teleporting, not a bot session. Possessing a
bot is allowed: the companion arrives beside the driven body. On its first AI
tick the companion leaves any stale saved group, joins the owner's group
(created with the owner as leader if there is none; a full party converts to a
raid), and teleports beside the owner's driven body. Max 9 companions.

Dismissal (explicit, or the owner's session logging its player out) drops the
companion out of the party, then logs it out with a save. A character login
that finds its own companion still logging out answers
`SMSG_CHARACTER_LOGIN_FAILED` and dismisses it; retry a moment later.

### SMSG_SUI_COMPANION

| Field | Type | Notes |
|---|---|---|
| kind | u8 | 1 result · 2 list |

kind 1 (result): `u8 action`, `u64 guid`, `u8 result` — 0 OK · 1 DENIED (not a
character on your account / unknown) · 2 ALREADY_IN_WORLD · 3 OWNER_STATE ·
4 LIMIT · 5 NOT_A_COMPANION · 6 FAILED.

kind 2 (list): `u8 count`, then per row `u64 guid, u8 race, u8 class, u8 gender,
u8 level, u8 state, cstring name` — state 0 offline/summonable · 1 online as your
companion · 2 loading · 3 the character you are playing · 4 unavailable. Pushed
after every result, when a companion enters the world and again on arrival,
when one leaves, and in answer to action 3.

### Roster flag

`SMSG_SUI_CONTROL_ROSTER` member flags gain `0x08` = this member is YOUR
companion (set only for its owner, who also sees 0x01; other humans see 0).

### GM command (stock-client testable)

`.sui companion summon <name>` · `.sui companion dismiss <name>` ·
`.sui companion list` — same rules and result codes as the wire.

**Group removal = dismissal (owner 2026-09-02):** a companion kicked from the
party, its owner leaving or being kicked, or a disband dismisses the companion
(party drop, logout with save, owner window refreshed, chat notice). Park a
companion with Hold instead. The companion arrival step (leaving a stale saved
group) is exempt.

**Feel pass (owner 2026-09-02):** result code **7 PARTY_FULL** — a full 5-man
party refuses the summon; convert to a raid on purpose first (arrival never
converts). Arrival lands in formation behind the owner's driven body (slot ring,
2.5 yd per ring, same fan as move orders) with the teleport-in visual (spell
7141) and a chat line. Death: the party gets first call on a resurrection
(healer out of combat, druid Rebirth in combat — fleet AI now casts these for any
dead party member); otherwise the ghost waits at its corpse for the graveyard
run time (floored by the reclaim delay) and pops in place at 50% once the party
is out of combat.

## Party flight — CMSG_SUI_PARTY_TAXI / SMSG_SUI_PARTY_TAXI_RESULT (owner decision 2026-09-03)

| Opcode | Value | Direction |
|---|---|---|
| `CMSG_SUI_PARTY_TAXI` | 868 (0x0364) | C → S |
| `SMSG_SUI_PARTY_TAXI_RESULT` | 869 (0x0365) | S → C |

`NUM_MSG_TYPES` is 874. Advertised by capability bit 11 (`PARTY_TAXI_V1`) in the
`SUI1` trailer; the client must not send the request until it has seen it.
Server implementation: `src/game/SuperUiContent/SuiWorld/CRPG/SuiTaxi.{h,cpp}`.

**Two taxi laws (owner 2026-09-03):**

1. **Direct control rides along.** While possessing, the stock taxi handlers
   (`TaxiHandler.cpp`) run as `GetSuiActor()`: the flight master is ranged from
   the driven bot, the map shows ITS discovered nodes, ITS purse pays, IT
   flies, and the human stays in control — the flight spline is the bot's
   public monster-move and the client lets it drive the controller. A flight
   never releases possession. The rest of the party HOLDS (DoPartyFollow: a
   same-map boss on a taxi is no longer chased) and the human may hop to
   another member with Ctrl+Tab / Ctrl+Click; the follow anchor re-points to
   the new driven body on the next tick and the flyer lands under its own AI
   (cargo: the AI runs nothing but the bridge tick while `IsTaxiFlying()`).
   Once landed and far behind, the ordinary catch-up rule brings it back.
2. **Command View flies the party.** From the sky the taxi map's destination
   click becomes `CMSG_SUI_PARTY_TAXI`; the whole party the commander commands
   takes the flight. Nothing flies unless everyone can board or the commander
   confirmed.

### CMSG_SUI_PARTY_TAXI (10 + 4·count bytes)

| Field | Type | Notes |
|---|---|---|
| flags | u8 | bit 0 CONFIRMED: fly the eligible members even if some cannot board |
| flightMaster | u64 | the NPC; must be interactable from the requester's driven body |
| count | u8 | 2..8 |
| nodes | u32 × count | the CMSG_ACTIVATETAXIEXPRESS chain: source first, destination last |

Flight party = the requester's own character + every group member that is a
bot for the requester (`SuiCompanion::MayCommand`; other humans' characters
and companions are never touched). Per member the server applies the gates of
`Player::ActivateTaxiPathTo`: alive / not in combat / not casting / not
teleporting (BUSY), not already flying (IN_FLIGHT), same map (OTHER_MAP),
within the boarding cube of the source node (TOO_FAR), every node of the chain
discovered (UNKNOWN_NODE), first-hop fare after reputation discount
(NO_MONEY). If anyone fails and the request is not CONFIRMED, nobody flies.
Boarding bots are dismounted, their motion cleared and their journey abandoned
first; a member the stock activation still refuses is reported REFUSED (7).

### SMSG_SUI_PARTY_TAXI_RESULT (14 + 9·count bytes)

| Field | Type | Notes |
|---|---|---|
| result | u8 | 0 FLYING (rows = left behind) · 1 CONFIRM_NEEDED (rows = cannot board, nobody flew) · 2 DENIED · 3 NO_PATH |
| flightMaster | u64 | echoed |
| destination | u32 | last node of the chain |
| count | u8 | |
| rows | count × { u64 guid, u8 reason } | reason 1 UNKNOWN_NODE · 2 NO_MONEY · 3 TOO_FAR · 4 BUSY · 5 IN_FLIGHT · 6 OTHER_MAP · 7 REFUSED |

## Command View tactical freeze + action queue (v1, 2026-09-03)

| Opcode | Value | Direction |
|---|---:|---|
| `CMSG_SUI_TACTICAL_FREEZE` | 870 (0x0366) | C → S |
| `SMSG_SUI_TACTICAL_FREEZE` | 871 (0x0367) | S → C |
| `CMSG_SUI_TACTICAL_QUEUE` | 872 (0x0368) | C → S |
| `SMSG_SUI_TACTICAL_QUEUE` | 873 (0x0369) | S → C |

`NUM_MSG_TYPES` is 874. Capability bit 12 (`TACTICAL_FREEZE_V1`) advertises
this feature. All four packet bodies begin with `u8 version = 1`; there is no
implicit version derived from the capability bit, and every CMSG is rejected
unless its byte length exactly matches the layout/counts below.

The lock owner is always the **real socketed requester**. Its fixed center is
sampled once from `WorldSession::GetSuiActor()` — the body currently driven by
that socket — never from the free-camera eye or blindly from the parked main.
Those identities can differ: `ownerGuid` remains the authorization identity,
while member flag bit 3 marks the driven anchor body. The field is map/instance
scoped, has a fixed 100-yard **full 3-D** radius, and latches every alive Unit
that enters until release. Overlapping locks are reference counted; releasing
one never thaws a Unit still held by another.

The exact registered SUI freecam-eye helper is the sole non-gameplay exclusion:
it must continue following `CMSG_SUI_CAM` to stream visibility/grids. The test
is by registered GUID, never creature entry, so ordinary World Trigger Units
remain eligible. The `u16` member count is the only size ceiling; acquisition
fails before freezing anything if the initial set cannot be represented, and
an active lock thaws as a whole rather than silently leaving a partial field.

### CMSG_SUI_TACTICAL_FREEZE (exactly 14 bytes)

| Field | Type | Notes |
|---|---|---|
| version | u8 | exactly 1 |
| requestId | u32 | nonzero client correlation id; zero is reserved for unsolicited SMSGs |
| desiredActive | u8 | 1 acquire · 0 release |
| lockId | u64 | 0 on acquire; exact authoritative id on release |

Acquire is legal only for a real, in-world player with Command View/free view
up and a live, controlled, same-map driven body. One active lock per owner.
Only its owner may release it. A player frozen by somebody else's field may
not acquire or release a lock.

### SMSG_SUI_TACTICAL_FREEZE (exactly 45 + 9·memberCount bytes)

| Field | Type | Notes |
|---|---|---|
| version | u8 | 1 |
| requestId | u32 | request correlation; 0 for unsolicited snapshots |
| result | u8 | enum below |
| active | u8 | 0/1 |
| revision | u32 | monotonic per lock; nonzero for a real lock/tombstone |
| lockId | u64 | authoritative lock id |
| ownerGuid | u64 | real socket owner, not necessarily the anchor |
| centerX, centerY, centerZ | f32 × 3 | fixed acquisition center; zero in tombstone |
| radius | f32 | 100 while active; zero in tombstone |
| memberCount | u16 | 0..65535; this natural wire ceiling is the only field-size limit |
| members | count × { u64 guid, u8 flags } | authoritative latched set |

Member flags: bit 0 frozen/member; bit 1 commandable **by this packet's
recipient**; bit 2 a real human character/session; bit 3 initiating/driven
anchor body. Nonowner recipients always see bit 1 clear. Active snapshots and
release tombstones are sent to every SUI-capable real session on the same
map/instance, including observers outside the radius; a newly arrived observer
gets the current snapshot even when membership did not change. Only receipt of
an SMSG mutates client state; sending a request is never optimistic.

Freeze results: 0 OK · 1 DENIED_SESSION · 2 DENIED_COMMAND_VIEW ·
3 DENIED_STATE · 4 ALREADY_ACTIVE · 5 FROZEN_BY_OTHER · 6 NOT_OWNER ·
7 NOT_FOUND · 8 BAD_PACKET · 9 RELEASED_VIEW · 10 RELEASED_LOGOUT ·
11 RELEASED_MAP_CHANGE · 12 RELEASED_DEATH. Values 15+ are reserved.
Pre-creation SESSION/VIEW/STATE/BAD_PACKET denials contain zero lock fields.
NOT_FOUND echoes the requested lock id but has owner/revision/members zero.
ALREADY_ACTIVE, FROZEN_BY_OTHER and NOT_OWNER return the relevant active
snapshot. A release tombstone keeps nonzero lockId/ownerGuid/revision but has
zero center/radius/members.

Command View exit, owner/anchor logout, any near/far teleport or map change, and
owner/anchor death force a server-authored release. Possession/control handoff
and ordinary `CMSG_SUI_ORDER` are rejected while frozen, so the anchor cannot
change and the typed queue is the only gameplay authoring path.

### CMSG_SUI_TACTICAL_QUEUE (exactly 15 + 37·recordCount bytes)

| Field | Type | Notes |
|---|---|---|
| version | u8 | 1 |
| lockId | u64 | active lock owned by requester |
| requestId | u32 | nonzero correlation id; zero is reserved for unsolicited SMSGs |
| operation | u8 | 0 ENQUEUE · 1 CANCEL · 2 CLEAR |
| recordCount | u8 | 1..40; actor GUIDs unique in one request |
| records | count × 37 bytes | layout below |

Each record is `u64 actorGuid, u32 actionId, u8 actionKind, u64 targetGuid,
f32 x, f32 y, f32 z, u32 spellId`. ENQUEUE requires actionId 0 and kind 1
MOVE, 2 ATTACK, or 3 CAST. MOVE uses finite/valid XYZ and requires targetGuid
and spellId zero. ATTACK requires a full unit target GUID and all XYZ/spellId
zero. CAST requires a known active nonpassive, non-auto-repeat spell; a unit
target uses zero XYZ, while a ground cast uses targetGuid zero and valid XYZ.
CANCEL requires actorGuid + nonzero actionId and every other action field zero.
CLEAR requires actorGuid and every action field/actionId zero.

Authority is checked both when enqueued and again when executed: actor must be
a latched same-map Player, self or same-group party/raid member, and pass
`SuiCompanion::MayCommand`. Another real human's character is frozen read-only
and is never commandable. Each actor has an authoritative FIFO of at most five
pending actions; a raid selection therefore uses up to 40 records in one
request but still adds only one action to each selected actor.

### SMSG_SUI_TACTICAL_QUEUE

Exact size is `31 + Σ(9 + 29·queueCount)` bytes:

| Field | Type | Notes |
|---|---|---|
| version | u8 | 1 |
| lockId | u64 | authoritative lock id |
| revision | u32 | queue revision |
| requestId | u32 | correlation id; 0 for execution updates |
| result | u8 | enum below |
| resultActorGuid | u64 | actor associated with result, else 0 |
| resultActionId | u32 | action associated with result, else 0 |
| actorCount | u8 | number of queue blocks |
| actors | repeated | `u64 actorGuid, u8 queueCount`, then actions |

Each action row is `u32 actionId, u8 actionKind, u64 targetGuid, f32 x, f32 y,
f32 z, u32 spellId` (29 bytes). Queue snapshots are private to the owner; other
frozen humans and map observers receive only freeze snapshots. Results:
0 OK · 1 BAD_PACKET · 2 LOCK_NOT_FOUND · 3 NOT_OWNER · 4 LOCK_NOT_ACTIVE ·
5 ACTOR_NOT_MEMBER · 6 ACTOR_NOT_COMMANDABLE · 7 ACTOR_UNAVAILABLE · 8 FULL ·
9 ACTION_INVALID · 10 ACTION_NOT_FOUND · 11 ACTION_STARTED ·
12 ACTION_COMPLETED · 13 ACTION_SKIPPED_INVALID · 14 DRAINED.
BAD_PACKET/LOCK_NOT_FOUND are the only stateless revision-0 queue denials;
NOT_OWNER for an existing lock echoes its nonzero current revision without
disclosing actor queues.

Thaw starts each actor's FIFO in order. Move reuses the AiBot validated RTS
move path and waits for arrival; Attack establishes the validated target and
then advances; Cast uses a real typed spell cast and waits until the non-melee
cast ends. Autonomous AI is held for the drain and its prior manual flag is
restored afterward. Its prior RTS-hold discipline is saved and restored with
the manual flag, so a tactical Move/Attack/Cast never leaves an autonomous bot
permanently idle after `DRAINED`. Authority/availability is revalidated for every action;
an invalid action is popped with result 13 and the next action continues.
The owner may acquire a later freeze while an older plan drains. Queues for a
shared actor serialize by increasing lock id and then FIFO action id; queues
for unrelated actors may progress concurrently. A later active field naturally
pauses an older plan through the same overlap refcount. Live gameplay/control
mutations remain fenced while any owned plan drains, and manual AI hold is
reasserted every map tick until the last queued action completes.

The fence is also target-local. A session outside another owner's field cannot
possess or issue an ordinary SUI order to a frozen bot, dismiss or mutate a
frozen companion, board it on party taxi, alter it through party quest/item/lead
or RTS operations, or change a frozen pet/charmed Unit's action, spell, stance,
autocast, attack or ownership state. Multi-subject ordinary orders reject
atomically if any named subject is frozen; they never seed post-thaw live AI
intent behind the authoritative queue. Ordinary melee, spell, item-use and pet
commands also reject a frozen explicit target at ingress; only an explicit
delayed hit launched before membership was latched may enter the deferral path.

Physical interaction targets are sealed as well as actors. An unfrozen session
cannot open or mutate gossip, vendor, banker, auctioneer, flight-master,
trainer/talent-reset, tabard, stable, quest, spirit-healer, binder, repair,
loot or trade services through a frozen Unit (or a frozen player's corpse).
Follow-up packets revalidate stored banker, loot-source and trade-partner
identity; a newly frozen stored loot/trade source is closed/canceled instead of
being mutated. Loot master assignment/give and party/raid membership, role and
target-icon changes reject frozen named members, because those identities feed
commandability. Duel, summon and resurrection accepts likewise reject a frozen
counterparty. Pure status/template/name/roster queries and decline, cancel,
release or close cleanup remain live; ordinary guild/social metadata remains
outside this physical-gameplay boundary. Text emote chat still broadcasts, but
neither a frozen source nor a frozen creature target receives the scripted
`ReceiveEmote` gameplay callback.

World-participation intent is fenced with the same rule: battleground and
meeting-stone queue join/leave/port operations, spirit-healer enrollment,
instance reset, PvP/at-war toggles and player-level dot commands do not execute
while the requester is frozen or owns a draining plan. Group queue joins
preflight every online member before changing any queue state. Battlemaster and
area-healer physical targets are target-fenced; status/list/position queries
stay live. Dot commands are rechecked after their asynchronous world-thread
handoff so a command parsed just before latching cannot race the lock.

### Clock and effect boundary semantics

The map, sessions, visibility, chat, and packet pump never pause. A latched
Unit's actor update (including AI, motion generators/spline time, attacks,
auras, events, summon/pet/totem lifetimes, regen and quest timers) does not
advance. Current motion/animation/cast state is not cleared or replaced.
Absolute GCD, spell/category cooldown and school-lockout deadlines are shifted
by the first-freeze-to-final-thaw duration. Movement/cast/attack/item/pet input
is ignored while the session's real/currently-driven body is frozen or an
owned plan is draining. The same central fence covers server selection and
pre-open mutation panels (loot, gossip/quest, vendor/trainer/bank, stable,
mail, auction and trade), while cleanup closes and genuinely read-only
facts/roster/state/chat/camera traffic remain live. Freeze acquire/release and
the typed tactical queue are deliberately outside this fence.
An already-open trade is canceled when either participant is first latched;
the accept/commit path also checks the counterparty fence, so an acceptance
sent before freeze can never commit inventory or money afterward.
Map packet ordering is movement → tactical entrant latch → spells/general
gameplay, so crossing the fixed sphere and casting cannot occur in one ingress
window before the server notices membership. Synchronous spline/MotionMaster
movement is also checked immediately after the position step; a newly latched
Player/Creature returns before derived AI or later actor clocks run in that
same tick. Threaded continent motion skips Units latched after scheduling, and
the existing post-cell map scan latches entries made by the async step before
the next player/actor update opportunity.

The v1 sealed boundary suppresses new immediate/proc/area damage, healing,
energize and aura-hit work whenever source or target is frozen. Persistent
dynamic objects retain their own duration/pulse clocks while their Unit owner
is frozen and skip frozen targets. An already-launched explicit delayed unit
hit is retained in its Spell target record and retried every 50 ms after thaw.
Arbitrary immediate/proc Spell objects are **not** retained and replayed: those
effects are suppressed. This is actor/effect suspension, not a claim that the
entire projectile/world simulation is paused.


## Threat meter — CMSG_SUI_THREAT / SMSG_SUI_THREAT (v1, owner 2026-09-22)

The 1.12 protocol never tells a client anyone's threat. The client's Threat Meter add-on pulls
the top of one creature's threat list about twice a second. Opcodes 876 / 877 (0x036C / 0x036D);
`NUM_MSG_TYPES` is 878. Capability bit 14 (`THREAT_METER_V1`) advertises the pair, and the
client sends nothing before it sees that bit.

Request, exactly 10 bytes; any other length, version or row count is dropped:

| Field | Type | Meaning |
|---|---|---|
| version | u8 | 1 |
| rows | u8 | 1..10, how many of the top holders to return |
| creature | u64 | the unit whose threat list is wanted |

Reply, always on the requesting session, never on a bot's socket-less one:

| Field | Type | Meaning |
|---|---|---|
| version | u8 | 1 |
| creature | u64 | echoes the request |
| status | u8 | 0 = list follows; 1 = dead, has no threat list, or more than 200 yd from the driven body (the reply ends here) |
| victim | u64 | the creature's current victim, 0 if none |
| listSize | u16 | entries on the whole (online) threat list |
| rows | u8 | rows that follow, sorted by threat, descending |
| row × rows | u64 guid, f32 threat | raw ThreatManager values |
| hasSelf | u8 | 1 when the driven body (`GetSuiActor`) is on the list but not among the rows |
| selfRank, selfThreat | u16, f32 | present only when hasSelf = 1; rank is 1-based |
