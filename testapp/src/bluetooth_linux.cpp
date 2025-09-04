#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <unistd.h>

class BluetoothManager {
private:
    int socket_fd;
    
public:
    bool connectToJoyCon(const char* mac_address) {
        struct sockaddr_l2 addr = {0};
        
        // Create L2CAP socket
        socket_fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
        if (socket_fd < 0) {
            return false;
        }
        
        // Set up destination address
        addr.l2_family = AF_BLUETOOTH;
        addr.l2_psm = htobs(0x11); // HID Control channel
        str2ba(mac_address, &addr.l2_bdaddr);
        
        // Connect to the device
        if (connect(socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(socket_fd);
            return false;
        }
        
        return true;
    }
    
    bool scanForDevices() {
        inquiry_info *ii = NULL;
        int max_rsp = 255;
        int num_rsp;
        int dev_id, sock, flags;
        
        dev_id = hci_get_route(NULL);
        sock = hci_open_dev(dev_id);
        if (dev_id < 0 || sock < 0) {
            return false;
        }
        
        flags = IREQ_CACHE_FLUSH;
        ii = (inquiry_info*)malloc(max_rsp * sizeof(inquiry_info));
        
        num_rsp = hci_inquiry(dev_id, 8, max_rsp, NULL, &ii, flags);
        if (num_rsp < 0) {
            free(ii);
            close(sock);
            return false;
        }
        
        // Process found devices
        for (int i = 0; i < num_rsp; i++) {
            char addr[19] = {0};
            char name[248] = {0};
            
            ba2str(&(ii + i)->bdaddr, addr);
            
            if (hci_read_remote_name(sock, &(ii + i)->bdaddr, sizeof(name), 
                                     name, 0) < 0) {
                strcpy(name, "[unknown]");
            }
            
            // Check if it's a Joy-Con or Pro Controller
            if (strstr(name, "Joy-Con") || strstr(name, "Pro Controller")) {
                printf("Found Nintendo controller: %s at %s\n", name, addr);
            }
        }
        
        free(ii);
        close(sock);
        return true;
    }
};
