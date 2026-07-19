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
#include "Logger.hpp"
#include "ProxyBridge.hpp"
#include "ProxySettings.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <parallel_hashmap/phmap.h>
#include <sculk/protocol/auth/AuthenticationKeyManager.hpp>
#include <sculk/protocol/codec/actor/player/DisconnectFailReason.hpp>
#include <sculk/protocol/codec/actor/player/PlayStatus.hpp>
#include <sculk/protocol/codec/packet/LoginPacket.hpp>
#include <sculk/protocol/codec/packet/NetworkSettingsPacket.hpp>
#include <sculk/protocol/codec/packet/RequestNetworkSettingsPacket.hpp>
#include <sculk/protocol/codec/packet/ServerToClientHandshakePacket.hpp>
#include <sculk/protocol/connection/ServerNetworkSystem.hpp>
#include <sculk/protocol/connection/thread/ThreadPool.hpp>

namespace sculk {

class ProxyPass {
    Logger                                                                            mLogger{};
    phmap::parallel_flat_hash_map_m<RakNet::RakNetGUID, std::shared_ptr<ProxyBridge>> mBridges{};
    ProxySettings                                                                     mSettings{};
    protocol::AuthenticationKeyManager                                                mAuthManager{};
    protocol::PemKeyPair                                                              mProxyServerKeyPair{};
    std::unique_ptr<protocol::thread::ThreadPool>                                     mThreadPool{};
    std::unique_ptr<protocol::ServerNetworkSystem>                                    mProxyServer{};
    std::mutex                                                                          mStopMutex{};
    std::condition_variable                                                             mStopCv{};
    std::atomic<bool>                                                                   mShouldStop{false};

public:
    ProxyPass();
    ~ProxyPass();

    Logger& getLogger();

    bool start();

    void shutdown();

    void requestStop();

    void waitForStop();

    static void setWorkingDirectory();

    static void initConsole();

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
    void processServerPacket(ProxyBridge&, const protocol::IPacket&);
    void handleServer(ProxyBridge&, const protocol::NetworkSettingsPacket&);
    void handleServer(ProxyBridge&, const protocol::ServerToClientHandshakePacket&);
    void disconnectClient(const RakNet::RakNetGUID&, protocol::PlayStatus);
    void disconnectClient(const RakNet::RakNetGUID&, std::string_view, protocol::DisconnectFailReason);
};

} // namespace sculk
