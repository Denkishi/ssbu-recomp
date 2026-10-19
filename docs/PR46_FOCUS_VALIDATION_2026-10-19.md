# PR #46 focus notification validation (2026-10-19)

Base: upstream `d876f640a8f516f9dc794bd5d759e04d12c13542`. This branch contains the focus change and its tests only; no personal networking, mod or delay extension is included.

## Startup regression

The initial PR accepted any foreground HWND owned by the process. The independent watcher could therefore make a transient foreground observation of the off-screen test window opt into subsequent focus loss. Both watcher sampling sites and guest queries now share one observation helper. Before tracking has started, the window must be visible, not minimized, activatable (no `WS_EX_NOACTIVATE`) and intersect a monitor (`MONITOR_DEFAULTTONULL`). Once tracking has started, the existing focus modes and suspension table apply.

`python tests/test_oe_focus.py` builds the real runtime notification queue/events with simulated window queries. The new startup checks fail on the previous PR at the off-screen case. With this revision they pass for off-screen, non-activating, hidden and minimized transient foreground windows. The suite runs event-only subscription, guest-bootstrap subscription and `SSBU_FOCUS_NOTIFY=0`. Each enabled run also checks 12 focus returns without guest polling, queue draining/no duplicate notification, NoSuspend and suspend/resume modes.

## Real game runs

Windows, NVIDIA Vulkan, local 13.0.4 dump, existing generated game DLL and script modules. Every upstream host translation unit was rebuilt; game machine-code translation was reused. Saves were copied into separate validation directories. No user save or original game archive was modified.

Both background runs used `SSBU_SHOW=0`, normal-speed `vsync=clock`, completed shader preparation with `shader_load=start`, fixed seed 1, deterministic fight logging and this pad-poll sequence:

```
@movie @until:500-650/60:Z/200 30:u/30 20:l/18 30:Z 60:r/50 20:d/8 30:Z @fight 120
```

The fixture save starts with the established Smash menu flow; `SSBU_DEBUG_MATCH=p1=mario,p2=donkey,p2=cpu,cpu=3,stage=battlefield` sets the selected match and `SSBU_DEBUG_END=180` exits after 180 presented fight frames. Each run passed through the title/menu path and reached the actual fight. `SSBU_FOCUS_NOTIFY=1` and `=0` both exited normally in 94.7 seconds; their 794 fighter-state records are identical.

A third run used the same real runtime, continued for 1,200 presented fight frames and exercised actual Windows foreground transitions. After reaching the fight, the off-screen game window was moved onto a monitor and made activatable; the foreground was alternated with a temporary helper window three times. A read-only observer located the runtime counters through the linker map and `ReadProcessMemory` (no runtime memory writes or injected notification calls).

- All three losses reported focus state 2; all three returns reported state 1 and resumed pad/notification polling.
- In two of the losses, the notification poll counter and presented-frame counter were unchanged across the settled observation interval. Returning focus resumed both, covering the original blocked-guest case.
- The third loss kept polling; it still returned correctly. It is not counted as another blocked-guest reproduction.
- Return observation latencies: 172, 171 and 156 ms (includes the probe's 150 ms settle interval, not an input-latency benchmark).
- The run reached 1,200 presented fight frames and exited normally, total 105.5 seconds. The original foreground application was restored afterwards.

The automated notification regressions and real-game results are distinct evidence. This does not claim a replay of the original two-PC online session.
