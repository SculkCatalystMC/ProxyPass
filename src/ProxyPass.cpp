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

#include "ProxyPass.hpp"
#include <sculk/protocol/codec/MinecraftPackets.hpp>
#include <sculk/protocol/codec/packet/ClientToServerHandshakePacket.hpp>
#include <sculk/protocol/codec/packet/DisconnectPacket.hpp>
#include <sculk/protocol/codec/packet/PlayStatusPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackChunkDataPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackChunkRequestPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackClientResponsePacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackDataInfoPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackStackPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePacksInfoPacket.hpp>
#include <sculk/protocol/connection/HandShakeToken.hpp>

#include <cstdint>
#include <print>
#include <string_view>

namespace sculk {

#ifdef Debug
#define PROXY_PASS_SHOULD_LOG_PACKET(ID)                                                                               \
    (mSettings.packets_logger->black_list_mode && !mSettings.packets_logger->packet_ids->contains(ID))                 \
        || (!mSettings.packets_logger->black_list_mode && mSettings.packets_logger->packet_ids->contains(ID))
#else
#define PROXY_PASS_SHOULD_LOG_PACKET(ID) false
#endif

#ifdef ResourcePack
#define PROXY_PASS_LOG_RESOURCE(...) std::println(__VA_ARGS__)
#else
#define PROXY_PASS_LOG_RESOURCE(...) ((void)0)
#endif

namespace {

[[nodiscard]] const char* resourcePackResponseName(std::uint8_t response) noexcept {
    switch (static_cast<ResourcePackManager::ClientResponse>(response)) {
    case ResourcePackManager::ClientResponse::Refused:
        return "REFUSED";
    case ResourcePackManager::ClientResponse::SendPacks:
        return "SEND_PACKS";
    case ResourcePackManager::ClientResponse::HaveAllPacks:
        return "HAVE_ALL_PACKS";
    case ResourcePackManager::ClientResponse::Completed:
        return "COMPLETED";
    }
    return "UNKNOWN";
}

void logResourceState(std::string_view event, const ProxyBridge& bridge) {
#ifdef ResourcePack
    const auto& session = bridge.mResourcePackSession;
    PROXY_PASS_LOG_RESOURCE(
        "[ProxyPass][ResourcePack][{}] {} | clientInfoSent={}, clientRequestedTransfer={}, "
        "clientHasAllPacks={}, clientCompleted={}, clientRefused={}, upstreamInfoReceived={}, "
        "upstreamHaveAllPacksSent={}, upstreamStackReceived={}, upstreamCompletedSent={}, "
        "clientLoginSuccessSent={}, "
        "pendingClientPacks={}, queuedServerPackets={}",
        bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
        event,
        session.clientInfoSent,
        session.clientRequestedTransfer,
        session.clientHasAllPacks,
        session.clientCompleted,
        session.clientRefused,
        session.upstreamInfoReceived,
        session.upstreamHaveAllPacksSent,
        session.upstreamStackReceived,
        session.upstreamCompletedSent,
        session.clientLoginSuccessSent,
        session.pendingClientPacks.size(),
        bridge.mQueuedServerPackets.size()
    );
#else
    (void)event;
    (void)bridge;
#endif
}

} // namespace

ProxyPass::ProxyPass(protocol::AuthenticationKeyManager const& authManager, ProxySettings& settings)
: mProxyServer(1),
  mAuthManager(authManager),
  mSettings(settings) {}

bool ProxyPass::start() {
    if (!mResourcePackManager.initialize()) {
        return false;
    }
    PROXY_PASS_LOG_RESOURCE(
        "[ProxyPass][ResourcePack] Manager initialized. clientOverrideEnabled={}, proxyResourcePacks={}, "
        "proxyResourcePackRequired={}",
        mResourcePackManager.hasClientOverride(),
        mResourcePackManager.clientResourcePackCount(),
        mResourcePackManager.clientResourcePackRequired()
    );

    auto serverKeyPair = protocol::ssl::randomES384KeyPair();
    if (!serverKeyPair) {
        return false;
    }
    mProxyServerKeyPair = *serverKeyPair;

    mProxyServer.setOnDisconnected([this](const RakNet::RakNetGUID& guid, const RakNet::SystemAddress&) noexcept {
        onClientDisconnected(guid);
    });

    mProxyServer.setOnPacketReceive([this](
                                        const RakNet::RakNetGUID&            guid,
                                        const RakNet::SystemAddress&         address,
                                        std::unique_ptr<protocol::IPacket>&& packet
                                    ) noexcept { onRealClientPacket(guid, address, *packet); });
    mProxyServer.setMotd(mSettings.motd);
    return mProxyServer.start(mSettings.proxy_port, mSettings.proxy_port_v6, mSettings.max_players);
}

void ProxyPass::disconnectClient(const RakNet::RakNetGUID& guid, protocol::PlayStatus status) {
    protocol::PlayStatusPacket playStatusPacket{};
    playStatusPacket.mStatus = status;
    mProxyServer.disconnectClient(guid);

    std::shared_ptr<ProxyBridge> bridge{};
    mBridges.erase_if(guid.g, [&bridge](auto& entry) {
        bridge = entry.second;
        return true;
    });

    if (bridge && bridge->mProxyClient.isConnected()) {
        bridge->mProxyClient.disconnect();
    }
}

void ProxyPass::disconnectClient(
    const RakNet::RakNetGUID&      guid,
    std::string_view               message,
    protocol::DisconnectFailReason reason
) {
    protocol::DisconnectPacket disconnectPacket{};
    disconnectPacket.mReason  = reason;
    disconnectPacket.mMessage = message;
    if (auto session = mProxyServer.getSession(guid)) {
        protocol::Session::Buffer buffer{};
        protocol::BinaryStream    stream{buffer};
        disconnectPacket.writeWithHeader(stream);
        session->sendPacketImmediately(std::move(buffer));
    }
    mProxyServer.disconnectClient(guid);

    std::shared_ptr<ProxyBridge> bridge{};
    mBridges.erase_if(guid.g, [&bridge](auto& entry) {
        bridge = entry.second;
        return true;
    });

    if (bridge && bridge->mProxyClient.isConnected()) {
        bridge->mProxyClient.disconnect();
    }
}

void ProxyPass::onClientDisconnected(const RakNet::RakNetGUID& guid) {
    std::shared_ptr<ProxyBridge> bridge{};
    mBridges.erase_if(guid.g, [&bridge](auto& entry) {
        bridge = entry.second;
        return true;
    });

    if (!bridge) {
        return;
    }

    if (bridge->mProxyClient.isConnected()) {
        bridge->mProxyClient.disconnect();
    }
    std::println(
        "[ProxyPass] [{}] Player disconnected: {}, xuid: {}, pfid: {}",
        bridge->mRealAddress.ToString(),
        bridge->mClientInfo.name,
        bridge->mClientInfo.xuid.empty() ? "N/A" : bridge->mClientInfo.xuid,
        bridge->mClientInfo.pfid.empty() ? "N/A" : bridge->mClientInfo.pfid
    );
}

void ProxyPass::processClientPacket(ProxyBridge& bridge, const protocol::IPacket& packet) {
    auto id = packet.getId();
    switch (id) {
    case protocol::MinecraftPacketIds::Login: {
        return handleClient(bridge, static_cast<const protocol::LoginPacket&>(packet));
    }
    case protocol::MinecraftPacketIds::ClientToServerHandshake: {
        if (PROXY_PASS_SHOULD_LOG_PACKET(id)) {
            std::println("[ProxyPass] Client => Proxy | {}", packet);
        }
        if (bridge.mRealClientSession.isConnected()) {
            auto pkt = protocol::RequestNetworkSettingsPacket{};
            bridge.sendPacketToServer(pkt, true);
            if (PROXY_PASS_SHOULD_LOG_PACKET(id)) {
                std::println("[ProxyPass] Proxy => Server | {}", pkt);
            }
        }
        bridge.mClientReady.store(true, std::memory_order_release);
        logResourceState("client handshake completed; waiting for server LoginSuccess before client/proxy resource-pack exchange", bridge);
        break;
    }
    case protocol::MinecraftPacketIds::ResourcePackClientResponse: {
        return handleClient(bridge, static_cast<const protocol::ResourcePackClientResponsePacket&>(packet));
    }
    case protocol::MinecraftPacketIds::ResourcePackChunkRequest: {
        return handleClient(bridge, static_cast<const protocol::ResourcePackChunkRequestPacket&>(packet));
    }
    default: {
        if (PROXY_PASS_SHOULD_LOG_PACKET(id)) {
            std::println("[ProxyPass] Client => Proxy => Server | {}", packet);
        }
        bridge.sendPacketToServer(packet);
        break;
    }
    }
}

void ProxyPass::handleClient(protocol::Session& session, const protocol::RequestNetworkSettingsPacket& packet) {
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::RequestNetworkSettings)) {
        std::println("[ProxyPass] Client => Proxy | {}", packet);
    }
    if (packet.mClientNetworkVersion != protocol::getProtocolVersion()) {
        if (packet.mClientNetworkVersion > protocol::getProtocolVersion()) {
            return disconnectClient(session.getGuid(), protocol::PlayStatus::LoginFailedServerOld);
        } else {
            return disconnectClient(session.getGuid(), protocol::PlayStatus::LoginFailedClientOld);
        }
    }
    protocol::NetworkSettingsPacket settingsPacket{};
    protocol::Session::Buffer       buffer{};
    protocol::BinaryStream          stream{buffer};
    settingsPacket.writeWithHeader(stream);
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::NetworkSettings)) {
        std::println("[ProxyPass] Proxy => Client | {}", settingsPacket);
    }
    session.sendPacketImmediately(std::move(buffer));
    session.setCompressed(
        static_cast<protocol::Session::CompressionType>(settingsPacket.mCompressionAlgorithm),
        settingsPacket.mCompressionThreshold
    );
}

void ProxyPass::handleClient(ProxyBridge& bridge, const protocol::LoginPacket& packet) {
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::Login)) {
        std::println("[ProxyPass] Client => Proxy | {}", packet);
    }

    if (packet.mNetworkVersion != protocol::getProtocolVersion()) {
        if (packet.mNetworkVersion > protocol::getProtocolVersion()) {
            return disconnectClient(bridge.mRealGuid, protocol::PlayStatus::LoginFailedServerOld);
        } else {
            return disconnectClient(bridge.mRealGuid, protocol::PlayStatus::LoginFailedClientOld);
        }
    }

    auto request = protocol::ConnectionRequest::fromString(packet.mRawConnectionRequest);
    if (!request) {
        return disconnectClient(
            bridge.mRealGuid,
            "Invalid connection request",
            protocol::DisconnectFailReason::BadPacket
        );
    }

    bridge.mConnectionRequest = std::move(*request);
    bridge.mClientInfo.name   = bridge.mConnectionRequest.getXboxLiveName();
    bridge.mClientInfo.xuid   = bridge.mConnectionRequest.getXboxLiveID().value_or("");
    bridge.mClientInfo.pfid   = bridge.mConnectionRequest.getPlayFabID();
    if (!bridge.mConnectionRequest.verify(mAuthManager, mSettings.online_mode)) {
        return disconnectClient(
            bridge.mRealGuid,
            "Connection request verification failed",
            protocol::DisconnectFailReason::NotAuthenticated
        );
    }

    auto token = protocol::HandShakeToken::random(mProxyServerKeyPair);
    if (!token) {
        return disconnectClient(
            bridge.mRealGuid,
            "Failed to generate handshake token",
            protocol::DisconnectFailReason::BadPacket
        );
    }
    protocol::ServerToClientHandshakePacket handshakePacket{};
    handshakePacket.mHandshakeWebToken = token->toString();

    bridge.sendPacketToClient(handshakePacket, true);
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ServerToClientHandshake)) {
        std::println("[ProxyPass] Proxy => Client | {}", handshakePacket);
    }

    auto sessionToken = protocol::CryptoManager::computeSessionKey(
        mProxyServerKeyPair.mPrivateKeyPem,
        bridge.mConnectionRequest.getClientPublicKey(),
        token->getSaltBytes()
    );
    if (!sessionToken) {
        return disconnectClient(
            bridge.mRealGuid,
            "Failed to compute client session token",
            protocol::DisconnectFailReason::BadPacket
        );
    }
    bridge.mRealClientSession.setEncrypted(std::move(*sessionToken));
    auto pfid = bridge.mConnectionRequest.getPlayFabID();
    std::println(
        "[ProxyPass] [{}] Player connected: {}, xuid: {}, pfid: {}",
        bridge.mRealAddress.ToString(),
        bridge.mClientInfo.name,
        bridge.mClientInfo.xuid.empty() ? "N/A" : bridge.mClientInfo.xuid,
        bridge.mClientInfo.pfid.empty() ? "N/A" : bridge.mClientInfo.pfid
    );
}

void ProxyPass::handleClient(ProxyBridge& bridge, const protocol::ResourcePackClientResponsePacket& packet) {
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackClientResponse)) {
        std::println("[ProxyPass] Client => Proxy | {}", packet);
    }

    PROXY_PASS_LOG_RESOURCE(
        "[ProxyPass][ResourcePack][{}] Client => Proxy response={}, requestedPacks={}, "
        "rawResponse={}, clientOverrideEnabled={}, upstreamInfoReceived={}, upstreamStackReceived={}",
        bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
        resourcePackResponseName(packet.mResponse),
        packet.mPackIds.size(),
        packet.mResponse,
        mResourcePackManager.hasClientOverride(),
        bridge.mResourcePackSession.upstreamInfoReceived,
        bridge.mResourcePackSession.upstreamStackReceived
    );

    mResourcePackManager.noteClientResponse(bridge.mResourcePackSession, packet);
    logResourceState("client response recorded", bridge);
    if (!mResourcePackManager.hasClientOverride()) {
        bridge.sendPacketToServer(packet);
        if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackClientResponse)) {
            std::println("[ProxyPass] Proxy => Server | {}", packet);
        }
        return;
    }

    switch (static_cast<ResourcePackManager::ClientResponse>(packet.mResponse)) {
    case ResourcePackManager::ClientResponse::Refused: {
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Client refused required proxy packs; disconnecting downstream without forwarding REFUSED upstream.",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name
        );
        return disconnectClient(
            bridge.mRealGuid,
            "Client refused required proxy resource packs",
            protocol::DisconnectFailReason::ResourcePackProblem
        );
    }
    case ResourcePackManager::ClientResponse::SendPacks: {
        auto response = mResourcePackManager.makeClientResponse(ResourcePackManager::ClientResponse::SendPacks);
        bridge.mResourcePackSession.pendingClientPacks = std::move(response.mPackIds);
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Client requested proxy packs. pendingClientPacks={}",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
            bridge.mResourcePackSession.pendingClientPacks.size()
        );
        sendNextClientResourcePackInfo(bridge);
        return;
    }
    case ResourcePackManager::ClientResponse::HaveAllPacks:
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Client reports all proxy pack chunks downloaded; sending proxy ResourcePackStack.",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name
        );
        bridge.sendPacketToClient(mResourcePackManager.clientStackPacket());
        if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackStack)) {
            std::println("[ProxyPass] Proxy => Client | {}", mResourcePackManager.clientStackPacket());
        }
        return;
    case ResourcePackManager::ClientResponse::Completed: {
        logResourceState("client completed proxy resource-pack application", bridge);
        completeUpstreamResourcePackIfReady(bridge);
        return;
    }
    }
}

void ProxyPass::handleClient(ProxyBridge& bridge, const protocol::ResourcePackChunkRequestPacket& packet) {
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackChunkRequest)) {
        std::println("[ProxyPass] Client => Proxy | {}", packet);
    }

    if (auto cachedChunk = mResourcePackManager.findClientChunk(packet)) {
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Client requested chunk resource={}, index={} -> cache hit, bytes={}, offset={}.",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
            packet.mResourceName,
            packet.mChunkIndex,
            cachedChunk->mChunkData.size(),
            cachedChunk->mBytesOffset
        );
        bridge.sendPacketToClient(*cachedChunk);
        if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackChunkData)) {
            std::println("[ProxyPass] Proxy => Client | {}", *cachedChunk);
        }
        const auto& sendingPack = bridge.mResourcePackSession.sendingClientPack;
        if (sendingPack && packet.mResourceName == sendingPack->mResourceName
            && packet.mChunkIndex + 1U >= sendingPack->mChunkIndex) {
            PROXY_PASS_LOG_RESOURCE(
                "[ProxyPass][ResourcePack][{}] Last chunk requested for resource={}; moving to next proxy pack.",
                bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
                packet.mResourceName
            );
            sendNextClientResourcePackInfo(bridge);
        }
        return;
    }

    PROXY_PASS_LOG_RESOURCE(
        "[ProxyPass][ResourcePack][{}] Client requested unknown/local-missing chunk resource={}, index={}; forwarding to upstream fallback.",
        bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
        packet.mResourceName,
        packet.mChunkIndex
    );
    bridge.sendPacketToServer(packet);
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackChunkRequest)) {
        std::println("[ProxyPass] Proxy => Server | {}", packet);
    }
}

void ProxyPass::handleFirstClientPacket(
    const RakNet::RakNetGUID&    guid,
    const RakNet::SystemAddress& address,
    const protocol::IPacket&     packet,
    protocol::Session&           session
) {
    if (packet.getId() != protocol::MinecraftPacketIds::RequestNetworkSettings) {
        return;
    }

    std::shared_ptr<ProxyBridge> bridge{};
    auto [bridgePtr, inserted] = mBridges.try_emplace_p(guid.g, std::make_shared<ProxyBridge>(guid, address, session));
    bridge                     = bridgePtr->second;

    std::weak_ptr<ProxyBridge> weakBridge = bridge;

    bridge->mProxyClient.setOnPacketReceive([this, weakBridge](std::unique_ptr<protocol::IPacket>&& packet) noexcept {
        auto currentBridge = weakBridge.lock();
        if (!currentBridge) {
            return;
        }
        processServerPacket(*currentBridge, *packet);
    });

    bridge->mProxyClient.setOnConnected([this, weakBridge]() noexcept {
        auto currentBridge = weakBridge.lock();
        if (!currentBridge) {
            return;
        }

        logResourceState("upstream transport connected", *currentBridge);

        if (!currentBridge->mClientReady.load(std::memory_order_acquire)) {
            logResourceState("upstream connected before client handshake; waiting before requesting network settings", *currentBridge);
            return;
        }

        auto pkt = protocol::RequestNetworkSettingsPacket{};
        currentBridge->sendPacketToServer(pkt, true);
        if (PROXY_PASS_SHOULD_LOG_PACKET(pkt.getId())) {
            std::println("[ProxyPass] Proxy => Server | {}", pkt);
        }

        currentBridge->mClientReady.store(true, std::memory_order_release);
        logResourceState("upstream network settings requested after delayed connection", *currentBridge);
    });

    bridge->mProxyClient.setOnConnectionFailed([this, weakBridge]() noexcept {
        auto currentBridge = weakBridge.lock();
        if (!currentBridge) {
            return;
        }

        std::println(
            "[ProxyPass] Failed to connect to upstream server for player: {}.",
            currentBridge->mConnectionRequest.getXboxLiveName()
        );
        std::println(
            "[ProxyPass] [{}] Player disconnected: {}, xuid: {}, pfid: {}",
            currentBridge->mRealAddress.ToString(),
            currentBridge->mClientInfo.name,
            currentBridge->mClientInfo.xuid.empty() ? "N/A" : currentBridge->mClientInfo.xuid,
            currentBridge->mClientInfo.pfid.empty() ? "N/A" : currentBridge->mClientInfo.pfid
        );
        disconnectClient(
            currentBridge->mRealGuid,
            "Failed to connect to upstream server",
            protocol::DisconnectFailReason::CantConnect
        );
    });

    if (!bridge->mProxyClient.connect(mSettings.upstream_host, mSettings.upstream_port)) {
        std::println(
            "[ProxyPass] Failed to connect to upstream server for player: {}.",
            bridge->mConnectionRequest.getXboxLiveName()
        );
        std::println(
            "[ProxyPass] [{}] Player disconnected: {}, xuid: {}, pfid: {}",
            bridge->mRealAddress.ToString(),
            bridge->mClientInfo.name,
            bridge->mClientInfo.xuid.empty() ? "N/A" : bridge->mClientInfo.xuid,
            bridge->mClientInfo.pfid.empty() ? "N/A" : bridge->mClientInfo.pfid
        );
        disconnectClient(guid, "Failed to connect to upstream server", protocol::DisconnectFailReason::CantConnect);
        return;
    }

    return handleClient(session, static_cast<const protocol::RequestNetworkSettingsPacket&>(packet));
}

void ProxyPass::onRealClientPacket(
    const RakNet::RakNetGUID&    guid,
    const RakNet::SystemAddress& address,
    const protocol::IPacket&     packet
) {
    auto session = mProxyServer.getSession(guid);
    if (!session) {
        return;
    }

    std::shared_ptr<ProxyBridge> bridge{};
    if (!mBridges.if_contains(guid.g, [&bridge](auto const& entry) { bridge = entry.second; })) {
        return handleFirstClientPacket(guid, address, packet, *session);
    }

    processClientPacket(*bridge, packet);
}

void ProxyPass::processServerPacket(ProxyBridge& bridge, const protocol::IPacket& packet) {
    auto id = packet.getId();
    switch (id) {
    case protocol::MinecraftPacketIds::NetworkSettings: {
        handleServer(bridge, static_cast<const protocol::NetworkSettingsPacket&>(packet));
        break;
    }
    case protocol::MinecraftPacketIds::ServerToClientHandshake: {
        handleServer(bridge, static_cast<const protocol::ServerToClientHandshakePacket&>(packet));
        break;
    }
    case protocol::MinecraftPacketIds::PlayStatus: {
        handleServer(bridge, static_cast<const protocol::PlayStatusPacket&>(packet));
        break;
    }
    case protocol::MinecraftPacketIds::ResourcePacksInfo: {
        handleServer(bridge, static_cast<const protocol::ResourcePacksInfoPacket&>(packet));
        break;
    }
    case protocol::MinecraftPacketIds::ResourcePackStack: {
        handleServer(bridge, static_cast<const protocol::ResourcePackStackPacket&>(packet));
        break;
    }
    case protocol::MinecraftPacketIds::ResourcePackDataInfo: {
        handleServer(bridge, static_cast<const protocol::ResourcePackDataInfoPacket&>(packet));
        break;
    }
    case protocol::MinecraftPacketIds::ResourcePackChunkData: {
        handleServer(bridge, static_cast<const protocol::ResourcePackChunkDataPacket&>(packet));
        break;
    }
    default: {
        forwardOrQueueServerPacket(bridge, packet);
        break;
    }
    }
}

void ProxyPass::handleServer(ProxyBridge& bridge, const protocol::NetworkSettingsPacket& packet) {
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::NetworkSettings)) {
        std::println("[ProxyPass] Server => Proxy | {}", packet);
    }
    bridge.mProxyClient.getSession().setCompressed(
        static_cast<protocol::Session::CompressionType>(packet.mCompressionAlgorithm),
        packet.mCompressionThreshold
    );

    (void)bridge.mConnectionRequest.selfSign(mProxyServerKeyPair);
    protocol::LoginPacket loginPacket{bridge.mConnectionRequest.toString()};

    bridge.sendPacketToServer(loginPacket, true);
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::Login)) {
        std::println("[ProxyPass] Proxy => Server | {}", loginPacket);
    }
}

void ProxyPass::handleServer(ProxyBridge& bridge, const protocol::ServerToClientHandshakePacket& packet) {
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ServerToClientHandshake)) {
        std::println("[ProxyPass] Server => Proxy | {}", packet);
    }
    auto handshakeToken = protocol::HandShakeToken::fromString(packet.mHandshakeWebToken);
    if (!handshakeToken || !handshakeToken->verify()) {
        auto pfid = bridge.mConnectionRequest.getPlayFabID();
        std::println(
            "[ProxyPass] [{}] Player disconnected: {}, xuid: {}, pfid: {}",
            bridge.mRealAddress.ToString(),
            bridge.mClientInfo.name,
            bridge.mClientInfo.xuid.empty() ? "N/A" : bridge.mClientInfo.xuid,
            bridge.mClientInfo.pfid.empty() ? "N/A" : bridge.mClientInfo.pfid
        );
        return disconnectClient(bridge.mRealGuid, "Invalid handshake token", protocol::DisconnectFailReason::BadPacket);
    }

    auto sessionKey = protocol::CryptoManager::computeSessionKey(
        mProxyServerKeyPair.mPrivateKeyPem,
        handshakeToken->getRemotePublicKey(),
        handshakeToken->getSaltBytes()
    );
    if (!sessionKey) {
        std::println(
            "[ProxyPass] [{}] Player disconnected: {}, xuid: {}, pfid: {}",
            bridge.mRealAddress.ToString(),
            bridge.mClientInfo.name,
            bridge.mClientInfo.xuid.empty() ? "N/A" : bridge.mClientInfo.xuid,
            bridge.mClientInfo.pfid.empty() ? "N/A" : bridge.mClientInfo.pfid
        );
        return disconnectClient(
            bridge.mRealGuid,
            "Failed to compute server session key",
            protocol::DisconnectFailReason::BadPacket
        );
    }
    bridge.mProxyClient.getSession().setEncrypted(std::move(*sessionKey));
    protocol::ClientToServerHandshakePacket handshakePacket{};
    bridge.sendPacketToServer(handshakePacket, true);
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ClientToServerHandshake)) {
        std::println("[ProxyPass] Proxy => Server | {}", handshakePacket);
    }
}

void ProxyPass::handleServer(ProxyBridge& bridge, const protocol::PlayStatusPacket& packet) {
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::PlayStatus)) {
        std::println("[ProxyPass] Server => Proxy => Client | {}", packet);
    }
    bridge.sendPacketToClient(packet);

    if (packet.mStatus == protocol::PlayStatus::LoginSuccess) {
        bridge.mResourcePackSession.clientLoginSuccessSent = true;
        logResourceState("server LoginSuccess forwarded; trying to start client/proxy resource-pack exchange", bridge);
        startClientResourcePackHandshake(bridge);
    } else {
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Server => Client PlayStatus forwarded before barrier: status={}.",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
            static_cast<int>(packet.mStatus)
        );
    }
}

void ProxyPass::handleServer(ProxyBridge& bridge, const protocol::ResourcePacksInfoPacket& packet) {
    mResourcePackManager.captureUpstream(bridge.mResourcePackSession, packet);
    PROXY_PASS_LOG_RESOURCE(
        "[ProxyPass][ResourcePack][{}] Server => Proxy ResourcePacksInfo: upstreamResourcePacks={}, "
        "required={}, hasAddonPacks={}, hasScripts={}, vibrantVisualsForceDisabled={}, "
        "worldTemplateVersion='{}', clientOverrideEnabled={}.",
        bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
        packet.mResourcePacks.size(),
        packet.mResourcePackRequired,
        packet.mHasAddonPacks,
        packet.mHasScripts,
        packet.mIsVibrantVisualsForceDisabled,
        packet.mWorldTemplateVersion,
        mResourcePackManager.hasClientOverride()
    );
    logResourceState("upstream resource-pack info captured", bridge);
    if (mResourcePackManager.hasClientOverride()) {
        auto haveAll = mResourcePackManager.makeUpstreamResponse(ResourcePackManager::ClientResponse::HaveAllPacks);
        bridge.mResourcePackSession.upstreamHaveAllPacksSent = true;
        bridge.sendPacketToServer(haveAll);
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Proxy => Server HAVE_ALL_PACKS; upstream packs are not downloaded by proxy.",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name
        );
        if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackClientResponse)) {
            std::println("[ProxyPass] Proxy => Server | {}", haveAll);
        }

        startClientResourcePackHandshake(bridge);
        return;
    }

    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePacksInfo)) {
        std::println("[ProxyPass] Server => Proxy => Client | {}", packet);
    }
    bridge.sendPacketToClient(packet);
}

void ProxyPass::handleServer(ProxyBridge& bridge, const protocol::ResourcePackStackPacket& packet) {
    mResourcePackManager.captureUpstream(bridge.mResourcePackSession, packet);
    PROXY_PASS_LOG_RESOURCE(
        "[ProxyPass][ResourcePack][{}] Server => Proxy ResourcePackStack captured: texturePackRequired={}, "
        "texturePacks={}, addonPacks={}, baseGameVersion='{}', includeEditorPacks={}; attempting upstream completion.",
        bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
        packet.mTexturePackRequired,
        packet.mTexturePackList.size(),
        packet.mAddonList.size(),
        packet.mBaseGameVersion,
        packet.mIncludeEditorPacks
    );
    logResourceState("upstream resource-pack stack captured", bridge);
    if (mResourcePackManager.hasClientOverride()) {
        completeUpstreamResourcePackIfReady(bridge);
        return;
    }

    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackStack)) {
        std::println("[ProxyPass] Server => Proxy => Client | {}", packet);
    }
    bridge.sendPacketToClient(packet);
}

void ProxyPass::handleServer(ProxyBridge& bridge, const protocol::ResourcePackDataInfoPacket& packet) {
    mResourcePackManager.captureUpstream(packet);
    if (mResourcePackManager.hasClientOverride()) {
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Server => Proxy ResourcePackDataInfo ignored in override mode: resource={}, chunks={}, fileSize={}.",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
            packet.mResourceName,
            packet.mChunkIndex,
            packet.mFileSize
        );
        return;
    }

    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackDataInfo)) {
        std::println("[ProxyPass] Server => Proxy => Client | {}", packet);
    }
    bridge.sendPacketToClient(packet);
}

void ProxyPass::handleServer(ProxyBridge& bridge, const protocol::ResourcePackChunkDataPacket& packet) {
    mResourcePackManager.captureUpstream(packet);
    if (mResourcePackManager.hasClientOverride()) {
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Server => Proxy ResourcePackChunkData ignored in override mode: resource={}, index={}, bytes={}.",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
            packet.mResourceName,
            packet.mChunkIndex,
            packet.mChunkData.size()
        );
        return;
    }

    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackChunkData)) {
        std::println("[ProxyPass] Server => Proxy => Client | {}", packet);
    }
    bridge.sendPacketToClient(packet);
}

void ProxyPass::startClientResourcePackHandshake(ProxyBridge& bridge) {
    if (!mResourcePackManager.hasClientOverride() || bridge.mResourcePackSession.clientInfoSent) {
        if (!mResourcePackManager.hasClientOverride()) {
            logResourceState("client resource-pack handshake skipped because override is disabled", bridge);
        } else {
            logResourceState("client resource-pack info already sent; skipping duplicate send", bridge);
        }
        return;
    }
    if (!bridge.mResourcePackSession.clientLoginSuccessSent) {
        logResourceState("client resource-pack handshake deferred until server LoginSuccess is forwarded", bridge);
        return;
    }

    const auto& clientInfo = mResourcePackManager.clientInfoPacket();
    bridge.mResourcePackSession.clientInfoSent = true;
    PROXY_PASS_LOG_RESOURCE(
        "[ProxyPass][ResourcePack][{}] Proxy => Client ResourcePacksInfo: proxyResourcePacks={}, "
        "required={}, hasAddonPacks={}, hasScripts={}, vibrantVisualsForceDisabled={}, worldTemplateVersion='{}'.",
        bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
        clientInfo.mResourcePacks.size(),
        clientInfo.mResourcePackRequired,
        clientInfo.mHasAddonPacks,
        clientInfo.mHasScripts,
        clientInfo.mIsVibrantVisualsForceDisabled,
        clientInfo.mWorldTemplateVersion
    );
    bridge.sendPacketToClient(clientInfo);
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePacksInfo)) {
        std::println("[ProxyPass] Proxy => Client | {}", clientInfo);
    }
}

void ProxyPass::sendNextClientResourcePackInfo(ProxyBridge& bridge) {
    auto& session = bridge.mResourcePackSession;
    while (!session.pendingClientPacks.empty()) {
        auto resourceName = std::move(session.pendingClientPacks.front());
        session.pendingClientPacks.erase(session.pendingClientPacks.begin());
        if (auto dataInfo = mResourcePackManager.findClientDataInfo(resourceName)) {
            session.sendingClientPack = *dataInfo;
            PROXY_PASS_LOG_RESOURCE(
                "[ProxyPass][ResourcePack][{}] Proxy => Client ResourcePackDataInfo: resource={}, chunks={}, "
                "chunkSize={}, fileSize={}, pendingAfterThis={}.",
                bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
                dataInfo->mResourceName,
                dataInfo->mChunkIndex,
                dataInfo->mChunkSize,
                dataInfo->mFileSize,
                session.pendingClientPacks.size()
            );
            bridge.sendPacketToClient(*dataInfo);
            if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackDataInfo)) {
                std::println("[ProxyPass] Proxy => Client | {}", *dataInfo);
            }
            return;
        }
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Client requested resource pack not found in proxy cache: {}.",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
            resourceName
        );
    }
    session.sendingClientPack.reset();
    logResourceState("all requested proxy ResourcePackDataInfo packets have been sent", bridge);
}

void ProxyPass::completeUpstreamResourcePackIfReady(ProxyBridge& bridge) {
    auto& session = bridge.mResourcePackSession;
    if (!mResourcePackManager.hasClientOverride()) {
        return;
    }

    if (!session.upstreamStackReceived) {
        logResourceState("upstream completion deferred because ResourcePackStack has not arrived", bridge);
        return;
    }

    if (!session.upstreamCompletedSent) {
        auto completed = mResourcePackManager.makeUpstreamResponse(ResourcePackManager::ClientResponse::Completed);
        session.upstreamCompletedSent = true;
        bridge.sendPacketToServer(completed);
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Proxy => Server COMPLETED.",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name
        );
        if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ResourcePackClientResponse)) {
            std::println("[ProxyPass] Proxy => Server | {}", completed);
        }
    } else {
        logResourceState("upstream completion already sent; checking barrier for queued packet flush", bridge);
    }

    if (resourcePackBarrierSatisfied(bridge)) {
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Barrier satisfied; flushing queued server packets: count={}.",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
            bridge.mQueuedServerPackets.size()
        );
        bridge.flushQueuedPacketsToClient();
    } else {
        logResourceState("resource-pack barrier not yet satisfied; queued server packets remain held", bridge);
    }
}

bool ProxyPass::resourcePackBarrierSatisfied(const ProxyBridge& bridge) const {
    if (!mResourcePackManager.hasClientOverride()) {
        return true;
    }

    const auto& session = bridge.mResourcePackSession;
    return session.clientCompleted && session.upstreamCompletedSent;
}

void ProxyPass::forwardOrQueueServerPacket(ProxyBridge& bridge, const protocol::IPacket& packet) {
    if (!resourcePackBarrierSatisfied(bridge)) {
        bridge.queuePacketToClient(packet);
        PROXY_PASS_LOG_RESOURCE(
            "[ProxyPass][ResourcePack][{}] Queued server packet id={} until barrier is satisfied. queuedServerPackets={}.",
            bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name,
            static_cast<int>(packet.getId()),
            bridge.mQueuedServerPackets.size()
        );
        if (PROXY_PASS_SHOULD_LOG_PACKET(packet.getId())) {
            std::println("[ProxyPass] Server => Proxy queued until ResourcePack is complete | {}", packet);
        }
        return;
    }

    if (PROXY_PASS_SHOULD_LOG_PACKET(packet.getId())) {
        std::println("[ProxyPass] Server => Proxy => Client | {}", packet);
    }
    bridge.sendPacketToClient(packet);
}

} // namespace sculk
