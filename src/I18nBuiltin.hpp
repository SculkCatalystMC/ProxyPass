// Copyright 漏 2026 SculkCatalystMC. All rights reserved.
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

#include <string_view>

namespace sculk::i18n::builtin {

inline constexpr std::string_view kEnUsCatalog = R"jsonc(
{
  "locale": "en-US",
  "fallback": "",
  "messages": {
    "proxy.starting": "Starting proxy server...",
    "proxy.version": "Version: {0}(ProtocolVersion {1})",
    "proxy.started": "Proxy server started in {0:.2f} seconds.",
    "proxy.stopped": "Proxy server stopped.",
    "proxy.shutdown": "Shutting down proxy server...",
    "proxy.exiting_with_code": "Exiting program with error code {0}.",

    "network.ipv4_supported": "IPv4 supported, port: {0}",
    "network.ipv6_supported": "IPv6 supported, port: {0}",
    "network.start_failed": "Failed to start proxy server.",
    "network.port_in_use": "Port [{0}] may be in use by another process. Free up port and re-run program or adjust proxy_settings.jsonc file to use alternate ports for proxy server",

    "auth.waiting_services": "Waiting for Minecraft services...",
    "auth.services_failed": "Failed to connect to Minecraft services: {0}",

    "command.unknown": "Unknown command: {0}. Please check that the command exists and that you have permission to use it.",

    "client.invalid_connection_request": "Invalid connection request",
    "client.connection_verification_failed": "Connection request verification failed",
    "client.failed_generate_handshake_token": "Failed to generate handshake token",
    "client.failed_compute_session_token": "Failed to compute client session token",
    "client.failed_upstream_connect": "Failed to connect to upstream server",
    "client.failed_bridge_init": "Failed to initialize proxy bridge",
    "client.invalid_handshake_token": "Invalid handshake token",
    "client.failed_server_session_key": "Failed to compute server session key",
    "client.proxy_shutdown": "Proxy server is shutting down",

    "crypto.server_key_pair_failed": "Failed to generate server key pair: {0}",
    "proxy.callback_disconnect_failed": "Failed to set proxy server disconnect callback.",
    "proxy.callback_packet_receive_failed": "Failed to set proxy server packet receive callback.",
    "proxy.callback_packet_parse_failed": "Failed to set proxy server packet parse failure callback.",
    "proxy.packet_parse_failed": "Failed to parse packet: {0}",
    "upstream.callback_packet_receive_failed": "Failed to set upstream packet receive callback.",
    "upstream.callback_packet_parse_failed": "Failed to set upstream packet parse failure callback.",
    "upstream.callback_connected_failed": "Failed to set upstream connected callback.",
    "upstream.callback_connection_failure_failed": "Failed to set upstream connection failure callback.",
    "upstream.connect_player_failed": "Failed to connect to upstream server for player: {0}.",
    "upstream.bridge_player_init_failed": "Failed to initialize proxy bridge for player: {0}.",
    "upstream.sign_login_token_failed": "Failed to sign upstream login token: {0}",
    "upstream.sign_login_token_disconnect": "Failed to sign upstream login token",

    "i18n.created_lang_dir": "Created language directory: {0}",
    "i18n.installed_builtin_catalog": "Installed builtin language catalog: {0}",
    "i18n.catalog_parse_failed": "Failed to parse language catalog: {0}",
    "i18n.catalog_incomplete": "Language catalog {0} is incomplete: {1} missing key(s).",
    "i18n.missing_report_written": "Wrote missing translation report: {0}",
    "i18n.placeholder_mismatch": "Placeholder mismatch in locale {0}, key {1}. Expected {2}, got {3}."
  }
}
)jsonc";

inline constexpr std::string_view kZhCnCatalog = R"jsonc(
{
  "locale": "zh-CN",
  "fallback": "en-US",
  "messages": {
    "proxy.starting": "正在启动代理服务器...",
    "proxy.version": "版本：{0}（协议版本 {1}）",
    "proxy.started": "代理服务器已启动，用时 {0:.2f} 秒。",
    "proxy.stopped": "代理服务器已停止。",
    "proxy.shutdown": "正在关闭代理服务器...",
    "proxy.exiting_with_code": "程序正在以错误代码 {0} 退出。",

    "network.ipv4_supported": "IPv4 已启用，端口：{0}",
    "network.ipv6_supported": "IPv6 已启用，端口：{0}",
    "network.start_failed": "代理服务器启动失败。",
    "network.port_in_use": "端口 [{0}] 可能已被其他进程占用。请释放端口后重新运行，或修改 proxy_settings.jsonc 为代理服务器使用其他端口。",

    "auth.waiting_services": "正在等待 Minecraft 服务...",
    "auth.services_failed": "连接 Minecraft 服务失败：{0}",

    "command.unknown": "未知命令：{0}。请确认该命令存在，并且你拥有使用权限。",

    "client.invalid_connection_request": "无效的连接请求",
    "client.connection_verification_failed": "连接请求验证失败",
    "client.failed_generate_handshake_token": "无法生成握手令牌",
    "client.failed_compute_session_token": "无法计算客户端会话令牌",
    "client.failed_upstream_connect": "无法连接到上游服务器",
    "client.failed_bridge_init": "代理桥初始化失败",
    "client.invalid_handshake_token": "无效的握手令牌",
    "client.failed_server_session_key": "无法计算服务器会话密钥",
    "client.proxy_shutdown": "代理服务器正在关闭",

    "crypto.server_key_pair_failed": "生成服务器密钥对失败：{0}",
    "proxy.callback_disconnect_failed": "设置代理服务器断开连接回调失败。",
    "proxy.callback_packet_receive_failed": "设置代理服务器数据包接收回调失败。",
    "proxy.callback_packet_parse_failed": "设置代理服务器数据包解析失败回调失败。",
    "proxy.packet_parse_failed": "解析数据包失败：{0}",
    "upstream.callback_packet_receive_failed": "设置上游数据包接收回调失败。",
    "upstream.callback_packet_parse_failed": "设置上游数据包解析失败回调失败。",
    "upstream.callback_connected_failed": "设置上游连接成功回调失败。",
    "upstream.callback_connection_failure_failed": "设置上游连接失败回调失败。",
    "upstream.connect_player_failed": "玩家 {0} 连接上游服务器失败。",
    "upstream.bridge_player_init_failed": "玩家 {0} 的代理桥初始化失败。",
    "upstream.sign_login_token_failed": "签名上游登录令牌失败：{0}",
    "upstream.sign_login_token_disconnect": "签名上游登录令牌失败",

    "i18n.created_lang_dir": "已创建语言目录：{0}",
    "i18n.installed_builtin_catalog": "已安装内置语言文件：{0}",
    "i18n.catalog_parse_failed": "语言文件解析失败：{0}",
    "i18n.catalog_incomplete": "语言文件 {0} 不完整：缺少 {1} 个 key。",
    "i18n.missing_report_written": "已写出缺失翻译报告：{0}",
    "i18n.placeholder_mismatch": "语言 {0} 的 key {1} 占位符不匹配。期望 {2}，实际 {3}。"
  }
}
)jsonc";

inline constexpr std::string_view kManifest = R"jsonc(
{
  "catalog_version": 1,
  "base_locale": "en-US",
  "supported_locales": [
    "en-US",
    "zh-CN"
  ]
}
)jsonc";

} // namespace sculk::i18n::builtin
