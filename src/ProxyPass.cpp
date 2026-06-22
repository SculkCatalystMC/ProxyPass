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
#include "I18n.hpp"
#include "Logger.hpp"
#include <iostream>
#include <print>
#include <sculk/protocol/codec/MinecraftPackets.hpp>
#include <sculk/protocol/codec/packet/ClientToServerHandshakePacket.hpp>
#include <sculk/protocol/codec/packet/DisconnectPacket.hpp>
#include <sculk/protocol/codec/packet/PlayStatusPacket.hpp>
#include <sculk/protocol/connection/HandShakeToken.hpp>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace sculk {

void ProxyPass::initConsole() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdout != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hStdout, &dwMode)) {
            SetConsoleMode(hStdout, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }

    HANDLE hStderr = GetStdHandle(STD_ERROR_HANDLE);
    if (hStderr != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hStderr, &dwMode)) {
            SetConsoleMode(hStderr, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif
    std::ios::sync_with_stdio(false);
    std::print("\033]0;ProxyPass\007");
}

#define PROXY_PASS_SHOULD_LOG_PACKET(ID)                                                                               \
    (mSettings.packets_logger->black_list_mode && !mSettings.packets_logger->packet_ids->contains(ID))                 \
        || (!mSettings.packets_logger->black_list_mode && mSettings.packets_logger->packet_ids->contains(ID))

ProxyPass::ProxyPass() : mProxyServer(1), mLogger("ProxyPass") {}

ProxyPass::~ProxyPass() { mSettings.save(); }

Logger& ProxyPass::getLogger() { return mLogger; }

bool ProxyPass::start() {
    std::chrono::high_resolution_clock::time_point startTime = std::chrono::high_resolution_clock::now();

    mSettings.load();

    i18n::init({
        .langDir                 = "./lang",
        .userLangDir             = "./lang/user",
        .locale                  = mSettings.language,
        .autoInstallMissingFiles = mSettings.i18n_auto_install,
        .checkCompleteness       = mSettings.i18n_check_completeness,
        .writeMissingReport      = mSettings.i18n_write_missing_report,
    });

    getLogger().info(i18n::tr("proxy.starting", "Starting proxy server..."));
    getLogger().info(i18n::tr(
        "proxy.version",
        "Version: {0}(ProtocolVersion {1})",
        protocol::getMinecraftVersion(),
        protocol::getProtocolVersion()
    ));

    auto serverKeyPair = protocol::ssl::randomES384KeyPair();
    if (!serverKeyPair) {
        getLogger().fatal(i18n::tr(
            "crypto.server_key_pair_failed",
            "Failed to generate server key pair: {0}",
            serverKeyPair.error().message()
        ));
        return false;
    }
    mProxyServerKeyPair = *serverKeyPair;

    if (!mProxyServer.setOnDisconnected([this](const RakNet::RakNetGUID& guid, const RakNet::SystemAddress&) noexcept {
            onClientDisconnected(guid);
        })) [[unlikely]] {
        getLogger().fatal(
            i18n::tr("proxy.callback_disconnect_failed", "Failed to set proxy server disconnect callback.")
        );
        return false;
    }

    if (!mProxyServer.setOnPacketReceive([this](
                                             const RakNet::RakNetGUID&            guid,
                                             const RakNet::SystemAddress&         address,
                                             std::unique_ptr<protocol::IPacket>&& packet
                                         ) noexcept { onRealClientPacket(guid, address, *packet); })) [[unlikely]] {
        getLogger().fatal(
            i18n::tr("proxy.callback_packet_receive_failed", "Failed to set proxy server packet receive callback.")
        );
        return false;
    }

    if (mSettings.packets_logger->log_parse_error) {
        if (!mProxyServer.setOnPacketParseFailed([this](
                                                     const RakNet::RakNetGUID&,
                                                     const RakNet::SystemAddress&,
                                                     protocol::Session::Buffer&&,
                                                     std::string message
                                                 ) noexcept {
                getLogger().error(i18n::tr("proxy.packet_parse_failed", "Failed to parse packet: {0}", message));
            })) [[unlikely]] {
            getLogger().fatal(i18n::tr(
                "proxy.callback_packet_parse_failed",
                "Failed to set proxy server packet parse failure callback."
            ));
            return false;
        }
    }

    mProxyServer.setMotd(mSettings.motd);
    auto startResult = mProxyServer.start(mSettings.proxy_port, mSettings.proxy_port_v6, mSettings.max_players);
    if (startResult != protocol::NetworkStartResult::Success) [[unlikely]] {
        getLogger().fatal(i18n::tr("network.start_failed", "Failed to start proxy server."));
        if (startResult == protocol::NetworkStartResult::SocketPortAlreadyInUse) {
            getLogger().fatal(i18n::tr(
                "network.port_in_use",
                "Port [{0}] may be in use by another process. Free up port and re-run program or adjust "
                "proxy_settings.jsonc file to use alternate ports for proxy server",
                mSettings.proxy_port
            ));
            getLogger().fatal(i18n::tr(
                "network.port_in_use",
                "Port [{0}] may be in use by another process. Free up port and re-run program or adjust "
                "proxy_settings.jsonc file to use alternate ports for proxy server",
                mSettings.proxy_port_v6
            ));
        }
        getLogger().fatal(i18n::tr(
            "proxy.exiting_with_code",
            "Exiting program with error code {0}.",
            static_cast<std::underlying_type_t<protocol::NetworkStartResult>>(startResult)
        ));
        return false;
    }

    getLogger().info(i18n::tr("network.ipv4_supported", "IPv4 supported, port: {0}", mSettings.proxy_port));
    getLogger().info(i18n::tr("network.ipv6_supported", "IPv6 supported, port: {0}", mSettings.proxy_port_v6));

    if (mSettings.online_mode) {
        getLogger().info(i18n::tr("auth.waiting_services", "Waiting for Minecraft services..."));
        if (auto status = mAuthManager.initMojangPublicKeyFromNetwork(); !status) {
            getLogger().fatal(i18n::tr(
                "auth.services_failed",
                "Failed to connect to Minecraft services: {0}",
                status.error().message()
            ));
            return false;
        }
    } else {
        mAuthManager.initMojangPublicKeyFromCachedKeys();
    }

    std::chrono::high_resolution_clock::time_point endTime = std::chrono::high_resolution_clock::now();

    getLogger().info(i18n::tr(
        "proxy.started",
        "Proxy server started in {0:.2f} seconds.",
        std::chrono::duration<double>(endTime - startTime).count()
    ));
    return true;
}

void ProxyPass::disconnectClient(const RakNet::RakNetGUID& guid, protocol::PlayStatus status) {
    protocol::PlayStatusPacket playStatusPacket{};
    playStatusPacket.mStatus = status;
    mProxyServer.disconnectClient(guid);

    std::shared_ptr<ProxyBridge> bridge{};
    mBridges.erase_if(guid, [&bridge](auto& entry) {
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
    if (auto session = mProxyServer.getSession(guid).lock()) {
        protocol::Session::Buffer buffer{};
        protocol::BinaryStream    stream{buffer};
        disconnectPacket.writeWithHeader(stream);
        session->sendPacketImmediately(std::move(buffer));
    }
    mProxyServer.disconnectClient(guid);

    std::shared_ptr<ProxyBridge> bridge{};
    mBridges.erase_if(guid, [&bridge](auto& entry) {
        bridge = entry.second;
        return true;
    });

    if (bridge && bridge->mProxyClient.isConnected()) {
        bridge->mProxyClient.disconnect();
    }
}

void ProxyPass::onClientDisconnected(const RakNet::RakNetGUID& guid) {
    std::shared_ptr<ProxyBridge> bridge{};
    mBridges.erase_if(guid, [&bridge](auto& entry) {
        bridge = entry.second;
        return true;
    });

    if (!bridge) {
        return;
    }

    if (bridge->mProxyClient.isConnected()) {
        bridge->mProxyClient.disconnect();
    }
    getLogger().info(
        "[{}] Player disconnected: {}, xuid: {}, pfid: {}",
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
            getLogger().info("Client => Proxy | {}", packet);
        }
        if (bridge.mRealClientSession.isConnected()) {
            auto pkt = protocol::RequestNetworkSettingsPacket{};
            bridge.sendPacketToServer(pkt, true);
            if (PROXY_PASS_SHOULD_LOG_PACKET(id)) {
                getLogger().info("Proxy => Server | {}", pkt);
            }
        }
        bridge.mClientReady.store(true, std::memory_order_release);
        break;
    }
    default: {
        if (PROXY_PASS_SHOULD_LOG_PACKET(id)) {
            getLogger().info("Client => Proxy => Server | {}", packet);
        }
        bridge.sendPacketToServer(packet);
        break;
    }
    }
}

void ProxyPass::handleClient(protocol::Session& session, const protocol::RequestNetworkSettingsPacket& packet) {
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::RequestNetworkSettings)) {
        getLogger().info("Client => Proxy | {}", packet);
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
        getLogger().info("Proxy => Client | {}", settingsPacket);
    }
    session.sendPacketImmediately(std::move(buffer));
    session.setCompressed(settingsPacket.mCompressionAlgorithm, settingsPacket.mCompressionThreshold);
}

void ProxyPass::handleClient(ProxyBridge& bridge, const protocol::LoginPacket& packet) {
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::Login)) {
        getLogger().info("Client => Proxy | {}", packet);
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
            i18n::tr("client.invalid_connection_request", "Invalid connection request"),
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
            i18n::tr("client.connection_verification_failed", "Connection request verification failed"),
            protocol::DisconnectFailReason::NotAuthenticated
        );
    }

    auto token = protocol::HandShakeToken::random(mProxyServerKeyPair);
    if (!token) {
        return disconnectClient(
            bridge.mRealGuid,
            i18n::tr("client.failed_generate_handshake_token", "Failed to generate handshake token"),
            protocol::DisconnectFailReason::BadPacket
        );
    }
    protocol::ServerToClientHandshakePacket handshakePacket{};
    handshakePacket.mHandshakeWebToken = token->toString();

    bridge.sendPacketToClient(handshakePacket, true);
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ServerToClientHandshake)) {
        getLogger().info("Proxy => Client | {}", handshakePacket);
    }

    auto sessionToken = protocol::CryptoManager::computeSessionKey(
        mProxyServerKeyPair.mPrivateKeyPem,
        bridge.mConnectionRequest.getClientPublicKey(),
        token->getSaltBytes()
    );
    if (!sessionToken) {
        return disconnectClient(
            bridge.mRealGuid,
            i18n::tr("client.failed_compute_session_token", "Failed to compute client session token"),
            protocol::DisconnectFailReason::BadPacket
        );
    }
    bridge.mRealClientSession.setEncrypted(std::move(*sessionToken));
    getLogger().info(
        "[{}] Player connected: {}, xuid: {}, pfid: {}",
        bridge.mRealAddress.ToString(),
        bridge.mClientInfo.name,
        bridge.mClientInfo.xuid.empty() ? "N/A" : bridge.mClientInfo.xuid,
        bridge.mClientInfo.pfid.empty() ? "N/A" : bridge.mClientInfo.pfid
    );
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
    auto [bridgePtr, inserted] = mBridges.try_emplace_p(guid, std::make_shared<ProxyBridge>(guid, address, session));
    bridge                     = bridgePtr->second;

    std::weak_ptr<ProxyBridge> weakBridge = bridge;

    if (!bridge->mProxyClient.setOnPacketReceive([this,
                                                  weakBridge](std::unique_ptr<protocol::IPacket>&& packet) noexcept {
            auto currentBridge = weakBridge.lock();
            if (!currentBridge) {
                return;
            }
            processServerPacket(*currentBridge, *packet);
        })) [[unlikely]] {
        getLogger().error(
            i18n::tr("upstream.callback_packet_receive_failed", "Failed to set upstream packet receive callback.")
        );
        return;
    }

    if (mSettings.packets_logger->log_parse_error) {
        if (!bridge->mProxyClient.setOnPacketParseFailed(
                [this](protocol::Session::Buffer&&, std::string message) noexcept {
                    getLogger().error(i18n::tr("proxy.packet_parse_failed", "Failed to parse packet: {0}", message));
                }
            )) [[unlikely]] {
            getLogger().error(i18n::tr(
                "upstream.callback_packet_parse_failed",
                "Failed to set upstream packet parse failure callback."
            ));
            return;
        }
    }

    if (!bridge->mProxyClient.setOnConnected([this, weakBridge]() noexcept {
            auto currentBridge = weakBridge.lock();
            if (!currentBridge) {
                return;
            }

            if (!currentBridge->mClientReady.load(std::memory_order_acquire)) {
                return;
            }

            auto pkt = protocol::RequestNetworkSettingsPacket{};
            currentBridge->sendPacketToServer(pkt, true);
            if (PROXY_PASS_SHOULD_LOG_PACKET(pkt.getId())) {
                getLogger().info("Proxy => Server | {}", pkt);
            }

            currentBridge->mClientReady.store(true, std::memory_order_release);
        })) [[unlikely]] {
        getLogger().error(i18n::tr("upstream.callback_connected_failed", "Failed to set upstream connected callback."));
        return;
    }

    if (!bridge->mProxyClient.setOnConnectionFailed([this, weakBridge]() noexcept {
            auto currentBridge = weakBridge.lock();
            if (!currentBridge) {
                return;
            }

            getLogger().info(i18n::tr(
                "upstream.connect_player_failed",
                "Failed to connect to upstream server for player: {0}.",
                currentBridge->mConnectionRequest.getXboxLiveName()
            ));
            getLogger().info(
                "[{}] Player disconnected: {}, xuid: {}, pfid: {}",
                currentBridge->mRealAddress.ToString(),
                currentBridge->mClientInfo.name,
                currentBridge->mClientInfo.xuid.empty() ? "N/A" : currentBridge->mClientInfo.xuid,
                currentBridge->mClientInfo.pfid.empty() ? "N/A" : currentBridge->mClientInfo.pfid
            );
            disconnectClient(
                currentBridge->mRealGuid,
                i18n::tr("client.failed_upstream_connect", "Failed to connect to upstream server"),
                protocol::DisconnectFailReason::CantConnect
            );
        })) [[unlikely]] {
        getLogger().error(i18n::tr(
            "upstream.callback_connection_failure_failed",
            "Failed to set upstream connection failure callback."
        ));
        return;
    }

    if (!bridge->init()) {
        getLogger().error(i18n::tr(
            "upstream.bridge_player_init_failed",
            "Failed to initialize proxy bridge for player: {0}.",
            bridge->mConnectionRequest.getXboxLiveName()
        ));
        return disconnectClient(
            guid,
            i18n::tr("client.failed_bridge_init", "Failed to initialize proxy bridge"),
            protocol::DisconnectFailReason::Unknown
        );
    }

    if (bridge->mProxyClient.connect(mSettings.upstream_host, mSettings.upstream_port)
        != protocol::ClientNetworkSystem::ConnectionResult::ConnectionAttemptStarted) [[unlikely]] {
        getLogger().info(i18n::tr(
            "upstream.connect_player_failed",
            "Failed to connect to upstream server for player: {0}.",
            bridge->mConnectionRequest.getXboxLiveName()
        ));
        getLogger().info(
            "[{}] Player disconnected: {}, xuid: {}, pfid: {}",
            bridge->mRealAddress.ToString(),
            bridge->mClientInfo.name,
            bridge->mClientInfo.xuid.empty() ? "N/A" : bridge->mClientInfo.xuid,
            bridge->mClientInfo.pfid.empty() ? "N/A" : bridge->mClientInfo.pfid
        );
        disconnectClient(
            guid,
            i18n::tr("client.failed_upstream_connect", "Failed to connect to upstream server"),
            protocol::DisconnectFailReason::CantConnect
        );
        return;
    }

    return handleClient(session, static_cast<const protocol::RequestNetworkSettingsPacket&>(packet));
}

void ProxyPass::onRealClientPacket(
    const RakNet::RakNetGUID&    guid,
    const RakNet::SystemAddress& address,
    const protocol::IPacket&     packet
) {
    auto session = mProxyServer.getSession(guid).lock();
    if (!session) {
        return;
    }

    std::shared_ptr<ProxyBridge> bridge{};
    if (!mBridges.if_contains(guid, [&bridge](auto const& entry) { bridge = entry.second; })) {
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
    default: {
        if (PROXY_PASS_SHOULD_LOG_PACKET(id)) {
            getLogger().info("Server => Proxy => Client | {}", packet);
        }
        bridge.sendPacketToClient(packet);
        break;
    }
    }
}

void ProxyPass::handleServer(ProxyBridge& bridge, const protocol::NetworkSettingsPacket& packet) {
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::NetworkSettings)) {
        getLogger().info("Server => Proxy | {}", packet);
    }
    bridge.mProxyClient.getSession().setCompressed(packet.mCompressionAlgorithm, packet.mCompressionThreshold);
    if (auto status = bridge.mConnectionRequest.selfSign(mProxyServerKeyPair); !status) [[unlikely]] {
        getLogger().error(i18n::tr(
            "upstream.sign_login_token_failed",
            "Failed to sign upstream login token: {0}",
            status.error().message()
        ));
        return disconnectClient(
            bridge.mRealGuid,
            i18n::tr("upstream.sign_login_token_disconnect", "Failed to sign upstream login token"),
            protocol::DisconnectFailReason::BadPacket
        );
    }
    protocol::LoginPacket loginPacket{bridge.mConnectionRequest.toString()};

    bridge.sendPacketToServer(loginPacket, true);
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::Login)) {
        getLogger().info("Proxy => Server | {}", loginPacket);
    }
}

void ProxyPass::handleServer(ProxyBridge& bridge, const protocol::ServerToClientHandshakePacket& packet) {
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ServerToClientHandshake)) {
        getLogger().info("Server => Proxy | {}", packet);
    }
    auto handshakeToken = protocol::HandShakeToken::fromString(packet.mHandshakeWebToken);
    if (!handshakeToken || !handshakeToken->verify()) {
        getLogger().info(
            "[{}] Player disconnected: {}, xuid: {}, pfid: {}",
            bridge.mRealAddress.ToString(),
            bridge.mClientInfo.name,
            bridge.mClientInfo.xuid.empty() ? "N/A" : bridge.mClientInfo.xuid,
            bridge.mClientInfo.pfid.empty() ? "N/A" : bridge.mClientInfo.pfid
        );
        return disconnectClient(
            bridge.mRealGuid,
            i18n::tr("client.invalid_handshake_token", "Invalid handshake token"),
            protocol::DisconnectFailReason::BadPacket
        );
    }

    auto sessionKey = protocol::CryptoManager::computeSessionKey(
        mProxyServerKeyPair.mPrivateKeyPem,
        handshakeToken->getRemotePublicKey(),
        handshakeToken->getSaltBytes()
    );
    if (!sessionKey) {
        getLogger().info(
            "[{}] Player disconnected: {}, xuid: {}, pfid: {}",
            bridge.mRealAddress.ToString(),
            bridge.mClientInfo.name,
            bridge.mClientInfo.xuid.empty() ? "N/A" : bridge.mClientInfo.xuid,
            bridge.mClientInfo.pfid.empty() ? "N/A" : bridge.mClientInfo.pfid
        );
        return disconnectClient(
            bridge.mRealGuid,
            i18n::tr("client.failed_server_session_key", "Failed to compute server session key"),
            protocol::DisconnectFailReason::BadPacket
        );
    }
    bridge.mProxyClient.getSession().setEncrypted(std::move(*sessionKey));
    protocol::ClientToServerHandshakePacket handshakePacket{};
    bridge.sendPacketToServer(handshakePacket, true);
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ClientToServerHandshake)) {
        getLogger().info("Proxy => Server | {}", handshakePacket);
    }
}

void ProxyPass::shutdown() {
    getLogger().info(i18n::tr("proxy.shutdown", "Shutting down proxy server..."));
    for (auto& [_, bridge] : mBridges) {
        if (bridge->mProxyClient.isConnected()) {
            protocol::DisconnectPacket disconnectPacket{};
            disconnectPacket.mReason  = protocol::DisconnectFailReason::Kicked;
            disconnectPacket.mMessage = i18n::tr("client.proxy_shutdown", "Proxy server is shutting down");
            protocol::Session::Buffer buffer{};
            protocol::BinaryStream    stream{buffer};
            disconnectPacket.writeWithHeader(stream);
            bridge->sendPacketToClient(disconnectPacket, true);
        }
    }
    mBridges.clear();
    mProxyServer.stop();
}

void ProxyPass::waitForStop() {
    std::string command{};
    while (true) {
        if (!(std::cin >> command)) {
            break;
        }
        if (command == "stop") {
            shutdown();
            break;
        }
        getLogger().error(i18n::tr(
            "command.unknown",
            "Unknown command: {0}. Please check that the command exists and that you have permission to use it.",
            command
        ));
    }
    getLogger().info(i18n::tr("proxy.stopped", "Proxy server stopped."));
}

} // namespace sculk
