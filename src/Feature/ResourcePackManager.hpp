// Copyright © 2026 SculkCatalystMC. All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <sculk/protocol/codec/actor/player/UUID.hpp>
#include <sculk/protocol/codec/packet/ResourcePackChunkDataPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackChunkRequestPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackClientResponsePacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackDataInfoPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePackStackPacket.hpp>
#include <sculk/protocol/codec/packet/ResourcePacksInfoPacket.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace sculk {

class ResourcePackManager {
public:
    struct Session {
        bool upstreamInfoReceived{};
        bool upstreamStackReceived{};
        bool upstreamHaveAllPacksSent{};
        bool upstreamCompletedSent{};
        bool clientInfoSent{};
        bool clientRequestedTransfer{};
        bool clientHasAllPacks{};
        bool clientCompleted{};
        bool clientRefused{};
        bool clientLoginSuccessSent{};
        std::vector<std::string> pendingClientPacks{};
        std::optional<protocol::ResourcePackDataInfoPacket> sendingClientPack{};
    };

    enum class ClientResponse : std::uint8_t {
        Refused      = 1,
        SendPacks   = 2,
        HaveAllPacks = 3,
        Completed    = 4,
    };

private:
    struct ChunkKey {
        std::string   resourceName;
        std::uint32_t chunkIndex{};

        bool operator==(const ChunkKey& other) const noexcept {
            return chunkIndex == other.chunkIndex && resourceName == other.resourceName;
        }
    };

    struct ChunkKeyHash {
        std::size_t operator()(const ChunkKey& key) const noexcept;
    };

    struct PackStore {
        protocol::ResourcePacksInfoPacket                                      info{};
        protocol::ResourcePackStackPacket                                      stack{};
        std::unordered_map<std::string, protocol::ResourcePackDataInfoPacket> dataInfoByName{};
        std::unordered_map<ChunkKey, protocol::ResourcePackChunkDataPacket, ChunkKeyHash> chunks{};
    };

    std::filesystem::path mRootDirectory{"./resource_packs"};
    std::filesystem::path mClientPacksDirectory{"./resource_packs/client"};
    std::filesystem::path mConfigPath{"./resource_packs/resource_packs.jsonc"};
    PackStore             mUpstream{};
    PackStore             mClient{};
    bool                  mClientOverrideEnabled{};

public:
    ResourcePackManager() = default;

    bool initialize();

    void captureUpstream(Session& session, const protocol::ResourcePacksInfoPacket& packet);
    void captureUpstream(Session& session, const protocol::ResourcePackStackPacket& packet);
    void captureUpstream(const protocol::ResourcePackDataInfoPacket& packet);
    void captureUpstream(const protocol::ResourcePackChunkDataPacket& packet);

    [[nodiscard]] bool hasClientOverride() const noexcept;
    [[nodiscard]] std::size_t clientResourcePackCount() const noexcept;
    [[nodiscard]] bool clientResourcePackRequired() const noexcept;
    [[nodiscard]] const protocol::ResourcePacksInfoPacket& clientInfoPacket() const noexcept;
    [[nodiscard]] const protocol::ResourcePackStackPacket& clientStackPacket() const noexcept;

    [[nodiscard]] std::optional<protocol::ResourcePackDataInfoPacket>
    findClientDataInfo(std::string_view resourceName) const;

    [[nodiscard]] std::optional<protocol::ResourcePackChunkDataPacket>
    findClientChunk(const protocol::ResourcePackChunkRequestPacket& packet) const;

    [[nodiscard]] protocol::ResourcePackClientResponsePacket makeUpstreamResponse(ClientResponse response) const;
    [[nodiscard]] protocol::ResourcePackClientResponsePacket makeClientResponse(ClientResponse response) const;

    void noteClientResponse(Session& session, const protocol::ResourcePackClientResponsePacket& packet) const;

private:
    bool loadLocalClientPacks();
    bool loadPackFile(const std::filesystem::path& path);
    void writeConfig() const;

    [[nodiscard]] static protocol::UUID uuidFromStableName(std::string_view value);
    [[nodiscard]] static std::optional<protocol::UUID> uuidFromString(std::string_view value);
    [[nodiscard]] static std::string uuidToString(const protocol::UUID& uuid);
    [[nodiscard]] static std::string packIdWithVersion(const protocol::PackInfoData& pack);
    [[nodiscard]] static std::vector<std::string> packIds(const protocol::ResourcePacksInfoPacket& packet);
    [[nodiscard]] static std::string sha256Binary(std::string_view content);
    [[nodiscard]] static std::string stableHashHex(std::string_view content);
};

} // namespace sculk
