#include "runtime/skill_registry.h"

namespace bonded {

void SkillRegistry::registerSkill(SkillDefinition definition)
{
    if (definition.name.empty() || !definition.handler) {
        throw DomainError("skill name and handler are required");
    }
    if (!skills_.emplace(definition.name, std::move(definition)).second) {
        throw DomainError("duplicate skill registration");
    }
}

bool SkillRegistry::has(const std::string& name, Profile profile) const
{
    const auto found = skills_.find(name);
    return found != skills_.end() && found->second.profiles.contains(profile);
}

Json SkillRegistry::invoke(const std::string& name, Profile profile, const Json& input) const
{
    const auto found = skills_.find(name);
    if (found == skills_.end()) {
        throw DomainError("unknown skill: " + name);
    }
    if (!found->second.profiles.contains(profile)) {
        throw DomainError("skill is not allowed by profile: " + name);
    }
    return found->second.handler(input);
}

Json SkillRegistry::manifest(Profile profile) const
{
    Json result = Json::array();
    for (const auto& [name, skill] : skills_) {
        if (!skill.profiles.contains(profile)) {
            continue;
        }
        result.push_back(Json{{"name", name},
                              {"description", skill.description},
                              {"input_schema", skill.input_schema},
                              {"output_schema", skill.output_schema}});
    }
    return result;
}

std::size_t SkillRegistry::size() const
{
    return skills_.size();
}

} // namespace bonded
