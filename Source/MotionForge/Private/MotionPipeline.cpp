// The pipeline base. Everything here is reflection over the subclass's own properties.

#include "MotionPipeline.h"

#include "IMotionProvider.h"
#include "MotionForge.h"

#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace MotionPipelinePrivate
{
	static EMotionOptionType TypeOf(const FProperty* Property, bool& bOutSupported)
	{
		bOutSupported = true;

		if (Property->IsA<FBoolProperty>())                                         { return EMotionOptionType::Bool; }
		if (Property->IsA<FEnumProperty>())                                         { return EMotionOptionType::Enum; }
		if (const FByteProperty* Byte = CastField<FByteProperty>(Property))         { return Byte->Enum ? EMotionOptionType::Enum : EMotionOptionType::Int; }
		if (Property->IsA<FIntProperty>())                                          { return EMotionOptionType::Int; }
		if (Property->IsA<FFloatProperty>() || Property->IsA<FDoubleProperty>())    { return EMotionOptionType::Float; }
		if (Property->IsA<FStrProperty>() || Property->IsA<FNameProperty>())        { return EMotionOptionType::Text; }

		// Structs, arrays and object references have no flat form. The window draws them; an agent is
		// not handed a string whose shape it would have to guess.
		bOutSupported = false;
		return EMotionOptionType::Text;
	}

	static const UEnum* EnumOf(const FProperty* Property)
	{
		if (const FEnumProperty* AsEnum = CastField<FEnumProperty>(Property))
		{
			return AsEnum->GetEnum();
		}
		if (const FByteProperty* AsByte = CastField<FByteProperty>(Property))
		{
			return AsByte->Enum;
		}
		return nullptr;
	}

	/** Enum values by their display name, which is where the vendor's spelling lives. */
	static FString EnumValueText(const UEnum* Enum, int64 Value)
	{
		const int32 Index = Enum->GetIndexByValue(Value);
		if (Index == INDEX_NONE)
		{
			return FString();
		}

		const FString Display = Enum->GetMetaData(TEXT("DisplayName"), Index);
		return Display.IsEmpty() ? Enum->GetNameStringByIndex(Index) : Display;
	}

	static FString ValueText(const FProperty* Property, const void* Container);

	/**
	 * The two EditCondition shapes pipelines use: a bool ("bFlag", "!bFlag") and a comparison with an
	 * enum value ("Prop == EType::Value", "!="). Anything else reads as met, so nothing is greyed on a
	 * guess.
	 */
	static bool EvaluateCondition(const UObject* Object, const FString& Condition)
	{
		FString Left, Right;
		bool bEquals = true;

		if (Condition.Split(TEXT("=="), &Left, &Right)) { bEquals = true; }
		else if (Condition.Split(TEXT("!="), &Left, &Right)) { bEquals = false; }
		else
		{
			FString Name = Condition.TrimStartAndEnd();
			const bool bNegate = Name.RemoveFromStart(TEXT("!"));
			if (const FBoolProperty* Bool = FindFProperty<FBoolProperty>(Object->GetClass(), FName(*Name)))
			{
				return Bool->GetPropertyValue_InContainer(Object) != bNegate;
			}
			return true;
		}

		Left.TrimStartAndEndInline();
		Right.TrimStartAndEndInline();

		const FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), FName(*Left));
		const UEnum* Enum = Property ? EnumOf(Property) : nullptr;
		if (!Enum)
		{
			return true;
		}

		int64 Raw = 0;
		const void* Value = Property->ContainerPtrToValuePtr<void>(Object);
		if (const FEnumProperty* AsEnum = CastField<FEnumProperty>(Property))
		{
			Raw = AsEnum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
		}
		else
		{
			Raw = *static_cast<const uint8*>(Value);
		}

		const int64 Wanted = Enum->GetValueByNameString(Right);
		return (Raw == Wanted) == bEquals;
	}

	/** The condition in the words the options use: "Only used when cfg_type is separated." */
	static FString DescribeCondition(const UObject* Object, const FString& Condition)
	{
		FString Left, Right;
		if (Condition.Split(TEXT("=="), &Left, &Right))
		{
			Left.TrimStartAndEndInline();
			Right.TrimStartAndEndInline();

			const FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), FName(*Left));
			const UEnum* Enum = Property ? EnumOf(Property) : nullptr;
			if (Property && Enum)
			{
				return FString::Printf(TEXT("Only used when %s is %s."),
					*UMotionPipeline::GetOptionKey(Property), *EnumValueText(Enum, Enum->GetValueByNameString(Right)));
			}
		}

		FString Name = Condition.TrimStartAndEnd();
		const bool bNegate = Name.RemoveFromStart(TEXT("!"));
		if (const FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), FName(*Name)))
		{
			return FString::Printf(TEXT("Only used when %s is %s."),
				*UMotionPipeline::GetOptionKey(Property), bNegate ? TEXT("false") : TEXT("true"));
		}

		return FString::Printf(TEXT("Only used when %s."), *Condition);
	}

	static FString ValueText(const FProperty* Property, const void* Container)
	{
		const void* Value = Property->ContainerPtrToValuePtr<void>(Container);

		if (const UEnum* Enum = EnumOf(Property))
		{
			int64 Raw = 0;
			if (const FEnumProperty* AsEnum = CastField<FEnumProperty>(Property))
			{
				Raw = AsEnum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
			}
			else
			{
				Raw = *static_cast<const uint8*>(Value);
			}
			return EnumValueText(Enum, Raw);
		}

		if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
		{
			return Bool->GetPropertyValue(Value) ? TEXT("true") : TEXT("false");
		}

		FString Out;
		Property->ExportTextItem_Direct(Out, Value, nullptr, nullptr, PPF_None);
		return Out;
	}
}

// -------------------------------------------------------------------------------------------------

bool UMotionPipeline::IsSettingProperty(const FProperty* Property)
{
	if (Property == nullptr || Property->GetOwnerClass() == UMotionPipeline::StaticClass())
	{
		return false;
	}

	return Property->HasAnyPropertyFlags(CPF_Edit)
		&& !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated);
}

FString UMotionPipeline::GetOptionKey(const FProperty* Property)
{
	if (Property == nullptr)
	{
		return FString();
	}

	const FString Wire = Property->GetMetaData(TEXT("WireName"));
	return Wire.IsEmpty() ? Property->GetName() : Wire;
}

FProperty* UMotionPipeline::FindOptionProperty(const FString& Key) const
{
	for (TFieldIterator<FProperty> It(GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsSettingProperty(Property))
		{
			continue;
		}

		if (GetOptionKey(Property).Equals(Key, ESearchCase::IgnoreCase)
			|| Property->GetName().Equals(Key, ESearchCase::IgnoreCase))
		{
			return Property;
		}
	}
	return nullptr;
}

bool UMotionPipeline::SetModelId(const FString& ModelId, FString& OutError)
{
	if (FMotionForgeModule* Module = FMotionForgeModule::GetPtr())
	{
		if (TSharedPtr<IMotionProvider> Provider = Module->FindProvider(GetProviderId()))
		{
			const TArray<FMotionModelInfo> Models = Provider->GetModels();

			const bool bKnown = Models.Num() == 0 || Models.ContainsByPredicate(
				[&ModelId](const FMotionModelInfo& Model) { return Model.Id == ModelId; });

			if (!bKnown)
			{
				OutError = FString::Printf(
					TEXT("%s does not offer the model '%s'. It offers: %s."),
					*Provider->GetDisplayName(), *ModelId,
					*FString::JoinBy(Models, TEXT(", "), [](const FMotionModelInfo& M) { return M.Id; }));
				return false;
			}
		}
	}

	return SetOption(TEXT("model"), ModelId, OutError);
}

TArray<FString> UMotionPipeline::DescribeSent(const FMotionSubmitRequest& Request) const
{
	// The default is honest for a pipeline whose every setting is sent as it stands. A pipeline whose
	// fields interact - one that sends nothing for a setting left at the model's default - overrides
	// this, so the record says what the provider received rather than what the panel showed.
	TArray<FString> Lines;

	for (const FMotionPipelineOption& Option : DescribeOptions())
	{
		if (Option.DisabledReason.IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("%s=%s"), *Option.Key, *Option.Value));
		}
	}
	return Lines;
}

FString UMotionPipeline::Signature() const
{
	FString Out = GetClass()->GetName() + TEXT(":");

	for (TFieldIterator<FProperty> It(GetClass()); It; ++It)
	{
		const FProperty* Property = *It;
		if (!IsSettingProperty(Property))
		{
			continue;
		}

		Out += FString::Printf(TEXT("%s=%s;"),
			*GetOptionKey(Property), *MotionPipelinePrivate::ValueText(Property, this));
	}
	return Out;
}

TArray<FMotionPipelineOption> UMotionPipeline::DescribeOptions() const
{
	TArray<FMotionPipelineOption> Options;

	const UObject* Defaults = GetClass()->GetDefaultObject();

	for (TFieldIterator<FProperty> It(GetClass()); It; ++It)
	{
		const FProperty* Property = *It;
		if (!IsSettingProperty(Property))
		{
			continue;
		}

		bool bSupported = false;
		const EMotionOptionType Type = MotionPipelinePrivate::TypeOf(Property, bSupported);
		if (!bSupported)
		{
			continue;
		}

		FMotionPipelineOption& Option = Options.AddDefaulted_GetRef();
		Option.Key          = GetOptionKey(Property);
		Option.Type         = Type;
		Option.Tooltip      = Property->GetToolTipText().ToString();
		Option.Value        = MotionPipelinePrivate::ValueText(Property, this);
		Option.DefaultValue = MotionPipelinePrivate::ValueText(Property, Defaults);
		Option.bAdvanced    = Property->HasAnyPropertyFlags(CPF_AdvancedDisplay);

		Option.Min = Property->GetMetaData(TEXT("ClampMin"));
		Option.Max = Property->GetMetaData(TEXT("ClampMax"));

		if (const UEnum* Enum = MotionPipelinePrivate::EnumOf(Property))
		{
			// NumEnums counts the generated _MAX, which nobody may choose.
			for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
			{
				if (!Enum->HasMetaData(TEXT("Hidden"), Index))
				{
					Option.AllowedValues.Add(MotionPipelinePrivate::EnumValueText(Enum, Enum->GetValueByIndex(Index)));
				}
			}
		}

		// A picker fed by a function - the model list - reports what it would offer.
		const FString GetOptions = Property->GetMetaData(TEXT("GetOptions"));
		if (!GetOptions.IsEmpty())
		{
			if (UFunction* Function = GetClass()->FindFunctionByName(FName(*GetOptions)))
			{
				TArray<FString> Values;
				const_cast<UMotionPipeline*>(this)->ProcessEvent(Function, &Values);
				Option.AllowedValues = MoveTemp(Values);
			}
		}

		// A setting this configuration makes meaningless, and why, from the same condition that greys it
		// in the details panel. The panel evaluates EditCondition itself, so it is evaluated here too.
		const FString EditCondition = Property->GetMetaData(TEXT("EditCondition"));
		if (!EditCondition.IsEmpty() && !MotionPipelinePrivate::EvaluateCondition(this, EditCondition))
		{
			Option.DisabledReason = MotionPipelinePrivate::DescribeCondition(this, EditCondition);
		}
	}

	return Options;
}

bool UMotionPipeline::SetOption(const FString& Key, const FString& Value, FString& OutError)
{
	FProperty* Property = FindOptionProperty(Key);
	if (Property == nullptr)
	{
		TArray<FString> Keys;
		for (const FMotionPipelineOption& Option : DescribeOptions())
		{
			Keys.Add(Option.Key);
		}

		OutError = FString::Printf(TEXT("%s has no setting '%s'. It has: %s."),
			*GetClass()->GetDisplayNameText().ToString(), *Key, *FString::Join(Keys, TEXT(", ")));
		return false;
	}

	void* Target = Property->ContainerPtrToValuePtr<void>(this);

	// Enums by their display name first, because that is the vendor's spelling and the one Describe
	// Options reports; by their C++ name as a fallback.
	if (const UEnum* Enum = MotionPipelinePrivate::EnumOf(Property))
	{
		int64 Found = INDEX_NONE;
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			const int64 Raw = Enum->GetValueByIndex(Index);
			if (MotionPipelinePrivate::EnumValueText(Enum, Raw).Equals(Value, ESearchCase::IgnoreCase)
				|| Enum->GetNameStringByIndex(Index).Equals(Value, ESearchCase::IgnoreCase))
			{
				Found = Raw;
				break;
			}
		}

		if (Found == INDEX_NONE)
		{
			TArray<FString> Allowed;
			for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
			{
				Allowed.Add(MotionPipelinePrivate::EnumValueText(Enum, Enum->GetValueByIndex(Index)));
			}
			OutError = FString::Printf(TEXT("'%s' is not a value of %s. Use one of: %s."),
				*Value, *Key, *FString::Join(Allowed, TEXT(", ")));
			return false;
		}

		Modify();
		if (const FEnumProperty* AsEnum = CastField<FEnumProperty>(Property))
		{
			AsEnum->GetUnderlyingProperty()->SetIntPropertyValue(Target, Found);
		}
		else
		{
			*static_cast<uint8*>(Target) = static_cast<uint8>(Found);
		}
	}
	else
	{
		Modify();

		const TCHAR* Result = Property->ImportText_Direct(*Value, Target, this, PPF_None);
		if (Result == nullptr)
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid value for %s."), *Value, *Key);
			return false;
		}
	}

	// Respect the declared range, the same way the details panel does.
	const FString Min = Property->GetMetaData(TEXT("ClampMin"));
	const FString Max = Property->GetMetaData(TEXT("ClampMax"));

	if (FIntProperty* AsInt = CastField<FIntProperty>(Property))
	{
		int32 V = AsInt->GetPropertyValue(Target);
		if (!Min.IsEmpty()) { V = FMath::Max(V, FCString::Atoi(*Min)); }
		if (!Max.IsEmpty()) { V = FMath::Min(V, FCString::Atoi(*Max)); }
		AsInt->SetPropertyValue(Target, V);
	}
	else if (FFloatProperty* AsFloat = CastField<FFloatProperty>(Property))
	{
		float V = AsFloat->GetPropertyValue(Target);
		if (!Min.IsEmpty()) { V = FMath::Max(V, FCString::Atof(*Min)); }
		if (!Max.IsEmpty()) { V = FMath::Min(V, FCString::Atof(*Max)); }
		AsFloat->SetPropertyValue(Target, V);
	}

	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	PostEditChangeProperty(Event);

	if (UObject* Outer = GetOuter())
	{
		Outer->MarkPackageDirty();
	}

	OutError.Reset();
	return true;
}
