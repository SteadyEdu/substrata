/*=====================================================================
ServerConfig.h
--------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#pragma once

#include <string>
#include <map>
#include <vector>


/*=====================================================================
AIModelConfig
-------------
One entry of the <ai_models> section of the server config, describing a model a
chatbot can be pointed at.

This exists so an operator can add models - in particular ones served from their
own network, which no built-in list could know about - by editing the config,
rather than needing a new build.  A school running a model fine-tuned for, say,
sixth-grade maths on a machine in the building is the motivating case.

An entry whose id matches a built-in model replaces it.
=====================================================================*/
struct AIModelConfig
{
	AIModelConfig() : port(-1) {}

	std::string id;          // Stable id stored on each chatbot, e.g. "local/llama-3.1-8b-instruct".
	std::string name;        // Shown to the user when choosing a model.  Defaults to id if not given.
	std::string description; // Shown to the user when choosing a model.  Optional.
	std::string api_id;      // The model id passed to the API itself, e.g. "llama3.1:8b".  Defaults to id if not given.

	std::string scheme;      // "https" (default) or "http".  Local model servers usually do not use TLS.
	std::string domain;      // e.g. "api.anthropic.com", "localhost".
	int port;                // -1 (default) = the default port for the scheme.
	std::string path;        // e.g. "/v1/chat/completions".

	// Name of the entry in the server credentials file holding this API's key.  Leave empty for an endpoint that
	// needs no key, which is the usual case for a locally hosted model.
	std::string credential_name;

	// "anthropic", "xai", "openai", "google", or "other".  Picks the request format.  Anthropic has its own; every
	// other value uses the OpenAI-compatible format, which is what local model servers speak.
	std::string provider;
};


class ServerConfig
{
public:
	ServerConfig() : allow_light_mapper_bot_full_perms(false), update_parcel_sales(false), do_lua_http_request_rate_limiting(true), enable_LOD_chunking(true), enable_registration(true), enable_mcp_server(true), do_mcp_rate_limiting(true),
		log_chat_transcripts(true), chat_transcript_retention_days(0) {}
	
	std::string webserver_fragments_dir; // empty string = use default.
	std::string webserver_public_files_dir; // empty string = use default.
	std::string webclient_dir; // empty string = use default.

	std::string tls_certificate_path; // empty string = use default.
	std::string tls_private_key_path; // empty string = use default.
	
	bool allow_light_mapper_bot_full_perms; // Allow lightmapper bot (User account with name "lightmapperbot") to have full write permissions.

	bool update_parcel_sales; // Should we run auctions?

	bool do_lua_http_request_rate_limiting; // Should we rate-limit HTTP requests made by Lua scripts?

	bool enable_LOD_chunking; // Should we generate LOD chunks?

	bool enable_registration; // Should we allow new users to register?

	bool enable_mcp_server; // Should the MCP (Model Context Protocol) server endpoint at /mcp be enabled?  Requests are authenticated with a per-user API key; world-mutation tools act as the key's owner, subject to that user's permissions.

	bool do_mcp_rate_limiting; // Should we rate-limit requests to the MCP endpoint (per API-key owner)?

	std::string AI_model_id; // Default value = "xai/grok-4.5".  Used by a chatbot that has not been given its own model.

	// Extra models, from the <ai_models> section of the config.  These are offered alongside the models built in to
	// glare-core (see getBuiltInAIModels()), and an entry here with the same id replaces the built-in one.
	std::vector<AIModelConfig> ai_models;

	// Where chatbot conversation transcripts are written.  Empty string = use the default (server_state_dir +
	// "/chat_transcripts").  Set log_chat_transcripts to false to turn transcript logging off entirely, which should
	// only be done on a server that no minors use: a private conversation with an AI that nobody can review afterwards
	// is not something to ship to a school.
	bool log_chat_transcripts;
	std::string chat_transcript_dir;
	int chat_transcript_retention_days; // 0 = keep transcripts forever, which is the default.
	std::string shared_LLM_prompt_part; // Default value = "You are a helpful bot in the Substrata Metaverse." etc..  See parseServerConfig in server.cpp for the default.
};


struct ServerCredentials
{
	std::map<std::string, std::string> creds;
};
