#include "Service.h"
#include "Backend.h"
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <QElapsedTimer>

namespace flora {
namespace {
using Json = nlohmann::json;
double millisecondsNow() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint64_t serial(const Json &j) {
    const auto text = j.get<std::string>(); uint64_t value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || !value || std::to_string(value) != text)
        throw std::runtime_error("Invalid service serial");
    return value;
}
}
int serveRdc(const RdcJobRunner &run) {
    std::mutex mutex, outputMutex;
    std::condition_variable ready;
    std::deque<Json> messages;
    std::atomic<uint64_t> cancelledThrough{};
    std::atomic<bool> closing{};
    auto publish = [&](Json response) {
        const auto text = response.dump() + '\n';
        const std::lock_guard lock(outputMutex);
        fwrite(text.data(), 1, text.size(), stdout); fflush(stdout);
    };
    std::thread worker([&] {
        std::unique_ptr<RdcReplaySession> session;
        std::string generation;
        uint64_t opens{};
        for (;;) {
            Json message;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return closing || !messages.empty(); });
                if (closing) break;
                message = std::move(messages.front()); messages.pop_front();
            }
            Json response{{"schema", 1}, {"id", message.at("id")}, {"generation", message.at("generation")},
                          {"command", message.at("command")}, {"ok", false}};
            response["queue_ms"] = millisecondsNow() - message.at("queued_ms").get<double>();
            QElapsedTimer timer; timer.start();
            try {
                const auto id = serial(message.at("id"));
                const auto command = message.at("command").get<std::string>();
                const auto cancelled = [&] { return closing || id <= cancelledThrough.load(); };
                if (command == "open") {
                    session.reset();
                    generation = message.at("generation").get<std::string>();
                    session = std::make_unique<RdcReplaySession>(message.at("job"));
                    ++opens;
                } else if (command == "execute") {
                    if (!session || generation != message.at("generation").get<std::string>())
                        throw std::runtime_error("Analysis session generation mismatch");
                    if (cancelled()) throw std::runtime_error("Analysis request cancelled");
                    auto job = message.at("job");
                    job["analysis_queue_ms"] = response.at("queue_ms");
                    const auto code = run(job, session.get(), cancelled);
                    response["exit_code"] = code;
                    if (code) throw std::runtime_error("Analysis request failed; inspect result.json");
                } else if (command == "close") {
                    session.reset(); generation.clear();
                } else throw std::runtime_error("Unknown analysis service command");
                response["ok"] = true;
            } catch (const std::exception &e) { response["error"] = e.what(); }
            response["duration_ms"] = timer.nsecsElapsed() / 1e6;
            response["open_capture_count"] = opens;
            publish(std::move(response));
        }
    });
    std::string line;
    for (int c; (c = getchar()) != EOF;) {
        if (c != '\n') {
            if (line.size() >= 2 * 1024 * 1024) break;
            line += char(c); continue;
        }
        try {
            auto message = Json::parse(line); line.clear();
            if (message.at("schema") != 1) throw std::runtime_error("Unsupported analysis service protocol");
            const auto id = serial(message.at("id"));
            serial(message.at("generation"));
            const auto command = message.at("command").get<std::string>();
            if (command == "shutdown") break;
            if (command == "cancel") {
                const auto through = serial(message.at("through"));
                cancelledThrough.store(std::max(cancelledThrough.load(), through));
                publish({{"schema", 1}, {"command", command}, {"id", std::to_string(id)},
                    {"generation", message.at("generation")}, {"ok", true}});
                continue;
            }
            {
                const std::lock_guard lock(mutex);
                if (messages.size() >= 8) throw std::runtime_error("Analysis queue limit exceeded");
                message["queued_ms"] = millisecondsNow();
                messages.push_back(std::move(message));
            }
            ready.notify_one();
        } catch (const std::exception &e) {
            fprintf(stderr, "Analysis protocol: %s\n", e.what()); break;
        }
    }
    closing = true; ready.notify_one(); worker.join();
    return 0;
}
}
