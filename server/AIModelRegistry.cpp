/*=====================================================================
AIModelRegistry.cpp
-------------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#include "AIModelRegistry.h"


#include "ServerConfig.h"
#include <StringUtils.h>
#include <ConPrint.h>
#include <TestUtils.h>


namespace AIModelRegistry
{


static AIModel::Provider providerForString(const std::string& s)
{
	// Anything that is not Anthropic uses the OpenAI-compatible request format, which is also what locally hosted
	// model servers (Ollama, llama.cpp, vLLM, LM Studio) speak, so Provider_Other is a safe default.
	if(StringUtils::equalCaseInsensitive(s, "anthropic"))
		return AIModel::Provider_Anthropic;
	else if(StringUtils::equalCaseInsensitive(s, "xai"))
		return AIModel::Provider_XAI;
	else if(StringUtils::equalCaseInsensitive(s, "openai"))
		return AIModel::Provider_OpenAI;
	else if(StringUtils::equalCaseInsensitive(s, "google"))
		return AIModel::Provider_Google;
	else
		return AIModel::Provider_Other;
}


static AIModel modelForConfig(const AIModelConfig& config_model)
{
	AIModel model;
	model.id_string     = config_model.id;
	model.api_id_string = config_model.api_id.empty() ? config_model.id : config_model.api_id;
	model.name          = config_model.name.empty() ? config_model.id : config_model.name;
	model.description   = config_model.description;

	model.api_scheme = config_model.scheme.empty() ? std::string("https") : config_model.scheme;
	model.api_domain = config_model.domain;
	model.api_port   = config_model.port;
	model.api_path   = config_model.path;

	model.api_key_credential_name = config_model.credential_name; // Empty means the endpoint needs no API key.

	model.provider = providerForString(config_model.provider);

	return model;
}


static bool reasoningEffortForString(const std::string& s, LLMClient::ReasoningEffort& effort_out)
{
	if(StringUtils::equalCaseInsensitive(s, "none"))       effort_out = LLMClient::ReasoningEffort_none;
	else if(StringUtils::equalCaseInsensitive(s, "low"))   effort_out = LLMClient::ReasoningEffort_low;
	else if(StringUtils::equalCaseInsensitive(s, "med"))   effort_out = LLMClient::ReasoningEffort_med;
	else if(StringUtils::equalCaseInsensitive(s, "high"))  effort_out = LLMClient::ReasoningEffort_high;
	else if(StringUtils::equalCaseInsensitive(s, "xhigh")) effort_out = LLMClient::ReasoningEffort_xhigh;
	else if(StringUtils::equalCaseInsensitive(s, "max"))   effort_out = LLMClient::ReasoningEffort_max;
	else return false;

	return true;
}


LLMClient::ReasoningEffort getReasoningEffortForID(const ServerConfig& config, const std::string& model_id,
	LLMClient::ReasoningEffort default_effort)
{
	for(size_t i=0; i<config.ai_models.size(); ++i)
		if(config.ai_models[i].id == model_id && !config.ai_models[i].reasoning_effort.empty())
		{
			LLMClient::ReasoningEffort effort;
			if(reasoningEffortForString(config.ai_models[i].reasoning_effort, effort))
				return effort;

			conPrint("AIModelRegistry: ignoring unknown reasoning_effort '" + config.ai_models[i].reasoning_effort +
				"' for model '" + model_id + "'.");
			break;
		}

	return default_effort;
}


std::vector<AIModel> getAvailableModels(const ServerConfig& config)
{
	std::vector<AIModel> models = getBuiltInAIModels();

	for(size_t i=0; i<config.ai_models.size(); ++i)
	{
		if(config.ai_models[i].id.empty()) // A model with no id can't be referred to by a chatbot, so ignore it.
			continue;

		const AIModel model = modelForConfig(config.ai_models[i]);

		// A config entry with the same id as a built-in model replaces it, so an operator can redirect a model to
		// their own endpoint without the built-in definition silently winning.
		bool replaced = false;
		for(size_t z=0; z<models.size(); ++z)
			if(models[z].id_string == model.id_string)
			{
				models[z] = model;
				replaced = true;
				break;
			}

		if(!replaced)
			models.push_back(model);
	}

	return models;
}


bool getModelForID(const ServerConfig& config, const std::string& model_id, AIModel& model_out)
{
	if(model_id.empty())
		return false;

	const std::vector<AIModel> models = getAvailableModels(config);
	for(size_t i=0; i<models.size(); ++i)
		if(models[i].id_string == model_id)
		{
			model_out = models[i];
			return true;
		}

	return false;
}



#if BUILD_TESTS


void test()
{
	conPrint("AIModelRegistry::test()");

	// A config model with a new id is added to the built-in ones.
	{
		ServerConfig config;
		AIModelConfig local;
		local.id     = "local/llama-3.1-8b-instruct";
		local.name   = "Llama 3.1 8B (local)";
		local.api_id = "llama3.1:8b";
		local.scheme = "http";
		local.domain = "localhost";
		local.port   = 11434;
		local.path   = "/v1/chat/completions";
		config.ai_models.push_back(local);

		const std::vector<AIModel> models = getAvailableModels(config);
		testAssert(models.size() == getBuiltInAIModels().size() + 1);

		AIModel model;
		testAssert(getModelForID(config, "local/llama-3.1-8b-instruct", model));
		testAssert(model.api_id_string == "llama3.1:8b");
		testAssert(model.name == "Llama 3.1 8B (local)");
		testAssert(model.provider == AIModel::Provider_Other); // Not Anthropic, so the OpenAI-compatible format.

		// A local model needs no API key, and must be reached over plain HTTP on its own port.
		testAssert(model.api_key_credential_name.empty());
		testStringsEqual(model.apiURL(), "http://localhost:11434/v1/chat/completions");
	}

	// A config model whose id matches a built-in one replaces it rather than being added alongside.
	{
		ServerConfig config;
		AIModelConfig override_model;
		override_model.id              = "anthropic/claude-opus-5";
		override_model.name            = "Claude Opus 5 (our account)";
		override_model.api_id          = "claude-opus-5";
		override_model.domain          = "api.anthropic.com";
		override_model.path            = "/v1/messages";
		override_model.credential_name = "anthropic_api_key";
		override_model.provider        = "anthropic";
		config.ai_models.push_back(override_model);

		const std::vector<AIModel> models = getAvailableModels(config);
		testAssert(models.size() == getBuiltInAIModels().size()); // Replaced, not appended.

		AIModel model;
		testAssert(getModelForID(config, "anthropic/claude-opus-5", model));
		testAssert(model.name == "Claude Opus 5 (our account)");
		testAssert(model.provider == AIModel::Provider_Anthropic);

		// Default scheme and port, so no port in the URL.
		testStringsEqual(model.apiURL(), "https://api.anthropic.com/v1/messages");
	}

	// Built-in models are still reachable when the config lists none.
	{
		ServerConfig config;
		testAssert(getAvailableModels(config).size() == getBuiltInAIModels().size());

		AIModel model;
		testAssert(getModelForID(config, "xai/grok-4.5", model));
		testAssert(model.provider == AIModel::Provider_XAI);
	}

	// Unknown and empty ids are not resolved, so the caller falls back to the built-in lookup by id.
	{
		ServerConfig config;
		AIModel model;
		testAssert(!getModelForID(config, "no/such-model", model));
		testAssert(!getModelForID(config, "", model));
	}

	// An entry with no id cannot be referred to by a chatbot, so it is ignored rather than added.
	{
		ServerConfig config;
		AIModelConfig bad;
		bad.domain = "localhost";
		config.ai_models.push_back(bad);

		testAssert(getAvailableModels(config).size() == getBuiltInAIModels().size());
	}

	// name and api_id default to the id when not given.
	{
		ServerConfig config;
		AIModelConfig minimal;
		minimal.id     = "local/minimal";
		minimal.scheme = "http";
		minimal.domain = "127.0.0.1";
		minimal.port   = 8080;
		minimal.path   = "/v1/chat/completions";
		config.ai_models.push_back(minimal);

		AIModel model;
		testAssert(getModelForID(config, "local/minimal", model));
		testAssert(model.name == "local/minimal");
		testAssert(model.api_id_string == "local/minimal");
		testStringsEqual(model.apiURL(), "http://127.0.0.1:8080/v1/chat/completions");
	}

	conPrint("AIModelRegistry::test() done.");
}


#endif // BUILD_TESTS


} // end namespace AIModelRegistry
