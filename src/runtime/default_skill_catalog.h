#pragma once

#include "domain/types.h"

#include <set>
#include <string>
#include <vector>

namespace bonded {

struct DefaultSkillSpec {
    std::string name;
    std::string description;
    std::set<Profile> profiles;
};

const std::vector<DefaultSkillSpec>& allDefaultSkillSpecs();
std::set<std::string> requiredDefaultSkillNames();

} // namespace bonded
