#include "storage/database.h"

#include <sqlite3.h>

#include <limits>
#include <memory>

namespace bonded {
namespace {

class Statement {
public:
    Statement(sqlite3* database, const char* sql)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw DomainError(sqlite3_errmsg(database));
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() const { return statement_; }

private:
    sqlite3_stmt* statement_{nullptr};
};

void bindText(sqlite3_stmt* statement, int index, const std::string& value)
{
    if (sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        throw DomainError("cannot bind database text value");
    }
}

std::string columnText(sqlite3_stmt* statement, int index)
{
    const auto* value = sqlite3_column_text(statement, index);
    return value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
}

void bindUnsigned(sqlite3_stmt* statement, int index, std::uint64_t value,
                  const char* field)
{
    if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        throw DomainError(std::string(field) + " exceeds the SQLite integer range");
    }
    if (sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
        throw DomainError(std::string("cannot bind database ") + field);
    }
}

} // namespace

Database::Database(const std::filesystem::path& path)
{
    if (sqlite3_open_v2(path.string().c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                                         SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string error = db_ == nullptr ? "cannot open database" : sqlite3_errmsg(db_);
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw DomainError(error);
    }
    execute("PRAGMA foreign_keys = ON");
    execute("PRAGMA journal_mode = WAL");
    execute("PRAGMA synchronous = FULL");
    sqlite3_busy_timeout(db_, 5000);
}

Database::~Database()
{
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void Database::execute(const std::string& sql) const
{
    char* error = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error == nullptr ? "database execution failed" : error;
        sqlite3_free(error);
        throw DomainError(message);
    }
}

void Database::migrate()
{
    execute("BEGIN IMMEDIATE");
    try {
        execute("CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL)");
        execute("INSERT INTO schema_version(version) SELECT 1 WHERE NOT EXISTS "
                "(SELECT 1 FROM schema_version)");
        execute("CREATE TABLE IF NOT EXISTS policies("
                "inbox_id TEXT NOT NULL, version INTEGER NOT NULL, document TEXT NOT NULL, "
                "PRIMARY KEY(inbox_id, version))");
        execute("CREATE TABLE IF NOT EXISTS messages("
                "id TEXT PRIMARY KEY, idempotency_key TEXT NOT NULL UNIQUE, document TEXT NOT NULL, "
                "revision INTEGER NOT NULL)");
        execute("CREATE TABLE IF NOT EXISTS bonds("
                "id TEXT PRIMARY KEY, document TEXT NOT NULL, outcome TEXT)");
        execute("CREATE TABLE IF NOT EXISTS outbox("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, topic TEXT NOT NULL, payload TEXT NOT NULL, "
                "attempts INTEGER NOT NULL DEFAULT 0, acknowledged INTEGER NOT NULL DEFAULT 0)");
        execute("CREATE TABLE IF NOT EXISTS outbox_deduplication("
                "deduplication_key TEXT PRIMARY KEY, outbox_id INTEGER NOT NULL UNIQUE, "
                "FOREIGN KEY(outbox_id) REFERENCES outbox(id))");
        execute("CREATE TABLE IF NOT EXISTS processed_events("
                "event_id TEXT PRIMARY KEY, processed_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
        execute("CREATE TABLE IF NOT EXISTS wallet_transfers("
                "id TEXT PRIMARY KEY, recipient TEXT NOT NULL, amount INTEGER NOT NULL, "
                "timestamp_unix INTEGER NOT NULL)");
        execute("CREATE TABLE IF NOT EXISTS runtime_records("
                "kind TEXT NOT NULL, id TEXT NOT NULL, document TEXT NOT NULL, "
                "PRIMARY KEY(kind, id))");
        execute("UPDATE schema_version SET version = 4 WHERE version < 4");
        execute("COMMIT");
    } catch (...) {
        execute("ROLLBACK");
        throw;
    }
}

void Database::savePolicy(const InboxPolicy& policy)
{
    Statement statement(db_, "INSERT INTO policies(inbox_id, version, document) VALUES(?, ?, ?)");
    bindText(statement.get(), 1, policy.inbox_id);
    bindUnsigned(statement.get(), 2, policy.version, "policy version");
    bindText(statement.get(), 3, Json(policy).dump());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
}

std::optional<InboxPolicy> Database::latestPolicy(const std::string& inbox_id) const
{
    Statement statement(db_, "SELECT document FROM policies WHERE inbox_id = ? "
                             "ORDER BY version DESC LIMIT 1");
    bindText(statement.get(), 1, inbox_id);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        return Json::parse(columnText(statement.get(), 0)).get<InboxPolicy>();
    }
    return std::nullopt;
}

bool Database::createMessage(const MessageRecord& message, const std::string& idempotency_key)
{
    Statement statement(db_, "INSERT OR IGNORE INTO messages(id, idempotency_key, document, revision) "
                             "VALUES(?, ?, ?, ?)");
    bindText(statement.get(), 1, message.id);
    bindText(statement.get(), 2, idempotency_key);
    bindText(statement.get(), 3, Json(message).dump());
    bindUnsigned(statement.get(), 4, message.revision, "message revision");
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
    return sqlite3_changes(db_) == 1;
}

std::optional<MessageRecord> Database::message(const std::string& message_id) const
{
    Statement statement(db_, "SELECT document FROM messages WHERE id = ?");
    bindText(statement.get(), 1, message_id);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        return Json::parse(columnText(statement.get(), 0)).get<MessageRecord>();
    }
    return std::nullopt;
}

std::optional<MessageRecord> Database::message(const std::string& message_id,
                                               const std::string& idempotency_key) const
{
    Statement statement(db_, "SELECT document FROM messages "
                             "WHERE id = ? AND idempotency_key = ?");
    bindText(statement.get(), 1, message_id);
    bindText(statement.get(), 2, idempotency_key);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        return Json::parse(columnText(statement.get(), 0)).get<MessageRecord>();
    }
    return std::nullopt;
}

std::vector<MessageRecord> Database::messages() const
{
    Statement statement(db_, "SELECT document FROM messages ORDER BY id");
    std::vector<MessageRecord> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back(Json::parse(columnText(statement.get(), 0)).get<MessageRecord>());
    }
    return result;
}

void Database::updateMessage(const MessageRecord& message, std::uint64_t previous_revision)
{
    Statement statement(db_, "UPDATE messages SET document = ?, revision = ? "
                             "WHERE id = ? AND revision = ?");
    bindText(statement.get(), 1, Json(message).dump());
    bindUnsigned(statement.get(), 2, message.revision, "message revision");
    bindText(statement.get(), 3, message.id);
    bindUnsigned(statement.get(), 4, previous_revision, "previous message revision");
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
    if (sqlite3_changes(db_) != 1) {
        throw DomainError("message update revision conflict");
    }
}

bool Database::createBond(const BondRecord& bond)
{
    Statement statement(db_, "INSERT OR IGNORE INTO bonds(id, document, outcome) VALUES(?, ?, ?)");
    bindText(statement.get(), 1, bond.id);
    bindText(statement.get(), 2, Json(bond).dump());
    if (bond.outcome.has_value()) {
        bindText(statement.get(), 3, toString(*bond.outcome));
    } else {
        sqlite3_bind_null(statement.get(), 3);
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
    return sqlite3_changes(db_) == 1;
}

std::optional<BondRecord> Database::bond(const std::string& bond_id) const
{
    Statement statement(db_, "SELECT document FROM bonds WHERE id = ?");
    bindText(statement.get(), 1, bond_id);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        return Json::parse(columnText(statement.get(), 0)).get<BondRecord>();
    }
    return std::nullopt;
}

bool Database::settleBond(const BondRecord& bond)
{
    if (!bond.outcome.has_value()) {
        throw DomainError("bond settlement outcome is required");
    }
    Statement statement(db_, "UPDATE bonds SET document = ?, outcome = ? "
                             "WHERE id = ? AND outcome IS NULL");
    bindText(statement.get(), 1, Json(bond).dump());
    bindText(statement.get(), 2, toString(*bond.outcome));
    bindText(statement.get(), 3, bond.id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
    return sqlite3_changes(db_) == 1;
}

std::size_t Database::bondCount() const
{
    Statement statement(db_, "SELECT COUNT(*) FROM bonds");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw DomainError(sqlite3_errmsg(db_));
    }
    return static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));
}

void Database::enqueue(const std::string& topic, const std::string& payload)
{
    Statement statement(db_, "INSERT INTO outbox(topic, payload) VALUES(?, ?)");
    bindText(statement.get(), 1, topic);
    bindText(statement.get(), 2, payload);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
}

bool Database::enqueueOnce(const std::string& deduplication_key, const std::string& topic,
                           const std::string& payload)
{
    if (deduplication_key.empty()) {
        throw DomainError("outbox deduplication key is required");
    }
    execute("BEGIN IMMEDIATE");
    try {
        Statement existing(db_, "SELECT 1 FROM outbox_deduplication "
                                "WHERE deduplication_key = ?");
        bindText(existing.get(), 1, deduplication_key);
        if (sqlite3_step(existing.get()) == SQLITE_ROW) {
            execute("COMMIT");
            return false;
        }

        Statement outbox(db_, "INSERT INTO outbox(topic, payload) VALUES(?, ?)");
        bindText(outbox.get(), 1, topic);
        bindText(outbox.get(), 2, payload);
        if (sqlite3_step(outbox.get()) != SQLITE_DONE) {
            throw DomainError(sqlite3_errmsg(db_));
        }
        const auto outbox_id = sqlite3_last_insert_rowid(db_);

        Statement marker(db_, "INSERT INTO outbox_deduplication(deduplication_key, outbox_id) "
                              "VALUES(?, ?)");
        bindText(marker.get(), 1, deduplication_key);
        sqlite3_bind_int64(marker.get(), 2, outbox_id);
        if (sqlite3_step(marker.get()) != SQLITE_DONE) {
            throw DomainError(sqlite3_errmsg(db_));
        }
        execute("COMMIT");
        return true;
    } catch (...) {
        execute("ROLLBACK");
        throw;
    }
}

std::vector<OutboxRecord> Database::pendingOutbox(std::size_t limit) const
{
    Statement statement(db_, "SELECT id, topic, payload, attempts FROM outbox "
                             "WHERE acknowledged = 0 ORDER BY id LIMIT ?");
    bindUnsigned(statement.get(), 1, limit, "outbox limit");
    std::vector<OutboxRecord> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back({sqlite3_column_int64(statement.get(), 0), columnText(statement.get(), 1),
                          columnText(statement.get(), 2),
                          static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 3))});
    }
    return result;
}

void Database::acknowledgeOutbox(std::int64_t id)
{
    Statement statement(db_, "UPDATE outbox SET acknowledged = 1 WHERE id = ?");
    sqlite3_bind_int64(statement.get(), 1, id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
}

void Database::recordProcessedEvent(const std::string& event_id)
{
    Statement statement(db_, "INSERT OR IGNORE INTO processed_events(event_id) VALUES(?)");
    bindText(statement.get(), 1, event_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
}

bool Database::hasProcessedEvent(const std::string& event_id) const
{
    Statement statement(db_, "SELECT 1 FROM processed_events WHERE event_id = ?");
    bindText(statement.get(), 1, event_id);
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

void Database::recordWalletTransfer(const WalletTransfer& transfer)
{
    if (transfer.id.empty() || transfer.recipient.empty() || transfer.amount == 0) {
        throw DomainError("wallet transfer record is invalid");
    }
    Statement statement(
        db_, "INSERT OR IGNORE INTO wallet_transfers(id, recipient, amount, timestamp_unix) "
             "VALUES(?, ?, ?, ?)");
    bindText(statement.get(), 1, transfer.id);
    bindText(statement.get(), 2, transfer.recipient);
    bindUnsigned(statement.get(), 3, transfer.amount, "wallet amount");
    bindUnsigned(statement.get(), 4, transfer.timestamp_unix, "wallet timestamp");
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
}

std::vector<WalletTransfer> Database::walletHistory() const
{
    Statement statement(db_, "SELECT id, recipient, amount, timestamp_unix "
                             "FROM wallet_transfers ORDER BY timestamp_unix, id");
    std::vector<WalletTransfer> transfers;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        transfers.push_back(
            {columnText(statement.get(), 0), columnText(statement.get(), 1),
             static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 2)),
             static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 3))});
    }
    return transfers;
}

void Database::upsertRuntimeRecord(const std::string& kind, const std::string& id,
                                   const Json& document)
{
    if (kind.empty() || id.empty() || !document.is_object()) {
        throw DomainError("runtime record kind, id, and object document are required");
    }
    Statement statement(
        db_, "INSERT INTO runtime_records(kind, id, document) VALUES(?, ?, ?) "
             "ON CONFLICT(kind, id) DO UPDATE SET document = excluded.document");
    bindText(statement.get(), 1, kind);
    bindText(statement.get(), 2, id);
    bindText(statement.get(), 3, document.dump());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
}

std::vector<Json> Database::runtimeRecords(const std::string& kind) const
{
    if (kind.empty()) {
        throw DomainError("runtime record kind is required");
    }
    Statement statement(db_, "SELECT document FROM runtime_records WHERE kind = ? ORDER BY id");
    bindText(statement.get(), 1, kind);
    std::vector<Json> records;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        records.push_back(Json::parse(columnText(statement.get(), 0)));
    }
    return records;
}

} // namespace bonded
