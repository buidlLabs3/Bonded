#include "integrations/memory_adapters.h"
#include "runtime/reliability.h"
#include "security/crypto.h"
#include "services/a2a_service.h"
#include "services/bond_service.h"
#include "services/contact_rules.h"
#include "storage/database.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using bonded::A2AService;
using bonded::A2ATask;
using bonded::A2ATaskState;
using bonded::AgentCard;
using bonded::BondRecord;
using bonded::BondService;
using bonded::BoundedQueue;
using bonded::ContactRules;
using bonded::Crypto;
using bonded::Database;
using bonded::Json;
using bonded::MemoryProgramAdapter;
using bonded::MemoryStorageAdapter;
using bonded::SettlementOutcome;

double elapsedSeconds(Clock::time_point started)
{
    return std::chrono::duration<double>(Clock::now() - started).count();
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Json queueBenchmark()
{
    constexpr std::size_t capacity = 4096;
    constexpr std::size_t items = 100000;
    BoundedQueue queue(capacity);
    const auto started = Clock::now();
    for (std::size_t offset = 0; offset < items; offset += capacity) {
        const auto count = std::min(capacity, items - offset);
        for (std::size_t index = 0; index < count; ++index) {
            queue.push(Json{{"id", offset + index}});
        }
        for (std::size_t index = 0; index < count; ++index) {
            require(queue.pop().at("id") == offset + index, "bounded queue lost FIFO order");
        }
    }
    const auto seconds = elapsedSeconds(started);
    BoundedQueue overflow(1);
    overflow.push(Json{{"id", 1}});
    bool rejected = false;
    try {
        overflow.push(Json{{"id", 2}});
    } catch (const bonded::DomainError&) {
        rejected = true;
    }
    require(rejected, "bounded queue did not reject overflow");
    return Json{{"items", items},
                {"capacity", capacity},
                {"operations_per_second", (items * 2) / seconds},
                {"overflow_rejected", rejected}};
}

Json spamBenchmark()
{
    constexpr std::size_t attempts = 100000;
    constexpr std::size_t limit = 100;
    ContactRules rules(limit, 60);
    std::size_t allowed = 0;
    const auto started = Clock::now();
    for (std::size_t index = 0; index < attempts; ++index) {
        allowed += rules.allow("benchmark-sender", 1000) ? 1 : 0;
    }
    const auto seconds = elapsedSeconds(started);
    require(allowed == limit, "spam burst did not enforce the exact rate limit");
    return Json{{"attempts", attempts},
                {"allowed", allowed},
                {"rejected", attempts - allowed},
                {"attempts_per_second", attempts / seconds}};
}

Json attachmentBenchmark()
{
    constexpr std::size_t attachment_bytes = 1024 * 1024;
    constexpr std::size_t repetitions = 24;
    const std::string payload(attachment_bytes, 'b');
    MemoryStorageAdapter storage;
    const auto started = Clock::now();
    for (std::size_t index = 0; index < repetitions; ++index) {
        const auto address = storage.put(payload);
        require(storage.get(address) == payload, "attachment round-trip changed bytes");
    }
    const auto seconds = elapsedSeconds(started);
    const auto total_bytes = attachment_bytes * repetitions * 2;
    return Json{{"attachment_bytes", attachment_bytes},
                {"repetitions", repetitions},
                {"mib_per_second", (total_bytes / (1024.0 * 1024.0)) / seconds},
                {"streaming_supported", false}};
}

Json a2aBenchmark()
{
    constexpr std::size_t thread_count = 4;
    constexpr std::size_t tasks_per_thread = 150;
    constexpr std::size_t total_tasks = thread_count * tasks_per_thread;
    MemoryProgramAdapter program;
    A2AService a2a(program, "logos-local");
    const auto [private_key, public_key] = Crypto::generateEd25519KeyPair();
    AgentCard card{"lf.a2a.v1", "logos-local", "provider", public_key,
                   {"private.process"}, Json::object(), "a2a/cards/provider", 12, 2000, ""};
    a2a.publishCard(A2AService::signCard(std::move(card), private_key), 1000);

    std::atomic<std::size_t> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    const auto started = Clock::now();
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([&, thread] {
            try {
                for (std::size_t index = 0; index < tasks_per_thread; ++index) {
                    const auto id = "task-" + std::to_string(thread) + "-" +
                                    std::to_string(index);
                    A2ATask task{id, "requester", "provider", "private.process",
                                 Json{{"object", "benchmark"}}, Json::object(), 12, 1900,
                                 A2ATaskState::Working, "", 0};
                    require(!a2a.createTask(task, 1000).payment_reference.empty(),
                            "A2A task did not lock payment");
                    require(a2a.complete(id, Json{{"result", "ok"}}).state ==
                                A2ATaskState::Completed,
                            "A2A task did not complete");
                }
            } catch (...) {
                ++failures;
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto seconds = elapsedSeconds(started);
    require(failures == 0, "concurrent A2A worker failed");
    require(a2a.activeTaskCount() == 0, "A2A benchmark left active tasks");
    return Json{{"threads", thread_count},
                {"tasks", total_tasks},
                {"tasks_per_second", total_tasks / seconds},
                {"active_after_completion", 0}};
}

Json recoveryBenchmark(const std::filesystem::path& database_path)
{
    constexpr std::size_t records = 1000;
    {
        Database database(database_path);
        database.migrate();
        for (std::size_t index = 0; index < records; ++index) {
            database.enqueue("benchmark/recovery", std::to_string(index));
        }
        database.recordProcessedEvent("benchmark-event");
    }
    const auto started = Clock::now();
    {
        Database database(database_path);
        database.migrate();
        require(database.pendingOutbox(records + 1).size() == records,
                "restart lost durable outbox records");
        require(database.hasProcessedEvent("benchmark-event"),
                "restart lost processed-event state");
    }
    return Json{{"durable_records", records},
                {"reopen_and_verify_ms", elapsedSeconds(started) * 1000.0}};
}

Json settlementBenchmark()
{
    constexpr std::size_t bonds = 30000;
    BondService service;
    const auto started = Clock::now();
    for (std::size_t index = 0; index < bonds; ++index) {
        const auto id = "bond-" + std::to_string(index);
        service.lock(BondRecord{id, "message-" + std::to_string(index), "sender", "owner",
                                "sink", "policy", 25, 2000, std::nullopt});
        const auto result = service.settle(id, SettlementOutcome::RefundAccepted);
        require(result.destination == "sender" && result.amount == 25 && !result.duplicate,
                "local settlement changed destination or amount");
    }
    const auto seconds = elapsedSeconds(started);
    require(service.size() == bonds, "bond benchmark lost records");
    return Json{{"bonds", bonds},
                {"lock_and_settle_operations_per_second", (bonds * 2) / seconds}};
}

std::int64_t peakRssKib()
{
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        throw std::runtime_error("getrusage failed");
    }
    return usage.ru_maxrss;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2) {
            throw std::runtime_error("usage: local-workloads <temporary-database-path>");
        }
        const std::filesystem::path database_path(argv[1]);
        const Json report{{"schema_version", 1},
                          {"queue", queueBenchmark()},
                          {"spam_burst", spamBenchmark()},
                          {"attachment_round_trip", attachmentBenchmark()},
                          {"a2a_concurrency", a2aBenchmark()},
                          {"reconnect_recovery", recoveryBenchmark(database_path)},
                          {"settlement", settlementBenchmark()},
                          {"peak_rss_kib", peakRssKib()}};
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
