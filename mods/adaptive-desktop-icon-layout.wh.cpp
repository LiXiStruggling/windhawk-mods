// ==WindhawkMod==
// @id              adaptive-desktop-icon-layout
// @name            Adaptive Desktop Icon Layout
// @description     Compacts and reflows desktop icons on manual refresh, display/DPI changes, screen rotation, and Remote Desktop transitions while preserving logical order.
// @description:zh-CN 在手动刷新、分辨率或缩放变化、屏幕旋转及远程桌面切换时，自适应紧密整理桌面图标并保持逻辑顺序。
// @version         1.0.0
// @author          LiXiStruggling
// @github          https://github.com/LiXiStruggling
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lshell32 -lshlwapi -lcomctl32 -lwtsapi32 -luuid
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Adaptive Desktop Icon Layout

Keeps normal desktop dragging free-form while making icon layout predictable when
Refresh or a display/session transition requires the desktop to be rearranged.
Icons are compacted in column-major order: top-to-bottom, then left-to-right.

## Features

- **Manual compact:** Press `F5` while the desktop is focused, or choose
  **Refresh** from the desktop background context menu.
- **Display adaptation:** Resolution, DPI/scale, work-area changes, and display
  rotation automatically reflow the layout after the new geometry stabilizes.
- **2-in-1 / convertible PCs:** Landscape/portrait rotation produces a compact
  layout for the new orientation without changing the logical icon order.
- **Remote Desktop:** RDP connect/disconnect and remote display-size changes
  preserve the pre-transition logical order instead of trusting Explorer's
  temporary transition coordinates.

## Order preservation

Manual Refresh freezes the logical icon order before Explorer processes the
refresh, so Explorer-side coordinate normalization cannot silently become a new
canonical order. Display and RDP transitions use the last stable local logical
order, wait for the new desktop geometry to settle, and then reflow that order
into the new grid.

The mod uses Shell folder-view APIs to position desktop items and does not modify
Windows system files.

## Notes

- The initial public release targets 64-bit Windows (`x86-64`) and was developed
  and tested on Windows 11.
- Manual compact and automatic reflow operate on the detected desktop grid. If
  you intentionally keep different icon groups spread across multiple monitors,
  this mod may not match that workflow.
- If the planned desktop grid would place any icon anchor outside the detected
  monitor work area, the mod refuses the operation before moving any icons.
- If the mod is first enabled while an RDP session is already active and no
  local order has ever been recorded, there is no pre-connection local layout
  to recover. Later normal connect/disconnect transitions preserve the known
  local logical order.

---

## 简体中文

平时仍可自由拖放桌面图标；手动刷新或显示/会话环境变化需要重新布局时，本模块会按
“从上到下、再从左到右”的列优先顺序紧密排列，并保持原有逻辑顺序。

### 功能

- **手动整理：** 桌面获得焦点时按 `F5`，或在桌面空白处右键选择“刷新”。
- **显示环境变化：** 分辨率、DPI/缩放、工作区以及横竖屏旋转变化后自动适配。
- **二合一设备：** 横屏/竖屏切换后按新方向重新紧密排列，不改变逻辑顺序。
- **远程桌面：** RDP 连接、断开和远程显示尺寸变化时沿用切换前的逻辑顺序，
  不把 Explorer 过渡阶段产生的临时坐标当作新的顺序。

### 顺序保护

手动刷新会在 Explorer 真正处理刷新前先冻结逻辑顺序，避免 Explorer 刷新过程中对坐标的
调整被误记为新的标准顺序。显示环境和 RDP 切换则使用最后一个稳定的本机逻辑顺序，等待
新桌面几何稳定后，再把同一顺序映射到新的网格。

模块通过 Shell FolderView API 定位图标，不会修改 Windows 系统文件。

### 说明

- 首个公开版本面向 64 位 Windows（`x86-64`），目前主要在 Windows 11 上开发和测试。
- 手动整理和自动适配会使用检测到的桌面网格；如果你刻意把不同图标组分散在多显示器上，
  本模块的紧密整理逻辑可能不符合这种使用方式。
- 如果计划后的桌面网格会使任何图标锚点超出检测到的显示器工作区，模块会在移动任何图标前
  拒绝本次操作。
- 如果第一次启用模块时已经处于 RDP 会话中，而且此前从未记录过本机顺序，模块无法凭空恢复
  “连接前”的本机布局；之后正常的连接/断开切换会沿用已知的本机逻辑顺序。
*/
// ==/WindhawkModReadme==

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cwchar>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace drc {

struct ItemState {
    std::wstring id;
    int x;
    int y;
};

struct GridGeometry {
    int originX;
    int originY;
    int spacingX;
    int spacingY;
    int workRight;
    int workBottom;
};

struct PlannedPosition {
    std::wstring id;
    int x;
    int y;
};


struct DesktopEnvironmentSignature {
    int clientWidth;
    int clientHeight;
    int workLeft;
    int workTop;
    int workRight;
    int workBottom;
    unsigned dpi;
    int spacingX;
    int spacingY;
};

struct ConsoleLayoutSignature {
    int originX;
    int originY;
    int spacingX;
    int spacingY;
    int workBottom;
};

bool SameConsoleLayout(const ConsoleLayoutSignature& a,
                       const ConsoleLayoutSignature& b) {
    return a.originX == b.originX &&
           a.originY == b.originY &&
           a.spacingX == b.spacingX &&
           a.spacingY == b.spacingY &&
           a.workBottom == b.workBottom;
}

bool ShouldPreserveCanonicalOnConsole(bool haveCanonical,
                                      bool haveStoredLayout,
                                      const ConsoleLayoutSignature& stored,
                                      const ConsoleLayoutSignature& current) {
    if (!haveCanonical) {
        return false;
    }
    // A canonical order without a stored layout is treated as authoritative.
    // Prefer the known logical order over coordinates that may already have been
    // transformed by a DPI/resolution change.
    if (!haveStoredLayout) {
        return true;
    }
    return !SameConsoleLayout(stored, current);
}

bool SameDesktopEnvironment(const DesktopEnvironmentSignature& a,
                            const DesktopEnvironmentSignature& b) {
    return a.clientWidth == b.clientWidth &&
           a.clientHeight == b.clientHeight &&
           a.workLeft == b.workLeft &&
           a.workTop == b.workTop &&
           a.workRight == b.workRight &&
           a.workBottom == b.workBottom &&
           a.dpi == b.dpi &&
           a.spacingX == b.spacingX &&
           a.spacingY == b.spacingY;
}

bool ShouldReflowEnvironment(bool haveBefore,
                             const DesktopEnvironmentSignature& before,
                             const DesktopEnvironmentSignature& after,
                             bool sessionModeChanged) {
    if (!haveBefore || sessionModeChanged) {
        return true;
    }
    return !SameDesktopEnvironment(before, after);
}



static int PositiveMod(int value, int modulus) {
    if (modulus <= 0) {
        return 0;
    }
    int remainder = value % modulus;
    return remainder < 0 ? remainder + modulus : remainder;
}

int InferGridPhase(const std::vector<int>& coordinates, int spacing) {
    if (coordinates.empty() || spacing <= 0) {
        return 0;
    }

    std::map<int, size_t> counts;
    for (int coordinate : coordinates) {
        counts[PositiveMod(coordinate, spacing)]++;
    }

    int bestPhase = 0;
    size_t bestCount = 0;
    for (const auto& [phase, count] : counts) {
        if (count > bestCount) {
            bestPhase = phase;
            bestCount = count;
        }
    }
    return bestPhase;
}

int RowsPerColumn(int originY, int spacingY, int workBottom) {
    if (spacingY <= 0 || workBottom <= originY) {
        return 1;
    }

    // Explorer's desktop grid capacity follows the number of complete grid
    // pitches that fit between the first-row phase and the work-area bottom.
    // Merely having the next row's anchor coordinate below workBottom doesn't
    // make that row usable (for example, the observed 200% grid is 7 rows,
    // not 8). Keep the floor division used by the empirically verified layout.
    const long long usableSpan =
        static_cast<long long>(workBottom) - static_cast<long long>(originY);
    return std::max(1, static_cast<int>(
        usableSpan / static_cast<long long>(spacingY)));
}

int ScaleMetricBetweenDpi(int value, unsigned sourceDpi, unsigned targetDpi) {
    if (value <= 0 || sourceDpi == 0 || targetDpi == 0 || sourceDpi == targetDpi) {
        return value;
    }

    const long long numerator =
        static_cast<long long>(value) * static_cast<long long>(targetDpi);
    const long long rounded =
        (numerator + static_cast<long long>(sourceDpi) / 2) /
        static_cast<long long>(sourceDpi);
    return std::max(1, static_cast<int>(rounded));
}

unsigned SelectEffectiveDpi(unsigned windowDpi,
                            unsigned monitorDpi,
                            int monitorScalePercent) {
    if (monitorDpi > 0) {
        return monitorDpi;
    }
    if (monitorScalePercent > 0) {
        const long long scaled = 96LL * monitorScalePercent;
        return static_cast<unsigned>((scaled + 50) / 100);
    }
    return windowDpi > 0 ? windowDpi : 96U;
}

std::vector<size_t> DeriveColumnMajorOrder(const std::vector<ItemState>& items,
                                           int spacingX) {
    std::vector<size_t> indices(items.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        indices[i] = i;
    }
    if (items.empty()) {
        return indices;
    }

    std::vector<int> xCoordinates;
    xCoordinates.reserve(items.size());
    for (const auto& item : items) {
        xCoordinates.push_back(item.x);
    }
    const int originX = InferGridPhase(xCoordinates, spacingX);

    auto columnFor = [originX, spacingX](int x) {
        if (spacingX <= 0) {
            return x;
        }
        return static_cast<int>(std::llround(
            static_cast<double>(x - originX) / static_cast<double>(spacingX)));
    };

    std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        const int colA = columnFor(items[a].x);
        const int colB = columnFor(items[b].x);
        if (colA != colB) {
            return colA < colB;
        }
        if (items[a].y != items[b].y) {
            return items[a].y < items[b].y;
        }
        if (items[a].x != items[b].x) {
            return items[a].x < items[b].x;
        }
        return a < b;
    });

    return indices;
}

std::vector<std::wstring> MergeCanonicalOrder(
    const std::vector<std::wstring>& canonical,
    const std::vector<std::wstring>& present) {
    std::unordered_set<std::wstring> presentSet(present.begin(), present.end());
    std::unordered_set<std::wstring> emitted;
    std::vector<std::wstring> merged;
    merged.reserve(present.size());

    for (const auto& id : canonical) {
        if (presentSet.find(id) != presentSet.end() && emitted.insert(id).second) {
            merged.push_back(id);
        }
    }
    for (const auto& id : present) {
        if (emitted.insert(id).second) {
            merged.push_back(id);
        }
    }
    return merged;
}

std::vector<std::wstring> ResolveEnvironmentOrder(
    const std::vector<std::wstring>& preferred,
    const std::vector<std::wstring>& present) {
    if (preferred.empty()) {
        return present;
    }
    return MergeCanonicalOrder(preferred, present);
}

std::wstring SerializeStrings(const std::vector<std::wstring>& values) {
    std::wstring result;
    for (const auto& value : values) {
        result += std::to_wstring(value.size());
        result.push_back(L'#');
        result += value;
    }
    return result;
}

std::vector<std::wstring> DeserializeStrings(const std::wstring& encoded) {
    std::vector<std::wstring> values;
    size_t pos = 0;
    while (pos < encoded.size()) {
        size_t marker = encoded.find(L'#', pos);
        if (marker == std::wstring::npos || marker == pos) {
            return {};
        }
        size_t length = 0;
        for (size_t i = pos; i < marker; ++i) {
            wchar_t ch = encoded[i];
            if (ch < L'0' || ch > L'9') {
                return {};
            }
            length = length * 10 + static_cast<size_t>(ch - L'0');
        }
        pos = marker + 1;
        if (length > encoded.size() - pos) {
            return {};
        }
        values.emplace_back(encoded.substr(pos, length));
        pos += length;
    }
    return values;
}



std::vector<PlannedPosition> PlanCompact(const std::vector<std::wstring>& ids,
                                         const GridGeometry& grid) {
    std::vector<PlannedPosition> plan;
    plan.reserve(ids.size());
    const int rows = RowsPerColumn(grid.originY, grid.spacingY, grid.workBottom);

    for (size_t i = 0; i < ids.size(); ++i) {
        const int column = static_cast<int>(i / static_cast<size_t>(rows));
        const int row = static_cast<int>(i % static_cast<size_t>(rows));
        const long long x = static_cast<long long>(grid.originX) +
                            static_cast<long long>(column) * grid.spacingX;
        const long long y = static_cast<long long>(grid.originY) +
                            static_cast<long long>(row) * grid.spacingY;

        // Never ask Explorer to place an icon anchor outside the detected
        // monitor work area. If the current work area cannot hold every item,
        // fail the whole plan instead of producing a partial/off-screen layout.
        if (x >= grid.workRight || y >= grid.workBottom) {
            return {};
        }

        plan.push_back({
            ids[i],
            static_cast<int>(x),
            static_cast<int>(y),
        });
    }
    return plan;
}

}  // namespace drc

#ifndef ADAPTIVE_DESKTOP_ICON_LAYOUT_UNIT_TEST

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlobj.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <wtsapi32.h>
#include <windhawk_utils.h>

namespace drcwin {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }
    T* Get() const { return ptr_; }
    T** Put() {
        Reset();
        return &ptr_;
    }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    void Reset(T* replacement = nullptr) {
        if (ptr_) {
            ptr_->Release();
        }
        ptr_ = replacement;
    }
private:
    T* ptr_ = nullptr;
};

struct DesktopItem {
    std::wstring id;
    POINT position{};
    PIDLIST_RELATIVE pidl = nullptr;

    DesktopItem() = default;
    DesktopItem(const DesktopItem&) = delete;
    DesktopItem& operator=(const DesktopItem&) = delete;
    DesktopItem(DesktopItem&& other) noexcept
        : id(std::move(other.id)), position(other.position), pidl(other.pidl) {
        other.pidl = nullptr;
    }
    DesktopItem& operator=(DesktopItem&& other) noexcept {
        if (this != &other) {
            if (pidl) {
                CoTaskMemFree(pidl);
            }
            id = std::move(other.id);
            position = other.position;
            pidl = other.pidl;
            other.pidl = nullptr;
        }
        return *this;
    }
    ~DesktopItem() {
        if (pidl) {
            CoTaskMemFree(pidl);
        }
    }
};

HRESULT FindDesktopFolderView(IFolderView** view) {
    if (!view) {
        return E_POINTER;
    }
    *view = nullptr;

    ComPtr<IShellWindows> shellWindows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(shellWindows.Put()));
    if (FAILED(hr)) {
        return hr;
    }

    VARIANT location;
    VARIANT empty;
    VariantInit(&location);
    VariantInit(&empty);
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;

    long hwnd = 0;
    ComPtr<IDispatch> dispatch;
    hr = shellWindows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd,
                                    SWFO_NEEDDISPATCH, dispatch.Put());
    VariantClear(&location);
    VariantClear(&empty);
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IServiceProvider> serviceProvider;
    hr = dispatch->QueryInterface(IID_PPV_ARGS(serviceProvider.Put()));
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IShellBrowser> browser;
    hr = serviceProvider->QueryService(SID_STopLevelBrowser,
                                       IID_PPV_ARGS(browser.Put()));
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IShellView> shellView;
    hr = browser->QueryActiveShellView(shellView.Put());
    if (FAILED(hr)) {
        return hr;
    }

    return shellView->QueryInterface(IID_PPV_ARGS(view));
}

static std::wstring PidlToHex(PCUIDLIST_RELATIVE pidl) {
    const UINT bytes = ILGetSize(pidl);
    const auto* raw = reinterpret_cast<const unsigned char*>(pidl);
    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring result = L"pidl:";
    result.reserve(5 + static_cast<size_t>(bytes) * 2);
    for (UINT i = 0; i < bytes; ++i) {
        result.push_back(kHex[(raw[i] >> 4) & 0x0F]);
        result.push_back(kHex[raw[i] & 0x0F]);
    }
    return result;
}

static std::wstring GetStableItemIdentity(IShellFolder* folder,
                                          PCUITEMID_CHILD pidl) {
    STRRET str{};
    PWSTR text = nullptr;
    if (SUCCEEDED(folder->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &str)) &&
        SUCCEEDED(StrRetToStrW(&str, pidl, &text)) && text && *text) {
        std::wstring result(text);
        CoTaskMemFree(text);
        return result;
    }
    if (text) {
        CoTaskMemFree(text);
    }
    return PidlToHex(pidl);
}

HRESULT EnumerateDesktop(IFolderView* view, std::vector<DesktopItem>& items) {
    if (!view) {
        return E_POINTER;
    }
    items.clear();

    ComPtr<IShellFolder> folder;
    HRESULT hr = view->GetFolder(IID_PPV_ARGS(folder.Put()));
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IEnumIDList> enumerator;
    hr = view->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(enumerator.Put()));
    if (FAILED(hr)) {
        return hr;
    }

    while (true) {
        PIDLIST_RELATIVE pidl = nullptr;
        ULONG fetched = 0;
        hr = enumerator->Next(1, &pidl, &fetched);
        if (hr == S_FALSE || fetched == 0) {
            return S_OK;
        }
        if (FAILED(hr)) {
            return hr;
        }

        DesktopItem item;
        item.pidl = pidl;
        item.id = GetStableItemIdentity(folder.Get(), pidl);
        hr = view->GetItemPosition(pidl, &item.position);
        if (FAILED(hr)) {
            return hr;
        }
        items.push_back(std::move(item));
    }
}

HRESULT ApplyPositions(IFolderView* view,
                       const std::vector<DesktopItem>& items,
                       const std::vector<drc::PlannedPosition>& plan) {
    if (!view) {
        return E_POINTER;
    }
    if (items.size() != plan.size()) {
        return E_INVALIDARG;
    }

    std::unordered_map<std::wstring, const DesktopItem*> byId;
    byId.reserve(items.size());
    for (const auto& item : items) {
        if (!byId.emplace(item.id, &item).second) {
            return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
        }
    }

    std::vector<PCUITEMID_CHILD> pidls;
    std::vector<POINT> points;
    pidls.reserve(plan.size());
    points.reserve(plan.size());
    for (const auto& target : plan) {
        auto found = byId.find(target.id);
        if (found == byId.end()) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        pidls.push_back(found->second->pidl);
        points.push_back({target.x, target.y});
    }

    if (pidls.empty()) {
        return S_OK;
    }
    return view->SelectAndPositionItems(
        static_cast<UINT>(pidls.size()), pidls.data(), points.data(),
        SVSI_POSITIONITEM);
}

static constexpr wchar_t kCanonicalOrderKey[] = L"canonicalOrderV1";
static constexpr wchar_t kConsoleLayoutKey[] = L"consoleLayoutV1";
static constexpr size_t kStateBufferChars = 256 * 1024;

static bool LoadStoredString(PCWSTR key, std::wstring& value) {
    std::vector<wchar_t> buffer(kStateBufferChars);
    size_t chars = Wh_GetStringValue(key, buffer.data(), buffer.size());
    if (chars == 0) {
        value.clear();
        return false;
    }
    value.assign(buffer.data(), chars);
    return true;
}

static bool LoadCanonicalOrderFromKey(PCWSTR key,
                                      std::vector<std::wstring>& canonical) {
    std::wstring encoded;
    if (!LoadStoredString(key, encoded)) {
        canonical.clear();
        return false;
    }
    canonical = drc::DeserializeStrings(encoded);
    return !canonical.empty();
}

bool LoadCanonicalOrder(std::vector<std::wstring>& canonical) {
    if (LoadCanonicalOrderFromKey(kCanonicalOrderKey, canonical)) {
        return true;
    }
    canonical.clear();
    return false;
}

static bool SaveCanonicalOrderOnly(const std::vector<std::wstring>& canonical) {
    const std::wstring encoded = drc::SerializeStrings(canonical);
    if (encoded.empty()) {
        return false;
    }
    return Wh_SetStringValue(kCanonicalOrderKey, encoded.c_str()) != FALSE;
}

static bool SerializeConsoleLayout(const drc::ConsoleLayoutSignature& layout,
                                   std::wstring& encoded) {
    wchar_t buffer[160]{};
    const int written = std::swprintf(
        buffer, ARRAYSIZE(buffer), L"%d,%d,%d,%d,%d",
        layout.originX, layout.originY, layout.spacingX, layout.spacingY,
        layout.workBottom);
    if (written <= 0 || static_cast<size_t>(written) >= ARRAYSIZE(buffer)) {
        encoded.clear();
        return false;
    }
    encoded.assign(buffer, static_cast<size_t>(written));
    return true;
}

static bool LoadConsoleLayout(drc::ConsoleLayoutSignature& layout) {
    std::wstring encoded;
    if (!LoadStoredString(kConsoleLayoutKey, encoded)) {
        return false;
    }
    int originX = 0;
    int originY = 0;
    int spacingX = 0;
    int spacingY = 0;
    int workBottom = 0;
    if (std::swscanf(encoded.c_str(), L"%d,%d,%d,%d,%d",
                     &originX, &originY, &spacingX, &spacingY, &workBottom) != 5) {
        return false;
    }
    if (spacingX <= 0 || spacingY <= 0) {
        return false;
    }
    layout = {originX, originY, spacingX, spacingY, workBottom};
    return true;
}

static bool SaveConsoleState(const std::vector<std::wstring>& canonical,
                             const drc::ConsoleLayoutSignature& layout) {
    const std::wstring encodedCanonical = drc::SerializeStrings(canonical);
    std::wstring encodedLayout;
    if (encodedCanonical.empty() || !SerializeConsoleLayout(layout, encodedLayout)) {
        return false;
    }

    // Write the small layout record first. If the canonical write fails, roll
    // the layout record back so a partial state commit can't change the next
    // console refresh decision.
    std::wstring previousLayout;
    const bool hadPreviousLayout = LoadStoredString(kConsoleLayoutKey, previousLayout);

    if (Wh_SetStringValue(kConsoleLayoutKey, encodedLayout.c_str()) == FALSE) {
        return false;
    }

    if (Wh_SetStringValue(kCanonicalOrderKey, encodedCanonical.c_str()) == FALSE) {
        const PCWSTR rollback = hadPreviousLayout ? previousLayout.c_str() : L"";
        if (Wh_SetStringValue(kConsoleLayoutKey, rollback) == FALSE) {
            Wh_Log(L"AdaptiveDesktopIconLayout: failed to roll back console layout after canonical write failure");
        }
        return false;
    }

    return true;
}

static HRESULT RestoreSnapshotInMemory(IFolderView* view,
                                       const std::vector<DesktopItem>& items) {
    std::vector<drc::PlannedPosition> original;
    original.reserve(items.size());
    for (const auto& item : items) {
        original.push_back({item.id, item.position.x, item.position.y});
    }
    return ApplyPositions(view, items, original);
}

bool IsRemoteSession() {
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
        return GetSystemMetrics(SM_REMOTESESSION) != 0;
    }

    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                    WTSClientProtocolType, &buffer, &bytes) &&
        buffer && bytes >= sizeof(USHORT)) {
        USHORT protocol = *reinterpret_cast<USHORT*>(buffer);
        WTSFreeMemory(buffer);
        return protocol != 0;
    }
    if (buffer) {
        WTSFreeMemory(buffer);
    }
    return GetSystemMetrics(SM_REMOTESESSION) != 0;
}

static HWND GetFolderViewListWindow(IFolderView* view) {
    ComPtr<IShellView> shellView;
    if (FAILED(view->QueryInterface(IID_PPV_ARGS(shellView.Put())))) {
        return nullptr;
    }
    HWND shellViewWindow = nullptr;
    if (FAILED(shellView->GetWindow(&shellViewWindow)) || !shellViewWindow) {
        return nullptr;
    }
    HWND list = FindWindowExW(shellViewWindow, nullptr, L"SysListView32", L"FolderView");
    return list ? list : shellViewWindow;
}

// Explorer's desktop FolderView can retain grid spacing in the window's DPI
// even after the monitor has switched to another effective DPI. Prefer the
// desktop SysListView32 as the raw spacing source, then normalize that spacing
// from the window DPI to the monitor DPI when the two differ.
static bool TryGetDesktopListViewSpacing(HWND listView, POINT* spacing) {
    if (!listView || !spacing || !IsWindow(listView)) {
        return false;
    }

    wchar_t className[64]{};
    if (!GetClassNameW(listView, className, ARRAYSIZE(className)) ||
        _wcsicmp(className, L"SysListView32") != 0) {
        return false;
    }

    const LRESULT packed = SendMessageW(listView, LVM_GETITEMSPACING, FALSE, 0);
    const int spacingX = static_cast<int>(LOWORD(static_cast<DWORD_PTR>(packed)));
    const int spacingY = static_cast<int>(HIWORD(static_cast<DWORD_PTR>(packed)));
    if (spacingX <= 0 || spacingY <= 0) {
        return false;
    }

    spacing->x = spacingX;
    spacing->y = spacingY;
    return true;
}

static int QueryMonitorScaleFactorPercent(HMONITOR monitor, HRESULT* outHr = nullptr) {
    if (outHr) {
        *outHr = E_FAIL;
    }

    HMODULE shcore = GetModuleHandleW(L"shcore.dll");
    bool loadedHere = false;
    if (!shcore) {
        shcore = LoadLibraryW(L"shcore.dll");
        loadedHere = shcore != nullptr;
    }
    if (!shcore) {
        if (outHr) {
            *outHr = HRESULT_FROM_WIN32(GetLastError());
        }
        return 0;
    }

    using GetScaleFactorForMonitorFn = HRESULT (WINAPI*)(HMONITOR, int*);
    auto fn = reinterpret_cast<GetScaleFactorForMonitorFn>(
        GetProcAddress(shcore, "GetScaleFactorForMonitor"));

    int scale = 0;
    HRESULT hr = fn ? fn(monitor, &scale) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (outHr) {
        *outHr = hr;
    }

    if (loadedHere) {
        FreeLibrary(shcore);
    }
    return SUCCEEDED(hr) ? scale : 0;
}

static bool QueryMonitorEffectiveDpi(HMONITOR monitor, UINT* dpiX, UINT* dpiY,
                                     HRESULT* outHr = nullptr) {
    if (dpiX) *dpiX = 0;
    if (dpiY) *dpiY = 0;
    if (outHr) *outHr = E_FAIL;

    HMODULE shcore = GetModuleHandleW(L"shcore.dll");
    bool loadedHere = false;
    if (!shcore) {
        shcore = LoadLibraryW(L"shcore.dll");
        loadedHere = shcore != nullptr;
    }
    if (!shcore) {
        if (outHr) *outHr = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }

    using GetDpiForMonitorFn = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
    auto fn = reinterpret_cast<GetDpiForMonitorFn>(
        GetProcAddress(shcore, "GetDpiForMonitor"));

    UINT x = 0;
    UINT y = 0;
    // MDT_EFFECTIVE_DPI == 0.
    HRESULT hr = fn ? fn(monitor, 0, &x, &y)
                    : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (outHr) *outHr = hr;
    if (SUCCEEDED(hr)) {
        if (dpiX) *dpiX = x;
        if (dpiY) *dpiY = y;
    }

    if (loadedHere) {
        FreeLibrary(shcore);
    }
    return SUCCEEDED(hr);
}

static bool BuildGridGeometry(IFolderView* view,
                              const std::vector<DesktopItem>& items,
                              drc::GridGeometry& grid,
                              bool logNormalization = true) {
    if (!view || items.empty()) {
        return false;
    }

    HWND viewWindow = GetFolderViewListWindow(view);
    if (!viewWindow) {
        return false;
    }

    POINT listSpacing{};
    const bool listSpacingOk = TryGetDesktopListViewSpacing(viewWindow, &listSpacing);

    POINT folderSpacing{};
    const HRESULT folderSpacingHr = view->GetSpacing(&folderSpacing);
    const bool folderSpacingOk = SUCCEEDED(folderSpacingHr) &&
                                 folderSpacing.x > 0 && folderSpacing.y > 0;

    POINT rawSpacing{};
    const wchar_t* spacingSource = L"none";
    if (listSpacingOk) {
        rawSpacing = listSpacing;
        spacingSource = L"ListView";
    } else if (folderSpacingOk) {
        rawSpacing = folderSpacing;
        spacingSource = L"IFolderView";
    } else {
        Wh_Log(L"AdaptiveDesktopIconLayout: GRIDDIAG no valid spacing; listSpacing=%d,%d listOk=%d folderSpacing=%d,%d folderHr=0x%08X",
               listSpacing.x, listSpacing.y, listSpacingOk ? 1 : 0,
               folderSpacing.x, folderSpacing.y, folderSpacingHr);
        return false;
    }

    std::vector<int> xs;
    std::vector<int> ys;
    xs.reserve(items.size());
    ys.reserve(items.size());
    for (const auto& item : items) {
        xs.push_back(item.position.x);
        ys.push_back(item.position.y);
    }

    // First locate the monitor using the raw Explorer grid. Explorer can keep
    // reporting spacing in the window's old DPI after a live display-scale
    // change, but the raw phase is still sufficient to identify the monitor.
    int rawOriginX = drc::InferGridPhase(xs, rawSpacing.x);
    int rawOriginY = drc::InferGridPhase(ys, rawSpacing.y);
    POINT rawOriginScreen{rawOriginX, rawOriginY};
    if (!ClientToScreen(viewWindow, &rawOriginScreen)) {
        return false;
    }

    HMONITOR monitor = MonitorFromPoint(rawOriginScreen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return false;
    }

    UINT windowDpi = GetDpiForWindow(viewWindow);
    if (windowDpi == 0) {
        windowDpi = 96;
    }

    UINT monitorDpiX = 0;
    UINT monitorDpiY = 0;
    QueryMonitorEffectiveDpi(monitor, &monitorDpiX, &monitorDpiY);

    const int monitorScalePercent = QueryMonitorScaleFactorPercent(monitor);

    const UINT targetDpiX =
        drc::SelectEffectiveDpi(windowDpi, monitorDpiX, monitorScalePercent);
    const UINT targetDpiY =
        drc::SelectEffectiveDpi(windowDpi, monitorDpiY, monitorScalePercent);

    POINT spacing = rawSpacing;
    const bool dpiNormalized = targetDpiX != windowDpi || targetDpiY != windowDpi;
    if (dpiNormalized) {
        spacing.x = drc::ScaleMetricBetweenDpi(rawSpacing.x, windowDpi, targetDpiX);
        spacing.y = drc::ScaleMetricBetweenDpi(rawSpacing.y, windowDpi, targetDpiY);
        if (spacing.x <= 0 || spacing.y <= 0) {
            spacing = rawSpacing;
        }
    }

    // Recompute the phase in the target monitor DPI. This is what removes the
    // "A, blank, B, blank..." pattern when Explorer's grid is still in the
    // previous DPI while the monitor has already switched scale.
    int originX = drc::InferGridPhase(xs, spacing.x);
    int originY = drc::InferGridPhase(ys, spacing.y);

    RECT work = monitorInfo.rcWork;
    SetLastError(ERROR_SUCCESS);
    int mapped = MapWindowPoints(nullptr, viewWindow,
                                 reinterpret_cast<POINT*>(&work), 2);
    if (mapped == 0 && GetLastError() != ERROR_SUCCESS) {
        return false;
    }

    while (originX < work.left) {
        originX += spacing.x;
    }
    while (originY < work.top) {
        originY += spacing.y;
    }
    if (work.bottom <= originY) {
        return false;
    }

    if (dpiNormalized && logNormalization) {
        Wh_Log(L"AdaptiveDesktopIconLayout: normalized desktop grid DPI %u -> %u,%u; spacing %d,%d -> %d,%d (%s)",
               windowDpi, targetDpiX, targetDpiY,
               rawSpacing.x, rawSpacing.y, spacing.x, spacing.y, spacingSource);
    }

    grid = {originX, originY, spacing.x, spacing.y, work.right, work.bottom};
    return true;
}

static std::vector<drc::ItemState> ToCoreItems(const std::vector<DesktopItem>& items) {
    std::vector<drc::ItemState> coreItems;
    coreItems.reserve(items.size());
    for (const auto& item : items) {
        coreItems.push_back({item.id, item.position.x, item.position.y});
    }
    return coreItems;
}

static std::vector<std::wstring> CurrentColumnMajorIds(
    const std::vector<DesktopItem>& items, int spacingX) {
    auto coreItems = ToCoreItems(items);
    auto order = drc::DeriveColumnMajorOrder(coreItems, spacingX);
    std::vector<std::wstring> ids;
    ids.reserve(order.size());
    for (size_t index : order) {
        ids.push_back(items[index].id);
    }
    return ids;
}

static bool CaptureCurrentLogicalOrder(std::vector<std::wstring>* orderOut) {
    if (!orderOut) {
        return false;
    }

    ComPtr<IFolderView> view;
    HRESULT hr = FindDesktopFolderView(view.Put());
    if (FAILED(hr)) {
        return false;
    }

    std::vector<DesktopItem> items;
    hr = EnumerateDesktop(view.Get(), items);
    if (FAILED(hr) || items.empty()) {
        return false;
    }

    drc::GridGeometry grid{};
    if (!BuildGridGeometry(view.Get(), items, grid, false)) {
        return false;
    }

    auto order = CurrentColumnMajorIds(items, grid.spacingX);
    std::unordered_set<std::wstring> unique(order.begin(), order.end());
    if (unique.size() != order.size()) {
        return false;
    }

    *orderOut = std::move(order);
    return true;
}

HRESULT CompactDesktop(bool remoteSession,
                       const std::vector<std::wstring>* forcedLogicalOrder = nullptr,
                       bool persistConsoleState = true,
                       PCWSTR reason = L"manual") {
    ComPtr<IFolderView> view;
    HRESULT hr = FindDesktopFolderView(view.Put());
    if (FAILED(hr)) {
        Wh_Log(L"AdaptiveDesktopIconLayout: FindDesktopFolderView failed: 0x%08X", hr);
        return hr;
    }

    std::vector<DesktopItem> items;
    hr = EnumerateDesktop(view.Get(), items);
    if (FAILED(hr)) {
        Wh_Log(L"AdaptiveDesktopIconLayout: EnumerateDesktop failed: 0x%08X", hr);
        return hr;
    }
    if (items.empty()) {
        return S_OK;
    }

    drc::GridGeometry grid{};
    if (!BuildGridGeometry(view.Get(), items, grid)) {
        Wh_Log(L"AdaptiveDesktopIconLayout: failed to derive grid geometry");
        return E_FAIL;
    }

    std::vector<std::wstring> orderedIds;
    bool preserveCanonicalForConsole = false;
    drc::ConsoleLayoutSignature currentConsoleLayout{
        grid.originX, grid.originY, grid.spacingX, grid.spacingY, grid.workBottom
    };

    auto currentIds = CurrentColumnMajorIds(items, grid.spacingX);

    if (forcedLogicalOrder && !forcedLogicalOrder->empty()) {
        orderedIds = drc::ResolveEnvironmentOrder(*forcedLogicalOrder, currentIds);
        preserveCanonicalForConsole = !remoteSession;
    } else if (remoteSession) {
        std::vector<std::wstring> canonical;
        if (!LoadCanonicalOrder(canonical)) {
            Wh_Log(L"AdaptiveDesktopIconLayout: remote compact skipped; canonical order not established yet");
            return S_FALSE;
        }
        orderedIds = drc::ResolveEnvironmentOrder(canonical, currentIds);
    } else {
        std::vector<std::wstring> canonical;
        const bool haveCanonical = LoadCanonicalOrder(canonical);
        drc::ConsoleLayoutSignature storedConsoleLayout{};
        const bool haveStoredConsoleLayout = LoadConsoleLayout(storedConsoleLayout);

        preserveCanonicalForConsole = drc::ShouldPreserveCanonicalOnConsole(
            haveCanonical, haveStoredConsoleLayout, storedConsoleLayout,
            currentConsoleLayout);

        if (preserveCanonicalForConsole) {
            orderedIds = drc::ResolveEnvironmentOrder(canonical, currentIds);
            Wh_Log(L"AdaptiveDesktopIconLayout: preserving canonical column-major order across console layout change");
        } else {
            orderedIds = std::move(currentIds);
        }
    }

    if (orderedIds.size() != items.size()) {
        Wh_Log(L"AdaptiveDesktopIconLayout: identity set is not unique; refusing to move icons");
        return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
    }

    auto plan = drc::PlanCompact(orderedIds, grid);
    if (plan.size() != items.size()) {
        Wh_Log(L"AdaptiveDesktopIconLayout: detected work area cannot fit all %u icons; refusing to move icons",
               static_cast<unsigned>(items.size()));
        return E_FAIL;
    }


    hr = ApplyPositions(view.Get(), items, plan);
    if (FAILED(hr)) {
        Wh_Log(L"AdaptiveDesktopIconLayout: positioning failed: 0x%08X; restoring original coordinates", hr);
        const HRESULT rollbackHr = RestoreSnapshotInMemory(view.Get(), items);
        if (FAILED(rollbackHr)) {
            Wh_Log(L"AdaptiveDesktopIconLayout: rollback failed after positioning error: 0x%08X",
                   rollbackHr);
        }
        return hr;
    }

    if (!remoteSession && persistConsoleState) {
        if (!SaveConsoleState(orderedIds, currentConsoleLayout)) {
            Wh_Log(L"AdaptiveDesktopIconLayout: failed to persist canonical order/layout; restoring original coordinates");
            const HRESULT rollbackHr = RestoreSnapshotInMemory(view.Get(), items);
            if (FAILED(rollbackHr)) {
                Wh_Log(L"AdaptiveDesktopIconLayout: rollback failed after state persistence error: 0x%08X",
                       rollbackHr);
            }
            return E_FAIL;
        }
    }

    Wh_Log(L"AdaptiveDesktopIconLayout: compacted %u icons (%s, column-major%s, reason=%s)",
           static_cast<unsigned>(items.size()),
           remoteSession ? L"RDP" : L"console",
           preserveCanonicalForConsole ? L", logical-order-preserved" : L"",
           reason ? reason : L"unknown");
    return S_OK;
}

static constexpr UINT kCompactAfterRefreshMessage = WM_APP + 0x4D2;
static constexpr UINT kDesktopBrowserRefreshCommand = 0xA220;
static constexpr UINT kDefViewRefreshCommand = 0x7103;
static constexpr wchar_t kCompactPendingProperty[] = L"AdaptiveDesktopIconLayout.Pending";

static constexpr UINT_PTR kStableBaselineTimer = 0xD240;
static constexpr UINT_PTR kEnvironmentSettleTimer = 0xD241;
static constexpr UINT kStableBaselineIntervalMs = 500;
static constexpr UINT kEnvironmentSettleIntervalMs = 150;
static constexpr UINT kEnvironmentQuietMs = 300;
static constexpr UINT kEnvironmentMaxWaitMs = 6000;
static constexpr unsigned kRequiredStableEnvironmentSamples = 3;

enum class CompactRequestKind : unsigned {
    None = 0,
    ManualRefresh = 1,
};

enum class EnvironmentTransitionReason : unsigned {
    None = 0,
    LocalDisplayChange = 1,
    RemoteConnect = 2,
    RemoteDisplayChange = 3,
    RemoteDisconnect = 4,
};

static HWND g_desktopDefView = nullptr;
static HWND g_desktopListView = nullptr;
static HWND g_desktopHostRoot = nullptr;
static HWND g_sessionNotificationWindow = nullptr;
static HHOOK g_desktopGetMessageHook = nullptr;
static DWORD g_desktopThreadId = 0;
static bool g_f5RefreshArmed = false;
static bool g_refreshReadyArmed = false;
static CompactRequestKind g_readyRequestKind = CompactRequestKind::None;
static CompactRequestKind g_pendingCompactKind = CompactRequestKind::None;
static bool g_refreshReadyProbe = false;

// Deliberate Refresh is asynchronous: Explorer processes the refresh command
// before the compact operation runs. Freeze the local visual order at the
// command boundary so Explorer's own refresh-time normalization can't become
// the new logical order.
static std::vector<std::wstring> g_manualRefreshFrozenOrder;
static bool g_manualRefreshOrderActive = false;

static std::vector<std::wstring> g_lastStableLocalOrder;
static bool g_haveLastStableLocalOrder = false;
static drc::DesktopEnvironmentSignature g_lastStableEnvironmentSignature{};
static bool g_haveLastStableEnvironmentSignature = false;

static bool g_environmentTransitionActive = false;
static EnvironmentTransitionReason g_environmentTransitionReason =
    EnvironmentTransitionReason::None;
static std::vector<std::wstring> g_environmentFrozenOrder;
static drc::DesktopEnvironmentSignature g_environmentStartSignature{};
static bool g_haveEnvironmentStartSignature = false;
static drc::DesktopEnvironmentSignature g_environmentLastSample{};
static bool g_haveEnvironmentLastSample = false;
static unsigned g_environmentStableSamples = 0;
static ULONGLONG g_environmentStartTick = 0;
static ULONGLONG g_environmentLastSignalTick = 0;
static bool g_environmentStartedRemote = false;

static bool g_rdpOrderActive = false;
static std::vector<std::wstring> g_rdpLogicalOrder;

static bool IsDesktopHostRoot(HWND root) {
    if (!root) {
        return false;
    }
    if (root == GetShellWindow()) {
        return true;
    }
    wchar_t className[64]{};
    if (!GetClassNameW(root, className, ARRAYSIZE(className))) {
        return false;
    }
    return _wcsicmp(className, L"Progman") == 0 ||
           _wcsicmp(className, L"WorkerW") == 0;
}

static bool IsDesktopDefView(HWND hwnd) {
    if (!hwnd) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != GetCurrentProcessId()) {
        return false;
    }

    wchar_t className[64]{};
    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className)) ||
        _wcsicmp(className, L"SHELLDLL_DefView") != 0) {
        return false;
    }

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!IsDesktopHostRoot(root)) {
        return false;
    }
    return true;
}

static BOOL CALLBACK FindDesktopDefViewEnumProc(HWND hwnd, LPARAM lParam) {
    HWND candidate = nullptr;
    if (IsDesktopDefView(hwnd)) {
        candidate = hwnd;
    } else {
        candidate = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (candidate && !IsDesktopDefView(candidate)) {
            candidate = nullptr;
        }
    }
    if (candidate) {
        *reinterpret_cast<HWND*>(lParam) = candidate;
        return FALSE;
    }
    return TRUE;
}

HWND FindDesktopDefView() {
    HWND shell = GetShellWindow();
    if (shell) {
        HWND direct = FindWindowExW(shell, nullptr, L"SHELLDLL_DefView", nullptr);
        if (direct && IsDesktopDefView(direct)) {
            return direct;
        }
    }
    HWND found = nullptr;
    EnumWindows(FindDesktopDefViewEnumProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

static void ScheduleCompactAfterRefresh(HWND hwnd, CompactRequestKind kind) {
    if (!hwnd || !IsWindow(hwnd) || kind == CompactRequestKind::None) {
        return;
    }

    if (GetPropW(hwnd, kCompactPendingProperty)) {
        g_pendingCompactKind = kind;
        return;
    }

    g_pendingCompactKind = kind;
    if (!SetPropW(hwnd, kCompactPendingProperty, reinterpret_cast<HANDLE>(1))) {
        g_pendingCompactKind = CompactRequestKind::None;
        Wh_Log(L"AdaptiveDesktopIconLayout: failed to mark pending refresh compact: %u",
               GetLastError());
        return;
    }
    if (!PostMessageW(hwnd, kCompactAfterRefreshMessage, 0, 0)) {
        RemovePropW(hwnd, kCompactPendingProperty);
        g_pendingCompactKind = CompactRequestKind::None;
        Wh_Log(L"AdaptiveDesktopIconLayout: failed to schedule refresh compact: %u",
               GetLastError());
    }
}

static bool TryGetCurrentProcessSessionId(DWORD* sessionId) {
    return sessionId &&
           ProcessIdToSessionId(GetCurrentProcessId(), sessionId) != FALSE;
}

static bool RegisterDesktopSessionNotifications(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }
    if (g_sessionNotificationWindow == hwnd) {
        return true;
    }
    if (g_sessionNotificationWindow && IsWindow(g_sessionNotificationWindow)) {
        WTSUnRegisterSessionNotification(g_sessionNotificationWindow);
        g_sessionNotificationWindow = nullptr;
    }
    if (!WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION)) {
        Wh_Log(L"AdaptiveDesktopIconLayout: WTS session notification registration failed: %u",
               GetLastError());
        return false;
    }
    g_sessionNotificationWindow = hwnd;
    Wh_Log(L"AdaptiveDesktopIconLayout: registered RDP session notifications");
    return true;
}

static void UnregisterDesktopSessionNotifications(HWND hwnd = nullptr) {
    HWND target = hwnd ? hwnd : g_sessionNotificationWindow;
    if (!target || target != g_sessionNotificationWindow) {
        return;
    }
    WTSUnRegisterSessionNotification(target);
    g_sessionNotificationWindow = nullptr;
}

static bool CaptureDesktopEnvironmentSignature(
    drc::DesktopEnvironmentSignature* signature) {
    if (!signature) {
        return false;
    }

    HWND listView = g_desktopListView;
    if (!listView || !IsWindow(listView)) {
        HWND defView = g_desktopDefView;
        if (!defView || !IsWindow(defView)) {
            defView = FindDesktopDefView();
        }
        if (defView) {
            listView = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
        }
    }
    if (!listView || !IsWindow(listView)) {
        return false;
    }

    RECT client{};
    if (!GetClientRect(listView, &client)) {
        return false;
    }

    POINT center{
        (client.left + client.right) / 2,
        (client.top + client.bottom) / 2,
    };
    if (!ClientToScreen(listView, &center)) {
        return false;
    }

    HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return false;
    }

    POINT rawSpacing{};
    if (!TryGetDesktopListViewSpacing(listView, &rawSpacing)) {
        ComPtr<IFolderView> view;
        HRESULT hr = FindDesktopFolderView(view.Put());
        if (FAILED(hr) || FAILED(view->GetSpacing(&rawSpacing)) ||
            rawSpacing.x <= 0 || rawSpacing.y <= 0) {
            return false;
        }
    }

    UINT windowDpi = GetDpiForWindow(listView);
    if (windowDpi == 0) {
        windowDpi = 96;
    }

    UINT monitorDpiX = 0;
    UINT monitorDpiY = 0;
    QueryMonitorEffectiveDpi(monitor, &monitorDpiX, &monitorDpiY);
    const int monitorScalePercent = QueryMonitorScaleFactorPercent(monitor);
    const UINT effectiveDpiX =
        drc::SelectEffectiveDpi(windowDpi, monitorDpiX, monitorScalePercent);
    const UINT effectiveDpiY =
        drc::SelectEffectiveDpi(windowDpi, monitorDpiY, monitorScalePercent);

    POINT spacing{
        drc::ScaleMetricBetweenDpi(rawSpacing.x, windowDpi, effectiveDpiX),
        drc::ScaleMetricBetweenDpi(rawSpacing.y, windowDpi, effectiveDpiY),
    };

    const RECT& work = monitorInfo.rcWork;
    *signature = {
        client.right - client.left,
        client.bottom - client.top,
        work.left,
        work.top,
        work.right,
        work.bottom,
        effectiveDpiX,
        spacing.x,
        spacing.y,
    };
    return true;
}

static void StartOrExtendEnvironmentTransition(
    HWND hwnd,
    EnvironmentTransitionReason reason);

static PCWSTR EnvironmentReasonName(EnvironmentTransitionReason reason) {
    switch (reason) {
        case EnvironmentTransitionReason::LocalDisplayChange:
            return L"local-display-change";
        case EnvironmentTransitionReason::RemoteConnect:
            return L"RDP-connect";
        case EnvironmentTransitionReason::RemoteDisplayChange:
            return L"RDP-display-change";
        case EnvironmentTransitionReason::RemoteDisconnect:
            return L"RDP-disconnect";
        default:
            return L"unknown";
    }
}

static void RefreshStableLocalBaseline() {
    if (g_environmentTransitionActive || IsRemoteSession()) {
        return;
    }

    drc::DesktopEnvironmentSignature signature{};
    if (!CaptureDesktopEnvironmentSignature(&signature)) {
        return;
    }

    // The baseline sampler is also a safety net for transitions whose earliest
    // shell messages arrive after Explorer has already moved icons. Never let a
    // changed environment overwrite the last known pre-change logical order.
    if (g_haveLastStableEnvironmentSignature &&
        !drc::SameDesktopEnvironment(g_lastStableEnvironmentSignature,
                                     signature)) {
        if (g_desktopHostRoot && IsWindow(g_desktopHostRoot)) {
            StartOrExtendEnvironmentTransition(
                g_desktopHostRoot,
                EnvironmentTransitionReason::LocalDisplayChange);
        }
        return;
    }

    std::vector<std::wstring> order;
    if (!CaptureCurrentLogicalOrder(&order) || order.empty()) {
        return;
    }

    // Revalidate geometry after enumerating items. If the display changed while
    // the order was being captured, discard the mixed sample rather than
    // pairing post-transition coordinates with the pre-transition signature.
    drc::DesktopEnvironmentSignature verifiedSignature{};
    if (!CaptureDesktopEnvironmentSignature(&verifiedSignature) ||
        !drc::SameDesktopEnvironment(signature, verifiedSignature)) {
        if (g_haveLastStableEnvironmentSignature &&
            g_desktopHostRoot && IsWindow(g_desktopHostRoot)) {
            StartOrExtendEnvironmentTransition(
                g_desktopHostRoot,
                EnvironmentTransitionReason::LocalDisplayChange);
        }
        return;
    }

    g_lastStableLocalOrder = std::move(order);
    g_haveLastStableLocalOrder = true;
    g_lastStableEnvironmentSignature = verifiedSignature;
    g_haveLastStableEnvironmentSignature = true;
}

static bool ChooseFrozenEnvironmentOrder(std::vector<std::wstring>* orderOut) {
    if (!orderOut) {
        return false;
    }

    if (g_rdpOrderActive && !g_rdpLogicalOrder.empty()) {
        *orderOut = g_rdpLogicalOrder;
        return true;
    }

    if (g_haveLastStableLocalOrder && !g_lastStableLocalOrder.empty()) {
        *orderOut = g_lastStableLocalOrder;
        return true;
    }

    std::vector<std::wstring> canonical;
    if (LoadCanonicalOrder(canonical) && !canonical.empty()) {
        *orderOut = std::move(canonical);
        return true;
    }

    // Last-resort fallback for a brand-new install before the baseline timer has
    // fired. This may already be post-transition, so it is deliberately only
    // used when there is no safer state at all.
    if (CaptureCurrentLogicalOrder(orderOut) && !orderOut->empty()) {
        Wh_Log(L"AdaptiveDesktopIconLayout: WARNING no pre-transition baseline/canonical; using current visual order as fallback");
        return true;
    }

    orderOut->clear();
    return false;
}

static void StartOrExtendEnvironmentTransition(
    HWND hwnd,
    EnvironmentTransitionReason reason) {
    if (!hwnd || !IsWindow(hwnd) || reason == EnvironmentTransitionReason::None) {
        return;
    }

    const ULONGLONG now = GetTickCount64();

    if (!g_environmentTransitionActive) {
        g_environmentTransitionActive = true;
        g_environmentTransitionReason = reason;
        g_environmentStartTick = now;
        g_environmentLastSignalTick = now;
        g_environmentStableSamples = 0;
        g_haveEnvironmentLastSample = false;
        g_environmentStartedRemote = g_rdpOrderActive || IsRemoteSession();

        g_environmentFrozenOrder.clear();
        ChooseFrozenEnvironmentOrder(&g_environmentFrozenOrder);

        if (g_haveLastStableEnvironmentSignature) {
            g_environmentStartSignature = g_lastStableEnvironmentSignature;
            g_haveEnvironmentStartSignature = true;
        } else {
            g_haveEnvironmentStartSignature =
                CaptureDesktopEnvironmentSignature(&g_environmentStartSignature);
        }

        // Any transition that starts from the local console treats the
        // pre-transition local visual order as authoritative. Persist only the
        // logical order here; the console geometry stays the old local geometry
        // until a new local environment has actually stabilized.
        if (!g_rdpOrderActive && !g_environmentFrozenOrder.empty()) {
            if (!SaveCanonicalOrderOnly(g_environmentFrozenOrder)) {
                Wh_Log(L"AdaptiveDesktopIconLayout: failed to persist pre-transition logical order");
            }
        }

        Wh_Log(L"AdaptiveDesktopIconLayout: environment transition started (%s), frozenOrder=%u",
               EnvironmentReasonName(reason),
               static_cast<unsigned>(g_environmentFrozenOrder.size()));
    } else {
        // Connect/disconnect has stronger semantics than ordinary display
        // chatter; keep it if it arrives during the same transition.
        if (reason == EnvironmentTransitionReason::RemoteConnect ||
            reason == EnvironmentTransitionReason::RemoteDisconnect) {
            g_environmentTransitionReason = reason;
        } else if (g_environmentTransitionReason == EnvironmentTransitionReason::None) {
            g_environmentTransitionReason = reason;
        }
        g_environmentLastSignalTick = now;
        g_environmentStableSamples = 0;
        g_haveEnvironmentLastSample = false;
    }

    SetTimer(hwnd, kEnvironmentSettleTimer,
             kEnvironmentSettleIntervalMs, nullptr);
}

static void FinishEnvironmentTransition(HWND hwnd) {
    if (hwnd && IsWindow(hwnd)) {
        KillTimer(hwnd, kEnvironmentSettleTimer);
    }
    g_environmentTransitionActive = false;
    g_environmentTransitionReason = EnvironmentTransitionReason::None;
    g_environmentFrozenOrder.clear();
    g_haveEnvironmentStartSignature = false;
    g_haveEnvironmentLastSample = false;
    g_environmentStableSamples = 0;
    g_environmentStartTick = 0;
    g_environmentLastSignalTick = 0;
    g_environmentStartedRemote = false;
}

static HRESULT ProbeDesktopViewReady(int* itemCount) {
    if (itemCount) {
        *itemCount = 0;
    }

    ComPtr<IFolderView> view;
    HRESULT hr = FindDesktopFolderView(view.Put());
    if (FAILED(hr)) {
        return hr;
    }

    int count = 0;
    hr = view->ItemCount(SVGIO_ALLVIEW, &count);
    if (FAILED(hr)) {
        return hr;
    }
    if (itemCount) {
        *itemCount = count;
    }
    if (count <= 0) {
        return S_FALSE;
    }

    ComPtr<IEnumIDList> enumerator;
    hr = view->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(enumerator.Put()));
    if (FAILED(hr)) {
        return hr;
    }
    return enumerator ? S_OK : E_NOINTERFACE;
}

static void HandleEnvironmentSettleTimer(HWND hwnd) {
    if (!g_environmentTransitionActive) {
        KillTimer(hwnd, kEnvironmentSettleTimer);
        return;
    }

    const ULONGLONG now = GetTickCount64();
    drc::DesktopEnvironmentSignature current{};
    if (!CaptureDesktopEnvironmentSignature(&current)) {
        if (now - g_environmentStartTick >= kEnvironmentMaxWaitMs) {
            Wh_Log(L"AdaptiveDesktopIconLayout: environment transition timed out before environment signature became available");
            FinishEnvironmentTransition(hwnd);
        }
        return;
    }

    if (!g_haveEnvironmentLastSample ||
        !drc::SameDesktopEnvironment(g_environmentLastSample, current)) {
        g_environmentLastSample = current;
        g_haveEnvironmentLastSample = true;
        g_environmentStableSamples = 1;
        return;
    }

    ++g_environmentStableSamples;

    const bool quietLongEnough =
        now - g_environmentLastSignalTick >= kEnvironmentQuietMs;
    const bool stableLongEnough =
        g_environmentStableSamples >= kRequiredStableEnvironmentSamples;
    const bool timedOut =
        now - g_environmentStartTick >= kEnvironmentMaxWaitMs;

    if ((!quietLongEnough || !stableLongEnough) && !timedOut) {
        return;
    }

    int itemCount = 0;
    if (ProbeDesktopViewReady(&itemCount) != S_OK) {
        if (!timedOut) {
            return;
        }
        Wh_Log(L"AdaptiveDesktopIconLayout: environment transition timed out waiting for desktop readiness");
        FinishEnvironmentTransition(hwnd);
        return;
    }

    const bool nowRemote = IsRemoteSession();
    const bool sessionModeChanged =
        g_environmentTransitionReason == EnvironmentTransitionReason::RemoteConnect ||
        g_environmentTransitionReason == EnvironmentTransitionReason::RemoteDisconnect ||
        nowRemote != g_environmentStartedRemote;

    const bool environmentChanged = drc::ShouldReflowEnvironment(
        g_haveEnvironmentStartSignature,
        g_environmentStartSignature,
        current,
        sessionModeChanged);

    if (!environmentChanged) {
        Wh_Log(L"AdaptiveDesktopIconLayout: environment signal settled without an actual geometry/session change; no reflow");
        FinishEnvironmentTransition(hwnd);
        RefreshStableLocalBaseline();
        return;
    }

    if (g_environmentFrozenOrder.empty()) {
        Wh_Log(L"AdaptiveDesktopIconLayout: environment reflow skipped; no safe pre-transition logical order");
        FinishEnvironmentTransition(hwnd);
        RefreshStableLocalBaseline();
        return;
    }

    const EnvironmentTransitionReason reason = g_environmentTransitionReason;
    HRESULT hr = CompactDesktop(
        nowRemote,
        &g_environmentFrozenOrder,
        !nowRemote,
        EnvironmentReasonName(reason));

    if (FAILED(hr) && hr != S_FALSE) {
        Wh_Log(L"AdaptiveDesktopIconLayout: environment reflow failed: 0x%08X; will keep frozen order for the next signal",
               hr);
        if (!timedOut) {
            g_environmentStableSamples = 0;
            return;
        }
    }

    if (SUCCEEDED(hr)) {
        if (nowRemote) {
            g_rdpLogicalOrder = g_environmentFrozenOrder;
            g_rdpOrderActive = true;
        } else {
            g_lastStableLocalOrder = g_environmentFrozenOrder;
            g_haveLastStableLocalOrder = true;
            g_lastStableEnvironmentSignature = current;
            g_haveLastStableEnvironmentSignature = true;

            if (g_rdpOrderActive) {
                g_rdpLogicalOrder.clear();
                g_rdpOrderActive = false;
            }
        }

        Wh_Log(L"AdaptiveDesktopIconLayout: environment transition applied (%s), items=%d",
               EnvironmentReasonName(reason), itemCount);
    }

    FinishEnvironmentTransition(hwnd);
    if (!nowRemote) {
        RefreshStableLocalBaseline();
    }
}

static bool TryScheduleCompactWhenDesktopReady() {
    if (!g_refreshReadyArmed || g_refreshReadyProbe ||
        g_readyRequestKind == CompactRequestKind::None) {
        return false;
    }

    g_refreshReadyProbe = true;
    int itemCount = 0;
    HRESULT hr = ProbeDesktopViewReady(&itemCount);
    g_refreshReadyProbe = false;

    if (hr != S_OK) {
        return false;
    }

    HWND defView = g_desktopDefView;
    if (!defView || !IsWindow(defView)) {
        defView = FindDesktopDefView();
    }
    if (!defView) {
        return false;
    }

    CompactRequestKind kind = g_readyRequestKind;
    g_refreshReadyArmed = false;
    g_readyRequestKind = CompactRequestKind::None;

    Wh_Log(L"AdaptiveDesktopIconLayout: desktop view ready (%d items); scheduling refresh compact",
           itemCount);
    ScheduleCompactAfterRefresh(defView, kind);
    return true;
}

static void ArmCompactWhenDesktopReady(
    CompactRequestKind kind = CompactRequestKind::ManualRefresh) {
    if (kind == CompactRequestKind::None) {
        return;
    }
    if (!g_refreshReadyArmed ||
        kind == CompactRequestKind::ManualRefresh ||
        g_readyRequestKind == CompactRequestKind::None) {
        g_readyRequestKind = kind;
    }
    g_refreshReadyArmed = true;

    if (!TryScheduleCompactWhenDesktopReady()) {
        Wh_Log(L"AdaptiveDesktopIconLayout: waiting for desktop view readiness (refresh)");
    }
}

static void ClearManualRefreshOrder() {
    g_manualRefreshFrozenOrder.clear();
    g_manualRefreshOrderActive = false;
}

static void FreezeManualRefreshOrder() {
    ClearManualRefreshOrder();

    // Environment/RDP transitions already maintain their own frozen logical
    // order and take precedence over deliberate local refreshes.
    if (g_environmentTransitionActive || IsRemoteSession()) {
        return;
    }

    std::vector<std::wstring> order;
    if (!CaptureCurrentLogicalOrder(&order) || order.empty()) {
        // The stable baseline is a safe local-only fallback if a synchronous
        // Shell capture fails at the command boundary.
        if (g_haveLastStableLocalOrder && !g_lastStableLocalOrder.empty()) {
            order = g_lastStableLocalOrder;
        }
    }

    if (order.empty()) {
        Wh_Log(L"AdaptiveDesktopIconLayout: couldn't freeze pre-refresh logical order; using post-refresh fallback");
        return;
    }

    g_manualRefreshFrozenOrder = std::move(order);
    g_manualRefreshOrderActive = true;
    Wh_Log(L"AdaptiveDesktopIconLayout: froze pre-refresh logical order (%u items)",
           static_cast<unsigned>(g_manualRefreshFrozenOrder.size()));
}

static bool IsDesktopMessageTarget(HWND hwnd) {
    if (!hwnd) {
        return false;
    }
    if (hwnd == g_desktopListView || hwnd == g_desktopDefView ||
        hwnd == g_desktopHostRoot) {
        return true;
    }
    if (g_desktopDefView && IsWindow(g_desktopDefView) &&
        IsChild(g_desktopDefView, hwnd)) {
        return true;
    }
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root && g_desktopHostRoot && root == g_desktopHostRoot;
}

LRESULT CALLBACK DesktopGetMessageHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && lParam) {
        MSG* msg = reinterpret_cast<MSG*>(lParam);
        if ((msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) &&
            msg->wParam == VK_F5 && IsDesktopMessageTarget(msg->hwnd)) {
            g_f5RefreshArmed = true;
            Wh_Log(L"AdaptiveDesktopIconLayout: armed F5 refresh token");
        } else if ((msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP) &&
                   msg->wParam == VK_F5 && IsDesktopMessageTarget(msg->hwnd)) {
            g_f5RefreshArmed = false;
        }
    }
    return CallNextHookEx(g_desktopGetMessageHook, code, wParam, lParam);
}

bool InstallDesktopThreadMessageHook() {
    HWND target = g_desktopListView ? g_desktopListView : g_desktopDefView;
    if (!target || !IsWindow(target)) {
        Wh_Log(L"AdaptiveDesktopIconLayout: cannot install desktop thread hook; desktop target missing");
        return false;
    }
    DWORD processId = 0;
    DWORD threadId = GetWindowThreadProcessId(target, &processId);
    if (!threadId || processId != GetCurrentProcessId()) {
        Wh_Log(L"AdaptiveDesktopIconLayout: cannot resolve desktop UI thread");
        return false;
    }

    if (g_desktopGetMessageHook && g_desktopThreadId == threadId) {
        return true;
    }
    if (g_desktopGetMessageHook) {
        UnhookWindowsHookEx(g_desktopGetMessageHook);
        g_desktopGetMessageHook = nullptr;
        g_desktopThreadId = 0;
    }

    HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, DesktopGetMessageHookProc,
                                   nullptr, threadId);
    if (!hook) {
        Wh_Log(L"AdaptiveDesktopIconLayout: SetWindowsHookExW(WH_GETMESSAGE) failed: %u",
               GetLastError());
        return false;
    }
    g_desktopThreadId = threadId;
    g_desktopGetMessageHook = hook;
    Wh_Log(L"AdaptiveDesktopIconLayout: attached desktop UI thread message hook tid=%u",
           threadId);
    return true;
}

LRESULT CALLBACK DesktopListViewSubclassProc(HWND hwnd,
                                              UINT message,
                                              WPARAM wParam,
                                              LPARAM lParam,
                                              DWORD_PTR) {
    LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);

    if (message == WM_NCDESTROY && g_desktopListView == hwnd) {
        g_desktopListView = nullptr;
        g_refreshReadyArmed = false;
        g_readyRequestKind = CompactRequestKind::None;
        return result;
    }

    if (!g_environmentTransitionActive && !IsRemoteSession() &&
        (message == WM_LBUTTONUP ||
         message == WM_RBUTTONUP ||
         message == WM_KEYUP)) {
        RefreshStableLocalBaseline();
    }

    if (g_refreshReadyArmed && !g_refreshReadyProbe) {
        TryScheduleCompactWhenDesktopReady();
    }

    return result;
}

LRESULT CALLBACK DesktopHostRootSubclassProc(HWND hwnd,
                                              UINT message,
                                              WPARAM wParam,
                                              LPARAM lParam,
                                              DWORD_PTR) {
    if (message == WM_TIMER) {
        const UINT_PTR timerId = static_cast<UINT_PTR>(wParam);
        if (timerId == kStableBaselineTimer) {
            RefreshStableLocalBaseline();
            return 0;
        }
        if (timerId == kEnvironmentSettleTimer) {
            HandleEnvironmentSettleTimer(hwnd);
            return 0;
        }
    }

    const bool f5RefreshCommand =
        message == WM_COMMAND &&
        LOWORD(wParam) == kDesktopBrowserRefreshCommand &&
        g_f5RefreshArmed;

    DWORD currentSessionId = 0;
    const bool haveCurrentSessionId =
        TryGetCurrentProcessSessionId(&currentSessionId);

    const bool remoteConnectEvent =
        message == WM_WTSSESSION_CHANGE &&
        wParam == WTS_REMOTE_CONNECT &&
        haveCurrentSessionId &&
        static_cast<DWORD>(lParam) == currentSessionId;

    const bool remoteDisconnectEvent =
        message == WM_WTSSESSION_CHANGE &&
        wParam == WTS_REMOTE_DISCONNECT &&
        haveCurrentSessionId &&
        static_cast<DWORD>(lParam) == currentSessionId;

    const bool environmentSignal =
        message == WM_SETTINGCHANGE ||
        message == WM_DISPLAYCHANGE ||
        message == WM_DPICHANGED ||
        message == WM_SIZE;

    const bool hostDestroying =
        message == WM_NCDESTROY && g_desktopHostRoot == hwnd;

    if (remoteConnectEvent) {
        StartOrExtendEnvironmentTransition(
            hwnd, EnvironmentTransitionReason::RemoteConnect);
    } else if (remoteDisconnectEvent) {
        StartOrExtendEnvironmentTransition(
            hwnd, EnvironmentTransitionReason::RemoteDisconnect);
    } else if (environmentSignal) {
        StartOrExtendEnvironmentTransition(
            hwnd,
            (g_rdpOrderActive || IsRemoteSession())
                ? EnvironmentTransitionReason::RemoteDisplayChange
                : EnvironmentTransitionReason::LocalDisplayChange);
    }

    if (hostDestroying) {
        UnregisterDesktopSessionNotifications(hwnd);
        KillTimer(hwnd, kStableBaselineTimer);
        KillTimer(hwnd, kEnvironmentSettleTimer);
    }

    if (f5RefreshCommand) {
        FreezeManualRefreshOrder();
    }

    LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);

    if (f5RefreshCommand) {
        g_f5RefreshArmed = false;
        HWND defView = g_desktopDefView;
        if (!defView || !IsWindow(defView)) {
            defView = FindDesktopDefView();
        }
        if (defView) {
            Wh_Log(L"AdaptiveDesktopIconLayout: confirmed desktop F5 refresh (Progman WM_COMMAND id=0x%04X code=0x%04X)",
                   LOWORD(wParam), HIWORD(wParam));
            ArmCompactWhenDesktopReady();
        }
    }

    if (hostDestroying) {
        g_desktopHostRoot = nullptr;
        FinishEnvironmentTransition(nullptr);
    }
    return result;
}

bool AttachToDesktopListView(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }
    wchar_t className[64]{};
    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className)) ||
        _wcsicmp(className, L"SysListView32") != 0 ||
        !IsDesktopDefView(GetParent(hwnd))) {
        return false;
    }
    if (g_desktopListView == hwnd) {
        return true;
    }
    if (!WindhawkUtils::SetWindowSubclassFromAnyThread(
            hwnd, DesktopListViewSubclassProc, 0)) {
        Wh_Log(L"AdaptiveDesktopIconLayout: failed to subclass desktop FolderView %p", hwnd);
        return false;
    }
    g_desktopListView = hwnd;
    Wh_Log(L"AdaptiveDesktopIconLayout: attached to desktop FolderView %p", hwnd);
    InstallDesktopThreadMessageHook();
    return true;
}

bool AttachToDesktopHostRoot(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsDesktopHostRoot(hwnd)) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != GetCurrentProcessId()) {
        return false;
    }
    if (g_desktopHostRoot == hwnd) {
        if (g_sessionNotificationWindow != hwnd) {
            RegisterDesktopSessionNotifications(hwnd);
        }
        SetTimer(hwnd, kStableBaselineTimer, kStableBaselineIntervalMs, nullptr);
        return true;
    }
    if (!WindhawkUtils::SetWindowSubclassFromAnyThread(
            hwnd, DesktopHostRootSubclassProc, 0)) {
        Wh_Log(L"AdaptiveDesktopIconLayout: failed to subclass desktop host root %p", hwnd);
        return false;
    }
    g_desktopHostRoot = hwnd;
    Wh_Log(L"AdaptiveDesktopIconLayout: attached to desktop host root %p", hwnd);
    RegisterDesktopSessionNotifications(hwnd);
    SetTimer(hwnd, kStableBaselineTimer, kStableBaselineIntervalMs, nullptr);
    return true;
}

LRESULT CALLBACK DesktopDefViewSubclassProc(HWND hwnd,
                                             UINT message,
                                             WPARAM wParam,
                                             LPARAM lParam,
                                             DWORD_PTR) {
    if (message == kCompactAfterRefreshMessage) {
        RemovePropW(hwnd, kCompactPendingProperty);
        CompactRequestKind kind = g_pendingCompactKind;
        g_pendingCompactKind = CompactRequestKind::None;

        HRESULT hr = S_FALSE;
        if (kind == CompactRequestKind::ManualRefresh) {
            const bool remoteSession = IsRemoteSession();
            const std::vector<std::wstring>* forcedOrder = nullptr;
            if (g_environmentTransitionActive &&
                !g_environmentFrozenOrder.empty()) {
                forcedOrder = &g_environmentFrozenOrder;
            } else if (remoteSession &&
                       g_rdpOrderActive &&
                       !g_rdpLogicalOrder.empty()) {
                forcedOrder = &g_rdpLogicalOrder;
            } else if (!remoteSession &&
                       g_manualRefreshOrderActive &&
                       !g_manualRefreshFrozenOrder.empty()) {
                forcedOrder = &g_manualRefreshFrozenOrder;
            }

            hr = CompactDesktop(
                remoteSession,
                forcedOrder,
                !remoteSession && !g_environmentTransitionActive,
                remoteSession ? L"RDP-manual-refresh"
                              : (g_environmentTransitionActive
                                     ? L"transition-manual-refresh"
                                     : L"console-manual-refresh"));

            if (SUCCEEDED(hr) && !remoteSession &&
                !g_environmentTransitionActive) {
                RefreshStableLocalBaseline();
            }

            ClearManualRefreshOrder();
        }

        if (FAILED(hr) && hr != S_FALSE) {
            Wh_Log(L"AdaptiveDesktopIconLayout: post-refresh action failed: 0x%08X", hr);
        }
        return 0;
    }

    const bool contextMenuRefreshCommand =
        message == WM_COMMAND &&
        LOWORD(wParam) == kDefViewRefreshCommand &&
        lParam == 0;

    if (contextMenuRefreshCommand) {
        FreezeManualRefreshOrder();
    }

    LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);

    if (contextMenuRefreshCommand) {
        Wh_Log(L"AdaptiveDesktopIconLayout: confirmed desktop context-menu Refresh (DefView WM_COMMAND id=0x%04X)",
               LOWORD(wParam));
        ArmCompactWhenDesktopReady();
    }

    if (message == WM_NCDESTROY && g_desktopDefView == hwnd) {
        g_desktopDefView = nullptr;
        g_refreshReadyArmed = false;
        g_readyRequestKind = CompactRequestKind::None;
        ClearManualRefreshOrder();
    }
    return result;
}

bool AttachToDesktopDefView(HWND hwnd) {
    if (!IsDesktopDefView(hwnd)) {
        return false;
    }
    if (g_desktopDefView == hwnd) {
        return true;
    }
    if (!WindhawkUtils::SetWindowSubclassFromAnyThread(
            hwnd, DesktopDefViewSubclassProc, 0)) {
        Wh_Log(L"AdaptiveDesktopIconLayout: failed to subclass desktop DefView %p", hwnd);
        return false;
    }
    g_desktopDefView = hwnd;
    Wh_Log(L"AdaptiveDesktopIconLayout: attached to desktop DefView %p", hwnd);

    if (HWND listView = FindWindowExW(hwnd, nullptr, L"SysListView32", nullptr)) {
        AttachToDesktopListView(listView);
    }
    if (HWND shell = GetShellWindow()) {
        AttachToDesktopHostRoot(shell);
    }
    return true;
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
static CreateWindowExW_t CreateWindowExW_Original = nullptr;

HWND WINAPI CreateWindowExW_Hook(DWORD exStyle,
                                 LPCWSTR className,
                                 LPCWSTR windowName,
                                 DWORD style,
                                 int x,
                                 int y,
                                 int width,
                                 int height,
                                 HWND parent,
                                 HMENU menu,
                                 HINSTANCE instance,
                                 LPVOID param) {
    HWND hwnd = CreateWindowExW_Original(exStyle, className, windowName, style,
                                         x, y, width, height, parent, menu,
                                         instance, param);
    if (hwnd) {
        wchar_t actualClass[64]{};
        if (GetClassNameW(hwnd, actualClass, ARRAYSIZE(actualClass))) {
            if (_wcsicmp(actualClass, L"SHELLDLL_DefView") == 0) {
                AttachToDesktopDefView(hwnd);
            } else if (_wcsicmp(actualClass, L"SysListView32") == 0 &&
                       IsDesktopDefView(parent)) {
                AttachToDesktopListView(hwnd);
            }
        }
    }
    return hwnd;
}

}  // namespace drcwin

BOOL Wh_ModInit() {
    Wh_Log(L"AdaptiveDesktopIconLayout: init");
    HWND shellWindow = GetShellWindow();
    DWORD shellProcessId = 0;
    if (!shellWindow ||
        !GetWindowThreadProcessId(shellWindow, &shellProcessId) ||
        shellProcessId != GetCurrentProcessId()) {
        Wh_Log(L"AdaptiveDesktopIconLayout: not the desktop shell explorer process; skipping");
        return FALSE;
    }
    if (!WindhawkUtils::SetFunctionHook(CreateWindowExW,
                                        drcwin::CreateWindowExW_Hook,
                                        &drcwin::CreateWindowExW_Original)) {
        Wh_Log(L"AdaptiveDesktopIconLayout: failed to hook CreateWindowExW");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    if (HWND shell = GetShellWindow()) {
        drcwin::AttachToDesktopHostRoot(shell);
    }
    if (HWND hwnd = drcwin::FindDesktopDefView()) {
        drcwin::AttachToDesktopDefView(hwnd);
    } else {
        Wh_Log(L"AdaptiveDesktopIconLayout: desktop DefView not found during after-init; waiting for creation");
    }

    if (!drcwin::InstallDesktopThreadMessageHook()) {
        Wh_Log(L"AdaptiveDesktopIconLayout: desktop thread message hook installation failed");
    }

    if (drcwin::IsRemoteSession()) {
        Wh_Log(L"AdaptiveDesktopIconLayout: initialized inside RDP; starting environment transition from persisted console order");
        if (drcwin::g_desktopHostRoot && IsWindow(drcwin::g_desktopHostRoot)) {
            drcwin::StartOrExtendEnvironmentTransition(
                drcwin::g_desktopHostRoot,
                drcwin::EnvironmentTransitionReason::RemoteConnect);
        }
    } else {
        drcwin::RefreshStableLocalBaseline();
    }
}

void Wh_ModUninit() {
    Wh_Log(L"AdaptiveDesktopIconLayout: uninit");

    if (drcwin::g_desktopGetMessageHook) {
        UnhookWindowsHookEx(drcwin::g_desktopGetMessageHook);
        drcwin::g_desktopGetMessageHook = nullptr;
        drcwin::g_desktopThreadId = 0;
    }

    drcwin::g_f5RefreshArmed = false;
    drcwin::g_refreshReadyArmed = false;
    drcwin::g_readyRequestKind = drcwin::CompactRequestKind::None;
    drcwin::g_pendingCompactKind = drcwin::CompactRequestKind::None;
    drcwin::ClearManualRefreshOrder();

    drcwin::g_lastStableLocalOrder.clear();
    drcwin::g_haveLastStableLocalOrder = false;
    drcwin::g_haveLastStableEnvironmentSignature = false;
    drcwin::g_environmentFrozenOrder.clear();
    drcwin::g_environmentTransitionActive = false;
    drcwin::g_environmentTransitionReason =
        drcwin::EnvironmentTransitionReason::None;
    drcwin::g_rdpLogicalOrder.clear();
    drcwin::g_rdpOrderActive = false;

    drcwin::UnregisterDesktopSessionNotifications();

    HWND listView = drcwin::g_desktopListView;
    if (listView && IsWindow(listView)) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(
            listView, drcwin::DesktopListViewSubclassProc);
    }
    drcwin::g_desktopListView = nullptr;

    HWND root = drcwin::g_desktopHostRoot;
    if (root && IsWindow(root)) {
        KillTimer(root, drcwin::kStableBaselineTimer);
        KillTimer(root, drcwin::kEnvironmentSettleTimer);
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(
            root, drcwin::DesktopHostRootSubclassProc);
    }
    drcwin::g_desktopHostRoot = nullptr;

    HWND hwnd = drcwin::g_desktopDefView;
    if (hwnd && IsWindow(hwnd)) {
        RemovePropW(hwnd, drcwin::kCompactPendingProperty);
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(
            hwnd, drcwin::DesktopDefViewSubclassProc);
    }
    drcwin::g_desktopDefView = nullptr;
}

#endif
