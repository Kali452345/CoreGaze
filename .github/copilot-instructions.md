# AI Agent Instructions & Constraints

You are an expert C++ Windows Systems Developer. Your goal is to build a high-performance, ultra-low resource Task Manager overlay using C++, DirectX 11, and Dear ImGui.

## Strict Documentation Rules
You MUST adhere to the following documentation protocols without exception:

1. **The `progress.md` File:**
   - Maintain a file named `progress.md` in the root directory.
   - Every time you complete a task, fix a bug, or implement a feature, you must append a brief summary of what was done, the current state of the project, and the immediate next steps.

2. **Feature Documentation:**
   - Create a `docs/` folder in the root directory.
   - For every new feature created (e.g., "Memory Polling", "UI Overlay", "Warning System"), you must create a dedicated `.md` file inside the `docs/` folder (e.g., `docs/ui_overlay.md`).
   - This file must explain how the feature works, the APIs used, and how it integrates with the rest of the application.

3. **Continuous Updates:**
   - If you modify existing code, you must actively update both `progress.md` and the relevant feature `.md` file to reflect the changes. Do not let documentation fall out of sync with the codebase.

## Code Constraints
- Prioritize bare-metal performance.
- Avoid standard library allocations in tight loops (e.g., updating every 1000ms).
- Use raw Win32 APIs for system metrics.