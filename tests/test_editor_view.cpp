#include <PluginIds.h>
#include <StreamAudioPlugin.h>

#include <pluginterfaces/base/coreiids.cpp>
#include <pluginterfaces/base/funknown.cpp>
#include <public.sdk/source/vst/vstinitiids.cpp>
#include <base/source/baseiids.cpp>

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <windows.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

using namespace Steinberg;
using namespace Steinberg::Vst;

typedef bool (*InitDllFunc)();
typedef bool (*ExitDllFunc)();
typedef IPluginFactory* (PLUGIN_API *GetPluginFactoryFunc)();

int main(int argc, char* argv[]) {
    std::cout << "=== [test_editor_view] Running Win32 IPlugView UI tests ===" << std::endl;

    const char* dllPath = "build/StreamAudioPlugin.vst3/Contents/x86_64-win/StreamAudioPlugin.vst3";
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        dllPath = argv[1];
    }
    std::cout << "[Step 1] Loading plugin DLL: " << dllPath << std::endl;

    HMODULE hMod = LoadLibraryA(dllPath);
    if (!hMod) {
        std::cerr << "FAILED: Could not load " << dllPath << " (GetLastError=" << GetLastError() << ")" << std::endl;
        return 1;
    }

    auto initDll = reinterpret_cast<InitDllFunc>(GetProcAddress(hMod, "InitDll"));
    auto exitDll = reinterpret_cast<ExitDllFunc>(GetProcAddress(hMod, "ExitDll"));
    auto getPluginFactory = reinterpret_cast<GetPluginFactoryFunc>(GetProcAddress(hMod, "GetPluginFactory"));

    assert(initDll != nullptr && "InitDll export must exist");
    assert(exitDll != nullptr && "ExitDll export must exist");
    assert(getPluginFactory != nullptr && "GetPluginFactory export must exist");

    bool inited = initDll();
    assert(inited && "InitDll() must return true");

    IPluginFactory* factory = getPluginFactory();
    assert(factory != nullptr && "GetPluginFactory() must return valid factory pointer");

    // Create IComponent / IEditController
    IComponent* component = nullptr;
    tresult cRes = factory->createInstance(
        StreamAudio::kProcessorUID,
        IComponent::iid,
        reinterpret_cast<void**>(&component)
    );
    assert(cRes == kResultOk && component != nullptr);

    IEditController* controller = nullptr;
    tresult qcRes = component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller));
    assert(qcRes == kResultOk && controller != nullptr);

    tresult initRes = component->initialize(nullptr);
    assert(initRes == kResultOk);

    std::cout << "[Step 2] Testing createView lifecycle" << std::endl;

    // 2a. createView("invalid") must return nullptr
    IPlugView* invalidView = controller->createView("invalid_view_name");
    assert(invalidView == nullptr && "createView with invalid name must return nullptr");

    // 2b. createView("editor") must return a valid non-null IPlugView
    IPlugView* editorView = controller->createView(ViewType::kEditor);
    if (!editorView) {
        std::cerr << "FAILED: createView(\"editor\") returned nullptr!" << std::endl;
        component->terminate();
        controller->release();
        component->release();
        exitDll();
        FreeLibrary(hMod);
        return 2;
    }
    assert(editorView != nullptr && "createView(ViewType::kEditor) must return valid IPlugView");

    // 2c. Platform type support
    assert(editorView->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue && "HWND platform must be supported");
    assert(editorView->isPlatformTypeSupported("NSView") != kResultTrue && "Non-HWND platform must not be supported");

    // 2d. Dimensions check
    ViewRect rect{};
    tresult sizeRes = editorView->getSize(&rect);
    assert(sizeRes == kResultTrue && "getSize must return kResultTrue");
    std::cout << "  View dimensions: [" << rect.left << ", " << rect.top << ", " << rect.right << ", " << rect.bottom << "]"
              << " (" << rect.getWidth() << "x" << rect.getHeight() << ")" << std::endl;
    assert(rect.left == 0);
    assert(rect.top == 0);
    assert(rect.right == 420);
    assert(rect.bottom == 260);
    assert(rect.getWidth() == 420);
    assert(rect.getHeight() == 260);

    std::cout << "[Step 3] Attaching view to Win32 parent window" << std::endl;

    // Register test parent window class
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "TestHostParentWindow";
    RegisterClassExA(&wc);

    HWND parentHwnd = CreateWindowExA(
        0,
        wc.lpszClassName,
        "Test Host Parent Window",
        WS_POPUP,
        100, 100, 420, 260,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    assert(parentHwnd != nullptr && "Parent HWND creation must succeed");

    tresult attachRes = editorView->attached(parentHwnd, kPlatformTypeHWND);
    assert(attachRes == kResultOk && "attached(parentHwnd, kPlatformTypeHWND) must return kResultOk");

    // Find the embedded child window
    HWND childHwnd = FindWindowExA(parentHwnd, nullptr, nullptr, nullptr);
    assert(childHwnd != nullptr && "Child editor window must be created inside parent HWND");

    // Verify child controls exist
    // Control IDs:
    // Enabled checkbox: 101
    // Send radio: 102
    // Receive radio: 103
    // Identifier edit: 104
    // Destination combo: 105
    // Channels combo: 106
    HWND hwndEnabled = GetDlgItem(childHwnd, 101);
    HWND hwndSend = GetDlgItem(childHwnd, 102);
    HWND hwndReceive = GetDlgItem(childHwnd, 103);
    HWND hwndIdentifier = GetDlgItem(childHwnd, 104);
    HWND hwndDestination = GetDlgItem(childHwnd, 105);
    HWND hwndChannels = GetDlgItem(childHwnd, 106);

    assert(hwndEnabled != nullptr && "Enabled checkbox control must exist");
    assert(hwndSend != nullptr && "Send button/radio control must exist");
    assert(hwndReceive != nullptr && "Receive button/radio control must exist");
    assert(hwndIdentifier != nullptr && "Identifier edit box control must exist");
    assert(hwndDestination != nullptr && "Destination combobox control must exist");
    assert(hwndChannels != nullptr && "Channels control must exist");

    // Check default Identifier edit box text is "default"
    char idText[64] = {0};
    GetWindowTextA(hwndIdentifier, idText, sizeof(idText));
    std::cout << "  Identifier control text: " << idText << std::endl;
    assert(std::string(idText) == "default" && "Default identifier text must be 'default'");

    // Check Enabled checkbox state (default is checked)
    LRESULT chkState = SendMessageA(hwndEnabled, BM_GETCHECK, 0, 0);
    assert(chkState == BST_CHECKED && "Enabled checkbox must be checked by default");

    // Send mode is default: verify Channels control is enabled
    assert(IsWindowEnabled(hwndChannels) == TRUE && "Channels control must be enabled in Send mode");

    auto* plugin = static_cast<StreamAudio::StreamAudioPlugin*>(component);

    // Test live Identifier change from UI
    SetWindowTextA(hwndIdentifier, "vocal_mic");
    SendMessageA(childHwnd, WM_COMMAND, MAKEWPARAM(104, EN_CHANGE), reinterpret_cast<LPARAM>(hwndIdentifier));
    assert(std::string(plugin->getIdentifier()) == "vocal_mic" && "Plugin identifier must update live on EN_CHANGE");

    // Test Destination commit with custom IPv4
    SetWindowTextA(hwndDestination, "192.168.1.99");
    SendMessageA(childHwnd, WM_COMMAND, MAKEWPARAM(105, CBN_KILLFOCUS), reinterpret_cast<LPARAM>(hwndDestination));
    assert(std::string(plugin->getDestination()) == "192.168.1.99" && "Plugin destination must update on CBN_KILLFOCUS");

    // Test Channels change via UI
    SendMessageA(hwndChannels, CB_SETCURSEL, 5, 0); // 6 channels (5.1)
    SendMessageA(childHwnd, WM_COMMAND, MAKEWPARAM(106, CBN_SELCHANGE), reinterpret_cast<LPARAM>(hwndChannels));
    assert(std::abs(controller->getParamNormalized(StreamAudio::kParamChannels) - (5.0 / 7.0)) < 1e-4);

    // Test Enabled checkbox toggle via UI
    SendMessageA(hwndEnabled, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageA(childHwnd, WM_COMMAND, MAKEWPARAM(101, BN_CLICKED), reinterpret_cast<LPARAM>(hwndEnabled));
    assert(controller->getParamNormalized(StreamAudio::kParamEnabled) == 0.0);

    // Click Receive radio: Channels should become disabled
    SendMessageA(childHwnd, WM_COMMAND, MAKEWPARAM(103, BN_CLICKED), reinterpret_cast<LPARAM>(hwndReceive));
    assert(IsWindowEnabled(hwndChannels) == FALSE && "Channels control must be disabled in Receive mode");
    assert(controller->getParamNormalized(StreamAudio::kParamMode) == 0.0);

    // Click Send radio back: Channels should become enabled again
    SendMessageA(childHwnd, WM_COMMAND, MAKEWPARAM(102, BN_CLICKED), reinterpret_cast<LPARAM>(hwndSend));
    assert(IsWindowEnabled(hwndChannels) == TRUE && "Channels control must be re-enabled in Send mode");
    assert(controller->getParamNormalized(StreamAudio::kParamMode) == 1.0);

    // Test reverse sync: host changes parameter -> WM_TIMER updates UI controls
    controller->setParamNormalized(StreamAudio::kParamEnabled, 1.0);

    std::cout << "[Step 4] Pumping Win32 message loop for 100 ms (WM_TIMER + WM_PAINT)" << std::endl;
    ShowWindow(parentHwnd, SW_SHOW);
    UpdateWindow(parentHwnd);

    auto startTime = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() < 100) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Verify reverse sync from host parameter update (WM_TIMER ticked and checked Enabled box)
    assert(SendMessageA(hwndEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED && "Enabled checkbox must sync to checked via host param update");

    std::cout << "[Step 5] Detaching and removing view" << std::endl;
    // Test double attached() call guard: must cleanly recreate window without crash or leak
    tresult attachAgainRes = editorView->attached(parentHwnd, kPlatformTypeHWND);
    assert(attachAgainRes == kResultOk && "Double attached() call must succeed cleanly");
    HWND refreshedChild = FindWindowExA(parentHwnd, nullptr, nullptr, nullptr);
    assert(refreshedChild != nullptr && "Child window must exist after double attached()");

    tresult remRes = editorView->removed();
    assert(remRes == kResultOk && "removed() must return kResultOk");

    // Check child window was destroyed
    assert(!IsWindow(refreshedChild) && "Child window must be destroyed after removed()");

    editorView->release();
    DestroyWindow(parentHwnd);

    std::cout << "[Step 5b] Testing parent HWND destruction before removed() & unsubclassing on WM_NCDESTROY" << std::endl;
    IPlugView* view2 = controller->createView(ViewType::kEditor);
    assert(view2 != nullptr);
    HWND parent2 = CreateWindowExA(0, wc.lpszClassName, "Host Parent 2", WS_POPUP, 150, 150, 420, 260, nullptr, nullptr, wc.hInstance, nullptr);
    assert(parent2 != nullptr);
    assert(view2->attached(parent2, kPlatformTypeHWND) == kResultOk);
    // Destroy parent window hierarchy before calling removed() (simulating abrupt DAW window close)
    DestroyWindow(parent2);
    // Now call removed() and release() - must handle destroyed HWND safely without crash or leak
    assert(view2->removed() == kResultOk);
    view2->release();

    std::cout << "[Step 6] Clean host teardown" << std::endl;
    assert(component->terminate() == kResultOk);
    controller->release();
    component->release();

    bool deinitRes = exitDll();
    assert(deinitRes && "ExitDll() must return true");

    BOOL freeRes = FreeLibrary(hMod);
    assert(freeRes && "FreeLibrary must succeed");

    std::cout << "=== [test_editor_view] ALL UI TESTS PASSED SUCCESSFULLY! ===" << std::endl;
    return 0;
}
