#include "services/triage_service.h"

#include "security/crypto.h"

#include <algorithm>
#include <cctype>

namespace bonded {

ClassifierResult KeywordClassifier::classify(const std::string& content,
                                             std::uint64_t timeout_ms)
{
    if (timeout_ms == 0) {
        return {0.0, {"classifier_timeout"}, "keyword-local", "1", true};
    }
    std::string normalized = content;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) { return std::tolower(character); });
    const bool suspicious = normalized.find("guaranteed profit") != std::string::npos ||
                            normalized.find("seed phrase") != std::string::npos;
    return {suspicious ? 0.95 : 0.05,
            {suspicious ? "suspicious_language" : "no_local_signal"},
            "keyword-local",
            "1",
            false};
}

TriageService::TriageService(std::unique_ptr<LocalClassifier> classifier,
                             std::uint64_t timeout_ms)
    : classifier_(std::move(classifier)), timeout_ms_(timeout_ms)
{
    if (timeout_ms_ == 0) {
        throw DomainError("classifier timeout must be positive");
    }
}

const std::set<std::string>& TriageService::deterministicReasons()
{
    static const std::set<std::string> reasons{"attachment_too_large",
                                                "attachment_type_denied",
                                                "invalid_policy_commitment",
                                                "rate_limit_exceeded",
                                                "underfunded_bond"};
    return reasons;
}

TriageDecision TriageService::evaluate(const TriageInput& input) const
{
    if (input.message_id.empty()) {
        throw DomainError("triage message id is required");
    }

    std::vector<std::string> reproducible;
    for (const auto& violation : input.deterministic_violations) {
        if (deterministicReasons().contains(violation)) {
            reproducible.push_back(violation);
        }
    }
    if (!reproducible.empty()) {
        return {TriageDisposition::DeterministicReject, reproducible, {}};
    }
    if (input.trusted_contact || input.explicit_policy_match) {
        return {TriageDisposition::AutoAccept,
                {input.trusted_contact ? "authenticated_trusted_contact"
                                       : "explicit_policy_match"},
                {}};
    }
    if (!classifier_) {
        return {TriageDisposition::OwnerReview, {"classifier_unavailable"}, {}};
    }
    try {
        auto result = classifier_->classify(input.content, timeout_ms_);
        if (result.timed_out) {
            return {TriageDisposition::OwnerReview, {"classifier_timeout"}, std::move(result)};
        }
        return {TriageDisposition::OwnerReview, {"model_output_is_advisory"}, std::move(result)};
    } catch (const std::exception&) {
        return {TriageDisposition::OwnerReview, {"classifier_error"}, {}};
    }
}

std::string TriageService::canonicalOwnerRejection(const OwnerRejection& rejection)
{
    return Json{{"protocol", "bonded-inbox/owner-rejection/v1"},
                {"message_id", rejection.message_id},
                {"policy_hash", rejection.policy_hash},
                {"sink_address", rejection.sink_address},
                {"timestamp_unix", rejection.timestamp_unix},
                {"owner_public_key", rejection.owner_public_key}}
        .dump();
}

bool TriageService::verifyOwnerRejection(const OwnerRejection& rejection)
{
    return !rejection.message_id.empty() && !rejection.policy_hash.empty() &&
           !rejection.sink_address.empty() && rejection.timestamp_unix > 0 &&
           !rejection.owner_public_key.empty() &&
           Crypto::verifyEd25519(rejection.owner_public_key,
                                 canonicalOwnerRejection(rejection), rejection.signature);
}

std::string toString(TriageDisposition disposition)
{
    switch (disposition) {
    case TriageDisposition::AutoAccept:
        return "auto_accept";
    case TriageDisposition::OwnerReview:
        return "owner_review";
    case TriageDisposition::DeterministicReject:
        return "deterministic_reject";
    }
    throw DomainError("unknown triage disposition");
}

void to_json(Json& json, const ClassifierResult& result)
{
    json = Json{{"score", result.score},
                {"reason_codes", result.reason_codes},
                {"model", result.model},
                {"version", result.version},
                {"timed_out", result.timed_out}};
}

void to_json(Json& json, const TriageDecision& decision)
{
    json = Json{{"disposition", toString(decision.disposition)},
                {"evidence", decision.evidence},
                {"classifier", decision.classifier}};
}

} // namespace bonded
