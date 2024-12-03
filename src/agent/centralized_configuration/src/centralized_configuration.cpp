#include <centralized_configuration.hpp>
#include <config.h>
#include <logger.hpp>

#include <filesystem>

namespace centralized_configuration
{
    boost::asio::awaitable<module_command::CommandExecutionResult> CentralizedConfiguration::ExecuteCommand(
        const std::string command,       // NOLINT(performance-unnecessary-value-param)
        const nlohmann::json parameters) // NOLINT(performance-unnecessary-value-param)
    {
        try
        {
            std::vector<std::string> groupIds {};

            if (command == "set-group")
            {
                if (m_setGroupIdFunction && m_downloadGroupFilesFunction && m_saveGroupIdFunction &&
                    m_validateFileFunction && m_reloadModulesFunction)
                {
                    if (parameters.empty())
                    {
                        LogWarn("Group set failed, no group list");
                        co_return module_command::CommandExecutionResult {
                            module_command::Status::FAILURE,
                            "CentralizedConfiguration group set failed, no group list"};
                    }

                    groupIds = parameters[0].get<std::vector<std::string>>();

                    m_setGroupIdFunction(groupIds);
                    m_saveGroupIdFunction();

                    try
                    {
                        std::filesystem::path sharedDirPath(config::DEFAULT_SHARED_CONFIG_PATH);

                        if (std::filesystem::exists(sharedDirPath) && std::filesystem::is_directory(sharedDirPath))
                        {
                            for (const auto& entry : std::filesystem::directory_iterator(sharedDirPath))
                            {
                                std::filesystem::remove_all(entry);
                            }
                        }
                    }
                    catch (const std::filesystem::filesystem_error& e)
                    {
                        LogWarn("Error while cleaning the shared directory {}.", e.what());
                        co_return module_command::CommandExecutionResult {
                            module_command::Status::FAILURE,
                            "CentralizedConfiguration group set failed, error while cleaning the shared directory"};
                    }
                }
                else
                {
                    LogWarn("Group set failed, one of the required functions has not been set.");
                    co_return module_command::CommandExecutionResult {
                        module_command::Status::FAILURE,
                        "CentralizedConfiguration group set failed, one of the required functions has not been set."};
                }
            }
            else if (command == "update-group")
            {
                if (m_getGroupIdFunction && m_downloadGroupFilesFunction && m_validateFileFunction &&
                    m_reloadModulesFunction)
                {
                    groupIds = m_getGroupIdFunction();
                }
                else
                {
                    LogWarn("Group update failed, one of the required functions has not been set.");
                    co_return module_command::CommandExecutionResult {
                        module_command::Status::FAILURE,
                        "CentralizedConfiguration group update failed, one of the required functions has not been "
                        "set."};
                }
            }
            else
            {
                LogWarn("CentralizedConfiguration command not recognized");
                co_return module_command::CommandExecutionResult {module_command::Status::FAILURE,
                                                                  "CentralizedConfiguration command not recognized"};
            }

            for (const auto& groupId : groupIds)
            {
                const std::filesystem::path tmpGroupFile =
                    std::filesystem::temp_directory_path() / (groupId + config::DEFAULT_SHARED_FILE_EXTENSION);
                m_downloadGroupFilesFunction(groupId, tmpGroupFile.string());
                if (!m_validateFileFunction(tmpGroupFile))
                {
                    LogWarn("Failed to validate the file for group '{}', invalid group file received: {}",
                            groupId,
                            tmpGroupFile.string());
                    co_return module_command::CommandExecutionResult {
                        module_command::Status::FAILURE,
                        "CentralizedConfiguration validate file failed, invalid file received."};
                }

                const std::filesystem::path destGroupFile = std::filesystem::path(config::DEFAULT_SHARED_CONFIG_PATH) /
                                                            (groupId + config::DEFAULT_SHARED_FILE_EXTENSION);

                try
                {
                    std::filesystem::create_directories(destGroupFile.parent_path());
                    std::filesystem::rename(tmpGroupFile, destGroupFile);
                }
                catch (const std::filesystem::filesystem_error& e)
                {
                    LogWarn("Failed to move file to destination: {}. Error: {}", destGroupFile.string(), e.what());
                    co_return module_command::CommandExecutionResult {module_command::Status::FAILURE,
                                                                      "Failed to move shared file to destination."};
                }
            }

            m_reloadModulesFunction();

            const std::string messageOnSuccess = "CentralizedConfiguration '" + command + "' done.";
            co_return module_command::CommandExecutionResult {module_command::Status::SUCCESS, messageOnSuccess};
        }
        catch (const nlohmann::json::exception&)
        {
            LogWarn("CentralizedConfiguration error while parsing parameters");
            co_return module_command::CommandExecutionResult {
                module_command::Status::FAILURE, "CentralizedConfiguration error while parsing parameters"};
        }
    }

    void CentralizedConfiguration::SetGroupIdFunction(SetGroupIdFunctionType setGroupIdFunction)
    {
        m_setGroupIdFunction = std::move(setGroupIdFunction);
    }

    void CentralizedConfiguration::GetGroupIdFunction(GetGroupIdFunctionType getGroupIdFunction)
    {
        m_getGroupIdFunction = std::move(getGroupIdFunction);
    }

    void CentralizedConfiguration::SaveGroupIdFunction(SaveGroupIdFunctionType saveGroupIdFunction)
    {
        m_saveGroupIdFunction = std::move(saveGroupIdFunction);
    }

    void
    CentralizedConfiguration::SetDownloadGroupFilesFunction(DownloadGroupFilesFunctionType downloadGroupFilesFunction)
    {
        m_downloadGroupFilesFunction = std::move(downloadGroupFilesFunction);
    }

    void CentralizedConfiguration::ValidateFileFunction(ValidateFileFunctionType validateFileFunction)
    {
        m_validateFileFunction = std::move(validateFileFunction);
    }

    void CentralizedConfiguration::ReloadModulesFunction(ReloadModulesFunctionType reloadModulesFunction)
    {
        m_reloadModulesFunction = std::move(reloadModulesFunction);
    }
} // namespace centralized_configuration
