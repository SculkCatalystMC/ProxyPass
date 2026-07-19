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
#include "Logger.hpp"
#include <atomic>
#include <climits>
#include <cstdio>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <print>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <sculk/protocol/codec/MinecraftPackets.hpp>
#include <sculk/protocol/codec/packet/ClientToServerHandshakePacket.hpp>
#include <sculk/protocol/codec/packet/DisconnectPacket.hpp>
#include <sculk/protocol/codec/packet/PlayStatusPacket.hpp>
#include <sculk/protocol/connection/HandShakeToken.hpp>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace sculk {

namespace {
    std::atomic<ProxyPass*> gProxyPassInstance{nullptr};

    void proxyPassSignalHandler(int /*signum*/) {
        if (auto* instance = gProxyPassInstance.load(std::memory_order_acquire)) {
            instance->requestStop();
        }
    }
}

void ProxyPass::setWorkingDirectory() {
    std::filesystem::path exePath;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) != 0) {
        exePath = buffer;
    }
#else
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len != -1) {
        buffer[len] = '\0';
        exePath = buffer;
    }
#endif
    if (exePath.empty()) {
        return;
    }

    auto dir = exePath.parent_path();
    for (int i = 0; i < 4 && !dir.empty(); ++i) {
        if (std::filesystem::exists(dir / "proxy_settings.jsonc")) {
            std::filesystem::current_path(dir);
            return;
        }
        dir = dir.parent_path();
    }
    std::filesystem::current_path(exePath.parent_path());
}

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
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::print("\033]0;ProxyPass\007");
}

#define PROXY_PASS_SHOULD_LOG_PACKET(ID)                                                                               \
    (mSettings.packets_logger->black_list_mode && !mSettings.packets_logger->packet_ids->contains(ID))                 \
        || (!mSettings.packets_logger->black_list_mode && mSettings.packets_logger->packet_ids->contains(ID))

ProxyPass::ProxyPass() : mLogger("ProxyPass") {}

ProxyPass::~ProxyPass() { mSettings.save(); }

Logger& ProxyPass::getLogger() { return mLogger; }

bool ProxyPass::start() {
    getLogger().info("Starting proxy server...");
    std::chrono::high_resolution_clock::time_point startTime = std::chrono::high_resolution_clock::now();

    getLogger()
        .info("Version: {}(ProtocolVersion {})", protocol::getMinecraftVersion(), protocol::getProtocolVersion());

    mSettings.load();
    mThreadPool = std::make_unique<protocol::thread::ThreadPool>(
        *mSettings.max_threads == 0 ? std::thread::hardware_concurrency() : *mSettings.max_threads
    );
    mProxyServer = std::make_unique<protocol::ServerNetworkSystem>(*mThreadPool);

    auto serverKeyPair = protocol::ssl::randomES384KeyPair();
    if (!serverKeyPair) {
        getLogger().fatal("Failed to generate server key pair: {}", serverKeyPair.error().message());
        return false;
    }
    mProxyServerKeyPair = *serverKeyPair;

    if (!mProxyServer->setOnDisconnected([this](const RakNet::RakNetGUID& guid, const RakNet::SystemAddress&) noexcept {
            onClientDisconnected(guid);
        })) [[unlikely]] {
        getLogger().fatal("Failed to set proxy server disconnect callback.");
        return false;
    }

    if (!mProxyServer->setOnPacketReceive([this](
                                              const RakNet::RakNetGUID&            guid,
                                              const RakNet::SystemAddress&         address,
                                              std::unique_ptr<protocol::IPacket>&& packet
                                          ) noexcept { onRealClientPacket(guid, address, *packet); })) [[unlikely]] {
        getLogger().fatal("Failed to set proxy server packet receive callback.");
        return false;
    }

    if (mSettings.packets_logger->log_parse_error) {
        if (!mProxyServer->setOnPacketParseFailed([this](
                                                      const RakNet::RakNetGUID&,
                                                      const RakNet::SystemAddress&,
                                                      protocol::MinecraftPacketIds id,
                                                      std::string_view             message
                                                  ) noexcept {
                if (id != static_cast<protocol::MinecraftPacketIds>(-1)) [[likely]] {
                    getLogger().error(
                        "Failed to parse {}Packet({}): {}",
                        magic_enum::enum_name(id),
                        std::to_underlying(id),
                        message
                    );
                } else {
                    getLogger().error("Received invalid packet: {}", message);
                }
            })) [[unlikely]] {
            getLogger().fatal("Failed to set proxy server packet parse failure callback.");
            return false;
        }
    }

    mProxyServer->setMotd(mSettings.motd);
    auto startResult = mProxyServer->start(mSettings.proxy_port, mSettings.proxy_port_v6, mSettings.max_players);
    if (startResult != protocol::NetworkStartResult::Success) [[unlikely]] {
        getLogger().fatal("Failed to start proxy server.");
        if (startResult == protocol::NetworkStartResult::SocketPortAlreadyInUse) {
            getLogger().fatal(
                "Port [{}] may be in use by another process. Free up port and re-run program or adjust "
                "proxy_settings.jsonc file to use alternate ports for proxy server",
                mSettings.proxy_port
            );
            getLogger().fatal(
                "Port [{}] may be in use by another process. Free up port and re-run program or adjust "
                "proxy_settings.jsonc file to use alternate ports for proxy server",
                mSettings.proxy_port_v6
            );
        }
        getLogger().fatal(
            "Exiting program with error code {}.",
            static_cast<std::underlying_type_t<protocol::NetworkStartResult>>(startResult)
        );
        return false;
    }

    getLogger().info("IPv4 supported, port: {}", mSettings.proxy_port);
    getLogger().info("IPv6 supported, port: {}", mSettings.proxy_port_v6);

    if (mSettings.online_mode) {
        getLogger().info("Waiting for Minecraft services...");
        if (auto status = mAuthManager.initMojangPublicKeyFromNetwork(); !status) {
            getLogger().fatal("Failed to connect to Minecraft services: {}", status.error().message());
            return false;
        }
    } else {
        mAuthManager.initMojangPublicKeyFromCachedKeys();
    }

    std::chrono::high_resolution_clock::time_point endTime = std::chrono::high_resolution_clock::now();

    getLogger().info(
        "Proxy server started in {:.2f} seconds.",
        std::chrono::duration<double>(endTime - startTime).count()
    );
    return true;
}

void ProxyPass::disconnectClient(const RakNet::RakNetGUID& guid, protocol::PlayStatus status) {
    protocol::PlayStatusPacket playStatusPacket{};
    playStatusPacket.mStatus = status;
    mProxyServer->disconnectClient(guid);

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
    if (auto session = mProxyServer->getSession(guid).lock()) {
        protocol::Session::Buffer buffer{};
        protocol::BinaryStream    stream{buffer};
        disconnectPacket.writeWithHeader(stream);
        session->sendPacketImmediately(std::move(buffer));
    }
    mProxyServer->disconnectClient(guid);

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
            "Failed to compute client session token",
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
    auto [bridgePtr, inserted] =
        mBridges.try_emplace_p(guid, std::make_shared<ProxyBridge>(guid, address, session, *mThreadPool));
    bridge = bridgePtr->second;

    std::weak_ptr<ProxyBridge> weakBridge = bridge;

    if (!bridge->mProxyClient.setOnPacketReceive([this,
                                                  weakBridge](std::unique_ptr<protocol::IPacket>&& packet) noexcept {
            auto currentBridge = weakBridge.lock();
            if (!currentBridge) {
                return;
            }
            processServerPacket(*currentBridge, *packet);
        })) [[unlikely]] {
        getLogger().error("Failed to set upstream packet receive callback.");
        return;
    }

    if (mSettings.packets_logger->log_parse_error) {
        if (!bridge->mProxyClient.setOnPacketParseFailed(
                [this](protocol::MinecraftPacketIds id, std::string_view message) noexcept {
                    if (id != static_cast<protocol::MinecraftPacketIds>(-1)) [[likely]] {
                        getLogger().error(
                            "Failed to parse {}Packet({}): {}",
                            magic_enum::enum_name(id),
                            std::to_underlying(id),
                            message
                        );
                    } else {
                        getLogger().error("Received invalid packet: {}", message);
                    }
                }
            )) [[unlikely]] {
            getLogger().error("Failed to set upstream packet parse failure callback.");
            return;
        }
    }

    if (!bridge->mProxyClient.setOnConnected([this, weakBridge]() noexcept {
            auto currentBridge = weakBridge.lock();
            if (!currentBridge) {
                return;
            }

            getLogger().info(
                "Upstream connection to {}:{} established for player: {}.",
                mSettings.upstream_host,
                mSettings.upstream_port,
                currentBridge->mConnectionRequest.getXboxLiveName()
            );

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
        getLogger().error("Failed to set upstream connected callback.");
        return;
    }

    if (!bridge->mProxyClient.setOnConnectionFailed([this, weakBridge]() noexcept {
            auto currentBridge = weakBridge.lock();
            if (!currentBridge) {
                return;
            }

            getLogger().info(
                "Upstream connection to {}:{} failed for player: {}.",
                mSettings.upstream_host,
                mSettings.upstream_port,
                currentBridge->mConnectionRequest.getXboxLiveName()
            );
            getLogger().info(
                "[{}] Player disconnected: {}, xuid: {}, pfid: {}",
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
        })) [[unlikely]] {
        getLogger().error("Failed to set upstream connection failure callback.");
        return;
    }

    if (!bridge->init()) {
        getLogger().error(
            "Failed to initialize proxy bridge for player: {}.",
            bridge->mConnectionRequest.getXboxLiveName()
        );
        return disconnectClient(guid, "Failed to initialize proxy bridge", protocol::DisconnectFailReason::Unknown);
    }

    getLogger().info(
        "Connecting to upstream server at {}:{} for player: {}.",
        mSettings.upstream_host,
        mSettings.upstream_port,
        bridge->mConnectionRequest.getXboxLiveName()
    );
    auto connectResult = bridge->mProxyClient.connect(mSettings.upstream_host, mSettings.upstream_port);
    if (connectResult != protocol::ClientNetworkSystem::ConnectionResult::ConnectionAttemptStarted) [[unlikely]] {
        getLogger().info(
            "Failed to start upstream connection (result: {}) for player: {}.",
            std::to_underlying(connectResult),
            bridge->mConnectionRequest.getXboxLiveName()
        );
        getLogger().info(
            "[{}] Player disconnected: {}, xuid: {}, pfid: {}",
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
    auto session = mProxyServer->getSession(guid).lock();
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
    auto clientSession = bridge.mProxyClient.getSession().lock();
    if (!clientSession) {
        return;
    }
    clientSession->setCompressed(packet.mCompressionAlgorithm, packet.mCompressionThreshold);
    if (auto status = bridge.mConnectionRequest.selfSign(mProxyServerKeyPair); !status) [[unlikely]] {
        getLogger().error("Failed to sign upstream login token: {}", status.error().message());
        return disconnectClient(
            bridge.mRealGuid,
            "Failed to sign upstream login token",
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
    auto clientSession = bridge.mProxyClient.getSession().lock();
    if (!clientSession) {
        return;
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
        return disconnectClient(bridge.mRealGuid, "Invalid handshake token", protocol::DisconnectFailReason::BadPacket);
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
            "Failed to compute server session key",
            protocol::DisconnectFailReason::BadPacket
        );
    }
    clientSession->setEncrypted(std::move(*sessionKey));
    protocol::ClientToServerHandshakePacket handshakePacket{};
    bridge.sendPacketToServer(handshakePacket, true);
    if (PROXY_PASS_SHOULD_LOG_PACKET(protocol::MinecraftPacketIds::ClientToServerHandshake)) {
        getLogger().info("Proxy => Server | {}", handshakePacket);
    }
}

void ProxyPass::shutdown() {
    getLogger().info("Shutting down proxy server...");
    for (auto& [_, bridge] : mBridges) {
        if (bridge->mProxyClient.isConnected()) {
            protocol::DisconnectPacket disconnectPacket{};
            disconnectPacket.mReason  = protocol::DisconnectFailReason::Kicked;
            disconnectPacket.mMessage = "Proxy server is shutting down";
            protocol::Session::Buffer buffer{};
            protocol::BinaryStream    stream{buffer};
            disconnectPacket.writeWithHeader(stream);
            bridge->sendPacketToClient(disconnectPacket, true);
        }
    }
    mBridges.clear();
    mProxyServer->stop();
    mProxyServer.reset();
    mThreadPool.reset();
}

void ProxyPass::requestStop() {
    if (!mShouldStop.exchange(true)) {
        std::lock_guard<std::mutex> lock(mStopMutex);
        mStopCv.notify_all();
    }
}

void ProxyPass::waitForStop() {
    gProxyPassInstance.store(this, std::memory_order_release);
    std::signal(SIGINT, proxyPassSignalHandler);
    std::signal(SIGTERM, proxyPassSignalHandler);

    std::thread inputThread([this]() {
        std::string command{};
        while (std::cin >> command) {
            if (command == "stop") {
                requestStop();
                break;
            }
            getLogger().error(
                "Unknown command: {}. Please check that the command exists and that you have permission to use it.",
                command
            );
        }
    });
    inputThread.detach();

    {
        std::unique_lock<std::mutex> lock(mStopMutex);
        mStopCv.wait(lock, [this] { return mShouldStop.load(); });
    }

    shutdown();
    getLogger().info("Proxy server stopped.");
}

} // namespace sculk
