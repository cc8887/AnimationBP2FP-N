#pragma once

#include "CoreMinimal.h"

#define ANIMBP2FP_HAS_ANIM_AUTHORING (ENGINE_MAJOR_VERSION >= 5)
#define ANIMBP2FP_HAS_MODERN_RIGVM_AUTHORING \
	(ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8))

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#define ANIMBP2FP_APPLICATION_CONTEXT_FLAGS \
	(EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | \
	 EAutomationTestFlags::ServerContext | EAutomationTestFlags::CommandletContext | \
	 EAutomationTestFlags::ProgramContext)
#define ANIMBP2FP_AUTOMATION_TEST_FLAGS_TYPE EAutomationTestFlags
#elif ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#define ANIMBP2FP_APPLICATION_CONTEXT_FLAGS EAutomationTestFlags_ApplicationContextMask
#define ANIMBP2FP_AUTOMATION_TEST_FLAGS_TYPE EAutomationTestFlags
#else
#define ANIMBP2FP_APPLICATION_CONTEXT_FLAGS EAutomationTestFlags::ApplicationContextMask
#define ANIMBP2FP_AUTOMATION_TEST_FLAGS_TYPE uint32
#endif

#if ENGINE_MAJOR_VERSION < 5
template<typename T>
using TAnimBP2FPObjectPtr = T*;
#else
template<typename T>
using TAnimBP2FPObjectPtr = TObjectPtr<T>;
#endif

#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 4)
#define ANIMBP2FP_NO_SHRINKING false
#define ANIMBP2FP_GUID_HYPHENS EGuidFormats::DigitsWithHyphens
#else
#define ANIMBP2FP_NO_SHRINKING EAllowShrinking::No
#define ANIMBP2FP_GUID_HYPHENS EGuidFormats::DigitsWithHyphensLower
#endif
