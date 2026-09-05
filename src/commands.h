#pragma once

#include <string>
#include <vector>
#include <memory>

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual int execute(const std::vector<std::string>& args) = 0;
};

class InstallCommand : public ICommand {
public:
    int execute(const std::vector<std::string>& args) override;
};

class UninstallCommand : public ICommand {
public:
    int execute(const std::vector<std::string>& args) override;
};

class RunCommand : public ICommand {
public:
    int execute(const std::vector<std::string>& args) override;
};

class CreateCommand : public ICommand {
public:
    int execute(const std::vector<std::string>& args) override;
};

class CommandFactory {
public:
    static std::unique_ptr<ICommand> create_command(const std::string& name);
};
