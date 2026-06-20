// Copyright © 2026 SculkCatalystMC. All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ProxyBridge.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <sculk/protocol/codec/packet/AvailableCommandsPacket.hpp>
#include <sculk/protocol/codec/packet/CommandOutputPacket.hpp>
#include <sculk/protocol/codec/packet/CommandRequestPacket.hpp>
#include <sculk/protocol/codec/packet/PlayerListPacket.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sculk {

struct CommandFlag {
    using Value = std::uint16_t;

    constexpr static Value None                         = 0;
    constexpr static Value UsageTest                    = 1 << 0;
    constexpr static Value HiddenFromCommandBlockOrigin = 1 << 1;
    constexpr static Value HiddenFromPlayerOrigin       = 1 << 2;
    constexpr static Value HiddenFromAutomationOrigin   = 1 << 3;
    constexpr static Value SyncLocal                    = 1 << 4;
    constexpr static Value ExecuteDisallowed            = 1 << 5;
    constexpr static Value TypeMessage                  = 1 << 6;
    constexpr static Value NotCheat                     = 1 << 7;
    constexpr static Value Async                        = 1 << 8;
    constexpr static Value NoEditor                     = 1 << 9;
    constexpr static Value Hidden                       = HiddenFromPlayerOrigin | HiddenFromCommandBlockOrigin;
    constexpr static Value Removed                      = Hidden | HiddenFromAutomationOrigin;

    Value value{None};

    CommandFlag() = default;

    constexpr explicit CommandFlag(Value flag) noexcept : value(flag) {}

    [[nodiscard]] constexpr bool operator==(const CommandFlag& rhs) const noexcept { return value == rhs.value; }
    [[nodiscard]] constexpr bool operator!=(const CommandFlag& rhs) const noexcept { return value != rhs.value; }

    constexpr CommandFlag& operator|=(const CommandFlag& rhs) noexcept {
        value = value | rhs.value;
        return *this;
    }

    constexpr CommandFlag& operator|=(Value rhs) noexcept {
        value = value | rhs;
        return *this;
    }

    constexpr CommandFlag& remove(Value rhs) noexcept {
        value = value & ~rhs;
        return *this;
    }

    [[nodiscard]] constexpr Value raw() const noexcept { return value; }
};

class CommandManager {
    struct CommandTarget {
        std::string value{};
    };

    struct CommandPosition {
        std::string x{};
        std::string y{};
        std::string z{};
    };

    using CommandArgumentValue =
        std::variant<std::monostate, bool, std::int64_t, double, std::string, CommandTarget, CommandPosition>;

    struct CommandArgument {
        std::string          type{};
        CommandArgumentValue value{};
        std::string          raw{};

        template <typename T>
        [[nodiscard]] const T& get() const {
            return std::get<T>(value);
        }

        template <typename T>
        [[nodiscard]] const T* get_if() const noexcept {
            return std::get_if<T>(&value);
        }
    };

    class CommandArguments {
        std::map<std::string, CommandArgument, std::less<>> mValues{};

    public:
        using Map            = std::map<std::string, CommandArgument, std::less<>>;
        using iterator       = Map::iterator;
        using const_iterator = Map::const_iterator;

        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

        [[nodiscard]] iterator begin() noexcept;
        [[nodiscard]] iterator end() noexcept;
        [[nodiscard]] const_iterator begin() const noexcept;
        [[nodiscard]] const_iterator end() const noexcept;

        [[nodiscard]] iterator find(std::string_view key);
        [[nodiscard]] const_iterator find(std::string_view key) const;
        [[nodiscard]] bool contains(std::string_view key) const;
        [[nodiscard]] CommandArgument& at(std::string_view key);
        [[nodiscard]] const CommandArgument& at(std::string_view key) const;

        CommandArgument& operator[](std::string key);
        void insert_or_assign(std::string key, CommandArgument argument);
    };

    class CommandOutput {
        protocol::CommandOutputPacket mPacket{};

    public:
        explicit CommandOutput(const protocol::CommandOriginData& originData);

        void success(std::string messageId, std::vector<std::string> parameters = {});
        void error(std::string messageId, std::vector<std::string> parameters = {});

        [[nodiscard]] const protocol::CommandOutputPacket& packet() const noexcept;
    };

    using CommandCallback = std::function<void(const CommandArguments&, CommandOutput&)>;

    struct ParameterDefinition {
        std::string              name{};
        std::string              type{};
        std::uint32_t            parseSymbol{};
        bool                     optional{};
        std::uint8_t             options{};
        std::vector<std::string> enumValues{};
    };

    struct OverloadDefinition {
        std::string                      name{};
        bool                             chaining{};
        std::vector<ParameterDefinition> parameters{};
    };

    struct CallbackDefinition {
        std::string     overload{};
        CommandCallback callback{};
    };

    struct CommandDefinition {
        std::string name{};
        std::string description{};
        std::string output{};
        CommandFlag flags{};
        std::vector<OverloadDefinition> overloads{};
        std::vector<CallbackDefinition> callbacks{};
    };

    enum class SoftEnumMatch {
        Strict,
        Loose,
    };

    struct Token {
        std::string raw{};
        std::size_t begin{};
        std::size_t end{};
        bool        quoted{};
        bool        valid{true};
    };

    std::vector<CommandDefinition> mCommands{};
    std::set<std::string, std::less<>> mOnlinePlayers{};
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> mPlayerNamesByUuid{};

public:
    CommandManager() = default;

    bool initialize();

    [[nodiscard]] std::pair<std::size_t, std::size_t> injectCommands(protocol::AvailableCommandsPacket& packet) const;
    [[nodiscard]] bool handleRequest(ProxyBridge& bridge, const protocol::CommandRequestPacket& packet) const;
    void handlePlayerList(const protocol::PlayerListPacket& packet);

private:
    void addBuiltInTestCommands();

    [[nodiscard]] const CommandDefinition* findCommand(std::string_view command) const;
    [[nodiscard]] std::pair<const OverloadDefinition*, CommandArguments> parseOverload(
        const CommandDefinition& command,
        std::string_view commandLine
    ) const;
    [[nodiscard]] const CommandCallback* findCallback(
        const CommandDefinition& command,
        const OverloadDefinition* overload
    ) const;
    [[nodiscard]] static protocol::CommandOverloadData makeOverload(
        protocol::AvailableCommandsPacket& packet,
        const CommandDefinition& command,
        const OverloadDefinition& overload
    );
    [[nodiscard]] std::optional<CommandArguments> parseArguments(
        std::string_view commandLine,
        const OverloadDefinition& overload,
        SoftEnumMatch softEnumMatch
    ) const;
    [[nodiscard]] std::optional<CommandArgument> parseArgument(
        const ParameterDefinition& parameter,
        std::string_view raw,
        bool quoted,
        SoftEnumMatch softEnumMatch
    ) const;
    [[nodiscard]] static std::optional<CommandPosition> parsePosition(std::string_view raw);
    [[nodiscard]] static bool overloadHasSoftEnum(const OverloadDefinition& overload);
    [[nodiscard]] bool isKnownPlayer(std::string_view name) const;
    [[nodiscard]] static std::vector<Token> tokenize(std::string_view commandLine);
    [[nodiscard]] static protocol::CommandParameterData makeParameter(
        protocol::AvailableCommandsPacket& packet,
        const CommandDefinition& command,
        const ParameterDefinition& parameter
    );
    [[nodiscard]] static std::uint32_t parseSymbolFor(
        protocol::AvailableCommandsPacket& packet,
        const CommandDefinition& command,
        const ParameterDefinition& parameter
    );
    [[nodiscard]] static std::uint32_t enumParseSymbol(
        protocol::AvailableCommandsPacket& packet,
        std::string enumName,
        const std::vector<std::string>& values
    );
    [[nodiscard]] static std::uint32_t addEnum(
        protocol::AvailableCommandsPacket& packet,
        std::string enumName,
        const std::vector<std::string>& values
    );
    [[nodiscard]] static std::uint32_t addSoftEnum(
        protocol::AvailableCommandsPacket& packet,
        std::string enumName,
        const std::vector<std::string>& values
    );
    [[nodiscard]] static std::uint32_t builtinParseSymbol(std::string_view type);
    [[nodiscard]] static std::string normalizeCommand(std::string_view command);
};

} // namespace sculk
