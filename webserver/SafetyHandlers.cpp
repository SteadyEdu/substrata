/*=====================================================================
SafetyHandlers.cpp
------------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#include "SafetyHandlers.h"


#include "RequestInfo.h"
#include "Response.h"
#include "WebsiteExcep.h"
#include "Escaping.h"
#include "ResponseUtils.h"
#include "WebServerResponseUtils.h"
#include "LoginHandlers.h"
#include "../server/ServerWorldState.h"
#include "../server/ChatTranscriptLog.h"
#include "../server/SafetyClassifier.h"
#include <ConPrint.h>
#include <Exception.h>
#include <Lock.h>
#include <StringUtils.h>


namespace SafetyHandlers
{


// A user may review a conversation if they are the safeguarding lead, the server admin, or the owner of the chatbot.
// Chatbot ownership is standing in for "the teacher responsible for this tutor" until there is a proper role model;
// the safeguarding lead is a real role and sees everything, because a disclosure is not confined to the bot whose
// owner happens to be on duty.
static bool userMayReviewChatBot(ServerAllWorldsState& world_state, const User* logged_in_user, uint64 bot_id,
	WorldStateLock& lock)
{
	if(!logged_in_user)
		return false;

	if(isGodUser(logged_in_user->id) || logged_in_user->isSafeguardingLead())
		return true;

	for(auto world_it = world_state.world_states.begin(); world_it != world_state.world_states.end(); ++world_it)
	{
		ServerWorldState* world = world_it->second.ptr();
		const auto res = world->getChatBots(lock).find(bot_id);
		if(res != world->getChatBots(lock).end())
			return res->second->owner_id == logged_in_user->id;
	}

	return false; // A bot that no longer exists: only the admin can look at its transcripts.
}


static std::string categoriesString(const std::vector<std::string>& categories)
{
	std::string s;
	for(size_t i=0; i<categories.size(); ++i)
	{
		if(i > 0)
			s += ", ";
		s += web::Escaping::HTMLEscape(categories[i]);
	}
	return s;
}


void renderSafetyAlertsPage(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	try
	{
		std::string page;

		{ // Lock scope
			WorldStateLock lock(world_state.mutex);

			const User* logged_in_user = LoginHandlers::getLoggedInUser(world_state, request);
			if(!logged_in_user)
				throw glare::Exception("You must be logged in to view this page.");

			page = WebServerResponseUtils::standardHeader(world_state, request, /*page title=*/"Safety alerts", "");
			page += "<div class=\"main\">   \n";
			page += "<h2>Safety alerts</h2>";

			page += "<p>Messages from students to chatbots that the safety check flagged.  This check is a simple phrase "
				"match: it will miss things that are phrased unexpectedly, and it will sometimes flag something harmless.  "
				"Treat a row here as <i>worth reading</i>, not as a conclusion, and do not treat a quiet page as evidence "
				"that nothing has happened.</p>";

			if(logged_in_user->isSafeguardingLead())
				page += "<p>You are a safeguarding lead on this server, so you see alerts for every chatbot and are emailed "
					"when one is raised.</p>";
			else
				page += "<p>You see alerts for chatbots you own.</p>";

			const std::vector<ChatTranscriptLog::StoredRecord> alerts = world_state.chat_transcript_log ?
				world_state.chat_transcript_log->readRecentAlerts(200) : std::vector<ChatTranscriptLog::StoredRecord>();

			size_t num_shown = 0;
			page += "<table><tr><th>Time (UTC)</th><th>Student</th><th>Chatbot</th><th>Flagged as</th><th>Matched</th><th>Message</th><th></th></tr>";

			for(size_t i=0; i<alerts.size(); ++i)
			{
				const ChatTranscriptLog::StoredRecord& rec = alerts[i];

				if(!userMayReviewChatBot(world_state, logged_in_user, rec.bot_id, lock))
					continue; // Not this user's to read.

				num_shown++;

				page += std::string("<tr") + (rec.urgent ? " class=\"danger-zone\"" : "") + ">";
				page += "<td>" + web::Escaping::HTMLEscape(rec.time) + "</td>";
				page += "<td>" + web::Escaping::HTMLEscape(rec.avatar_name) + "</td>";
				page += "<td>" + web::Escaping::HTMLEscape(rec.bot_name) + "</td>";
				page += "<td>" + std::string(rec.urgent ? "<b>URGENT</b> " : "") + categoriesString(rec.safety_categories) + "</td>";
				page += "<td>" + web::Escaping::HTMLEscape(rec.matched_phrase) + "</td>";
				page += "<td>" + web::Escaping::HTMLEscape(rec.text) + "</td>";
				page += "<td><a href=\"/chat_transcript?bot_id=" + toString(rec.bot_id) + "&amp;avatar_uid=" +
					rec.avatar_uid.toString() + "\">conversation</a></td>";
				page += "</tr>";
			}

			page += "</table>";

			if(num_shown == 0)
				page += "<p>No flagged messages.</p>";

			page += "</div>   \n";
		} // End lock scope

		page += WebServerResponseUtils::standardFooter(request, true);
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, page);
	}
	catch(glare::Exception& e)
	{
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Error: " + web::Escaping::HTMLEscape(e.what()));
	}
}


void renderChatTranscriptPage(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	try
	{
		const uint64 bot_id = (uint64)request.getURLIntParam("bot_id");
		const UID avatar_uid((uint64)std::atoll(request.getURLParam("avatar_uid").str().c_str()));

		std::string page;

		{ // Lock scope
			WorldStateLock lock(world_state.mutex);

			const User* logged_in_user = LoginHandlers::getLoggedInUser(world_state, request);
			if(!logged_in_user)
				throw glare::Exception("You must be logged in to view this page.");

			if(!userMayReviewChatBot(world_state, logged_in_user, bot_id, lock))
				throw glare::Exception("You do not have permission to review this chatbot's conversations.");

			page = WebServerResponseUtils::standardHeader(world_state, request, /*page title=*/"Conversation", "");
			page += "<div class=\"main\">   \n";
			page += "<h2>Conversation</h2>";

			const std::vector<ChatTranscriptLog::StoredRecord> records = world_state.chat_transcript_log ?
				world_state.chat_transcript_log->readConversation(bot_id, avatar_uid, 2000) :
				std::vector<ChatTranscriptLog::StoredRecord>();

			if(records.empty())
				page += "<p>No messages recorded for this conversation.</p>";
			else
			{
				page += "<p>" + toString(records.size()) + " message(s), oldest first.</p>";
				page += "<table><tr><th>Time (UTC)</th><th>From</th><th>Message</th><th>Flagged as</th></tr>";

				for(size_t i=0; i<records.size(); ++i)
				{
					const ChatTranscriptLog::StoredRecord& rec = records[i];

					page += std::string("<tr") + (rec.urgent ? " class=\"danger-zone\"" : "") + ">";
					page += "<td>" + web::Escaping::HTMLEscape(rec.time) + "</td>";
					page += "<td>" + web::Escaping::HTMLEscape(rec.from_bot ? rec.bot_name : rec.avatar_name) + "</td>";
					page += "<td>" + web::Escaping::HTMLEscape(rec.text) + "</td>";
					page += "<td>" + categoriesString(rec.safety_categories) + "</td>";
					page += "</tr>";
				}

				page += "</table>";
			}

			page += "<p><a href=\"/safety_alerts\">Back to safety alerts</a></p>";
			page += "</div>   \n";
		} // End lock scope

		page += WebServerResponseUtils::standardFooter(request, true);
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, page);
	}
	catch(glare::Exception& e)
	{
		web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Error: " + web::Escaping::HTMLEscape(e.what()));
	}
}


} // end namespace SafetyHandlers
