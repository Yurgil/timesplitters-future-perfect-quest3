# AGENTS.md — Native Meta Quest 3 Game Port

## 1. Mission and priorities

Port the target game to run **natively on Meta Quest 3** as an Android ARM64 application with stereoscopic VR, tracked head movement, appropriate motion-controller input, and practical standalone performance. The final deliverable is a reproducible, installable APK/AAB and a documented build process, not PC VR streaming.

Priorities, in order:
1. Preserve game behavior, feel, and recognizable presentation.
2. Achieve correct, comfortable, low-latency VR interaction.
3. Maintain a legal, reproducible, inspectable build.
4. Reach a stable frame rate and acceptable thermals on the headset.
5. Minimize complexity, regressions, and agent/token cost.

Do not claim a feature, build, test, or headset performance result without evidence.

## 2. Project facts and uncertainty

At the start of a task, inspect only the relevant existing documentation and files. Prefer `docs/port-status.md`, `docs/architecture.md`, and the build configuration, if present. Do not assume the original engine, available source, assets, licensing, graphics API, or target SDK.

Record established facts in `docs/port-status.md`; distinguish **verified**, **inferred**, and **unknown**. If a critical choice depends on an unknown, perform the smallest useful investigation or ask the user.

Use the following placeholders until verified:
- Original game and version: UNKNOWN
- Source availability and license: UNKNOWN
- Original engine and platform: UNKNOWN
- Existing ports/decompilation/reimplementations: UNKNOWN
- Rendering backend: UNKNOWN
- Android toolchain: UNKNOWN
- XR runtime/API: UNKNOWN
- Target frame rate: 72 Hz initially, subject to headset profiling and user approval

## 3. Scope and non-goals

In scope when needed: engine/platform feasibility, legal asset loading, ARM64/Android port, native activity lifecycle, OpenXR integration, stereoscopic rendering, VR camera, controller input, locomotion, UI, audio, save paths, packaging, debugging, and performance.

Out of scope unless explicitly requested: rewriting the whole game, unrelated refactoring, visual remasters, new gameplay systems, multiplayer changes, cloud services, account integrations, and Quest 2/Pro support.

Do not silently change the original game's mechanics to make VR implementation easier. Document proposed compromises and obtain approval for major changes.

## 4. Working method

For each task:
1. Restate the requested outcome and measurable acceptance criteria in one or two sentences.
2. Check the smallest relevant part of the repository and current port status.
3. Identify the shortest implementation path and important risks.
4. Make a focused change; avoid unrelated edits.
5. Run the cheapest meaningful verification first, then broader tests when warranted.
6. Report changed files, actual test results, remaining risks, and next step concisely.

For large or ambiguous tasks, present a short plan before substantial changes. For straightforward tasks, implement directly without lengthy planning.

Prefer existing project conventions. Keep changes incremental and buildable. Do not replace working subsystems solely for architectural elegance. Avoid speculative abstractions, large rewrites, and broad dependency upgrades.

## 5. Token efficiency and context management

Optimize for **total subscription usage and completed work**, not maximum agent parallelism.

- Default to a single main agent. Do not spawn subagents for routine implementation, straightforward debugging, or sequential work.
- Delegate only when an independent, bounded task has a clear deliverable and is likely to save wall-clock time or isolate genuinely large context.
- Normally use at most **one** subagent; use two only for clearly independent investigations or components. Never spawn three or more without explicit user approval.
- Do not recursively delegate or create agents that supervise other agents.
- Do not repeat repository-wide scans, broad searches, dependency inventories, or architecture reviews when recent results already exist.
- Prefer `rg --files`, targeted `rg` patterns, and bounded file ranges over dumping whole files or logs.
- Check existing build artifacts and logs before rebuilding; never pretend stale artifacts validate current changes.
- Use deterministic tools (compiler, formatter, linter, profiler, tests) instead of asking agents to simulate them.
- Limit verbose command output and summarize failures; preserve full logs as files when necessary.
- Avoid repeated reviews without new code changes or concrete unresolved defects.
- Do not produce long reasoning transcripts, duplicate summaries, or unnecessary documentation.
- When the conversation grows large or a milestone ends, write a concise handoff in `docs/agent-handoff.md` and recommend a fresh chat. A new chat reduces carried conversation history but does **not** reset subscription quotas.
- Never assume token-count savings translate directly into equal subscription-limit savings.

### Delegation contract

Each delegated task must specify:
- Exact question or bounded deliverable.
- Relevant paths and allowed scope.
- Whether edits are permitted; default is **read-only**.
- Required evidence or test.
- A concise output format (normally 200–400 words maximum).
- Stop conditions and explicit exclusions.

Subagents must not modify overlapping files concurrently. Main agent owns integration, acceptance, and final reporting. Do not ask multiple agents to independently rediscover the same architecture.

Recommended subagent roles, invoked only when useful:
- **Explorer:** read-only, find relevant code, call graph, format, platform constraints; return paths and evidence.
- **Portability analyst:** isolate Android/ARM64 blockers, build constraints, and minimal fixes.
- **VR reviewer:** inspect one specific OpenXR, stereo, input, or comfort concern; do not perform general review.
- **Performance analyst:** analyze an actual captured trace, frame timing, GPU/CPU profile, or measured regression.

Use a lower-cost model/reasoning level for bounded research if the installed Codex version supports it. Do not invent model names or unsupported configuration keys; verify against the local Codex version.

## 6. Architecture and implementation boundaries

Prefer clean separation between:
- **Game logic:** simulation, original mechanics, scripting, world state.
- **Platform layer:** Android lifecycle, file paths, input events, audio, packaging.
- **Rendering backend:** GPU resources, frame submission, shader compatibility.
- **XR layer:** OpenXR instance/session, swapchains, view poses, actions, reference spaces.
- **VR adaptation:** camera, player rig, interaction, locomotion, HUD, comfort settings.
- **Assets and data:** original game data, converted resources, legal packaging strategy.

Do not mix OpenXR runtime calls deeply into gameplay code unless required by existing architecture. Prefer small interfaces/adapters and preserve a non-VR mode or test harness when practical.

Maintain explicit ownership of graphics contexts, framebuffers, swapchain images, and lifecycle resources. Handle suspend/resume, focus changes, session loss, graphics resource recreation, and application exit gracefully.

## 7. Native Quest 3 and OpenXR

Target Android ARM64 (`arm64-v8a`) and a standalone Quest 3 runtime. Prefer standard OpenXR interfaces and supported extensions over undocumented device-specific APIs. Verify current Meta/Android/OpenXR requirements from authoritative documentation before making SDK, loader, manifest, permission, or packaging decisions.

OpenXR integration must correctly handle:
- Instance/system/session creation and teardown.
- Session-state events and Android lifecycle transitions.
- Reference spaces and recentering behavior.
- View configuration, eye poses, and projection matrices.
- Swapchain creation, acquisition, wait, release, and frame submission.
- Predicted display time, pose sampling, and frame timing.
- Action sets, controller profiles, tracking loss, and haptics where supported.

Use a consistent coordinate convention and document conversions between game, rendering, and OpenXR spaces. Validate world scale, handedness, near/far clipping, eye offsets, and camera transforms. Never implement stereoscopy by simply shifting a flat camera image.

Choose Vulkan or OpenGL ES based on existing engine feasibility and Quest support, not preference alone. Avoid adding a second renderer unless necessary.

## 8. VR experience and comfort

Maintain correct 6DoF head tracking and stable stereo perspective. Decouple head pose from gameplay camera motion, scripted shakes, cutscenes, and original mouse-look where appropriate.

Prefer physical head movement with controller-driven locomotion. Provide configurable snap turn and smooth turn when feasible; use conservative defaults. Avoid forced acceleration, camera roll, head bob, and involuntary camera movement. For vehicles, cockpit experiences, climbing, and scripted sequences, design and test context-specific comfort behavior.

Make menus, HUD, subtitles, reticles, and interactions usable in stereo at comfortable depth and scale. Preserve controller mappings in a documented input table. Handle loss of tracking and controller disconnection without unsafe or stuck input states.

Do not call a VR interaction comfortable without headset testing. Expose options rather than forcing a single locomotion style.

## 9. Performance and thermal budget

Optimize for **sustained standalone performance**, not a brief desktop benchmark. Initially aim for a stable 72 FPS (approximately 13.9 ms per frame), then revise the target based on real Quest 3 profiling and game requirements. Frame budget is shared across CPU, GPU, runtime, and system overhead; do not treat 13.9 ms as an independent budget for each subsystem.

Profile on device before significant optimization. Capture actual FPS, frame-time distribution, CPU/GPU bottleneck, thermal behavior, memory, and resolution settings when tools permit.

Prioritize measured bottlenecks: draw calls, shader cost, overdraw, transparency, shadow passes, texture memory/bandwidth, CPU simulation, asset loading, garbage collection, and synchronization. Consider supported multiview, foveation, dynamic resolution, texture compression, and LOD where compatible with the engine and runtime.

Never claim stable frame rate, thermal stability, or absence of judder based solely on an emulator, desktop build, or successful APK compilation.

## 10. Build, validation, and evidence

Prefer a documented, repeatable build using pinned or recorded versions of Android SDK/NDK, build system, compiler, XR loader, and relevant dependencies. Do not modify global toolchains or install system packages without permission.

Use this validation ladder when applicable:
1. Static checks, formatting, and targeted unit tests.
2. Host-side compile/tests for portable components.
3. Android ARM64 build and link.
4. APK packaging, manifest/ABI/dependency inspection.
5. Installation and startup on Quest 3.
6. OpenXR session, stereo rendering, head/controller tracking, input, audio, saves, and lifecycle checks.
7. On-device profiling, thermal soak, and comfort assessment.

Run only levels supported by the environment. Report **not run** for inaccessible tests; never substitute a guess. Do not repeatedly run full builds after documentation-only changes.

For each significant milestone, capture:
- Build command and toolchain version.
- Commit/revision and artifact path.
- Test device and runtime version when available.
- Pass/fail evidence and reproducible failure steps.
- Performance measurements with capture method, if applicable.

## 11. Debugging protocol

Reproduce before patching whenever possible. Identify the failing layer: game logic, asset format, Android platform, graphics backend, OpenXR lifecycle, input, or runtime compatibility.

Use a hypothesis-driven loop:
1. State one plausible cause and evidence.
2. Run one discriminating test or inspect a bounded trace.
3. Apply the smallest fix.
4. Verify the original failure and nearby regressions.
5. Stop when acceptance criteria are met.

Avoid random flag changes, repeated rebuilds without a new hypothesis, and broad refactoring during bug fixes. If blocked, report the exact blocker and a minimal next diagnostic action.


## 12. Repository and documentation hygiene

Recommended files (create only when useful, not automatically):
- `docs/port-status.md`: current verified state, working/broken features, next milestone.
- `docs/architecture.md`: relevant engine and platform boundaries.
- `docs/feasibility.md`: source, licenses, candidate approaches, risks.
- `docs/vr-controls.md`: action bindings, locomotion, comfort options.
- `docs/build.md`: reproducible host and Quest build/install commands.
- `docs/performance.md`: dated on-device measurements and test settings.
- `docs/agent-handoff.md`: concise continuation state for a fresh chat.

Keep `docs/agent-handoff.md` short (preferably under 500 words) and structured as:
- Current goal and status.
- Relevant files and last meaningful changes.
- Verified build/test results.
- Known blockers and unresolved decisions.
- Exact next action.

Do not add generated binaries, SDK directories, proprietary assets, huge logs, or profiler dumps to Git by default. Update `.gitignore` appropriately.

## 13. Git and change safety

Do not discard or overwrite user changes. Inspect Git status before destructive operations. Do not force-push, reset, rebase shared history, delete branches, or modify unrelated work without approval.

Prefer small, reviewable commits with meaningful descriptions when commits are requested. Do not create commits automatically unless the user or project workflow explicitly authorizes it.

Ask before major architectural rewrites, new heavyweight dependencies, destructive migrations, or changes that materially affect legal distribution, VR comfort, or gameplay fidelity.

## 14. Communication and completion format

Communicate with the user in **Russian**, unless requested otherwise. Code, identifiers, commit messages, and technical documentation may remain in English.

Keep updates concise and evidence-based. For completed work, use:

**Сделано:** what changed and why.
**Файлы:** relevant paths.
**Проверено:** exact commands/tests and observed results.
**Не проверено:** device-dependent or unavailable validation.
**Риски:** only concrete remaining issues.
**Дальше:** one highest-value next step.

Do not declare completion if the APK has not been built or headset behavior has not been verified when those are part of the acceptance criteria.

## 15. Definition of done

A task is done when its explicit acceptance criteria are met, the smallest relevant verification passes, changes are documented where needed, and remaining limitations are stated accurately.

The overall native VR port is not done until a legally distributable or user-buildable package can be reproduced, the game runs on an actual Quest 3, stereo and tracking are correct, required controls and gameplay work, save/load and lifecycle are acceptable, and sustained performance/comfort have been measured on-device.
