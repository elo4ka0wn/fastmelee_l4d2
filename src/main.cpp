#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QtGlobal>
#include <QtCore/QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

struct KeyBinding {
    int qtKey = 0;
    quint32 nativeScanCode = 0;
    quint32 nativeVirtualKey = 0;
    quint32 nativeModifiers = 0;
    QString displayName;

    bool isValid() const noexcept { return qtKey != 0; }
};

struct DelaySettings {
    int key2DelayMs = 0;
    int mouseDelayMs = 300;
    int finalKeyDelayMs = 0;
};

struct MacroSettings {
    KeyBinding trigger;
    DelaySettings delays;
    int finalKey = 1; // Allowed values: 1, 4, 5
};

class ConfigManager {
public:
    ConfigManager();

    MacroSettings load() const;
    bool save(const MacroSettings& settings) const;

private:
    QString ensureConfigDir() const;

    QString m_configFile;
};

ConfigManager::ConfigManager() {
    const auto dir = ensureConfigDir();
    if (!dir.isEmpty()) {
        m_configFile = dir + QLatin1Char('/') + QLatin1String("profile.json");
    }
}

QString ConfigManager::ensureConfigDir() const {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (base.isEmpty()) {
        return {};
    }

    QDir dir(base);
    if (!dir.exists("fastmelee_macro")) {
        dir.mkpath("fastmelee_macro");
    }
    dir.cd("fastmelee_macro");
    return dir.absolutePath();
}

MacroSettings ConfigManager::load() const {
    MacroSettings settings;
    settings.delays.key2DelayMs = 0;
    settings.delays.mouseDelayMs = 300;
    settings.delays.finalKeyDelayMs = 0;
    settings.finalKey = 1;

    if (m_configFile.isEmpty()) {
        return settings;
    }

    QFile file(m_configFile);
    if (!file.exists()) {
        return settings;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        return settings;
    }

    const auto data = file.readAll();
    file.close();

    const auto document = QJsonDocument::fromJson(data);
    if (!document.isObject()) {
        return settings;
    }

    const auto root = document.object();
    const auto trigger = root.value(QLatin1String("trigger")).toObject();
    settings.trigger.qtKey = trigger.value(QLatin1String("qtKey")).toInt();
    settings.trigger.nativeScanCode = static_cast<quint32>(trigger.value(QLatin1String("scanCode")).toInt());
    settings.trigger.nativeVirtualKey = static_cast<quint32>(trigger.value(QLatin1String("virtualKey")).toInt());
    settings.trigger.nativeModifiers = static_cast<quint32>(trigger.value(QLatin1String("modifiers")).toInt());
    settings.trigger.displayName = trigger.value(QLatin1String("displayName")).toString();

    const auto delays = root.value(QLatin1String("delays")).toObject();
    settings.delays.key2DelayMs = delays.value(QLatin1String("key2")).toInt(settings.delays.key2DelayMs);
    settings.delays.mouseDelayMs = delays.value(QLatin1String("mouse")).toInt(settings.delays.mouseDelayMs);
    settings.delays.finalKeyDelayMs = delays.value(QLatin1String("final")).toInt(settings.delays.finalKeyDelayMs);

    settings.finalKey = root.value(QLatin1String("finalKey")).toInt(settings.finalKey);
    if (settings.finalKey != 1 && settings.finalKey != 4 && settings.finalKey != 5) {
        settings.finalKey = 1;
    }

    return settings;
}

bool ConfigManager::save(const MacroSettings& settings) const {
    if (m_configFile.isEmpty()) {
        return false;
    }

    QJsonObject trigger;
    trigger.insert(QLatin1String("qtKey"), settings.trigger.qtKey);
    trigger.insert(QLatin1String("scanCode"), static_cast<int>(settings.trigger.nativeScanCode));
    trigger.insert(QLatin1String("virtualKey"), static_cast<int>(settings.trigger.nativeVirtualKey));
    trigger.insert(QLatin1String("modifiers"), static_cast<int>(settings.trigger.nativeModifiers));
    trigger.insert(QLatin1String("displayName"), settings.trigger.displayName);

    QJsonObject delays;
    delays.insert(QLatin1String("key2"), settings.delays.key2DelayMs);
    delays.insert(QLatin1String("mouse"), settings.delays.mouseDelayMs);
    delays.insert(QLatin1String("final"), settings.delays.finalKeyDelayMs);

    QJsonObject root;
    root.insert(QLatin1String("trigger"), trigger);
    root.insert(QLatin1String("delays"), delays);
    root.insert(QLatin1String("finalKey"), settings.finalKey);

    const QJsonDocument document(root);

    QFile file(m_configFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    const auto bytesWritten = file.write(document.toJson(QJsonDocument::Indented));
    file.close();
    return bytesWritten > 0;
}

class InputSimulator {
public:
    virtual ~InputSimulator() = default;
    virtual void tapDigit(int digit) = 0;
    virtual void clickLeftButton() = 0;
};

class GlobalKeyListener {
public:
    struct NativeBinding {
        quint32 scanCode = 0;
        quint32 virtualKey = 0;
        bool valid = false;
    };

    using Callback = std::function<void(bool)>;

    virtual ~GlobalKeyListener() = default;
    virtual void setCallback(Callback callback) = 0;
    virtual void updateBinding(const NativeBinding& binding) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
};

#ifdef Q_OS_WIN
class WindowsGlobalKeyListener : public GlobalKeyListener {
public:
    WindowsGlobalKeyListener();
    ~WindowsGlobalKeyListener() override;

    void setCallback(Callback callback) override;
    void updateBinding(const NativeBinding& binding) override;
    void start() override;
    void stop() override;

private:
    void processEvent(bool pressed);
    static LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_bindingValid{false};
    std::atomic<DWORD> m_virtualKey{0};

    std::thread m_thread;
    std::atomic<DWORD> m_threadId{0};
    HHOOK m_hook = nullptr;

    mutable std::mutex m_callbackMutex;
    Callback m_callback;
};

namespace {
WindowsGlobalKeyListener* g_listenerInstance = nullptr;
}

WindowsGlobalKeyListener::WindowsGlobalKeyListener() {
    g_listenerInstance = this;
}

WindowsGlobalKeyListener::~WindowsGlobalKeyListener() {
    stop();
    if (g_listenerInstance == this) {
        g_listenerInstance = nullptr;
    }
}

void WindowsGlobalKeyListener::setCallback(Callback callback) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_callback = std::move(callback);
}

void WindowsGlobalKeyListener::updateBinding(const NativeBinding& binding) {
    if (binding.valid && binding.virtualKey != 0) {
        m_virtualKey.store(binding.virtualKey, std::memory_order_release);
        m_bindingValid.store(true, std::memory_order_release);
    } else {
        m_bindingValid.store(false, std::memory_order_release);
    }
}

void WindowsGlobalKeyListener::start() {
    if (m_running.exchange(true)) {
        return;
    }

    m_thread = std::thread([this]() {
        m_threadId.store(GetCurrentThreadId(), std::memory_order_release);
        m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, &WindowsGlobalKeyListener::hookProc, GetModuleHandleW(nullptr), 0);
        if (!m_hook) {
            qWarning() << "Failed to install keyboard hook:" << GetLastError();
            m_running.store(false);
            return;
        }

        MSG msg;
        while (m_running.load(std::memory_order_acquire)) {
            const auto res = GetMessageW(&msg, nullptr, 0, 0);
            if (res <= 0) {
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (m_hook) {
            UnhookWindowsHookEx(m_hook);
            m_hook = nullptr;
        }
    });
}

void WindowsGlobalKeyListener::stop() {
    if (!m_running.exchange(false)) {
        return;
    }

    if (m_threadId.load(std::memory_order_acquire) != 0) {
        PostThreadMessageW(m_threadId.load(std::memory_order_acquire), WM_QUIT, 0, 0);
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_threadId.store(0, std::memory_order_release);
}

LRESULT CALLBACK WindowsGlobalKeyListener::hookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || !g_listenerInstance) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (!g_listenerInstance->m_bindingValid.load(std::memory_order_acquire)) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const auto info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    if (info->vkCode != g_listenerInstance->m_virtualKey.load(std::memory_order_acquire)) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    switch (wParam) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        g_listenerInstance->processEvent(true);
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        g_listenerInstance->processEvent(false);
        break;
    default:
        break;
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void WindowsGlobalKeyListener::processEvent(bool pressed) {
    Callback cb;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        cb = m_callback;
    }
    if (cb) {
        cb(pressed);
    }
}

class WindowsInputSimulator : public InputSimulator {
public:
    void tapDigit(int digit) override {
        const WORD vk = digitToVirtualKey(digit);
        if (vk == 0) {
            return;
        }

        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = vk;
        inputs[0].ki.dwFlags = 0;

        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = vk;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

        SendInput(2, inputs, sizeof(INPUT));
    }

    void clickLeftButton() override {
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;

        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;

        SendInput(2, inputs, sizeof(INPUT));
    }

private:
    static WORD digitToVirtualKey(int digit) {
        if (digit < 0 || digit > 9) {
            return 0;
        }
        return static_cast<WORD>(0x30 + digit);
    }
};
#elif defined(Q_OS_LINUX)
class WaylandGlobalKeyListener : public GlobalKeyListener {
public:
    WaylandGlobalKeyListener();
    ~WaylandGlobalKeyListener() override;

    void setCallback(Callback callback) override;
    void updateBinding(const NativeBinding& binding) override;
    void start() override;
    void stop() override;

private:
    struct DeviceHandle {
        int fd = -1;
        std::string path;
    };

    void run();
    void refreshDevices();
    void closeDevices();
    void applyBinding(const NativeBinding& binding);
    void dispatch(bool pressed);
    void wake();
    void drainWakeup();
    bool handleEvent(const input_event& event);
    static bool isKeyboard(int fd);
    static bool testBit(const std::vector<unsigned long>& bits, int bit);
    static int resolveKeyCode(const NativeBinding& binding);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_bindingDirty{false};
    std::atomic<int> m_targetCode{-1};
    std::atomic<bool> m_bindingValid{false};
    std::atomic<bool> m_pressed{false};

    std::thread m_thread;
    int m_wakeupPipe[2]{-1, -1};

    std::vector<DeviceHandle> m_devices;

    mutable std::mutex m_callbackMutex;
    Callback m_callback;

    mutable std::mutex m_bindingMutex;
    NativeBinding m_binding;
};

namespace {
constexpr const char* kInputDirectory = "/dev/input";
constexpr int kPollTimeoutMs = 100;

struct PipeGuard {
    int* pipe = nullptr;
    ~PipeGuard() {
        if (!pipe) {
            return;
        }
        for (int i = 0; i < 2; ++i) {
            if (pipe[i] >= 0) {
                close(pipe[i]);
                pipe[i] = -1;
            }
        }
    }
};
}

WaylandGlobalKeyListener::WaylandGlobalKeyListener() = default;

WaylandGlobalKeyListener::~WaylandGlobalKeyListener() { stop(); }

void WaylandGlobalKeyListener::setCallback(Callback callback) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_callback = std::move(callback);
}

void WaylandGlobalKeyListener::updateBinding(const NativeBinding& binding) {
    {
        std::lock_guard<std::mutex> lock(m_bindingMutex);
        m_binding = binding;
    }
    m_bindingDirty.store(true, std::memory_order_release);
    wake();
}

void WaylandGlobalKeyListener::start() {
    if (m_running.exchange(true)) {
        return;
    }

    if (pipe(m_wakeupPipe) != 0) {
        qWarning() << "Failed to create wake-up pipe for Wayland listener";
        m_running.store(false);
        return;
    }

    for (int i = 0; i < 2; ++i) {
        if (fcntl(m_wakeupPipe[i], F_SETFL, O_NONBLOCK) < 0) {
            qWarning() << "Failed to set pipe non-blocking";
        }
    }

    m_thread = std::thread([this]() { run(); });
}

void WaylandGlobalKeyListener::stop() {
    if (!m_running.exchange(false)) {
        return;
    }

    wake();

    if (m_thread.joinable()) {
        m_thread.join();
    }

    closeDevices();

    for (int& fd : m_wakeupPipe) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
}

void WaylandGlobalKeyListener::run() {
    PipeGuard guard{m_wakeupPipe};

    refreshDevices();

    NativeBinding initialBinding;
    {
        std::lock_guard<std::mutex> lock(m_bindingMutex);
        initialBinding = m_binding;
    }
    applyBinding(initialBinding);

    std::vector<pollfd> pollFds;

    while (m_running.load(std::memory_order_acquire)) {
        if (m_bindingDirty.exchange(false, std::memory_order_acq_rel)) {
            NativeBinding binding;
            {
                std::lock_guard<std::mutex> lock(m_bindingMutex);
                binding = m_binding;
            }
            applyBinding(binding);
        }

        pollFds.clear();
        pollFds.reserve(m_devices.size() + 1);
        pollfd wakeFd{m_wakeupPipe[0], POLLIN, 0};
        pollFds.push_back(wakeFd);
        for (const auto& device : m_devices) {
            pollfd fd{device.fd, POLLIN, 0};
            pollFds.push_back(fd);
        }

        const int result = poll(pollFds.data(), static_cast<nfds_t>(pollFds.size()), kPollTimeoutMs);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            qWarning() << "poll failed for Wayland listener" << strerror(errno);
            break;
        }

        if (!m_running.load(std::memory_order_acquire)) {
            break;
        }

        if (!pollFds.empty() && (pollFds[0].revents & POLLIN)) {
            drainWakeup();
        }

        for (size_t i = 1; i < pollFds.size(); ++i) {
            if (!(pollFds[i].revents & POLLIN)) {
                continue;
            }
            const int fd = pollFds[i].fd;
            bool keepReading = true;
            while (keepReading) {
                input_event event{};
                const ssize_t bytes = read(fd, &event, sizeof(event));
                if (bytes == static_cast<ssize_t>(sizeof(event))) {
                    keepReading = handleEvent(event);
                } else if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    keepReading = false;
                } else {
                    keepReading = false;
                }
            }
        }
    }
}

void WaylandGlobalKeyListener::refreshDevices() {
    closeDevices();

    namespace fs = std::filesystem;
    std::error_code ec;
    bool permissionDenied = false;
    for (const auto& entry : fs::directory_iterator(kInputDirectory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_character_file()) {
            continue;
        }
        const auto filename = entry.path().filename().string();
        if (filename.rfind("event", 0) != 0) {
            continue;
        }

        const std::string path = entry.path().string();
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (errno == EACCES) {
                permissionDenied = true;
            }
            continue;
        }

        if (!isKeyboard(fd)) {
            close(fd);
            continue;
        }

        m_devices.push_back(DeviceHandle{fd, path});
    }

    if (m_devices.empty()) {
        qWarning() << "No readable keyboard devices found under" << kInputDirectory;
        if (permissionDenied) {
            qWarning() << "Permission denied when accessing event devices; grant access to the 'input' group.";
        }
    }
}

void WaylandGlobalKeyListener::closeDevices() {
    for (auto& device : m_devices) {
        if (device.fd >= 0) {
            close(device.fd);
            device.fd = -1;
        }
    }
    m_devices.clear();
}

void WaylandGlobalKeyListener::applyBinding(const NativeBinding& binding) {
    const int code = resolveKeyCode(binding);
    const bool valid = binding.valid && code >= 0;

    if (valid) {
        m_targetCode.store(code, std::memory_order_release);
    } else {
        m_targetCode.store(-1, std::memory_order_release);
    }

    const bool wasPressed = m_pressed.exchange(false, std::memory_order_acq_rel);
    m_bindingValid.store(valid, std::memory_order_release);

    if (wasPressed) {
        dispatch(false);
    }

    if (binding.valid && !valid) {
        qWarning() << "Unable to resolve evdev code for trigger key";
    }
}

void WaylandGlobalKeyListener::dispatch(bool pressed) {
    Callback cb;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        cb = m_callback;
    }
    if (cb) {
        cb(pressed);
    }
}

void WaylandGlobalKeyListener::wake() {
    if (m_wakeupPipe[1] < 0) {
        return;
    }
    const uint8_t byte = 1;
    const ssize_t result = write(m_wakeupPipe[1], &byte, sizeof(byte));
    (void)result;
}

void WaylandGlobalKeyListener::drainWakeup() {
    if (m_wakeupPipe[0] < 0) {
        return;
    }
    uint8_t buffer[32];
    while (read(m_wakeupPipe[0], buffer, sizeof(buffer)) > 0) {
    }
}

bool WaylandGlobalKeyListener::handleEvent(const input_event& event) {
    if (event.type != EV_KEY) {
        return true;
    }

    if (!m_bindingValid.load(std::memory_order_acquire)) {
        return true;
    }

    const int target = m_targetCode.load(std::memory_order_acquire);
    if (target < 0 || event.code != target) {
        return true;
    }

    if (event.value != 0 && event.value != 1 && event.value != 2) {
        return true;
    }

    const bool pressed = event.value != 0;
    const bool previous = m_pressed.exchange(pressed, std::memory_order_acq_rel);
    if (pressed != previous) {
        dispatch(pressed);
    }

    return true;
}

bool WaylandGlobalKeyListener::isKeyboard(int fd) {
    if (fd < 0) {
        return false;
    }

    std::vector<unsigned long> evBits((EV_MAX + sizeof(unsigned long) * 8) / (sizeof(unsigned long) * 8), 0);
    if (ioctl(fd, EVIOCGBIT(0, evBits.size() * sizeof(unsigned long)), evBits.data()) < 0) {
        return false;
    }

    if (!testBit(evBits, EV_KEY)) {
        return false;
    }

    std::vector<unsigned long> keyBits((KEY_MAX + sizeof(unsigned long) * 8) / (sizeof(unsigned long) * 8), 0);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, keyBits.size() * sizeof(unsigned long)), keyBits.data()) < 0) {
        return false;
    }

    constexpr int requiredKeys[] = {KEY_A, KEY_Z, KEY_1, KEY_9, KEY_SPACE};
    for (int key : requiredKeys) {
        if (testBit(keyBits, key)) {
            return true;
        }
    }
    return false;
}

bool WaylandGlobalKeyListener::testBit(const std::vector<unsigned long>& bits, int bit) {
    const size_t index = static_cast<size_t>(bit) / (sizeof(unsigned long) * 8);
    if (index >= bits.size()) {
        return false;
    }
    const unsigned long mask = 1UL << (bit % (sizeof(unsigned long) * 8));
    return (bits[index] & mask) != 0;
}

int WaylandGlobalKeyListener::resolveKeyCode(const NativeBinding& binding) {
    if (!binding.valid) {
        return -1;
    }

    if (binding.scanCode != 0) {
        return static_cast<int>(binding.scanCode);
    }

    if (binding.virtualKey != 0) {
        if (binding.virtualKey >= '0' && binding.virtualKey <= '9') {
            return KEY_0 + static_cast<int>(binding.virtualKey - '0');
        }
    }

    return -1;
}

class WaylandInputSimulator : public InputSimulator {
public:
    WaylandInputSimulator() = default;
    ~WaylandInputSimulator() override;

    void tapDigit(int digit) override;
    void clickLeftButton() override;

private:
    bool ensureDevice();
    void sendKey(int code, bool pressed);
    void sendButton(int code, bool pressed);
    void sendSync();
    static int digitToKey(int digit);

    int m_deviceFd = -1;
    std::atomic<bool> m_ready{false};
};

WaylandInputSimulator::~WaylandInputSimulator() {
    if (m_deviceFd >= 0) {
        ioctl(m_deviceFd, UI_DEV_DESTROY);
        close(m_deviceFd);
        m_deviceFd = -1;
    }
}

namespace {
constexpr const char* kUinputPrimary = "/dev/uinput";
constexpr const char* kUinputLegacy = "/dev/input/uinput";
}

void WaylandInputSimulator::tapDigit(int digit) {
    if (!ensureDevice()) {
        return;
    }

    const int key = digitToKey(digit);
    if (key < 0) {
        qWarning() << "Unsupported digit" << digit;
        return;
    }

    sendKey(key, true);
    sendKey(key, false);
}

void WaylandInputSimulator::clickLeftButton() {
    if (!ensureDevice()) {
        return;
    }

    sendButton(BTN_LEFT, true);
    sendButton(BTN_LEFT, false);
}

bool WaylandInputSimulator::ensureDevice() {
    if (m_ready.load(std::memory_order_acquire)) {
        return true;
    }

    if (m_deviceFd < 0) {
        m_deviceFd = open(kUinputPrimary, O_WRONLY | O_NONBLOCK);
        if (m_deviceFd < 0) {
            m_deviceFd = open(kUinputLegacy, O_WRONLY | O_NONBLOCK);
        }
        if (m_deviceFd < 0) {
            qWarning() << "Unable to open uinput device";
            return false;
        }
    }

    auto fail = [this]() {
        if (m_deviceFd >= 0) {
            close(m_deviceFd);
            m_deviceFd = -1;
        }
        m_ready.store(false, std::memory_order_release);
        return false;
    };

    auto enableKey = [this](unsigned int code) {
        if (ioctl(m_deviceFd, UI_SET_EVBIT, EV_KEY) < 0) {
            return false;
        }
        if (ioctl(m_deviceFd, UI_SET_KEYBIT, code) < 0) {
            return false;
        }
        return true;
    };

    if (!enableKey(KEY_0) || !enableKey(KEY_1) || !enableKey(KEY_2) || !enableKey(KEY_3) || !enableKey(KEY_4) ||
        !enableKey(KEY_5) || !enableKey(KEY_6) || !enableKey(KEY_7) || !enableKey(KEY_8) || !enableKey(KEY_9)) {
        qWarning() << "Failed to enable digit key events";
        return fail();
    }

    if (!enableKey(BTN_LEFT)) {
        qWarning() << "Failed to enable left button events";
        return fail();
    }

    if (ioctl(m_deviceFd, UI_SET_EVBIT, EV_SYN) < 0) {
        qWarning() << "Failed to enable sync events";
        return fail();
    }

    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1;
    setup.id.product = 0x1;
    std::snprintf(setup.name, sizeof(setup.name), "FastMeleeMacro Virtual Device");

    if (ioctl(m_deviceFd, UI_DEV_SETUP, &setup) < 0) {
        if (ioctl(m_deviceFd, UI_DEV_CREATE) < 0) {
            qWarning() << "Unable to create uinput device";
            return fail();
        }
    } else {
        if (ioctl(m_deviceFd, UI_DEV_CREATE) < 0) {
            qWarning() << "Unable to finalize uinput device";
            return fail();
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    m_ready.store(true, std::memory_order_release);
    return true;
}

void WaylandInputSimulator::sendKey(int code, bool pressed) {
    input_event event{};
    event.type = EV_KEY;
    event.code = static_cast<unsigned short>(code);
    event.value = pressed ? 1 : 0;
    if (write(m_deviceFd, &event, sizeof(event)) < 0) {
        qWarning() << "Failed to write key event" << strerror(errno);
    }
    sendSync();
}

void WaylandInputSimulator::sendButton(int code, bool pressed) {
    input_event event{};
    event.type = EV_KEY;
    event.code = static_cast<unsigned short>(code);
    event.value = pressed ? 1 : 0;
    if (write(m_deviceFd, &event, sizeof(event)) < 0) {
        qWarning() << "Failed to write button event" << strerror(errno);
    }
    sendSync();
}

void WaylandInputSimulator::sendSync() {
    input_event event{};
    event.type = EV_SYN;
    event.code = SYN_REPORT;
    event.value = 0;
    if (write(m_deviceFd, &event, sizeof(event)) < 0) {
        qWarning() << "Failed to write sync event" << strerror(errno);
    }
}

int WaylandInputSimulator::digitToKey(int digit) {
    switch (digit) {
    case 0:
        return KEY_0;
    case 1:
        return KEY_1;
    case 2:
        return KEY_2;
    case 3:
        return KEY_3;
    case 4:
        return KEY_4;
    case 5:
        return KEY_5;
    case 6:
        return KEY_6;
    case 7:
        return KEY_7;
    case 8:
        return KEY_8;
    case 9:
        return KEY_9;
    default:
        return -1;
    }
}
#else
class GlobalKeyListenerStub : public GlobalKeyListener {
public:
    void setCallback(Callback) override {}
    void updateBinding(const NativeBinding&) override {}
    void start() override {}
    void stop() override {}
};

class InputSimulatorStub : public InputSimulator {
public:
    void tapDigit(int) override {}
    void clickLeftButton() override {}
};
#endif

std::unique_ptr<InputSimulator> createSimulator() {
#ifdef Q_OS_WIN
    return std::make_unique<WindowsInputSimulator>();
#elif defined(Q_OS_LINUX)
    return std::make_unique<WaylandInputSimulator>();
#else
    return std::make_unique<InputSimulatorStub>();
#endif
}

std::unique_ptr<GlobalKeyListener> createListener() {
#ifdef Q_OS_WIN
    return std::make_unique<WindowsGlobalKeyListener>();
#elif defined(Q_OS_LINUX)
    return std::make_unique<WaylandGlobalKeyListener>();
#else
    return std::make_unique<GlobalKeyListenerStub>();
#endif
}

class MacroController {
public:
    MacroController();
    ~MacroController();

    void setSettings(const MacroSettings& settings);
    MacroSettings settings() const;

    void startListening();
    void stopListening();

private:
    void onTriggerChanged(bool pressed);
    void startMacroLoop();
    void stopMacroLoop();
    void macroLoop();
    bool waitFor(int milliseconds);
    static GlobalKeyListener::NativeBinding toNative(const MacroSettings& settings);

    std::unique_ptr<InputSimulator> m_simulator;
    std::unique_ptr<GlobalKeyListener> m_listener;

    mutable std::mutex m_settingsMutex;
    MacroSettings m_settings;

    std::atomic<bool> m_listening{false};
    std::atomic<bool> m_triggerActive{false};
    std::atomic<bool> m_macroActive{false};

    std::thread m_macroThread;
    mutable std::mutex m_macroThreadMutex;
    std::condition_variable m_stopCv;
    mutable std::mutex m_waitMutex;
};

MacroController::MacroController()
    : m_simulator(createSimulator()), m_listener(createListener()) {
    if (m_listener) {
        m_listener->setCallback([this](bool pressed) { onTriggerChanged(pressed); });
    }
}

MacroController::~MacroController() {
    stopListening();
    stopMacroLoop();
}

void MacroController::setSettings(const MacroSettings& settings) {
    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        m_settings = settings;
    }

    if (m_listener) {
        m_listener->updateBinding(toNative(settings));
    }
}

MacroSettings MacroController::settings() const {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    return m_settings;
}

void MacroController::startListening() {
    if (m_listening.exchange(true)) {
        return;
    }
    if (m_listener) {
        m_listener->updateBinding(toNative(settings()));
        m_listener->start();
    }
}

void MacroController::stopListening() {
    if (!m_listening.exchange(false)) {
        return;
    }
    if (m_listener) {
        m_listener->stop();
    }
}

void MacroController::onTriggerChanged(bool pressed) {
    if (pressed) {
        if (!m_triggerActive.exchange(true)) {
            startMacroLoop();
        }
    } else {
        m_triggerActive.store(false, std::memory_order_release);
        stopMacroLoop();
    }
}

void MacroController::startMacroLoop() {
    if (!m_simulator) {
        return;
    }

    bool expected = false;
    if (!m_macroActive.compare_exchange_strong(expected, true)) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_macroThreadMutex);
    if (m_macroThread.joinable()) {
        m_macroThread.join();
    }

    m_macroThread = std::thread([this]() { macroLoop(); });
}

void MacroController::stopMacroLoop() {
    bool expected = true;
    if (!m_macroActive.compare_exchange_strong(expected, false)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_waitMutex);
    }
    m_stopCv.notify_all();

    std::lock_guard<std::mutex> lock(m_macroThreadMutex);
    if (m_macroThread.joinable()) {
        m_macroThread.join();
    }

    m_triggerActive.store(false, std::memory_order_release);
}

void MacroController::macroLoop() {
    while (m_macroActive.load(std::memory_order_acquire)) {
        MacroSettings current = settings();
        if (!current.trigger.isValid()) {
            break;
        }

        if (!waitFor(current.delays.key2DelayMs)) {
            break;
        }
        if (!m_macroActive.load(std::memory_order_acquire)) {
            break;
        }
        m_simulator->tapDigit(2);

        if (!waitFor(current.delays.mouseDelayMs)) {
            break;
        }
        if (!m_macroActive.load(std::memory_order_acquire)) {
            break;
        }
        m_simulator->clickLeftButton();

        if (!waitFor(current.delays.finalKeyDelayMs)) {
            break;
        }
        if (!m_macroActive.load(std::memory_order_acquire)) {
            break;
        }
        m_simulator->tapDigit(current.finalKey);
    }

    m_macroActive.store(false, std::memory_order_release);
    m_triggerActive.store(false, std::memory_order_release);
    m_stopCv.notify_all();
}

bool MacroController::waitFor(int milliseconds) {
    if (milliseconds <= 0) {
        return m_macroActive.load(std::memory_order_acquire);
    }

    std::unique_lock<std::mutex> lock(m_waitMutex);
    const bool stopped = m_stopCv.wait_for(lock, std::chrono::milliseconds(milliseconds), [this]() {
        return !m_macroActive.load(std::memory_order_acquire);
    });
    return !stopped;
}

GlobalKeyListener::NativeBinding MacroController::toNative(const MacroSettings& settings) {
    GlobalKeyListener::NativeBinding binding;
    if (settings.trigger.isValid()) {
        binding.virtualKey = settings.trigger.nativeVirtualKey;
        binding.scanCode = settings.trigger.nativeScanCode;
        binding.valid = true;
    }
    return binding;
}

QString keyToText(int qtKey) {
    if (qtKey == 0) {
        return QObject::tr("Not bound");
    }
    QKeySequence sequence(qtKey);
    const auto text = sequence.toString(QKeySequence::NativeText);
    return text.isEmpty() ? QObject::tr("Key %1").arg(qtKey) : text;
}

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    void setupUi();
    void loadSettings();
    void applySettings();
    void updateUiFromSettings();
    void startKeyCapture();
    void finishKeyCapture();

    void handleKey2DelayChanged(int value);
    void handleMouseDelayChanged(int value);
    void handleFinalDelayChanged(int value);
    void handleFinalKeyChanged(int index);
    void saveProfile();

    std::unique_ptr<MacroController> m_macroController;
    std::unique_ptr<ConfigManager> m_configManager;

    MacroSettings m_settings;
    bool m_capturing = false;
    bool m_updatingUi = false;

    QPushButton* m_bindButton = nullptr;
    QLabel* m_bindingLabel = nullptr;
    QSpinBox* m_key2DelaySpin = nullptr;
    QSpinBox* m_mouseDelaySpin = nullptr;
    QSpinBox* m_finalDelaySpin = nullptr;
    QComboBox* m_finalKeyCombo = nullptr;
    QPushButton* m_saveButton = nullptr;
};

constexpr int kMaxDelayMs = 5000;

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_macroController(std::make_unique<MacroController>()),
      m_configManager(std::make_unique<ConfigManager>()) {
    setWindowTitle(tr("Fast Melee Macro"));
    setMinimumSize(420, 280);

    setupUi();
    loadSettings();
    applySettings();
    statusBar()->showMessage(tr("Hold the bound key to start the macro."));

    m_macroController->startListening();
}

MainWindow::~MainWindow() {
    if (m_macroController) {
        m_macroController->stopListening();
    }
}

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    auto* bindingGroup = new QGroupBox(tr("Trigger key"), central);
    auto* bindingLayout = new QHBoxLayout(bindingGroup);

    m_bindButton = new QPushButton(tr("Bind key"), bindingGroup);
    m_bindingLabel = new QLabel(tr("Not bound"), bindingGroup);
    m_bindingLabel->setMinimumWidth(160);

    bindingLayout->addWidget(m_bindButton);
    bindingLayout->addWidget(m_bindingLabel);
    bindingLayout->addStretch();

    bindingGroup->setLayout(bindingLayout);
    layout->addWidget(bindingGroup);

    auto* macroGroup = new QGroupBox(tr("Macro configuration"), central);
    auto* form = new QFormLayout(macroGroup);

    m_key2DelaySpin = new QSpinBox(macroGroup);
    m_key2DelaySpin->setRange(0, kMaxDelayMs);
    m_key2DelaySpin->setSuffix(tr(" ms"));
    form->addRow(tr("Delay before key 2"), m_key2DelaySpin);

    m_mouseDelaySpin = new QSpinBox(macroGroup);
    m_mouseDelaySpin->setRange(0, kMaxDelayMs);
    m_mouseDelaySpin->setSuffix(tr(" ms"));
    form->addRow(tr("Delay before left click"), m_mouseDelaySpin);

    m_finalDelaySpin = new QSpinBox(macroGroup);
    m_finalDelaySpin->setRange(0, kMaxDelayMs);
    m_finalDelaySpin->setSuffix(tr(" ms"));
    form->addRow(tr("Delay before final key"), m_finalDelaySpin);

    m_finalKeyCombo = new QComboBox(macroGroup);
    m_finalKeyCombo->addItem(QStringLiteral("1"), 1);
    m_finalKeyCombo->addItem(QStringLiteral("4"), 4);
    m_finalKeyCombo->addItem(QStringLiteral("5"), 5);
    form->addRow(tr("Final key"), m_finalKeyCombo);

    macroGroup->setLayout(form);
    layout->addWidget(macroGroup);

    m_saveButton = new QPushButton(tr("Save profile"), central);
    layout->addWidget(m_saveButton);
    layout->addStretch();

    setCentralWidget(central);

    connect(m_bindButton, &QPushButton::clicked, this, &MainWindow::startKeyCapture);
    connect(m_key2DelaySpin, &QSpinBox::valueChanged, this, &MainWindow::handleKey2DelayChanged);
    connect(m_mouseDelaySpin, &QSpinBox::valueChanged, this, &MainWindow::handleMouseDelayChanged);
    connect(m_finalDelaySpin, &QSpinBox::valueChanged, this, &MainWindow::handleFinalDelayChanged);
    connect(m_finalKeyCombo, &QComboBox::currentIndexChanged, this, &MainWindow::handleFinalKeyChanged);
    connect(m_saveButton, &QPushButton::clicked, this, &MainWindow::saveProfile);
}

void MainWindow::loadSettings() {
    m_settings = m_configManager->load();
    updateUiFromSettings();
}

void MainWindow::applySettings() {
    if (m_macroController) {
        m_macroController->setSettings(m_settings);
    }
}

void MainWindow::updateUiFromSettings() {
    m_updatingUi = true;

    if (m_settings.trigger.displayName.isEmpty()) {
        m_bindingLabel->setText(keyToText(m_settings.trigger.qtKey));
    } else {
        m_bindingLabel->setText(m_settings.trigger.displayName);
    }
    m_key2DelaySpin->setValue(m_settings.delays.key2DelayMs);
    m_mouseDelaySpin->setValue(m_settings.delays.mouseDelayMs);
    m_finalDelaySpin->setValue(m_settings.delays.finalKeyDelayMs);

    const int index = m_finalKeyCombo->findData(m_settings.finalKey);
    if (index >= 0) {
        m_finalKeyCombo->setCurrentIndex(index);
    } else {
        m_finalKeyCombo->setCurrentIndex(0);
        m_settings.finalKey = m_finalKeyCombo->itemData(0).toInt();
    }

    m_updatingUi = false;
}

void MainWindow::startKeyCapture() {
    if (m_capturing) {
        return;
    }

    m_capturing = true;
    m_bindingLabel->setText(tr("Press a key..."));
    statusBar()->showMessage(tr("Press a key to assign as the macro trigger."), 3000);
    grabKeyboard();
    setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::finishKeyCapture() {
    if (!m_capturing) {
        return;
    }
    m_capturing = false;
    releaseKeyboard();
    statusBar()->showMessage(tr("Hold the bound key to start the macro."), 3000);
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (m_capturing) {
        event->accept();
        if (event->isAutoRepeat()) {
            return;
        }
        if (event->modifiers() != Qt::NoModifier) {
            statusBar()->showMessage(tr("Only single keys without modifiers are supported."), 4000);
            m_settings.trigger = KeyBinding{};
            updateUiFromSettings();
            applySettings();
            finishKeyCapture();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            finishKeyCapture();
            return;
        }

        m_settings.trigger.qtKey = event->key();
        m_settings.trigger.nativeScanCode = static_cast<quint32>(event->nativeScanCode());
        m_settings.trigger.nativeVirtualKey = static_cast<quint32>(event->nativeVirtualKey());
        m_settings.trigger.nativeModifiers = static_cast<quint32>(event->nativeModifiers());
        m_settings.trigger.displayName = QKeySequence(event->key()).toString(QKeySequence::NativeText);
        if (m_settings.trigger.displayName.isEmpty()) {
            m_settings.trigger.displayName = keyToText(event->key());
        }

        updateUiFromSettings();
        applySettings();
        finishKeyCapture();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event) {
    if (m_capturing) {
        event->accept();
        return;
    }
    QMainWindow::keyReleaseEvent(event);
}

void MainWindow::handleKey2DelayChanged(int value) {
    if (m_updatingUi) {
        return;
    }
    m_settings.delays.key2DelayMs = value;
    applySettings();
}

void MainWindow::handleMouseDelayChanged(int value) {
    if (m_updatingUi) {
        return;
    }
    m_settings.delays.mouseDelayMs = value;
    applySettings();
}

void MainWindow::handleFinalDelayChanged(int value) {
    if (m_updatingUi) {
        return;
    }
    m_settings.delays.finalKeyDelayMs = value;
    applySettings();
}

void MainWindow::handleFinalKeyChanged(int index) {
    if (m_updatingUi) {
        return;
    }
    const auto value = m_finalKeyCombo->itemData(index).toInt();
    m_settings.finalKey = value;
    applySettings();
}

void MainWindow::saveProfile() {
    if (m_configManager->save(m_settings)) {
        statusBar()->showMessage(tr("Profile saved."), 3000);
    } else {
        statusBar()->showMessage(tr("Failed to save profile."), 3000);
    }
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QIcon icon(QStringLiteral(":/icons/app_icon.png"));
    if (!icon.isNull()) {
        QApplication::setWindowIcon(icon);
    }

    MainWindow window;
    window.show();

    return app.exec();
}

#include "main.moc"
