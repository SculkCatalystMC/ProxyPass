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

#pragma once
#include "Feature/ResourcePackManager.hpp"
#include <atomic>
#include <sculk/protocol/auth/ConnectionRequest.hpp>
#include <sculk/protocol/connection/ClientNetworkSystem.hpp>
#include <vector>

namespace sculk {

class ProxyBridge {
public:
    struct ClientInfo {
        std::string name;
        std::string xuid;
        std::string pfid;
    };

public:
    RakNet::RakNetGUID            mRealGuid{};
    RakNet::SystemAddress         mRealAddress{};
    protocol::ClientNetworkSystem mProxyClient{};
    protocol::Session&            mRealClientSession;
    protocol::ConnectionRequest   mConnectionRequest{};
    ClientInfo                    mClientInfo{};
    ResourcePackManager::Session  mResourcePackSession{};
    std::atomic_bool              mClientReady{false};
    std::vector<protocol::Session::Buffer> mQueuedServerPackets{};

public:
    explicit ProxyBridge(
        const RakNet::RakNetGUID&    guid,
        const RakNet::SystemAddress& address,
        protocol::Session&           realClientSession
    ) noexcept;

    ~ProxyBridge();

    bool sendPacketToClient(const protocol::IPacket& packet, bool immediate = false);
    bool sendBufferToClient(protocol::Session::Buffer&& buffer, bool immediate = false);
    void queuePacketToClient(const protocol::IPacket& packet);
    void flushQueuedPacketsToClient();

    bool sendPacketToServer(const protocol::IPacket& packet, bool immediate = false);
};

} // namespace sculk
