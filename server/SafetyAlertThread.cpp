/*=====================================================================
SafetyAlertThread.cpp
---------------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#include "SafetyAlertThread.h"


#include "ServerWorldState.h"
#include "User.h"
#include <networking/SMTPClient.h>
#include <ConPrint.h>
#include <Exception.h>
#include <KillThreadMessage.h>
#include <Lock.h>
#include <PlatformUtils.h>
#include <StringUtils.h>
#include <Timer.h>
#include <Vector.h>


// One recipient, copied out from under the world lock so that the slow part - talking to an SMTP server - happens
// without holding it.
struct AlertRecipient
{
	std::string name;
	std::string email_address;
};


SafetyAlertThread::SafetyAlertThread(ServerAllWorldsState* world_state_)
:	world_state(world_state_)
{
}


SafetyAlertThread::~SafetyAlertThread()
{
}


void SafetyAlertThread::kill()
{
	should_quit = 1;
	getMessageQueue().enqueue(new KillThreadMessage());
}


static std::string alertSummary(const SafetyAlertMessage& alert)
{
	std::string categories;
	for(size_t i=0; i<alert.safety.categories.size(); ++i)
	{
		if(i > 0)
			categories += ", ";
		categories += SafetyClassifier::categoryDescription(alert.safety.categories[i]);
	}

	if(categories.empty())
		categories = "Flagged";

	return categories;
}


void SafetyAlertThread::handleAlert(const SafetyAlertMessage& alert)
{
	const std::string summary = alertSummary(alert);

	// The server console is the delivery route that cannot fail, so write there first and unconditionally, before
	// anything that depends on a mail server or on a lead having been configured.
	conPrint("");
	conPrint("========================= SAFETY ALERT =========================");
	conPrint(std::string(alert.safety.urgent ? "URGENT: " : "") + summary);
	conPrint("  time:    " + alert.time_str);
	conPrint("  student: " + alert.student_name + " (user id " + alert.student_user_id.toString() + ")");
	conPrint("  chatbot: " + alert.bot_name + " in world '" + alert.world_name + "'");
	conPrint("  matched: '" + alert.safety.matched_phrase + "'");
	conPrint("  message: " + alert.text);
	conPrint("===============================================================");
	conPrint("");

	// Collect the recipients, then release the lock before sending anything.
	std::vector<AlertRecipient> recipients;
	std::string smtp_servername, smtp_username, smtp_password, from_name, from_email_addr, webserver_hostname;
	{
		Lock lock(world_state->mutex);

		for(auto it = world_state->user_id_to_users.begin(); it != world_state->user_id_to_users.end(); ++it)
		{
			const User* user = it->second.ptr();
			if(user->isSafeguardingLead() && !user->email_address.empty())
			{
				AlertRecipient r;
				r.name = user->name;
				r.email_address = user->email_address;
				recipients.push_back(r);
			}
		}

		// getCredential throws when a credential is absent, which is the normal case on a server that has not set up
		// email at all, so treat that as "no email configured" rather than an error.
		try
		{
			smtp_servername = world_state->getCredential("email_sending_smtp_servername");
			smtp_username   = world_state->getCredential("email_sending_smtp_username");
			smtp_password   = world_state->getCredential("email_sending_smtp_password");
			from_name       = world_state->getCredential("email_sending_from_name");
			from_email_addr = world_state->getCredential("email_sending_from_email_addr");

			// Same credential the password-reset email uses, so there is one place to configure the hostname.
			try { webserver_hostname = world_state->getCredential("email_sending_reset_webserver_hostname"); }
			catch(glare::Exception&) {} // A link is a convenience; carry on without one.
		}
		catch(glare::Exception&)
		{
			smtp_servername.clear();
		}
	} // End lock scope

	if(recipients.empty())
	{
		conPrint("SafetyAlertThread: no user has the safeguarding lead flag set, so no alert email was sent.  "
			"Set one on the admin page for a user, otherwise flagged messages are only visible to someone watching "
			"this console or the /safety_alerts page.");
		return;
	}

	if(smtp_servername.empty())
	{
		conPrint("SafetyAlertThread: email is not configured (see the server credentials file), so no alert email was "
			"sent to the " + toString(recipients.size()) + " safeguarding lead(s).");
		return;
	}

	const std::string link = webserver_hostname.empty() ? std::string() :
		("\n\nThe conversation: https://" + webserver_hostname + "/chat_transcript?bot_id=" + toString(alert.bot_id) +
		 "&avatar_uid=" + alert.avatar_uid_str +
		 "\nAll alerts: https://" + webserver_hostname + "/safety_alerts");

	// The body deliberately includes what the student wrote.  A safeguarding email that says only "something was
	// flagged" makes the reader go and look before they know whether it is urgent, which costs time that may matter.
	const std::string body =
		std::string(alert.safety.urgent ? "An URGENT safety alert has been raised.\n\n" : "A safety alert has been raised.\n\n") +
		"Flagged as: " + summary + "\n" +
		"Time (UTC): " + alert.time_str + "\n" +
		"Student:    " + alert.student_name + "\n" +
		"Chatbot:    " + alert.bot_name + " in world '" + alert.world_name + "'\n" +
		"Matched:    '" + alert.safety.matched_phrase + "'\n\n" +
		"What the student wrote:\n" +
		"  " + alert.text + "\n" +
		link + "\n\n" +
		"This alert came from an automatic phrase check.  It is not a judgement about the student, and it can be "
		"wrong in both directions: it misses anything phrased in a way it does not expect, so no alert does not mean "
		"nothing has happened.  Please read the conversation before acting.\n";

	for(size_t i=0; i<recipients.size(); ++i)
	{
		try
		{
			SMTPClient::SendEmailArgs args;
			args.servername      = smtp_servername;
			args.username        = smtp_username;
			args.password        = smtp_password;
			args.from_name       = from_name;
			args.from_email_addr = from_email_addr;
			args.to_name         = recipients[i].name;
			args.to_email_addr   = recipients[i].email_address;
			args.subject         = std::string(alert.safety.urgent ? "[URGENT] " : "") + "Safety alert: " + summary +
				" - " + alert.student_name;
			args.contents        = body;

			SMTPClient::sendEmail(args);

			conPrint("SafetyAlertThread: alert emailed to safeguarding lead '" + recipients[i].name + "'.");
		}
		catch(glare::Exception& e)
		{
			// One recipient failing must not stop the others being told.
			conPrint("SafetyAlertThread: ERROR sending alert email to '" + recipients[i].name + "': " + e.what());
		}
	}
}


void SafetyAlertThread::doRun()
{
	PlatformUtils::setCurrentThreadNameIfTestsEnabled("SafetyAlertThread");

	js::Vector<Reference<ThreadMessage>, 16> messages;

	while(!should_quit)
	{
		getMessageQueue().dequeueAllQueuedItemsBlocking(messages);

		for(size_t i=0; i<messages.size(); ++i)
		{
			if(dynamic_cast<KillThreadMessage*>(messages[i].ptr()))
				return;

			const SafetyAlertMessage* alert = dynamic_cast<const SafetyAlertMessage*>(messages[i].ptr());
			if(alert)
			{
				try
				{
					handleAlert(*alert);
				}
				catch(glare::Exception& e)
				{
					// Nothing that happens while delivering an alert may take this thread down: the next alert still
					// needs to get through.
					conPrint("SafetyAlertThread: ERROR handling alert: " + e.what());
				}
			}
		}

		messages.clear();
	}
}
