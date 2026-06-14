// Copyright © 2026 SculkCatalystMC. All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ResourcePackManager.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <print>
#include <sculk/jsonc/jsonc.hpp>
#include <sstream>
#include <zlib.h>

namespace sculk {

namespace {
constexpr std::uint32_t LocalChunkSize = 256U * 1024U;

constexpr std::array<std::uint32_t, 64U> Sha256RoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

[[nodiscard]] std::uint32_t readBe32(const std::array<unsigned char, 64U>& block, std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(block[offset]) << 24U) |
        (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
        (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
        static_cast<std::uint32_t>(block[offset + 3U]);
}

[[nodiscard]] std::string bytesToHex(std::string_view value) {
    std::ostringstream stream{};
    stream << std::hex << std::setfill('0') << std::nouppercase;
    for (const auto byte : value) {
        stream << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(byte));
    }
    return stream.str();
}

[[nodiscard]] bool isPackFile(const std::filesystem::path& path) {
    const auto extension = path.extension().string();
    return extension == ".mcpack" || extension == ".zip";
}

struct LocalPackMetadata {
    std::optional<protocol::UUID> id{};
    std::optional<std::string>    version{};
    std::optional<std::string>    encryptionKey{};
    std::optional<std::string>    contentIdentity{};
    std::optional<std::string>    subpackName{};
    std::optional<bool>           hasScripts{};
    std::optional<bool>           isAddonPack{};
    std::optional<std::string>    cdnUrl{};
};

[[nodiscard]] std::uint16_t readLe16(std::string_view data, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(data[offset]) | (static_cast<unsigned char>(data[offset + 1U]) << 8U)
    );
}

[[nodiscard]] std::uint32_t readLe32(std::string_view data, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(
        static_cast<unsigned char>(data[offset]) | (static_cast<unsigned char>(data[offset + 1U]) << 8U) |
        (static_cast<unsigned char>(data[offset + 2U]) << 16U) | (static_cast<unsigned char>(data[offset + 3U]) << 24U)
    );
}

[[nodiscard]] std::optional<std::uint64_t> parseHex64(std::string_view value) {
    std::uint64_t result{};
    const auto*   first = value.data();
    const auto*   last  = value.data() + value.size();
    const auto    parsed = std::from_chars(first, last, result, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<protocol::UUID> parseUuid(std::string_view value) {
    std::string hex{};
    hex.reserve(32U);
    for (const auto character : value) {
        if (character == '-') {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(character))) {
            return std::nullopt;
        }
        hex.push_back(character);
    }
    if (hex.size() != 32U) {
        return std::nullopt;
    }

    auto high = parseHex64(std::string_view{hex}.substr(0U, 16U));
    auto low  = parseHex64(std::string_view{hex}.substr(16U, 16U));
    if (!high || !low) {
        return std::nullopt;
    }
    return protocol::UUID{*high, *low};
}

[[nodiscard]] std::optional<std::string> inflateRaw(std::string_view compressed, std::uint32_t uncompressedSize) {
    std::string result(uncompressedSize, '\0');
    z_stream    stream{};
    stream.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    stream.avail_in  = static_cast<uInt>(compressed.size());
    stream.next_out  = reinterpret_cast<Bytef*>(result.data());
    stream.avail_out = static_cast<uInt>(result.size());

    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return std::nullopt;
    }
    const auto status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (status != Z_STREAM_END || stream.total_out != uncompressedSize) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<std::string> extractZipEntry(std::string_view archive, std::string_view targetName) {
    constexpr std::uint32_t EndOfCentralDirectorySignature = 0x06054b50U;
    constexpr std::uint32_t CentralDirectorySignature      = 0x02014b50U;
    constexpr std::uint32_t LocalFileHeaderSignature       = 0x04034b50U;
    constexpr std::size_t   EndOfCentralDirectorySize      = 22U;

    if (archive.size() < EndOfCentralDirectorySize) {
        return std::nullopt;
    }

    const auto searchBegin = archive.size() > 0xFFFFU + EndOfCentralDirectorySize
        ? archive.size() - (0xFFFFU + EndOfCentralDirectorySize)
        : 0U;
    std::optional<std::size_t> endDirectoryOffset{};
    for (auto offset = archive.size() - EndOfCentralDirectorySize + 1U; offset-- > searchBegin;) {
        if (readLe32(archive, offset) == EndOfCentralDirectorySignature) {
            endDirectoryOffset = offset;
            break;
        }
        if (offset == 0U) {
            break;
        }
    }
    if (!endDirectoryOffset) {
        return std::nullopt;
    }

    const auto centralDirectorySize   = readLe32(archive, *endDirectoryOffset + 12U);
    const auto centralDirectoryOffset = readLe32(archive, *endDirectoryOffset + 16U);
    if (static_cast<std::uint64_t>(centralDirectoryOffset) + centralDirectorySize > archive.size()) {
        return std::nullopt;
    }

    auto offset = static_cast<std::size_t>(centralDirectoryOffset);
    const auto centralDirectoryEnd = offset + centralDirectorySize;
    while (offset + 46U <= centralDirectoryEnd && readLe32(archive, offset) == CentralDirectorySignature) {
        const auto method             = readLe16(archive, offset + 10U);
        const auto compressedSize     = readLe32(archive, offset + 20U);
        const auto uncompressedSize   = readLe32(archive, offset + 24U);
        const auto fileNameLength     = readLe16(archive, offset + 28U);
        const auto extraLength        = readLe16(archive, offset + 30U);
        const auto fileCommentLength  = readLe16(archive, offset + 32U);
        const auto localHeaderOffset  = readLe32(archive, offset + 42U);
        const auto fileNameOffset     = offset + 46U;
        const auto nextDirectoryEntry = fileNameOffset + fileNameLength + extraLength + fileCommentLength;
        if (nextDirectoryEntry > centralDirectoryEnd) {
            return std::nullopt;
        }

        const auto fileName = archive.substr(fileNameOffset, fileNameLength);
        if (fileName == targetName || fileName.ends_with('/' + std::string{targetName})) {
            if (static_cast<std::uint64_t>(localHeaderOffset) + 30U > archive.size() ||
                readLe32(archive, localHeaderOffset) != LocalFileHeaderSignature) {
                return std::nullopt;
            }
            const auto localFileNameLength = readLe16(archive, localHeaderOffset + 26U);
            const auto localExtraLength    = readLe16(archive, localHeaderOffset + 28U);
            const auto dataOffset = static_cast<std::size_t>(localHeaderOffset) + 30U + localFileNameLength + localExtraLength;
            if (static_cast<std::uint64_t>(dataOffset) + compressedSize > archive.size()) {
                return std::nullopt;
            }
            const auto compressedData = archive.substr(dataOffset, compressedSize);
            if (method == 0U) {
                return std::string{compressedData};
            }
            if (method == 8U) {
                return inflateRaw(compressedData, uncompressedSize);
            }
            return std::nullopt;
        }

        offset = nextDirectoryEntry;
    }
    return std::nullopt;
}

[[nodiscard]] std::string versionArrayToString(const jsonc::ordered_jsonc& json) {
    if (json.is_string()) {
        return json.get<std::string>();
    }
    if (!json.is_array() || json.size() == 0U) {
        return "1.0.0";
    }

    std::string result{};
    for (std::size_t index = 0U; index < json.size(); ++index) {
        if (index != 0U) {
            result += '.';
        }
        result += std::to_string(json[index].get<int>());
    }
    return result;
}

[[nodiscard]] LocalPackMetadata metadataFromManifest(std::string_view manifestContent) {
    LocalPackMetadata metadata{};
    const auto        manifest = jsonc::ordered_jsonc::parse(manifestContent, false, true);
    if (!manifest.contains("header")) {
        return metadata;
    }

    const auto& header = manifest["header"];
    if (header.contains("uuid")) {
        metadata.id = parseUuid(header["uuid"].get<std::string>());
    }
    if (header.contains("version")) {
        metadata.version = versionArrayToString(header["version"]);
    }
    return metadata;
}
} // namespace

std::size_t ResourcePackManager::ChunkKeyHash::operator()(const ChunkKey& key) const noexcept {
    auto seed = std::hash<std::string>{}(key.resourceName);
    seed ^= std::hash<std::uint32_t>{}(key.chunkIndex) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    return seed;
}

bool ResourcePackManager::initialize() {
    std::error_code error{};
    std::filesystem::create_directories(mClientPacksDirectory, error);
    if (error) {
        std::println(
            "[ProxyPass] Failed to create ResourcePackManager directory '{}': {}",
            mClientPacksDirectory.string(),
            error.message()
        );
        return false;
    }

    const auto loaded = loadLocalClientPacks();
    writeConfig();
    return loaded;
}

void ResourcePackManager::captureUpstream(Session& session, const protocol::ResourcePacksInfoPacket& packet) {
    session.upstreamInfoReceived = true;
    mUpstream.info               = packet;
    if (!mClientOverrideEnabled) {
        mClient.info = packet;
    }
    writeConfig();
}

void ResourcePackManager::captureUpstream(Session& session, const protocol::ResourcePackStackPacket& packet) {
    session.upstreamStackReceived = true;
    mUpstream.stack               = packet;
    if (!mClientOverrideEnabled) {
        mClient.stack = packet;
    }
    writeConfig();
}

void ResourcePackManager::captureUpstream(const protocol::ResourcePackDataInfoPacket& packet) {
    mUpstream.dataInfoByName[packet.mResourceName] = packet;
    if (!mClientOverrideEnabled) {
        mClient.dataInfoByName[packet.mResourceName] = packet;
    }
    writeConfig();
}

void ResourcePackManager::captureUpstream(const protocol::ResourcePackChunkDataPacket& packet) {
    mUpstream.chunks[ChunkKey{packet.mResourceName, packet.mChunkIndex}] = packet;
    if (!mClientOverrideEnabled) {
        mClient.chunks[ChunkKey{packet.mResourceName, packet.mChunkIndex}] = packet;
    }
}

bool ResourcePackManager::hasClientOverride() const noexcept { return mClientOverrideEnabled; }

std::size_t ResourcePackManager::clientResourcePackCount() const noexcept { return mClient.info.mResourcePacks.size(); }

bool ResourcePackManager::clientResourcePackRequired() const noexcept { return mClient.info.mResourcePackRequired; }

const protocol::ResourcePacksInfoPacket& ResourcePackManager::clientInfoPacket() const noexcept { return mClient.info; }

const protocol::ResourcePackStackPacket& ResourcePackManager::clientStackPacket() const noexcept {
    return mClient.stack;
}

std::optional<protocol::ResourcePackDataInfoPacket>
ResourcePackManager::findClientDataInfo(std::string_view resourceName) const {
    auto found = mClient.dataInfoByName.find(std::string{resourceName});
    if (found == mClient.dataInfoByName.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<protocol::ResourcePackChunkDataPacket>
ResourcePackManager::findClientChunk(const protocol::ResourcePackChunkRequestPacket& packet) const {
    auto found = mClient.chunks.find(ChunkKey{packet.mResourceName, packet.mChunkIndex});
    if (found == mClient.chunks.end()) {
        return std::nullopt;
    }
    return found->second;
}

protocol::ResourcePackClientResponsePacket ResourcePackManager::makeUpstreamResponse(ClientResponse response) const {
    protocol::ResourcePackClientResponsePacket packet{};
    packet.mResponse = static_cast<std::uint8_t>(response);
    packet.mPackIds  = packIds(mUpstream.info);
    return packet;
}

protocol::ResourcePackClientResponsePacket ResourcePackManager::makeClientResponse(ClientResponse response) const {
    protocol::ResourcePackClientResponsePacket packet{};
    packet.mResponse = static_cast<std::uint8_t>(response);
    packet.mPackIds  = packIds(mClient.info);
    return packet;
}

void ResourcePackManager::noteClientResponse(
    Session&                                      session,
    const protocol::ResourcePackClientResponsePacket& packet
) const {
    switch (static_cast<ClientResponse>(packet.mResponse)) {
    case ClientResponse::Refused:
        session.clientRefused = true;
        break;
    case ClientResponse::SendPacks:
        session.clientRequestedTransfer = true;
        break;
    case ClientResponse::HaveAllPacks:
        session.clientHasAllPacks = true;
        break;
    case ClientResponse::Completed:
        session.clientCompleted = true;
        break;
    }
}

bool ResourcePackManager::loadLocalClientPacks() {
    mClientOverrideEnabled = false;
    mClient                = {};

    std::vector<std::filesystem::path> packFiles{};
    std::error_code                    error{};
    for (const auto& entry : std::filesystem::directory_iterator{mClientPacksDirectory, error}) {
        if (!entry.is_regular_file() || !isPackFile(entry.path())) {
            continue;
        }
        packFiles.push_back(entry.path());
    }
    if (error) {
        std::println("[ProxyPass] Failed to scan local client resource packs: {}", error.message());
        return false;
    }

    std::ranges::sort(packFiles);
    for (const auto& path : packFiles) {
        if (!loadPackFile(path)) {
            return false;
        }
    }

    mClientOverrideEnabled                  = !mClient.info.mResourcePacks.empty();
    mClient.info.mResourcePackRequired      = mClientOverrideEnabled;
    mClient.stack.mTexturePackRequired      = mClientOverrideEnabled;
    mClient.stack.mBaseGameVersion          = "*";
    mClient.stack.mIncludeEditorPacks       = false;
    mClient.info.mHasAddonPacks             = false;
    mClient.info.mHasScripts                = false;
    mClient.info.mWorldTemplateVersion      = "";
    mClient.info.mWorldTemplateId           = {};
    mClient.info.mIsVibrantVisualsForceDisabled = false;
    return true;
}

bool ResourcePackManager::loadPackFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        std::println("[ProxyPass] Failed to open local resource pack '{}'.", path.string());
        return false;
    }

    std::string content{std::istreambuf_iterator<char>(file), {}};
    auto metadata = extractZipEntry(content, "manifest.json")
        .transform([](const std::string& manifest) { return metadataFromManifest(manifest); })
        .value_or(LocalPackMetadata{});

    auto metadataPath = path;
    metadataPath.replace_extension(".jsonc");
    if (std::ifstream metadataFile{metadataPath}; metadataFile) {
        std::string metadataContent{std::istreambuf_iterator<char>(metadataFile), {}};
        auto        json = jsonc::ordered_jsonc::parse(metadataContent, false, true);

        if (json.contains("pack_id")) {
            metadata.id = uuidFromString(json["pack_id"].get<std::string>());
        } else if (json.contains("id")) {
            metadata.id = uuidFromString(json["id"].get<std::string>());
        }
        if (json.contains("version")) {
            metadata.version = json["version"].get<std::string>();
        }
        if (json.contains("encryption_key")) {
            metadata.encryptionKey = json["encryption_key"].get<std::string>();
        }
        if (json.contains("content_identity")) {
            metadata.contentIdentity = json["content_identity"].get<std::string>();
        }
        if (json.contains("subpack_name")) {
            metadata.subpackName = json["subpack_name"].get<std::string>();
        }
        if (json.contains("has_scripts")) {
            metadata.hasScripts = json["has_scripts"].get<bool>();
        }
        if (json.contains("is_addon_pack")) {
            metadata.isAddonPack = json["is_addon_pack"].get<bool>();
        }
        if (json.contains("cdn_url")) {
            metadata.cdnUrl = json["cdn_url"].get<std::string>();
        }
    }

    const auto  id           = metadata.id.value_or(uuidFromStableName(path.filename().generic_string()));
    const auto  version      = metadata.version.value_or("1.0.0");
    const auto  resourceName = uuidToString(id) + '_' + version;
    const auto  chunkCount   = static_cast<std::uint32_t>((content.size() + LocalChunkSize - 1U) / LocalChunkSize);

    protocol::PackInfoData pack{};
    pack.mPackId          = id;
    pack.mPackVersion     = version;
    pack.mPackSize        = content.size();
    pack.mContentKey      = metadata.encryptionKey.value_or("");
    pack.mContentIdentity = metadata.contentIdentity.value_or(uuidToString(id));
    pack.mSubpackName     = metadata.subpackName.value_or("");
    pack.mHasScripts      = metadata.hasScripts.value_or(false);
    pack.mIsAddonPack     = metadata.isAddonPack.value_or(false);
    pack.mCDNUrl          = metadata.cdnUrl.value_or("");
    mClient.info.mResourcePacks.push_back(pack);

    protocol::ResourcePackStackPacket::PackInfo stackInfo{};
    stackInfo.mId      = uuidToString(id);
    stackInfo.mVersion = version;
    mClient.stack.mTexturePackList.push_back(std::move(stackInfo));

    protocol::ResourcePackDataInfoPacket dataInfo{};
    dataInfo.mResourceName  = resourceName;
    dataInfo.mChunkSize     = LocalChunkSize;
    dataInfo.mChunkIndex    = chunkCount;
    dataInfo.mFileSize      = content.size();
    dataInfo.mFileHash      = sha256Binary(content);
    dataInfo.mIsPremiumPack = false;
    dataInfo.mPackType      = protocol::ResourcePackDataInfoPacket::PackType::Resources;
    mClient.dataInfoByName[dataInfo.mResourceName] = dataInfo;

    for (std::uint32_t index = 0; index < chunkCount; ++index) {
        const auto offset = static_cast<std::uint64_t>(index) * LocalChunkSize;
        const auto size   = std::min<std::size_t>(LocalChunkSize, content.size() - static_cast<std::size_t>(offset));

        protocol::ResourcePackChunkDataPacket chunk{};
        chunk.mResourceName = resourceName;
        chunk.mChunkIndex   = index;
        chunk.mBytesOffset  = offset;
        chunk.mChunkData    = content.substr(static_cast<std::size_t>(offset), size);
        mClient.chunks[ChunkKey{chunk.mResourceName, chunk.mChunkIndex}] = std::move(chunk);
    }

    return true;
}

void ResourcePackManager::writeConfig() const {
    using sculk::jsonc::ordered_jsonc;

    auto doc = ordered_jsonc::object();
    doc["install_directory"]     = mRootDirectory.generic_string();
    doc["client_pack_directory"] = mClientPacksDirectory.generic_string();
    doc["client_override_enabled"] = mClientOverrideEnabled;
    doc["notes"] = "Put .mcpack or .zip files into client_pack_directory and restart ProxyPass to make client packs independent from upstream packs.";

    auto clientPacks = ordered_jsonc::array();
    for (const auto& pack : mClient.info.mResourcePacks) {
        auto item                 = ordered_jsonc::object();
        item["id"]                = uuidToString(pack.mPackId);
        item["version"]           = pack.mPackVersion;
        item["size"]              = pack.mPackSize;
        item["encryption_key"]    = pack.mContentKey;
        item["content_identity"]  = pack.mContentIdentity;
        item["subpack_name"]      = pack.mSubpackName;
        item["has_scripts"]       = pack.mHasScripts;
        item["is_addon_pack"]     = pack.mIsAddonPack;
        item["cdn_url"]           = pack.mCDNUrl;
        clientPacks.push_back(std::move(item));
    }
    doc["client_resource_packs"] = std::move(clientPacks);

    auto upstreamPacks = ordered_jsonc::array();
    for (const auto& pack : mUpstream.info.mResourcePacks) {
        auto item                 = ordered_jsonc::object();
        item["id"]                = uuidToString(pack.mPackId);
        item["version"]           = pack.mPackVersion;
        item["size"]              = pack.mPackSize;
        item["encryption_key"]    = pack.mContentKey;
        item["content_identity"]  = pack.mContentIdentity;
        item["subpack_name"]      = pack.mSubpackName;
        item["has_scripts"]       = pack.mHasScripts;
        item["is_addon_pack"]     = pack.mIsAddonPack;
        item["cdn_url"]           = pack.mCDNUrl;
        upstreamPacks.push_back(std::move(item));
    }
    doc["upstream_resource_packs"] = std::move(upstreamPacks);

    auto cachedFiles = ordered_jsonc::array();
    std::vector<std::string> resourceNames{};
    resourceNames.reserve(mUpstream.dataInfoByName.size());
    for (const auto& [resourceName, _] : mUpstream.dataInfoByName) {
        resourceNames.push_back(resourceName);
    }
    std::ranges::sort(resourceNames);
    for (const auto& resourceName : resourceNames) {
        const auto& info        = mUpstream.dataInfoByName.at(resourceName);
        auto        item        = ordered_jsonc::object();
        item["name"]            = info.mResourceName;
        item["hash"]            = bytesToHex(info.mFileHash);
        item["chunk_size"]      = info.mChunkSize;
        item["chunk_count"]     = info.mChunkIndex;
        item["file_size"]       = info.mFileSize;
        cachedFiles.push_back(std::move(item));
    }
    doc["cached_upstream_resource_files"] = std::move(cachedFiles);

    std::ofstream file{mConfigPath, std::ios::trunc};
    if (file) {
        file << doc.dump(4, false, false, true) << '\n';
    }
}

protocol::UUID ResourcePackManager::uuidFromStableName(std::string_view value) {
    const auto first  = std::hash<std::string_view>{}(value);
    const auto second = std::hash<std::string>{}("ProxyPass:" + std::string{value});
    return protocol::UUID{static_cast<std::uint64_t>(first), static_cast<std::uint64_t>(second)};
}

std::optional<protocol::UUID> ResourcePackManager::uuidFromString(std::string_view value) {
    return parseUuid(value);
}

std::string ResourcePackManager::uuidToString(const protocol::UUID& uuid) {
    std::ostringstream stream{};
    stream << std::hex << std::setfill('0') << std::nouppercase;
    const auto high = uuid.mMostSignificantBits;
    const auto low  = uuid.mLeastSignificantBits;
    stream << std::setw(8) << static_cast<std::uint32_t>(high >> 32U) << '-';
    stream << std::setw(4) << static_cast<std::uint16_t>(high >> 16U) << '-';
    stream << std::setw(4) << static_cast<std::uint16_t>(high) << '-';
    stream << std::setw(4) << static_cast<std::uint16_t>(low >> 48U) << '-';
    stream << std::setw(12) << (low & 0x0000FFFFFFFFFFFFULL);
    return stream.str();
}

std::string ResourcePackManager::packIdWithVersion(const protocol::PackInfoData& pack) {
    return uuidToString(pack.mPackId) + '_' + pack.mPackVersion;
}

std::vector<std::string> ResourcePackManager::packIds(const protocol::ResourcePacksInfoPacket& packet) {
    std::vector<std::string> result{};
    result.reserve(packet.mResourcePacks.size());
    for (const auto& pack : packet.mResourcePacks) {
        result.push_back(packIdWithVersion(pack));
    }
    return result;
}

std::string ResourcePackManager::sha256Binary(std::string_view content) {
    std::array<std::uint32_t, 8U> state{
        0x6a09e667U,
        0xbb67ae85U,
        0x3c6ef372U,
        0xa54ff53aU,
        0x510e527fU,
        0x9b05688cU,
        0x1f83d9abU,
        0x5be0cd19U,
    };

    auto processBlock = [&state](const std::array<unsigned char, 64U>& block) {
        std::array<std::uint32_t, 64U> words{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            words[index] = readBe32(block, index * 4U);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const auto s0 = std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
            const auto s1 = std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
            words[index]  = words[index - 16U] + s0 + words[index - 7U] + s1;
        }

        auto a = state[0U];
        auto b = state[1U];
        auto c = state[2U];
        auto d = state[3U];
        auto e = state[4U];
        auto f = state[5U];
        auto g = state[6U];
        auto h = state[7U];

        for (std::size_t index = 0U; index < words.size(); ++index) {
            const auto s1    = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto ch    = (e & f) ^ ((~e) & g);
            const auto temp1 = h + s1 + ch + Sha256RoundConstants[index] + words[index];
            const auto s0    = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto maj   = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0U] += a;
        state[1U] += b;
        state[2U] += c;
        state[3U] += d;
        state[4U] += e;
        state[5U] += f;
        state[6U] += g;
        state[7U] += h;
    };

    std::size_t offset{};
    while (offset + 64U <= content.size()) {
        std::array<unsigned char, 64U> block{};
        for (std::size_t index = 0U; index < block.size(); ++index) {
            block[index] = static_cast<unsigned char>(content[offset + index]);
        }
        processBlock(block);
        offset += block.size();
    }

    std::array<unsigned char, 64U> finalBlock{};
    const auto remaining = content.size() - offset;
    for (std::size_t index = 0U; index < remaining; ++index) {
        finalBlock[index] = static_cast<unsigned char>(content[offset + index]);
    }
    finalBlock[remaining] = 0x80U;

    if (remaining >= 56U) {
        processBlock(finalBlock);
        finalBlock = {};
    }

    const auto bitLength = static_cast<std::uint64_t>(content.size()) * 8U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        finalBlock[63U - index] = static_cast<unsigned char>((bitLength >> (index * 8U)) & 0xFFU);
    }
    processBlock(finalBlock);

    std::string digest(32U, '\0');
    for (std::size_t index = 0U; index < state.size(); ++index) {
        digest[index * 4U]      = static_cast<char>((state[index] >> 24U) & 0xFFU);
        digest[index * 4U + 1U] = static_cast<char>((state[index] >> 16U) & 0xFFU);
        digest[index * 4U + 2U] = static_cast<char>((state[index] >> 8U) & 0xFFU);
        digest[index * 4U + 3U] = static_cast<char>(state[index] & 0xFFU);
    }
    return digest;
}

std::string ResourcePackManager::stableHashHex(std::string_view content) {
    std::array<std::uint64_t, 4> parts{
        std::hash<std::string_view>{}(content),
        std::hash<std::string>{}("ProxyPass:1:" + std::string{content.substr(0, std::min<std::size_t>(content.size(), 4096U))}),
        std::hash<std::string>{}("ProxyPass:2:" + std::to_string(content.size())),
        std::hash<std::string>{}("ProxyPass:3:" + std::string{content.substr(content.size() / 2U, std::min<std::size_t>(content.size() - content.size() / 2U, 4096U))}),
    };
    std::ostringstream stream{};
    stream << std::hex << std::setfill('0');
    for (const auto part : parts) {
        stream << std::setw(16) << part;
    }
    return stream.str();
}

} // namespace sculk
