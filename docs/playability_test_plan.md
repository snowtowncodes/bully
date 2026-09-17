# No-Launch Playability Qualification Plan

Status: planning artifact only. Creating this document does not launch Bully,
run a probe, or report a new result. Validation owner: the orchestrator.

Use this plan with the [M2 renderer verification plan](m2_test_plan.md) and the
[DXVK evidence ledger](dxvk_evidence_ledger.md). The ledger remains authoritative
for renderer evidence classification.

## 1. Scope and claim separation

Qualify one backend at a time, with the proxy active:

1. **Proxy-native first:** `renderer.backend=native` is the control and the first
   playability target. This is a claim about Bully running through the project
   proxy and the native D3D9 backend.
2. **Deferred M1 visible-runtime gate (`M1-V`):** no DXVK playability run starts
   until a newly authorized proxy-native visible-runtime run passes the gate in
   Section 11. Historical DXVK evidence does not waive this prerequisite.
   `M1-V` is this playability plan's prerequisite built on the M2 visible-output
   gate in [m2_test_plan.md](m2_test_plan.md); it requires a current proxy-native
   pass before gameplay or DXVK qualification proceeds.
3. **DXVK second:** only after `M1-V`, qualify the proxy plus the x86 DXVK
   chainload path as its own backend claim.

Native success does not imply DXVK success. DXVK success does not imply native
success. A native/DXVK save cross-load is a separate compatibility observation,
not evidence for either backend's campaign claim. D3D9On12 is outside this plan.

## 2. Fully playable definition

Backend `B` is **fully playable** only when all of the following are passed for
`B`, with no unresolved blocker:

- The main story reaches its completion state. 100% completion, all side
  content, and optional collectibles are not required.
- Title/menu UI, in-game HUD, subtitles, and required text remain visible and
  usable. Cutscenes and the main-story transitions play and return to gameplay.
- Keyboard, mouse, and controller paths cover menu navigation, movement,
  camera, core actions, pause, accept, and cancel/back as applicable.
- Voice, music, sound effects, subtitle timing, volume/mute behavior, and
  continuity after focus or display changes are usable.
- Saves can be created, exited, restarted, loaded, and overwritten in the
  isolated test corpus without losing the expected story state.
- There is no progression blocker, white or blank game window, crash, or hang.
  A varied pre-Present backbuffer or successful `Present` call does not excuse a
  white selected `main-window` capture.
- The display/lifecycle matrix passes, including the tested window modes,
  focus transitions, and monitor conditions.
- The backend passes a two-hour smoke session and a continuous four-hour
  endurance session. An eight-hour session is the target when an 8-hour claim
  is needed; record the exact duration and never round a shorter run up.

## 3. Preconditions and safety

Before any human-controlled runtime case:

- Obtain explicit approval for this specific game launch and backend. No game
  launch is permitted merely because this plan or a no-launch preflight exists.
- Use an isolated Windows test profile and an isolated copy of the game's save
  set. Do not point the test at, overwrite, or delete primary user saves.
- Preserve the original save/profile files and record path, size, timestamp,
  and SHA-256 before copying or changing anything. Also record the game
  executable, proxy DLL, active INI, existing game-folder DLL/INI/log state,
  Git `HEAD`, and the DXVK DLL hash when DXVK is eventually authorized.
- Keep a known-good copy of every source save outside the active game save
  location. A changed save hash alone is not corruption; the load and observed
  story state must also be checked.
- All menu, gameplay, save, focus, display, and controller actions are manual.
  Do not use automated input injection, macro playback, or synthetic device-loss
  injection.
- Use the active physical console display. A remote, virtual, or contaminated
  desktop is not visual evidence. Do not use `-AllowVirtualDisplay` for a visual
  pass.
- Do not replace an unknown existing `Bully Scholarship Edition/d3d9.dll`.
  Preserve the state and escalate the ownership decision before continuing.
- Do not change display modes, monitor arrangement, DPI, or game files without
  a human-approved, reversible case. Stop when cleanup or restoration is not
  verified.

## 4. Native smoke checklist

Run this checklist first with the proxy-native backend. The operator performs
the actions; the orchestrator records the evidence at the indicated pauses.

1. **Clean boot A.** Confirm no `Bully.exe` is running, start from the isolated
   profile, and boot to the title/menu. Confirm the selected game-window capture
   is `target=main-window`, uncontaminated, and nonblank. Record the effective
   backend, process/window identity, and first `CreateDevice`/`Present` results.
2. **Title and menu.** Navigate the title, options, save/load, and pause/menu
   paths. Check readable UI, selectable subtitles, resolution/window options,
   and return/back behavior. Record the actual settings rather than assuming
   the requested INI was applied.
3. **New game and first playable area.** Start a new game, reach the first
   playable area, move the character, move the camera, perform the first core
   action, and verify the HUD/instructions. Note the first story checkpoint.
4. **First cutscene.** Reach the first available main-story cutscene or scripted
   transition. Verify picture, voice, music/SFX, subtitles, timing, skip/return
   behavior where offered, and return to controllable play.
5. **Input pass.** Manually test keyboard, mouse, and controller separately.
   Exercise menu navigation, movement, camera, core action, pause, accept, and
   cancel/back. Record unavailable hardware as `not tested`, not as a pass.
6. **Audio pass.** Check voice, music, effects, subtitle readability, volume
   changes, mute/unmute, and continuity after losing and regaining focus.
7. **Save, exit, restart, load.** Save at a safe point in corpus slot A, exit
   normally, verify the process is gone, restart cleanly, and load slot A.
   Confirm the same story location/state and that no slot was unexpectedly
   changed. Hash the test save before and after this sequence.
8. **Clean boot B.** Repeat a process-absent launch, load the known-good copy,
   and reach the same playable state. This second boot is required; one launch
   is not a clean-boot qualification.
9. **Collect and review.** Before the operator leaves the game state, record the
   checkpoint and issue status. After exit, collect the existing run artifacts
   listed in Section 10, verify restoration, and reject captures containing a
   terminal or another application.

Any hard stop in Section 6 ends the case. Do not continue the campaign to hide
the failure.

## 5. Save and checkpoint corpus

Maintain a manifest entry for every retained checkpoint with these fields:

| Field | Required value |
| --- | --- |
| `checkpoint_id` | Stable ID, such as `tutorial-01` or `chapter-03-boundary` |
| Story context | Chapter, mission, objective, cutscene, and whether the state is before/after it |
| Creation state | Exact in-game location and the condition that makes the save useful |
| Profile/backend | Isolated profile, effective backend, resolution/window mode, and relevant settings |
| Slot and operation | Slot name/number plus `create`, `load`, `overwrite`, or `restart` |
| Identity | Timestamp, Git `HEAD`, game/proxy hashes, and DXVK hash when applicable |
| Save evidence | File path, size, SHA-256, source copy, and pre/post hashes |
| Result | Load result, observed story state, restart result, overwrite result, and issue ID if any |
| Evidence reference | Existing run root, `report.json`/`summary.txt`, capture or log reference, and operator initials |

Suggested corpus, in order:

- Tutorial start and tutorial completion, including the first playable area and
  first scripted/cutscene transition.
- Every main-story chapter boundary, with a copy before and after any
  irreversible transition where the game permits it.
- Representative missions covering travel, combat/action, scripted objectives,
  indoor/outdoor transitions, and representative cutscenes.
- A late-game checkpoint, a pre-final checkpoint before the irreversible final
  sequence, and the main-story completion state/end screen.

Use at least three test slots when the game exposes them: slot A as the known-good
source, slot B as the overwrite target, and slot C as a restart/recovery copy.
For critical checkpoints, create the source copy before exercising overwrite,
exit, restart, and load. If the game offers fewer slots, record the limitation
and keep the preserved copy outside the active save location. Do not perform
native/DXVK cross-loads until the DXVK backend itself has passed its playability
gate; after that point, test both directions from copies and record the result
separately from the campaign claims.

## 6. Campaign issue log

Every issue gets an ID and records: backend, run root, build/hash identity,
checkpoint/mission, exact manual steps, expected behavior, observed behavior,
first occurrence, repeatability, severity, slot/save hashes before and after,
relevant capture/log/WER references, recovery attempt, and disposition.

Stop and preserve the run root on any of these conditions:

| Area | Stop condition and action |
| --- | --- |
| Rendering | Selected `main-window` is white, blank, low-information, persistently corrupted, or missing required UI/cutscene output. Stop visual qualification; retain PNGs, `report.json`, proxy logs, and backbuffer diagnostics without treating the latter as visible proof. |
| Progression | A main-story objective, trigger, cutscene return, transition, or required interaction does not advance after one controlled retry or a known-good checkpoint reload. Stop before making more saves. |
| Save corruption | A save disappears, fails to load, loads the wrong story state, unexpectedly overwrites another slot, or changes the isolated corpus in a way the operator cannot explain. Stop all further writes, preserve/hash the active files, and compare with the external copy. |
| Crash | `Bully.exe` exits unexpectedly, Windows Error Reporting records a matching application error, or a crash occurs during a repeatable action. Stop and preserve the first failing artifacts. |
| Hang | No visual, input, audio, or state response for 60 seconds outside an expected transition, or a loading/return-to-menu transition exceeds 120 seconds. Do not force a restart until the orchestrator has collected the available evidence. |

One hard stop is a gate failure or hold pending triage; it is not averaged away
by later good checkpoints.

## 7. Display and lifecycle matrix

Run each row manually at the baseline settings, recording actual resolution,
window rectangle, monitor, DPI, focus state, and audio/input continuity. Repeat
the relevant rows for each backend only after its gate is open.

| Case | Human action | Pass evidence |
| --- | --- | --- |
| Windowed baseline | Play, save, and return to menu in the normal windowed mode | Game remains visible and responsive; UI, input, audio, and save state survive |
| Fullscreen baseline | Enter fullscreen at the same supported mode, then return | No white/black window, lost input, missing audio, or progression change; display restores |
| Resolution change | Use one additional resolution offered by the game, then restore baseline | Correct aspect/window bounds and readable UI; no crash or stuck mode |
| Alt-tab/focus | Alt-tab away and back during menu, gameplay, and audio; use a separate visible window only for the manual action | Focus returns; keyboard/mouse/controller, audio, subtitles, and rendering continue |
| Minimize/restore | Minimize and restore in windowed mode | Window returns with the same story state and usable input/audio |
| Multi-monitor/DPI | If a second monitor or non-100% DPI exists, move the window or start on each relevant monitor | No clipped UI, incorrect scaling, lost focus, white output, or input/audio discontinuity |
| Device-lost/reset observation | Record any naturally occurring cooperative-level/reset event; do not inject one | Proxy log/HRESULT evidence shows recovery, or the case is stopped and classified as a failure |

The orchestrator collects display state before launch, after termination, and
after cleanup, plus `TestCooperativeLevel`/`Reset` evidence when the proxy logs
it. The operator decides whether the game actually remained usable.

## 8. Performance and endurance evidence

First establish a proxy-native reference using the same executable, settings,
resolution, route, and scene that will be used for later backend comparisons.
Record exact measured FPS or frame-time statistics, not a visual impression. If
no approved counter is available, record `not measured` and do not claim a
performance pass from PNG metrics.

Initial relative thresholds for a DXVK comparison, fixed before the first DXVK
run, are:

- warm-cache median FPS at least 90% of proxy-native;
- warm-cache 1% low at least 80% of proxy-native; and
- warm-cache p95 frame time no more than 1.25x proxy-native.

These are initial qualification thresholds, not completed results. Absolute FPS
is hardware-dependent. A cold DXVK cache and a warm DXVK cache are separate
cases and are permitted only after `M1-V`; record cache state and the DXVK log
for each. Cold-cache shader compilation stutter is recorded separately, but it
must not become a crash, hang, unrecoverable white window, or save/progression
failure.

For the two-hour smoke, use a repeatable route through menus, gameplay,
cutscene, save/load, and at least one focus/lifecycle transition. For endurance,
play one continuous session for at least four hours, or eight hours when that
claim is required, with manual checkpoints at least every 30-60 minutes. Record
exact duration, restarts, loads, issue IDs, and final save/load state. No backend
may pass endurance if it accumulates visual degradation, input/audio loss,
progression failure, save corruption, crash, or hang.

## 9. Human intervention and automatic collection

| Point | Human must do | Orchestrator may collect automatically |
| --- | --- | --- |
| Before launch | Select isolated profile/save copy, approve the backend and launch, confirm no primary save is targeted, close contaminating windows, and confirm the display case | Git `HEAD`; game/proxy/INI/DXVK hashes; original save/profile hashes; process absence; display/session preflight; requested/effective settings; run root preparation |
| Boot/menu/gameplay | Operate every control, read UI/subtitles, judge rendering/audio, reach missions/cutscenes, and decide when a checkpoint is valid | Window handle/title/rectangle; liveness; selected captures and deterministic metrics; `report.json`; `run.log`; proxy/DXVK logs; process exit and WER records |
| Save/checkpoint | Name the story state, choose slot, verify the loaded state, perform overwrite/restart manually, and stop on corruption | Before/after hashes and sizes of isolated test saves; artifact timestamps; run identity; restoration state. It must not rewrite saves or inject input. |
| Lifecycle/performance | Alt-tab, minimize, change supported display modes, move monitors, operate controller, and observe audio/input/frame pacing | Display snapshots before/after/cleanup; capture records; proxy HRESULT/reset evidence; approved performance measurements; cache/backend identity |
| Stop/after exit | Confirm the issue or pass, sign the corpus/issue entry, and approve any termination needed after evidence collection | Preserve the existing run root; collect `report.json`, `summary.txt`, `run.log`, WER, captures, logs, hashes, and cleanup/restoration results; verify no unexpected primary-file change |

The orchestrator cannot infer story progression, subtitle correctness, controller
feel, audio continuity, or save semantics from renderer telemetry. Those are
human assertions backed by the manifest and issue log.

## 10. Required artifacts

Retain the existing run root and names from the render-probe evidence contract:

```text
dump/render-probe/<timestamp>-pid<pid>-<backend>-se-<swap>_pi-<interval>_od-<i|e>/
  report.json
  summary.txt
  run.log
  wer-application-errors.json
  captures/capture-0005s-printwindow.png              (and other scheduled times)
  captures/capture-0005s-copy-from-screen.png         (and other scheduled times)
  artifacts/bully_d3d9proxy.log
  artifacts/bully_d3d9proxy.full.log             (when present)
  artifacts/bully_d3d9proxy.active.ini
  artifacts/bully_renderprobe_backbuffer.bmp
  artifacts/bully_renderprobe_frontbuffer.bmp   (when present)
  state/requested-bully_d3d9proxy.ini
  state/original-d3d9.dll                        (when one existed)
  state/original-bully_d3d9proxy.ini             (when one existed)
```

`report.json` is authoritative for process, display, capture, hash, WER, and
restoration facts. The selected capture record must identify `target=main-window`
and be uncontaminated. Backbuffer/frontbuffer BMPs, successful API calls, and
logs are diagnostic or backend evidence; none alone proves visible gameplay.

For an authorized DXVK case, also retain the existing
`dump/render-probe/dxvk-<timestamp>/dxvk-manifest.json`, bridge output, captured
`Bully_d3d9.log`, source/staged DLL hashes, and restoration result. The completed
checkpoint manifest, campaign issue log, human pass/fail decisions, performance
measurements, and exact session duration stay with that same evidence package;
they must reference the run root rather than replace the existing artifacts.

## 11. Gate pass/fail table

| Gate | Pass requires | Fail/hold rule |
| --- | --- | --- |
| `P0` safety and identity | Explicit approval, isolated profile/saves, original hashes, known file identities, physical display, no contaminating window, and no primary-save mutation | Any missing approval, unsafe profile, unknown DLL, failed preflight, or unverified restoration is a hold/fail; no launch |
| `M1-V` deferred proxy-native visible runtime | Current proxy-native run has passed preflight; effective native backend and `CreateDevice`/`Present` evidence; at least one uncontaminated nonblank selected `main-window` capture; no white window; report and restoration are complete | Backbuffer/API success without visible output, fallback/contaminated capture, blank/white capture, crash, or cleanup uncertainty fails the gate and blocks DXVK |
| `N-S` proxy-native smoke | All nine smoke steps pass, including two clean boots, first playable area/cutscene, all available input/audio checks, save/exit/restart/load, and required artifacts | Any Section 6 hard stop or missing human evidence is fail/hold |
| `N-C` proxy-native campaign | Corpus covers tutorial, chapter boundaries, representative missions/cutscenes, late game, pre-final, and completion; main story completes; lifecycle matrix, two-hour smoke, and four-hour minimum endurance pass | Any unresolved blocker, save/progression issue, lifecycle failure, crash/hang, or missing duration/corpus evidence fails |
| `D-V` DXVK visible runtime | `M1-V` already passed; current DXVK run independently proves effective DXVK/Vulkan path plus an uncontaminated nonblank selected `main-window` capture and clean restoration | No DXVK launch before `M1-V`; white/blank/fallback/contaminated output or incomplete backend evidence fails/holds |
| `D-C` DXVK campaign | DXVK independently passes smoke, corpus/main-story completion, controls, UI/subtitles, cutscenes, audio, save/load/restart, lifecycle, performance, two-hour smoke, and four-to-eight-hour endurance | Native results or cross-loads cannot repair a DXVK failure; any hard stop fails |
| `Fully playable(B)` claim | The applicable backend gate is passed and all definition items in Section 2 have retained evidence | Claim remains unproven if any item is not tested, unresolved, or only inferred from another backend |

Recorded outcomes on this machine (2026-09-16), operator-signed unless noted:

- `P0`: followed for the authorized launches (explicit approval, physical display).
- `M1-V`: **pass** — probe `20260916-160549-pid34040-native-se-none_pi-none_od-i`
  plus operator start-menu confirmation. 15s capture was contaminated and is not
  used as proof.
- `N-S`: **pass** for keyboard/mouse. Controller: **not tested**.
- `N-C`: **not yet tested**.
- DXVK gameplay smoke (not `D-V`): **pass** by operator report, two boots, HUD
  visible. Controller: **not tested**.
- `D-V`: **not yet tested** (no current probe capture packet).
- `D-C` / `Fully playable(B)`: **not yet tested**.
- Modern-display program (2026-09-16, same day, this machine):
  - `W1` pixel path: **pass** — `force_width/force_height/force_windowed/force_refresh_hz`
    applied at CreateDevice and Reset with requested/forced/created backbuffer honesty
    log; forced 1920x1080 boot-proven (log: requested=800x600, forced=1920x1080,
    created=1920x1080).
  - `W2` ASI coexistence stack: **pass** — proxy `d3d9.dll` + `dinput8.dll` Ultimate
    ASI Loader + `plugins\Bully.WidescreenFix.asi` (resolution unlock operator-confirmed)
    + `plugins\SilentPatchBully.asi`; multiple boots, no load conflicts.
  - `W5` borderless display contract: **pass** (operator: "everything works") —
    `borderless=1` strips chrome, pins the window to the whole monitor (`rcMonitor`,
    backbuffer scales to fit; 1080p backbuffer fills 2048x1152, 2560x1440
    supersamples), re-asserts after Reset, re-pins on drift every 10th Present,
    IAT-neutralizes the exe's `ChangeDisplaySettingsA` import (CDS_FULLSCREEN calls
    return success without changing the desktop), and forces windowed with a
    refresh-rate=0 guard. Alt-tab survives; the DEVICELOST/Reset-INVALIDCALL death
    spiral observed on earlier boots is gone. Known residue: the game rewrites its
    registry `WIN=0` preference at exit (launches re-set `WIN=1`); WidescreenFix's
    unlocked mode list on this machine excludes 2048x1152 (2560x1440 is present).
  - `W4` (DXVK as default with full stack) / `W6` (drop-in package): **not yet
    tested**.
