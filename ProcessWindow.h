#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "ProcessMonitor.h"
#include "EtwMonitor.h"

class SystemMonitor;
struct ImGuiContext;

// Posted to the notify window when the user asks to restart CoreGaze as administrator.
static const UINT WM_COREGAZE_RESTART_ELEVATED = WM_APP + 110;

// Task Manager-style process list in its own top-level window, opened on demand.
//
// Cost model: nothing here exists until Open(), and Close() frees it all (window, swap chain, ImGui
// context, collectors, ETW session). While open, data is sampled at the chosen update speed and a
// frame is drawn only after new data or user input, capped at 60 fps; an idle, unfocused window
// draws once per refresh. Sampling pauses while the window is minimized.
class ProcessWindow {
public:
    ProcessWindow();
    ~ProcessWindow();

    // Creates the window, or restores and focuses it if it is already open. The D3D device and
    // context are shared with the HUD. Settings are read from and saved to configPath [Processes].
    bool Open(HINSTANCE instance, ID3D11Device* device, ID3D11DeviceContext* deviceContext,
              SystemMonitor* systemMonitor, HWND notifyWindow, const wchar_t* configPath,
              HICON largeIcon, HICON smallIcon);
    void Close();
    bool IsOpen() const { return m_hwnd != NULL; }

    // Samples and draws when due. Returns how many ms the caller may wait before the next call.
    DWORD Tick();

private:
    enum Column {
        COL_NAME = 0,
        COL_PID,
        COL_CPU,
        COL_MEMORY,
        COL_DISK,
        COL_NETWORK,
        COL_GPU,
        COL_GPU_ENGINE,
        COL_IO,
        COL_THREADS,
        COL_HANDLES,
        COL_WORKING_SET,
        COL_COMMIT,
        COL_SESSION,
        COL_PARENT_PID,
        COL_COUNT
    };

    enum LineKind {
        LINE_PROCESS = 0,   // a process on its own (ungrouped, or the only one with its name)
        LINE_GROUP,         // several processes with the same name
        LINE_MEMBER         // a process inside an expanded group
    };

    struct Line {
        int kind;
        int index;          // row index for processes/members, group index for groups
    };

    // Several processes with the same image name, summed like Task Manager's app groups.
    struct Group {
        int firstMember;    // into m_members
        int memberCount;
        DWORD nameHash;
        const char* name;
        float cpuPercent;
        ULONGLONG privateWorkingSet;
        ULONGLONG workingSet;
        ULONGLONG commitBytes;
        float diskBytesPerSec;
        float networkBytesPerSec;
        float ioBytesPerSec;
        float gpuPercent;
        BYTE gpuEngineType;
        DWORD threadCount;
        DWORD handleCount;
        DWORD sessionId;
    };

    struct Settings {
        DWORD refreshMs;    // 0 = paused
        BOOL groupByName;
        BOOL alwaysOnTop;
        BOOL hasPlacement;
        RECT placement;     // normal (restored) window rect, physical pixels
        BOOL maximized;
    };

    // Window, rendering
    HWND m_hwnd;
    HWND m_notifyWindow;
    HINSTANCE m_instance;
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_deviceContext;
    IDXGISwapChain1* m_swapChain;
    ID3D11RenderTargetView* m_renderTarget;
    ImGuiContext* m_imgui;
    float m_dpiScale;
    bool m_styleDirty;
    bool m_inFrame;
    char m_iniPath[MAX_PATH * 3];
    wchar_t m_configPath[MAX_PATH];

    // Frame scheduling
    int m_pendingFrames;
    ULONGLONG m_lastFrameTick;
    ULONGLONG m_lateFrameTick;     // one extra frame after input stops (tooltips, hover delays)
    ULONGLONG m_lastSampleTick;
    bool m_sampleRequested;

    // Data
    SystemMonitor* m_systemMonitor;
    ProcessMonitor m_monitor;
    EtwMonitor m_etw;
    bool m_elevated;
    MEMORYSTATUSEX m_memoryStatus;
    float m_gpuTotalPercent;
    Settings m_settings;

    // View (rebuilt after each sample and on sort/filter/group changes; all buffers grow-only)
    int* m_filtered;
    int* m_members;
    Group* m_groups;
    Line* m_lines;
    UINT m_viewCapacity;
    UINT m_filteredCount;
    UINT m_groupCount;
    UINT m_lineCount;
    bool m_viewDirty;
    int m_sortColumn;
    bool m_sortDescending;
    char m_filter[128];

    DWORD* m_expandedGroups;       // name hashes of expanded groups
    UINT m_expandedCount;
    UINT m_expandedCapacity;

    // Selection survives refreshes by identity: a pid, or a group's name hash.
    bool m_selectionIsGroup;
    DWORD m_selectedPid;
    DWORD m_selectedGroupHash;
    int m_selectedLine;            // -1 if the selection is not in the current view
    bool m_scrollToSelection;
    bool m_focusFilter;
    int m_visibleLines;            // rows that fit in the table, for Page Up/Down

    // Pending actions. Targets are captured with their creation times, so a pid reused by a new
    // process between the click and the confirmation is never acted on.
    DWORD* m_actionPids;           // targets of the context menu / End task confirmation
    ULONGLONG* m_actionCreateTimes;
    UINT m_actionPidCount;
    UINT m_actionPidCapacity;
    bool m_actionIsGroup;
    DWORD m_actionGroupHash;
    DWORD m_actionPriority;        // current priority class of a single target, 0 if unknown
    char m_actionLabel[160];
    bool m_openContextMenu;
    bool m_openEndTaskConfirm;
    char m_statusText[256];
    ULONGLONG m_statusExpireTick;

    // Setup and teardown
    bool CreateWindowAndDevice(HICON largeIcon, HICON smallIcon);
    bool CreateSwapChain();
    void CreateRenderTarget();
    void ReleaseRenderTarget();
    void ResizeSwapChain(UINT width, UINT height);
    bool CreateImGuiContext();
    void ApplyStyle();
    void LoadSettings();
    void SaveSettings();
    void ApplyAlwaysOnTop();
    RECT DefaultPlacement() const;

    // Scheduling
    void RequestFrames(int count);
    void OnInput();

    // Data and view
    bool EnsureViewCapacity(UINT rowCount);
    void ReleaseView();
    void SampleData();
    void RebuildView();
    bool MatchesFilter(const ProcessRow& row, const char* loweredFilter, bool numericFilter) const;
    void SortRows(int* indices, UINT count) const;
    void SortGroups();
    void BuildLines();
    void LocateSelection();
    bool IsExpanded(DWORD nameHash) const;
    void SetExpanded(DWORD nameHash, bool expanded);
    void SelectLine(int lineIndex);
    bool CollectSelectionPids();

    // Drawing
    void RenderFrame();
    void DrawUi();
    void DrawToolbar();
    void DrawTable();
    void DrawTotalsRow(float rowHeight);
    void DrawLine(int lineIndex, float rowHeight);
    void DrawContextMenu();
    void DrawEndTaskConfirm();
    void DrawStatusBar();
    void HandleKeyboard();

    // Actions
    void EndSelectedTasks();
    void SetSelectedPriority(DWORD priorityClass);
    void OpenFileLocation(DWORD pid);
    void OpenProperties(DWORD pid);
    void CopySelectionToClipboard();
    void SetStatus(const char* format, ...);

    static int CompareRowIndices(const void* left, const void* right);
    static int CompareRowIndicesByName(const void* left, const void* right);
    static int CompareGroups(const void* left, const void* right);
    static LRESULT WINAPI WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
};
