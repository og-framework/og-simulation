#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimWireToken-rationale.md

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "OGSimulation/CorrectionStateBufferCodec.h"
#include "OGSimulation/InputRedundancyBundleCodec.h"
#include "OGSimulation/Network/SimEnvelope.h"
#include "OGSimulation/RelayedInputRingCodec.h"

namespace simWireToken
{
inline constexpr std::size_t kCapacity = 32;

struct Field
{
	std::string_view prefix;
	uint8_t          version = 0;
};

struct Token
{
	char        text[kCapacity] = {};
	std::size_t length          = 0;

	constexpr std::string_view view() const { return std::string_view(text, length); }
};

consteval void appendText(Token& token, std::string_view part)
{
	if (part.empty())
		return;
	if (token.length != 0)
		token.text[token.length++] = '.';
	for (const char c : part)
		token.text[token.length++] = c;
}

consteval Token tokenOf(std::initializer_list<Field> fields)
{
	Token token;
	for (const Field& field : fields)
	{
		Token part;
		for (const char c : field.prefix)
			part.text[part.length++] = c;
		if (field.version >= 100)
			part.text[part.length++] = static_cast<char>('0' + field.version / 100);
		if (field.version >= 10)
			part.text[part.length++] = static_cast<char>('0' + field.version / 10 % 10);
		part.text[part.length++] = static_cast<char>('0' + field.version % 10);
		appendText(token, part.view());
	}
	return token;
}

consteval Token join(std::initializer_list<Token> fragments)
{
	Token token;
	for (const Token& fragment : fragments)
		appendText(token, fragment.view());
	return token;
}

inline constexpr Token kSimFragment = tokenOf({ { "c", correctionStateBuffer::kWireFormatVersion },
	{ "r", relayedInputRing::kWireFormatVersion }, { "b", inputRedundancyBundle::kWireFormatVersion },
	{ "e", kSimEnvelopeVersion } });
} // namespace simWireToken
