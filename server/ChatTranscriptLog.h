/*=====================================================================
ChatTranscriptLog.h
-------------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#pragma once


#include "../shared/UID.h"
#include "../shared/UserID.h"
#include <Mutex.h>
#include <Lock.h>
#include <Platform.h>
#include <string>
class FileOutStream;


/*=====================================================================
ChatTranscriptLog
-----------------
An append-only record of what chatbots say to users, and of what users say to a
chatbot in a private conversation.

Private conversations (see ChatBot::PRIVATE_CONVERSATION_FLAG) are not visible to
anyone else in the world, so without this log a student could talk to an AI with
no adult able to review it afterwards.  For a K-12 deployment that is not
acceptable: the transcript is what lets a teacher check what a tutor actually
told a child, and what lets a safeguarding disclosure be found.

Records are written as JSON Lines (one JSON object per line) to a file per UTC
day, which keeps them easy to grep, to feed to a safety classifier, and to
retain or delete a day at a time.

Logging must never take down the server, so append() swallows and reports errors
rather than throwing.  Each record is flushed as it is written: these logs are
most needed when something has gone wrong, which is exactly when the process may
not shut down cleanly.
=====================================================================*/
class ChatTranscriptLog
{
public:
	ChatTranscriptLog();
	~ChatTranscriptLog();

	// Creates log_dir if needed.  retention_days of 0 means keep transcripts forever, which is the default: expiring
	// safeguarding records is a decision for an administrator to make explicitly, not something to do by default.
	// Throws glare::Exception if the directory cannot be created.
	void open(const std::string& log_dir, int retention_days);

	bool isOpen() const { Lock lock(mutex); return !log_dir.empty(); } // threadsafe

	struct Record
	{
		Record() : bot_id(0), is_private(false), from_bot(false) {}

		std::string world_name; // Empty string for the root world.
		uint64 bot_id;
		std::string bot_name;

		// The user's avatar.  This is the key that ties the two directions of a conversation together: a bot reply is
		// produced on the server's main thread, which does not know which logged-in user is behind the avatar.
		UID avatar_uid;
		std::string avatar_name;

		UserID user_id; // Invalid when not known at the point the record was made (i.e. on bot -> user records).

		bool is_private;
		bool from_bot; // true: chatbot -> user.  false: user -> chatbot.
		std::string text;
	};

	// Threadsafe.  Does not throw.
	void append(const Record& record);

private:
	GLARE_DISABLE_COPY(ChatTranscriptLog)

	void openFileForDay(const std::string& day) REQUIRES(mutex); // Does not throw.
	void deleteExpiredFiles() REQUIRES(mutex); // Does not throw.

	mutable Mutex mutex;
	std::string log_dir		GUARDED_BY(mutex);
	int retention_days		GUARDED_BY(mutex);
	std::string cur_day		GUARDED_BY(mutex); // "YYYY-MM-DD" of the currently open file, or empty if none is open.
	FileOutStream* file		GUARDED_BY(mutex);
	bool reported_error		GUARDED_BY(mutex); // Only complain to the console once, so a broken disk doesn't spam the log.
};
