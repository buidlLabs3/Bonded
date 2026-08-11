#pragma once

#include "domain/types.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace bonded {

enum class TriageDisposition { AutoAccept, OwnerReview, DeterministicReject };

struct ClassifierResult {
    double score{0.0};
    std::vector<std::string> reason_codes;
    std::string model;
    std::string version;
    bool timed_out{false};
};

struct TriageInput {
    std::string message_id;
    std::string content;
    bool trusted_contact{false};
    bool explicit_policy_match{false};
    std::vector<std::string> deterministic_violations;
};

struct TriageDecision {
    TriageDisposition disposition{TriageDisposition::OwnerReview};
    std::vector<std::string> evidence;
    ClassifierResult classifier;
};

struct OwnerRejection {
    std::string message_id;
    std::string policy_hash;
    std::string sink_address;
    std::uint64_t timestamp_unix{0};
    std::string owner_public_key;
    std::string signature;
};

class LocalClassifier {
public:
    virtual ~LocalClassifier() = default;
    virtual ClassifierResult classify(const std::string& content,
                                      std::uint64_t timeout_ms) = 0;
};

class KeywordClassifier final : public LocalClassifier {
public:
    ClassifierResult classify(const std::string& content, std::uint64_t timeout_ms) override;
};

class TriageService {
public:
    explicit TriageService(std::unique_ptr<LocalClassifier> classifier = nullptr,
                           std::uint64_t timeout_ms = 1000);
    TriageDecision evaluate(const TriageInput& input) const;

    static std::string canonicalOwnerRejection(const OwnerRejection& rejection);
    static bool verifyOwnerRejection(const OwnerRejection& rejection);
    static const std::set<std::string>& deterministicReasons();

private:
    std::unique_ptr<LocalClassifier> classifier_;
    std::uint64_t timeout_ms_;
};

std::string toString(TriageDisposition disposition);
void to_json(Json& json, const ClassifierResult& result);
void to_json(Json& json, const TriageDecision& decision);

} // namespace bonded
