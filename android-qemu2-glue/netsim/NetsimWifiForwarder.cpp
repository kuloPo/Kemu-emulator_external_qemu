/* Copyright 2023 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
#include "android-qemu2-glue/netsim/NetsimWifiForwarder.h"

#include "aemu/base/logging/Log.h"
#include "android-qemu2-glue/netsim/NetsimWifiConnection.h"
#include "android/emulation/HostapdController.h"
#include "android/network/GenericNetlinkMessage.h"
#include "android/network/mac80211_hwsim.h"
#include "backend/packet_streamer_client.h"
#include "host-common/VmLock.h"
#include "netsim.h"

namespace android {
namespace qemu2 {

NetsimWifiForwarder::NetsimWifiForwarder(
        WifiService::OnReceiveCallback onRecv,
        WifiService::CanReceiveCallback canReceive)
    : mOnRecv(onRecv), mCanReceive(canReceive) {}

NetsimWifiForwarder::~NetsimWifiForwarder() {
    stop();
}

bool NetsimWifiForwarder::init() {
    netsim::packet::PacketRequest registration;
    auto* initialInfo = registration.mutable_initial_info();
    auto deviceInfo = get_netsim_device_info();
    initialInfo->set_name(deviceInfo->name());
    initialInfo->mutable_chip()->set_kind(netsim::common::ChipKind::WIFI);
    initialInfo->mutable_device_info()->CopyFrom(*deviceInfo);

    mConnection = std::make_unique<NetsimWifiConnection>(
            [options = get_netsim_options()](const auto& cancelled) {
                return netsim::packet::CreateChannel(options, cancelled);
            },
            std::move(registration),
            [this](const std::string& packet) {
                android::RecursiveScopedVmLock lock;
                if (mCanReceive(0)) {
                    mOnRecv(reinterpret_cast<const uint8_t*>(packet.data()),
                            packet.size());
                }
            });
    if (!mConnection->start()) {
        dwarning("Unable to establish gRPC connection with netsim.");
        return false;
    }
    auto* hostapd = android::emulation::HostapdController::getInstance();
    if (hostapd) {
        hostapd->terminate();
    }
    dinfo("Successfully initialized netsim WiFi");
    return true;
}

int NetsimWifiForwarder::send(const android::base::IOVector& iov) {
    if (!mConnection) {
        return 0;
    }
    const android::network::GenericNetlinkMessage msg(iov);
    if (msg.genericNetlinkHeader()->cmd != HWSIM_CMD_FRAME) {
        return 0;
    }
    // Drop packets while disconnected. Returning -EBUSY would stall the guest
    // virtqueue: this backend has no completion callback to wake it again.
    mConnection->sendPacket(std::string(msg.data(), msg.data() + msg.dataLen()));
    return msg.dataLen();
}

int NetsimWifiForwarder::hostapd_send(const android::base::IOVector& iov) {
    return send(iov);
}

int NetsimWifiForwarder::libslirp_send(const android::base::IOVector& iov) {
    return send(iov);
}

void NetsimWifiForwarder::stop() {
    if (mConnection) {
        // A pending receive may need the VM lock. Drain it before the device
        // releases its queues, without waiting while holding that lock.
        android::ScopedVmUnlock unlock;
        mConnection->stop();
    }
}

int NetsimWifiForwarder::recv(android::base::IOVector& iov) {
    // The virtio wifi driver uses the receive callback.
    return 0;
}

}  // namespace qemu2
}  // namespace android
