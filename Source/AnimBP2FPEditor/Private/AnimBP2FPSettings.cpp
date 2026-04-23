// AnimBP2FPSettings.cpp - Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPSettings.h"

UAnimBP2FPSettings::UAnimBP2FPSettings()
	: StubOutputPath(TEXT("Intermediate/AnimLangStub/animlang-nodes-generated.rkt"))
	, AutoSyncMode(EBP2FPSyncMode::None)  // 默认关闭，手动开启
	, DSLOutputCategory(TEXT("AnimBP"))
	, bIncludeDeprecatedNodes(false)
	, bIncludeExperimentalNodes(false)
{
}