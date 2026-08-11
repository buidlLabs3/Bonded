#pragma once

#include "domain/types.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bonded {

struct SkillDefinition {
    std::string name;
    std::string description;
    Json input_schema;
    Json output_schema;
    std::set<Profile> profiles;
    std::function<Json(const Json&)> handler;
};

class SkillRegistry {
public:
    void registerSkill(SkillDefinition definition);
    bool has(const std::string& name, Profile profile) const;
    Json invoke(const std::string& name, Profile profile, const Json& input) const;
    Json manifest(Profile profile) const;
    std::size_t size() const;

private:
    std::map<std::string, SkillDefinition> skills_;
};

} // namespace bonded
