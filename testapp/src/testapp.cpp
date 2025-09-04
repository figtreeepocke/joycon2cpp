#ifdef _WIN32
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#pragma comment(lib, "setupapi.lib")
#include <iostream>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <memory>
#include "JoyConDecoder.h"
#include <Windows.h>

#include <ViGEm/Client.h>
#include <ViGEm/Common.h>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
using namespace Windows::Foundation;

constexpr uint16_t JOYCON_MANUFACTURER_ID = 1363; // Nintendo
const std::vector<uint8_t> JOYCON_MANUFACTURER_PREFIX = { 0x01, 0x00, 0x03, 0x7E };
const wchar_t* INPUT_REPORT_UUID = L"ab7de9be-89fe-49ad-828f-118f09df7fd2";
const wchar_t* WRITE_COMMAND_UUID = L"649d4ac9-8eb7-4e6c-af44-1ea54fe5f005";

PVIGEM_CLIENT vigem_client = nullptr;

void InitializeViGEm()
{
    if (vigem_client != nullptr)
        return;

    vigem_client = vigem_alloc();
    if (vigem_client == nullptr)
    {
        std::wcerr << L"Failed to allocate ViGEm client.\n";
        exit(1);
    }

    auto ret = vigem_connect(vigem_client);
    if (!VIGEM_SUCCESS(ret))
    {
        std::wcerr << L"Failed to connect to ViGEm bus: 0x" << std::hex << ret << L"\n";
        exit(1);
    }

    std::wcout << L"ViGEm client initialized and connected.\n";
}

void PrintRawNotification(const std::vector<uint8_t>& buffer)
{
    std::cout << "[Raw Notification] ";
    for (auto b : buffer) {
        printf("%02X ", b);
    }
    std::cout << std::endl;
}

void SendCustomCommands(GattCharacteristic const& characteristic)
{
    std::vector<std::vector<uint8_t>> commands = {
        { 0x0c, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00 },
        { 0x0c, 0x91, 0x01, 0x04, 0x00, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00 }
    };

    for (const auto& cmd : commands)
    {
        auto writer = DataWriter();
        writer.WriteBytes(cmd);
        IBuffer buffer = writer.DetachBuffer();

        auto status = characteristic.WriteValueAsync(buffer, GattWriteOption::WriteWithoutResponse).get();

        if (status == GattCommunicationStatus::Success)
        {
            std::wcout << L"Command sent successfully.\n";
        }
        else
        {
            std::wcout << L"Failed to send command.\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

struct ConnectedJoyCon {
    BluetoothLEDevice device = nullptr;
    GattCharacteristic inputChar = nullptr;
    GattCharacteristic writeChar = nullptr;
};

ConnectedJoyCon WaitForJoyCon(const std::wstring& prompt)
{
    std::wcout << prompt << L"\n";

    ConnectedJoyCon cj{};

    BluetoothLEDevice device = nullptr;
    bool connected = false;

    BluetoothLEAdvertisementWatcher watcher;

    std::mutex mtx;
    std::condition_variable cv;

    watcher.Received([&](auto const&, auto const& args)
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (connected) return;

            auto mfg = args.Advertisement().ManufacturerData();
            for (uint32_t i = 0; i < mfg.Size(); i++)
            {
                auto section = mfg.GetAt(i);
                if (section.CompanyId() != JOYCON_MANUFACTURER_ID) continue;
                auto reader = DataReader::FromBuffer(section.Data());
                std::vector<uint8_t> data(reader.UnconsumedBufferLength());
                reader.ReadBytes(data);
                if (data.size() >= JOYCON_MANUFACTURER_PREFIX.size() &&
                    std::equal(JOYCON_MANUFACTURER_PREFIX.begin(), JOYCON_MANUFACTURER_PREFIX.end(), data.begin()))
                {
                    device = BluetoothLEDevice::FromBluetoothAddressAsync(args.BluetoothAddress()).get();
                    if (!device) return;

                    connected = true;
                    watcher.Stop();
                    cv.notify_one();
                    return;
                }
            }
        });

    watcher.ScanningMode(BluetoothLEScanningMode::Active);
    watcher.Start();

    std::wcout << L"Scanning for Joy-Con... (Waiting up to 30 seconds)\n";

    {
        std::unique_lock<std::mutex> lock(mtx);
        if (!cv.wait_for(lock, std::chrono::seconds(30), [&]() { return connected; }))
        {
            watcher.Stop();
            std::wcerr << L"Timeout: Joy-Con not found.\n";
            exit(1);
        }
    }

    cj.device = device;

    auto servicesResult = device.GetGattServicesAsync().get();
    if (servicesResult.Status() != GattCommunicationStatus::Success)
    {
        std::wcerr << L"Failed to get GATT services.\n";
        exit(1);
    }

    for (auto service : servicesResult.Services())
    {
        auto charsResult = service.GetCharacteristicsAsync().get();
        if (charsResult.Status() != GattCommunicationStatus::Success) continue;
        for (auto characteristic : charsResult.Characteristics())
        {
            if (characteristic.Uuid() == guid(INPUT_REPORT_UUID))
                cj.inputChar = characteristic;
            else if (characteristic.Uuid() == guid(WRITE_COMMAND_UUID))
                cj.writeChar = characteristic;
        }
    }

    return cj;
}

enum ControllerType {
    SingleJoyCon = 1,
    DualJoyCon = 2,
    ProController = 3,
    NSOGCController = 4
};

struct PlayerConfig {
    ControllerType controllerType;
    JoyConSide joyconSide;
    JoyConOrientation joyconOrientation;
};

// For single Joy-Con players, store controller + JoyCon info to keep alive
struct SingleJoyConPlayer {
    ConnectedJoyCon joycon;
    PVIGEM_TARGET ds4Controller;
    JoyConSide side;
    JoyConOrientation orientation;
};

// For dual Joy-Con players, store both JoyCons, controller, thread, and running flag
struct DualJoyConPlayer {
    ConnectedJoyCon leftJoyCon;
    ConnectedJoyCon rightJoyCon;
    PVIGEM_TARGET ds4Controller;
    std::atomic<bool> running;
    std::thread updateThread;
};

// For Pro Controller players
struct ProControllerPlayer {
    ConnectedJoyCon controller;
    PVIGEM_TARGET ds4Controller;
};

// Declare the Pro Controller report generator (implement in JoyConDecoder.cpp)
DS4_REPORT_EX GenerateProControllerReport(const std::vector<uint8_t>& buffer);

int main()
{
    init_apartment();

    int numPlayers;
    std::wcout << L"How many players? ";
    std::wcin >> numPlayers;
    std::wcin.ignore();

    std::vector<PlayerConfig> playerConfigs;

    for (int i = 0; i < numPlayers; ++i) {
        PlayerConfig config{};
        std::wstring line;

        while (true) {
            std::wcout << L"Player " << (i + 1) << L":\n";
            std::wcout << L"  What controller type? (1=Single JoyCon, 2=Dual JoyCon, 3=Pro Controller, 4=NSO GC Controller): ";
            std::getline(std::wcin, line);
            if (line == L"1" || line == L"2" || line == L"3" || line == L"4") {
                config.controllerType = static_cast<ControllerType>(std::stoi(std::string(line.begin(), line.end())));
                break;
            }
            std::wcout << L"Invalid input. Please enter 1, 2, or 3.\n";
        }

        if (config.controllerType == SingleJoyCon) {
            while (true) {
                std::wcout << L"  Which side? (L=Left, R=Right): ";
                std::getline(std::wcin, line);
                if (line == L"L" || line == L"R" || line == L"l" || line == L"r") {
                    config.joyconSide = (line == L"L" || line == L"l") ? JoyConSide::Left : JoyConSide::Right;
                    break;
                }
                std::wcout << L"Invalid input. Please enter L or R.\n";
            }
            while (true) {
                std::wcout << L"  What orientation? (U=Upright, S=Sideways): ";
                std::getline(std::wcin, line);
                if (line == L"U" || line == L"S" || line == L"u" || line == L"s") {
                    config.joyconOrientation = (line == L"S" || line == L"s") ? JoyConOrientation::Sideways : JoyConOrientation::Upright;
                    break;
                }
                std::wcout << L"Invalid input. Please enter U or S.\n";
            }
        }
        else if (config.controllerType == DualJoyCon) {
            config.joyconSide = JoyConSide::Left;
            config.joyconOrientation = JoyConOrientation::Upright;
        }

        playerConfigs.push_back(config);
    }

    InitializeViGEm();

    // Store all players to keep them alive
    std::vector<SingleJoyConPlayer> singlePlayers;
    std::vector<std::unique_ptr<DualJoyConPlayer>> dualPlayers;
    std::vector<ProControllerPlayer> proPlayers;

    for (int i = 0; i < numPlayers; ++i) {
        auto& config = playerConfigs[i];
        std::wcout << L"Player " << (i + 1) << L" setup...\n";

        if (config.controllerType == SingleJoyCon) {
            std::wstring sideStr = (config.joyconSide == JoyConSide::Left) ? L"Left" : L"Right";
            std::wcout << L"Please sync your single Joy-Con (" << sideStr << L") now.\n";

            ConnectedJoyCon cj = WaitForJoyCon(L"Waiting for single Joy-Con...");

            PVIGEM_TARGET ds4_controller = vigem_target_ds4_alloc();
            auto ret = vigem_target_add(vigem_client, ds4_controller);
            if (!VIGEM_SUCCESS(ret))
            {
                std::wcerr << L"Failed to add DS4 controller target: 0x" << std::hex << ret << L"\n";
                exit(1);
            }

            singlePlayers.push_back({ cj, ds4_controller, config.joyconSide, config.joyconOrientation });
            auto& player = singlePlayers.back();

            player.joycon.inputChar.ValueChanged([joyconSide = player.side, joyconOrientation = player.orientation, &player](GattCharacteristic const&, GattValueChangedEventArgs const& args)
                {
                    auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                    std::vector<uint8_t> buffer(reader.UnconsumedBufferLength());
                    reader.ReadBytes(buffer);

                    DS4_REPORT_EX report = GenerateDS4Report(buffer, joyconSide, joyconOrientation);

                    auto ret = vigem_target_ds4_update_ex(vigem_client, player.ds4Controller, report);
                    if (!VIGEM_SUCCESS(ret)) {
                        std::wcerr << L"Failed to update DS4 EX report: 0x" << std::hex << ret << L"\n";
                    }
                });

            auto status = player.joycon.inputChar.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify).get();

            if (player.joycon.writeChar)
                SendCustomCommands(player.joycon.writeChar);

            if (status == GattCommunicationStatus::Success)
                std::wcout << L"Notifications enabled.\n";
            else
                std::wcout << L"Failed to enable notifications.\n";

            std::wcout << L"Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);
        }
        else if (config.controllerType == DualJoyCon) {
            std::wcout << L"Please sync your RIGHT Joy-Con now.\n";
            ConnectedJoyCon rightJoyCon = WaitForJoyCon(L"Waiting for RIGHT Joy-Con...");
            if (rightJoyCon.writeChar)
                SendCustomCommands(rightJoyCon.writeChar);

            std::wcout << L"Please sync your LEFT Joy-Con now.\n";
            ConnectedJoyCon leftJoyCon = WaitForJoyCon(L"Waiting for LEFT Joy-Con...");
            if (leftJoyCon.writeChar)
                SendCustomCommands(leftJoyCon.writeChar);

            PVIGEM_TARGET ds4Controller = vigem_target_ds4_alloc();
            auto ret = vigem_target_add(vigem_client, ds4Controller);
            if (!VIGEM_SUCCESS(ret))
            {
                std::wcerr << L"Failed to add DS4 controller target: 0x" << std::hex << ret << L"\n";
                exit(1);
            }

            auto dualPlayer = std::make_unique<DualJoyConPlayer>();
            dualPlayer->leftJoyCon = leftJoyCon;
            dualPlayer->rightJoyCon = rightJoyCon;
            dualPlayer->ds4Controller = ds4Controller;
            dualPlayer->running.store(true);

            std::atomic<std::shared_ptr<std::vector<uint8_t>>> leftBufferAtomic{ std::make_shared<std::vector<uint8_t>>() };
            std::atomic<std::shared_ptr<std::vector<uint8_t>>> rightBufferAtomic{ std::make_shared<std::vector<uint8_t>>() };

            dualPlayer->leftJoyCon.inputChar.ValueChanged([&leftBufferAtomic](GattCharacteristic const&, GattValueChangedEventArgs const& args)
                {
                    auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                    auto buf = std::make_shared<std::vector<uint8_t>>(reader.UnconsumedBufferLength());
                    reader.ReadBytes(*buf);
                    leftBufferAtomic.store(buf, std::memory_order_release);
                });

            auto statusLeft = dualPlayer->leftJoyCon.inputChar.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify).get();

            if (statusLeft == GattCommunicationStatus::Success)
                std::wcout << L"LEFT Joy-Con notifications enabled.\n";
            else
                std::wcout << L"Failed to enable LEFT Joy-Con notifications.\n";

            dualPlayer->rightJoyCon.inputChar.ValueChanged([&rightBufferAtomic](GattCharacteristic const&, GattValueChangedEventArgs const& args)
                {
                    auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                    auto buf = std::make_shared<std::vector<uint8_t>>(reader.UnconsumedBufferLength());
                    reader.ReadBytes(*buf);
                    rightBufferAtomic.store(buf, std::memory_order_release);
                });

            auto statusRight = dualPlayer->rightJoyCon.inputChar.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify).get();

            if (statusRight == GattCommunicationStatus::Success)
                std::wcout << L"RIGHT Joy-Con notifications enabled.\n";
            else
                std::wcout << L"Failed to enable RIGHT Joy-Con notifications.\n";

            dualPlayer->updateThread = std::thread([dualPlayerPtr = dualPlayer.get(), &leftBufferAtomic, &rightBufferAtomic]()
                {
                    while (dualPlayerPtr->running.load(std::memory_order_acquire))
                    {
                        auto leftBuf = leftBufferAtomic.load(std::memory_order_acquire);
                        auto rightBuf = rightBufferAtomic.load(std::memory_order_acquire);

                        if (leftBuf->empty() || rightBuf->empty())
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            continue;
                        }

                        DS4_REPORT_EX report = GenerateDualJoyConDS4Report(*leftBuf, *rightBuf);

                        auto ret = vigem_target_ds4_update_ex(vigem_client, dualPlayerPtr->ds4Controller, report);
                        if (!VIGEM_SUCCESS(ret))
                        {
                            std::wcerr << L"Failed to update DS4 report: 0x" << std::hex << ret << L"\n";
                        }

                        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60Hz
                    }
                });

            dualPlayers.push_back(std::move(dualPlayer));

            std::wcout << L"Dual Joy-Cons connected and configured. Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);
        }
        else if (config.controllerType == ProController) {
            std::wcout << L"Please sync your Pro Controller now.\n";

            ConnectedJoyCon proController = WaitForJoyCon(L"Waiting for Pro Controller...");

            PVIGEM_TARGET ds4_controller = vigem_target_ds4_alloc();
            auto ret = vigem_target_add(vigem_client, ds4_controller);
            if (!VIGEM_SUCCESS(ret))
            {
                std::wcerr << L"Failed to add DS4 controller target: 0x" << std::hex << ret << L"\n";
                exit(1);
            }

            proController.inputChar.ValueChanged([ds4_controller](GattCharacteristic const&, GattValueChangedEventArgs const& args) mutable
                {
                    auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                    std::vector<uint8_t> buffer(reader.UnconsumedBufferLength());
                    reader.ReadBytes(buffer);


                    DS4_REPORT_EX report = GenerateProControllerReport(buffer);

                    auto ret = vigem_target_ds4_update_ex(vigem_client, ds4_controller, report);
                    if (!VIGEM_SUCCESS(ret)) {
                        std::wcerr << L"Failed to update DS4 EX report: 0x" << std::hex << ret << L"\n";
                    }
                });

            auto status = proController.inputChar.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify).get();

            if (proController.writeChar)
                SendCustomCommands(proController.writeChar);

            if (status == GattCommunicationStatus::Success)
                std::wcout << L"Pro Controller notifications enabled.\n";
            else
                std::wcout << L"Failed to enable Pro Controller notifications.\n";

            std::wcout << L"Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);

            proPlayers.push_back({ proController, ds4_controller });
        }
        else if (config.controllerType == NSOGCController) {
            std::wcout << L"Please sync your NSO GameCube Controller now.\n";

            ConnectedJoyCon gcController = WaitForJoyCon(L"Waiting for NSO GC Controller...");

            PVIGEM_TARGET ds4_controller = vigem_target_ds4_alloc();
            auto ret = vigem_target_add(vigem_client, ds4_controller);
            if (!VIGEM_SUCCESS(ret)) {
                std::wcerr << L"Failed to add DS4 controller target: 0x" << std::hex << ret << L"\n";
                exit(1);
            }

            gcController.inputChar.ValueChanged([ds4_controller](GattCharacteristic const&, GattValueChangedEventArgs const& args) mutable {
                auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                std::vector<uint8_t> buffer(reader.UnconsumedBufferLength());
                reader.ReadBytes(buffer);

                DS4_REPORT_EX report = GenerateNSOGCReport(buffer);

                auto ret = vigem_target_ds4_update_ex(vigem_client, ds4_controller, report);
                if (!VIGEM_SUCCESS(ret)) {
                    std::wcerr << L"Failed to update DS4 EX report: 0x" << std::hex << ret << L"\n";
                }
                });

            auto status = gcController.inputChar.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify).get();

            if (gcController.writeChar)
                SendCustomCommands(gcController.writeChar); // Optional, only if NSO GC expects init commands

            if (status == GattCommunicationStatus::Success)
                std::wcout << L"NSO GC Controller notifications enabled.\n";
            else
                std::wcout << L"Failed to enable NSO GC Controller notifications.\n";

            std::wcout << L"Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);

            proPlayers.push_back({ gcController, ds4_controller }); // reuse ProControllerPlayer struct
}
    }

    std::wcout << L"All players connected. Press Enter to exit...\n";
    std::wstring dummy;
    std::getline(std::wcin, dummy);

    // Clean up dual player threads & free controllers
    for (auto& dp : dualPlayers)
    {
        dp->running.store(false);
        if (dp->updateThread.joinable())
            dp->updateThread.join();

        vigem_target_remove(vigem_client, dp->ds4Controller);
        vigem_target_free(dp->ds4Controller);
    }

    // Free single players controllers
    for (auto& sp : singlePlayers)
    {
        vigem_target_remove(vigem_client, sp.ds4Controller);
        vigem_target_free(sp.ds4Controller);
    }

    // Free Pro Controllers
    for (auto& pp : proPlayers)
    {
        vigem_target_remove(vigem_client, pp.ds4Controller);
        vigem_target_free(pp.ds4Controller);
    }

    if (vigem_client)
    {
        vigem_disconnect(vigem_client);
        vigem_free(vigem_client);
        vigem_client = nullptr;
    }

    return 0;
}
#else
#include <fcntl.h>
#include <unistd.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <string>
#include <cstring>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <dirent.h>

enum JoyConSide { Left, Right };
enum JoyConOrientation { Upright, Sideways };

static const uint16_t JOYCON_VENDOR_ID = 0x057e;
static const uint16_t JOYCON_L_PRODUCT_ID = 0x2006;
static const uint16_t JOYCON_R_PRODUCT_ID = 0x2007;
static const uint16_t PRO_CONTROLLER_PRODUCT_ID = 0x2009;
static const uint16_t NSO_GC_PRODUCT_ID = 0x200e;

struct JoyConDevice {
    std::string path;
    int fd;
    bool isPro;
    bool isNSOGC;
    JoyConSide side;
    JoyConOrientation orientation;
    bool partOfDual;
};

struct VirtualController {
    int uinput_fd;
    std::mutex write_mutex;
};

void emitEvent(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ie;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ie.time = tv;
    ie.type = type;
    ie.code = code;
    ie.value = value;
    if (write(fd, &ie, sizeof(ie)) < 0) {
        std::cerr << "Failed to write uinput event." << std::endl;
    }
}

void initializeVirtualController(VirtualController &vc) {
    vc.uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (vc.uinput_fd < 0) {
        perror("open /dev/uinput");
        exit(1);
    }
    // Enable event types
    ioctl(vc.uinput_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(vc.uinput_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(vc.uinput_fd, UI_SET_EVBIT, EV_SYN);
    // Enable buttons
    int keys[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
                   BTN_TL, BTN_TR,
                   BTN_SELECT, BTN_START,
                   BTN_MODE,
                   BTN_THUMBL, BTN_THUMBR };
    for (int code : keys) {
        ioctl(vc.uinput_fd, UI_SET_KEYBIT, code);
    }
    // Enable axes
    int axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y };
    for (int code : axes) {
        ioctl(vc.uinput_fd, UI_SET_ABSBIT, code);
    }
    // Configure axis ranges
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Virtual JoyCon Controller");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x057e;
    uidev.id.product = 0x200e;
    uidev.id.version = 0x0001;
    // Left stick and right stick axes 0-65535
    uidev.absmin[ABS_X] = 0;    uidev.absmax[ABS_X] = 65535;
    uidev.absmin[ABS_Y] = 0;    uidev.absmax[ABS_Y] = 65535;
    uidev.absmin[ABS_RX] = 0;   uidev.absmax[ABS_RX] = 65535;
    uidev.absmin[ABS_RY] = 0;   uidev.absmax[ABS_RY] = 65535;
    // Trigger axes 0-255
    uidev.absmin[ABS_Z] = 0;    uidev.absmax[ABS_Z] = 255;
    uidev.absmin[ABS_RZ] = 0;   uidev.absmax[ABS_RZ] = 255;
    // D-Pad hat -1 to 1
    uidev.absmin[ABS_HAT0X] = -1; uidev.absmax[ABS_HAT0X] = 1;
    uidev.absmin[ABS_HAT0Y] = -1; uidev.absmax[ABS_HAT0Y] = 1;
    // Write device configuration to uinput
    if (write(vc.uinput_fd, &uidev, sizeof(uidev)) < 0) {
        perror("write uidev");
        exit(1);
    }
    if (ioctl(vc.uinput_fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        exit(1);
    }
    std::cout << "Virtual controller created via uinput." << std::endl;
}

// Helper to check if a device supports EV_KEY events (to filter out sensor-only interfaces)
bool deviceHasKeys(int fd) {
    unsigned long evbit[EV_MAX/ (8 * sizeof(unsigned long)) + 1];
    memset(evbit, 0, sizeof(evbit));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) {
        return false;
    }
    return evbit[EV_KEY / (8 * sizeof(unsigned long))] & (1UL << (EV_KEY % (8 * sizeof(unsigned long))));
}

static std::vector<std::string> usedDevices; // track used device paths to avoid duplicates

std::string findDeviceByVendorProduct(uint16_t vendor, uint16_t product, bool requireKeys = true) {
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        perror("opendir /dev/input");
        exit(1);
    }
    struct dirent *ent;
    std::string result;
    while ((ent = readdir(dir)) != NULL) {
        std::string name = ent->d_name;
        if (name.rfind("event", 0) == 0) {
            std::string path = "/dev/input/" + name;
            // Skip if already used
            if (std::find(usedDevices.begin(), usedDevices.end(), path) != usedDevices.end()) {
                continue;
            }
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            struct input_id dev_id;
            if (ioctl(fd, EVIOCGID, &dev_id) >= 0) {
                if (dev_id.vendor == vendor && dev_id.product == product) {
                    bool ok = true;
                    if (requireKeys && !deviceHasKeys(fd)) {
                        ok = false;
                    }
                    close(fd);
                    if (ok) {
                        result = path;
                        break;
                    }
                }
            }
            close(fd);
        }
    }
    closedir(dir);
    return result;
}

JoyConDevice waitForJoyConDevice(uint16_t vendor, uint16_t product, JoyConSide side = Left, JoyConOrientation orientation = Upright, bool partOfDual = false) {
    std::wcout << L"Waiting for device (vendor 0x" << std::hex << vendor << L", product 0x" << product << L")..." << std::endl;
    std::string path;
    for (int i = 0; i < 300; ++i) { // poll for up to 30 seconds
        path = findDeviceByVendorProduct(vendor, product, true);
        if (!path.empty()) break;
        usleep(100000); // wait 100ms
    }
    if (path.empty()) {
        std::wcerr << L"Timeout: device (vendor 0x" << std::hex << vendor << L", product 0x" << product << L") not found.\n";
        exit(1);
    }
    usedDevices.push_back(path);
    JoyConDevice jc;
    jc.path = path;
    jc.fd = open(path.c_str(), O_RDONLY);
    if (jc.fd < 0) {
        std::cerr << "Failed to open " << path << std::endl;
        exit(1);
    }
    jc.isPro = (product == PRO_CONTROLLER_PRODUCT_ID);
    jc.isNSOGC = (product == NSO_GC_PRODUCT_ID);
    jc.side = side;
    jc.orientation = orientation;
    jc.partOfDual = partOfDual;
    std::wcout << L"Found device " << path.c_str() << L" for Joy-Con/Controller.\n";
    return jc;
}

int main() {
    // Prompt number of players
    int numPlayers;
    std::cout << "How many players? ";
    std::cin >> numPlayers;
    std::cin.ignore();
    std::vector<JoyConDevice> joyconDevices;
    std::vector<VirtualController> virtualControllers;
    virtualControllers.reserve(numPlayers);

    for (int i = 0; i < numPlayers; ++i) {
        std::wstring line;
        int controllerType;
        std::wcout << L"Player " << (i+1) << L":\n";
        std::wcout << L"  What controller type? (1=Single JoyCon, 2=Dual JoyCon, 3=Pro Controller, 4=NSO GC Controller): ";
        std::wcin >> controllerType;
        std::wcin.ignore();
        JoyConSide side;
        JoyConOrientation orientation;
        if (controllerType == 1) { // Single JoyCon
            while (true) {
                std::wcout << L"  Which side? (L=Left, R=Right): ";
                std::getline(std::wcin, line);
                if (line == L"L" || line == L"l" || line == L"R" || line == L"r") {
                    side = (line[0] == L'L' || line[0] == L'l') ? Left : Right;
                    break;
                }
                std::wcout << L"Invalid input. Please enter L or R.\n";
            }
            while (true) {
                std::wcout << L"  What orientation? (U=Upright, S=Sideways): ";
                std::getline(std::wcin, line);
                if (line == L"U" || line == L"u" || line == L"S" || line == L"s") {
                    orientation = (line[0] == L'S' || line[0] == L's') ? Sideways : Upright;
                    break;
                }
                std::wcout << L"Invalid input. Please enter U or S.\n";
            }
            // Connect to the Joy-Con device
            uint16_t prod = (side == Left ? JOYCON_L_PRODUCT_ID : JOYCON_R_PRODUCT_ID);
            JoyConDevice jc = waitForJoyConDevice(JOYCON_VENDOR_ID, prod, side, orientation, false);
            joyconDevices.push_back(jc);
            // Create a virtual controller for this player
            virtualControllers.emplace_back();
            initializeVirtualController(virtualControllers.back());
            // Spawn a thread to handle this Joy-Con's input events
            JoyConDevice *jd = &joyconDevices.back();
            VirtualController *vc = &virtualControllers.back();
            std::thread([jd, vc]() {
                struct input_event ev;
                while (true) {
                    ssize_t rb = read(jd->fd, &ev, sizeof(ev));
                    if (rb <= 0) {
                        if (rb < 0 && errno == EAGAIN) {
                            // No immediate event, continue polling
                            usleep(1000);
                            continue;
                        }
                        break; // device disconnected or error
                    }
                    if (ev.type == EV_KEY) {
                        uint16_t code = ev.code;
                        int32_t val = ev.value;
                        // Face buttons or D-Pad
                        if (code == BTN_SOUTH || code == BTN_EAST || code == BTN_NORTH || code == BTN_WEST) {
                            if (jd->side == Left && jd->orientation == Upright) {
                                // Map D-Pad (Left Joy-Con upright) to hat axes
                                if (code == BTN_WEST) { // left
                                    emitEvent(vc->uinput_fd, EV_ABS, ABS_HAT0X, val ? -1 : 0);
                                } else if (code == BTN_EAST) { // right
                                    emitEvent(vc->uinput_fd, EV_ABS, ABS_HAT0X, val ? 1 : 0);
                                } else if (code == BTN_NORTH) { // up
                                    emitEvent(vc->uinput_fd, EV_ABS, ABS_HAT0Y, val ? -1 : 0);
                                } else if (code == BTN_SOUTH) { // down
                                    emitEvent(vc->uinput_fd, EV_ABS, ABS_HAT0Y, val ? 1 : 0);
                                }
                            } else {
                                // For Joy-Con R upright, or any Joy-Con in sideways orientation, treat codes as regular face buttons
                                emitEvent(vc->uinput_fd, EV_KEY, code, val);
                            }
                        }
                        // Shoulder and trigger buttons
                        else if (code == BTN_TL || code == BTN_TR || code == BTN_TL2 || code == BTN_TR2) {
                            if (jd->orientation == Sideways) {
                                // In sideways mode, ignore original L/ZL or R/ZR (not used as they are inaccessible in this orientation)
                                continue;
                            }
                            if (code == BTN_TL) {
                                emitEvent(vc->uinput_fd, EV_KEY, BTN_TL, val);
                            } else if (code == BTN_TR) {
                                emitEvent(vc->uinput_fd, EV_KEY, BTN_TR, val);
                            } else if (code == BTN_TL2) {
                                // Map ZL to analog trigger axis
                                emitEvent(vc->uinput_fd, EV_ABS, ABS_Z, val ? 255 : 0);
                            } else if (code == BTN_TR2) {
                                emitEvent(vc->uinput_fd, EV_ABS, ABS_RZ, val ? 255 : 0);
                            }
                        }
                        // SL/SR rail buttons on Joy-Con
                        else if (code == BTN_C || code == BTN_Z) {
                            if (jd->orientation == Sideways) {
                                // In sideways orientation, map SL -> L (BTN_TL) and SR -> R (BTN_TR)
                                if (code == BTN_C) {
                                    emitEvent(vc->uinput_fd, EV_KEY, BTN_TL, val);
                                } else if (code == BTN_Z) {
                                    emitEvent(vc->uinput_fd, EV_KEY, BTN_TR, val);
                                }
                            }
                            // In upright orientation, SL/SR are not used (ignore)
                        }
                        // Minus/Plus/Home buttons
                        else if (code == BTN_SELECT || code == BTN_START || code == BTN_MODE) {
                            if (code == BTN_SELECT) {
                                emitEvent(vc->uinput_fd, EV_KEY, BTN_SELECT, val);
                            } else if (code == BTN_START) {
                                emitEvent(vc->uinput_fd, EV_KEY, BTN_START, val);
                            } else if (code == BTN_MODE) {
                                emitEvent(vc->uinput_fd, EV_KEY, BTN_MODE, val);
                            }
                        }
                        // Stick clicks
                        else if (code == BTN_THUMBL || code == BTN_THUMBR) {
                            emitEvent(vc->uinput_fd, EV_KEY, code, val);
                        }
                        // Capture button (code 319) not mapped
                    }
                    else if (ev.type == EV_ABS) {
                        uint16_t code = ev.code;
                        int32_t val = ev.value;
                        if (code == ABS_X || code == ABS_Y) {
                            // Single Joy-Con (either left or right) uses its stick as the virtual left stick
                            emitEvent(vc->uinput_fd, EV_ABS, code, val);
                        } else if (code == ABS_RX || code == ABS_RY) {
                            // Single Joy-Con should not produce RX/RY (no second stick), ignore if any
                        } else if (code == ABS_HAT0X || code == ABS_HAT0Y) {
                            // Forward hat events (if any) directly
                            emitEvent(vc->uinput_fd, EV_ABS, code, val);
                        }
                    }
                    else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                        // Flush out a sync event after each batch
                        emitEvent(vc->uinput_fd, EV_SYN, SYN_REPORT, 0);
                    }
                }
            }).detach();
            std::wcout << L"Single Joy-Con configured. Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);
        }
        else if (controllerType == 2) { // Dual Joy-Con
            std::wcout << L"Player " << (i+1) << L": Please sync your RIGHT Joy-Con now.\n";
            JoyConDevice rightJc = waitForJoyConDevice(JOYCON_VENDOR_ID, JOYCON_R_PRODUCT_ID, Right, Upright, true);
            std::wcout << L"Please sync your LEFT Joy-Con now.\n";
            JoyConDevice leftJc = waitForJoyConDevice(JOYCON_VENDOR_ID, JOYCON_L_PRODUCT_ID, Left, Upright, true);
            joyconDevices.push_back(rightJc);
            joyconDevices.push_back(leftJc);
            // Create one virtual controller for this pair
            virtualControllers.emplace_back();
            initializeVirtualController(virtualControllers.back());
            VirtualController *vc = &virtualControllers.back();
            // References to the JoyCon devices
            JoyConDevice *rd = &joyconDevices[joyconDevices.size() - 2]; // right Joy-Con
            JoyConDevice *ld = &joyconDevices[joyconDevices.size() - 1]; // left Joy-Con
            // Thread for left Joy-Con (handles D-Pad, L, ZL, etc.)
            std::thread([ld, vc]() {
                struct input_event ev;
                while (true) {
                    ssize_t rb = read(ld->fd, &ev, sizeof(ev));
                    if (rb <= 0) {
                        if (rb < 0 && errno == EAGAIN) {
                            usleep(1000);
                            continue;
                        }
                        break;
                    }
                    if (ev.type == EV_KEY) {
                        uint16_t code = ev.code;
                        int32_t val = ev.value;
                        if (code == BTN_WEST || code == BTN_EAST || code == BTN_NORTH || code == BTN_SOUTH) {
                            // Map Left Joy-Con D-Pad to virtual hat
                            if (code == BTN_WEST) {
                                emitEvent(vc->uinput_fd, EV_ABS, ABS_HAT0X, val ? -1 : 0);
                            } else if (code == BTN_EAST) {
                                emitEvent(vc->uinput_fd, EV_ABS, ABS_HAT0X, val ? 1 : 0);
                            } else if (code == BTN_NORTH) {
                                emitEvent(vc->uinput_fd, EV_ABS, ABS_HAT0Y, val ? -1 : 0);
                            } else if (code == BTN_SOUTH) {
                                emitEvent(vc->uinput_fd, EV_ABS, ABS_HAT0Y, val ? 1 : 0);
                            }
                        }
                        else if (code == BTN_TL || code == BTN_TL2) {
                            // Left Joy-Con L and ZL
                            if (code == BTN_TL) {
                                emitEvent(vc->uinput_fd, EV_KEY, BTN_TL, val);
                            } else if (code == BTN_TL2) {
                                emitEvent(vc->uinput_fd, EV_ABS, ABS_Z, val ? 255 : 0);
                            }
                        }
                        else if (code == BTN_C || code == BTN_Z) {
                            // Left Joy-Con SL/SR (not used in dual upright mode)
                            continue;
                        }
                        else if (code == BTN_SELECT) {
                            emitEvent(vc->uinput_fd, EV_KEY, BTN_SELECT, val);
                        }
                        else if (code == BTN_THUMBL) {
                            emitEvent(vc->uinput_fd, EV_KEY, BTN_THUMBL, val);
                        }
                        // Ignore capture (319) if present
                    }
                    else if (ev.type == EV_ABS) {
                        uint16_t code = ev.code;
                        int32_t val = ev.value;
                        if (code == ABS_X || code == ABS_Y) {
                            // Left Joy-Con analog -> virtual left stick
                            emitEvent(vc->uinput_fd, EV_ABS, code, val);
                        } else if (code == ABS_HAT0X || code == ABS_HAT0Y) {
                            emitEvent(vc->uinput_fd, EV_ABS, code, val);
                        }
                    }
                    else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                        emitEvent(vc->uinput_fd, EV_SYN, SYN_REPORT, 0);
                    }
                }
            }).detach();
            // Thread for right Joy-Con (handles ABXY, R, ZR, etc.)
            std::thread([rd, vc]() {
                struct input_event ev;
                while (true) {
                    ssize_t rb = read(rd->fd, &ev, sizeof(ev));
                    if (rb <= 0) {
                        if (rb < 0 && errno == EAGAIN) {
                            usleep(1000);
                            continue;
                        }
                        break;
                    }
                    if (ev.type == EV_KEY) {
                        uint16_t code = ev.code;
                        int32_t val = ev.value;
                        if (code == BTN_SOUTH || code == BTN_EAST || code == BTN_NORTH || code == BTN_WEST) {
                            // Map Right Joy-Con face buttons A/B/X/Y directly
                            emitEvent(vc->uinput_fd, EV_KEY, code, val);
                        }
                        else if (code == BTN_TR || code == BTN_TR2) {
                            if (code == BTN_TR) {
                                emitEvent(vc->uinput_fd, EV_KEY, BTN_TR, val);
                            } else if (code == BTN_TR2) {
                                emitEvent(vc->uinput_fd, EV_ABS, ABS_RZ, val ? 255 : 0);
                            }
                        }
                        else if (code == BTN_Z || code == BTN_C) {
                            // Right Joy-Con SL/SR not used in dual mode
                            continue;
                        }
                        else if (code == BTN_START) {
                            emitEvent(vc->uinput_fd, EV_KEY, BTN_START, val);
                        }
                        else if (code == BTN_MODE) {
                            emitEvent(vc->uinput_fd, EV_KEY, BTN_MODE, val);
                        }
                        else if (code == BTN_THUMBR) {
                            emitEvent(vc->uinput_fd, EV_KEY, BTN_THUMBR, val);
                        }
                    }
                    else if (ev.type == EV_ABS) {
                        uint16_t code = ev.code;
                        int32_t val = ev.value;
                        if (code == ABS_X || code == ABS_Y) {
                            // Right Joy-Con analog -> virtual right stick (remap ABS_X->ABS_RX, ABS_Y->ABS_RY)
                            if (code == ABS_X) {
                                emitEvent(vc->uinput_fd, EV_ABS, ABS_RX, val);
                            } else if (code == ABS_Y) {
                                emitEvent(vc->uinput_fd, EV_ABS, ABS_RY, val);
                            }
                        }
                        // Right Joy-Con has no D-pad, ignore hat events if any
                    }
                    else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                        emitEvent(vc->uinput_fd, EV_SYN, SYN_REPORT, 0);
                    }
                }
            }).detach();
            std::wcout << L"Dual Joy-Cons connected and configured. Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);
        }
        else if (controllerType == 3) { // Pro Controller
            std::wcout << L"Player " << (i+1) << L": Please sync your Pro Controller now.\n";
            JoyConDevice proDev = waitForJoyConDevice(JOYCON_VENDOR_ID, PRO_CONTROLLER_PRODUCT_ID, Left, Upright, false);
            joyconDevices.push_back(proDev);
            virtualControllers.emplace_back();
            initializeVirtualController(virtualControllers.back());
            VirtualController *vc = &virtualControllers.back();
            JoyConDevice *pd = &joyconDevices.back();
            std::thread([pd, vc]() {
                struct input_event ev;
                while (true) {
                    ssize_t rb = read(pd->fd, &ev, sizeof(ev));
                    if (rb <= 0) {
                        if (rb < 0 && errno == EAGAIN) {
                            usleep(1000);
                            continue;
                        }
                        break;
                    }
                    if (ev.type == EV_KEY) {
                        uint16_t code = ev.code;
                        int32_t val = ev.value;
                        if (code == BTN_SOUTH || code == BTN_EAST || code == BTN_NORTH || code == BTN_WEST ||
                            code == BTN_TL || code == BTN_TR ||
                            code == BTN_SELECT || code == BTN_START || code == BTN_MODE ||
                            code == BTN_THUMBL || code == BTN_THUMBR) {
                            emitEvent(vc->uinput_fd, EV_KEY, code, val);
                        }
                        else if (code == BTN_TL2) {
                            emitEvent(vc->uinput_fd, EV_ABS, ABS_Z, val ? 255 : 0);
                        }
                        else if (code == BTN_TR2) {
                            emitEvent(vc->uinput_fd, EV_ABS, ABS_RZ, val ? 255 : 0);
                        }
                        // Ignore capture (if present) for simplicity
                    }
                    else if (ev.type == EV_ABS) {
                        uint16_t code = ev.code;
                        int32_t val = ev.value;
                        if (code == ABS_X || code == ABS_Y || code == ABS_RX || code == ABS_RY) {
                            emitEvent(vc->uinput_fd, EV_ABS, code, val);
                        }
                        else if (code == ABS_HAT0X || code == ABS_HAT0Y) {
                            emitEvent(vc->uinput_fd, EV_ABS, code, val);
                        }
                    }
                    else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                        emitEvent(vc->uinput_fd, EV_SYN, SYN_REPORT, 0);
                    }
                }
            }).detach();
            std::wcout << L"Pro Controller connected and configured. Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);
        }
        else if (controllerType == 4) { // NSO GameCube Controller
            std::wcout << L"Player " << (i+1) << L": Please sync your NSO GameCube Controller now.\n";
            JoyConDevice gcDev = waitForJoyConDevice(JOYCON_VENDOR_ID, NSO_GC_PRODUCT_ID, Left, Upright, false);
            joyconDevices.push_back(gcDev);
            virtualControllers.emplace_back();
            initializeVirtualController(virtualControllers.back());
            VirtualController *vc = &virtualControllers.back();
            JoyConDevice *gd = &joyconDevices.back();
            std::thread([gd, vc]() {
                struct input_event ev;
                while (true) {
                    ssize_t rb = read(gd->fd, &ev, sizeof(ev));
                    if (rb <= 0) {
                        if (rb < 0 && errno == EAGAIN) {
                            usleep(1000);
                            continue;
                        }
                        break;
                    }
                    if (ev.type == EV_KEY) {
                        uint16_t code = ev.code;
                        int32_t val = ev.value;
                        if (code == BTN_SOUTH || code == BTN_EAST || code == BTN_NORTH || code == BTN_WEST ||
                            code == BTN_TL || code == BTN_TR ||
                            code == BTN_SELECT || code == BTN_START || code == BTN_MODE ||
                            code == BTN_THUMBL || code == BTN_THUMBR) {
                            emitEvent(vc->uinput_fd, EV_KEY, code, val);
                        }
                        else if (code == BTN_TL2) {
                            emitEvent(vc->uinput_fd, EV_ABS, ABS_Z, val ? 255 : 0);
                        }
                        else if (code == BTN_TR2) {
                            emitEvent(vc->uinput_fd, EV_ABS, ABS_RZ, val ? 255 : 0);
                        }
                    }
                    else if (ev.type == EV_ABS) {
                        uint16_t code = ev.code;
                        int32_t val = ev.value;
                        if (code == ABS_X || code == ABS_Y || code == ABS_RX || code == ABS_RY) {
                            emitEvent(vc->uinput_fd, EV_ABS, code, val);
                        }
                        else if (code == ABS_HAT0X || code == ABS_HAT0Y) {
                            emitEvent(vc->uinput_fd, EV_ABS, code, val);
                        }
                    }
                    else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                        emitEvent(vc->uinput_fd, EV_SYN, SYN_REPORT, 0);
                    }
                }
            }).detach();
            std::wcout << L"NSO GameCube Controller connected and configured. Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);
        }
    }
    std::wcout << L"All players connected. Press Enter to exit...\n";
    std::wstring dummy;
    std::getline(std::wcin, dummy);
    // Cleanup: close input fds and destroy uinput devices
    for (auto &jd : joyconDevices) {
        if (jd.fd >= 0) close(jd.fd);
    }
    for (auto &vc : virtualControllers) {
        ioctl(vc.uinput_fd, UI_DEV_DESTROY);
        close(vc.uinput_fd);
    }
    return 0;
}
#endif
