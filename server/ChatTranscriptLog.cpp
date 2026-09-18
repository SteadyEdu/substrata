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
#include <JSONParser.h>
#include <MemMappedFile.h>
#include <algorithm>
#include <time.h>


static const std::string FILE_PREFIX = "chat-";
static const std::string ALERTS_PREFIX = "alerts-";
static const std::string FILE_SUFFIX = ".jsonl";


ChatTranscriptLog::ChatTranscriptLog()
:	retention_days(0),
	file(NULL),
	alerts_file(NULL),
	reported_error(false)
{
}


ChatTranscriptLog::~ChatTranscriptLog()
{
	Lock lock(mutex);
	delete file;
	file = NULL;
	delete alerts_file;
	alerts_file = NULL;
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


// Writes one line to the transcript file, or to the alerts file, opening or rolling it over as needed.
void ChatTranscriptLog::writeLine(const std::string& line, const std::string& day, bool to_alerts_file)
{
	try
	{
		if(to_alerts_file)
		{
			if(!alerts_file || (day != alerts_day))
			{
				delete alerts_file;
				alerts_file = new FileOutStream(log_dir + "/" + ALERTS_PREFIX + day + FILE_SUFFIX, std::ios::binary | std::ios::app);
				alerts_day = day;
			}

			alerts_file->writeData(line.data(), line.size());
			alerts_file->flush();
		}
		else if(file)
		{
			file->writeData(line.data(), line.size());
			file->flush(); // Flush per record: these logs matter most when the server does not shut down cleanly.
		}
	}
	catch(glare::Exception& e)
	{
		if(to_alerts_file) { delete alerts_file; alerts_file = NULL; alerts_day.clear(); }
		if(!reported_error)
		{
			conPrint("ChatTranscriptLog: ERROR while writing a record: " + e.what());
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
			const std::string prefix = hasPrefix(filename, FILE_PREFIX) ? FILE_PREFIX : ALERTS_PREFIX;
			if(!hasPrefix(filename, prefix) || !hasSuffix(filename, FILE_SUFFIX))
				continue;
			if(filename.size() != prefix.size() + 10 + FILE_SUFFIX.size()) // "YYYY-MM-DD" is 10 chars.
				continue;
			const std::string day = filename.substr(prefix.size(), 10);

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

		if(!file && !record.safety.flagged)
			return; // openFileForDay() has already reported why.  A flagged record still goes to the alerts file.

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

		if(record.safety.flagged)
		{
			line += ",\"safety_urgent\":" + std::string(record.safety.urgent ? "true" : "false");
			line += ",\"safety_matched\":\"" + web::Escaping::JSONEscape(record.safety.matched_phrase) + "\"";
			line += ",\"safety_categories\":[";
			for(size_t i=0; i<record.safety.categories.size(); ++i)
			{
				if(i > 0)
					line += ",";
				line += "\"" + std::string(SafetyClassifier::categoryName(record.safety.categories[i])) + "\"";
			}
			line += "]";
		}

		line += "}\n";

		writeLine(line, day, /*to_alerts_file=*/false);

		// A flagged message also goes to the alerts file, so review does not mean reading every transcript.
		if(record.safety.flagged)
			writeLine(line, day, /*to_alerts_file=*/true);
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


// Reads back records written by append().  Files are named with the date, so sorting the filenames sorts by day.
// Reading is done by a webserver thread, is low volume, and must never take down the server, so any problem with a
// file just means that file contributes nothing.
std::vector<ChatTranscriptLog::StoredRecord> ChatTranscriptLog::readRecords(bool alerts, size_t max_records,
	uint64 filter_bot_id, UID filter_avatar_uid, bool use_filter, bool newest_first) const
{
	std::vector<StoredRecord> records;

	std::string dir;
	{
		Lock lock(mutex);
		dir = log_dir;
	}

	if(dir.empty())
		return records;

	const std::string& prefix = alerts ? ALERTS_PREFIX : FILE_PREFIX;

	std::vector<std::string> day_files;
	try
	{
		const std::vector<std::string> filenames = FileUtils::getFilesInDir(dir);
		for(size_t i=0; i<filenames.size(); ++i)
			if(hasPrefix(filenames[i], prefix) && hasSuffix(filenames[i], FILE_SUFFIX) &&
				(filenames[i].size() == prefix.size() + 10 + FILE_SUFFIX.size()))
				day_files.push_back(filenames[i]);
	}
	catch(glare::Exception&)
	{
		return records;
	}

	std::sort(day_files.begin(), day_files.end());
	std::reverse(day_files.begin(), day_files.end()); // Newest day first, so we can stop once we have enough.

	for(size_t f=0; f<day_files.size() && (records.size() < max_records); ++f)
	{
		std::string contents;
		try
		{
			MemMappedFile mapped_file(dir + "/" + day_files[f]);
			contents.assign((const char*)mapped_file.fileData(), mapped_file.fileSize());
		}
		catch(glare::Exception&)
		{
			continue; // A file we cannot read contributes nothing.
		}

		// Collect this file's records, then append them in the requested order.
		std::vector<StoredRecord> file_records;

		size_t line_start = 0;
		while(line_start < contents.size())
		{
			size_t line_end = contents.find('\n', line_start);
			if(line_end == std::string::npos)
				line_end = contents.size();

			const size_t line_len = line_end - line_start;
			if(line_len > 1)
			{
				try
				{
					JSONParser parser;
					parser.parseBuffer(contents.data() + line_start, line_len);
					const JSONNode& root = parser.nodes[0];

					StoredRecord rec;
					rec.time        = root.getChildStringValueWithDefaultVal(parser, "time", "");
					rec.world_name  = root.getChildStringValueWithDefaultVal(parser, "world", "");
					rec.bot_id      = (uint64)root.getChildUIntValueWithDefaultVal(parser, "bot_id", 0);
					rec.bot_name    = root.getChildStringValueWithDefaultVal(parser, "bot_name", "");
					rec.avatar_name = root.getChildStringValueWithDefaultVal(parser, "avatar_name", "");
					rec.is_private  = root.getChildBoolValueWithDefaultVal(parser, "private", false);
					rec.from_bot    = root.getChildStringValueWithDefaultVal(parser, "dir", "") == "bot";
					rec.text        = root.getChildStringValueWithDefaultVal(parser, "text", "");
					rec.urgent      = root.getChildBoolValueWithDefaultVal(parser, "safety_urgent", false);
					rec.matched_phrase = root.getChildStringValueWithDefaultVal(parser, "safety_matched", "");

					// avatar_uid is written as null for a message spoken to the whole world.
					if(root.hasChild("avatar_uid"))
					{
						const JSONNode& uid_node = root.getChildNode(parser, "avatar_uid");
						if(uid_node.type == JSONNode::Type_Number)
							rec.avatar_uid = UID((uint64)uid_node.getUIntValue());
					}

					if(root.hasChild("user_id"))
						rec.user_id = UserID((uint32)root.getChildUIntValueWithDefaultVal(parser, "user_id", 0));

					if(root.hasChild("safety_categories"))
					{
						const JSONNode& cats = root.getChildArray(parser, "safety_categories");
						for(size_t c=0; c<cats.child_indices.size(); ++c)
						{
							const JSONNode& cat_node = parser.nodes[cats.child_indices[c]];
							if(cat_node.type == JSONNode::Type_String)
								rec.safety_categories.push_back(cat_node.string_v);
						}
					}

					const bool matches = !use_filter ||
						((rec.bot_id == filter_bot_id) && (rec.avatar_uid == filter_avatar_uid));

					if(matches)
						file_records.push_back(rec);
				}
				catch(glare::Exception&)
				{
					// A malformed line is skipped rather than losing the rest of the file.
				}
			}

			line_start = line_end + 1;
		}

		if(newest_first)
		{
			// Within a file, records are oldest first; reverse so the newest come out first.
			for(size_t i=file_records.size(); i-- > 0; )
			{
				records.push_back(file_records[i]);
				if(records.size() >= max_records)
					break;
			}
		}
		else
		{
			records.insert(records.begin(), file_records.begin(), file_records.end());
			if(records.size() > max_records)
				records.erase(records.begin(), records.begin() + (records.size() - max_records));
		}
	}

	return records;
}


std::vector<ChatTranscriptLog::StoredRecord> ChatTranscriptLog::readRecentAlerts(size_t max_records) const
{
	return readRecords(/*alerts=*/true, max_records, /*filter_bot_id=*/0, /*filter_avatar_uid=*/UID::invalidUID(),
		/*use_filter=*/false, /*newest_first=*/true);
}


std::vector<ChatTranscriptLog::StoredRecord> ChatTranscriptLog::readConversation(uint64 bot_id, UID avatar_uid,
	size_t max_records) const
{
	return readRecords(/*alerts=*/false, max_records, bot_id, avatar_uid, /*use_filter=*/true, /*newest_first=*/false);
}
