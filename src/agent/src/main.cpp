#include <agent.hpp>
#include <agent_info.hpp>
#include <cmd_ln_parser.hpp>
#include <http_client.hpp>
#include <register.hpp>

#include <iostream>
#include <optional>

int main(int argc, char* argv[])
{
    try
    {
        CommandlineParser cmdParser(argc, argv);

        if (cmdParser.OptionExists("--register"))
        {
            std::cout << "Starting registration process" << '\n';

            if (cmdParser.OptionExists("--user") && cmdParser.OptionExists("--password") &&
                cmdParser.OptionExists("--key"))
            {
                const auto user = cmdParser.GetOptionValue("--user");
                const auto password = cmdParser.GetOptionValue("--password");

                AgentInfo agentInfo;
                agentInfo.SetKey(cmdParser.GetOptionValue("--key"));

                if (cmdParser.OptionExists("--name"))
                {
                    agentInfo.SetName(cmdParser.GetOptionValue("--name"));
                }

                http_client::HttpClient httpClient;
                const registration::UserCredentials userCredentials {user, password};

                if (registration::RegisterAgent(userCredentials, httpClient))
                {
                    std::cout << "Agent registered." << '\n';
                }
                else
                {
                    std::cout << "Registration fail." << '\n';
                }
            }
            else
            {
                std::cout << "--user, --password and --key args are mandatory" << '\n';
            }

            std::cout << "Exiting ..." << '\n';
            return 0;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "An error occurred: " << e.what() << '\n';
        return 1;
    }

    Agent agent;
    agent.Run();
}
