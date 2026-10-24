// viewer_win32.cpp -- a small GDI window on the shm stream: pick a segment,
// pick a channel, watch that channel's waveform at 60 Hz.
//
// Single-threaded by design: pump messages, wait until the next 60 Hz frame
// boundary (MsgWaitForMultipleObjects on the producer event in event mode),
// drain new blocks into a scrolling trace, paint at the boundary. No reader
// thread, no queues -- the whole point of the demo is that Client's API is
// enough.
//
// Event vs. polling is decided once, after attach, from the header: the
// publish rate is sampleRateMilliHz / 1000 / samplesPerBlock blocks per
// second; at <= 60 Hz the frame boundary and the publish cadence are the same
// order, so waiting on the event buys real power savings. Above that, the
// frame loop drains whatever arrived anyway, so the event is ignored.
//
// ASSUMPTION: the producer's event is named after the segment itself (this
// demo passes the segment name as both names). Real producers append their
// own suffixes to the base names -- see reader_win32.cpp for the resolved
// names -- and Client silently falls back to polling when no event of the
// given name exists, so a wrong event name costs latency, not function.
//
// Build via examples/CMakeLists.txt (target viewer_win32), or standalone:
//
//     cl /std:c++17 /EHsc /W4 /I..\include viewer_win32.cpp
//
// SPDX-License-Identifier: MIT

#include <irtactile/shm/stream.h>
#include <irtactile/shm/stream/win32.h>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace irtactile::shm::stream;

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

enum : int {
    IDC_DEVICE = 101,
    IDC_CONNECT,
    IDC_CHANNEL,
};

constexpr int kTopBarHeight{32};        ///< control row, owned by the child controls
constexpr int kStatusHeight{20};        ///< status strip along the bottom edge
constexpr long long kFramePeriodMs{16}; ///< ~60 Hz
constexpr double kEventModeMaxRateHz{60.0};
constexpr size_t kNameBufferChars{256};
constexpr wchar_t kDefaultSegmentName[]{L"irTactile_Stream_Primary_256k"};

// Top bar geometry. The device field takes whatever width is left over once
// the fixed-width controls to its right have been placed.
constexpr int kMargin{8};
constexpr int kControlTop{5};
constexpr int kControlHeight{22};
constexpr int kBarCenterY{kControlTop + kControlHeight / 2}; ///< every control centers on this
constexpr int kDeviceLabelWidth{46};
constexpr int kDeviceEditLeft{58};
constexpr int kDeviceEditMinWidth{120};
constexpr int kDeviceEditReserveWidth{470}; ///< width claimed by the controls to its right
constexpr int kConnectButtonWidth{76};
constexpr int kChannelLabelWidth{54};
constexpr int kChannelComboWidth{64};
constexpr int kChannelComboDropHeight{200};

constexpr COLORREF kBackgroundColor{RGB(18, 20, 24)};
constexpr COLORREF kStatusTextColor{RGB(190, 195, 200)};
constexpr COLORREF kZeroLineColor{RGB(60, 64, 70)};
constexpr COLORREF kTraceColor{RGB(90, 200, 250)};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct AppState {
    Client client;

    HWND window{nullptr};
    HWND deviceLabel{nullptr};
    HWND deviceEdit{nullptr};
    HWND connectButton{nullptr};
    HWND channelLabel{nullptr};
    HWND channelCombo{nullptr};

    bool connected{false};
    bool eventMode{false};
    int selectedChannel{0};
    int traceCapacity{640};

    std::deque<float> trace;
    std::string status{"Not connected. Enter the resolved shm segment name and press Connect."};
};

AppState g_app;
HINSTANCE g_instance{nullptr};
HFONT g_uiFont{nullptr};   ///< shell font, shared by every control and the status text

// ---------------------------------------------------------------------------
// Connect / stream plumbing
// ---------------------------------------------------------------------------

/// Blocks published per second, derived from the header geometry.
double publishRateHz(const Reader& reader)
{
    return static_cast<double>(reader.sampleRateMilliHz()) / 1000.0 /
           static_cast<double>(reader.samplesPerBlock());
}

void setDisconnected(AppState& app)
{
    app.connected = false;
    app.trace.clear();
    EnableWindow(app.channelCombo, FALSE);
}

void populateChannels(AppState& app)
{
    const uint32_t channelCount = app.client.reader().channelCount();
    SendMessageW(app.channelCombo, CB_RESETCONTENT, 0, 0);
    for (uint32_t channel = 0; channel < channelCount; ++channel) {
        wchar_t label[12];
        std::swprintf(label, sizeof(label) / sizeof(label[0]), L"%u", channel);
        SendMessageW(app.channelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    }
    // A geometry change may have shrunk the channel count: clamp.
    if (app.selectedChannel >= static_cast<int>(channelCount))
        app.selectedChannel = static_cast<int>(channelCount) - 1;
    if (app.selectedChannel < 0) app.selectedChannel = 0;
    SendMessageW(app.channelCombo, CB_SETCURSEL, static_cast<WPARAM>(app.selectedChannel), 0);
}

void connectNow(AppState& app)
{
    wchar_t nameBuffer[kNameBufferChars] = {0};
    GetWindowTextW(app.deviceEdit, nameBuffer, static_cast<int>(kNameBufferChars) - 1);
    const std::wstring segmentName{nameBuffer};
    if (segmentName.empty()) {
        app.status = "Enter a segment name first.";
        return;
    }

    // Event name == segment name by demo assumption; a missing event just
    // leaves Client in polling mode. No structural assertion: this viewer
    // renders whatever geometry it finds.
    const OpenResult openResult =
        app.client.open(segmentName.c_str(), segmentName.c_str(), nullptr);

    if (openResult != OpenResult::Ok) {
        std::string message{"Open failed: "};
        message += toCString(openResult);
        if (shouldRetry(openResult)) {
            message += " - producer not up yet, or wrong name; try again";
        } else if (app.client.reader().mismatchDetail()[0] != '\0') {
            message += " (";
            message += app.client.reader().mismatchDetail();
            message += ")";
        }
        app.status = message;
        setDisconnected(app);
        return;
    }

    const Reader& reader = app.client.reader();
    const double rateHz = publishRateHz(reader);

    app.connected = true;
    app.eventMode = (app.client.nativeEvent() != nullptr) && rateHz <= kEventModeMaxRateHz;
    app.selectedChannel = 0;
    app.trace.clear();
    populateChannels(app);
    EnableWindow(app.channelCombo, TRUE);

    char text[256];
    std::snprintf(text, sizeof(text),
                  "Attached: \"%s\", %u ch, publish %.1f Hz, period %.2f ms, %s mode",
                  reader.deviceName(), reader.channelCount(), rateHz, reader.periodMs(),
                  app.eventMode ? "event" : "polling");
    app.status = text;
}

/// Called once per 60 Hz frame while connected: classify header changes,
/// drain new blocks into the trace, refresh the status line.
void processStream(AppState& app)
{
    // Zero-timeout wait: consumes an event pulse if one is pending and
    // classifies layoutSeq / deviceState changes; the blocks themselves are
    // drained below (wakes coalesce, so always loop next()).
    const WaitResult waitResult = app.client.wait(std::chrono::milliseconds(0));

    if (waitResult == WaitResult::Closed) {
        char text[128];
        std::snprintf(text, sizeof(text), "Producer closed. dropped=%llu",
                      static_cast<unsigned long long>(app.client.reader().droppedBlocks()));
        app.status = text;
        app.client.close();
        setDisconnected(app);
        return;
    }
    if (waitResult == WaitResult::Reconfigured) {
        // Geometry changed in place: the channel count may differ, and the
        // old trace was a different channel's worth of data anyway.
        populateChannels(app);
        app.trace.clear();
    }
    // ProducerRestarted needs no action here: layout unchanged, status text
    // below picks up the (possibly new) device name.

    BlockView block;
    while (app.client.next(block)) {
        const float* samples = block.channel(static_cast<uint32_t>(app.selectedChannel));
        if (!samples) continue;
        for (uint32_t i = 0; i < block.sampleCount; ++i) {
            app.trace.push_back(samples[i]);
            if (static_cast<int>(app.trace.size()) > app.traceCapacity) app.trace.pop_front();
        }
    }

    const Reader& reader = app.client.reader();
    char text[256];
    std::snprintf(text, sizeof(text), "\"%s\"  %.1f Hz  %s  state=%s  dropped=%llu",
                  reader.deviceName(), publishRateHz(reader),
                  app.eventMode ? "event" : "polling",
                  toCString(app.client.state()),
                  static_cast<unsigned long long>(reader.droppedBlocks()));
    app.status = text;
}

// ---------------------------------------------------------------------------
// Layout and painting
// ---------------------------------------------------------------------------

/// Places a control of a known height centered on the top bar's mid-line.
void placeControl(HWND control, int left, int width, int height)
{
    MoveWindow(control, left, kBarCenterY - height / 2, width, height, TRUE);
}

/// A combobox ignores the height it is given -- that argument sizes the
/// dropped-down list, while the closed control snaps to its own font-derived
/// item height. So place it, measure what it actually became, then place it
/// again centered on that real height.
void placeCombo(HWND combo, int left, int width)
{
    MoveWindow(combo, left, kControlTop, width, kChannelComboDropHeight, TRUE);
    RECT bounds;
    GetWindowRect(combo, &bounds);
    const int closedHeight = bounds.bottom - bounds.top;
    MoveWindow(combo, left, kBarCenterY - closedHeight / 2, width,
               kChannelComboDropHeight, TRUE);
}

void layoutControls(AppState& app, int clientWidth)
{
    const int deviceEditWidth =
        std::max(kDeviceEditMinWidth, clientWidth - kDeviceEditReserveWidth);
    int left = kDeviceEditLeft;

    // The labels carry SS_CENTERIMAGE, so a full-height rect centers their
    // text the same way the button centers its own.
    placeControl(app.deviceLabel, kMargin, kDeviceLabelWidth, kControlHeight);
    placeControl(app.deviceEdit, left, deviceEditWidth, kControlHeight);
    left += deviceEditWidth + kMargin;
    placeControl(app.connectButton, left, kConnectButtonWidth, kControlHeight);
    left += kConnectButtonWidth + 12;
    placeControl(app.channelLabel, left, kChannelLabelWidth, kControlHeight);
    left += kChannelLabelWidth + 2;
    placeCombo(app.channelCombo, left, kChannelComboWidth);

    // One trace sample per horizontal pixel.
    app.traceCapacity = clientWidth > 0 ? clientWidth : 1;
    while (static_cast<int>(app.trace.size()) > app.traceCapacity) app.trace.pop_front();
}

/// Everything the frame loop repaints: the client area below the control row.
/// The top bar belongs to the child controls and must not be invalidated at
/// frame rate.
RECT plotRect(HWND window)
{
    RECT rect;
    GetClientRect(window, &rect);
    rect.top = kTopBarHeight;
    if (rect.top > rect.bottom) rect.top = rect.bottom;
    return rect;
}

void drawTrace(HDC dc, const AppState& app, int width, int plotHeight)
{
    const HPEN zeroPen = CreatePen(PS_SOLID, 1, kZeroLineColor);
    HGDIOBJ previousPen = SelectObject(dc, zeroPen);
    const int zeroY = kTopBarHeight + plotHeight / 2;
    MoveToEx(dc, 0, zeroY, nullptr);
    LineTo(dc, width, zeroY);

    // Waveform: [-1, 1] mapped onto the plot area, newest sample at the right
    // edge (older columns scroll left and out).
    if (app.trace.size() >= 2) {
        const int sampleCount = static_cast<int>(app.trace.size());
        std::vector<POINT> points;
        points.reserve(app.trace.size());
        for (int i = 0; i < sampleCount; ++i) {
            float value = app.trace[static_cast<size_t>(i)];
            if (value > 1.0f) value = 1.0f;
            if (value < -1.0f) value = -1.0f;
            const LONG x = static_cast<LONG>(width - sampleCount + i);
            const LONG y = kTopBarHeight + static_cast<LONG>((1.0f - value) * 0.5f *
                                                             static_cast<float>(plotHeight));
            points.push_back({x, y});
        }
        const HPEN tracePen = CreatePen(PS_SOLID, 1, kTraceColor);
        HGDIOBJ previousTracePen = SelectObject(dc, tracePen);
        Polyline(dc, points.data(), static_cast<int>(points.size()));
        SelectObject(dc, previousTracePen);
        DeleteObject(tracePen);
    }

    SelectObject(dc, previousPen);
    DeleteObject(zeroPen);
}

void onPaint(AppState& app)
{
    PAINTSTRUCT paint;
    const HDC windowDc = BeginPaint(app.window, &paint);
    RECT clientRect;
    GetClientRect(app.window, &clientRect);
    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0) {
        EndPaint(app.window, &paint);
        return;
    }

    // Double buffer: one memory DC, one Polyline, one BitBlt.
    const HDC memoryDc = CreateCompatibleDC(windowDc);
    const HBITMAP memoryBitmap = CreateCompatibleBitmap(windowDc, width, height);
    HGDIOBJ previousBitmap = SelectObject(memoryDc, memoryBitmap);

    const HBRUSH backgroundBrush = CreateSolidBrush(kBackgroundColor);
    FillRect(memoryDc, &clientRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    // Status goes in the bottom strip: the top bar is the controls' territory,
    // and text drawn there is painted over by the children.
    SetBkMode(memoryDc, TRANSPARENT);
    SetTextColor(memoryDc, kStatusTextColor);
    HGDIOBJ previousFont = SelectObject(memoryDc, g_uiFont);
    TextOutA(memoryDc, kMargin, height - kStatusHeight + 3, app.status.c_str(),
             static_cast<int>(app.status.size()));
    SelectObject(memoryDc, previousFont);

    const int plotHeight = height - kTopBarHeight - kStatusHeight;
    if (app.connected && plotHeight > 8) drawTrace(memoryDc, app, width, plotHeight);

    BitBlt(windowDc, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);

    SelectObject(memoryDc, previousBitmap);
    DeleteObject(memoryBitmap);
    DeleteDC(memoryDc);
    EndPaint(app.window, &paint);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

/// The shell's message font. Without it every control falls back to the
/// ancient System bitmap font, whose per-class metrics are what make a row of
/// controls refuse to line up.
HFONT createUiFont()
{
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        return CreateFontIndirectW(&metrics.lfMessageFont);
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

void createControls(AppState& app, HWND window)
{
    app.deviceLabel = CreateWindowExW(0, L"STATIC", L"Device:",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                      0, 0, 0, 0, window, nullptr, g_instance, nullptr);
    app.deviceEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     0, 0, 0, 0, window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DEVICE)),
                                     g_instance, nullptr);
    SendMessageW(app.deviceEdit, EM_SETLIMITTEXT, kNameBufferChars - 1, 0);
    SetWindowTextW(app.deviceEdit, kDefaultSegmentName);

    app.connectButton = CreateWindowExW(0, L"BUTTON", L"Connect",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                        0, 0, 0, 0, window,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CONNECT)),
                                        g_instance, nullptr);
    app.channelLabel = CreateWindowExW(0, L"STATIC", L"Channel:",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                       0, 0, 0, 0, window, nullptr, g_instance, nullptr);
    // CBS_DROPDOWNLIST is mandatory, not decoration: the combo type lives in
    // the low style bits, and a zero there falls back to CBS_SIMPLE -- a list
    // box that is always dropped and cannot be collapsed.
    app.channelCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                           CBS_DROPDOWNLIST,
                                       0, 0, 0, 0, window,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHANNEL)),
                                       g_instance, nullptr);
    EnableWindow(app.channelCombo, FALSE);

    // Set before the first layout pass: the combo's closed height, which
    // placeCombo() measures, follows from this font.
    const HWND controls[]{app.deviceLabel, app.deviceEdit, app.connectButton,
                          app.channelLabel, app.channelCombo};
    for (const HWND control : controls)
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
        case WM_CREATE: {
            AppState& app = g_app;
            app.window = window;
            g_uiFont = createUiFont();
            createControls(app, window);

            RECT clientRect;
            GetClientRect(window, &clientRect);
            layoutControls(app, clientRect.right);
            return 0;
        }

        case WM_SIZE:
            layoutControls(g_app, static_cast<int>(static_cast<short>(LOWORD(lParam))));
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_CONNECT:
                    connectNow(g_app);
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                case IDC_CHANNEL:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        const LRESULT selection =
                            SendMessageW(g_app.channelCombo, CB_GETCURSEL, 0, 0);
                        if (selection != CB_ERR)
                            g_app.selectedChannel = static_cast<int>(selection);
                        // The trace keeps its history and scrolls the new
                        // channel in from the right.
                    }
                    return 0;
            }
            break;

        case WM_PAINT:
            onPaint(g_app);
            return 0;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            if (g_uiFont) {
                DeleteObject(g_uiFont);
                g_uiFont = nullptr;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

// ---------------------------------------------------------------------------
// Entry and frame loop
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    g_instance = instance;
    AppState& app = g_app;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    windowClass.lpszClassName = L"irTactileStreamViewer";
    if (RegisterClassExW(&windowClass) == 0) return 1;

    RECT windowRect{0, 0, 640, 360};
    AdjustWindowRectEx(&windowRect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    // WS_CLIPCHILDREN: the frame loop repaints the client area 60x/s, and
    // without it every repaint scribbles over the child controls, which then
    // repaint themselves -- visible flicker in the top bar.
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName,
                                        L"irTactile shm stream viewer",
                                        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                        CW_USEDEFAULT, CW_USEDEFAULT,
                                        windowRect.right - windowRect.left,
                                        windowRect.bottom - windowRect.top,
                                        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    using Clock = std::chrono::steady_clock;
    Clock::time_point nextFrame = Clock::now() + std::chrono::milliseconds(kFramePeriodMs);

    bool running = true;
    while (running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            // Enter in the name field means Connect. Swallowing the key here
            // also keeps the edit control from beeping at it.
            if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN &&
                message.hwnd == app.deviceEdit) {
                connectNow(app);
                InvalidateRect(window, nullptr, TRUE);
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) break;

        // Sleep until the next 60 Hz boundary, but stay interruptible by both
        // window messages and -- in event mode -- the producer's publish event.
        const Clock::time_point now = Clock::now();
        const long long msToFrame =
            std::chrono::duration_cast<std::chrono::milliseconds>(nextFrame - now).count();
        if (msToFrame > 0) {
            if (app.connected && app.eventMode) {
                HANDLE publishEvent = app.client.nativeEvent();
                const DWORD waitStatus =
                    MsgWaitForMultipleObjects(1, &publishEvent, FALSE,
                                              static_cast<DWORD>(msToFrame), QS_ALLINPUT);
                if (waitStatus == WAIT_OBJECT_0) {
                    // MsgWaitForMultipleObjects does not consume auto-reset
                    // events: eat the pulse so the loop does not spin until
                    // the boundary. The block itself is drained in
                    // processStream(). (Single waiter only -- if two viewers
                    // ran against one event, one could sleep through a wake;
                    // acceptable for a demo.)
                    WaitForSingleObject(publishEvent, 0);
                }
            } else {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(msToFrame),
                                          QS_ALLINPUT);
            }
            continue;
        }

        // Frame boundary.
        nextFrame += std::chrono::milliseconds(kFramePeriodMs);
        if (nextFrame < now) nextFrame = now + std::chrono::milliseconds(kFramePeriodMs);

        if (app.connected) processStream(app);
        const RECT plot = plotRect(window);
        InvalidateRect(window, &plot, FALSE);
    }

    app.client.close();
    return 0;
}
