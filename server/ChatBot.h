/*=====================================================================
ChatBot.h
---------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#pragma once


#include "../shared/UserID.h"
#include "../shared/WorldMaterial.h"
#include "../shared/Avatar.h"
#include "../shared/WorldStateLock.h"
#include <ai/LLMThreadUser.h>
#include <TimeStamp.h>
#include <ThreadSafeRefCounted.h>
#include <WeakRefCounted.h>
#include <Reference.h>
#include <Timer.h>
#include <OutStream.h>
#include <SocketBufferOutStream.h>
#include <InStream.h>
#include <DatabaseKey.h>
#include <map>


class RandomAccessInStream;
class LLMThread;
class Server;
class ServerWorldState;
class ToolFunctionCall;


class ChatBotToolFunction : public ThreadSafeRefCounted
{
public:
	void writeToStream(RandomAccessOutStream& stream);

	static const int MAX_FUNCTION_NAME_SIZE = 1'000;
	static const int MAX_DESCRIPTION_NAME_SIZE = 10'000;
	static const int MAX_RESULT_CONTENT_SIZE = 100'000;

	std::string function_name;
	std::string description;
	std::string result_content;
};

void readChatBotToolFunctionFromStream(RandomAccessInStream& stream, ChatBotToolFunction& func);



/*=====================================================================
ChatBot
-------
A chatbot that uses LLMs for thinking.
=====================================================================*/
class ChatBot : public LLMThreadUser
{
public:
	ChatBot();
	~ChatBot();

	

	struct EventHandlerResults
	{
		Reference<LLMThread> new_llm_thread;
	};

	[[nodiscard]] EventHandlerResults userMovedNearToBotAvatar(AvatarRef other_avatar, Server* server, WorldStateLock& lock);
	[[nodiscard]] EventHandlerResults userMovedAwayFromBotAvatar(AvatarRef other_avatar, Server* server, WorldStateLock& lock);

	// A user/avatar sent a chat message, which this bot is in range of.
	// Threadsafe, will be called from WorkerThreads but only when the world lock is held.
	[[nodiscard]] EventHandlerResults processHeardChatMessage(const std::string& msg, AvatarRef sender_avatar, const std::string& avatar_name, Server* server, uint32 client_capabilities, WorldStateLock& lock);

	// Handle some (partial, streaming) chat data coming back from an LLM cloud server.
	// Called from the main thread only, in server.cpp.
	void handleLLMChatResponse(const std::string& msg, Server* server, WorldStateLock& world_lock);

	// Called from the main thread only, in server.cpp.
	void handleLLMChatResponseDone(Server* server, WorldStateLock& world_lock);

	// Called from the main thread only, in server.cpp.
	void handleLLMToolFunctionCall(const std::vector<Reference<ToolFunctionCall>>& calls, Server* server, WorldStateLock& world_lock);

	// Called from the main thread only, in server.cpp.
	struct ThinkResults
	{
		Reference<LLMThread> llm_thread_being_killed;
		Reference<LLMThread> new_llm_thread;
	};
	ThinkResults think(Server* server, WorldStateLock& world_lock);

	void writeToStream(RandomAccessOutStream& stream);





	uint64 id;

	UserID owner_id;
	TimeStamp created_time;

	static const int MAX_NAME_SIZE = 200;
	std::string name;

	AvatarSettings avatar_settings;

	// Bits in 'flags'.
	// PRIVATE_CONVERSATION_FLAG: the bot converses with one user at a time, and its replies are sent only to that
	// user rather than broadcast to the whole world.
	static const uint32 PRIVATE_CONVERSATION_FLAG = 1;

	// DISABLE_GESTURE_TOOLS_FLAG: do not offer the built-in wave and bow tool functions to the model.
	// Small models - the ones a school is most likely to be able to run on its own hardware - tend to answer a tool
	// call *instead of* speaking, so a bot given these tools can end up waving at a student and saying nothing.
	// The flag is "disable" rather than "enable" so that bots created before it existed keep their gestures.
	static const uint32 DISABLE_GESTURE_TOOLS_FLAG = 2;

	uint32 flags;

	bool isPrivateConversationBot() const { return (flags & PRIVATE_CONVERSATION_FLAG) != 0; }
	bool gestureToolsEnabled() const { return (flags & DISABLE_GESTURE_TOOLS_FLAG) == 0; }

	// Should a chat message from the given avatar be kept out of world chat, because it is part of this bot's
	// private conversation?  Called by WorkerThread before it decides how to deliver the user's own message.
	bool capturesChatFrom(UID sender_avatar_uid) const;

	Vec3d pos;
	float heading;

	static const int MAX_CUSTOM_PROMPT_PART_SIZE = 10000;
	std::string custom_prompt_part;

	// id_string of the AI model this bot thinks with, e.g. "anthropic/claude-opus-5" or a model defined in the
	// <ai_models> section of the server config.  Empty means use the server-wide default (ServerConfig::AI_model_id),
	// which is also what every bot created before this field existed gets.
	// Per-bot rather than per-server so a sixth-grade maths tutor and a high-school chemistry tutor can run on
	// different models - including one served from the school's own network.
	static const int MAX_MODEL_ID_SIZE = 200;
	std::string model_id;

	// Set when a setting that is baked into the LLM thread at creation - the model, the prompt, the tool functions -
	// has been changed.  think() then drops the thread so the next turn picks the new setting up.
	// Without this a change only takes effect once the thread dies of inactivity, which can be minutes away or never
	// while someone is talking to the bot: a teacher switches model, sees no difference, and concludes it did not work.
	bool llm_thread_needs_restart = false;

	std::map<std::string, Reference<ChatBotToolFunction>> info_tool_functions; // Map from function name to ChatBotToolFunction ref.  Tool functions that the LLM can call.

	DatabaseKey database_key;

	Reference<LLMThread> llm_thread; // Thread that does the communication with the LLM cloud server.

	ServerWorldState* world; // world the chatbot is in.

	UID avatar_uid; // UID of the avatar of the chatbot.
	Reference<Avatar> avatar; // avatar of the chatbot.

	// Information about an avatar that is or was nearby the chatbot.
	struct OtherAvatarInfo
	{
		OtherAvatarInfo() : conversing(false) {}
		Timer attention_timer; // How long the other avatar has been staring at the chatbot avatar.
		Timer time_since_last_greeted_other_av;
		Timer time_since_farewelled_other_av;
		bool conversing; // Are we in a conversation with this other avatar.
	};
	std::map<Reference<Avatar>, OtherAvatarInfo> other_avatar_info;
	Reference<const Avatar> look_target_avatar; // Avatar we should look at, may be null if not chatting with anyone.

	// When PRIVATE_CONVERSATION_FLAG is set, the avatar this bot is currently in a private conversation with,
	// or an invalid UID if it is free.  Runtime-only state; not serialised.
	UID private_partner_avatar_uid;
	std::string private_partner_avatar_name; // Kept alongside the UID so transcripts name the user, not just a number.

private:
	void sendChatMessageToClients(const string_view message, Server* server, WorldStateLock& world_lock); // Send message to clients if non-empty

	// The single point through which every chatbot utterance leaves the server, and so the place to hook
	// transcript logging.  If target_avatar_uid is valid, the message is sent only to that avatar's client and is
	// marked private; otherwise it is broadcast to the whole world as before.
	void sendChatMessagePacket(const string_view message, UID target_avatar_uid, Server* server, WorldStateLock& world_lock);

	// Write one bot utterance to the server's transcript log.  target_avatar_uid is invalid for messages spoken to
	// the whole world.
	void logSpokenMessage(const string_view message, UID target_avatar_uid, Server* server);
	Reference<LLMThread> createLLMThread(Server* server);

	// The response from the LLM is streamed back from the cloud server, however we only want to chat in complete sentences, not in fragments of sentences.  So we will scan the accumulated response for sentence ends.
	size_t body_start_index;// Index into total_llm_response.  Index at which the first of the current sentences' body starts.  Will be > 0 if after e.g. a [SPEAK] prefix.
	size_t next_sentence_start_index; // Index into total_llm_response.  Index at which the next sentence starts, e.g. 1 place past end of last sentence.
	size_t next_sentence_search_pos; // Current search position in total_llm_response for a sentence end.
	std::string total_llm_response;
	bool response_has_speak_prefix; // Did the chatbot want to speak this message by emitting the "[SPEAK]" prefix? (as opposed to staying silent for this message)?
	bool processed_first_response_data; // Have we received at least strlen("[SPEAK]") chars in the response, and looked for "[SPEAK]"?

	Timer sentences_received_timer; // Once we have received a complete sentence from the LLM server, start this timer.  When it has completed, send any queued sentences.

	Timer repeating_gesture_timer; // This will be unpaused and reset when a repeating gesture such as waving starts.  When it hits some elapsed time, we will stop the gesture.

	Timer time_since_last_LLM_activity; // Time since we sent a message or received a message rom the LLM server.  After this hits some threshold, kill the LLM thread.

	SocketBufferOutStream scratch_packet;
};


typedef Reference<ChatBot> ChatBotRef;


void readChatBotFromStream(RandomAccessInStream& stream, ChatBot& chatbot);


struct ChatBotRefHash
{
	size_t operator() (const ChatBotRef& ob) const
	{
		return (size_t)ob.ptr() >> 3; // Assuming 8-byte aligned, get rid of lower zero bits.
	}
};
