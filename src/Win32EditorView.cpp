#include "Win32EditorView.h"
#include "StreamAudioPlugin.h"
#include "ConfigStore.h"
#include "Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace StreamAudio {

static const Steinberg::ViewRect kDefaultRect(0, 0, 420, 260);
static const char* const kEditorClassName = "StreamAudioPlugin_EditorWindow";

static HINSTANCE getPluginInstance() {
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&getPluginInstance),
            &hMod)) {
        return hMod;
    }
    return nullptr;
}

Win32EditorView::Win32EditorView(StreamAudioPlugin* plugin)
    : Steinberg::CPluginView(&kDefaultRect), plugin_(plugin) {
    rect = kDefaultRect;
    if (plugin_) {
        plugin_->addRef();
    }
}

Win32EditorView::~Win32EditorView() {
    if (hwnd_) {
        KillTimer(hwnd_, kTimerId);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    cleanupGdi();
    if (plugin_) {
        plugin_->release();
        plugin_ = nullptr;
    }
}

Steinberg::tresult PLUGIN_API Win32EditorView::isPlatformTypeSupported(Steinberg::FIDString type) {
    if (type && std::strcmp(type, Steinberg::kPlatformTypeHWND) == 0) {
        return Steinberg::kResultTrue;
    }
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Win32EditorView::getSize(Steinberg::ViewRect* size) {
    if (!size) {
        return Steinberg::kInvalidArgument;
    }
    *size = rect;
    return Steinberg::kResultTrue;
}

void Win32EditorView::registerEditorClass(HINSTANCE hInst) {
    WNDCLASSEXA existing{};
    if (!GetClassInfoExA(hInst, kEditorClassName, &existing)) {
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Win32EditorView::wndProcStatic;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = hInst;
        wc.hIcon = nullptr;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszMenuName = nullptr;
        wc.lpszClassName = kEditorClassName;
        wc.hIconSm = nullptr;
        RegisterClassExA(&wc);
    }
}

void Win32EditorView::initGdi() {
    if (!hFontTitle_) {
        hFontTitle_ = CreateFontA(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    }
    if (!hFontUi_) {
        hFontUi_ = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    }
    if (!hFontSmall_) {
        hFontSmall_ = CreateFontA(-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    }
    if (!hFontSmallBold_) {
        hFontSmallBold_ = CreateFontA(-10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    }
    if (!hFontPill_) {
        hFontPill_ = CreateFontA(-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    }

    if (!hEditBrush_) {
        hEditBrush_ = CreateSolidBrush(RGB(0x18, 0x1A, 0x1F));
    }
    if (!hCardBrush_) {
        hCardBrush_ = CreateSolidBrush(RGB(0x24, 0x28, 0x30));
    }
    if (!hBgBrush_) {
        hBgBrush_ = CreateSolidBrush(RGB(0x18, 0x1A, 0x1F));
    }
}

void Win32EditorView::cleanupGdi() {
    if (hFontTitle_) { DeleteObject(hFontTitle_); hFontTitle_ = nullptr; }
    if (hFontUi_) { DeleteObject(hFontUi_); hFontUi_ = nullptr; }
    if (hFontSmall_) { DeleteObject(hFontSmall_); hFontSmall_ = nullptr; }
    if (hFontSmallBold_) { DeleteObject(hFontSmallBold_); hFontSmallBold_ = nullptr; }
    if (hFontPill_) { DeleteObject(hFontPill_); hFontPill_ = nullptr; }

    if (hEditBrush_) { DeleteObject(hEditBrush_); hEditBrush_ = nullptr; }
    if (hCardBrush_) { DeleteObject(hCardBrush_); hCardBrush_ = nullptr; }
    if (hBgBrush_) { DeleteObject(hBgBrush_); hBgBrush_ = nullptr; }
}

LRESULT CALLBACK Win32EditorView::wndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<Win32EditorView*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (self) {
        return self->wndProc(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK Win32EditorView::editSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto origProc = reinterpret_cast<WNDPROC>(GetPropA(hwnd, "OrigWndProc"));
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        HWND parent = GetParent(hwnd);
        if (parent) {
            SendMessageA(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), EN_KILLFOCUS), reinterpret_cast<LPARAM>(hwnd));
        }
        return 0;
    }
    if (origProc) {
        return CallWindowProcA(origProc, hwnd, uMsg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK Win32EditorView::comboEditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto origProc = reinterpret_cast<WNDPROC>(GetPropA(hwnd, "OrigWndProc"));
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        HWND combo = GetParent(hwnd);
        if (combo) {
            HWND parent = GetParent(combo);
            if (parent) {
                SendMessageA(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_KILLFOCUS), reinterpret_cast<LPARAM>(combo));
            }
        }
        return 0;
    }
    if (origProc) {
        return CallWindowProcA(origProc, hwnd, uMsg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

void Win32EditorView::createChildControls(HINSTANCE hInst) {
    if (!hwnd_) return;

    // Enabled checkbox
    hwndEnabled_ = CreateWindowExA(
        0, "BUTTON", "Enabled",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        26, 54, 85, 22,
        hwnd_, reinterpret_cast<HMENU>(kIdEnabled), hInst, nullptr
    );

    // Send radio
    hwndSend_ = CreateWindowExA(
        0, "BUTTON", "Send (TX)",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
        125, 54, 90, 22,
        hwnd_, reinterpret_cast<HMENU>(kIdSend), hInst, nullptr
    );

    // Receive radio
    hwndReceive_ = CreateWindowExA(
        0, "BUTTON", "Receive (RX)",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_TABSTOP,
        225, 54, 110, 22,
        hwnd_, reinterpret_cast<HMENU>(kIdReceive), hInst, nullptr
    );

    // Identifier edit box
    hwndIdentifier_ = CreateWindowExA(
        WS_EX_CLIENTEDGE, "EDIT", "default",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
        110, 88, 280, 22,
        hwnd_, reinterpret_cast<HMENU>(kIdIdentifier), hInst, nullptr
    );
    SendMessageA(hwndIdentifier_, EM_SETLIMITTEXT, 31, 0);

    // Destination editable combobox
    hwndDestination_ = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
        110, 122, 280, 160,
        hwnd_, reinterpret_cast<HMENU>(kIdDestination), hInst, nullptr
    );

    // Channels combobox
    hwndChannels_ = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        110, 156, 120, 160,
        hwnd_, reinterpret_cast<HMENU>(kIdChannels), hInst, nullptr
    );

    // Set fonts for all child controls
    HWND controls[] = {
        hwndEnabled_, hwndSend_, hwndReceive_,
        hwndIdentifier_, hwndDestination_, hwndChannels_
    };
    for (HWND c : controls) {
        if (c && hFontUi_) {
            SendMessageA(c, WM_SETFONT, reinterpret_cast<WPARAM>(hFontUi_), TRUE);
        }
    }

    // Populate channels dropdown (1..8)
    for (int i = 1; i <= 8; ++i) {
        char buf[32];
        if (i == 2) {
            snprintf(buf, sizeof(buf), "2 ch (Stereo)");
        } else if (i == 6) {
            snprintf(buf, sizeof(buf), "6 ch (5.1)");
        } else if (i == 8) {
            snprintf(buf, sizeof(buf), "8 ch (7.1)");
        } else {
            snprintf(buf, sizeof(buf), "%d ch", i);
        }
        SendMessageA(hwndChannels_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }

    // Populate destination dropdown
    refreshDestinationDropdown();

    // Subclass Edit child of hwndDestination_ to catch Enter key
    HWND hwndDestEdit = FindWindowExA(hwndDestination_, nullptr, "EDIT", nullptr);
    if (hwndDestEdit) {
        hwndDestEdit_ = hwndDestEdit;
        auto orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
            hwndDestEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Win32EditorView::comboEditSubclassProc)));
        SetPropA(hwndDestEdit, "OrigWndProc", reinterpret_cast<HANDLE>(orig));
    }

    // Subclass hwndIdentifier_ to catch Enter key
    if (hwndIdentifier_) {
        auto orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
            hwndIdentifier_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Win32EditorView::editSubclassProc)));
        SetPropA(hwndIdentifier_, "OrigWndProc", reinterpret_cast<HANDLE>(orig));
    }
}

void Win32EditorView::refreshDestinationDropdown() {
    if (!hwndDestination_) return;

    char curText[64] = {0};
    GetWindowTextA(hwndDestination_, curText, sizeof(curText));

    updatingUi_ = true;
    SendMessageA(hwndDestination_, CB_RESETCONTENT, 0, 0);
    SendMessageA(hwndDestination_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("* local broadcast"));
    SendMessageA(hwndDestination_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("* broadcast"));

    std::vector<std::string> recents = ConfigStore::loadRecentIps();
    for (const auto& ip : recents) {
        SendMessageA(hwndDestination_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ip.c_str()));
    }

    SetWindowTextA(hwndDestination_, curText);
    updatingUi_ = false;
}

void Win32EditorView::commitDestination(const char* dest) {
    if (!dest || !plugin_) return;
    plugin_->setDestination(dest);

    Protocol::DestinationKind kind;
    uint32_t netIp = 0;
    if (Protocol::resolveDestination(dest, kind, netIp) && kind == Protocol::DestinationKind::CustomIp) {
        ConfigStore::saveRecentIp(dest);
        refreshDestinationDropdown();
    }
}

void Win32EditorView::syncFromPlugin() {
    if (!plugin_ || !hwnd_) return;
    updatingUi_ = true;

    // Enabled
    double enVal = plugin_->getParamNormalized(kParamEnabled);
    bool isEn = (enVal >= 0.5);
    SendMessageA(hwndEnabled_, BM_SETCHECK, isEn ? BST_CHECKED : BST_UNCHECKED, 0);

    // Mode
    double modeVal = plugin_->getParamNormalized(kParamMode);
    bool isSend = (modeVal >= 0.5);
    SendMessageA(hwndSend_, BM_SETCHECK, isSend ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(hwndReceive_, BM_SETCHECK, !isSend ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(hwndChannels_, isSend ? TRUE : FALSE);

    // Channels
    double chVal = plugin_->getParamNormalized(kParamChannels);
    int chIdx = std::clamp(static_cast<int>(std::round(chVal * 7.0)), 0, 7);
    SendMessageA(hwndChannels_, CB_SETCURSEL, chIdx, 0);

    // Identifier
    const char* id = plugin_->getIdentifier();
    SetWindowTextA(hwndIdentifier_, (id && id[0] != '\0') ? id : "default");

    // Destination
    const char* dest = plugin_->getDestination();
    SetWindowTextA(hwndDestination_, (dest && dest[0] != '\0') ? dest : "* local broadcast");

    updatingUi_ = false;
}

void Win32EditorView::onTimer() {
    if (!plugin_ || !hwnd_) return;

    updatingUi_ = true;

    // Sync parameters from host automation
    double enVal = plugin_->getParamNormalized(kParamEnabled);
    bool isEn = (enVal >= 0.5);
    LRESULT curEnCheck = SendMessageA(hwndEnabled_, BM_GETCHECK, 0, 0);
    if ((curEnCheck == BST_CHECKED) != isEn) {
        SendMessageA(hwndEnabled_, BM_SETCHECK, isEn ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    double modeVal = plugin_->getParamNormalized(kParamMode);
    bool isSend = (modeVal >= 0.5);
    LRESULT curSendCheck = SendMessageA(hwndSend_, BM_GETCHECK, 0, 0);
    LRESULT curRecvCheck = SendMessageA(hwndReceive_, BM_GETCHECK, 0, 0);
    if ((curSendCheck == BST_CHECKED) != isSend || (curRecvCheck == BST_CHECKED) == isSend) {
        SendMessageA(hwndSend_, BM_SETCHECK, isSend ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(hwndReceive_, BM_SETCHECK, !isSend ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(hwndChannels_, isSend ? TRUE : FALSE);
    }

    double chVal = plugin_->getParamNormalized(kParamChannels);
    int chIdx = std::clamp(static_cast<int>(std::round(chVal * 7.0)), 0, 7);
    int curSel = static_cast<int>(SendMessageA(hwndChannels_, CB_GETCURSEL, 0, 0));
    if (curSel != chIdx) {
        SendMessageA(hwndChannels_, CB_SETCURSEL, chIdx, 0);
    }

    // Non-focused text fields: Identifier
    HWND focusHwnd = GetFocus();
    if (focusHwnd != hwndIdentifier_) {
        char curId[32] = {0};
        GetWindowTextA(hwndIdentifier_, curId, sizeof(curId));
        const char* plugId = plugin_->getIdentifier();
        if (plugId && std::strcmp(curId, plugId) != 0) {
            SetWindowTextA(hwndIdentifier_, plugId);
        }
    }

    // Destination: check if focus is within combo or its edit box
    bool destFocused = (focusHwnd == hwndDestination_ || (hwndDestEdit_ && focusHwnd == hwndDestEdit_));
    if (!destFocused) {
        char curDest[64] = {0};
        GetWindowTextA(hwndDestination_, curDest, sizeof(curDest));
        const char* plugDest = plugin_->getDestination();
        if (plugDest && std::strcmp(curDest, plugDest) != 0) {
            SetWindowTextA(hwndDestination_, plugDest);
        }
    }

    updatingUi_ = false;

    // Telemetry meter decay
    float pL = plugin_->getPeakL();
    float pR = plugin_->getPeakR();

    constexpr float kDecayFactor = 0.85f;
    if (pL >= dispPeakL_) {
        dispPeakL_ = pL;
    } else {
        dispPeakL_ = dispPeakL_ * kDecayFactor;
    }
    if (pR >= dispPeakR_) {
        dispPeakR_ = pR;
    } else {
        dispPeakR_ = dispPeakR_ * kDecayFactor;
    }

    // Invalidate status pill area and meter area
    RECT pillRect{ 320, 10, 410, 40 };
    InvalidateRect(hwnd_, &pillRect, FALSE);

    RECT meterRect{ 14, 196, 406, 256 };
    InvalidateRect(hwnd_, &meterRect, FALSE);
}

void Win32EditorView::onPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int width = clientRect.right - clientRect.left;
    int height = clientRect.bottom - clientRect.top;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

    // 1. Fill main background (#181A1F)
    HBRUSH bgBrush = CreateSolidBrush(RGB(0x18, 0x1A, 0x1F));
    FillRect(memDC, &clientRect, bgBrush);
    DeleteObject(bgBrush);

    // 2. Draw Header
    HFONT oldFont = static_cast<HFONT>(SelectObject(memDC, hFontTitle_));
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(0xE6, 0xEA, 0xF0));
    TextOutA(memDC, 18, 14, "Stream Audio", 12);

    // Format / sample rate text
    SelectObject(memDC, hFontSmall_);
    SetTextColor(memDC, RGB(0x9C, 0xA3, 0xAF));
    char rateBuf[32];
    uint32_t srate = plugin_ ? plugin_->getLastSampleRate() : 48000;
    if (srate == 0 && plugin_) {
        srate = static_cast<uint32_t>(plugin_->getHostSampleRate());
    }
    snprintf(rateBuf, sizeof(rateBuf), "%u Hz", srate);
    TextOutA(memDC, 125, 17, rateBuf, static_cast<int>(std::strlen(rateBuf)));

    // Status Pill
    bool isEnabled = plugin_ ? (plugin_->getParamNormalized(kParamEnabled) >= 0.5) : true;
    bool isSend = plugin_ ? (plugin_->getParamNormalized(kParamMode) >= 0.5) : true;
    bool isTxActive = plugin_ ? plugin_->isTxActive() : false;
    bool isRxActive = plugin_ ? plugin_->isRxActive() : false;

    COLORREF pillBg, pillBorder, pillText;
    const char* pillStr = "OFF";
    if (!isEnabled) {
        pillBg = RGB(0x37, 0x41, 0x51);
        pillBorder = RGB(0x4B, 0x55, 0x63);
        pillText = RGB(0x9C, 0xA3, 0xAF);
        pillStr = "OFF";
    } else if (isSend) {
        pillBg = isTxActive ? RGB(0x06, 0x5F, 0x46) : RGB(0x04, 0x47, 0x34);
        pillBorder = RGB(0x10, 0xB9, 0x81);
        pillText = RGB(0x34, 0xD3, 0x99);
        pillStr = "TX";
    } else {
        pillBg = isRxActive ? RGB(0x1E, 0x40, 0xAF) : RGB(0x1E, 0x29, 0x3B);
        pillBorder = RGB(0x3B, 0x82, 0xF6);
        pillText = RGB(0x60, 0xA5, 0xFA);
        pillStr = "RX";
    }

    HBRUSH pillBrush = CreateSolidBrush(pillBg);
    HPEN pillPen = CreatePen(PS_SOLID, 1, pillBorder);
    HBRUSH oldPillBrush = static_cast<HBRUSH>(SelectObject(memDC, pillBrush));
    HPEN oldPillPen = static_cast<HPEN>(SelectObject(memDC, pillPen));
    RoundRect(memDC, 344, 12, 404, 34, 10, 10);
    SelectObject(memDC, oldPillBrush);
    SelectObject(memDC, oldPillPen);
    DeleteObject(pillBrush);
    DeleteObject(pillPen);

    SelectObject(memDC, hFontPill_);
    SetTextColor(memDC, pillText);
    RECT pillTextRect{ 344, 12, 404, 34 };
    DrawTextA(memDC, pillStr, -1, &pillTextRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 3. Draw Card 1 Panel (Config Section)
    HBRUSH cardBrush = CreateSolidBrush(RGB(0x24, 0x28, 0x30));
    HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(0x33, 0x38, 0x42));
    HBRUSH oldCardBrush = static_cast<HBRUSH>(SelectObject(memDC, cardBrush));
    HPEN oldCardPen = static_cast<HPEN>(SelectObject(memDC, cardPen));
    RoundRect(memDC, 14, 44, 406, 192, 8, 8);
    SelectObject(memDC, oldCardBrush);
    SelectObject(memDC, oldCardPen);
    DeleteObject(cardBrush);
    DeleteObject(cardPen);

    // Row labels inside card
    SelectObject(memDC, hFontUi_);
    SetTextColor(memDC, RGB(0x9C, 0xA3, 0xAF));
    TextOutA(memDC, 26, 90, "Identifier:", 11);
    TextOutA(memDC, 26, 124, "Destination:", 12);
    TextOutA(memDC, 26, 158, "Channels:", 9);

    // 4. Draw Footer Meter Panel
    HBRUSH meterBgBrush = CreateSolidBrush(RGB(0x20, 0x24, 0x2B));
    HPEN meterBorderPen = CreatePen(PS_SOLID, 1, RGB(0x33, 0x38, 0x42));
    SelectObject(memDC, meterBgBrush);
    SelectObject(memDC, meterBorderPen);
    RoundRect(memDC, 14, 198, 406, 252, 8, 8);
    SelectObject(memDC, oldCardBrush);
    SelectObject(memDC, oldCardPen);
    DeleteObject(meterBgBrush);
    DeleteObject(meterBorderPen);

    // Draw stereo meters
    auto drawMeter = [&](int y, const char* chName, float peakVal) {
        SelectObject(memDC, hFontSmallBold_);
        SetTextColor(memDC, RGB(0x9C, 0xA3, 0xAF));
        TextOutA(memDC, 24, y - 1, chName, 1);

        RECT trackRect{ 44, y, 344, y + 10 };
        HBRUSH trackBrush = CreateSolidBrush(RGB(0x13, 0x15, 0x19));
        FillRect(memDC, &trackRect, trackBrush);
        DeleteObject(trackBrush);

        float dB = -60.0f;
        if (peakVal > 0.00001f) {
            dB = 20.0f * std::log10(peakVal);
        }
        if (dB < -60.0f) dB = -60.0f;
        if (dB > 6.0f) dB = 6.0f;

        float frac = (dB + 60.0f) / 60.0f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        int fillW = static_cast<int>(frac * 300.0f);

        if (fillW > 0) {
            int greenW = std::min(fillW, 270);
            int amberW = (fillW > 270) ? std::min(fillW - 270, 25) : 0;
            int redW = (fillW > 295) ? std::min(fillW - 295, 5) : 0;

            if (greenW > 0) {
                RECT gRect{ 44, y, 44 + greenW, y + 10 };
                HBRUSH gBrush = CreateSolidBrush(RGB(0x10, 0xB9, 0x81));
                FillRect(memDC, &gRect, gBrush);
                DeleteObject(gBrush);
            }
            if (amberW > 0) {
                RECT aRect{ 44 + 270, y, 44 + 270 + amberW, y + 10 };
                HBRUSH aBrush = CreateSolidBrush(RGB(0xF5, 0x9E, 0x0B));
                FillRect(memDC, &aRect, aBrush);
                DeleteObject(aBrush);
            }
            if (redW > 0) {
                RECT rRect{ 44 + 295, y, 44 + 295 + redW, y + 10 };
                HBRUSH rBrush = CreateSolidBrush(RGB(0xEF, 0x44, 0x44));
                FillRect(memDC, &rRect, rBrush);
                DeleteObject(rBrush);
            }
        }

        char dbStr[16];
        if (peakVal <= 0.001f) {
            snprintf(dbStr, sizeof(dbStr), "-inf dB");
        } else {
            snprintf(dbStr, sizeof(dbStr), "%.1f dB", dB);
        }
        SelectObject(memDC, hFontSmall_);
        SetTextColor(memDC, (dB >= -0.5f) ? RGB(0xEF, 0x44, 0x44) : RGB(0x9C, 0xA3, 0xAF));
        TextOutA(memDC, 350, y - 1, dbStr, static_cast<int>(std::strlen(dbStr)));
    };

    drawMeter(208, "L", dispPeakL_);
    drawMeter(226, "R", dispPeakR_);

    SelectObject(memDC, oldFont);

    // BitBlt double-buffer to screen DC
    BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    EndPaint(hwnd, &ps);
}

LRESULT Win32EditorView::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
        onPaint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (wParam == kTimerId) {
            onTimer();
            return 0;
        }
        break;

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        if (updatingUi_) {
            break;
        }

        switch (id) {
        case kIdEnabled:
            if (code == BN_CLICKED) {
                bool checked = (SendMessageA(hwndEnabled_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (plugin_) {
                    plugin_->beginEdit(kParamEnabled);
                    plugin_->performEdit(kParamEnabled, checked ? 1.0 : 0.0);
                    plugin_->endEdit(kParamEnabled);
                    plugin_->setParamNormalized(kParamEnabled, checked ? 1.0 : 0.0);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;

        case kIdSend:
            if (code == BN_CLICKED) {
                if (plugin_) {
                    plugin_->beginEdit(kParamMode);
                    plugin_->performEdit(kParamMode, 1.0);
                    plugin_->endEdit(kParamMode);
                    plugin_->setParamNormalized(kParamMode, 1.0);
                }
                SendMessageA(hwndSend_, BM_SETCHECK, BST_CHECKED, 0);
                SendMessageA(hwndReceive_, BM_SETCHECK, BST_UNCHECKED, 0);
                EnableWindow(hwndChannels_, TRUE);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;

        case kIdReceive:
            if (code == BN_CLICKED) {
                if (plugin_) {
                    plugin_->beginEdit(kParamMode);
                    plugin_->performEdit(kParamMode, 0.0);
                    plugin_->endEdit(kParamMode);
                    plugin_->setParamNormalized(kParamMode, 0.0);
                }
                SendMessageA(hwndSend_, BM_SETCHECK, BST_UNCHECKED, 0);
                SendMessageA(hwndReceive_, BM_SETCHECK, BST_CHECKED, 0);
                EnableWindow(hwndChannels_, FALSE);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;

        case kIdIdentifier:
            if (code == EN_CHANGE || code == EN_KILLFOCUS) {
                char buf[32] = {0};
                GetWindowTextA(hwndIdentifier_, buf, sizeof(buf));
                if (plugin_) {
                    plugin_->setIdentifier(buf);
                }
            }
            break;

        case kIdDestination:
            if (code == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageA(hwndDestination_, CB_GETCURSEL, 0, 0));
                char buf[64] = {0};
                if (sel >= 0) {
                    SendMessageA(hwndDestination_, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(buf));
                } else {
                    GetWindowTextA(hwndDestination_, buf, sizeof(buf));
                }
                commitDestination(buf);
            } else if (code == CBN_KILLFOCUS) {
                char buf[64] = {0};
                GetWindowTextA(hwndDestination_, buf, sizeof(buf));
                commitDestination(buf);
            }
            break;

        case kIdChannels:
            if (code == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageA(hwndChannels_, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < 8 && plugin_) {
                    double norm = static_cast<double>(sel) / 7.0;
                    plugin_->beginEdit(kParamChannels);
                    plugin_->performEdit(kParamChannels, norm);
                    plugin_->endEdit(kParamChannels);
                    plugin_->setParamNormalized(kParamChannels, norm);
                }
            }
            break;
        }
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(0xE6, 0xEA, 0xF0));
        SetBkColor(hdc, RGB(0x18, 0x1A, 0x1F));
        return reinterpret_cast<LRESULT>(hEditBrush_);
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(0x9C, 0xA3, 0xAF));
        SetBkColor(hdc, RGB(0x24, 0x28, 0x30));
        return reinterpret_cast<LRESULT>(hCardBrush_);
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(0xE6, 0xEA, 0xF0));
        SetBkColor(hdc, RGB(0x24, 0x28, 0x30));
        return reinterpret_cast<LRESULT>(hCardBrush_);
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(0xE6, 0xEA, 0xF0));
        SetBkColor(hdc, RGB(0x18, 0x1A, 0x1F));
        return reinterpret_cast<LRESULT>(hEditBrush_);
    }

    case WM_DESTROY: {
        KillTimer(hwnd, kTimerId);
        hwnd_ = nullptr;
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

Steinberg::tresult PLUGIN_API Win32EditorView::attached(void* parent, Steinberg::FIDString type) {
    if (!parent || !type || std::strcmp(type, Steinberg::kPlatformTypeHWND) != 0) {
        return Steinberg::kInvalidArgument;
    }

    CPluginView::attached(parent, type);

    HWND parentHwnd = reinterpret_cast<HWND>(parent);
    HINSTANCE hInst = getPluginInstance();

    registerEditorClass(hInst);
    initGdi();

    hwnd_ = CreateWindowExA(
        0,
        kEditorClassName,
        "StreamAudioPlugin Editor",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 420, 260,
        parentHwnd,
        nullptr,
        hInst,
        this
    );

    if (!hwnd_) {
        return Steinberg::kResultFalse;
    }

    createChildControls(hInst);
    syncFromPlugin();

    SetTimer(hwnd_, kTimerId, 33, nullptr);

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Win32EditorView::removed() {
    if (hwnd_) {
        KillTimer(hwnd_, kTimerId);

        if (hwndDestEdit_) {
            auto orig = reinterpret_cast<WNDPROC>(GetPropA(hwndDestEdit_, "OrigWndProc"));
            if (orig && IsWindow(hwndDestEdit_)) {
                SetWindowLongPtrA(hwndDestEdit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
            }
            RemovePropA(hwndDestEdit_, "OrigWndProc");
            hwndDestEdit_ = nullptr;
        }

        if (hwndIdentifier_) {
            auto orig = reinterpret_cast<WNDPROC>(GetPropA(hwndIdentifier_, "OrigWndProc"));
            if (orig && IsWindow(hwndIdentifier_)) {
                SetWindowLongPtrA(hwndIdentifier_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
            }
            RemovePropA(hwndIdentifier_, "OrigWndProc");
        }

        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    return CPluginView::removed();
}

} // namespace StreamAudio
