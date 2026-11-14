#pragma once

#include <public.sdk/source/common/pluginview.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace StreamAudio {

class StreamAudioPlugin;

enum EditorControlId : int {
    kIdEnabled = 101,
    kIdSend = 102,
    kIdReceive = 103,
    kIdIdentifier = 104,
    kIdDestination = 105,
    kIdChannels = 106
};

class Win32EditorView : public Steinberg::CPluginView {
public:
    explicit Win32EditorView(StreamAudioPlugin* plugin);
    ~Win32EditorView() override;

    //---from IPlugView-------
    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override;

    // Win32 window message handling
    static LRESULT CALLBACK wndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK editSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK comboEditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND getHwnd() const noexcept { return hwnd_; }

private:
    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void registerEditorClass(HINSTANCE hInst);
    void initGdi();
    void cleanupGdi();
    void createChildControls(HINSTANCE hInst);
    void syncFromPlugin();
    void onTimer();
    void onPaint(HWND hwnd);
    void commitDestination(const char* dest);
    void refreshDestinationDropdown();

    StreamAudioPlugin* plugin_{nullptr};
    HWND hwnd_{nullptr};

    // Child control handles
    HWND hwndEnabled_{nullptr};
    HWND hwndSend_{nullptr};
    HWND hwndReceive_{nullptr};
    HWND hwndIdentifier_{nullptr};
    HWND hwndDestination_{nullptr};
    HWND hwndChannels_{nullptr};
    HWND hwndDestEdit_{nullptr};

    // GDI resources
    HFONT hFontTitle_{nullptr};
    HFONT hFontUi_{nullptr};
    HFONT hFontSmall_{nullptr};
    HFONT hFontSmallBold_{nullptr};
    HFONT hFontPill_{nullptr};

    HBRUSH hEditBrush_{nullptr};
    HBRUSH hCardBrush_{nullptr};
    HBRUSH hBgBrush_{nullptr};

    // UI state flags
    bool updatingUi_{false};
    float dispPeakL_{0.0f};
    float dispPeakR_{0.0f};

    static constexpr UINT_PTR kTimerId = 1001;
};

} // namespace StreamAudio
