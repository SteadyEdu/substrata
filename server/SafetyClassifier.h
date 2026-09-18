/*=====================================================================
SafetyClassifier.h
------------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#pragma once


#include <string>
#include <vector>


/*=====================================================================
SafetyClassifier
----------------
A first-pass check over what a user says to a chatbot, looking for the things a
school has a duty to notice: a child talking about harming themselves,
disclosing that someone is hurting them, threatening violence, or describing
being bullied.

What this is
------------
A deterministic phrase matcher.  It runs in microseconds, needs no model, cannot
be unavailable, and behaves the same way every time - which is what you want
from the layer that decides whether to raise an alarm.

What this is NOT
----------------
It is not a substitute for a model-based classifier, and still less for a human.
It will miss anything phrased in a way the list does not anticipate, including
most indirect disclosure, and it will sometimes flag an innocent sentence.  It
is the cheap, reliable first layer of a system that needs a second one: treat a
flag as "a person should look at this soon", never as a diagnosis, and treat the
absence of a flag as meaning nothing at all.

Every message is written to the transcript either way (see ChatTranscriptLog);
flagging only decides what gets surfaced for review first.
=====================================================================*/
namespace SafetyClassifier
{

enum Category
{
	Category_SelfHarm,      // The user may be at risk of harming themselves.
	Category_AbuseDisclose, // The user may be disclosing that someone is hurting them.
	Category_Violence,      // The user may be threatening harm to someone else.
	Category_Bullying       // The user may be describing being bullied or excluded.
};


struct Result
{
	Result() : flagged(false), urgent(false) {}

	bool flagged;
	bool urgent; // A person should be told now, not at the end of the day.

	std::vector<Category> categories;
	std::string matched_phrase; // The phrase that triggered the match, so a reviewer can see why.
};


// Threadsafe, and does no allocation beyond the result.  Safe to call on a worker thread for every message.
Result classify(const std::string& message);

const char* categoryName(Category c);       // e.g. "self_harm", for storing and matching on.
const char* categoryDescription(Category c); // e.g. "Possible self-harm", for showing to a person.

void test();

} // end namespace SafetyClassifier
