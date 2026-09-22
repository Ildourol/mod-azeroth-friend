#ifndef MOD_AZEROTH_FRIEND_ACTION_REGISTRY_H
#define MOD_AZEROTH_FRIEND_ACTION_REGISTRY_H

#include "Common.h"
#include <string>
#include <vector>

// Single source of truth for the companion's curated action vocabulary.
// The LLM only ever sees (and the executor only ever accepts) these names, plus
// the validated raw passthrough actions.
namespace AzerothFriendActionRegistry
{
    // Bot API v1 authority is assigned by the bridge from the triggering event;
    // it is never accepted from model-authored params_json.
    enum class ActionAuthority : uint8
    {
        Autonomous = 0,
        OwnerCommand = 1,
        GmManual = 2
    };

    struct ActionInfo
    {
        std::string name;
        std::string category;
        std::string params;
        std::string description;
        std::string playerbotAction;
        bool passthrough;
        uint8 apiVersion = 1;
        std::string paramsJsonSchema;
        std::string resultJsonSchema;
        std::string bindingKind;
        std::string authority;
        std::string preconditions;
        std::string completionPolicy;
    };

    std::vector<ActionInfo> const& Curated();
    ActionInfo const* Find(std::string const& name);
    bool IsCurated(std::string const& name);
    ActionAuthority RequiredAuthority(std::string const& name);
    char const* AuthorityName(ActionAuthority authority);
    bool IsAuthorized(std::string const& name, std::string const& grantedAuthority,
                      uint32 originSourceGuid, uint32 masterGuid);

    // Compact, human readable catalogue used by `.af catalog` and by the prompt
    // generator in the Python bridge.
    std::string CuratedJson();
    std::string CuratedPromptList();
}

#endif // MOD_AZEROTH_FRIEND_ACTION_REGISTRY_H
