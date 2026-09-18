/*=====================================================================
SafetyClassifier.cpp
--------------------
Copyright Glare Technologies Limited 2026 -
=====================================================================*/
#include "SafetyClassifier.h"


#include <ConPrint.h>
#include <StringUtils.h>
#include <TestUtils.h>


namespace SafetyClassifier
{


struct Phrase
{
	const char* text;
	Category category;
	bool urgent;
};


// Phrases are matched on word boundaries against a normalised copy of the message, so "cut myself" matches
// "I cut myself." but "ass" would not match "classic".
//
// These are deliberately multi-word where a single word would be ambiguous: "kill" on its own matches "you killed that
// question", and a tutor that raises a safeguarding alert every time a child says "this homework is killing me" will be
// switched off within a week.  A missed phrase is a gap in one layer; a noisy layer destroys the whole system's
// credibility.  When in doubt, leave it out and rely on the model-based layer that should sit behind this.
static const Phrase phrases[] =
{
	// Self-harm.  Always urgent.
	{ "kill myself",            Category_SelfHarm, true },
	{ "killing myself",         Category_SelfHarm, true },
	{ "end my life",            Category_SelfHarm, true },
	{ "ending my life",         Category_SelfHarm, true },
	{ "want to die",            Category_SelfHarm, true },
	{ "wanna die",              Category_SelfHarm, true },
	{ "wish i was dead",        Category_SelfHarm, true },
	{ "wish i were dead",       Category_SelfHarm, true },
	{ "better off dead",        Category_SelfHarm, true },
	{ "better off without me",  Category_SelfHarm, true },
	{ "hurt myself",            Category_SelfHarm, true },
	{ "hurting myself",         Category_SelfHarm, true },
	{ "cut myself",             Category_SelfHarm, true },
	{ "cutting myself",         Category_SelfHarm, true },
	{ "suicide",                Category_SelfHarm, true },
	{ "suicidal",               Category_SelfHarm, true },
	{ "no reason to live",      Category_SelfHarm, true },
	{ "do not want to live",    Category_SelfHarm, true },
	{ "dont want to live",      Category_SelfHarm, true },

	// Disclosure that someone is hurting the child.  Always urgent.
	{ "hits me",                Category_AbuseDisclose, true },
	{ "beats me",               Category_AbuseDisclose, true },
	{ "hurts me at home",       Category_AbuseDisclose, true },
	{ "scared to go home",      Category_AbuseDisclose, true },
	{ "afraid to go home",      Category_AbuseDisclose, true },
	{ "touched me",             Category_AbuseDisclose, true },
	{ "touches me",             Category_AbuseDisclose, true },
	{ "hit me last night",      Category_AbuseDisclose, true },

	// Threats towards others.  Always urgent.
	{ "kill him",               Category_Violence, true },
	{ "kill her",               Category_Violence, true },
	{ "kill them",              Category_Violence, true },
	{ "shoot up the school",    Category_Violence, true },
	{ "bring a gun",            Category_Violence, true },
	{ "going to hurt him",      Category_Violence, true },
	{ "going to hurt her",      Category_Violence, true },
	{ "going to hurt them",     Category_Violence, true },

	// Bullying and isolation.  Worth a look, but not an emergency.
	{ "being bullied",          Category_Bullying, false },
	{ "they bully me",          Category_Bullying, false },
	{ "bullying me",            Category_Bullying, false },
	{ "everyone hates me",      Category_Bullying, false },
	{ "nobody likes me",        Category_Bullying, false },
	{ "no one likes me",        Category_Bullying, false },
	{ "make fun of me",         Category_Bullying, false },
	{ "makes fun of me",        Category_Bullying, false },
	{ "i have no friends",      Category_Bullying, false }
};


// Lowercase, drop apostrophes, replace everything else that is not a letter or digit with a space, collapse runs of
// spaces, and pad with a space at each end.  Searching the result for " phrase " then gives word-boundary matching
// without a regex engine.
//
// Apostrophes are dropped rather than turned into a space so that a contraction stays one word: "don't" becomes
// "dont" and matches, where turning it into a space would give "don t" and match nothing.  Both the ASCII quote and
// the typographic one (U+2019) are handled, since phones produce the latter.
static std::string normalise(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 2);
	out.push_back(' ');

	bool last_was_space = true;
	for(size_t i=0; i<s.size(); ++i)
	{
		const unsigned char c = (unsigned char)s[i];

		if(c == '\'') // ASCII apostrophe: drop it, joining the two halves of the contraction.
			continue;

		// U+2019 RIGHT SINGLE QUOTATION MARK, encoded as E2 80 99.  Drop all three bytes.
		if((c == 0xE2) && ((i + 2) < s.size()) && ((unsigned char)s[i+1] == 0x80) && ((unsigned char)s[i+2] == 0x99))
		{
			i += 2;
			continue;
		}
		if((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		{
			out.push_back((char)c);
			last_was_space = false;
		}
		else if(c >= 'A' && c <= 'Z')
		{
			out.push_back((char)(c - 'A' + 'a'));
			last_was_space = false;
		}
		else
		{
			// Any run of punctuation, whitespace or non-ASCII becomes a single space.
			if(!last_was_space)
				out.push_back(' ');
			last_was_space = true;
		}
	}

	if(!last_was_space)
		out.push_back(' ');

	return out;
}


Result classify(const std::string& message)
{
	Result result;

	if(message.empty())
		return result;

	const std::string normalised = normalise(message);

	for(size_t i=0; i<staticArrayNumElems(phrases); ++i)
	{
		const std::string padded_phrase = std::string(" ") + phrases[i].text + " ";
		if(normalised.find(padded_phrase) == std::string::npos)
			continue;

		result.flagged = true;
		result.urgent = result.urgent || phrases[i].urgent;

		// Record the first phrase that matched, which is the most useful single thing to show a reviewer.
		if(result.matched_phrase.empty())
			result.matched_phrase = phrases[i].text;

		bool already_have_category = false;
		for(size_t z=0; z<result.categories.size(); ++z)
			if(result.categories[z] == phrases[i].category)
				already_have_category = true;

		if(!already_have_category)
			result.categories.push_back(phrases[i].category);
	}

	return result;
}


const char* categoryName(Category c)
{
	switch(c)
	{
	case Category_SelfHarm:      return "self_harm";
	case Category_AbuseDisclose: return "abuse_disclosure";
	case Category_Violence:      return "violence";
	case Category_Bullying:      return "bullying";
	default:                     return "unknown";
	}
}


const char* categoryDescription(Category c)
{
	switch(c)
	{
	case Category_SelfHarm:      return "Possible self-harm";
	case Category_AbuseDisclose: return "Possible disclosure of abuse";
	case Category_Violence:      return "Possible threat of violence";
	case Category_Bullying:      return "Possible bullying";
	default:                     return "Unknown";
	}
}


#if BUILD_TESTS


static bool hasCategory(const Result& r, Category c)
{
	for(size_t i=0; i<r.categories.size(); ++i)
		if(r.categories[i] == c)
			return true;
	return false;
}


void test()
{
	conPrint("SafetyClassifier::test()");

	// Ordinary schoolwork must not be flagged.  This is the case that matters most: a classifier that cries wolf
	// during a maths lesson gets turned off, and then it protects nobody.
	{
		const char* const ordinary[] = {
			"what is 7 times 8?",
			"Can you explain long division?",
			"I hate fractions so much",
			"this homework is killing me",
			"you killed that explanation, thanks!",
			"We read a book about the Civil War and lots of people died.",
			"My hit counter says 40.",
			"hit me up with another question",
			"The volcano killed the dinosaurs right?",
			"I cut the paper into eight pieces."
		};
		for(size_t i=0; i<staticArrayNumElems(ordinary); ++i)
		{
			const Result r = classify(ordinary[i]);
			if(r.flagged)
				conPrint("SafetyClassifier::test(): false positive on: " + std::string(ordinary[i]) + " (matched '" + r.matched_phrase + "')");
			testAssert(!r.flagged);
		}
	}

	// Self-harm, in a few of the shapes a child actually uses.
	{
		const Result r = classify("sometimes i want to die");
		testAssert(r.flagged && r.urgent);
		testAssert(hasCategory(r, Category_SelfHarm));
	}
	{
		const Result r = classify("I've been cutting myself again");
		testAssert(r.flagged && r.urgent);
		testAssert(hasCategory(r, Category_SelfHarm));
	}
	{
		// Punctuation and capitals must not matter, and "don't" must match the "dont" phrase.
		const Result r = classify("I DON'T WANT TO LIVE!!!");
		testAssert(r.flagged && r.urgent);
		testAssert(hasCategory(r, Category_SelfHarm));
	}
	{
		// The same, with the typographic apostrophe a phone keyboard produces.
		const Result r = classify("i don\xe2\x80\x99t want to live any more");
		testAssert(r.flagged && r.urgent);
		testAssert(hasCategory(r, Category_SelfHarm));
	}

	// Disclosure of abuse.
	{
		const Result r = classify("my stepdad hits me when he is angry");
		testAssert(r.flagged && r.urgent);
		testAssert(hasCategory(r, Category_AbuseDisclose));
	}
	{
		const Result r = classify("im scared to go home today");
		testAssert(r.flagged && r.urgent);
		testAssert(hasCategory(r, Category_AbuseDisclose));
	}

	// Threats of violence.
	{
		const Result r = classify("im going to hurt him tomorrow");
		testAssert(r.flagged && r.urgent);
		testAssert(hasCategory(r, Category_Violence));
	}

	// Bullying: flagged for review, but not an emergency.
	{
		const Result r = classify("the other kids make fun of me every day");
		testAssert(r.flagged);
		testAssert(!r.urgent);
		testAssert(hasCategory(r, Category_Bullying));
	}

	// A message can match more than one category, and urgency wins over non-urgency.
	{
		const Result r = classify("everyone hates me and i want to die");
		testAssert(r.flagged && r.urgent);
		testAssert(hasCategory(r, Category_Bullying));
		testAssert(hasCategory(r, Category_SelfHarm));
		testAssert(r.categories.size() == 2);
	}

	// Empty and whitespace-only input must be handled without flagging or crashing.
	{
		testAssert(!classify("").flagged);
		testAssert(!classify("   \n\t  ").flagged);
	}

	// Non-ASCII must not break normalisation.
	{
		const Result r = classify("j'ai envie de mourir \xE2\x80\x94 i want to die");
		testAssert(r.flagged && r.urgent);
	}

	conPrint("SafetyClassifier::test() done.");
}


#endif // BUILD_TESTS


} // end namespace SafetyClassifier
