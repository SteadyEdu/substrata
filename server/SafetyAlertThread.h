/*=====================================================================
SafetyAlertThread.h
-------------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#pragma once


#include "SafetyClassifier.h"
#include "../shared/UserID.h"
#include <MessageableThread.h>
#include <ThreadMessage.h>
#include <Reference.h>
#include <AtomicInt.h>
#include <string>
class ServerAllWorldsState;


/*=====================================================================
SafetyAlertMessage
------------------
One flagged message, handed to the alert thread for delivery.
=====================================================================*/
class SafetyAlertMessage : public ThreadMessage
{
public:
	SafetyAlertMessage() : bot_id(0) {}

	std::string world_name;
	uint64 bot_id;
	std::string bot_name;
	std::string student_name;
	UserID student_user_id;
	std::string avatar_uid_str; // Used to build the link to the conversation.
	std::string text;
	SafetyClassifier::Result safety;
	std::string time_str;
};


/*=====================================================================
SafetyAlertThread
-----------------
Delivers safety alerts to the people who need to see them.

Sending an email means talking to an SMTP server, which can take seconds and can
hang, so it cannot be done on the WorkerThread that is handling a student's chat
- that would stall the conversation the child is having, at exactly the worst
moment.  The WorkerThread hands the alert here instead and carries on.

Delivery is best-effort by design.  The durable record is the alerts file (see
ChatTranscriptLog) and the /safety_alerts page; email is a prompt to go and look
at those, so a mail server being down must not lose the alert or take anything
else with it.
=====================================================================*/
class SafetyAlertThread : public MessageableThread
{
public:
	SafetyAlertThread(ServerAllWorldsState* world_state);
	virtual ~SafetyAlertThread();

	virtual void doRun() override;
	virtual void kill() override;

private:
	GLARE_DISABLE_COPY(SafetyAlertThread)

	void handleAlert(const SafetyAlertMessage& alert);

	ServerAllWorldsState* world_state;
	glare::AtomicInt should_quit;
};
