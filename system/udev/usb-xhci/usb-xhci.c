// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/driver.h>
#include <ddk/protocol/usb-hci.h>
#include <ddk/protocol/usb.h>

#include <hw/reg.h>
#include <magenta/syscalls.h>
#include <magenta/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "xhci-device-manager.h"
#include "xhci-root-hub.h"
#include "xhci-util.h"
#include "xhci.h"

//#define TRACE 1
#include "xhci-debug.h"

#define MAX_SLOTS 255

mx_status_t xhci_add_device(xhci_t* xhci, int slot_id, int hub_address, int speed) {
    xprintf("xhci_add_new_device\n");

    if (!xhci->bus_device || !xhci->bus_protocol) {
        printf("no bus device in xhci_add_device\n");
        return ERR_INTERNAL;
    }

    return xhci->bus_protocol->add_device(xhci->bus_device, slot_id, hub_address, speed);
}

void xhci_remove_device(xhci_t* xhci, int slot_id) {
    xprintf("xhci_remove_device %d\n", slot_id);

    if (!xhci->bus_device || !xhci->bus_protocol) {
        printf("no bus device in xhci_remove_device\n");
        return;
    }

    xhci->bus_protocol->remove_device(xhci->bus_device, slot_id);
}

static int xhci_irq_thread(void* arg) {
    xhci_t* xhci = (xhci_t*)arg;
    xprintf("xhci_irq_thread start\n");

    // xhci_start will block, so do this part here instead of in usb_xhci_bind
    xhci_start(xhci);

    device_add(&xhci->device, xhci->parent);
    xhci->parent = NULL;

    while (1) {
        mx_status_t wait_res;

        wait_res = mx_interrupt_wait(xhci->irq_handle);
        if (wait_res != NO_ERROR) {
            if (wait_res != ERR_HANDLE_CLOSED) {
                printf("unexpected pci_wait_interrupt failure (%d)\n", wait_res);
            }
            mx_interrupt_complete(xhci->irq_handle);
            break;
        }

        mx_interrupt_complete(xhci->irq_handle);
        xhci_handle_interrupt(xhci, xhci->legacy_irq_mode);
    }
    xprintf("xhci_irq_thread done\n");
    return 0;
}

static void xhci_set_bus_device(mx_device_t* device, mx_device_t* busdev) {
    xhci_t* xhci = dev_to_xhci(device);
    xhci->bus_device = busdev;
    if (busdev) {
        device_get_protocol(busdev, MX_PROTOCOL_USB_BUS, (void**)&xhci->bus_protocol);
        // wait until bus driver has started before doing this
        xhci_queue_start_root_hubs(xhci);
    } else {
        xhci->bus_protocol = NULL;
    }
}

static size_t xhci_get_max_device_count(mx_device_t* device) {
    xhci_t* xhci = dev_to_xhci(device);
    // add one to allow device IDs to be 1-based
    return xhci->max_slots + XHCI_RH_COUNT + 1;
}

static mx_status_t xhci_enable_ep(mx_device_t* device, uint32_t device_id,
                                  usb_endpoint_descriptor_t* ep_desc, bool enable) {
    xhci_t* xhci = dev_to_xhci(device);
    return xhci_enable_endpoint(xhci, device_id, ep_desc, enable);
}

static uint64_t xhci_get_frame(mx_device_t* device) {
    xhci_t* xhci = dev_to_xhci(device);
    return xhci_get_current_frame(xhci);
}

mx_status_t xhci_config_hub(mx_device_t* device, uint32_t device_id, usb_speed_t speed,
                            usb_hub_descriptor_t* descriptor) {
    xhci_t* xhci = dev_to_xhci(device);
    return xhci_configure_hub(xhci, device_id, speed, descriptor);
}

mx_status_t xhci_hub_device_added(mx_device_t* device, uint32_t hub_address, int port,
                                  usb_speed_t speed) {
    xhci_t* xhci = dev_to_xhci(device);
    return xhci_enumerate_device(xhci, hub_address, port, speed);
}

mx_status_t xhci_hub_device_removed(mx_device_t* device, uint32_t hub_address, int port) {
    xhci_t* xhci = dev_to_xhci(device);
    xhci_device_disconnected(xhci, hub_address, port);
    return NO_ERROR;
}

mx_status_t xhci_reset_ep(mx_device_t* device, uint32_t device_id, uint8_t ep_address) {
    xhci_t* xhci = dev_to_xhci(device);
    uint8_t ep_index = xhci_endpoint_index(ep_address);
    return xhci_reset_endpoint(xhci, device_id, ep_index);
}

size_t xhci_get_max_transfer_size(mx_device_t* device, uint32_t device_id, uint8_t ep_address) {
    if (ep_address == 0) {
        // control requests have uint16 length field
        return UINT16_MAX;
    }
    // non-control transfers consist of normal transfer TRBs plus one data event TRB
    // Assuming contiguous data buffers here.
    // This will need to change when we add scatter/gather support.
    // Subtract 1 to reserve a TRB for data event.
    return XHCI_MAX_DATA_BUFFER * (TRANSFER_RING_SIZE - 1);
}

usb_hci_protocol_t xhci_hci_protocol = {
    .set_bus_device = xhci_set_bus_device,
    .get_max_device_count = xhci_get_max_device_count,
    .enable_endpoint = xhci_enable_ep,
    .get_current_frame = xhci_get_frame,
    .configure_hub = xhci_config_hub,
    .hub_device_added = xhci_hub_device_added,
    .hub_device_removed = xhci_hub_device_removed,
    .reset_endpoint = xhci_reset_ep,
    .get_max_transfer_size = xhci_get_max_transfer_size,
};

void xhci_process_deferred_txns(xhci_t* xhci, xhci_endpoint_t* ep, bool closed) {
    list_node_t list;
    list_node_t* node;
    iotxn_t* txn;

    list_initialize(&list);

    mtx_lock(&ep->lock);
    // make a copy of deferred_txns list so we can operate on it safely outside of the mutex
    while ((node = list_remove_head(&ep->deferred_txns)) != NULL) {
        list_add_tail(&list, node);
    }
    list_initialize(&ep->deferred_txns);
    mtx_unlock(&ep->lock);

    if (closed) {
        while ((txn = list_remove_head_type(&list, iotxn_t, node)) != NULL) {
            iotxn_complete(txn, ERR_REMOTE_CLOSED, 0);
        }
        return;
    }

    // requeue all deferred transactions
    // this will either add them to the transfer ring or put them back on deferred_txns list
    while ((txn = list_remove_head_type(&list, iotxn_t, node)) != NULL) {
        mx_status_t status = xhci_queue_transfer(xhci, txn);
        if (status != NO_ERROR && status != ERR_BUFFER_TOO_SMALL) {
            iotxn_complete(txn, status, 0);
        }
    }
}

static void xhci_iotxn_queue(mx_device_t* device, iotxn_t* txn) {
    usb_protocol_data_t* data = iotxn_pdata(txn, usb_protocol_data_t);
    mx_status_t status;

    if (txn->length > xhci_get_max_transfer_size(device, data->device_id, data->ep_address)) {
        status = ERR_INVALID_ARGS;
    } else {
        xhci_t* xhci = dev_to_xhci(device);
        status = xhci_queue_transfer(xhci, txn);
    }

    if (status != NO_ERROR && status != ERR_BUFFER_TOO_SMALL) {
        iotxn_complete(txn, status, 0);
    }
}

static void xhci_unbind(mx_device_t* device) {
    xprintf("xhci_unbind\n");
    xhci_t* xhci = dev_to_xhci(device);

    if (xhci->bus_device) {
        device_remove(xhci->bus_device);
    }
}

static mx_status_t xhci_release(mx_device_t* device) {
    // FIXME - do something here
    return NO_ERROR;
}

static mx_protocol_device_t xhci_device_proto = {
    .iotxn_queue = xhci_iotxn_queue,
    .unbind = xhci_unbind,
    .release = xhci_release,
};

static mx_status_t usb_xhci_bind(mx_driver_t* drv, mx_device_t* dev, void** cookie) {
    mx_handle_t irq_handle = MX_HANDLE_INVALID;
    mx_handle_t mmio_handle = MX_HANDLE_INVALID;
    mx_handle_t cfg_handle = MX_HANDLE_INVALID;
    xhci_t* xhci = NULL;
    mx_status_t status;

    pci_protocol_t* pci_proto;
    if (device_get_protocol(dev, MX_PROTOCOL_PCI, (void**)&pci_proto)) {
        status = ERR_NOT_SUPPORTED;
        goto error_return;
    }

    xhci = calloc(1, sizeof(xhci_t));
    if (!xhci) {
        status = ERR_NO_MEMORY;
        goto error_return;
    }

    status = pci_proto->claim_device(dev);
    if (status < 0) {
        printf("usb_xhci_bind claim_device failed %d\n", status);
        goto error_return;
    }

    int bar = -1;
    void* mmio;
    uint64_t mmio_len;
    /*
     * TODO(cja): according to eXtensible Host Controller Interface revision 1.1, section 5, xhci
     * should only use BARs 0 and 1. 0 for 32 bit addressing, and 0+1 for 64 bit addressing.
     */
    for (size_t i = 0; i < PCI_MAX_BAR_COUNT; i++) {
        status = pci_proto->map_mmio(dev, i, MX_CACHE_POLICY_UNCACHED_DEVICE, &mmio, &mmio_len, &mmio_handle);
        if (status == NO_ERROR) {
            bar = i;
            break;
        }
    }
    if (bar == -1) {
        printf("usb_xhci_bind could not find bar\n");
        status = ERR_INTERNAL;
        goto error_return;
    }

    // enable bus master
    status = pci_proto->enable_bus_master(dev, true);
    if (status < 0) {
        printf("usb_xhci_bind enable_bus_master failed %d\n", status);
        goto error_return;
    }

    // select our IRQ mode
    status = pci_proto->set_irq_mode(dev, MX_PCIE_IRQ_MODE_MSI, 1);
    if (status < 0) {
        mx_status_t status_legacy = pci_proto->set_irq_mode(dev, MX_PCIE_IRQ_MODE_LEGACY, 1);

        if (status_legacy < 0) {
            printf("usb_xhci_bind Failed to set IRQ mode to either MSI "
                   "(err = %d) or Legacy (err = %d)\n",
                   status, status_legacy);
            goto error_return;
        }

        xhci->legacy_irq_mode = true;
    }

    // register for interrupts
    status = pci_proto->map_interrupt(dev, 0, &irq_handle);
    if (status != NO_ERROR) {
        printf("usb_xhci_bind map_interrupt failed %d\n", status);
        goto error_return;
    }

    xhci->irq_handle = irq_handle;
    xhci->mmio_handle = mmio_handle;
    xhci->cfg_handle = cfg_handle;
    xhci->pci_proto = pci_proto;

    // stash this here for the startup thread to call device_add() with
    xhci->parent = dev;

    device_init(&xhci->device, drv, "usb-xhci", &xhci_device_proto);

    status = xhci_init(xhci, mmio);
    if (status < 0)
        goto error_return;

    xhci->device.protocol_id = MX_PROTOCOL_USB_HCI;
    xhci->device.protocol_ops = &xhci_hci_protocol;

    thrd_t thread;
    thrd_create_with_name(&thread, xhci_irq_thread, xhci, "xhci_irq_thread");
    thrd_detach(thread);

    return NO_ERROR;

error_return:
    if (xhci)
        free(xhci);
    if (irq_handle != MX_HANDLE_INVALID)
        mx_handle_close(irq_handle);
    if (mmio_handle != MX_HANDLE_INVALID)
        mx_handle_close(mmio_handle);
    if (cfg_handle != MX_HANDLE_INVALID)
        mx_handle_close(cfg_handle);
    return status;
}

mx_driver_t _driver_usb_xhci = {
    .ops = {
        .bind = usb_xhci_bind,
    },
};

// clang-format off
MAGENTA_DRIVER_BEGIN(_driver_usb_xhci, "usb-xhci", "magenta", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, MX_PROTOCOL_PCI),
    BI_ABORT_IF(NE, BIND_PCI_CLASS, 0x0C),
    BI_ABORT_IF(NE, BIND_PCI_SUBCLASS, 0x03),
    BI_MATCH_IF(EQ, BIND_PCI_INTERFACE, 0x30),
MAGENTA_DRIVER_END(_driver_usb_xhci)
