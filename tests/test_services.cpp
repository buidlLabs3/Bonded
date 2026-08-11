#include "integrations/memory_adapters.h"
#include "security/crypto.h"
#include "services/contact_rules.h"
#include "services/messaging_service.h"
#include "services/receipt_service.h"
#include "services/spending_controller.h"
#include "services/storage_service.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using bonded::ApprovalState;
using bonded::ContactRules;
using bonded::Crypto;
using bonded::DomainError;
using bonded::Json;
using bonded::MemoryMessagingAdapter;
using bonded::MemoryProgramAdapter;
using bonded::MemoryStorageAdapter;
using bonded::MemoryWalletAdapter;
using bonded::MessagingService;
using bonded::ReceiptService;
using bonded::SettlementOutcome;
using bonded::SettlementReceipt;
using bonded::SignedEnvelope;
using bonded::SpendingController;
using bonded::SpendingPolicy;
using bonded::StorageService;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function> void expectDomainError(Function&& function, const std::string& message)
{
    try {
        function();
    } catch (const DomainError&) {
        return;
    }
    throw std::runtime_error(message);
}

void testMessaging()
{
    MemoryMessagingAdapter adapter;
    MessagingService messaging(adapter, "lez-devnet");
    const auto [private_key, public_key] = Crypto::generateEd25519KeyPair();
    SignedEnvelope envelope{"bonded-inbox/envelope/v1", "lez-devnet", "event-1", "owner",
                            "inbox", "owner/topic", "accept:message-1", Crypto::randomHex(16),
                            2000, "", ""};
    envelope = MessagingService::sign(envelope, private_key, public_key);
    check(MessagingService::verify(envelope, 1000, "lez-devnet"),
          "valid messaging envelope failed verification");

    std::size_t deliveries = 0;
    messaging.subscribe("owner/topic", 1000,
                        [&](const SignedEnvelope& received) {
                            check(received.payload == envelope.payload, "message payload changed");
                            ++deliveries;
                        });
    messaging.send(envelope);
    messaging.send(envelope);
    check(deliveries == 1, "duplicate messaging envelope was processed twice");

    auto tampered = envelope;
    tampered.payload = "reject:message-1";
    check(!MessagingService::verify(tampered, 1000, "lez-devnet"),
          "tampered messaging envelope verified");
    check(!MessagingService::verify(envelope, 2001, "lez-devnet"),
          "expired messaging envelope verified");

    adapter.join("group-1");
    check(!adapter.createGroup({"owner", "inbox"}).empty(), "messaging group was not created");
}

void testStorage()
{
    MemoryStorageAdapter adapter;
    StorageService storage(adapter);
    const auto key = Crypto::randomHex(32);
    const auto entry = storage.upload("classified attachment", "brief.txt", key);
    check(entry.address.starts_with("sha256:"), "storage address is not content-derived");
    check(storage.download(entry.address, key) == "classified attachment",
          "storage plaintext did not round trip");
    check(storage.list().size() == 1, "storage list omitted an upload");
    const auto grant = storage.share(entry.address, "processor", 2000, "wrapped-key-fixture");
    check(grant.recipient == "processor", "storage grant recipient changed");
    check(storage.canAccess(entry.address, "processor", 1999), "valid storage grant denied");
    check(!storage.canAccess(entry.address, "processor", 2001), "expired storage grant allowed");
    expectDomainError([&] { storage.download(entry.address, Crypto::randomHex(32)); },
                      "wrong storage key decrypted data");
}

void testSpending()
{
    MemoryWalletAdapter wallet(1000);
    SpendingController spending(wallet, SpendingPolicy{100, 250, 1000, 100});
    const auto automatic = spending.propose("translation-agent", 75, 1000);
    check(automatic.state == ApprovalState::Executed && wallet.balance() == 925,
          "below-limit spend was not autonomous");

    const auto pending = spending.propose("expensive-agent", 150, 1010);
    check(pending.state == ApprovalState::Pending && wallet.balance() == 925,
          "above-limit spend executed before approval");
    const auto approved = spending.approve(pending.id, 1020);
    check(approved.state == ApprovalState::Executed && wallet.balance() == 775,
          "approved spend did not execute once");
    check(spending.approve(pending.id, 1030).transfer_id == approved.transfer_id,
          "duplicate approval created another transfer");

    const auto expiring = spending.propose("offline-owner", 200, 1040);
    spending.expire(1141);
    check(spending.get(expiring.id)->state == ApprovalState::Expired,
          "unanswered approval did not expire");
    expectDomainError([&] { spending.approve(expiring.id, 1141); },
                      "expired approval executed");
}

void testReceipt()
{
    const auto [private_key, public_key] = Crypto::generateEd25519KeyPair();
    SettlementReceipt receipt{"bonded-inbox/receipt/v1",
                              "lez-devnet",
                              "message-1",
                              "bond:message-1",
                              "policy-hash",
                              "transaction-1",
                              Crypto::sha256("sender-address"),
                              25,
                              1200,
                              SettlementOutcome::RefundAccepted,
                              "",
                              ""};
    receipt = ReceiptService::sign(receipt, private_key, public_key);
    check(ReceiptService::verify(receipt), "valid private receipt failed verification");
    receipt.amount = 26;
    check(!ReceiptService::verify(receipt), "tampered receipt verified");
}

void testContactRules()
{
    ContactRules rules(2, 60);
    rules.trust("authenticated-contact");
    check(rules.isTrusted("authenticated-contact"), "trusted contact missing");
    rules.revoke("authenticated-contact");
    check(!rules.isTrusted("authenticated-contact"), "revoked contact remained trusted");
    check(rules.allow("sender-commitment", 1000), "first message was rate-limited");
    check(rules.allow("sender-commitment", 1001), "second message was rate-limited");
    check(!rules.allow("sender-commitment", 1002), "spam burst exceeded rate limit");
    check(rules.allow("sender-commitment", 1061), "rate window did not recover");
}

void testProgramAdapter()
{
    MemoryProgramAdapter programs;
    const auto program_id = programs.deploy("programs/bonded-inbox/Cargo.toml");
    const auto transaction = programs.call(program_id, "lock", Json{{"amount", 25}});
    check(!transaction.empty(), "program call returned no transaction id");
    check(programs.query(program_id, Json::object()).at("state").at("lock").at("amount") == 25,
          "program query did not return state");
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"messaging", testMessaging}, {"storage", testStorage},
        {"spending", testSpending},   {"receipt", testReceipt},
        {"contact rules", testContactRules}, {"program adapter", testProgramAdapter},
    };
    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size() << " service tests passed\n";
    return failures == 0 ? 0 : 1;
}
