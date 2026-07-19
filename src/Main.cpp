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

#include "Logger.hpp"
#include "ProxyPass.hpp"

int main() {
    sculk::ProxyPass::setWorkingDirectory();
    sculk::ProxyPass::initConsole();
    auto proxyPass = sculk::ProxyPass();
    if (!proxyPass.start()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }
    proxyPass.waitForStop();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    return 0;
}