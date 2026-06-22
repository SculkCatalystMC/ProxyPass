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

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace sculk::i18n {

struct InitOptions {
    std::filesystem::path langDir{"./lang"};
    std::filesystem::path userLangDir{"./lang/user"};
    std::string           locale{"auto"};

    bool autoInstallMissingFiles{true};
    bool checkCompleteness{true};
    bool writeMissingReport{true};
};

void             init(const InitOptions& options) noexcept;
std::string_view locale() noexcept;

std::string_view lookup(std::string_view key, std::string_view fallback) noexcept;

template <class... Args>
std::string tr(std::string_view key, std::string_view fallback, Args&&... args) {
    const auto pattern = lookup(key, fallback);

    try {
        return std::vformat(pattern, std::make_format_args(args...));
    } catch (const std::format_error&) {
        try {
            return std::vformat(fallback, std::make_format_args(args...));
        } catch (...) {
            return std::string(fallback);
        }
    }
}

} // namespace sculk::i18n
