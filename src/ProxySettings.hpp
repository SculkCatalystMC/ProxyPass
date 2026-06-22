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
#include <cstdint>
#include <sculk/protocol/codec/MinecraftPacketIds.hpp>
#include <sculk/reflection/annotated.hpp>
#include <set>
#include <string>

namespace sculk {

#define COMMENT(...)                  __VA_ARGS__
#define CONFIG(TYPE, FIELD, COMMENTS) reflection::annotated<TYPE, COMMENTS> FIELD
#define CONFIG_STRUCT(FIELD, COMMENTS, ...)                                                                            \
    struct AnonymousType_##FIELD {                                                                                     \
        __VA_ARGS__                                                                                                    \
    };                                                                                                                 \
    reflection::annotated<AnonymousType_##FIELD, COMMENTS> FIELD;

struct ProxySettings {
    CONFIG(
        std::string,
        motd = "A Minecraft Proxy Server",
        COMMENT("Proxy server MOTD displayed.", "展示的代理服务器 MOTD")
    );
    CONFIG(std::uint16_t, proxy_port = 19132, COMMENT("Proxy server IPv4 port", "代理服务器 IPv4 端口"));
    CONFIG(std::uint16_t, proxy_port_v6 = 19133, COMMENT("Proxy server IPv6 port", "代理服务器 IPv6 端口"));
    CONFIG(
        bool,
        online_mode = true,
        COMMENT("Whether to enable Xbox Live online authentication", "是否启用 Xbox Live 在线认证")
    );
    CONFIG(
        std::string,
        language = "auto",
        COMMENT(
            "Display language. Use auto, en-US, zh-CN, or a custom locale file under lang/user.",
            "显示语言。可使用 auto、en-US、zh-CN，或 lang/user 下的自定义语言文件。"
        )
    );
    CONFIG(
        bool,
        i18n_auto_install = true,
        COMMENT(
            "Whether to automatically install default language files when missing.",
            "语言文件缺失时是否自动安装默认语言文件。"
        )
    );
    CONFIG(
        bool,
        i18n_check_completeness = true,
        COMMENT("Whether to check language file completeness at startup.", "启动时是否检查语言文件完整性。")
    );
    CONFIG(
        bool,
        i18n_write_missing_report = true,
        COMMENT("Whether to write missing translation reports into lang/user.", "是否将缺失翻译报告写入 lang/user。")
    );
    CONFIG(
        std::string,
        upstream_host = "127.0.0.1",
        COMMENT(
            "Upstream server host, usually an IP or domain name, supports IPv4 and IPv6",
            "上游服务器主机地址，通常为 IP 或域名，支持 IPv4 和 IPv6"
        )
    );
    CONFIG(std::uint16_t, upstream_port = 19134, COMMENT("Upstream server port", "上游服务器端口"));
    CONFIG(std::uint32_t, max_players = 100, COMMENT("Maximum number of players", "最大玩家数量"));
    CONFIG_STRUCT(
        packets_logger,
        COMMENT("Settings for packets logger", "数据包记录设置"),
        CONFIG(
            bool,
            log_parse_error = false,
            COMMENT(
                "Whether to log parse errors for packets logger",
                "It should not normally occur, this option is usually only used during protocol development and "
                "testing.",
                "数据包记录是否启用解析错误记录",
                "正常情况下不应该发生解析错误，此选项通常仅在协议开发测试使用。"
            )
        );
        CONFIG(
            bool,
            black_list_mode = false,
            COMMENT("Whether to use black list mode for packets logger", "数据包记录是否启用黑名单模式")
        );
        CONFIG(
            std::set<protocol::MinecraftPacketIds>,
            packet_ids = {},
            COMMENT(
                "Packet IDs for the packets logger, can use numeric IDs or protocol packet names (case-insensitive)",
                "Under black list mode, it means not logging these packets; under white list mode, it means only "
                "logging these packets",
                "数据包 ID 列表，可使用数字ID，也可以使用协议包名（不区分大小写）",
                "在黑名单模式下表示不记录这些数据包，在白名单模式下表示只记录这些数据包"
            )
        );
    );

    bool load();
    void save() const;
};

} // namespace sculk
