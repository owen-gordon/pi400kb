#include "gadget-hid.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/ether.h>
#include <linux/usb/ch9.h>
#include <usbg/usbg.h>
#include <usbg/function/hid.h>
#include <usbg/function/net.h>

#define NMCLI_PATH "/usr/bin/nmcli"

usbg_state *s;
usbg_gadget *g;
usbg_config *c;
usbg_function *f_hid;
usbg_function *f_ecm;

static char ecm_ifname[32] = "usb0";
static bool ecm_ready = false;

static void refresh_ecm_ifname(void) {
    ecm_ifname[0] = '\0';

    if (!f_ecm) {
        return;
    }

    usbg_f_net *net = usbg_to_net_function(f_ecm);
    if (!net) {
        return;
    }

    if (usbg_f_net_get_ifname_s(net, ecm_ifname, sizeof(ecm_ifname)) < 0) {
        ecm_ifname[0] = '\0';
    }
}

static bool is_nmcli_available(void) {
    return access(NMCLI_PATH, X_OK) == 0;
}

static int run_nmcli(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execv("/usr/bin/nmcli", argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    return -1;
}

static void ecm_activate_connection(void) {
#ifdef ECM_NM_CONNECTION
    if (ECM_NM_CONNECTION[0] == '\0') {
        return;
    }

    if (!is_nmcli_available()) {
        fprintf(stderr, "Warning: nmcli not available at %s, skipping NetworkManager integration\n", NMCLI_PATH);
        return;
    }

    char *const argv[] = {(char *)"nmcli", (char *)"con", (char *)"up", (char *)ECM_NM_CONNECTION, NULL};
    int rc = run_nmcli(argv);
    if (rc != 0) {
        if (rc == 4) {
            fprintf(stderr, "Info: NetworkManager connection '%s' will activate once usb gadget enumerates\n", ECM_NM_CONNECTION);
        } else {
            fprintf(stderr, "Warning: failed to activate NetworkManager connection '%s' (rc=%d)\n", ECM_NM_CONNECTION, rc);
        }
    }
#endif
}

static void ecm_deactivate_connection(void) {
#ifdef ECM_NM_CONNECTION
    if (ECM_NM_CONNECTION[0] == '\0') {
        return;
    }

    if (!is_nmcli_available()) {
        return;
    }

    char *const argv[] = {(char *)"nmcli", (char *)"con", (char *)"down", (char *)ECM_NM_CONNECTION, NULL};
    int rc = run_nmcli(argv);
    if (rc != 0 && rc != 4) {
        fprintf(stderr, "Info: NetworkManager reported rc=%d when bringing '%s' down\n", rc, ECM_NM_CONNECTION);
    }
#endif
}

static char report_desc[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,        //   Usage Minimum (0xE0)
    0x29, 0xE7,        //   Usage Maximum (0xE7)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x03,        //   Report Count (3)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x03,        //   Usage Maximum (Scroll Lock)
    0x91, 0x02,        //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x91, 0x01,        //   Output (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,        //   Usage Minimum (0x00)
    0x2A, 0xFF, 0x00,  //   Usage Maximum (0xFF)
    0x81, 0x00,        //   Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,              // End Collection

    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (0x01)
    0x29, 0x03,        //     Usage Maximum (0x03)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x05,        //     Report Size (5)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x01,        //     Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x01,        //     Usage Page (Generic Desktop Ctrls)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

int initUSB() {
    int usbg_ret = -EINVAL;
    bool state_ready = false;

    /* Reset globals in case a prior run left stale pointers. */
    g = NULL;
    c = NULL;
    f_hid = NULL;
    f_ecm = NULL;
    ecm_ready = false;
    ecm_ifname[0] = '\0';

    struct usbg_gadget_attrs g_attrs = {
        .bcdUSB = 0x0200,
        .bDeviceClass = USB_CLASS_PER_INTERFACE,
        .bDeviceSubClass = 0x00,
        .bDeviceProtocol = 0x00,
        .bMaxPacketSize0 = 64, /* Max allowed ep0 packet size */
        .idVendor = KEYBOARD_VID,
        .idProduct = KEYBOARD_PID,
        .bcdDevice = 0x0001, /* Verson of device */
    };

    struct usbg_gadget_strs g_strs = {
        .serial = "0123456789",      /* Serial number */
        .manufacturer = "OWENLABS", /* Manufacturer */
        .product = "Pi500+"         /* Product string */
    };

    struct usbg_config_strs c_strs = {
        .configuration = "HID+ECM"
    };

    struct usbg_f_net_attrs ecm_attrs;
    memset(&ecm_attrs, 0, sizeof(ecm_attrs));
    ecm_attrs.qmult = ECM_QMULT;

    struct usbg_f_hid_attrs f_attrs = {
        .protocol = 1,
        .report_desc = {
            .desc = report_desc,
            .len = sizeof(report_desc),
        },
        .report_length = 16,
        .subclass = 0,
    };

    usbg_ret = usbg_init("/sys/kernel/config", &s);
    if (usbg_ret != USBG_SUCCESS) {
        fprintf(stderr, "Error on usbg init\n");
        fprintf(stderr, "Error: %s : %s\n", usbg_error_name(usbg_ret),
                usbg_strerror(usbg_ret));
        goto failure;
    }
    state_ready = true;

    usbg_gadget *existing = usbg_get_gadget(s, "g1");
    if (existing) {
        int ret = usbg_disable_gadget(existing);
        if (ret != USBG_SUCCESS && ret != USBG_ERROR_NOT_FOUND) {
            fprintf(stderr, "Warning: failed to disable existing gadget 'g1' (%s)\n",
                    usbg_strerror(ret));
        }

        ret = usbg_rm_gadget(existing, USBG_RM_RECURSE);
        if (ret != USBG_SUCCESS && ret != USBG_ERROR_NOT_FOUND) {
            fprintf(stderr, "Warning: failed to remove existing gadget 'g1' (%s)\n",
                    usbg_strerror(ret));
        }
        existing = NULL;
    }

    usbg_ret = usbg_create_gadget(s, "g1", &g_attrs, &g_strs, &g);
    if (usbg_ret != USBG_SUCCESS) {
        fprintf(stderr, "Error creating gadget\n");
        fprintf(stderr, "Error: %s : %s\n", usbg_error_name(usbg_ret),
                usbg_strerror(usbg_ret));
        goto failure;
    }

    usbg_ret = usbg_create_function(g, USBG_F_HID, "hid", &f_attrs, &f_hid);
    if (usbg_ret != USBG_SUCCESS) {
        fprintf(stderr, "Error creating function: USBG_F_HID\n");
        fprintf(stderr, "Error: %s : %s\n", usbg_error_name(usbg_ret),
                usbg_strerror(usbg_ret));
        goto failure;
    }

    struct ether_addr dev_addr;
    struct ether_addr host_addr;

    memset(&dev_addr, 0, sizeof(dev_addr));
    memset(&host_addr, 0, sizeof(host_addr));

    if (!ether_aton_r(ECM_DEV_ADDR, &dev_addr)) {
        fprintf(stderr, "Invalid ECM_DEV_ADDR '%s'\n", ECM_DEV_ADDR);
        usbg_ret = -EINVAL;
        goto failure;
    }

    if (!ether_aton_r(ECM_HOST_ADDR, &host_addr)) {
        fprintf(stderr, "Invalid ECM_HOST_ADDR '%s'\n", ECM_HOST_ADDR);
        usbg_ret = -EINVAL;
        goto failure;
    }

    memcpy(&ecm_attrs.dev_addr, &dev_addr, sizeof(dev_addr));
    memcpy(&ecm_attrs.host_addr, &host_addr, sizeof(host_addr));

    usbg_ret = usbg_create_function(g, USBG_F_ECM, "eth", &ecm_attrs, &f_ecm);
    if (usbg_ret != USBG_SUCCESS) {
        fprintf(stderr, "Error creating function: USBG_F_ECM\n");
        fprintf(stderr, "Error: %s : %s\n", usbg_error_name(usbg_ret),
                usbg_strerror(usbg_ret));
        goto failure;
    }

    refresh_ecm_ifname();
    if (ecm_ifname[0] == '\0') {
        strncpy(ecm_ifname, "usb0", sizeof(ecm_ifname) - 1);
        ecm_ifname[sizeof(ecm_ifname) - 1] = '\0';
    }

    usbg_ret = usbg_create_config(g, 1, "config", NULL, &c_strs, &c);
    if (usbg_ret != USBG_SUCCESS) {
        fprintf(stderr, "Error creating config\n");
        fprintf(stderr, "Error: %s : %s\n", usbg_error_name(usbg_ret),
                usbg_strerror(usbg_ret));
        goto failure;
    }

    usbg_ret = usbg_add_config_function(c, "keyboard", f_hid);
    if (usbg_ret != USBG_SUCCESS) {
        fprintf(stderr, "Error adding function: keyboard\n");
        fprintf(stderr, "Error: %s : %s\n", usbg_error_name(usbg_ret),
                usbg_strerror(usbg_ret));
        goto failure;
    }

    usbg_ret = usbg_add_config_function(c, "ecm.usb0", f_ecm);
    if (usbg_ret != USBG_SUCCESS) {
        fprintf(stderr, "Error adding function: ecm.usb0\n");
        fprintf(stderr, "Error: %s : %s\n", usbg_error_name(usbg_ret),
                usbg_strerror(usbg_ret));
        goto failure;
    }

    const char *udc_to_use = NULL;
    usbg_udc *udc_handle = NULL;

#ifdef ECM_FORCE_UDC
    if (ECM_FORCE_UDC[0] != '\0') {
        udc_handle = usbg_get_udc(s, ECM_FORCE_UDC);
        if (!udc_handle) {
            fprintf(stderr, "Error: specified UDC '%s' not found\n", ECM_FORCE_UDC);
            usbg_ret = USBG_ERROR_NOT_FOUND;
            goto failure;
        }
    }
#endif

    usbg_ret = usbg_enable_gadget(g, udc_handle);
    if (usbg_ret != USBG_SUCCESS) {
        fprintf(stderr, "Error enabling gadget\n");
        fprintf(stderr, "Error: %s : %s\n", usbg_error_name(usbg_ret),
                usbg_strerror(usbg_ret));
        goto failure;
    }

    ecm_activate_connection();
    ecm_ready = true;

    return usbg_ret;

failure:
    if (g && s) {
        usbg_disable_gadget(g);
        usbg_rm_gadget(g, USBG_RM_RECURSE);
        g = NULL;
    }
    if (state_ready && s) {
        usbg_cleanup(s);
        s = NULL;
    }

    return usbg_ret;
}

int cleanupUSB(){
    ecm_deactivate_connection();
    ecm_ready = false;

    if(g && s){
        usbg_disable_gadget(g);
        usbg_rm_gadget(g, USBG_RM_RECURSE);
        g = NULL;
        c = NULL;
        f_hid = NULL;
        f_ecm = NULL;
    }
    if(s){
        usbg_cleanup(s);
        s = NULL;
    }
    ecm_ifname[0] = '\0';
    return 0;
}
