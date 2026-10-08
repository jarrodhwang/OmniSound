#include <omni/ipc.hpp>
#include <iostream>
#include <future>
#include <set>
using namespace omni;
int main()
{
    try
    {
        ComScope com;
        std::set<std::string> generated_ids;
        for (int i = 0; i < 256; ++i)
        {
            const auto id = new_id();
            if (!valid_id(id) || !generated_ids.insert(id).second)
                throw std::runtime_error("Generated group ID does not satisfy configuration validation");
        }
        auto value = std::string("OmniSound · 책상 🎧");
        if (utf8(wide(value)) != value)
            throw std::runtime_error("UTF round trip failed");
        auto path =
            std::filesystem::temp_directory_path() / wide("omnisound-test-" + new_id()) / L"config.json";
        Config c;
        c.groups.push_back({new_id(), "책상", {{"test-output-one", 1, 0, {}}}, 200});
        c.groups.push_back(
            {new_id(), "전체", {{"test-output-one", 1, 0, {}}, {"test-output-two", 1, 0, {}}}, 200});
        validate(c);
        atomic_json(path, Json(c));
        const auto saved = read_json(path).get<Config>();
        if (saved.version != 1 || saved.groups.size() != c.groups.size() ||
            saved.groups[0].id != c.groups[0].id || saved.groups[1].id != c.groups[1].id)
            throw std::runtime_error("Atomic configuration failed");
        c.revision = 1;
        atomic_json(path, Json(c));
        if (read_json(path).get<Config>().revision != 1)
            throw std::runtime_error("Replacement was not committed");
        std::filesystem::remove(path);
        std::filesystem::remove(path.parent_path());
        auto pipe = L"\\\\.\\pipe\\OmniSound.Test." + wide(new_id());
        {
            PipeServer server(pipe, false, [](const Json &j, const Caller &c) {
                if (c.sid != user_sid() || c.session != process_session())
                    throw std::runtime_error("Wrong caller identity");
                if (j.value("reject", false))
                    throw std::runtime_error("Rejected intentionally");
                return j;
            });
            for (int i = 0; i < 30; ++i)
            {
                Json data = {{"sequence", i}, {"unicode", value}, {"payload", std::string(16384, 'x')}};
                auto result = request(pipe, data);
                if (data != result)
                    throw std::runtime_error("IPC frame changed or truncated");
            }
            bool rejected = false;
            try
            {
                request(pipe, {{"reject", true}});
            }
            catch (const std::exception &)
            {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("Remote error was not propagated");
            bool duplicate_rejected = false;
            try
            {
                PipeServer duplicate(pipe, false, [](const Json &j, const Caller &) { return j; });
            }
            catch (const std::exception &)
            {
                duplicate_rejected = true;
            }
            if (!duplicate_rejected)
                throw std::runtime_error("A second server claimed the same pipe");
            std::vector<std::future<void>> clients;
            for (int client = 0; client < 4; ++client)
                clients.push_back(std::async(std::launch::async, [&, client] {
                    for (int i = 0; i < 12; ++i)
                    {
                        Json data = {{"client", client}, {"request", i}};
                        if (request(pipe, data, 5000) != data)
                            throw std::runtime_error("Concurrent IPC response mismatch");
                    }
                }));
            for (auto &result : clients)
                result.get();
            try
            {
                request(pipe, {{"oversized", std::string(max_message_bytes + 1, 'x')}}, 1000);
                throw std::logic_error("Oversized request accepted");
            }
            catch (const std::logic_error &)
            {
                throw;
            }
            catch (const std::exception &)
            {
            }
        }
        std::cout << "PASS generated IDs, populated group save/load, Unicode, atomic configuration, "
                     "authenticated bounded IPC, remote errors, "
                     "duplicate-server rejection, concurrent clients, oversized-message rejection, server "
                     "cancellation\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
