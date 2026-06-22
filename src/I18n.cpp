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

#include "I18n.hpp"
#include "I18nBuiltin.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace sculk::i18n {
namespace {

struct Catalog {
    std::string                                  locale{};
    std::string                                  fallback{};
    std::unordered_map<std::string, std::string> messages{};
};

struct CatalogCheck {
    std::vector<std::string> missingKeys{};
};

static Catalog     gBaseCatalog{};
static Catalog     gLocaleCatalog{};
static Catalog     gUserCatalog{};
static std::string gCurrentLocale{"en-US"};

void warn(std::string_view message) noexcept {
    try {
        std::println(stderr, "[i18n] warning: {}", message);
    } catch (...) {}
}

void warn(std::string_view pattern, const auto&... args) noexcept {
    try {
        std::println(stderr, "[i18n] warning: {}", std::vformat(pattern, std::make_format_args(args...)));
    } catch (...) {
        warn(pattern);
    }
}

std::string stripJsoncComments(std::string_view input) {
    if (input.starts_with("\xEF\xBB\xBF")) {
        input.remove_prefix(3);
    }

    std::string output{};
    output.reserve(input.size());

    bool inString = false;
    bool escaped  = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];

        if (inString) {
            output.push_back(ch);
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            inString = true;
            output.push_back(ch);
            continue;
        }

        if (ch == '/' && i + 1 < input.size() && input[i + 1] == '/') {
            i += 2;
            while (i < input.size() && input[i] != '\n') {
                ++i;
            }
            if (i < input.size()) {
                output.push_back(input[i]);
            }
            continue;
        }

        if (ch == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            i += 2;
            while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) {
                if (input[i] == '\n') {
                    output.push_back('\n');
                }
                ++i;
            }
            if (i + 1 < input.size()) {
                ++i;
            }
            continue;
        }

        output.push_back(ch);
    }

    return output;
}

class CatalogParser {
public:
    explicit CatalogParser(std::string text) : mText(std::move(text)) {}

    std::optional<Catalog> parse() {
        Catalog catalog{};
        if (!parseObject([&](const std::string& key) -> bool {
                if (key == "locale") {
                    return parseStringValue(catalog.locale);
                }
                if (key == "fallback") {
                    return parseStringValue(catalog.fallback);
                }
                if (key == "messages") {
                    return parseMessages(catalog.messages);
                }
                return skipValue();
            })) {
            return std::nullopt;
        }
        skipWhitespace();
        if (mPosition != mText.size()) {
            return std::nullopt;
        }
        return catalog;
    }

private:
    template <class Fn>
    bool parseObject(Fn&& fn) {
        skipWhitespace();
        if (!consume('{')) {
            return false;
        }
        skipWhitespace();
        if (consume('}')) {
            return true;
        }

        while (mPosition < mText.size()) {
            auto key = parseString();
            if (!key) {
                return false;
            }
            skipWhitespace();
            if (!consume(':')) {
                return false;
            }
            if (!fn(*key)) {
                return false;
            }
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
        }
        return false;
    }

    bool parseMessages(std::unordered_map<std::string, std::string>& messages) {
        return parseObject([&](const std::string& key) -> bool {
            std::string value{};
            if (!parseStringValue(value)) {
                return skipValue();
            }
            messages.insert_or_assign(key, std::move(value));
            return true;
        });
    }

    bool parseStringValue(std::string& value) {
        auto parsed = parseString();
        if (!parsed) {
            return false;
        }
        value = std::move(*parsed);
        return true;
    }

    bool skipValue() {
        skipWhitespace();
        if (mPosition >= mText.size()) {
            return false;
        }
        if (mText[mPosition] == '"') {
            return parseString().has_value();
        }
        if (mText[mPosition] == '{') {
            int depth = 0;
            do {
                if (mPosition >= mText.size()) {
                    return false;
                }
                if (mText[mPosition] == '"') {
                    if (!parseString()) {
                        return false;
                    }
                    continue;
                }
                if (mText[mPosition] == '{') {
                    ++depth;
                } else if (mText[mPosition] == '}') {
                    --depth;
                }
                ++mPosition;
            } while (depth > 0);
            return true;
        }
        if (mText[mPosition] == '[') {
            int depth = 0;
            do {
                if (mPosition >= mText.size()) {
                    return false;
                }
                if (mText[mPosition] == '"') {
                    if (!parseString()) {
                        return false;
                    }
                    continue;
                }
                if (mText[mPosition] == '[') {
                    ++depth;
                } else if (mText[mPosition] == ']') {
                    --depth;
                }
                ++mPosition;
            } while (depth > 0);
            return true;
        }
        while (mPosition < mText.size() && mText[mPosition] != ',' && mText[mPosition] != '}' && mText[mPosition] != ']'
        ) {
            ++mPosition;
        }
        return true;
    }

    std::optional<std::string> parseString() {
        skipWhitespace();
        if (!consume('"')) {
            return std::nullopt;
        }

        std::string value{};
        while (mPosition < mText.size()) {
            const char ch = mText[mPosition++];
            if (ch == '"') {
                return value;
            }
            if (ch != '\\') {
                value.push_back(ch);
                continue;
            }
            if (mPosition >= mText.size()) {
                return std::nullopt;
            }
            const char escaped = mText[mPosition++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u':
                if (mPosition + 4 > mText.size()) {
                    return std::nullopt;
                }
                value.append("\\u");
                value.append(mText.substr(mPosition, 4));
                mPosition += 4;
                break;
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    bool consume(char ch) {
        skipWhitespace();
        if (mPosition < mText.size() && mText[mPosition] == ch) {
            ++mPosition;
            return true;
        }
        return false;
    }

    void skipWhitespace() {
        while (mPosition < mText.size() && std::isspace(static_cast<unsigned char>(mText[mPosition]))) {
            ++mPosition;
        }
    }

    std::string mText{};
    std::size_t mPosition{};
};

std::optional<Catalog> parseCatalog(std::string_view text) {
    try {
        return CatalogParser(stripJsoncComments(text)).parse();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Catalog> readCatalog(const std::filesystem::path& path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return std::nullopt;
        }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return parseCatalog(content);
    } catch (...) {
        return std::nullopt;
    }
}

bool writeTextFileIfMissing(const std::filesystem::path& path, std::string_view content) noexcept {
    try {
        std::error_code ec{};
        if (std::filesystem::exists(path, ec)) {
            return true;
        }
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            warn("Failed to write language file: {}", path.string());
            return false;
        }
        file << content;
        warn("Installed builtin language catalog: {}", path.string());
        return true;
    } catch (const std::exception& e) {
        warn("Failed to write language file {}: {}", path.string(), e.what());
    } catch (...) {
        warn("Failed to write language file: {}", path.string());
    }
    return false;
}

void createDirectory(const std::filesystem::path& path) noexcept {
    std::error_code ec{};
    if (std::filesystem::exists(path, ec)) {
        return;
    }
    if (std::filesystem::create_directories(path, ec)) {
        warn("Created language directory: {}", path.string());
    } else if (ec) {
        warn("Failed to create language directory {}: {}", path.string(), ec.message());
    }
}

void installBuiltinCatalogs(const InitOptions& options) noexcept {
    createDirectory(options.langDir);
    createDirectory(options.userLangDir);
    writeTextFileIfMissing(options.langDir / "en-US.jsonc", builtin::kEnUsCatalog);
    writeTextFileIfMissing(options.langDir / "zh-CN.jsonc", builtin::kZhCnCatalog);
    writeTextFileIfMissing(options.langDir / "manifest.jsonc", builtin::kManifest);
}

std::string detectLocale(std::string requested) {
    if (requested != "auto" && !requested.empty()) {
        return requested;
    }

#ifdef _WIN32
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, localeName, -1, nullptr, 0, nullptr, nullptr);
        if (size > 1) {
            std::string utf8(static_cast<std::size_t>(size - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, localeName, -1, utf8.data(), size, nullptr, nullptr);
            if (utf8.starts_with("zh")) {
                return "zh-CN";
            }
        }
    }
#else
    const auto* envLocale = std::getenv("LC_ALL");
    if (envLocale == nullptr || *envLocale == '\0') {
        envLocale = std::getenv("LANG");
    }
    if (envLocale != nullptr && std::string_view(envLocale).starts_with("zh")) {
        return "zh-CN";
    }
#endif

    return "en-US";
}

std::set<std::string> extractPlaceholders(std::string_view value, bool& hasBare, bool& hasInvalidBraces) {
    std::set<std::string> result{};
    hasBare          = false;
    hasInvalidBraces = false;

    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '{') {
            if (i + 1 < value.size() && value[i + 1] == '{') {
                ++i;
                continue;
            }
            const auto close = value.find('}', i + 1);
            if (close == std::string_view::npos) {
                hasInvalidBraces = true;
                return result;
            }
            const auto inside = value.substr(i + 1, close - i - 1);
            if (inside.empty()) {
                hasBare = true;
            } else {
                std::size_t pos = 0;
                while (pos < inside.size() && std::isdigit(static_cast<unsigned char>(inside[pos]))) {
                    ++pos;
                }
                if (pos == 0 || (pos < inside.size() && inside[pos] != ':')) {
                    hasInvalidBraces = true;
                } else {
                    result.emplace(inside.substr(0, pos));
                }
            }
            i = close;
        } else if (value[i] == '}') {
            if (i + 1 < value.size() && value[i + 1] == '}') {
                ++i;
                continue;
            }
            hasInvalidBraces = true;
        }
    }

    return result;
}

std::string joinPlaceholders(const std::set<std::string>& placeholders) {
    std::string result{};
    for (const auto& item : placeholders) {
        if (!result.empty()) {
            result += ",";
        }
        result += "{";
        result += item;
        result += "}";
    }
    return result.empty() ? "<none>" : result;
}

CatalogCheck checkCatalog(const Catalog& base, const Catalog& catalog, std::string_view label) noexcept {
    CatalogCheck result{};

    for (const auto& [key, baseValue] : base.messages) {
        const auto iter = catalog.messages.find(key);
        if (iter == catalog.messages.end()) {
            result.missingKeys.push_back(key);
            continue;
        }
        if (iter->second.empty()) {
            warn("Language catalog {} has empty translation for key {}.", label, key);
        }

        bool       baseBare          = false;
        bool       baseInvalid       = false;
        bool       valueBare         = false;
        bool       valueInvalid      = false;
        const auto basePlaceholders  = extractPlaceholders(baseValue, baseBare, baseInvalid);
        const auto valuePlaceholders = extractPlaceholders(iter->second, valueBare, valueInvalid);

        if (baseBare) {
            warn("Language catalog en-US key {} uses bare {{}} placeholders.", key);
        }
        if (baseInvalid) {
            warn("Language catalog en-US key {} has invalid braces.", key);
        }
        if (valueBare) {
            warn("Language catalog {} key {} uses bare {{}} placeholders.", label, key);
        }
        if (valueInvalid) {
            warn("Language catalog {} key {} has invalid braces.", label, key);
        }
        if (!baseInvalid && !valueInvalid && basePlaceholders != valuePlaceholders) {
            warn(
                "Placeholder mismatch in locale {}, key {}. Expected {}, got {}.",
                label,
                key,
                joinPlaceholders(basePlaceholders),
                joinPlaceholders(valuePlaceholders)
            );
        }
    }

    for (const auto& [key, _] : catalog.messages) {
        if (!base.messages.contains(key)) {
            warn("Language catalog {} has extra key {}.", label, key);
        }
    }

    if (!result.missingKeys.empty()) {
        warn("Language catalog {} is incomplete: {} missing key(s).", label, result.missingKeys.size());
    }

    return result;
}

std::string jsonEscape(std::string_view value) {
    std::string result{};
    result.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result.push_back(ch);
            break;
        }
    }
    return result;
}

void writeMissingReport(
    const std::filesystem::path& userLangDir,
    std::string_view             localeName,
    const Catalog&               base,
    std::span<const std::string> missingKeys
) noexcept {
    const auto reportPath = userLangDir / (std::string(localeName) + ".missing.jsonc");

    try {
        if (missingKeys.empty()) {
            std::error_code ec{};
            std::filesystem::remove(reportPath, ec);
            return;
        }

        createDirectory(userLangDir);
        std::ofstream file(reportPath, std::ios::binary);
        if (!file.is_open()) {
            warn("Failed to write missing translation report: {}", reportPath.string());
            return;
        }

        file << "{\n";
        file << "  \"locale\": \"" << jsonEscape(localeName) << "\",\n";
        file << "  \"generated_by\": \"ProxyPass i18n checker\",\n";
        file << "  \"messages\": {\n";
        for (std::size_t i = 0; i < missingKeys.size(); ++i) {
            const auto& key = missingKeys[i];
            const auto  it  = base.messages.find(key);
            if (it == base.messages.end()) {
                continue;
            }
            file << "    \"" << jsonEscape(key) << "\": \"" << jsonEscape(it->second) << "\"";
            file << (i + 1 == missingKeys.size() ? "\n" : ",\n");
        }
        file << "  }\n";
        file << "}\n";
        warn("Wrote missing translation report: {}", reportPath.string());
    } catch (const std::exception& e) {
        warn("Failed to write missing translation report {}: {}", reportPath.string(), e.what());
    } catch (...) {
        warn("Failed to write missing translation report: {}", reportPath.string());
    }
}

Catalog loadCatalogOrEmpty(const std::filesystem::path& path, std::string_view label) noexcept {
    std::error_code ec{};
    if (!std::filesystem::exists(path, ec)) {
        return {};
    }
    if (auto catalog = readCatalog(path)) {
        return std::move(*catalog);
    }
    warn("Failed to parse language catalog: {}", label);
    return {};
}

Catalog loadBaseCatalog(const std::filesystem::path& path) noexcept {
    if (auto catalog = readCatalog(path)) {
        return std::move(*catalog);
    }
    warn("Failed to parse language catalog: {}", path.string());
    if (auto catalog = parseCatalog(builtin::kEnUsCatalog)) {
        return std::move(*catalog);
    }
    return {};
}

} // anonymous namespace

void init(const InitOptions& options) noexcept {
    try {
        if (options.autoInstallMissingFiles) {
            installBuiltinCatalogs(options);
        } else {
            createDirectory(options.langDir);
            createDirectory(options.userLangDir);
        }

        gCurrentLocale = detectLocale(options.locale);
        gBaseCatalog   = loadBaseCatalog(options.langDir / "en-US.jsonc");

        if (gCurrentLocale == "en-US") {
            gLocaleCatalog = {};
        } else {
            const auto localePath = options.langDir / (gCurrentLocale + ".jsonc");
            gLocaleCatalog        = loadCatalogOrEmpty(localePath, localePath.string());
        }

        const auto userLocalePath = options.userLangDir / (gCurrentLocale + ".jsonc");
        gUserCatalog              = loadCatalogOrEmpty(userLocalePath, userLocalePath.string());

        if (options.checkCompleteness && !gBaseCatalog.messages.empty()) {
            if (!gLocaleCatalog.messages.empty()) {
                const auto check = checkCatalog(gBaseCatalog, gLocaleCatalog, gCurrentLocale);
                if (options.writeMissingReport) {
                    writeMissingReport(options.userLangDir, gCurrentLocale, gBaseCatalog, check.missingKeys);
                }
            } else if (gCurrentLocale != "en-US" && options.writeMissingReport) {
                std::vector<std::string> missingKeys{};
                missingKeys.reserve(gBaseCatalog.messages.size());
                for (const auto& [key, _] : gBaseCatalog.messages) {
                    missingKeys.push_back(key);
                }
                writeMissingReport(options.userLangDir, gCurrentLocale, gBaseCatalog, missingKeys);
            }

            if (!gUserCatalog.messages.empty()) {
                const auto check = checkCatalog(gBaseCatalog, gUserCatalog, std::string("user/") + gCurrentLocale);
                if (options.writeMissingReport) {
                    writeMissingReport(options.userLangDir, gCurrentLocale, gBaseCatalog, check.missingKeys);
                }
            }

            const auto baseCheck = checkCatalog(gBaseCatalog, gBaseCatalog, "en-US");
            (void)baseCheck;
        }
    } catch (const std::exception& e) {
        warn("i18n initialization failed: {}", e.what());
        gCurrentLocale = "en-US";
        if (auto catalog = parseCatalog(builtin::kEnUsCatalog)) {
            gBaseCatalog = std::move(*catalog);
        }
        gLocaleCatalog = {};
        gUserCatalog   = {};
    } catch (...) {
        warn("i18n initialization failed.");
        gCurrentLocale = "en-US";
        if (auto catalog = parseCatalog(builtin::kEnUsCatalog)) {
            gBaseCatalog = std::move(*catalog);
        }
        gLocaleCatalog = {};
        gUserCatalog   = {};
    }
}

std::string_view locale() noexcept { return gCurrentLocale; }

std::string_view lookup(std::string_view key, std::string_view fallback) noexcept {
    try {
        const std::string keyString(key);
        if (const auto it = gUserCatalog.messages.find(keyString); it != gUserCatalog.messages.end()) {
            return it->second;
        }
        if (const auto it = gLocaleCatalog.messages.find(keyString); it != gLocaleCatalog.messages.end()) {
            return it->second;
        }
        if (const auto it = gBaseCatalog.messages.find(keyString); it != gBaseCatalog.messages.end()) {
            return it->second;
        }
    } catch (...) {}
    return fallback;
}

} // namespace sculk::i18n
