/*GRB*

    Gerbera - https://gerbera.io/

    config_setup.cc - this file is part of Gerbera.
    Copyright (C) 2020-2023 Gerbera Contributors

    Gerbera is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation.

    Gerbera is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Gerbera.  If not, see <http://www.gnu.org/licenses/>.

    $Id$
*/

/// \file config_setup.cc

#include "config/config_setup.h" // API

#include "config/client_config.h"
#include "config/config_definition.h"
#include "config/config_options.h"
#include "content/autoscan.h"

void ConfigIntSetup::makeOption(const pugi::xml_node& root, const std::shared_ptr<Config>& config, const std::map<std::string, std::string>* arguments)
{
    newOption(getXmlContent(root));
    setOption(config);
}

void ConfigIntSetup::makeOption(std::string optValue, const std::shared_ptr<Config>& config, const std::map<std::string, std::string>* arguments)
{
    if (rawCheck) {
        if (!rawCheck(optValue)) {
            throw_std_runtime_error("Invalid {} value '{}'", xpath, optValue);
        }
    } else if (valueCheck) {
        if (!valueCheck(std::stoi(optValue))) {
            throw_std_runtime_error("Invalid {} value '{}'", xpath, optValue);
        }
    } else if (minCheck) {
        if (!minCheck(std::stoi(optValue), minValue)) {
            throw_std_runtime_error("Invalid {} value '{}', must be at least {}", xpath, optValue, minValue);
        }
    }
    try {
        optionValue = std::make_shared<IntOption>(std::stoi(optValue));
        setOption(config);
    } catch (const std::runtime_error& e) {
        throw_std_runtime_error("Error in config file: {} unsupported int value '{}'", xpath, optValue);
    }
}

int ConfigIntSetup::checkIntValue(std::string& sVal, const std::string& pathName) const
{
    try {
        if (rawCheck) {
            if (!rawCheck(sVal)) {
                throw_std_runtime_error("Invalid {}/{} value '{}'", pathName, xpath, sVal);
            }
        } else if (valueCheck) {
            if (!valueCheck(std::stoi(sVal))) {
                throw_std_runtime_error("Invalid {}/{} value {}", pathName, xpath, sVal);
            }
        } else if (minCheck) {
            if (!minCheck(std::stoi(sVal), minValue)) {
                throw_std_runtime_error("Invalid {}/{} value '{}', must be at least {}", pathName, xpath, sVal, minValue);
            }
        }
        if (parseValue)
            return parseValue(sVal);

        return std::stoi(sVal);
    } catch (const std::runtime_error& e) {
        throw_std_runtime_error("Error in config file: {}/{} unsupported int value '{}'", pathName, xpath, sVal);
    }
}

int ConfigIntSetup::getXmlContent(const pugi::xml_node& root)
{
    std::string sVal = ConfigSetup::getXmlContent(root, true);
    log_debug("Config: option: '{}/{}' value: '{}'", root.path(), xpath, sVal);
    return checkIntValue(sVal, root.path());
}

std::shared_ptr<ConfigOption> ConfigIntSetup::newOption(int optValue)
{
    if (valueCheck && !valueCheck(optValue)) {
        throw_std_runtime_error("Invalid {} value {}", xpath, optValue);
    }
    if (minCheck && !minCheck(optValue, minValue)) {
        throw_std_runtime_error("Invalid {} value {}, must be at least {}", xpath, optValue, minValue);
    }
    optionValue = std::make_shared<IntOption>(optValue);
    return optionValue;
}

#define MT_SQLITE_SYNC_EXTRA 3
#define MT_SQLITE_SYNC_FULL 2
#define MT_SQLITE_SYNC_NORMAL 1
#define MT_SQLITE_SYNC_OFF 0

bool ConfigIntSetup::CheckSqlLiteSyncValue(std::string& value)
{
    auto tempInt = 0;
    if (value == "off" || value == fmt::to_string(MT_SQLITE_SYNC_OFF))
        tempInt = MT_SQLITE_SYNC_OFF;
    else if (value == "normal" || value == fmt::to_string(MT_SQLITE_SYNC_NORMAL))
        tempInt = MT_SQLITE_SYNC_NORMAL;
    else if (value == "full" || value == fmt::to_string(MT_SQLITE_SYNC_FULL))
        tempInt = MT_SQLITE_SYNC_FULL;
    else if (value == "extra" || value == fmt::to_string(MT_SQLITE_SYNC_EXTRA))
        tempInt = MT_SQLITE_SYNC_EXTRA;
    else
        return false;
    value.assign(fmt::to_string(tempInt));
    return true;
}

bool ConfigIntSetup::CheckProfileNumberValue(std::string& value)
{
    auto tempInt = 0;
    if (value == "source" || value == fmt::to_string(SOURCE))
        tempInt = SOURCE;
    else if (value == "off" || value == fmt::to_string(OFF))
        tempInt = OFF;
    else {
        tempInt = std::stoi(value);
        if (tempInt <= 0)
            return false;
    }
    value.assign(fmt::to_string(tempInt));
    return true;
}

bool ConfigIntSetup::CheckMinValue(int value, int minValue)
{
    return value >= minValue;
}

bool ConfigIntSetup::CheckImageQualityValue(int value)
{
    return value >= 0 && value <= 10;
}

bool ConfigIntSetup::CheckUpnpStringLimitValue(int value)
{
    return value == -1 || value >= 4;
}

bool ConfigIntSetup::CheckPortValue(int value)
{
    return value >= 0 && value <= 65535;
}
