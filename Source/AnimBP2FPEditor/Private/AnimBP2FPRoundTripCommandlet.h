// Round-trip commandlet.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AnimBP2FPRoundTripCommandlet.generated.h"

// Executes round-trip validation.
UCLASS()
class UAnimBP2FPRoundTripCommandlet : public UCommandlet
{
	GENERATED_BODY()
	
public:
	UAnimBP2FPRoundTripCommandlet();
	
	virtual int32 Main(const FString& Params) override;
};
