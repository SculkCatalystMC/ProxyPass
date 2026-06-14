// Copyright © 2026 SculkCatalystMC. All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ProxyBridge.hpp"

namespace sculk {

ProxyBridge::ProxyBridge(
    const RakNet::RakNetGUID&    guid,
    const RakNet::SystemAddress& address,
    protocol::Session&           realClientSession
) noexcept
: mRealGuid(guid),
  mRealAddress(address),
  mProxyClient(1),
  mRealClientSession(realClientSession),
  mClientReady(false) {
    mResourcePackSession = {};
}

ProxyBridge::~ProxyBridge() {
    if (mProxyClient.isConnected()) {
        mProxyClient.disconnect();
    }
}

bool ProxyBridge::sendPacketToClient(const protocol::IPacket& packet, bool immediate) {
    protocol::Session::Buffer buffer{};
    protocol::BinaryStream    stream{buffer};
    packet.writeWithHeader(stream);
    return sendBufferToClient(std::move(buffer), immediate);
}

bool ProxyBridge::sendBufferToClient(protocol::Session::Buffer&& buffer, bool immediate) {
    if (immediate) {
        return mRealClientSession.sendPacketImmediately(std::move(buffer));
    }
    return mRealClientSession.sendPacket(std::move(buffer));
}

void ProxyBridge::queuePacketToClient(const protocol::IPacket& packet) {
    protocol::Session::Buffer buffer{};
    protocol::BinaryStream    stream{buffer};
    packet.writeWithHeader(stream);
    mQueuedServerPackets.emplace_back(std::move(buffer));
}

void ProxyBridge::flushQueuedPacketsToClient() {
    auto queuedPackets = std::move(mQueuedServerPackets);
    mQueuedServerPackets.clear();
    for (auto& buffer : queuedPackets) {
        sendBufferToClient(std::move(buffer));
    }
}

bool ProxyBridge::sendPacketToServer(const protocol::IPacket& packet, bool immediate) {
    if (!mProxyClient.isConnected()) {
        return false;
    }
    protocol::Session::Buffer buffer{};
    protocol::BinaryStream    stream{buffer};
    packet.writeWithHeader(stream);
    if (immediate) {
        return mProxyClient.getSession().sendPacketImmediately(std::move(buffer));
    }
    return mProxyClient.getSession().sendPacket(std::move(buffer));
}

} // namespace sculk
