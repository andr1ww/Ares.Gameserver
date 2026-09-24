# Changes in this pull

Everything that was changed or added on top of Ares.Gameserver, what the problem
was and what the fix does. Offsets are RVAs in the 1.08 client
(`release-01.08-10-471230`), same as in the rest of the repo.

The short version: the server can now also run **standalone**, one process that
is both the server and the player, with no Riot services and no internet. On top
of that there are a lot of gameplay fixes (guns, abilities, damage, deaths,
teams, skins, drops), a developer console, and tactical bots that play refereed
rounds on Ascent. The original two-process setup is still there as `listen` mode.

## Inventory changes (the part you asked about)

These are the changes that affect inventory loading, in order of importance.
Details for each are further down.

1. **Rel32Hook at `0x1E8269B` removed** (`AresInventory.cpp`). That address is
   the `E8` opcode of the call, not its operand, and `Rel32Hook` rewrites the
   4 bytes at the address it is given. So it corrupted the call instruction.
   The game's own loadout path also crashed through that hook. Only the
   `AuthCreateAndAddEquippable` hook (`0x1999C60`) is installed now.
2. **Slot null check** (`AuthCreateAndAddItem`). `ItemSlots[SlotIndex]` can be
   null (knife + sidearm agents have no primary slot) and writing through it
   crashed the client. Bounds and null checked now, plus null checks on the
   inventory, class, CDO and owner.
3. **Item ownership** (`AuthCreateAndAddItem`). Items get `MyPawn`,
   `InInventory = Yes` and `AttributeOwner`. Without them a granted gun
   equipped and aimed but never fired, and abilities stayed disabled.
4. **Instigator on spawned items** (`Shared.cpp`, `SpawnActor`). Ability actors
   inherit the item's instigator, and with none, things like Cypher's cage
   failed their ownership checks.
5. **Spawn equip** (`ShooterGameMode.cpp`, `SpawnDefaultPawnFor`). Equips
   through `SetDesiredEquippable(..., Normal, ...)` instead of setting
   `CurrentEquippable` and friends by hand, which skipped the equip state
   machine (no draw animation, often no first person model). The
   `while (!Pawn)` spawn retry that could spin forever is now bounded.
6. **Respawn crash** (`SDK/CoreUObject_classes.hpp`). `OnPlayerSpawned` still
   listed an equippable destroyed with the previous body, and
   `TMulticastInlineDelegate::Process` called into it. Dead listeners are
   skipped now.
7. **Standalone only extras** (`AresInventory.cpp`): mandatory weapon
   attachments (rifle sights/ADS), sniper scope hiding the first person gun,
   and a loadout for possessed ability pawns like Cypher's camera.

1 to 6 apply in every mode, including your listen/dedicated setup.

## Before you merge: things to know

- **Default mode changed.** Settings now come from environment variables (list
  below). With nothing set, the DLL starts the **Range in standalone mode**.
  The old behaviour (dedicated server on Ascent, waiting for a client on 7777)
  is `ARES_MODE=listen` plus `ARES_MAP=/Game/Maps/Ascent/Ascent`.
- **Boot wait.** After injecting, the DLL waits for the named event
  `Local\AresGoEvent` before it touches the engine (touching FName/UObject too
  early crashed inside Riot's startup code). Our launcher sets it once the boot
  settles. Without a launcher it gives up after **120 s** and continues, so
  either set the event from your injector or expect a two minute delay.
- **Listen mode with a second client is untested.** We only ever played
  standalone. Listen mode keeps your behaviour (GIsClient cleared, match waits
  for a connection), and most of the new gameplay fixes only switch on in
  standalone mode, but nobody has connected a real second client to this build.
- **Visual Studio build is untested.** It was built and run with zig's clang
  (`build.py`). The `.vcxproj` has every new file registered and a few MSVC
  specific fixes, but it has never been compiled with MSVC. If MSVC complains
  about something, it is probably a clang vs MSVC difference.
- **Files outside the repo.** Two features read files that are not part of this
  pull. They fail softly without them:
  - Tactical bots load a baked map graph (`assets/maps/<map>/tactical.txt` or
    `logs/maps/tactical-<Map>.txt`, relative to the folder above `ARES_LOG`).
    Without it bots fall back to plain navmesh behaviour and log
    `tactical: no bake for ...`. The in-game `tacbake` command generates one
    (about 8 s on Ascent).
  - The custom "Benjamin" Vandal skin needs its mesh and textures in
    `assets/benjamin/`. Without them `skin benjamin` just reports that it could
    not load.

## Settings (environment variables)

Read once in `Source/AresRuntime.cpp`.

| Variable | Meaning | Default |
| --- | --- | --- |
| `ARES_MAP` | travel URL, may carry `?game=...` | the Range |
| `ARES_PAWN` | pawn class path | Jett (`Wushu_PC_C`) |
| `ARES_LOADOUT` | `;` separated equippable class paths granted on spawn | none |
| `ARES_PORT` | UDP port | 7777 |
| `ARES_MODE` | `standalone` or `listen` | standalone |
| `ARES_START_DELAY` | seconds to let sublevels stream before a standalone match starts | 6 |
| `ARES_LOG` | log file path | `Ares.log` |
| `ARES_CONSOLE` | `1` opens a console and mirrors the log | off |
| `ARES_ENDPOINT_PATCH` | `0` leaves the platform endpoint check alone | on |
| `ARES_ABILITIES` | `0` puts back the original ability activation stub | on |
| `ARES_DIAG` | `1` dumps game mode, state machine, players every 2 s | off |
| `ARES_TRACE_GAMEPLAY` | bounded damage and camera visibility logging | off |

## Building

- **Visual Studio:** `Ares.slnx` / `Ares.vcxproj` as before. All new sources and
  headers are in the project and filters. `_CRT_SECURE_NO_WARNINGS` was added
  because `/sdl` turns the `sscanf` calls into errors.
- **Without Visual Studio:** `python -m pip install ziglang`, then
  `python build.py` (or `python build.py --clean`). Output is `Ares.dll` next to
  the script, objects go to `build/`. Both are in `.gitignore`.
- `tests/` has two small standalone unit tests for the bot logic
  (`BotPolicy.h`, `BotTactics.h`). They are not part of the DLL:
  `python -m ziglang c++ -target x86_64-windows-gnu -std=c++17 -I. tests/bot_tactics_test.cpp -o bot_tactics_test.exe`

---

## Changes to files that were already in the repo

### Launch.cpp (rewritten)

- **Problem:** everything was hardcoded (Ascent, dedicated server), the
  ProcessEvent hook wrote every function name to `Ares_PE.log`, and the
  ability activation entry point (vtable `0x7F8/8`) was always replaced with an
  empty function. In one process that means the server never acts on anything
  the player does: no shooting, no abilities, no weapon switching.
- **Fix:**
  - Map, mode, port, pawn and loadout come from the settings above.
  - Waits for `Local\AresGoEvent` (see above), then installs one ProcessEvent
    hook and runs the whole server setup from the first call that arrives on
    the game thread, which is the only safe place for it.
  - Standalone keeps `GIsClient = true` next to `GIsServer = true` and adds
    `listen` to the travel URL (`UEngine::LoadMap` only calls `UWorld::Listen`
    when `!GIsClient || URL has "listen"`). Clearing GIsClient makes the engine
    a dedicated server, and a dedicated server never renders the world, so a
    single process just showed the boot screen with the match running behind
    it. Listen mode still clears GIsClient like before.
  - Ability activation is left intact by default (`ARES_ABILITIES=0` restores
    the stub).
  - The ProcessEvent detour drives the per-frame ticks of the new modules
    (match flow, screen cleanup, settings, bots and so on), each behind a
    reentrancy guard and a clock throttle. Optional per-module timings
    (`perf on`).
  - Keeps your `CoordinatedHUDModel.HandlePlayerStateListUpdated` suppression.
  - A watchdog reports where the game thread is parked if it stops making
    progress (this is what found the Vanguard wait below).
  - A first chance access violation handler logs the Blueprint calls that were
    in flight on that thread when something crashes (`crash:` lines).

### Source/Modules/Engine/World.cpp

- Port comes from `ARES_PORT`, null check on `CreateNetDriver`, logs whether
  the listen actually worked.
- `GetNetMode` returns `NM_ListenServer` (2) in standalone and still
  `NM_DedicatedServer` (1) in listen mode. Same reason as the GIsClient change.

### Source/Modules/Engine/NetDriver.cpp

- Logging goes through `Ares::Log`. `__int64(x)` casts written as
  `(__int64)(x)` so they also compile with clang. No behaviour change.

### Source/Modules/ShooterGame/ShooterGameMode.cpp

- **ReadyToStartMatch** only returned true once a remote client was connected,
  so a single process sat on the loading screen forever. It still starts on a
  connection, and in standalone it also starts once the streamed sublevels had
  `ARES_START_DELAY` seconds to load. Sublevels are requested once.
- **SpawnDefaultPawnFor**
  - The retry loop was `while (!Pawn)` and retried at `StartSpot` again, so a
    failed spawn could hang the game thread forever. Now 8 tries at other
    player starts, then it logs and returns null.
  - In standalone it spawns an `OwnerExclusivePlayerInfo` for the player first
    (`AuthSetController` is stripped, so `AresController` is set by hand).
    Ability ownership and Cypher's camera return logic look the controller up
    through it.
  - Grants the `ARES_LOADOUT` weapons after the agent's starting equippables and
    spawns holding the gun instead of the knife.
  - The equip used to be done by hand (CurrentEquippable, MyPawn,
    EquippableChange timestamps, `EEquipSpeed::Instant`). That put the inventory
    in the right state but never ran the equippable state machine, so there was
    no draw animation and often no first person model until the next switch.
    Now it just calls `SetDesiredEquippable(..., Normal, ...)` and lets the
    state machine run. Your correction/pending-change block after it is kept.
- **GetDefaultPawnClassForController** uses `ARES_PAWN` and caches the class.
- **AuthGetPhaseRemainingTime** is clamped to at least 0.05 s so it never goes
  negative.
- **AreAllPlayersReadyToStart / AreAllPlayersResponsive** are exec hooked to
  return true. The load-in state waits for every player to report ready, and
  offline nobody ever does, so it burned its full timeout.

### Source/Modules/ShooterGame/AresInventory.cpp / .h

- **Removed the Rel32Hook at `0x1E8269B`.** That address is the `E8` opcode of
  the call, and `Rel32Hook` wants the operand address (one byte after), so it
  patched the wrong bytes. The game's own loadout path also crashed through it.
  `AuthCreateAndAddEquippable` (hooked at `0x1999C60`) covers the spawn flow.
- Null checks on the inventory, class, CDO and owner.
- **Slot bounds / null check.** Not every slot has an entry (knife + sidearm
  agents have no primary slot), and writing through a missing one was a null
  dereference that took the client down.
- **Item ownership.** `Owner` alone is not Ares item ownership, and the
  server side inventory functions are stubs, so granted items had a null
  `MyPawn` and `InInventory` unset. A granted sidearm equipped, aimed, had ammo
  and did nothing on trigger pull, and ability charge bars stayed empty. Now
  sets `MyPawn`, `InInventory = Yes` and `AttributeOwner`.
- **Rifle ADS** (standalone only). Inventory grants skip mandatory attachments, so rifles had no
  sight and ADS filled the screen with the gun. Grants now load the
  equippable data asset's `ForcedAttachments`, resolve each one to its
  component class and add it through `UContentLibrary::AddWeaponAttachment`.
  (Pass the component class, not the data asset class.)
- **Sniper zoom** (standalone only). Operator and Marshal have the scope overlay but leave the
  first person hide policy off, so the gun and arms covered the scope.
  `bHideFirstPersonWhileZoomed` and the zoom hide/show options are set for
  `Gun_Sniper_C` and `LeverSniperRifle_C`.
- **Possessed ability pawns** (standalone only, Cypher's camera and similar) never go through
  SpawnDefaultPawnFor, and `AuthSpawnDefaultInventory` is stripped, so they had
  empty inventories. `TickPossessedPawn` gives them their authored loadout, and
  on possession changes syncs the Ares view target, `PossessedCharacter` and
  the engine view target, and notifies interaction components. This is what
  makes Cypher's camera enter and exit cleanly.
- `RefillAbilities` / infinite abilities for the console (`abilities`,
  `infinite`).

### Source/Modules/ShooterGame/Components/PlayerPurchaseablesComponent.cpp

- Logging goes through `Ares::Log`. No behaviour change.

### Source/Shared.cpp

- `SpawnActor` now also sets `Instigator` (the owner if it is a pawn, else the
  owner's instigator). Items spawned for a pawn had no instigator, and ability
  actors inherit it from the item, so e.g. Cypher's cage failed
  `HasSamePlayerInstigator` (`0x1D5F0A0`) and could never be used.

### Headers/Shared.hpp

- `FFrame::Step` / `StepExplicitProperty` are resolved lazily on first use.
  As static inline initializers they could run before `ImageBase` was set
  (static init order) and end up as garbage pointers.
- `Name## ::Init` in `INIT_MODULE` is not a valid token paste for clang, written
  as `Name ::Init`. Same result on MSVC.
- `__int64(x)` casts written as `(__int64)(x)`.

### UnrealContainers.hpp

- `FMemory` realloc pointer is resolved lazily for the same static init order
  reason. This one really crashed the game with a garbage pointer.
- Two `private:` sections made `public:` (the array/set internals are read by
  the new code) and a template parameter renamed because it shadowed the
  outer one, which clang rejects.

### Headers/Hooking/Hooking.hpp, Source/Hooking/Hooking.cpp

- The hook helpers take the detour as a template type and cast it themselves,
  so plain function pointers can be passed. Clang does not convert function
  pointers to `void*` implicitly. Same behaviour.
- `max`/`min` written as `(std::max)`/`(std::min)` so it works with and without
  the windows.h macros.

### pch.h

- Adds the UMG and AssetRegistry SDK headers (skins are found through the asset
  registry), `AresRuntime.hpp`, `Reentrancy.hpp` and the `AresBootLogLine`
  declaration. `__int64` is defined as `long long` for mingw only.

### SDK

- `Basic.hpp`: `TMulticastInlineDelegate::Process` / `Bind` bodies moved to
  `CoreUObject_classes.hpp` because `UObject` is only forward declared in
  Basic.hpp. `TDelegate`'s "invalid use" member no longer breaks compilation of
  valid signatures. Two `static_cast<UClass*>` changed to `reinterpret_cast`
  (the class is incomplete at that point).
- `CoreUObject_classes.hpp`: `Process` now skips listeners whose object or
  function no longer exists, like the engine's own broadcast. Respawning
  crashed in `SpawnDefaultPawnFor -> OnPlayerSpawned` because a listener was an
  equippable that had been destroyed with the previous body.
- `UMG_functions.cpp`, `ProceduralMeshComponent_functions.cpp`,
  `NavigationSystem_functions.cpp`: include `pch.h` so they can be compiled
  (they are now part of the build).

### Ares.vcxproj / .filters / .gitignore

- New files registered, `_CRT_SECURE_NO_WARNINGS` added, `build/` and
  `Ares.dll` from `build.py` ignored.

---

## New modules

### Core

- **AresRuntime** (`Source/AresRuntime.cpp`, `Headers/AresRuntime.hpp`):
  settings from the environment, timestamped log file, optional console.
- **Reentrancy.hpp**: `FReentrancyGuard`. Everything that ticks from the
  ProcessEvent detour dispatches more Blueprint calls, which come back through
  the same detour. On Ascent, where the client drops to a few FPS while
  streaming, the pawn spawn recursed eight levels deep and none finished.
  Every tick is wrapped in this now.
- **MatchFlow**: the client does not own the round flow, the real server pushes
  state transitions. Offline the game mode's state machine walked into the
  intro state and stopped (`GoToState was called with null NewState`). This
  walks it LoadIn -> Intro -> Setup -> play state. Timed states reject
  `GoToState` and need `GoToStateAndSkipTimedEvents` (that is why load-in used
  to burn ~120 s). `PlayerCanRestart` refuses outside Riot's round flow, so the
  pawn is spawned and possessed directly. Also:
  - "NOT AVAILABLE DURING BUY PHASE": gameplay was never made active. Sets
    `GameInfo.bGameplayActive` and clears the two pending timestamps
    (`LocalGameplayStartTimestamp` / `EndTimestamp`) to -1.
    `AuthSetGameplayEnabled` is a stub.
  - Buy phase used to rank as the play state on bomb maps. `GameStateRound` is
    the play state now.
  - The Range was spawning the player on the bhop course (the mode happily
    hands out side attraction starts). Prefers the `Main` start and skips the
    side attraction ones. Bomb maps are unaffected.
  - Sets `PlayerState.DesiredClass` from the pawn so the HUD shows the right
    agent's icons (it reads the agent from there, not the pawn).
  - Respawns: the respawn manager is stripped, so a dead player kept possessing
    the corpse. A dead body is released and the normal spawn path runs again.
  - Endless rounds: states at or past post-round go back to the round state
    (`phase endless off` turns it off).
- **ScreenCleanup**: clears leftover loading/transition screens and input
  blocks (cinematic mode, ignore input flags). It used to scan the whole
  object array on every ProcessEvent call, which is where most of the old
  ~150 s load time went. Now it is throttled and uses a direct lookup; launch
  to spawn is ~35 s.
- **ConsoleGuard**: the client opens its own console window. A Ctrl+C in it (for
  example copying log text) sends a break and kills the game. Swallows
  CTRL_C / CTRL_BREAK and hides that window.
- **PlatformEndpoint**: the platform config endpoint is validated as a Riot
  domain (`.a.pvp.net`, `https://shared.`), so a local endpoint was dropped and
  the boot failed with VAL 29. The four validation call sites in the endpoint
  getter (`0x1AAA9B8`, `0x1AAAA74`, `0x1AAAB56`, `0x1AAAC15`) are Rel32 hooked
  to accept it. `ARES_ENDPOINT_PATCH=0` turns it off.
- **VanguardBypass**: with the platform layer up, the game thread froze forever
  in a `WaitForSingleObject(INFINITE)` on Vanguard's worker (call returns to
  `0x188EF36`). That one call is patched to return `WAIT_OBJECT_0`.
- **Diagnostics**: `ARES_DIAG=1` state dumps, and the stall report used by the
  watchdog.
- **LocalSettings**: VALORANT keeps game settings in Riot's preferences service.
  This saves the settings manager's values to
  `Saved/Config/Windows/AresOfflineSettings.ini` (checked every 250 ms, atomic
  replace) and restores them through the native setters on launch.

### Developer console

- **DeveloperConsole**: F8 opens a separate command window. Commands are queued
  and run on the game thread, so they are safe at any time. About 90 commands:
  `give`, `equip`, `ammo`, `abilities`, `status`, `states`/`state`, `phase`,
  `spawn`, `teams`, `health`, `skins`/`skin`, `rounds`, the bot commands, and a
  lot of read only inspection tools (`spy` logs every distinct UFunction the
  client dispatches, `trace`, `waiting`, `stripped` lists every function whose
  body is the client's empty stub, `inspect`, `actors`, `cdo`, `bytecode`,
  `fnaddr`/`memdump`, `perf`). It also reads commands from
  `logs/bots/request.txt` so tools can drive a running game.
- **GameplayDiagnostics**: optional camera visibility trace
  (`ARES_TRACE_GAMEPLAY`, which also turns on damage logging in LocalDamage).
- **Freecam**: F6 or `freecam`. Spectator camera with WASD/mouse. The body's
  input is off, which also kills its look binding, so mouse look reads raw
  input (`GetRawInputData` / `GetRawInputBuffer` hooked).
- **MapSurvey**: `mapprobe` / `mapscan` export navigation, spawns, objectives
  and standing clearance for a map.

### Weapons and damage

- **WeaponGuards**: every rifle, SMG, shotgun and sniper crashed the client at
  `0x1E0F5F6`. The caller passes an object straight from a field guarded only
  by a bit test, and offline that object is never built. The callee is hooked
  and returns when `this` is null, which is what the missing null check would
  have done. All guns work now.
- **LocalDamage**: the native projectile calculator (`0x1D16AF0`) computed the
  right damage but the authoritative apply path is stripped. The damage is
  bridged to the Ares health sections (`AuthApplyDamage` on a child section,
  `0x18E65A0`) and `NotifyDamage_Point`. Also covers melee (`0x19AF1A0`). No
  friendly fire when both sides have a valid team. Runs the server half of a
  kill: `AuthOnGotKill`, kill notifications, kill feed through
  `UCoordinatedHUDModel::RebroadcastOnAnyPlayerKilled`, ultimate points, the
  kill banner (guns without a skin get `KillBannerData_Base`) and its tiered
  sound (per killer kill count this round). Targets with shields are still
  skipped.
- **LocalRadialDamage**: explosions hurt nobody. The radial apply (`0x1D17D30`)
  computes the right number and hands it to `TakeDamage`, which never reaches
  the health section. Bridged there, and only if `TakeDamage` did not already
  lower health, so nothing is applied twice.
- **LocalEffects**: replicated effects were rejected ("Attempted to replicate
  effect ... from a client"). The effect entry points take their local path in
  standalone, which brings back reload animations and in-hand ability effects.
  Client simulated effects ask for `Alliance_Any` because the alliance check
  does not resolve offline (this is what made weapon inspect work). Creates the
  missing game state `DecalManager` so bullet holes and knife marks show.
- **LocalDrop**: G / F. Drop and pickup are stubbed server functions. Drop spawns
  the on-ground marker, sets its equippable by hand and raises
  `OnRep_MyEquippable`, and the game does the rest (pose, light, prompt).
  Pickup is the reverse.
- **LocalSkins**: gun skins. The registry files skin data assets under
  `Blueprint`, `GetAssetsByClass` does not work on this cooked registry
  (`GetAssetsByPath` does), and `FName::ToString` in this SDK drops everything
  before the last slash, so paths come from `GetRawString`. A skin level's look
  is a weapon attachment component added with `AddWeaponAttachment` next to
  `ApplySkin`. Switching skins builds a clean gun of the same class and copies
  the ammo, because the old skin's mesh overrides survive otherwise.
- **CustomSkins / BenjaminGeometry**: a custom Vandal skin ("Benjamin") built at
  runtime from our own mesh, textures and sounds, with its own kill banner,
  tracer colours and ADS reticle. Needs `assets/benjamin/`.

### Abilities

- **OfflineAuthority**: a lot of ability Blueprints only do their authoritative
  work behind `IsDedicatedServer`, which is false on a listen server. Rewrites
  only the exact positive "dedicated server returns early" guard shapes to
  `IsServer` at load. Inverted guards and if/else blocks are left alone,
  because `if (IsDedicatedServer) {} else { spawn visuals }` has the same
  bytecode shape and rewriting it broke visuals all over the game. A few graphs
  get named rules after their else blocks were read (Raze's Boom Bot deploy,
  Sova's shock dart damage, the recon bolt's collision, the reflex sight and
  skin mesh graphs). Sage's wall also lives here: her rotation confirm waits
  for a client RPC that cannot arrive from a local pawn, so
  `AuthConfirmRotation` is called while she is in `WaitForConfirmedRotation`
  with a valid placement.
- **LocalAbilityActivate**: nothing bound a key to `EquippableInput_Activate`, so
  abilities like Omen's smoke opened their placement UI and never cast. When the
  ability key is pressed for the equippable that is already current and waiting
  for Activate, Activate is delivered instead of another equip
  (`OnActivationInputStateChanged`, `0x19AF780`).
- **LocalGameObjects**: `ShooterBlueprintLibrary.SpawnGameObject` is stripped
  (`0x12925D0` is `xor eax, eax; ret`), so Killjoy's nanoswarm, Brimstone's sky
  smoke, Viper's cloud and others fired their effects and left nothing. The call
  inside the exec thunk (operand `0x209ED7B`) is Rel32 hooked and spawns the
  actor with owner and instigator.
- **LocalSyncedTimers**: `SyncedTimerComponent.AuthStartTimer` is stripped, so
  Phoenix's curveball hung in the air forever (also Sova's recon bolt, Reyna's
  eye). The thunk (`0x16AC1F0`) is hooked and arms the timer through
  `MulticastSetNetExpireTime`, which is intact.
- **LocalSlowField**: the client has no server half of `CellularPatch` (no ground
  sampler, no overlap bookkeeping), so Sage's slow orb and Viper's ult spawned a
  patch with an empty grid. BeginPlay (`0x1926EE0`) and Tick (`0x194AA10`) are
  hooked: the grid is sampled from the floor and handed to
  `NetMulticastUpdateNodeGrid`, and overlap enter/exit events are raised each
  tick.
- **LocalForceModules**: Raze's satchel launched her twice as far because the
  knockback was applied twice, once by the authority and once by
  `ClientApplyForceModule` meant for the owning client. In one process the
  client copy is dropped (caught in ProcessEvent).
- **LocalInteractions**: F (use) handlers are stripped. The use input
  (`0x1966DE0`) is hooked to run zero duration interactions (Cypher's cage,
  Killjoy's nanoswarm detonate) and pickups. `FindBestUsable` skipped the prompt
  delegates when NetMode is below Client, so its one NetMode call
  (operand `0x18C224A`) is redirected and the "PRESS F" prompt shows.
- **LocalCharacterVisibility**: deployables that are AI pawns (Killjoy's turret
  and alarm bot, Raze's Boom Bot) were hidden by the first person rule because
  their controller lives in the same process. Calls `Set3PMeshVisible(true)` on
  AI characters that are not the local pawn or view target.

### Deaths and teams

- **LocalDeath**: bodies snapped to the ground. `AuthTriggerDeathReaction` is
  stripped, so the death reaction is played from `HandleOnDeath`: picks the
  hit reaction entry for the damage region, plays its montage, fills in
  `DeathAnimMontage` and the related fields.
- **LocalTeams**: nobody offline had a team, and both team setters are stripped.
  `GetTeamComponent` (`0x1CCC550`) is hooked and answers from an assignment
  table that points at the game state's own Red/Blue components. Actors without
  a team walk Owner/Instigator up to eight levels (ability spawned actors like
  Sova's recon ping). `teams`, `spawn ... enemy|ally`.

### Bots and rounds

- **LocalBots**: practice stand-ins (`spawn <agent> [side] [behaviour] [weapon]`)
  and tactical bots. Each stand-in gets a player state from the mode's own
  `PlayerStateClass` so kills count as player kills, and a gun through
  `AuthForceDesiredEquippable`. Tactical bots are three layers: team knowledge
  and strategy (5 Hz, a belief per enemy over the baked graph), per bot task
  choice (engage, hold, clear, peek, trade, plant, defuse, retreat), and per
  frame mechanics (reaction time, aim with a second order controller adapted
  from CSKnow, counter-strafe, burst control). Trigger and reload go through
  native input so spread, recoil and damage are the game's own. Bot footsteps
  were heard across the map because every bot is "locally controlled" on a
  listen server, so `IsLocallyControlled` (vtable `0x6E0`) answers no for a few
  presentation callers only. The player's old body is never destroyed: it is
  hidden after 12 s and parked under the map at a round reset
  (`LocalBots::RetireCharacter`), because destroying it together with its gun
  and ability items crashed the game a few respawns later. Dead characters
  drop their gun as a pickup through LocalDrop.
- **BotPolicy.h / BotTactics.h / TacticalGraph.h**: the pure decision and graph
  code, header only, covered by `tests/`. The aim controller and the
  range based fire mode choice are adapted from CSKnow v0.1.3
  (MIT License, Copyright (c) 2021 David Durst).
- **TacticalMap**: `tacbake` samples standing spots over the navmesh, links them
  by native path queries (rejects climbs the capsule can't make and drops that
  are walled off, adds jump links), and traces eye to eye visibility between
  every pair within 55 m. Loads the bake per map.
- **RoundReferee**: the bomb mode's round functions (`AuthStartRound`,
  `AuthWinRound`, respawn manager) are stubs, so rounds are refereed here:
  sides, spike carrier, plant/defuse/detonation, eliminations, time outs,
  score, half time. `rounds start [allies] [enemies] [attack|defend]`,
  `rounds skirmish`, `rounds status|next|stop`. The game's own HUD timer and
  scoreboard do not follow it.

---

## Known not working

- No main menu (the payload travels straight to a map), no buy menu (use
  `give`), the in-game agent select does not apply (pick the agent at launch).
- Damage on armored targets is skipped.
- Jett's Blade Storm view model sits on the camera, Phoenix's wall does not
  rise, Brimstone's stim beacon does nothing, Reyna can't heal.
- Bots use no abilities and there is no economy.
- Listen mode with a real second client and the MSVC build are untested (see top).
