# CarHvacController UI photo checklist

Use the 320×240 panel in landscape. Photograph the full active area straight-on, avoiding glare and motion blur. Include enough exposure to read all labels; do not change firmware between shots except where a test step says to.

1. **Main / normal:** day mode, no faults, setpoint and cabin readings visible, manual fan level and blower percentage visible.
2. **Main / AUTO:** controller-reported AUTO selected and actual fan percentage visible.
3. **Main / DEF:** after one tap on DEF; show the reported operating mode/status.
4. **Main / A/C disagreement:** where test conditions permit, photograph the A/C tile's explicit `ON` with `COMP OFF` (and optionally the reverse) to demonstrate the request and compressor state are independent.
5. **Main / sensor fault:** unplug the selected sensor safely with ignition/vehicle in a safe test state; capture the main SENSOR badge.
6. **Main / valve fault:** under a controlled test, trigger a known valve stall; capture the main VALVE/FAULT badge.
7. **Main / link lost:** safely stop/disconnect remote IO communication; capture LINK LOST on the main header.
8. **Diagnostics / normal:** show temperatures, mode, fan, blower %, the five named actual/target valve rows and thermal state.
9. **Diagnostics / link loss:** capture the plain-language LINK LOST explanation and TX/RX/CRC/timeout counters.
10. **Diagnostics / DAY:** photograph the button in DAY state; then tap it, photograph NIGHT and power-cycle to verify persistence.
11. **Airflow picker:** open MODE and photograph all seven modes plus Cancel; confirm all cells are visible and tappable.
12. **Calibration confirmation / cancel:** open RESET CAL, photograph the prompt, tap KEEP MODEL, and verify no reset occurred.
13. **Calibration confirmation / accept:** reopen and photograph the confirmed RESET CAL action; only perform the reset in a controlled test environment.
14. **Gesture release safety:** swipe starting on a button or inside the diagnostics list switches pages without activating anything; repeat in both directions and confirm an open picker/message box prevents page changes behind it.

Also record the `HVAC_UI` boot log containing `LVGL heap after UI init`, then two or more five-second `hvac_ui_update avg/max` timing lines. Note ambient daylight/night conditions and whether the photographer could read the main screen in under one second without touching it.
15. **Tap slop — AUTO:** slowly drag across AUTO with either axis moving more than 10 px between press and release; confirm AUTO does not activate.
16. **Swipe starting on DEF:** perform a fast horizontal swipe starting on DEF; confirm the page changes and quick defrost does not start.
17. **Swipe inside picker:** open MODE and swipe within the picker; confirm no mode is selected and the picker stays open.
18. **Repeat slide-off:** hold temperature `+` or fan `+`, then slide more than 10 px on either axis; confirm repeat stepping stops and release causes no extra step.
