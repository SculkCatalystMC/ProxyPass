// Copyright © 2026 SculkCatalystMC. All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "CommandManager.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <print>
#include <sstream>
#include <stdexcept>

namespace sculk {

bool CommandManager::CommandArguments::empty() const noexcept { return mValues.empty(); }

std::size_t CommandManager::CommandArguments::size() const noexcept { return mValues.size(); }

CommandManager::CommandArguments::iterator CommandManager::CommandArguments::begin() noexcept { return mValues.begin(); }

CommandManager::CommandArguments::iterator CommandManager::CommandArguments::end() noexcept { return mValues.end(); }

CommandManager::CommandArguments::const_iterator CommandManager::CommandArguments::begin() const noexcept {
    return mValues.begin();
}

CommandManager::CommandArguments::const_iterator CommandManager::CommandArguments::end() const noexcept {
    return mValues.end();
}

CommandManager::CommandArguments::iterator CommandManager::CommandArguments::find(std::string_view key) {
    return mValues.find(key);
}

CommandManager::CommandArguments::const_iterator CommandManager::CommandArguments::find(std::string_view key) const {
    return mValues.find(key);
}

bool CommandManager::CommandArguments::contains(std::string_view key) const { return mValues.contains(key); }

CommandManager::CommandArgument& CommandManager::CommandArguments::at(std::string_view key) {
    auto found = find(key);
    if (found == end()) {
        throw std::out_of_range{"Command argument not found"};
    }
    return found->second;
}

const CommandManager::CommandArgument& CommandManager::CommandArguments::at(std::string_view key) const {
    auto found = find(key);
    if (found == end()) {
        throw std::out_of_range{"Command argument not found"};
    }
    return found->second;
}

CommandManager::CommandArgument& CommandManager::CommandArguments::operator[](std::string key) {
    return mValues[std::move(key)];
}

void CommandManager::CommandArguments::insert_or_assign(std::string key, CommandArgument argument) {
    mValues.insert_or_assign(std::move(key), std::move(argument));
}

CommandManager::CommandOutput::CommandOutput(const protocol::CommandOriginData& originData) {
    mPacket.mOriginData  = originData;
    mPacket.mOutputType  = protocol::CommandOutputPacket::Type::AllOutput;
    mPacket.mSuccessCount = 0;
}

void CommandManager::CommandOutput::success(std::string messageId, std::vector<std::string> parameters) {
    protocol::CommandOutputPacket::OutputMessage message{};
    message.mSuccess    = true;
    message.mMessageId  = std::move(messageId);
    message.mParameters = std::move(parameters);
    mPacket.mOutputMessages.push_back(std::move(message));
    ++mPacket.mSuccessCount;
}

void CommandManager::CommandOutput::error(std::string messageId, std::vector<std::string> parameters) {
    protocol::CommandOutputPacket::OutputMessage message{};
    message.mSuccess    = false;
    message.mMessageId  = std::move(messageId);
    message.mParameters = std::move(parameters);
    mPacket.mOutputMessages.push_back(std::move(message));
}

const protocol::CommandOutputPacket& CommandManager::CommandOutput::packet() const noexcept { return mPacket; }

bool CommandManager::initialize() {
    mCommands.clear();
    addBuiltInTestCommands();
    std::println("[ProxyPass][Command] Manager initialized. commands={}", mCommands.size());
    return true;
}

std::pair<std::size_t, std::size_t> CommandManager::injectCommands(protocol::AvailableCommandsPacket& packet) const {
    std::size_t inserted{};
    std::size_t skipped{};
    for (const auto& definition : mCommands) {
        const auto alreadyExists = std::ranges::any_of(packet.mCommands, [&definition](const protocol::CommandData& command) {
            return command.mName == definition.name;
        });
        if (alreadyExists) {
            ++skipped;
            std::println("[ProxyPass][Command] Skipped existing command '/{}'.", definition.name);
            continue;
        }

        protocol::CommandData command{};
        command.mName                   = definition.name;
        command.mDescription            = definition.description;
        command.mFlags                  = definition.flags.raw();
        command.mCommandPermissionLevel = protocol::CommandPermissionLevel::Any;
        command.mAliasEnum              = -1;
        if (definition.overloads.empty()) {
            command.mOverloads.push_back({});
        } else {
            for (const auto& overload : definition.overloads) {
                command.mOverloads.push_back(makeOverload(packet, definition, overload));
            }
        }
        auto insertPosition = std::ranges::lower_bound(
            packet.mCommands,
            command.mName,
            {},
            &protocol::CommandData::mName
        );
        const auto index = static_cast<std::size_t>(std::ranges::distance(packet.mCommands.begin(), insertPosition));
        packet.mCommands.insert(insertPosition, std::move(command));
        ++inserted;
        const auto sorted = std::ranges::is_sorted(packet.mCommands, {}, &protocol::CommandData::mName);
        std::println(
            "[ProxyPass][Command] Registered proxy command '/{}' into AvailableCommands with flags={}, overloads={}, index={}, sorted={}",
            definition.name,
            definition.flags.raw(),
            packet.mCommands[index].mOverloads.size(),
            index,
            sorted
        );
    }
    return {inserted, skipped};
}

bool CommandManager::handleRequest(ProxyBridge& bridge, const protocol::CommandRequestPacket& packet) const {
    const auto* definition = findCommand(packet.mCommand);
    if (!definition) {
        return false;
    }

    const auto [overload, arguments] = parseOverload(*definition, packet.mCommand);
    const auto* callback = findCallback(*definition, overload);
    CommandOutput output{packet.mOriginData};
    if (callback) {
        (*callback)(arguments, output);
    } else if (!overload && !definition->overloads.empty()) {
        output.error("No matching overload for proxy command '/" + definition->name + "'.");
    } else {
        output.success(definition->output);
    }

    bridge.sendPacketToClient(output.packet());
    std::println(
        "[ProxyPass][Command] Intercepted proxy command '/{}' overload='{}' args={} from player '{}'.",
        definition->name,
        overload ? overload->name : "",
        arguments.size(),
        bridge.mClientInfo.name.empty() ? "unknown" : bridge.mClientInfo.name
    );
    return true;
}

void CommandManager::handlePlayerList(const protocol::PlayerListPacket& packet) {
    if (packet.mAction == protocol::PlayerListPacket::ActionType::Add) {
        for (const auto& entry : packet.mPlayerEntryList) {
            if (!entry.mPlayerName.empty()) {
                mOnlinePlayers.insert(entry.mPlayerName);
                mPlayerNamesByUuid.insert_or_assign(
                    {entry.mUUID.mMostSignificantBits, entry.mUUID.mLeastSignificantBits},
                    entry.mPlayerName
                );
            }
        }
    } else {
        for (const auto& entry : packet.mPlayerEntryList) {
            const auto key = std::pair{entry.mUUID.mMostSignificantBits, entry.mUUID.mLeastSignificantBits};
            auto found = mPlayerNamesByUuid.find(key);
            if (found != mPlayerNamesByUuid.end()) {
                mOnlinePlayers.erase(found->second);
                mPlayerNamesByUuid.erase(found);
            }
        }
    }
    std::println("[ProxyPass][Command] PlayerList updated. onlinePlayers={}", mOnlinePlayers.size());
}

void CommandManager::addBuiltInTestCommands() {
    auto addCommand = [this](CommandDefinition command) {
        auto found = std::ranges::find_if(mCommands, [&command](const CommandDefinition& existing) {
            return existing.name == command.name;
        });
        if (found != mCommands.end()) {
            *found = std::move(command);
            return;
        }
        mCommands.push_back(std::move(command));
    };

    addCommand({
        .name        = "proxytest",
        .description = "ProxyPass test command without parameters",
        .output      = "ProxyPass proxytest executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .callbacks   = {{
            .overload = "",
            .callback = [](const CommandArguments&, CommandOutput& output) {
                output.success("ProxyPass proxytest callback executed.");
            },
        }},
    });

    addCommand({
        .name        = "proxymsg",
        .description = "ProxyPass test command with a message parameter",
        .output      = "ProxyPass proxymsg executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {{
            .name       = "message",
            .parameters = {{
                .name = "message",
                .type = "message",
            }},
        }},
        .callbacks   = {{
            .overload = "message",
            .callback = [](const CommandArguments& args, CommandOutput& output) {
                auto message = args.at("message").get<std::string>();
                output.success("ProxyPass proxymsg callback executed: " + message);
            },
        }},
    });

    addCommand({
        .name        = "proxytarget",
        .description = "ProxyPass test command with target and optional message",
        .output      = "ProxyPass proxytarget executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {{
            .name       = "target_message",
            .parameters = {
                {
                    .name = "target",
                    .type = "target",
                },
                {
                    .name     = "message",
                    .type     = "message",
                    .optional = true,
                },
            },
        }},
        .callbacks   = {{
            .overload = "target_message",
            .callback = [](const CommandArguments& args, CommandOutput& output) {
                const auto& target = args.at("target").get<CommandTarget>();
                auto message = std::string{};
                if (auto found = args.find("message"); found != args.end()) {
                    message = found->second.get<std::string>();
                }
                output.success("ProxyPass proxytarget target=" + target.value + " message=" + message);
            },
        }},
    });

    addCommand({
        .name        = "proxyenum",
        .description = "ProxyPass test command with enum choices",
        .output      = "ProxyPass proxyenum executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {{
            .name       = "action",
            .parameters = {{
                .name       = "action",
                .type       = "enum",
                .enumValues = {"status", "reload", "dump"},
            }},
        }},
        .callbacks   = {{
            .overload = "action",
            .callback = [](const CommandArguments& args, CommandOutput& output) {
                output.success("ProxyPass proxyenum action: " + args.at("action").get<std::string>());
            },
        }},
    });

    addCommand({
        .name        = "proxysoftenum",
        .description = "ProxyPass test command with soft enum choices",
        .output      = "ProxyPass proxysoftenum executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {{
            .name       = "action",
            .parameters = {{
                .name       = "action",
                .type       = "soft_enum",
                .enumValues = {"status", "reload", "dump"},
            }},
        }},
        .callbacks   = {{
            .overload = "action",
            .callback = [](const CommandArguments& args, CommandOutput& output) {
                output.success("ProxyPass proxysoftenum action: " + args.at("action").get<std::string>());
            },
        }},
    });

    addCommand({
        .name        = "proxymulti",
        .description = "ProxyPass test command with multiple overloads",
        .output      = "ProxyPass proxymulti executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {
            {.name = "empty"},
            {
                .name       = "count",
                .parameters = {{
                    .name = "count",
                    .type = "int",
                }},
            },
            {
                .name       = "target_message",
                .parameters = {
                    {
                        .name = "target",
                        .type = "target",
                    },
                    {
                        .name = "message",
                        .type = "message",
                    },
                },
            },
        },
        .callbacks   = {
            {
                .overload = "empty",
                .callback = [](const CommandArguments&, CommandOutput& output) {
                    output.success("ProxyPass proxymulti empty overload.");
                },
            },
            {
                .overload = "count",
                .callback = [](const CommandArguments& args, CommandOutput& output) {
                    output.success("ProxyPass proxymulti count overload: " + std::to_string(args.at("count").get<std::int64_t>()));
                },
            },
            {
                .overload = "target_message",
                .callback = [](const CommandArguments& args, CommandOutput& output) {
                    output.success("ProxyPass proxymulti target=" + args.at("target").get<CommandTarget>().value + " message=" + args.at("message").get<std::string>());
                },
            },
        },
    });

    addCommand({
        .name        = "proxyparseint",
        .description = "ProxyPass parser test for int values",
        .output      = "ProxyPass proxyparseint executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {{
            .name       = "int",
            .parameters = {{
                .name = "value",
                .type = "int",
            }},
        }},
        .callbacks   = {{
            .overload = "int",
            .callback = [](const CommandArguments& args, CommandOutput& output) {
                output.success("proxyparseint value=" + std::to_string(args.at("value").get<std::int64_t>()));
            },
        }},
    });

    addCommand({
        .name        = "proxyparsefloat",
        .description = "ProxyPass parser test for float values",
        .output      = "ProxyPass proxyparsefloat executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {{
            .name       = "float",
            .parameters = {{
                .name = "value",
                .type = "float",
            }},
        }},
        .callbacks   = {{
            .overload = "float",
            .callback = [](const CommandArguments& args, CommandOutput& output) {
                output.success("proxyparsefloat value=" + std::to_string(args.at("value").get<double>()));
            },
        }},
    });

    addCommand({
        .name        = "proxyparsestring",
        .description = "ProxyPass parser test for string and message values",
        .output      = "ProxyPass proxyparsestring executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {{
            .name       = "string_message",
            .parameters = {
                {
                    .name = "key",
                    .type = "string",
                },
                {
                    .name     = "message",
                    .type     = "message",
                    .optional = true,
                },
            },
        }},
        .callbacks   = {{
            .overload = "string_message",
            .callback = [](const CommandArguments& args, CommandOutput& output) {
                auto message = std::string{};
                if (auto found = args.find("message"); found != args.end()) {
                    message = found->second.get<std::string>();
                }
                output.success("proxyparsestring key=" + args.at("key").get<std::string>() + " message=" + message);
            },
        }},
    });

    addCommand({
        .name        = "proxyparseoverload",
        .description = "ProxyPass parser test for overload priority",
        .output      = "ProxyPass proxyparseoverload executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {
            {
                .name       = "int",
                .parameters = {{
                    .name = "value",
                    .type = "int",
                }},
            },
            {
                .name       = "string",
                .parameters = {{
                    .name = "value",
                    .type = "string",
                }},
            },
        },
        .callbacks   = {
            {
                .overload = "int",
                .callback = [](const CommandArguments& args, CommandOutput& output) {
                    output.success("proxyparseoverload int=" + std::to_string(args.at("value").get<std::int64_t>()));
                },
            },
            {
                .overload = "string",
                .callback = [](const CommandArguments& args, CommandOutput& output) {
                    output.success("proxyparseoverload string=" + args.at("value").get<std::string>());
                },
            },
        },
    });

    addCommand({
        .name        = "proxyparsesoft",
        .description = "ProxyPass parser test for soft enum priority and string fallback",
        .output      = "ProxyPass proxyparsesoft executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {
            {
                .name       = "soft_enum",
                .parameters = {{
                    .name       = "value",
                    .type       = "soft_enum",
                    .enumValues = {"status", "reload", "dump"},
                }},
            },
            {
                .name       = "string",
                .parameters = {{
                    .name = "value",
                    .type = "string",
                }},
            },
        },
        .callbacks   = {
            {
                .overload = "soft_enum",
                .callback = [](const CommandArguments& args, CommandOutput& output) {
                    output.success("proxyparsesoft soft_enum=" + args.at("value").get<std::string>());
                },
            },
            {
                .overload = "string",
                .callback = [](const CommandArguments& args, CommandOutput& output) {
                    output.success("proxyparsesoft string=" + args.at("value").get<std::string>());
                },
            },
        },
    });

    addCommand({
        .name        = "proxyparsetarget",
        .description = "ProxyPass parser test for exact player targets",
        .output      = "ProxyPass proxyparsetarget executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {{
            .name       = "target",
            .parameters = {{
                .name = "target",
                .type = "target",
            }},
        }},
        .callbacks   = {{
            .overload = "target",
            .callback = [](const CommandArguments& args, CommandOutput& output) {
                output.success("proxyparsetarget target=" + args.at("target").get<CommandTarget>().value);
            },
        }},
    });

    addCommand({
        .name        = "proxyparsepos",
        .description = "ProxyPass parser test for Bedrock compact coordinates",
        .output      = "ProxyPass proxyparsepos executed.",
        .flags       = CommandFlag{CommandFlag::NotCheat},
        .overloads   = {{
            .name       = "position",
            .parameters = {{
                .name = "pos",
                .type = "position",
            }},
        }},
        .callbacks   = {{
            .overload = "position",
            .callback = [](const CommandArguments& args, CommandOutput& output) {
                const auto& pos = args.at("pos").get<CommandPosition>();
                output.success("proxyparsepos x=" + pos.x + " y=" + pos.y + " z=" + pos.z);
            },
        }},
    });

    std::println("[ProxyPass][Command] Built-in test commands registered. commands={}", mCommands.size());
}

const CommandManager::CommandDefinition* CommandManager::findCommand(std::string_view command) const {
    const auto normalized = normalizeCommand(command);
    auto found = std::ranges::find_if(mCommands, [&normalized](const CommandDefinition& definition) {
        return definition.name == normalized;
    });
    if (found == mCommands.end()) {
        return nullptr;
    }
    return &*found;
}

std::pair<const CommandManager::OverloadDefinition*, CommandManager::CommandArguments> CommandManager::parseOverload(
    const CommandDefinition& command,
    std::string_view commandLine
) const {
    for (const auto& overload : command.overloads) {
        if (!overloadHasSoftEnum(overload)) {
            continue;
        }
        if (auto arguments = parseArguments(commandLine, overload, SoftEnumMatch::Strict)) {
            return {&overload, std::move(*arguments)};
        }
    }
    for (const auto& overload : command.overloads) {
        if (overloadHasSoftEnum(overload)) {
            continue;
        }
        if (auto arguments = parseArguments(commandLine, overload, SoftEnumMatch::Strict)) {
            return {&overload, std::move(*arguments)};
        }
    }
    for (const auto& overload : command.overloads) {
        if (!overloadHasSoftEnum(overload)) {
            continue;
        }
        if (auto arguments = parseArguments(commandLine, overload, SoftEnumMatch::Loose)) {
            return {&overload, std::move(*arguments)};
        }
    }
    return {nullptr, {}};
}

const CommandManager::CommandCallback* CommandManager::findCallback(
    const CommandDefinition& command,
    const OverloadDefinition* overload
) const {
    const auto overloadName = overload ? std::string_view{overload->name} : std::string_view{};
    auto found = std::ranges::find_if(command.callbacks, [overloadName](const CallbackDefinition& callback) {
        return callback.overload == overloadName;
    });
    if (found == command.callbacks.end()) {
        return nullptr;
    }
    return &found->callback;
}

protocol::CommandOverloadData CommandManager::makeOverload(
    protocol::AvailableCommandsPacket& packet,
    const CommandDefinition& command,
    const OverloadDefinition& overload
) {
    protocol::CommandOverloadData result{};
    result.mIsChaining = overload.chaining;
    for (const auto& parameter : overload.parameters) {
        result.mParameters.push_back(makeParameter(packet, command, parameter));
    }
    return result;
}

std::optional<CommandManager::CommandArguments> CommandManager::parseArguments(
    std::string_view commandLine,
    const OverloadDefinition& overload,
    SoftEnumMatch softEnumMatch
) const {
    if (commandLine.starts_with('/')) {
        commandLine.remove_prefix(1U);
    }

    const auto firstSpace = commandLine.find(' ');
    const auto argsLine = firstSpace == std::string_view::npos ? std::string_view{} : commandLine.substr(firstSpace + 1U);
    const auto tokens   = tokenize(argsLine);

    CommandArguments result{};
    std::size_t      tokenIndex{};
    for (std::size_t parameterIndex = 0U; parameterIndex < overload.parameters.size(); ++parameterIndex) {
        const auto& parameter = overload.parameters[parameterIndex];
        if (parameter.type == "message" && parameterIndex + 1U == overload.parameters.size()) {
            if (tokenIndex >= tokens.size()) {
                if (parameter.optional) {
                    continue;
                }
                return std::nullopt;
            }
            const auto messageStart = tokens[tokenIndex].begin;
            CommandArgument argument{};
            argument.type  = parameter.type;
            argument.raw   = std::string{argsLine.substr(messageStart)};
            argument.value = argument.raw;
            result.insert_or_assign(parameter.name, std::move(argument));
            tokenIndex = tokens.size();
            continue;
        }

        if (parameter.type == "position" || parameter.type == "blockpos") {
            if (tokenIndex >= tokens.size()) {
                if (parameter.optional) {
                    continue;
                }
                return std::nullopt;
            }

            if (!tokens[tokenIndex].valid) {
                return std::nullopt;
            }

            auto argument = parseArgument(parameter, tokens[tokenIndex].raw, tokens[tokenIndex].quoted, softEnumMatch);
            if (argument) {
                result.insert_or_assign(parameter.name, std::move(*argument));
                ++tokenIndex;
                continue;
            }

            if (tokenIndex + 2U >= tokens.size()) {
                if (parameter.optional) {
                    continue;
                }
                return std::nullopt;
            }

            auto joined = tokens[tokenIndex].raw + ' ' + tokens[tokenIndex + 1U].raw + ' ' + tokens[tokenIndex + 2U].raw;
            argument = parseArgument(parameter, joined, false, softEnumMatch);
            if (!argument) {
                if (parameter.optional) {
                    continue;
                }
                return std::nullopt;
            }
            result.insert_or_assign(parameter.name, std::move(*argument));
            tokenIndex += 3U;
            continue;
        }

        if (tokenIndex >= tokens.size()) {
            if (parameter.optional) {
                continue;
            }
            return std::nullopt;
        }

        if (!tokens[tokenIndex].valid) {
            return std::nullopt;
        }

        auto argument = parseArgument(parameter, tokens[tokenIndex].raw, tokens[tokenIndex].quoted, softEnumMatch);
        if (!argument) {
            if (parameter.optional) {
                continue;
            }
            return std::nullopt;
        }
        result.insert_or_assign(parameter.name, std::move(*argument));
        ++tokenIndex;
    }

    if (tokenIndex != tokens.size()) {
        return std::nullopt;
    }
    return result;
}

std::optional<CommandManager::CommandArgument> CommandManager::parseArgument(
    const ParameterDefinition& parameter,
    std::string_view raw,
    bool quoted,
    SoftEnumMatch softEnumMatch
) const {
    CommandArgument argument{};
    argument.type = parameter.type;
    argument.raw  = std::string{raw};

    if (quoted && parameter.type != "string" && parameter.type != "message") {
        return std::nullopt;
    }

    if (parameter.type == "int") {
        std::int64_t value{};
        const auto* first = raw.data();
        const auto* last  = raw.data() + raw.size();
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            return std::nullopt;
        }
        argument.value = value;
        return argument;
    }

    if (parameter.type == "float" || parameter.type == "value") {
        double value{};
        const auto* first = raw.data();
        const auto* last  = raw.data() + raw.size();
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            return std::nullopt;
        }
        argument.value = value;
        return argument;
    }

    if (parameter.type == "target") {
        if (raw.starts_with('@') || isKnownPlayer(raw)) {
            argument.value = CommandTarget{std::string{raw}};
            return argument;
        }
        return std::nullopt;
    }

    if (parameter.type == "position" || parameter.type == "blockpos") {
        auto position = parsePosition(raw);
        if (!position) {
            return std::nullopt;
        }
        argument.value = std::move(*position);
        return argument;
    }

    if (parameter.type == "enum") {
        auto found = std::ranges::find(parameter.enumValues, raw);
        if (found == parameter.enumValues.end()) {
            return std::nullopt;
        }
        argument.value = std::string{raw};
        return argument;
    }

    if (parameter.type == "soft_enum") {
        auto found = std::ranges::find(parameter.enumValues, raw);
        if (found == parameter.enumValues.end() && softEnumMatch == SoftEnumMatch::Strict) {
            return std::nullopt;
        }
        argument.value = std::string{raw};
        return argument;
    }

    argument.value = std::string{raw};
    return argument;
}

std::optional<CommandManager::CommandPosition> CommandManager::parsePosition(std::string_view raw) {
    auto parsePart = [](std::string_view value, char prefix) -> std::optional<std::string> {
        if (value.empty()) {
            return std::nullopt;
        }
        const auto original = value;
        if (value.front() == prefix) {
            value.remove_prefix(1U);
            if (value.empty()) {
                return std::string{original};
            }
        }
        if (value.empty()) {
            return std::string{original};
        }

        double parsed{};
        const auto* first = value.data();
        const auto* last  = value.data() + value.size();
        const auto result = std::from_chars(first, last, parsed);
        if (result.ec != std::errc{} || result.ptr != last) {
            return std::nullopt;
        }
        return std::string{original};
    };

    auto parseSeparated = [&]() -> std::optional<CommandPosition> {
        auto tokens = tokenize(raw);
        if (tokens.size() != 3U) {
            return std::nullopt;
        }

        auto x = parsePart(tokens[0].raw, tokens[0].raw.starts_with('^') ? '^' : '~');
        auto y = parsePart(tokens[1].raw, tokens[1].raw.starts_with('^') ? '^' : '~');
        auto z = parsePart(tokens[2].raw, tokens[2].raw.starts_with('^') ? '^' : '~');
        if (!x || !y || !z) {
            return std::nullopt;
        }
        return CommandPosition{std::move(*x), std::move(*y), std::move(*z)};
    };

    auto parseCompact = [&](char prefix) -> std::optional<CommandPosition> {
        if (raw.empty() || raw.front() != prefix) {
            return std::nullopt;
        }

        std::vector<std::string> parts{};
        std::size_t              offset{};
        while (offset < raw.size() && raw[offset] == prefix) {
            const auto begin = offset++;
            while (offset < raw.size() && raw[offset] != prefix) {
                ++offset;
            }
            parts.push_back(std::string{raw.substr(begin, offset - begin)});
        }
        if (parts.size() != 3U || offset != raw.size()) {
            return std::nullopt;
        }

        auto x = parsePart(parts[0], prefix);
        auto y = parsePart(parts[1], prefix);
        auto z = parsePart(parts[2], prefix);
        if (!x || !y || !z) {
            return std::nullopt;
        }
        return CommandPosition{
            std::move(*x),
            std::move(*y),
            std::move(*z)
        };
    };

    if (auto position = parseCompact('~')) {
        return position;
    }
    if (auto position = parseCompact('^')) {
        return position;
    }
    if (auto position = parseSeparated()) {
        return position;
    }
    return std::nullopt;
}

bool CommandManager::overloadHasSoftEnum(const OverloadDefinition& overload) {
    return std::ranges::any_of(overload.parameters, [](const ParameterDefinition& parameter) {
        return parameter.type == "soft_enum";
    });
}

protocol::CommandParameterData CommandManager::makeParameter(
    protocol::AvailableCommandsPacket& packet,
    const CommandDefinition& command,
    const ParameterDefinition& parameter
) {
    protocol::CommandParameterData result{};
    result.mName        = parameter.name;
    result.mParseSymbol = parseSymbolFor(packet, command, parameter);
    result.mIsOptional  = parameter.optional;
    result.mOptions     = parameter.options;
    return result;
}

std::uint32_t CommandManager::parseSymbolFor(
    protocol::AvailableCommandsPacket& packet,
    const CommandDefinition& command,
    const ParameterDefinition& parameter
) {
    if (parameter.parseSymbol != 0U) {
        return parameter.parseSymbol;
    }
    if (parameter.type == "enum") {
        return addEnum(packet, command.name + '_' + parameter.name, parameter.enumValues);
    }
    if (parameter.type == "soft_enum") {
        return addSoftEnum(packet, command.name + '_' + parameter.name, parameter.enumValues);
    }
    return builtinParseSymbol(parameter.type);
}

std::uint32_t CommandManager::enumParseSymbol(
    protocol::AvailableCommandsPacket& packet,
    std::string enumName,
    const std::vector<std::string>& values
) {
    return addEnum(packet, std::move(enumName), values);
}

std::uint32_t CommandManager::addEnum(
    protocol::AvailableCommandsPacket& packet,
    std::string enumName,
    const std::vector<std::string>& values
) {
    constexpr std::uint32_t EnumSymbolFlag = 0x300000U;

    auto enumData = protocol::CommandEnumData{};
    enumData.mName = std::move(enumName);
    for (const auto& value : values) {
        auto found = std::ranges::find(packet.mEnumValues, value);
        if (found == packet.mEnumValues.end()) {
            packet.mEnumValues.push_back(value);
            enumData.mValues.push_back(static_cast<std::uint32_t>(packet.mEnumValues.size() - 1U));
            continue;
        }
        enumData.mValues.push_back(static_cast<std::uint32_t>(std::ranges::distance(packet.mEnumValues.begin(), found)));
    }

    packet.mEnumData.push_back(std::move(enumData));
    return EnumSymbolFlag | static_cast<std::uint32_t>(packet.mEnumData.size() - 1U);
}

std::uint32_t CommandManager::addSoftEnum(
    protocol::AvailableCommandsPacket& packet,
    std::string enumName,
    const std::vector<std::string>& values
) {
    constexpr std::uint32_t SoftEnumSymbolFlag = 0x4000000U;

    auto found = std::ranges::find_if(packet.mSoftEnums, [&enumName](const protocol::CommandSoftEnumData& softEnum) {
        return softEnum.mName == enumName;
    });
    if (found != packet.mSoftEnums.end()) {
        return SoftEnumSymbolFlag |
            static_cast<std::uint32_t>(std::ranges::distance(packet.mSoftEnums.begin(), found));
    }

    protocol::CommandSoftEnumData softEnum{};
    softEnum.mName   = std::move(enumName);
    softEnum.mValues = values;
    packet.mSoftEnums.push_back(std::move(softEnum));
    return SoftEnumSymbolFlag | static_cast<std::uint32_t>(packet.mSoftEnums.size() - 1U);
}

std::uint32_t CommandManager::builtinParseSymbol(std::string_view type) {
    if (type == "int") {
        return 0x100001U;
    }
    if (type == "float") {
        return 0x100003U;
    }
    if (type == "value") {
        return 0x100004U;
    }
    if (type == "target") {
        return 0x100008U;
    }
    if (type == "filepath") {
        return 0x100011U;
    }
    if (type == "string") {
        return 0x100038U;
    }
    if (type == "blockpos") {
        return 0x100040U;
    }
    if (type == "position") {
        return 0x100041U;
    }
    if (type == "message") {
        return 0x100044U;
    }
    if (type == "json") {
        return 0x10004AU;
    }
    if (type == "blockstates") {
        return 0x100054U;
    }
    if (type == "range") {
        return 0x1000003U;
    }
    return 0x100038U;
}

bool CommandManager::isKnownPlayer(std::string_view name) const { return mOnlinePlayers.contains(name); }

std::vector<CommandManager::Token> CommandManager::tokenize(std::string_view commandLine) {
    std::vector<Token> result{};
    std::size_t        offset{};
    while (offset < commandLine.size()) {
        while (offset < commandLine.size() && std::isspace(static_cast<unsigned char>(commandLine[offset]))) {
            ++offset;
        }
        if (offset >= commandLine.size()) {
            break;
        }

        const auto begin = offset;
        std::string raw{};
        if (commandLine[offset] == '"') {
            ++offset;
            while (offset < commandLine.size()) {
                const auto character = commandLine[offset++];
                if (character == '\\') {
                    if (offset >= commandLine.size()) {
                        raw.push_back('\\');
                        break;
                    }
                    const auto escaped = commandLine[offset++];
                    switch (escaped) {
                    case 'n':
                        raw.push_back('\n');
                        break;
                    case 'r':
                        raw.push_back('\r');
                        break;
                    case 't':
                        raw.push_back('\t');
                        break;
                    case '"':
                        raw.push_back('"');
                        break;
                    case '\\':
                        raw.push_back('\\');
                        break;
                    default:
                        raw.push_back(escaped);
                        break;
                    }
                    continue;
                }
                if (character == '"') {
                    break;
                }
                raw.push_back(character);
            }
            const auto valid = offset <= commandLine.size() && offset > 0U && commandLine[offset - 1U] == '"';
            result.push_back(Token{std::move(raw), begin, offset, true, valid});
            continue;
        }

        while (offset < commandLine.size() && !std::isspace(static_cast<unsigned char>(commandLine[offset]))) {
            ++offset;
        }
        if (begin != offset) {
            result.push_back(Token{std::string{commandLine.substr(begin, offset - begin)}, begin, offset, false, true});
        }
    }
    return result;
}

std::string CommandManager::normalizeCommand(std::string_view command) {
    if (command.starts_with('/')) {
        command.remove_prefix(1U);
    }
    if (const auto space = command.find(' '); space != std::string_view::npos) {
        command = command.substr(0U, space);
    }

    std::string result{command};
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

} // namespace sculk
