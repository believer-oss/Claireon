// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonLintTypes.h"

namespace ClaireonLint
{

const TCHAR* ToString(EClaireonLintSeverity Severity)
{
	switch (Severity)
	{
	case EClaireonLintSeverity::Warning: return TEXT("warning");
	default:                             return TEXT("info");
	}
}

const TCHAR* ToString(EClaireonLintConfidence Confidence)
{
	switch (Confidence)
	{
	case EClaireonLintConfidence::High:   return TEXT("high");
	case EClaireonLintConfidence::Medium: return TEXT("medium");
	default:                              return TEXT("unverified");
	}
}

const TCHAR* ToString(EClaireonLintScope Scope)
{
	switch (Scope)
	{
	case EClaireonLintScope::Variables: return TEXT("variables");
	case EClaireonLintScope::Layout:    return TEXT("layout");
	case EClaireonLintScope::Functions: return TEXT("functions");
	case EClaireonLintScope::Hygiene:   return TEXT("hygiene");
	default:                            return TEXT("graph");
	}
}

bool ParseScope(const FString& Name, EClaireonLintScope& OutScope)
{
	if (Name.Equals(TEXT("variables"), ESearchCase::IgnoreCase)) { OutScope = EClaireonLintScope::Variables; return true; }
	if (Name.Equals(TEXT("graph"), ESearchCase::IgnoreCase))     { OutScope = EClaireonLintScope::Graph;     return true; }
	if (Name.Equals(TEXT("layout"), ESearchCase::IgnoreCase))    { OutScope = EClaireonLintScope::Layout;    return true; }
	if (Name.Equals(TEXT("functions"), ESearchCase::IgnoreCase)) { OutScope = EClaireonLintScope::Functions; return true; }
	if (Name.Equals(TEXT("hygiene"), ESearchCase::IgnoreCase))   { OutScope = EClaireonLintScope::Hygiene;   return true; }
	return false;
}

}
