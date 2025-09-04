#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

class VirtualController {
private:
    int fd;
    struct uinput_setup usetup;

public:
    VirtualController() : fd(-1) {
        fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            // Try alternative location
            fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
        }
        
        if (fd < 0) {
            throw std::runtime_error("Cannot open uinput device");
        }

        // Enable button events
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        
        // Enable gamepad buttons
        ioctl(fd, UI_SET_KEYBIT, BTN_A);
        ioctl(fd, UI_SET_KEYBIT, BTN_B);
        ioctl(fd, UI_SET_KEYBIT, BTN_X);
        ioctl(fd, UI_SET_KEYBIT, BTN_Y);
        ioctl(fd, UI_SET_KEYBIT, BTN_TL);
        ioctl(fd, UI_SET_KEYBIT, BTN_TR);
        ioctl(fd, UI_SET_KEYBIT, BTN_SELECT);
        ioctl(fd, UI_SET_KEYBIT, BTN_START);
        ioctl(fd, UI_SET_KEYBIT, BTN_THUMBL);
        ioctl(fd, UI_SET_KEYBIT, BTN_THUMBR);
        
        // Enable absolute axes for sticks
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        ioctl(fd, UI_SET_ABSBIT, ABS_X);
        ioctl(fd, UI_SET_ABSBIT, ABS_Y);
        ioctl(fd, UI_SET_ABSBIT, ABS_RX);
        ioctl(fd, UI_SET_ABSBIT, ABS_RY);
        
        // Set up axes ranges
        struct uinput_abs_setup abs_setup;
        memset(&abs_setup, 0, sizeof(abs_setup));
        
        // Left stick X
        abs_setup.code = ABS_X;
        abs_setup.absinfo.minimum = -32768;
        abs_setup.absinfo.maximum = 32767;
        ioctl(fd, UI_ABS_SETUP, &abs_setup);
        
        // Left stick Y
        abs_setup.code = ABS_Y;
        ioctl(fd, UI_ABS_SETUP, &abs_setup);
        
        // Right stick X
        abs_setup.code = ABS_RX;
        ioctl(fd, UI_ABS_SETUP, &abs_setup);
        
        // Right stick Y
        abs_setup.code = ABS_RY;
        ioctl(fd, UI_ABS_SETUP, &abs_setup);

        // Configure device
        memset(&usetup, 0, sizeof(usetup));
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor = 0x057e;  // Nintendo vendor ID
        usetup.id.product = 0x2009; // Joy-Con product ID
        strcpy(usetup.name, "Nintendo Switch Controller");

        ioctl(fd, UI_DEV_SETUP, &usetup);
        ioctl(fd, UI_DEV_CREATE);
    }

    void sendButton(int button, bool pressed) {
        struct input_event ev;
        memset(&ev, 0, sizeof(ev));
        
        ev.type = EV_KEY;
        ev.code = button;
        ev.value = pressed ? 1 : 0;
        
        write(fd, &ev, sizeof(ev));
        
        // Sync event
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        write(fd, &ev, sizeof(ev));
    }

    void sendAxis(int axis, int value) {
        struct input_event ev;
        memset(&ev, 0, sizeof(ev));
        
        ev.type = EV_ABS;
        ev.code = axis;
        ev.value = value;
        
        write(fd, &ev, sizeof(ev));
        
        // Sync event
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        write(fd, &ev, sizeof(ev));
    }

    ~VirtualController() {
        if (fd >= 0) {
            ioctl(fd, UI_DEV_DESTROY);
            close(fd);
        }
    }
};
