// Copyright (c) 2005, Rodrigo Braz Monteiro
// Copyright (c) 2010, Thomas Goyne <plorkyeran@aegisub.org>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file ass_override.cpp
/// @brief Parse and modify ASSA style overrides
/// @ingroup subs_storage
///

#include "config.h"

#include "ass_dialogue.h"

#include <libaegisub/color.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <functional>

using namespace boost::adaptors;

AssOverrideParameter::AssOverrideParameter(VariableDataType type, AssParameterClass classification)
: type(type)
, classification(classification)
, omitted(true)
{
}

AssOverrideParameter::AssOverrideParameter(AssOverrideParameter&& o)
: value(std::move(o.value))
, block(std::move(o.block))
, type(o.type)
, classification(o.classification)
, omitted(o.omitted)
{
}

AssOverrideParameter& AssOverrideParameter::operator=(AssOverrideParameter&& rhs) {
	value = std::move(rhs.value);
	block = std::move(rhs.block);
	type = rhs.type;
	classification = rhs.classification;
	return *this;
}

AssOverrideParameter::~AssOverrideParameter() {
}

template<> std::string AssOverrideParameter::Get<std::string>() const {
	if (omitted) throw agi::InternalError("AssOverrideParameter::Get() called on omitted parameter", 0);
	if (block.get()) {
		std::string str(block->GetText());
		if (boost::starts_with(str, "{")) str.erase(begin(str));
		if (boost::ends_with(str, "}")) str.erase(end(str) - 1);
		return str;
	}
	return value;
}

template<> int AssOverrideParameter::Get<int>() const {
	return atoi(Get<std::string>().c_str());
}

template<> double AssOverrideParameter::Get<double>() const {
	return atof(Get<std::string>().c_str());
}

template<> float AssOverrideParameter::Get<float>() const {
	return atof(Get<std::string>().c_str());
}

template<> bool AssOverrideParameter::Get<bool>() const {
	return Get<int>() != 0;
}

template<> agi::Color AssOverrideParameter::Get<agi::Color>() const {
	return Get<std::string>();
}

template<> AssDialogueBlockOverride *AssOverrideParameter::Get<AssDialogueBlockOverride*>() const {
	if (!block.get()) {
		block.reset(new AssDialogueBlockOverride(Get<std::string>()));
		block->ParseTags();
	}
	return block.get();
}

template<> void AssOverrideParameter::Set<std::string>(std::string new_value) {
	omitted = false;
	value = new_value;
	block.reset();
}

template<> void AssOverrideParameter::Set<int>(int new_value) {
	Set(std::to_string(new_value));
}

template<> void AssOverrideParameter::Set<double>(double new_value) {
	Set(std::to_string(new_value));
}

template<> void AssOverrideParameter::Set<bool>(bool new_value) {
	Set<int>(new_value);
}

namespace {
/// The parameter is absent unless the total number of parameters is the
/// indicated number. Note that only arguments not at the end need to be marked
/// as optional; this is just to know which parameters to skip when there are
/// earlier optional arguments
enum AssParameterOptional {
	NOT_OPTIONAL = 0xFF,
	OPTIONAL_1 = 0x01,
	OPTIONAL_2 = 0x02,
	OPTIONAL_3 = 0x04,
	OPTIONAL_4 = 0x08,
	OPTIONAL_5 = 0x10,
	OPTIONAL_6 = 0x20,
	OPTIONAL_7 = 0x40
};

/// Prototype of a single override parameter
struct AssOverrideParamProto {
	/// ASS_ParameterOptional
	int optional;

	/// Type of this parameter
	VariableDataType type;

	/// Semantic type of this parameter
	AssParameterClass classification;

	AssOverrideParamProto(VariableDataType type, int opt, AssParameterClass classi)
	: optional(opt)
	, type(type)
	, classification(classi)
	{
	}
};

struct AssOverrideTagProto {
	/// Name of the tag, with slash
	std::string name;

	/// Parameters to this tag
	std::vector<AssOverrideParamProto> params;

	typedef std::vector<AssOverrideTagProto>::iterator iterator;

	/// @brief Add a parameter to this tag prototype
	/// @param type Data type of the parameter
	/// @param classi Semantic type of the parameter
	/// @param opt Situations in which this parameter is present
	void AddParam(VariableDataType type, AssParameterClass classi = PARCLASS_NORMAL, int opt = NOT_OPTIONAL) {
		params.emplace_back(type, opt, classi);
	}

	/// @brief Convenience function for single-argument tags
	/// @param name Name of the tag, with slash
	/// @param type Data type of the parameter
	/// @param classi Semantic type of the parameter
	/// @param opt Situations in which this parameter is present
	void Set(const char *name, VariableDataType type, AssParameterClass classi = PARCLASS_NORMAL, int opt = NOT_OPTIONAL) {
		this->name = name;
		params.emplace_back(type, opt, classi);
	}
};

static std::vector<AssOverrideTagProto> proto;
static void load_protos() {
	static bool loaded = false;
	if (loaded) return;
	loaded = true;

	proto.resize(56);
	int i = 0;

	// Longer tag names must appear before shorter tag names

	proto[0].Set("\\alpha", VARDATA_TEXT); // \alpha
	proto[++i].Set("\\bord", VARDATA_FLOAT,PARCLASS_ABSOLUTE_SIZE); // \bord<depth>
	proto[++i].Set("\\xbord", VARDATA_FLOAT,PARCLASS_ABSOLUTE_SIZE); // \xbord<depth>
	proto[++i].Set("\\ybord", VARDATA_FLOAT,PARCLASS_ABSOLUTE_SIZE); // \ybord<depth>
	proto[++i].Set("\\shad", VARDATA_FLOAT,PARCLASS_ABSOLUTE_SIZE); // \shad<depth>
	proto[++i].Set("\\xshad", VARDATA_FLOAT,PARCLASS_ABSOLUTE_SIZE); // \xshad<depth>
	proto[++i].Set("\\yshad", VARDATA_FLOAT,PARCLASS_ABSOLUTE_SIZE); // \yshad<depth>

	// \fade(<a1>,<a2>,<a3>,<t1>,<t2>,<t3>,<t4>)
	i++;
	proto[i].name = "\\fade";
	proto[i].AddParam(VARDATA_INT);
	proto[i].AddParam(VARDATA_INT);
	proto[i].AddParam(VARDATA_INT);
	proto[i].AddParam(VARDATA_INT,PARCLASS_RELATIVE_TIME_START);
	proto[i].AddParam(VARDATA_INT,PARCLASS_RELATIVE_TIME_START);
	proto[i].AddParam(VARDATA_INT,PARCLASS_RELATIVE_TIME_START);
	proto[i].AddParam(VARDATA_INT,PARCLASS_RELATIVE_TIME_START);

	// \move(<x1>,<y1>,<x2>,<y2>[,<t1>,<t2>])
	i++;
	proto[i].name = "\\move";
	proto[i].AddParam(VARDATA_FLOAT,PARCLASS_ABSOLUTE_POS_X);
	proto[i].AddParam(VARDATA_FLOAT,PARCLASS_ABSOLUTE_POS_Y);
	proto[i].AddParam(VARDATA_FLOAT,PARCLASS_ABSOLUTE_POS_X);
	proto[i].AddParam(VARDATA_FLOAT,PARCLASS_ABSOLUTE_POS_Y);
	proto[i].AddParam(VARDATA_INT,PARCLASS_RELATIVE_TIME_START);
	proto[i].AddParam(VARDATA_INT,PARCLASS_RELATIVE_TIME_START);

	// If these are rearranged, keep rect clip and vector clip adjacent in this order
	// \clip(<x1>,<y1>,<x2>,<y2>)
	i++;
	proto[i].name = "\\clip";
	proto[i].AddParam(VARDATA_INT,PARCLASS_ABSOLUTE_POS_X);
	proto[i].AddParam(VARDATA_INT,PARCLASS_ABSOLUTE_POS_Y);
	proto[i].AddParam(VARDATA_INT,PARCLASS_ABSOLUTE_POS_X);
	proto[i].AddParam(VARDATA_INT,PARCLASS_ABSOLUTE_POS_Y);

	// \clip([<scale>,]<some drawings>)
	i++;
	proto[i].name = "\\clip";
	proto[i].AddParam(VARDATA_INT,PARCLASS_NORMAL,OPTIONAL_2);
	proto[i].AddParam(VARDATA_TEXT,PARCLASS_DRAWING);

	// \iclip(<x1>,<y1>,<x2>,<y2>)
	i++;
	proto[i].name = "\\iclip";
	proto[i].AddParam(VARDATA_INT,PARCLASS_ABSOLUTE_POS_X);
	proto[i].AddParam(VARDATA_INT,PARCLASS_ABSOLUTE_POS_Y);
	proto[i].AddParam(VARDATA_INT,PARCLASS_ABSOLUTE_POS_X);
	proto[i].AddParam(VARDATA_INT,PARCLASS_ABSOLUTE_POS_Y);

	// \iclip([<scale>,]<some drawings>)
	i++;
	proto[i].name = "\\iclip";
	proto[i].AddParam(VARDATA_INT,PARCLASS_NORMAL,OPTIONAL_2);
	proto[i].AddParam(VARDATA_TEXT,PARCLASS_DRAWING);

	proto[++i].Set("\\fscx", VARDATA_FLOAT,PARCLASS_RELATIVE_SIZE_X); // \fscx<percent>
	proto[++i].Set("\\fscy", VARDATA_FLOAT,PARCLASS_RELATIVE_SIZE_Y); // \fscy<percent>
	// \pos(<x>,<y>)
	i++;
	proto[i].name = "\\pos";
	proto[i].AddParam(VARDATA_FLOAT,PARCLASS_ABSOLUTE_POS_X);
	proto[i].AddParam(VARDATA_FLOAT,PARCLASS_ABSOLUTE_POS_Y);

	// \org(<x>,<y>)
	i++;
	proto[i].name = "\\org";
	proto[i].AddParam(VARDATA_INT,PARCLASS_ABSOLUTE_POS_X);
	proto[i].AddParam(VARDATA_INT,PARCLASS_ABSOLUTE_POS_Y);

	proto[++i].Set("\\pbo", VARDATA_INT,PARCLASS_ABSOLUTE_POS_Y); // \pbo<y>
	// \fad(<t1>,<t2>)
	i++;
	proto[i].name = "\\fad";
	proto[i].AddParam(VARDATA_INT,PARCLASS_RELATIVE_TIME_START);
	proto[i].AddParam(VARDATA_INT,PARCLASS_RELATIVE_TIME_END);

	proto[++i].Set("\\fsp", VARDATA_FLOAT,PARCLASS_ABSOLUTE_SIZE); // \fsp<pixels>
	proto[++i].Set("\\frx", VARDATA_FLOAT); // \frx<degrees>
	proto[++i].Set("\\fry", VARDATA_FLOAT); // \fry<degrees>
	proto[++i].Set("\\frz", VARDATA_FLOAT); // \frz<degrees>
	proto[++i].Set("\\fr", VARDATA_FLOAT); // \fr<degrees>
	proto[++i].Set("\\fax", VARDATA_FLOAT); // \fax<factor>
	proto[++i].Set("\\fay", VARDATA_FLOAT); // \fay<factor>
	proto[++i].Set("\\1c", VARDATA_TEXT); // \1c&H<bbggrr>&
	proto[++i].Set("\\2c", VARDATA_TEXT); // \2c&H<bbggrr>&
	proto[++i].Set("\\3c", VARDATA_TEXT); // \3c&H<bbggrr>&
	proto[++i].Set("\\4c", VARDATA_TEXT); // \4c&H<bbggrr>&
	proto[++i].Set("\\1a", VARDATA_TEXT); // \1a&H<aa>&
	proto[++i].Set("\\2a", VARDATA_TEXT); // \2a&H<aa>&
	proto[++i].Set("\\3a", VARDATA_TEXT); // \3a&H<aa>&
	proto[++i].Set("\\4a", VARDATA_TEXT); // \4a&H<aa>&
	proto[++i].Set("\\fe", VARDATA_TEXT); // \fe<charset>
	proto[++i].Set("\\ko", VARDATA_INT,PARCLASS_KARAOKE); // \ko<duration>
	proto[++i].Set("\\kf", VARDATA_INT,PARCLASS_KARAOKE); // \kf<duration>
	proto[++i].Set("\\be", VARDATA_INT); // \be<strength>
	proto[++i].Set("\\blur", VARDATA_FLOAT); // \blur<strength>
	proto[++i].Set("\\fn", VARDATA_TEXT); // \fn<name>
	proto[++i].Set("\\fs+", VARDATA_FLOAT); // \fs+<size>
	proto[++i].Set("\\fs-", VARDATA_FLOAT); // \fs-<size>
	proto[++i].Set("\\fs", VARDATA_FLOAT,PARCLASS_ABSOLUTE_SIZE); // \fs<size>
	proto[++i].Set("\\an", VARDATA_INT); // \an<alignment>
	proto[++i].Set("\\c", VARDATA_TEXT); // \c&H<bbggrr>&
	proto[++i].Set("\\b", VARDATA_INT); // \b<0/1/weight>
	proto[++i].Set("\\i", VARDATA_BOOL); // \i<0/1>
	proto[++i].Set("\\u", VARDATA_BOOL); // \u<0/1>
	proto[++i].Set("\\s", VARDATA_BOOL); // \s<0/1>
	proto[++i].Set("\\a", VARDATA_INT); // \a<alignment>
	proto[++i].Set("\\k", VARDATA_INT,PARCLASS_KARAOKE); // \k<duration>
	proto[++i].Set("\\K", VARDATA_INT,PARCLASS_KARAOKE); // \K<duration>
	proto[++i].Set("\\q", VARDATA_INT); // \q<0-3>
	proto[++i].Set("\\p", VARDATA_INT); // \p<n>
	proto[++i].Set("\\r", VARDATA_TEXT); // \r[<name>]

	// \t([<t1>,<t2>,][<accel>,]<style modifiers>)
	i++;
	proto[i].name = "\\t";
	proto[i].AddParam(VARDATA_INT,PARCLASS_RELATIVE_TIME_START,OPTIONAL_3 | OPTIONAL_4);
	proto[i].AddParam(VARDATA_INT,PARCLASS_RELATIVE_TIME_START,OPTIONAL_3 | OPTIONAL_4);
	proto[i].AddParam(VARDATA_FLOAT,PARCLASS_NORMAL,OPTIONAL_2 | OPTIONAL_4);
	proto[i].AddParam(VARDATA_BLOCK);
}

std::vector<std::string> tokenize(const std::string &text) {
	std::vector<std::string> paramList;
	paramList.reserve(6);

	if (text.empty())
		return paramList;

	if (text[0] != '(') {
		// There's just one parameter (because there's no parentheses)
		// This means text is all our parameters
		paramList.emplace_back(boost::trim_copy(text));
		return paramList;
	}

	// Ok, so there are parentheses used here, so there may be more than one parameter
	// Enter fullscale parsing!
	size_t i = 0, textlen = text.size();
	int parDepth = 1;
	while (i < textlen && parDepth > 0) {
		// Just skip until next ',' or ')', whichever comes first
		// (Next ')' is achieved when parDepth == 0)
		size_t start = ++i;
		while (i < textlen && parDepth > 0) {
			char c = text[i];
			// parDepth 1 is where we start, and the tag-level we're interested in parsing on
			if (c == ',' && parDepth == 1) break;
			if (c == '(') parDepth++;
			else if (c == ')') {
				if (--parDepth == 0) {
					// We just ate the parenthesis ending this parameter block
					// Make sure it doesn't get included in the parameter text
					break;
				}
			}
			i++;
		}
		// i now points to the first character not member of this parameter
		paramList.emplace_back(boost::trim_copy(text.substr(start, i - start)));
	}

	if (i+1 < textlen) {
		// There's some additional garbage after the parentheses
		// Just add it in for completeness
		paramList.emplace_back(text.begin() + i + 1, text.end());
	}
	return paramList;
}

void parse_parameters(AssOverrideTag *tag, const std::string &text, AssOverrideTagProto::iterator proto_it) {
	tag->Clear();

	// Tokenize text, attempting to find all parameters
	std::vector<std::string> paramList = tokenize(text);
	size_t totalPars = paramList.size();

	int parsFlag = 1 << (totalPars - 1); // Get optional parameters flag
	// vector (i)clip is the second clip proto_ittype in the list
	if ((tag->Name == "\\clip" || tag->Name == "\\iclip") && totalPars != 4) {
		++proto_it;
	}

	unsigned curPar = 0;
	for (auto& curproto : proto_it->params) {
		// Create parameter
		tag->Params.emplace_back(curproto.type, curproto.classification);

		// Check if it's optional and not present
		if (!(curproto.optional & parsFlag) || curPar >= totalPars)
			continue;

		tag->Params.back().Set(paramList[curPar++]);
	}
}

}

// From ass_dialogue.h
void AssDialogueBlockOverride::ParseTags() {
	Tags.clear();

	int depth = 0;
	size_t start = 0;
	for (size_t i = 1; i < text.size(); ++i) {
		if (depth > 0) {
			if (text[i] == ')')
				--depth;
		}
		else if (text[i] == '\\') {
			Tags.emplace_back(text.substr(start, i - start));
			start = i;
		}
		else if (text[i] == '(')
			++depth;
	}

	if (!text.empty())
		Tags.emplace_back(text.substr(start));
}

void AssDialogueBlockOverride::AddTag(std::string const& tag) {
	Tags.emplace_back(tag);
}

static std::string tag_str(AssOverrideTag const& t) { return t; }
std::string AssDialogueBlockOverride::GetText() {
	text = "{" + join(Tags | transformed(tag_str), std::string()) + "}";
	return text;
}

void AssDialogueBlockOverride::ProcessParameters(ProcessParametersCallback callback, void *userData) {
	for (auto& tag : Tags) {
		for (auto& par : tag.Params) {
			if (par.omitted) continue;

			callback(tag.Name, &par, userData);

			// Go recursive if it's a block parameter
			if (par.GetType() == VARDATA_BLOCK)
				par.Get<AssDialogueBlockOverride*>()->ProcessParameters(callback, userData);
		}
	}
}

AssOverrideTag::AssOverrideTag() : valid(false) { }
AssOverrideTag::AssOverrideTag(std::string const& text) {
	SetText(text);
}
AssOverrideTag::AssOverrideTag(AssOverrideTag&& rhs)
: valid(rhs.valid)
, Name(std::move(rhs.Name))
, Params(std::move(rhs.Params))
{
}

AssOverrideTag& AssOverrideTag::operator=(AssOverrideTag&& rhs) {
	valid = rhs.valid;
	Name = std::move(rhs.Name);
	Params = std::move(rhs.Params);
	return *this;
}

void AssOverrideTag::Clear() {
	Params.clear();
	Params.reserve(6);
	valid = false;
}

void AssOverrideTag::SetText(const std::string &text) {
	load_protos();
	for (auto cur = proto.begin(); cur != proto.end(); ++cur) {
		if (boost::starts_with(text, cur->name)) {
			Name = cur->name;
			parse_parameters(this, text.substr(Name.size()), cur);
			valid = true;
			return;
		}
	}

	// Junk tag
	Name = text;
	valid = false;
}

static std::string param_str(AssOverrideParameter const& p) { return p.Get<std::string>(); }
AssOverrideTag::operator std::string() const {
	std::string result = Name;

	// Determine if it needs parentheses
	bool parentheses = Params.size() > 1;
	if (parentheses) result += "(";

	// Add parameters
	result += join(Params
		| filtered([](AssOverrideParameter const& p) { return !p.omitted; } )
		| transformed(param_str),
		",");

	if (parentheses) result += ")";
	return result;
}
