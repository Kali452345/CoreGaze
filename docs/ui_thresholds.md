# UI Threshold Color Shifts

CoreGaze now applies utilization-driven color shifts to selected HUD progress bars directly inside the Dear ImGui render loop.

This is a frontend-only enhancement:
- no metric collection cadence was changed
- no SystemMonitor polling logic was changed
- no new heap allocations were introduced

## Scope

Dynamic thresholds are applied to:
- CPU progress bar
- RAM progress bar
- GPU progress bars

Disk and network bars keep their existing color behavior.

## Threshold Rules

The render loop uses utilization fractions already passed to `ImGui::ProgressBar`.

For each targeted bar:
- `< 0.80`: keep the metric's base color
- `>= 0.80` and `< 0.95`: switch to warning orange
- `>= 0.95`: switch to critical red

Thresholds are inclusive at boundaries (`>= 0.80`, `>= 0.95`) so transitions are deterministic.

## Implementation Detail

A small helper in `main.cpp` resolves bar colors per frame:
- input: normalized utilization fraction and the bar's base color
- output: `ImVec4` for `ImGuiCol_PlotHistogram`

The helper:
- clamps fraction to `[0, 1]`
- returns base/warning/critical color according to thresholds
- uses only stack values and `ImVec4` literals (zero dynamic allocation)

Rendering continues to use the existing style stack pattern:
- `ImGui::PushStyleColor(...)`
- `ImGui::ProgressBar(...)`
- `ImGui::PopStyleColor()`

This keeps style state isolated per bar draw call and avoids stack imbalance.

## Integration Points

CPU, RAM, and GPU bars were updated to call the threshold helper before each `ProgressBar` draw.
Existing bar fractions are reused, so there is no additional backend work in the hot polling path.
