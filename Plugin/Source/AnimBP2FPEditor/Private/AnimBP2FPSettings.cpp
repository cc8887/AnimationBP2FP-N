// AnimBP2FPSettings.cpp - Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPSettings.h"

UAnimBP2FPSettings::UAnimBP2FPSettings()
	: bAutoGenerateStub(true)
	, bGenerateOnStartup(true)
	, bGenerateOnReload(false)  // 默认关闭，避免频繁重新生成
	, StubOutputPath(TEXT("Intermediate/AnimLangStub/animlang-nodes-generated.rkt"))
	, bIncludeDeprecatedNodes(false)
	, bIncludeExperimentalNodes(false)
{
}