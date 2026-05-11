#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>       // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>
#include <iphlpapi.h>       // transitively pulls in netioapi.h
#include <wlanapi.h>
#include <netlistmgr.h>     // INetworkListManager
#include <ocidl.h>          // IConnectionPointContainer
#include <IPTypes.h>

#include <string>
#include <vector>

#include "resource.h"

// =============================================================================
// Constants
// =============================================================================

constexpr UINT WM_TRAY_CALLBACK = WM_APP + 1;
constexpr UINT WM_RECOMPUTE     = WM_APP + 2;

constexpr wchar_t kWindowClassName[] = L"NetworkTrayHiddenWindow";
constexpr wchar_t kAppMutexName[]    = L"Local\\NetworkTray-{a1b2c3d4-e5f6-7890-1234-567890abcdef}";


static constexpr GUID kTrayIconGuid =
    { 0xa1b2c3d4, 0xe5f6, 0x7890,
      { 0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xcd, 0xef } };

// =============================================================================
// State types
// =============================================================================

enum class InterfaceType { None, Ethernet, WiFi };
enum class ConnectivityState { Disconnected, Limited, Internet };

struct NetworkState {
    InterfaceType     type         = InterfaceType::None;
    ConnectivityState connectivity = ConnectivityState::Disconnected;
    int               signalQuality = 0;     // 0..100, Wi-Fi only
    std::wstring      ssid;                   // Wi-Fi only
    std::wstring      adapterName;
    bool              radioOff      = false;  // Wi-Fi radio disabled / no adapter

    bool operator==(const NetworkState& o) const {
        return type == o.type
            && connectivity == o.connectivity
            && signalQuality == o.signalQuality
            && ssid == o.ssid
            && adapterName == o.adapterName
            && radioOff == o.radioOff;
    }
    bool operator!=(const NetworkState& o) const { return !(*this == o); }
};

// =============================================================================
// Globals
// =============================================================================

namespace g {
    HINSTANCE                hInstance        = nullptr;
    HWND                     hWnd             = nullptr;
    UINT                     taskbarCreatedMsg = 0;

    HANDLE                   wlanHandle       = nullptr;
    HANDLE                   ipChangeHandle   = nullptr;

    INetworkListManager*     nlm              = nullptr;
    IConnectionPoint*        nlmConnPoint     = nullptr;
    DWORD                    nlmCookie        = 0;

    NetworkState             currentState;
}

// =============================================================================
// NLM event sink
// =============================================================================

class NlmEventSink : public INetworkListManagerEvents {
    LONG m_ref = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_INetworkListManagerEvents) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP ConnectivityChanged(NLM_CONNECTIVITY /*newConnectivity*/) override {
        if (g::hWnd) PostMessageW(g::hWnd, WM_RECOMPUTE, 0, 0);
        return S_OK;
    }
};

// =============================================================================
// Worker-thread notification callbacks — only post; do not touch shared state
// =============================================================================

static void CALLBACK WlanNotificationCallback(PWLAN_NOTIFICATION_DATA, PVOID) {
    if (g::hWnd) PostMessageW(g::hWnd, WM_RECOMPUTE, 0, 0);
}

static void CALLBACK IpInterfaceChangeCallback(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
    if (g::hWnd) PostMessageW(g::hWnd, WM_RECOMPUTE, 0, 0);
}

// =============================================================================
// State computation
// =============================================================================

// Pick the "best" interface index that would carry traffic to the internet.
// Falls back to 0 (caller will then try any-up adapter).
static DWORD FindBestInterfaceIndex() {
    SOCKADDR_IN dest = {};
    dest.sin_family = AF_INET;
    InetPtonW(AF_INET, L"8.8.8.8", &dest.sin_addr);
    DWORD idx = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&dest), &idx) != NO_ERROR) return 0;
    return idx;
}

static void FillWiFiDetails(NetworkState& state) {
    if (!g::wlanHandle) return;

    PWLAN_INTERFACE_INFO_LIST ifList = nullptr;
    if (WlanEnumInterfaces(g::wlanHandle, nullptr, &ifList) != ERROR_SUCCESS || !ifList) {
        return;
    }

    bool anyConnected = false;
    for (DWORD i = 0; i < ifList->dwNumberOfItems; ++i) {
        const auto& info = ifList->InterfaceInfo[i];
        if (info.isState != wlan_interface_state_connected) continue;
        anyConnected = true;

        DWORD dataSize = 0;
        PVOID data = nullptr;
        WLAN_OPCODE_VALUE_TYPE opType = wlan_opcode_value_type_invalid;
        if (WlanQueryInterface(g::wlanHandle, &info.InterfaceGuid,
                wlan_intf_opcode_current_connection, nullptr,
                &dataSize, &data, &opType) != ERROR_SUCCESS || !data) {
            continue;
        }
        auto* attrs = reinterpret_cast<WLAN_CONNECTION_ATTRIBUTES*>(data);
        state.signalQuality = static_cast<int>(attrs->wlanAssociationAttributes.wlanSignalQuality);

        const auto& ssid = attrs->wlanAssociationAttributes.dot11Ssid;
        if (ssid.uSSIDLength > 0) {
            int n = MultiByteToWideChar(CP_UTF8, 0,
                reinterpret_cast<LPCSTR>(ssid.ucSSID),
                static_cast<int>(ssid.uSSIDLength), nullptr, 0);
            if (n > 0) {
                state.ssid.assign(n, L'\0');
                MultiByteToWideChar(CP_UTF8, 0,
                    reinterpret_cast<LPCSTR>(ssid.ucSSID),
                    static_cast<int>(ssid.uSSIDLength),
                    state.ssid.data(), n);
            }
        }
        WlanFreeMemory(data);
        break;
    }

    if (!anyConnected) {
        // Wi-Fi adapter present but not associated. Treat as no Wi-Fi for icon
        // purposes; the caller will fall back to other interfaces or off.
        state.type = InterfaceType::None;
        state.radioOff = true;
    }
    WlanFreeMemory(ifList);
}

static NetworkState ComputeNetworkState() {
    NetworkState state;

    // 1. Overall connectivity from NLM
    NLM_CONNECTIVITY conn = NLM_CONNECTIVITY_DISCONNECTED;
    if (g::nlm) g::nlm->GetConnectivity(&conn);

    constexpr int kInternetMask =
        NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET;
    constexpr int kLocalMask =
        NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_LOCALNETWORK
      | NLM_CONNECTIVITY_IPV4_SUBNET      | NLM_CONNECTIVITY_IPV6_SUBNET
      | NLM_CONNECTIVITY_IPV4_NOTRAFFIC   | NLM_CONNECTIVITY_IPV6_NOTRAFFIC;

    if (conn & kInternetMask)       state.connectivity = ConnectivityState::Internet;
    else if (conn & kLocalMask)     state.connectivity = ConnectivityState::Limited;
    else                            state.connectivity = ConnectivityState::Disconnected;

    // 2. Find the primary outbound interface and classify it
    DWORD bestIfIndex = FindBestInterfaceIndex();

    ULONG bufLen = 16 * 1024;
    std::vector<BYTE> buf;
    PIP_ADAPTER_ADDRESSES adapters = nullptr;
    DWORD rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf.resize(bufLen);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        rc = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &bufLen);
    }

    auto classify = [&](PIP_ADAPTER_ADDRESSES a) {
        if (a->IfType == IF_TYPE_ETHERNET_CSMACD) state.type = InterfaceType::Ethernet;
        else if (a->IfType == IF_TYPE_IEEE80211)  state.type = InterfaceType::WiFi;
        if (a->FriendlyName) state.adapterName = a->FriendlyName;
    };

    if (rc == NO_ERROR && adapters) {
        PIP_ADAPTER_ADDRESSES matched = nullptr;
        PIP_ADAPTER_ADDRESSES anyUpEth = nullptr;
        PIP_ADAPTER_ADDRESSES anyUpWifi = nullptr;
        for (auto* a = adapters; a; a = a->Next) {
            const bool isEth  = (a->IfType == IF_TYPE_ETHERNET_CSMACD);
            const bool isWifi = (a->IfType == IF_TYPE_IEEE80211);
            if (!isEth && !isWifi) continue;
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfIndex == bestIfIndex) { matched = a; break; }
            if (isEth  && !anyUpEth)  anyUpEth  = a;
            if (isWifi && !anyUpWifi) anyUpWifi = a;
        }
        if      (matched)     classify(matched);
        else if (anyUpEth)    classify(anyUpEth);
        else if (anyUpWifi)   classify(anyUpWifi);
    }

    // 3. If we picked Wi-Fi, fetch signal + SSID
    if (state.type == InterfaceType::WiFi) {
        FillWiFiDetails(state);
    }

    return state;
}

// =============================================================================
// Icon + tooltip selection
// =============================================================================

static int IconResourceForState(const NetworkState& s) {
    if (s.type == InterfaceType::WiFi) {
        int bars = s.signalQuality / 20;
        if (bars < 0) bars = 0;
        if (bars > 4) bars = 4;
        switch (s.connectivity) {
            case ConnectivityState::Internet: return IDI_WIFI_0 + bars;
            case ConnectivityState::Limited:  return IDI_WIFI_0_LIMITED + bars;
            default:                          return IDI_WIFI_OFF;
        }
    }
    if (s.type == InterfaceType::Ethernet) {
        switch (s.connectivity) {
            case ConnectivityState::Internet: return IDI_ETHERNET;
            case ConnectivityState::Limited:  return IDI_ETHERNET_LIMITED;
            default:                          return IDI_ETHERNET_UNPLUGGED;
        }
    }
    return IDI_DISCONNECTED;
}

static std::wstring TooltipForState(const NetworkState& s) {
    wchar_t buf[128] = {};
    auto suffix = [&](ConnectivityState c) -> const wchar_t* {
        switch (c) {
            case ConnectivityState::Internet: return L"";
            case ConnectivityState::Limited:  return L" — No internet";
            default:                          return L" — Disconnected";
        }
    };
    switch (s.type) {
    case InterfaceType::WiFi:
        if (s.ssid.empty())
            swprintf_s(buf, L"Wi-Fi%s", suffix(s.connectivity));
        else
            swprintf_s(buf, L"%s (%d%%)%s", s.ssid.c_str(), s.signalQuality,
                       suffix(s.connectivity));
        break;
    case InterfaceType::Ethernet:
        swprintf_s(buf, L"Ethernet: %s",
            s.connectivity == ConnectivityState::Internet ? L"Connected" :
            s.connectivity == ConnectivityState::Limited  ? L"No internet" :
                                                             L"Unplugged");
        break;
    default:
        wcscpy_s(buf, L"Not connected");
        break;
    }
    return buf;
}

// =============================================================================
// Tray icon management
// =============================================================================

static void FillNotifyIconData(NOTIFYICONDATAW& nid) {
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g::hWnd;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.guidItem         = kTrayIconGuid;
}

static void AddTrayIcon() {
    NOTIFYICONDATAW nid;
    FillNotifyIconData(nid);
    nid.hIcon = LoadIconW(g::hInstance, MAKEINTRESOURCEW(IDI_DISCONNECTED));
    wcscpy_s(nid.szTip, L"Network");
    Shell_NotifyIconW(NIM_ADD, &nid);

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void UpdateTrayIcon(const NetworkState& state) {
    int iconId = IconResourceForState(state);
    HICON hIcon = static_cast<HICON>(LoadImageW(g::hInstance, MAKEINTRESOURCEW(iconId),
        IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    if (!hIcon) hIcon = LoadIconW(g::hInstance, MAKEINTRESOURCEW(IDI_DISCONNECTED));

    NOTIFYICONDATAW nid;
    FillNotifyIconData(nid);
    nid.hIcon = hIcon;

    std::wstring tip = TooltipForState(state);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void RemoveTrayIcon() {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize   = sizeof(nid);
    nid.hWnd     = g::hWnd;
    nid.uFlags   = NIF_GUID;
    nid.guidItem = kTrayIconGuid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// =============================================================================
// Recompute & dispatch
// =============================================================================

static void Recompute() {
    NetworkState s = ComputeNetworkState();
    if (s != g::currentState) {
        g::currentState = std::move(s);
        UpdateTrayIcon(g::currentState);
    }
}

// =============================================================================
// Context menu
// =============================================================================

static void OpenWiFiFlyout() {
    // ms-availablenetworks: was the Win10 quick network flyout; in Win11 it
    // generally lands on the Wi-Fi quick settings. Fall back to the Settings
    // page if the URI is not registered on this build.
    auto r = ShellExecuteW(nullptr, L"open", L"ms-availablenetworks:",
                           nullptr, nullptr, SW_SHOW);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        ShellExecuteW(nullptr, L"open", L"ms-settings:network-wifi",
                      nullptr, nullptr, SW_SHOW);
    }
}

static void ShowContextMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IDM_OPEN_NETWORK_SETTINGS, L"Network &settings");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_NCPA,             L"Network &connections");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_REFRESH,               L"&Refresh");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT,                  L"E&xit");

    SetForegroundWindow(g::hWnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, g::hWnd, nullptr);
    PostMessageW(g::hWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// =============================================================================
// Window procedure
// =============================================================================

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g::taskbarCreatedMsg && g::taskbarCreatedMsg != 0) {
        // Explorer restarted; re-add our icon.
        AddTrayIcon();
        UpdateTrayIcon(g::currentState);
        return 0;
    }

    switch (msg) {
    case WM_RECOMPUTE:
        Recompute();
        return 0;

    case WM_TRAY_CALLBACK: {
        // NOTIFYICON_VERSION_4 packs the event in LOWORD(lParam) and the screen
        // position of the click in wParam.
        UINT event = LOWORD(lParam);
        switch (event) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONUP:
            OpenWiFiFlyout();
            break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP: {
            POINT pt = { GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) };
            if (pt.x == 0 && pt.y == 0) GetCursorPos(&pt);
            ShowContextMenu(pt);
            break;
        }
        default:
            break;
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_OPEN_NETWORK_SETTINGS:
            ShellExecuteW(nullptr, L"open", L"ms-settings:network",
                          nullptr, nullptr, SW_SHOW);
            break;
        case IDM_OPEN_NCPA:
            ShellExecuteW(nullptr, nullptr, L"ncpa.cpl", nullptr, nullptr, SW_SHOW);
            break;
        case IDM_REFRESH:
            Recompute();
            break;
        case IDM_EXIT:
            RemoveTrayIcon();
            DestroyWindow(g::hWnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// =============================================================================
// Subsystem init / teardown
// =============================================================================

static bool InitNlm() {
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
            IID_INetworkListManager, reinterpret_cast<void**>(&g::nlm)))) {
        return false;
    }
    IConnectionPointContainer* cpc = nullptr;
    if (FAILED(g::nlm->QueryInterface(IID_IConnectionPointContainer,
            reinterpret_cast<void**>(&cpc)))) {
        return false;
    }
    HRESULT hr = cpc->FindConnectionPoint(IID_INetworkListManagerEvents,
                                          &g::nlmConnPoint);
    cpc->Release();
    if (FAILED(hr)) return false;

    auto* sink = new NlmEventSink();
    hr = g::nlmConnPoint->Advise(sink, &g::nlmCookie);
    sink->Release();  // ConnectionPoint holds the surviving reference.
    return SUCCEEDED(hr);
}

static void ShutdownNlm() {
    if (g::nlmConnPoint && g::nlmCookie) {
        g::nlmConnPoint->Unadvise(g::nlmCookie);
        g::nlmCookie = 0;
    }
    if (g::nlmConnPoint) { g::nlmConnPoint->Release(); g::nlmConnPoint = nullptr; }
    if (g::nlm)          { g::nlm->Release();          g::nlm          = nullptr; }
}

static bool InitWlan() {
    DWORD negotiatedVersion = 0;
    if (WlanOpenHandle(2, nullptr, &negotiatedVersion, &g::wlanHandle) != ERROR_SUCCESS) {
        g::wlanHandle = nullptr;
        return false;  // WLAN service may be stopped or no adapter present.
    }
    WlanRegisterNotification(g::wlanHandle, WLAN_NOTIFICATION_SOURCE_ALL, TRUE,
        WlanNotificationCallback, nullptr, nullptr, nullptr);
    return true;
}

static void ShutdownWlan() {
    if (g::wlanHandle) {
        WlanRegisterNotification(g::wlanHandle, WLAN_NOTIFICATION_SOURCE_NONE,
            TRUE, nullptr, nullptr, nullptr, nullptr);
        WlanCloseHandle(g::wlanHandle, nullptr);
        g::wlanHandle = nullptr;
    }
}

static bool InitIpChange() {
    return NotifyIpInterfaceChange(AF_UNSPEC, IpInterfaceChangeCallback,
        nullptr, FALSE, &g::ipChangeHandle) == NO_ERROR;
}

static void ShutdownIpChange() {
    if (g::ipChangeHandle) {
        CancelMibChangeNotify2(g::ipChangeHandle);
        g::ipChangeHandle = nullptr;
    }
}

// =============================================================================
// Entry point
// =============================================================================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g::hInstance = hInstance;

    // Single-instance guard.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kAppMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    // STA: NLM connection points + window message loop.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        CloseHandle(mutex);
        return 1;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g::taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kWindowClassName;
    if (!RegisterClassExW(&wc)) {
        CoUninitialize();
        CloseHandle(mutex);
        return 1;
    }

    // Message-only window — never visible, never receives input focus.
    g::hWnd = CreateWindowExW(0, kWindowClassName, L"", 0,
        0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!g::hWnd) {
        CoUninitialize();
        CloseHandle(mutex);
        return 1;
    }

    InitNlm();
    InitWlan();
    InitIpChange();

    AddTrayIcon();
    Recompute();  // Seed initial state.

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ShutdownIpChange();
    ShutdownWlan();
    ShutdownNlm();
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
