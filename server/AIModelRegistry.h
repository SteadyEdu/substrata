/*=====================================================================
AIModelRegistry.h
-----------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#pragma once


#include <ai/LLMClient.h>
#include <string>
#include <vector>
class ServerConfig;


/*=====================================================================
AIModelRegistry
---------------
The set of AI models a chatbot on this server can be pointed at.

That is the models built in to glare-core (getBuiltInAIModels()) plus any listed
in the <ai_models> section of the server config, with a config entry replacing a
built-in one of the same id.

Having this in the config rather than in the code is what makes it possible to
point a bot at a model running on the school's own network - one fine-tuned for a
particular grade or subject, say - without needing a new build of the server.
=====================================================================*/
namespace AIModelRegistry
{

// All available models: the built-in ones first, in their original order, then any config-only ones.
std::vector<AIModel> getAvailableModels(const ServerConfig& config);

// Look up a single model by its id_string.  Returns false if there is no model with that id.
bool getModelForID(const ServerConfig& config, const std::string& model_id, AIModel& model_out);

void test();

} // end namespace AIModelRegistry
