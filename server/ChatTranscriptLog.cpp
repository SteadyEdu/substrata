/*=====================================================================
ChatTranscriptLog.cpp
---------------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#include "ChatTranscriptLog.h"


#include <webserver/Escaping.h>
#include <ConPrint.h>
#include <Exception.h>
#include <FileOutStream.h>
#include <FileUtils.h>
#include <Lock.h>
#include <StringUtils.h>
#include <time.h>


static const std::string FILE_PREFIX = "chat-";
static const std::string FILE_SUFFIX = ".jsonl";


ChatTranscriptLog::ChatTranscriptLog()
:	retention_days(0),
	file(NULL),
	reported_error(false)
{
}


ChatTranscriptLog::~ChatTranscriptLog()
{
	Lock lock(mutex);
	delete file;
	file = NULL;
}


// Splits a time_t into UTC calendar components.  gmtime() itself returns a pointer to shared static storage, so use
// the re-entrant form: transcripts are appended from both worker threads and the server main thread.
static bool getUTCTimeComponents(uint64 time, struct tm& tm_out)
{
	const time_t t = (time_t)time;
#ifdef _WIN32
	return gmtime_s(&tm_out, &t) == 0;
#else
	return gmtime_r(&t, &tm_out) != NULL;
#endif
}


static std::string dayStringForTM(const struct tm& tm_val)
{
	char buf[32];
	const size_t n = strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_val);
	return std::string(buf, n);
}


static std::string ISO8601StringForTM(const struct tm& tm_val)
{
	char buf[32];
	const size_t n = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_val);
	return std::string(buf, n);
}


void ChatTranscriptLog::open(const std::string& log_dir_, int retention_days_)
{
	Lock lock(mutex);

	FileUtils::createDirIfDoesNotExist(log_dir_); // Throws FileUtils::FileUtilsExcep, which derives from glare::Exception.

	this->log_dir = log_dir_;
	this->retention_days = retention_days_;

	conPrint("ChatTranscriptLog: writing chatbot transcripts to '" + log_dir_ + "'" +
		((retention_days_ > 0) ? (", keeping " + toString(retention_days_) + " days") : ", keeping all transcripts"));
}


void ChatTranscriptLog::openFileForDay(const std::string& day)
{
	delete file;
	file = NULL;

	try
	{
		file = new FileOutStream(log_dir + "/" + FILE_PREFIX + day + FILE_SUFFIX, std::ios::binary | std::ios::app);
		cur_day = day;
		reported_error = false;
	}
	catch(glare::Exception& e)
	{
		cur_day.clear();
		if(!reported_error)
		{
			conPrint("ChatTranscriptLog: ERROR: failed to open transcript file for " + day + ": " + e.what());
			reported_error = true;
		}
	}
}


void ChatTranscriptLog::deleteExpiredFiles()
{
	if(retention_days <= 0) // 0 = keep forever.
		return;

	// Work out the oldest day we are keeping, then delete whole day-files older than it.  Comparing the "YYYY-MM-DD"
	// strings is the same as comparing the dates, so no date parsing is needed.
	struct tm cutoff_tm;
	if(!getUTCTimeComponents((uint64)time(NULL) - (uint64)retention_days * 86400, cutoff_tm))
		return;
	const std::string cutoff_day = dayStringForTM(cutoff_tm);

	try
	{
		const std::vector<std::string> filenames = FileUtils::getFilesInDir(log_dir);
		for(size_t i=0; i<filenames.size(); ++i)
		{
			const std::string& filename = filenames[i];

			// Only ever touch files we wrote ourselves, matching exactly "chat-YYYY-MM-DD.jsonl".
			if(!hasPrefix(filename, FILE_PREFIX) || !hasSuffix(filename, FILE_SUFFIX))
				continue;
			if(filename.size() != FILE_PREFIX.size() + 10 + FILE_SUFFIX.size()) // "YYYY-MM-DD" is 10 chars.
				continue;
			const std::string day = filename.substr(FILE_PREFIX.size(), 10);

			if(day < cutoff_day)
			{
				conPrint("ChatTranscriptLog: deleting transcript '" + filename + "', older than the configured retention of " + toString(retention_days) + " days.");
				FileUtils::deleteFile(log_dir + "/" + filename);
			}
		}
	}
	catch(glare::Exception& e)
	{
		conPrint("ChatTranscriptLog: ERROR while expiring old transcripts: " + e.what());
	}
}


void ChatTranscriptLog::append(const Record& record)
{
	try
	{
		struct tm now_tm;
		const uint64 now = (uint64)time(NULL);
		if(!getUTCTimeComponents(now, now_tm))
			return;

		const std::string day = dayStringForTM(now_tm);

		Lock lock(mutex);

		if(log_dir.empty()) // Logging disabled.
			return;

		if(day != cur_day) // First record, or the UTC day has rolled over:
		{
			openFileForDay(day);
			deleteExpiredFiles();
		}

		if(!file)
			return; // openFileForDay() has already reported why.

		std::string line = "{";
		line += "\"time\":\"" + ISO8601StringForTM(now_tm) + "\"";
		line += ",\"world\":\"" + web::Escaping::JSONEscape(record.world_name) + "\"";
		line += ",\"bot_id\":" + toString(record.bot_id);
		line += ",\"bot_name\":\"" + web::Escaping::JSONEscape(record.bot_name) + "\"";
		// A message spoken to the whole world has no single recipient avatar.
		line += ",\"avatar_uid\":" + (record.avatar_uid.valid() ? record.avatar_uid.toString() : std::string("null"));
		line += ",\"avatar_name\":\"" + web::Escaping::JSONEscape(record.avatar_name) + "\"";
		if(record.user_id.valid())
			line += ",\"user_id\":" + toString(record.user_id.value());
		line += ",\"private\":" + std::string(record.is_private ? "true" : "false");
		line += ",\"dir\":\"" + std::string(record.from_bot ? "bot" : "user") + "\"";
		line += ",\"text\":\"" + web::Escaping::JSONEscape(record.text) + "\"";
		line += "}\n";

		file->writeData(line.data(), line.size());
		file->flush(); // Flush per record: these logs matter most when the server does not shut down cleanly.
	}
	catch(glare::Exception& e)
	{
		Lock lock(mutex);
		if(!reported_error)
		{
			conPrint("ChatTranscriptLog: ERROR while writing transcript record: " + e.what());
			reported_error = true;
		}
	}
	catch(std::bad_alloc&)
	{
	}
}
