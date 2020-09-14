// Copyright (c) 2020 Jonas Reich

#include "OUUTests.h"

#if WITH_AUTOMATION_WORKER

#include "Templates/Tie.h"
#include "Engine/EngineTypes.h"

BEGIN_DEFINE_SPEC(FTieSpec, "OpenUnrealUtilities.Templates.Tie", DEFAULT_OUU_TEST_FLAGS)
END_DEFINE_SPEC(FTieSpec)
void FTieSpec::Define()
{
	It("should unpack a tuple of two int32", [this]()
	{
		int32 x = 0;
		int32 y = 0;

		Tie(x, y) = []() { return TTuple < int32, int32 >{ 42, 69 }; }();

		SPEC_TEST_EQUAL(x, 42);
		SPEC_TEST_EQUAL(y, 69);
	});

	It("should unpack a mixed tuple of int32 and FString", [this]()
	{
		int32 i = 0;
		FString s = "before";

		Tie(i, s) = []() { return TTuple < int32, FString >{ 42, "after" }; }();

		SPEC_TEST_EQUAL(i, 42);
		SPEC_TEST_EQUAL(s, "after");
	});

	It("should unpack string tuples from lambda call", [this]()
	{
		FString A, B, C;
		Tie(A, B, C) = []() { return TTuple < FString, FString, FString >{ "Alpha",  "Beta",  "Gamma" }; }();
		
		SPEC_TEST_EQUAL(A, "Alpha");
		SPEC_TEST_EQUAL(B, "Beta"); 
		SPEC_TEST_EQUAL(C, "Gamma");
	});

	It("should unpack string tuples from local variable", [this]()
	{
		FString A, B, C;
		TTuple < FString, FString, FString > LocalVar { "Alpha", "Beta", "Gamma" };
		Tie(A, B, C) = LocalVar;

		SPEC_TEST_EQUAL(A, "Alpha");
		SPEC_TEST_EQUAL(B, "Beta");
		SPEC_TEST_EQUAL(C, "Gamma");
	});

	It("should unpack string tuples from inline variable", [this]()
	{
		FString A, B, C;
		Tie(A, B, C) = TTuple < FString, FString, FString >{ "Alpha", "Beta", "Gamma" };

		SPEC_TEST_EQUAL(A, "Alpha");
		SPEC_TEST_EQUAL(B, "Beta");
		SPEC_TEST_EQUAL(C, "Gamma");
	});
}

#endif
