/*=====================================================================
SafetyHandlers.h
----------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#pragma once


class ServerAllWorldsState;
namespace web
{
class RequestInfo;
class ReplyInfo;
}


/*=====================================================================
SafetyHandlers
--------------
Pages for reviewing what students have said to chatbots.

A transcript nobody can read is not a safeguarding record.  Before these pages
existed, reviewing a conversation meant having shell access to the server, which
no teacher has, so in practice the transcripts protected nobody.
=====================================================================*/
namespace SafetyHandlers
{

// /safety_alerts - messages the safety check flagged, newest first.
void renderSafetyAlertsPage(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info);

// /chat_transcript?bot_id=N&avatar_uid=M - one conversation in full.
void renderChatTranscriptPage(ServerAllWorldsState& world_state, const web::RequestInfo& request, web::ReplyInfo& reply_info);

} // end namespace SafetyHandlers
