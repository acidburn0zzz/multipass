/*
 * Copyright (C) 2017 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Alberto Aguirre <alberto.aguirre@canonical.com>
 *
 */

#include <src/client/client.h>
#include <src/daemon/daemon.h>
#include <src/daemon/daemon_config.h>

#include <multipass/name_generator.h>
#include <multipass/version.h>
#include <multipass/virtual_machine_factory.h>
#include <multipass/vm_image_host.h>
#include <multipass/vm_image_vault.h>

#include "mock_virtual_machine_factory.h"
#include "stub_image_host.h"
#include "stub_virtual_machine_factory.h"
#include "stub_vm_image_vault.h"
#include "stub_ssh_key_provider.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <memory>
#include <sstream>
#include <string>

namespace mp = multipass;
using namespace testing;

namespace
{

auto make_config(std::string server_address)
{
    mp::DaemonConfigBuilder builder;
    builder.server_address = server_address;
    return builder.build();
}

struct MockDaemon : public mp::Daemon
{
    using mp::Daemon::Daemon;
    MOCK_METHOD3(create,
                 grpc::Status(grpc::ServerContext*, const mp::CreateRequest*, grpc::ServerWriter<mp::CreateReply>*));
    MOCK_METHOD3(empty_trash, grpc::Status(grpc::ServerContext*, const mp::EmptyTrashRequest*, mp::EmptyTrashReply*));
    MOCK_METHOD3(exec, grpc::Status(grpc::ServerContext*, const mp::ExecRequest*, mp::ExecReply*));
    MOCK_METHOD3(info, grpc::Status(grpc::ServerContext*, const mp::InfoRequest*, mp::InfoReply*));
    MOCK_METHOD3(list, grpc::Status(grpc::ServerContext*, const mp::ListRequest*, mp::ListReply*));
    MOCK_METHOD3(recover, grpc::Status(grpc::ServerContext*, const mp::RecoverRequest*, mp::RecoverReply*));
    MOCK_METHOD3(ssh_info, grpc::Status(grpc::ServerContext*, const mp::SSHInfoRequest*, mp::SSHInfoReply*));
    MOCK_METHOD3(start, grpc::Status(grpc::ServerContext*, const mp::StartRequest*, mp::StartReply*));
    MOCK_METHOD3(stop, grpc::Status(grpc::ServerContext*, const mp::StopRequest*, mp::StopReply*));
    MOCK_METHOD3(trash, grpc::Status(grpc::ServerContext*, const mp::TrashRequest*, mp::TrashReply*));
    MOCK_METHOD3(version, grpc::Status(grpc::ServerContext*, const mp::VersionRequest*, mp::VersionReply*));
};

struct StubNameGenerator : public mp::NameGenerator
{
    StubNameGenerator(std::string name) : name{name}
    {
    }
    std::string make_name() override
    {
        return name;
    }
    std::string name;
};
} // namespace

struct Daemon : public Test
{
    Daemon()
    {
        config_builder.server_address = server_address;
        config_builder.cache_directory = cache_dir.path();
        config_builder.vault = std::make_unique<StubVMImageVault>();
        config_builder.factory = std::make_unique<StubVirtualMachineFactory>();
        config_builder.image_host = std::make_unique<StubVMImageHost>();
        config_builder.ssh_key_provider = std::make_unique<mp::StubSSHKeyProvider>();
    }

    MockVirtualMachineFactory* use_a_mock_vm_factory()
    {
        auto mock_factory = std::make_unique<NiceMock<MockVirtualMachineFactory>>();
        auto mock_factory_ptr = mock_factory.get();

        ON_CALL(*mock_factory_ptr, create_virtual_machine(_, _))
            .WillByDefault(Return(ByMove(std::make_unique<StubVirtualMachine>())));

        config_builder.factory = std::move(mock_factory);
        return mock_factory_ptr;
    }

    void send_command(std::vector<std::string> command, std::ostream& cout = std::cout)
    {
        send_commands({command}, cout);
    }

    // "commands" is a vector of commands that includes necessary positional arguments, ie,
    // "start foo"
    void send_commands(std::vector<std::vector<std::string>> commands, std::ostream& cout = std::cout)
    {
        // Commands need to be sent from a thread different from that the QEventLoop is on.
        // Event loop is started/stopped to ensure all signals are delivered
        std::thread t([this, &commands, &cout]() {
            mp::ClientConfig client_config{server_address, cout, std::cerr};
            mp::Client client{client_config};
            for (const auto& command : commands)
            {
                QStringList args = QStringList() << "multipass_test";

                for (const auto& arg : command)
                {
                    args << QString::fromStdString(arg);
                }
                client.run(args);
            }
            loop.quit();
        });
        loop.exec();
        t.join();
    }

#ifdef MULTIPASS_PLATFORM_WINDOWS
    std::string server_address{"localhost:50051"};
#else
    std::string server_address{"unix:/tmp/test-multipassd.socket"};
#endif
    QEventLoop loop; // needed as cross-thread signal/slots used internally by mp::Daemon
    QTemporaryDir cache_dir;
    mp::DaemonConfigBuilder config_builder;
};

TEST_F(Daemon, receives_commands)
{
    MockDaemon daemon{make_config(server_address)};

    EXPECT_CALL(daemon, create(_, _, _));
    EXPECT_CALL(daemon, empty_trash(_, _, _));
    // Expect this is called twice due to the connect and exec commands using the same call
#ifdef MULTIPASS_PLATFORM_WINDOWS
    EXPECT_CALL(daemon, exec(_, _, _)).Times(2);
#else
    EXPECT_CALL(daemon, ssh_info(_, _, _)).Times(2);
#endif
    EXPECT_CALL(daemon, info(_, _, _));
    EXPECT_CALL(daemon, list(_, _, _));
    EXPECT_CALL(daemon, recover(_, _, _));
    EXPECT_CALL(daemon, start(_, _, _));
    EXPECT_CALL(daemon, stop(_, _, _));
    EXPECT_CALL(daemon, trash(_, _, _));
    EXPECT_CALL(daemon, version(_, _, _));

    send_commands({{"connect", "foo"},
                   {"create"},
                   {"empty-trash"},
                   {"exec", "foo", "--", "cmd"},
                   {"info", "foo"}, // name argument is required
                   {"list"},
                   {"recover", "foo"}, // name argument is required
                   {"start", "foo"},   // name argument is required
                   {"stop", "foo"},    // name argument is required
                   {"trash", "foo"},   // name argument is required
                   {"version"}});
}

TEST_F(Daemon, creates_virtual_machines)
{
    auto mock_factory = use_a_mock_vm_factory();
    mp::Daemon daemon{config_builder.build()};

    EXPECT_CALL(*mock_factory, create_virtual_machine(_, _));
    send_command({"create"});
}

TEST_F(Daemon, on_creation_hooks_up_platform_prepare)
{
    auto mock_factory = use_a_mock_vm_factory();
    mp::Daemon daemon{config_builder.build()};

    EXPECT_CALL(*mock_factory, prepare(_));
    send_command({"create"});
}

TEST_F(Daemon, provides_version)
{
    mp::Daemon daemon{config_builder.build()};

    std::stringstream stream;
    send_command({"version"}, stream);

    EXPECT_THAT(stream.str(), HasSubstr(mp::version_string));
}

TEST_F(Daemon, generates_name_when_client_does_not_provide_one)
{
    const std::string expected_name{"pied-piper-valley"};

    config_builder.name_generator = std::make_unique<StubNameGenerator>(expected_name);
    mp::Daemon daemon{config_builder.build()};

    std::stringstream stream;
    send_command({"create"}, stream);

    EXPECT_THAT(stream.str(), HasSubstr(expected_name));
}
