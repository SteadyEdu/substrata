/*=====================================================================
ChatBotHandlers.cpp
-------------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#include "ChatBotHandlers.h"


#include "RequestInfo.h"
#include "Response.h"
#include "WebDataStore.h"
#include "WebsiteExcep.h"
#include "Escaping.h"
#include "WorldHandlers.h"
#include "ResponseUtils.h"
#include "WebServerResponseUtils.h"
#include "LoginHandlers.h"
#include "../server/ServerWorldState.h"
#include <ConPrint.h>
#include <Exception.h>
#include <Lock.h>
#include <StringUtils.h>
#include <BitUtils.h>
#include "../server/AIModelRegistry.h"
#include <PlatformUtils.h>
#include <Parser.h>
#include <MemMappedFile.h>
#include <functional>


namespace ChatBotHandlers
{


void renderEditChatBotPage(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	try
	{
		const int chatbot_id = request.getURLIntParam("chatbot_id");

		std::string page = WebServerResponseUtils::standardHeader(world_state, request, "Edit ChatBot");
		page += "<div class=\"main\">   \n";

		{ // Lock scope

			WorldStateLock lock(world_state.mutex);

			const User* logged_in_user = LoginHandlers::getLoggedInUser(world_state, request);
			if(!logged_in_user)
				throw glare::Exception("You must be logged in to view this page.");


			const std::string msg = world_state.getAndRemoveUserWebMessage(logged_in_user->id);
			if(!msg.empty())
				page += "<div class=\"msg\">" + web::Escaping::HTMLEscape(msg) + "</div>  \n";


			// Lookup chatbot
			// Look through all worlds.  NOTE: slow
			for(auto world_it = world_state.world_states.begin(); world_it != world_state.world_states.end(); ++world_it)
			{
				ServerWorldState* world = world_it->second.ptr();
				const auto res = world->getChatBots(lock).find((uint64)chatbot_id);
				if(res != world->getChatBots(lock).end())
				{
					const ChatBot* chatbot = res->second.ptr();
					if((chatbot->owner_id == logged_in_user->id) || isGodUser(logged_in_user->id))
					{
						page += "<div class=\"grouped-region\">";
						page += "<form action=\"/edit_chatbot_post\" method=\"post\" id=\"usrform\" class=\"full-width\" >";
						page += "<input type=\"hidden\" name=\"chatbot_id\" value=\"" + toString(chatbot_id) + "\">";

						page += "<div class=\"form-field\">";
						page += "<label for=\"name\">Name:</label><br/>";
						page += "<input type=\"text\" id=\"name\" name=\"name\" value=\"" + web::Escaping::HTMLEscape(chatbot->name) + "\">";
						page += "</div>";


						page += std::string("<div>Position:<br/> ") + 
							"x: <input type=\"number\" step=\"any\" name=\"pos_x\" value=\"" + toString(chatbot->pos.x) + "\"> " + 
							"y: <input type=\"number\" step=\"any\" name=\"pos_y\" value=\"" + toString(chatbot->pos.y) + "\"> " + 
							"z: <input type=\"number\" step=\"any\" name=\"pos_z\" value=\"" + toString(chatbot->pos.z) + "\"> " + 
							"<div class=\"field-description\">Eye position of chatbot avatar.  Z=up.</div></div>";

						page += "<br/>";
						page += "<div>Heading:<br/> <input type=\"number\" step=\"any\" name=\"heading\" value=\"" + toString(chatbot->heading) + "\">" + 
							"<div class=\"field-description\">Default facing direction.  Counter-clockwise rotation in radians from looking in positive x direction.  0 = look east.   1.57 = look north.  3.14 = look west.  4.71 = look south. </div></div>";

						

						page += "<p>Built-in prompt part:</p>";
						page += "<p>" + web::Escaping::HTMLEscape(world_state.server_config.shared_LLM_prompt_part) + "</p>";

						page += "<div class=\"form-field\">";
						page += "<label for=\"base_prompt\">Custom prompt part:</label><br/>";
						page += "<textarea rows=\"20\" class=\"full-width\" id=\"base_prompt\" name=\"base_prompt\">" + web::Escaping::HTMLEscape(chatbot->custom_prompt_part) + "</textarea>";
						page += "<div class=\"field-description\">Max 10,000 characters</div>";
						page += "</div>";

						// Model picker.  A bot with no model of its own follows the server-wide default, which is the first option.
						page += "<div class=\"form-field\">";
						page += "<label for=\"model_id\">Model:</label><br/>";
						page += "<select id=\"model_id\" name=\"model_id\">";
						page += std::string("<option value=\"\"") + (chatbot->model_id.empty() ? " selected" : "") + ">Server default (" +
							web::Escaping::HTMLEscape(world_state.server_config.AI_model_id) + ")</option>";

						{
							const std::vector<AIModel> models = AIModelRegistry::getAvailableModels(world_state.server_config);
							bool found_current_model = chatbot->model_id.empty();
							for(size_t i=0; i<models.size(); ++i)
							{
								const bool selected = (models[i].id_string == chatbot->model_id);
								found_current_model = found_current_model || selected;
								page += "<option value=\"" + web::Escaping::HTMLEscape(models[i].id_string) + "\"" + (selected ? " selected" : "") + ">" +
									web::Escaping::HTMLEscape(models[i].name.empty() ? models[i].id_string : models[i].name) + "</option>";
							}

							// The bot refers to a model that is no longer configured.  Keep it in the list rather than silently
							// resetting the bot to the default the next time someone saves this page.
							if(!found_current_model)
								page += "<option value=\"" + web::Escaping::HTMLEscape(chatbot->model_id) + "\" selected>" +
									web::Escaping::HTMLEscape(chatbot->model_id) + " (not configured on this server)</option>";
						}

						page += "</select>";
						page += "<div class=\"field-description\">Which AI model this bot thinks with.  Models are listed in the &lt;ai_models&gt; "
							"section of the server config, which can include a model running on your own network.</div>";
						page += "</div>";

						page += "<div class=\"form-field\">";
						page += std::string("Allow gesture tools: <input type=\"checkbox\" name=\"gesture_tools\" value=\"checked\" ") + (chatbot->gestureToolsEnabled() ? "checked" : "") + ">";
						page += "<div class=\"field-description\">Lets the bot wave and bow.  Turn this off for a bot running on a small model: "
							"such models tend to answer with a gesture <i>instead of</i> speaking, so the bot waves at the student and says nothing.</div>";
						page += "</div>";

						page += "<div class=\"form-field\">";
						page += std::string("Private conversations: <input type=\"checkbox\" name=\"private_conversation\" value=\"checked\" ") + (chatbot->isPrivateConversationBot() ? "checked" : "") + ">";
						page += "<div class=\"field-description\">If ticked, this bot talks to one person at a time, and what each of them says to it is not shown in world chat.  "
							"Use this for tutoring, where a student should be able to ask a question without the rest of the room seeing it.</div>";
						page += "</div>";

						page += "<input type=\"submit\" value=\"Save Changes\">";
						page += "</form>";
						page += "</div>"; // End grouped-region div


						page += "<h3>Tool Info Functions</h3>";
						page += "Use these to allow the LLM to access more information when it needs to.";



						for(auto it = chatbot->info_tool_functions.begin(); it != chatbot->info_tool_functions.end(); ++it)
						{
							const ChatBotToolFunction* func = it->second.ptr();
							
							page += "<div class=\"grouped-region\">";
							page += "<form action=\"/update_info_tool_function_post\" method=\"post\" class=\"full-width\">";
							page += "<input type=\"hidden\" name=\"chatbot_id\" value=\"" + toString(chatbot_id) + "\">";
							page += "<input type=\"hidden\" name=\"cur_function_name\" value=\"" + web::Escaping::HTMLEscape(func->function_name) + "\">";
							
							page += "<div class=\"form-field\">";
							page += "Function name:<br/>";
							page += "<input type=\"text\" name=\"new_function_name\" value=\"" + web::Escaping::HTMLEscape(func->function_name) + "\"><br>";
							page += "</div>";
	
							page += "<div class=\"form-field\">";
							page += "Description:<br/>";
							page += "<textarea rows=\"2\" class=\"full-width\" name=\"description\">" + web::Escaping::HTMLEscape(func->description) + "</textarea>";
							page += "</div>";

							page += "<div class=\"form-field\">";
							page += "Result content:<br/>";
							page += "<textarea rows=\"10\" class=\"full-width\" name=\"result_content\">" + web::Escaping::HTMLEscape(func->result_content) + "</textarea>";
							page += "<div class=\"field-description\">Max 100,000 characters</div>";
							page += "</div>";

							page += "<input type=\"submit\" value=\"Save Changes to Function\">";
							page += "</form>";

							// Add 'delete function' button
							page += "<form action=\"/delete_info_tool_function_post\" method=\"post\">";
							page += "<input type=\"hidden\" name=\"chatbot_id\" value=\"" + toString(chatbot_id) + "\">";
							page += "<input type=\"hidden\" name=\"function_name\" value=\"" + web::Escaping::HTMLEscape(func->function_name) + "\">";
							page += "<input type=\"submit\" value=\"Delete function\">";
							page += "</form>";
							page += "</div>"; // End grouped-region div
						}

						// Add 'add new function' button
						page += "<form action=\"/add_new_info_tool_function_post\" method=\"post\">";
						page += "<input type=\"hidden\" name=\"chatbot_id\" value=\"" + toString(chatbot_id) + "\">";
						page += "Function Name: <input type=\"text\" name=\"function_name\" value=\"\"><br>";
						page += "<input type=\"submit\" value=\"Add new function\">";
						page += "</form>";


						page += "<h3>Avatar Settings</h3>";

						page += "<div class=\"grouped-region\">";
						page += "<div>Avatar settings model URL: " + web::Escaping::HTMLEscape(toString(chatbot->avatar_settings.model_url)) + "</div>";

						page += "<form action=\"/copy_user_avatar_settings_post\" method=\"post\">";
						page += "<input type=\"hidden\" name=\"chatbot_id\" value=\"" + toString(chatbot_id) + "\">";
						page += "<input type=\"submit\" value=\"Copy user's current avatar settings for ChatBot\">";
						page += "</form>";
						page += "</div>"; // End grouped-region div


						page += "<h3>Misc Settings</h3>";
						page += "<div>World: " + (world_it->first.empty() ? "Main World" : web::Escaping::HTMLEscape(world_it->first)) + "</div>";
						page += "<div>Chatbot created: " + chatbot->created_time.dayString() + "</div>";

						page += "<h3>Delete ChatBot</h3>";
						page += "<div class=\"danger-zone\">";
						page += "<form action=\"/delete_chatbot_post\" method=\"post\">";
						page += "<input type=\"hidden\" name=\"chatbot_id\" value=\"" + toString(chatbot_id) + "\">";
						page += "<input type=\"submit\" class=\"delete-chatbot\" value=\"Delete Chatbot\">";
						page += "</form>";
						page += "</div>"; // End grouped-region div
					}
					else
						throw glare::Exception("You must be the owner of this chatbot to view this page.");
				}
			}
		} // End lock scope

		page += "</div>   \n"; // end main div

		page += "<script src=\"/files/chatbot.js\"></script>";

		page += WebServerResponseUtils::standardFooter(request, true);

		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, page);
	}
	catch(glare::Exception& e)
	{
		if(!request.fuzzing)
			conPrint("handleChatBotPageRequest error: " + e.what());
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Error: " + e.what());
	}
}


void renderNewChatBotPage(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	try
	{
		std::string page = WebServerResponseUtils::standardHeader(world_state, request, "New ChatBot");
		page += "<div class=\"main\">   \n";

		web::UnsafeString world = request.getURLParam("world");

		Vec3d pos = Vec3d(0.0, 0.0, 1.67);
		if(request.isURLParamPresent("pos_x"))
		{
			pos.x = request.getURLDoubleParam("pos_x");
			pos.y = request.getURLDoubleParam("pos_y");
			pos.z = request.getURLDoubleParam("pos_z");
		}

		{ // Lock scope

			WorldStateLock lock(world_state.mutex);

			const User* logged_in_user = LoginHandlers::getLoggedInUser(world_state, request);
			if(!logged_in_user)
				throw glare::Exception("You must be logged in to view this page.");


			const std::string msg = world_state.getAndRemoveUserWebMessage(logged_in_user->id);
			if(!msg.empty())
				page += "<div class=\"msg\">" + web::Escaping::HTMLEscape(msg) + "</div>  \n";


			page += "<div class=\"grouped-region\">";
			page += "<form action=\"/create_new_chatbot_post\" method=\"post\" id=\"usrform\" class=\"full-width\" >";


			page += "<div class=\"form-field\">";
			page += "<label for=\"world\">World:  (leave empty to create in main world)</label><br/>";
			page += "<input type=\"text\" id=\"world\" name=\"world\" value=\"" + web::Escaping::HTMLEscape(world.str()) + "\">";
			page += "</div>";

			page += std::string("<div>Position:<br/> ") + 
				"x: <input type=\"number\" step=\"any\" name=\"pos_x\" value=\"" + toString(pos.x) + "\"> " + 
				"y: <input type=\"number\" step=\"any\" name=\"pos_y\" value=\"" + toString(pos.y) + "\"> " + 
				"z: <input type=\"number\" step=\"any\" name=\"pos_z\" value=\"" + toString(pos.z) + "\"> </div>";

				
			page += "<input type=\"submit\" value=\"Create ChatBot\">";
			page += "</form>";
			page += "</div>"; // End grouped-region div

		} // End lock scope

		page += "</div>   \n"; // end main div
		page += WebServerResponseUtils::standardFooter(request, true);

		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, page);
	}
	catch(glare::Exception& e)
	{
		if(!request.fuzzing)
			conPrint("renderNewChatBotPage error: " + e.what());
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Error: " + e.what());
	}
}


// Adapted from workerthread
static bool positionIsInParcelForWhichLoggedInUserHasWritePerms(const Vec3d& pos, const UserID& user_id, ServerWorldState& world_state, WorldStateLock& lock)
{
	assert(user_id.valid());

	const Vec4f ob_pos = pos.toVec4fPoint();

	ServerWorldState::ParcelMapType& parcels = world_state.getParcels(lock);
	for(ServerWorldState::ParcelMapType::iterator it = parcels.begin(); it != parcels.end(); ++it)
	{
		const Parcel* parcel = it->second.ptr();
		if(parcel->pointInParcel(ob_pos) && parcel->userHasWritePerms(user_id))
			return true;
	}

	return false;
}


void handleNewChatBotPost(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	try
	{
		if(world_state.isInReadOnlyMode())
			throw glare::Exception("Server is in read-only mode, editing disabled currently.");

		const web::UnsafeString world_name = request.getPostField("world");

		Vec3d pos;
		pos.x = request.getPostDoubleField("pos_x");
		pos.y = request.getPostDoubleField("pos_y");
		pos.z = request.getPostDoubleField("pos_z");

		bool successfully_created = false;
		uint64 chatbot_id = 0;

		{ // Lock scope

			WorldStateLock lock(world_state.mutex);


			const User* logged_in_user = LoginHandlers::getLoggedInUser(world_state, request);
			if(!logged_in_user)
				throw glare::Exception("You must be logged in to view this page.");

			// Look up the world
			auto res = world_state.world_states.find(world_name.str());
			if(res == world_state.world_states.end())
			{
				world_state.setUserWebMessage(logged_in_user->id, "Could not find the world '" + world_name.str() + "'");
			}
			else
			{
				ServerWorldState* world = res->second.ptr();

				// Check the new position is valid - must be in a world or parcel owned by user.
				if((world->details.owner_id == logged_in_user->id) || // If the user owns this world, or
					positionIsInParcelForWhichLoggedInUserHasWritePerms(pos, logged_in_user->id, *world, lock)) // chatbot is placed in a parcel the user has write permissions for:
				{
					// Create the ChatBot
					ChatBotRef chatbot = new ChatBot();
					chatbot->id = world_state.getNextChatBotUID();
					chatbot->owner_id = logged_in_user->id;
					chatbot->created_time = TimeStamp::currentTime();
					chatbot->name = "New ChatBot";
					chatbot->pos = pos;
					chatbot->heading = 0;
					chatbot->world = world;

					// Insert into world
					world->getChatBots(lock)[chatbot->id] = chatbot;


					// Create avatar for the chatbot
					AvatarRef avatar = world->createAndInsertAvatarForChatBot(&world_state, chatbot.ptr(), lock);
					chatbot->avatar_uid = avatar->uid;
					chatbot->avatar = avatar;


					world->addChatBotAsDBDirty(chatbot, lock);

					world_state.markAsChanged();

					world_state.setUserWebMessage(logged_in_user->id, "Created chatbot.");

					successfully_created = true;
					chatbot_id = chatbot->id;
				}
				else
				{
					world_state.setUserWebMessage(logged_in_user->id, "Position must be in a parcel you have write permission for, or in a world you own.");
				}
			}
		} // End lock scope

		if(successfully_created)
			web::ResponseUtils::writeRedirectTo(reply_info, "/edit_chatbot?chatbot_id=" + toString(chatbot_id)); // redirect to edit page.
		else
			web::ResponseUtils::writeRedirectTo(reply_info, "/new_chatbot?world=" + web::Escaping::URLEscape(world_name.str()) + 
				"&pos_x=" + toString(pos.x) + "&pos_y=" + toString(pos.y) + "&pos_z=" + toString(pos.z));  // redirect back to new chatbot page.
	}
	catch(glare::Exception& e)
	{
		if(!request.fuzzing)
			conPrint("handleNewChatBotPost error: " + e.what());
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Error: " + e.what());
	}
}


void handleEditChatBotPost(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	try
	{
		if(world_state.isInReadOnlyMode())
			throw glare::Exception("Server is in read-only mode, editing disabled currently.");

		const int chatbot_id = request.getPostIntField("chatbot_id");
		const web::UnsafeString new_name = request.getPostField("name");
		const web::UnsafeString new_base_prompt = request.getPostField("base_prompt");

		Vec3d new_pos;
		new_pos.x = request.getPostDoubleField("pos_x");
		new_pos.y = request.getPostDoubleField("pos_y");
		new_pos.z = request.getPostDoubleField("pos_z");

		const double new_heading = request.getPostDoubleField("heading");

		const bool new_private_conversation = request.getPostField("private_conversation") == "checked";
		const bool new_gesture_tools = request.getPostField("gesture_tools") == "checked";
		const web::UnsafeString new_model_id = request.getPostField("model_id");

		{ // Lock scope

			WorldStateLock lock(world_state.mutex);


			const User* logged_in_user = LoginHandlers::getLoggedInUser(world_state, request);
			if(!logged_in_user)
				throw glare::Exception("You must be logged in to view this page.");


			// Lookup chatbot
			// Look through all worlds.  NOTE: slow
			for(auto it = world_state.world_states.begin(); it != world_state.world_states.end(); ++it)
			{
				ServerWorldState* world = it->second.ptr();
				const auto res = world->getChatBots(lock).find((uint64)chatbot_id);
				if(res != world->getChatBots(lock).end())
				{
					ChatBot* chatbot = res->second.ptr();
					if((chatbot->owner_id == logged_in_user->id) || isGodUser(logged_in_user->id))
					{
						// Check the new position is valid - must be in a world or parcel owned by user.
						if((world->details.owner_id == logged_in_user->id) || // If the user owns this world, or
							positionIsInParcelForWhichLoggedInUserHasWritePerms(new_pos, logged_in_user->id, *world, lock)) // chatbot is placed in a parcel the user has write permissions for:
						{
							chatbot->name = new_name.str();
							if(chatbot->name.size() > ChatBot::MAX_NAME_SIZE)
							{
								chatbot->name = chatbot->name.substr(0, ChatBot::MAX_NAME_SIZE);
								world_state.setUserWebMessage(logged_in_user->id, "Name exceeded max length of " + toString(ChatBot::MAX_NAME_SIZE) + " chars, truncated.");
							}

							chatbot->custom_prompt_part = new_base_prompt.str();
							if(chatbot->custom_prompt_part.size() > ChatBot::MAX_CUSTOM_PROMPT_PART_SIZE)
							{
								chatbot->custom_prompt_part = chatbot->custom_prompt_part.substr(0, ChatBot::MAX_CUSTOM_PROMPT_PART_SIZE);
								world_state.setUserWebMessage(logged_in_user->id, "Prompt exceeded max length of " + toString(ChatBot::MAX_CUSTOM_PROMPT_PART_SIZE) + " chars, truncated.");
							}

							chatbot->pos = new_pos;
							chatbot->heading = (float)new_heading;

							BitUtils::setOrZeroBit(chatbot->flags, ChatBot::PRIVATE_CONVERSATION_FLAG, new_private_conversation);
							BitUtils::setOrZeroBit(chatbot->flags, ChatBot::DISABLE_GESTURE_TOOLS_FLAG, !new_gesture_tools);

							chatbot->model_id = new_model_id.str();
							if(chatbot->model_id.size() > ChatBot::MAX_MODEL_ID_SIZE)
								chatbot->model_id.clear(); // Not a value we offered; fall back to the server default.


							// Update the avatar's state
							if(chatbot->avatar)
							{
								chatbot->avatar->name = chatbot->name;
								chatbot->avatar->pos = new_pos;
								chatbot->avatar->rotation = Vec3f(0, Maths::pi_2<float>(), chatbot->heading);
								chatbot->avatar->other_dirty = true;
							}


							world->addChatBotAsDBDirty(chatbot, lock);

							world_state.markAsChanged();

							world_state.setUserWebMessage(logged_in_user->id, "Updated chatbot.");
						}
						else
						{
							world_state.setUserWebMessage(logged_in_user->id, "Position must be in a parcel you have write permission for, or in a world you own.");
						}
					}
					else
						throw glare::Exception("You must be the owner of this chatbot to edit it.");
				}
			}
		} // End lock scope

		web::ResponseUtils::writeRedirectTo(reply_info, "/edit_chatbot?chatbot_id=" + toString(chatbot_id));
	}
	catch(glare::Exception& e)
	{
		if(!request.fuzzing)
			conPrint("handleEditChatBotPost error: " + e.what());
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Error: " + e.what());
	}
}


void handleDeleteChatBotPost(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	try
	{
		if(world_state.isInReadOnlyMode())
			throw glare::Exception("Server is in read-only mode, editing disabled currently.");

		const int chatbot_id = request.getPostIntField("chatbot_id");
		
		{ // Lock scope

			WorldStateLock lock(world_state.mutex);

			const User* logged_in_user = LoginHandlers::getLoggedInUser(world_state, request);
			if(!logged_in_user)
				throw glare::Exception("You must be logged in to view this page.");

			// Lookup chatbot
			// Look through all worlds.  NOTE: slow
			for(auto it = world_state.world_states.begin(); it != world_state.world_states.end(); ++it)
			{
				ServerWorldState* world = it->second.ptr();
				const auto res = world->getChatBots(lock).find((uint64)chatbot_id);
				if(res != world->getChatBots(lock).end())
				{
					ChatBot* chatbot = res->second.ptr();
					if((chatbot->owner_id == logged_in_user->id) || isGodUser(logged_in_user->id))
					{
						// Remove avatar
						if(chatbot->avatar)
						{
							// Mark as dead so it will be removed in server.cpp main loop.
							chatbot->avatar->state = Avatar::State_Dead;
							chatbot->avatar->other_dirty = true;
						}


						// Remove from dirty-set, so it's not updated in DB.
						world->getDBDirtyChatBots(lock).erase(chatbot);

						// Add DB record to list of records to be deleted.
						world_state.db_records_to_delete.insert(chatbot->database_key);

						world->getChatBots(lock).erase(chatbot->id);

						world_state.markAsChanged();

						world_state.setUserWebMessage(logged_in_user->id, "Deleted chatbot.");
					}
					else
						throw glare::Exception("You must be the owner of this chatbot to edit it.");
				}
			}
		} // End lock scope

		web::ResponseUtils::writeRedirectTo(reply_info, "/account"); // Redirect back to user account page.
	}
	catch(glare::Exception& e)
	{
		if(!request.fuzzing)
			conPrint("handleDeleteChatBotPost error: " + e.what());
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Error: " + e.what());
	}
}


void handleCopyUserAvatarSettingsPost(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	try
	{
		if(world_state.isInReadOnlyMode())
			throw glare::Exception("Server is in read-only mode, editing disabled currently.");

		const int chatbot_id = request.getPostIntField("chatbot_id");

		{ // Lock scope

			WorldStateLock lock(world_state.mutex);

			const User* logged_in_user = LoginHandlers::getLoggedInUser(world_state, request);
			if(!logged_in_user)
				throw glare::Exception("You must be logged in to view this page.");

			// Lookup chatbot
			// Look through all worlds.  NOTE: slow
			for(auto it = world_state.world_states.begin(); it != world_state.world_states.end(); ++it)
			{
				ServerWorldState* world = it->second.ptr();
				const auto res = world->getChatBots(lock).find((uint64)chatbot_id);
				if(res != world->getChatBots(lock).end())
				{
					ChatBot* chatbot = res->second.ptr();
					if((chatbot->owner_id == logged_in_user->id) || isGodUser(logged_in_user->id))
					{
						chatbot->avatar_settings = logged_in_user->avatar_settings; // Copy the avatar settings

						// Update the avatar's state
						if(chatbot->avatar)
						{
							chatbot->avatar->avatar_settings = chatbot->avatar_settings;
							chatbot->avatar->other_dirty = true;
						}

						world->addChatBotAsDBDirty(chatbot, lock);
						world_state.markAsChanged();
						world_state.setUserWebMessage(logged_in_user->id, "Updated chatbot avatar settings.");
					}
					else
						throw glare::Exception("You must be the owner of this chatbot to edit it.");
				}
			}
		} // End lock scope

		web::ResponseUtils::writeRedirectTo(reply_info, "/edit_chatbot?chatbot_id=" + toString(chatbot_id));
	}
	catch(glare::Exception& e)
	{
		if(!request.fuzzing)
			conPrint("handleCopyUserAvatarSettingsPost error: " + e.what());
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Error: " + e.what());
	}
}


void handleUpdateInfoToolFunctionPost(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	try
	{
		if(world_state.isInReadOnlyMode())
			throw glare::Exception("Server is in read-only mode, editing disabled currently.");

		const int chatbot_id = request.getPostIntField("chatbot_id");
		const web::UnsafeString cur_function_name = request.getPostField("cur_function_name");
		const web::UnsafeString new_function_name = request.getPostField("new_function_name");
		const web::UnsafeString description = request.getPostField("description");
		const web::UnsafeString result_content = request.getPostField("result_content");

		{ // Lock scope

			WorldStateLock lock(world_state.mutex);

			const User* logged_in_user = LoginHandlers::getLoggedInUser(world_state, request);
			if(!logged_in_user)
				throw glare::Exception("You must be logged in to view this page.");

			// Lookup chatbot
			// Look through all worlds.  NOTE: slow
			for(auto it = world_state.world_states.begin(); it != world_state.world_states.end(); ++it)
			{
				ServerWorldState* world = it->second.ptr();
				const auto res = world->getChatBots(lock).find((uint64)chatbot_id);
				if(res != world->getChatBots(lock).end())
				{
					ChatBot* chatbot = res->second.ptr();
					if((chatbot->owner_id == logged_in_user->id) || isGodUser(logged_in_user->id))
					{
						auto func_res = chatbot->info_tool_functions.find(cur_function_name.str());
						if(func_res == chatbot->info_tool_functions.end())
							throw glare::Exception("Couldn't find function");

						Reference<ChatBotToolFunction> func = func_res->second;

						chatbot->info_tool_functions.erase(cur_function_name.str()); // Remove from map as name will change

						func->function_name = new_function_name.str();
						func->description = description.str();
						func->result_content = result_content.str();

						chatbot->info_tool_functions[func->function_name] = func; // re-insert into map using new name.


						world->addChatBotAsDBDirty(chatbot, lock);
						world_state.markAsChanged();
						world_state.setUserWebMessage(logged_in_user->id, "Updated info tool function.");
					}
					else
						throw glare::Exception("You must be the owner of this chatbot to edit it.");
				}
			}
		} // End lock scope

		web::ResponseUtils::writeRedirectTo(reply_info, "/edit_chatbot?chatbot_id=" + toString(chatbot_id));
	}
	catch(glare::Exception& e)
	{
		if(!request.fuzzing)
			conPrint("handleUpdateInfoToolFunctionPost error: " + e.what());
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Error: " + e.what());
	}
}


void handleDeleteInfoToolFunctionPost(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	try
	{
		if(world_state.isInReadOnlyMode())
			throw glare::Exception("Server is in read-only mode, editing disabled currently.");

		const int chatbot_id = request.getPostIntField("chatbot_id");
		const web::UnsafeString function_name = request.getPostField("function_name");

		{ // Lock scope

			WorldStateLock lock(world_state.mutex);

			const User* logged_in_user = LoginHandlers::getLoggedInUser(world_state, request);
			if(!logged_in_user)
				throw glare::Exception("You must be logged in to view this page.");

			// Lookup chatbot
			// Look through all worlds.  NOTE: slow
			for(auto it = world_state.world_states.begin(); it != world_state.world_states.end(); ++it)
			{
				ServerWorldState* world = it->second.ptr();
				const auto res = world->getChatBots(lock).find((uint64)chatbot_id);
				if(res != world->getChatBots(lock).end())
				{
					ChatBot* chatbot = res->second.ptr();
					if((chatbot->owner_id == logged_in_user->id) || isGodUser(logged_in_user->id))
					{
						chatbot->info_tool_functions.erase(function_name.str()); // Remove from map

						world->addChatBotAsDBDirty(chatbot, lock);
						world_state.markAsChanged();
						world_state.setUserWebMessage(logged_in_user->id, "Deleted info tool function.");
					}
					else
						throw glare::Exception("You must be the owner of this chatbot to edit it.");
				}
			}
		} // End lock scope

		web::ResponseUtils::writeRedirectTo(reply_info, "/edit_chatbot?chatbot_id=" + toString(chatbot_id));
	}
	catch(glare::Exception& e)
	{
		if(!request.fuzzing)
			conPrint("handleDeleteInfoToolFunctionPost error: " + e.what());
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Error: " + e.what());
	}
}


void handleAddNewInfoToolFunctionPost(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	try
	{
		if(world_state.isInReadOnlyMode())
			throw glare::Exception("Server is in read-only mode, editing disabled currently.");

		const int chatbot_id = request.getPostIntField("chatbot_id");
		const web::UnsafeString function_name = request.getPostField("function_name");

		{ // Lock scope

			WorldStateLock lock(world_state.mutex);

			const User* logged_in_user = LoginHandlers::getLoggedInUser(world_state, request);
			if(!logged_in_user)
				throw glare::Exception("You must be logged in to view this page.");

			// Lookup chatbot
			// Look through all worlds.  NOTE: slow
			for(auto it = world_state.world_states.begin(); it != world_state.world_states.end(); ++it)
			{
				ServerWorldState* world = it->second.ptr();
				const auto res = world->getChatBots(lock).find((uint64)chatbot_id);
				if(res != world->getChatBots(lock).end())
				{
					ChatBot* chatbot = res->second.ptr();
					if((chatbot->owner_id == logged_in_user->id) || isGodUser(logged_in_user->id))
					{
						Reference<ChatBotToolFunction> func = new ChatBotToolFunction();
						func->function_name = function_name.str();
						chatbot->info_tool_functions[func->function_name] = func;

						world->addChatBotAsDBDirty(chatbot, lock);
						world_state.markAsChanged();
						world_state.setUserWebMessage(logged_in_user->id, "Added new tool function.");
					}
					else
						throw glare::Exception("You must be the owner of this chatbot to edit it.");
				}
			}
		} // End lock scope

		web::ResponseUtils::writeRedirectTo(reply_info, "/edit_chatbot?chatbot_id=" + toString(chatbot_id));
	}
	catch(glare::Exception& e)
	{
		if(!request.fuzzing)
			conPrint("handleAddNewInfoToolFunctionPost error: " + e.what());
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Error: " + e.what());
	}
}


} // end namespace ChatBotHandlers
