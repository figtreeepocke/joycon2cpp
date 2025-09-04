// joycon2cpp Linux Port - Main Entry Point
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <signal.h>
#include <unistd.h>

// Platform-specific includes
#ifdef HAS_BLUETOOTH
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#endif

#ifdef HAS_HIDAPI
#include <hidapi/hidapi.h>
#endif

// Linux input subsystem
#include <linux/uinput.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

// Global flag for graceful shutdown
volatile bool g_running = true;

// Signal handler for clean exit
void signal_handler(int signum) {
    std::cout << "\nReceived signal " << signum << ", shutting down..." << std::endl;
    g_running = false;
}

// Simple virtual controller class for uinput
class VirtualController {
private:
    int fd;
    struct uinput_setup usetup;
    bool initialized;

public:
    VirtualController() : fd(-1), initialized(false) {
        // Try to open uinput device
        fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
        }
        
        if (fd < 0) {
            std::cerr << "Error: Cannot open uinput device (errno: " << errno << ")" << std::endl;
            std::cerr << "Try running with sudo or check permissions" << std::endl;
            return;
        }

        // Enable event types
        if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
            ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
            ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
            std::cerr << "Error setting event bits" << std::endl;
            close(fd);
            fd = -1;
            return;
        }

        // Enable gamepad buttons
        int buttons[] = {
            BTN_A, BTN_B, BTN_X, BTN_Y,
            BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
            BTN_SELECT, BTN_START, BTN_MODE,
            BTN_THUMBL, BTN_THUMBR,
            BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT
        };
        
        for (int btn : buttons) {
            if (ioctl(fd, UI_SET_KEYBIT, btn) < 0) {
                std::cerr << "Warning: Could not set button " << btn << std::endl;
            }
        }

        // Enable absolute axes for analog sticks
        int axes[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ};
        for (int axis : axes) {
            if (ioctl(fd, UI_SET_ABSBIT, axis) < 0) {
                std::cerr << "Warning: Could not set axis " << axis << std::endl;
            }
        }

        // Configure axes ranges
        struct uinput_abs_setup abs_setup;
        memset(&abs_setup, 0, sizeof(abs_setup));
        abs_setup.absinfo.minimum = -32768;
        abs_setup.absinfo.maximum = 32767;
        abs_setup.absinfo.fuzz = 0;
        abs_setup.absinfo.flat = 0;

        // Set up each axis
        for (int axis : axes) {
            abs_setup.code = axis;
            ioctl(fd, UI_ABS_SETUP, &abs_setup);
        }

        // Set up the device
        memset(&usetup, 0, sizeof(usetup));
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor = 0x057e;  // Nintendo
        usetup.id.product = 0x2009; // Switch Pro Controller
        usetup.id.version = 1;
        strncpy(usetup.name, "Nintendo Switch Controller (joycon2cpp)", UINPUT_MAX_NAME_SIZE);

        if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
            std::cerr << "Error: UI_DEV_SETUP failed" << std::endl;
            close(fd);
            fd = -1;
            return;
        }

        if (ioctl(fd, UI_DEV_CREATE) < 0) {
            std::cerr << "Error: UI_DEV_CREATE failed" << std::endl;
            close(fd);
            fd = -1;
            return;
        }

        initialized = true;
        std::cout << "Virtual controller created successfully" << std::endl;
    }

    ~VirtualController() {
        if (fd >= 0) {
            ioctl(fd, UI_DEV_DESTROY);
            close(fd);
        }
    }

    bool isInitialized() const { return initialized; }

    void sendEvent(int type, int code, int value) {
        if (fd < 0) return;

        struct input_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = type;
        ev.code = code;
        ev.value = value;
        
        if (write(fd, &ev, sizeof(ev)) < 0) {
            std::cerr << "Error writing event" << std::endl;
        }

        // Send sync event
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        write(fd, &ev, sizeof(ev));
    }

    void sendButton(int button, bool pressed) {
        sendEvent(EV_KEY, button, pressed ? 1 : 0);
    }

    void sendAxis(int axis, int value) {
        sendEvent(EV_ABS, axis, value);
    }
};

// Controller detection and connection
class ControllerManager {
private:
    std::vector<std::unique_ptr<VirtualController>> controllers;

public:
    ControllerManager() {
        std::cout << "Initializing Controller Manager..." << std::endl;
    }

    bool scanForControllers() {
#ifdef HAS_HIDAPI
        std::cout << "Scanning for HID devices..." << std::endl;
        
        if (hid_init() != 0) {
            std::cerr << "Failed to initialize hidapi" << std::endl;
            return false;
        }

        struct hid_device_info *devs, *cur_dev;
        devs = hid_enumerate(0x057e, 0x0); // Nintendo vendor ID
        
        if (!devs) {
            std::cout << "No Nintendo controllers found via HID" << std::endl;
        }

        cur_dev = devs;
        while (cur_dev) {
            printf("Found device: %04x:%04x - %ls %ls\n",
                cur_dev->vendor_id, cur_dev->product_id,
                cur_dev->manufacturer_string, cur_dev->product_string);
            
            // Check for known Nintendo controller product IDs
            switch(cur_dev->product_id) {
                case 0x2006: // Joy-Con L
                case 0x2007: // Joy-Con R
                case 0x2009: // Pro Controller
                case 0x200e: // Joy-Con Charging Grip
                    std::cout << "  -> Recognized Nintendo controller!" << std::endl;
                    break;
            }
            
            cur_dev = cur_dev->next;
        }
        hid_free_enumeration(devs);
#endif

#ifdef HAS_BLUETOOTH
        std::cout << "\nScanning for Bluetooth devices..." << std::endl;
        scanBluetooth();
#endif

        return true;
    }

#ifdef HAS_BLUETOOTH
    void scanBluetooth() {
        inquiry_info *ii = NULL;
        int max_rsp = 255;
        int num_rsp;
        int dev_id, sock;
        char addr[19] = {0};
        char name[248] = {0};

        dev_id = hci_get_route(NULL);
        if (dev_id < 0) {
            std::cerr << "No Bluetooth adapter found" << std::endl;
            return;
        }

        sock = hci_open_dev(dev_id);
        if (sock < 0) {
            std::cerr << "Error opening Bluetooth socket" << std::endl;
            return;
        }

        ii = (inquiry_info*)malloc(max_rsp * sizeof(inquiry_info));
        
        std::cout << "Scanning for Bluetooth devices (8 seconds)..." << std::endl;
        num_rsp = hci_inquiry(dev_id, 8, max_rsp, NULL, &ii, IREQ_CACHE_FLUSH);
        
        if (num_rsp < 0) {
            std::cerr << "Bluetooth inquiry failed" << std::endl;
            free(ii);
            close(sock);
            return;
        }

        std::cout << "Found " << num_rsp << " Bluetooth device(s)" << std::endl;

        for (int i = 0; i < num_rsp; i++) {
            ba2str(&(ii+i)->bdaddr, addr);
            memset(name, 0, sizeof(name));
            
            if (hid_read_remote_name(sock, &(ii+i)->bdaddr, sizeof(name), name, 0) < 0)
                strcpy(name, "[unknown]");
            
            std::cout << "  " << addr << " - " << name << std::endl;
            
            // Check for Nintendo controllers
            if (strstr(name, "Joy-Con") || strstr(name, "Pro Controller")) {
                std::cout << "    -> Found Nintendo controller!" << std::endl;
            }
        }

        free(ii);
        close(sock);
    }
#endif

    void createVirtualController() {
        auto controller = std::make_unique<VirtualController>();
        if (controller->isInitialized()) {
            controllers.push_back(std::move(controller));
            std::cout << "Created virtual controller #" << controllers.size() << std::endl;
        }
    }

    void testVirtualController() {
        if (controllers.empty()) {
            std::cout << "No virtual controllers available" << std::endl;
            return;
        }

        auto& controller = controllers[0];
        std::cout << "Testing virtual controller (press Ctrl+C to stop)..." << std::endl;
        
        while (g_running) {
            // Test A button
            std::cout << "Pressing A button" << std::endl;
            controller->sendButton(BTN_A, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            controller->sendButton(BTN_A, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Test left stick
            std::cout << "Moving left stick" << std::endl;
            controller->sendAxis(ABS_X, 16384);
            controller->sendAxis(ABS_Y, 16384);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            controller->sendAxis(ABS_X, 0);
            controller->sendAxis(ABS_Y, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
};

void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help     Show this help message" << std::endl;
    std::cout << "  -s, --scan     Scan for controllers only" << std::endl;
    std::cout << "  -t, --test     Test virtual controller" << std::endl;
    std::cout << "  -v, --verbose  Enable verbose output" << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "joycon2cpp Linux Port v1.0" << std::endl;
    std::cout << "===========================" << std::endl;

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Parse command line arguments
    bool scan_only = false;
    bool test_mode = false;
    
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-s" || arg == "--scan") {
            scan_only = true;
        } else if (arg == "-t" || arg == "--test") {
            test_mode = true;
        }
    }

    // Check if running as root (needed for uinput)
    if (geteuid() != 0) {
        std::cout << "Warning: Not running as root. You may need sudo for uinput access." << std::endl;
        std::cout << "Alternatively, add your user to the 'input' group." << std::endl;
    }

    // Initialize controller manager
    ControllerManager manager;

    // Scan for controllers
    manager.scanForControllers();

    if (scan_only) {
        return 0;
    }

    // Create virtual controller
    std::cout << "\nCreating virtual controller..." << std::endl;
    manager.createVirtualController();

    if (test_mode) {
        manager.testVirtualController();
    } else {
        std::cout << "\nReady. Press Ctrl+C to exit." << std::endl;
        std::cout << "Virtual controller is now available as a gamepad device." << std::endl;
        std::cout << "You can test it with: jstest /dev/input/js0" << std::endl;
        
        // Main loop
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    std::cout << "Shutting down..." << std::endl;
    return 0;
}
