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
#include "ProxyBridge.hpp"
#include "ProxySettings.hpp"
#include <parallel_hashmap/phmap.h>
#include <sculk/protocol/auth/AuthenticationKeyManager.hpp>
#include <sculk/protocol/codec/actor/player/DisconnectFailReason.hpp>
#include <sculk/protocol/codec/actor/player/PlayStatus.hpp>
#include <sculk/protocol/codec/packet/LoginPacket.hpp>
#include <sculk/protocol/codec/packet/NetworkSettingsPacket.hpp>
#include <sculk/protocol/codec/packet/PlayStatusPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackChunkDataPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackChunkRequestPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackClientResponsePacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackDataInfoPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackStackPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePacksInfoPacket.hpp>
#include <sculk/protocol/codec/packet/RequestNetworkSettingsPacket.hpp>
#include <sculk/protocol/codec/packet/ServerToClientHandshakePacket.hpp>
#include <sculk/protocol/connection/ServerNetworkSystem.hpp>
#include <sculk/protocol/connection/io/ClientIoRuntime.hpp>
#include <sculk/protocol/connection/thread/ThreadPool.hpp>

namespace sculk {

class ProxyPass {
    protocol::ServerNetworkSystem                                                mProxyServer{};
    phmap::parallel_flat_hash_map_m<std::uint64_t, std::shared_ptr<ProxyBridge>> mBridges{};
    const protocol::AuthenticationKeyManager&                                    mAuthManager;
    protocol::PemKeyPair                                                         mProxyServerKeyPair{};
    ProxySettings&                                                               mSettings;
    ResourcePackManager                                                          mResourcePackManager{};

public:
    ProxyPass(protocol::AuthenticationKeyManager const& authManager, ProxySettings& settings);

    bool start();

private:
    void onClientDisconnected(const RakNet::RakNetGUID&);
    void onRealClientPacket(const RakNet::RakNetGUID&, const RakNet::SystemAddress&, const protocol::IPacket&);
    void handleFirstClientPacket(
        const RakNet::RakNetGUID&,
        const RakNet::SystemAddress&,
        const protocol::IPacket&,
        protocol::Session&
    );
    void processClientPacket(ProxyBridge&, const protocol::IPacket&);
    void handleClient(protocol::Session&, const protocol::RequestNetworkSettingsPacket&);
    void handleClient(ProxyBridge&, const protocol::LoginPacket&);
    void handleClient(ProxyBridge&, const protocol::ResourcePackClientResponsePacket&);
    void handleClient(ProxyBridge&, const protocol::ResourcePackChunkRequestPacket&);
    void processServerPacket(ProxyBridge&, const protocol::IPacket&);
    void handleServer(ProxyBridge&, const protocol::NetworkSettingsPacket&);
    void handleServer(ProxyBridge&, const protocol::ServerToClientHandshakePacket&);
    void handleServer(ProxyBridge&, const protocol::PlayStatusPacket&);
    void handleServer(ProxyBridge&, const protocol::ResourcePacksInfoPacket&);
    void handleServer(ProxyBridge&, const protocol::ResourcePackStackPacket&);
    void handleServer(ProxyBridge&, const protocol::ResourcePackDataInfoPacket&);
    void handleServer(ProxyBridge&, const protocol::ResourcePackChunkDataPacket&);
    void startClientResourcePackHandshake(ProxyBridge&);
    void sendNextClientResourcePackInfo(ProxyBridge&);
    void completeUpstreamResourcePackIfReady(ProxyBridge&);
    bool resourcePackBarrierSatisfied(const ProxyBridge&) const;
    void forwardOrQueueServerPacket(ProxyBridge&, const protocol::IPacket&);
    void disconnectClient(const RakNet::RakNetGUID&, protocol::PlayStatus);
    void disconnectClient(const RakNet::RakNetGUID&, std::string_view, protocol::DisconnectFailReason);
};

} // namespace sculk
