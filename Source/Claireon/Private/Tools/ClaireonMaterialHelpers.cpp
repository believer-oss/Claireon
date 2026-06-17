// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialHelpers.h"

#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionStaticComponentMaskParameter.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "MaterialTypes.h"
#include "RenderUtils.h"
#include "ShaderCompiler.h"
#include "StaticParameterSet.h"
#include "Misc/EngineVersionComparison.h"

#include "Engine/Texture.h"
#include "FileHelpers.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace ClaireonMaterialHelpers
{
	// ------------------------------------------------------------------
	// Internal: per-material FMaterialUpdateContext stash (for BeginEdit/EndEdit batching)
	// ------------------------------------------------------------------

	namespace
	{
		static TMap<TWeakObjectPtr<UMaterial>, TUniquePtr<FMaterialUpdateContext>>& GetEditContexts()
		{
			static TMap<TWeakObjectPtr<UMaterial>, TUniquePtr<FMaterialUpdateContext>> Contexts;
			return Contexts;
		}

		/** Static map of legacy material attribute names -> EMaterialProperty. */
		static const TMap<FString, EMaterialProperty>& GetAttributeNameMap()
		{
			static TMap<FString, EMaterialProperty> Map = []()
			{
				TMap<FString, EMaterialProperty> M;
				M.Add(TEXT("BaseColor"), MP_BaseColor);
				M.Add(TEXT("Metallic"), MP_Metallic);
				M.Add(TEXT("Specular"), MP_Specular);
				M.Add(TEXT("Roughness"), MP_Roughness);
				M.Add(TEXT("Anisotropy"), MP_Anisotropy);
				M.Add(TEXT("EmissiveColor"), MP_EmissiveColor);
				M.Add(TEXT("Opacity"), MP_Opacity);
				M.Add(TEXT("OpacityMask"), MP_OpacityMask);
				M.Add(TEXT("Normal"), MP_Normal);
				M.Add(TEXT("Tangent"), MP_Tangent);
				M.Add(TEXT("WorldPositionOffset"), MP_WorldPositionOffset);
				M.Add(TEXT("SubsurfaceColor"), MP_SubsurfaceColor);
				M.Add(TEXT("AmbientOcclusion"), MP_AmbientOcclusion);
				M.Add(TEXT("Refraction"), MP_Refraction);
				M.Add(TEXT("PixelDepthOffset"), MP_PixelDepthOffset);
				M.Add(TEXT("CustomizedUVs0"), MP_CustomizedUVs0);
				M.Add(TEXT("CustomizedUVs1"), MP_CustomizedUVs1);
				M.Add(TEXT("CustomizedUVs2"), MP_CustomizedUVs2);
				M.Add(TEXT("CustomizedUVs3"), MP_CustomizedUVs3);
				M.Add(TEXT("CustomizedUVs4"), MP_CustomizedUVs4);
				M.Add(TEXT("CustomizedUVs5"), MP_CustomizedUVs5);
				M.Add(TEXT("CustomizedUVs6"), MP_CustomizedUVs6);
				M.Add(TEXT("CustomizedUVs7"), MP_CustomizedUVs7);
				// Substrate-only - validated separately at the call site.
				M.Add(TEXT("FrontMaterial"), MP_FrontMaterial);
				return M;
			}();
			return Map;
		}

		/** Strip common prefixes/suffixes from a material expression class name for short-name display. */
		static FString StripExpressionShortName(const FString& InName)
		{
			FString Out = InName;
			Out.RemoveFromStart(TEXT("U"));
			Out.RemoveFromStart(TEXT("MaterialExpression"));
			return Out.IsEmpty() ? InName : Out;
		}
	} // anonymous namespace

	// ============================================================================
	// Asset loading
	// ============================================================================

	UMaterial* LoadMaterialAsset(const FString& AssetPath, FString& OutError)
	{
		ClaireonPathResolver::FResolveResult Resolve = ClaireonPathResolver::Resolve(AssetPath);
		if (!Resolve.bSuccess)
		{
			OutError = Resolve.Error;
			return nullptr;
		}

		const FString ResolvedPath = Resolve.ResolvedPath.Path;
		FSoftObjectPath SoftPath(ResolvedPath);
		UObject* Loaded = SoftPath.TryLoad();
		if (!Loaded)
		{
			OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
			return nullptr;
		}

		UMaterial* Material = Cast<UMaterial>(Loaded);
		if (!Material)
		{
			OutError = FString::Printf(TEXT("Asset at %s is not a UMaterial (actual type: %s)"),
				*ResolvedPath, *Loaded->GetClass()->GetName());
			return nullptr;
		}

		return Material;
	}

	UMaterialInstanceConstant* LoadMaterialInstanceAsset(const FString& AssetPath, FString& OutError)
	{
		ClaireonPathResolver::FResolveResult Resolve = ClaireonPathResolver::Resolve(AssetPath);
		if (!Resolve.bSuccess)
		{
			OutError = Resolve.Error;
			return nullptr;
		}

		const FString ResolvedPath = Resolve.ResolvedPath.Path;
		FSoftObjectPath SoftPath(ResolvedPath);
		UObject* Loaded = SoftPath.TryLoad();
		if (!Loaded)
		{
			OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
			return nullptr;
		}

		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Loaded);
		if (!Instance)
		{
			OutError = FString::Printf(TEXT("Asset at %s is not a UMaterialInstanceConstant (actual type: %s)"),
				*ResolvedPath, *Loaded->GetClass()->GetName());
			return nullptr;
		}

		return Instance;
	}

	// ============================================================================
	// Expression class + identifier resolution
	// ============================================================================

	UClass* ResolveExpressionClass(const FString& ClassName, FString& OutError)
	{
		if (ClassName.IsEmpty())
		{
			OutError = TEXT("Expression class name is empty");
			return nullptr;
		}

		// Build candidate names to try in priority order.
		TArray<FString> Candidates;
		Candidates.Add(ClassName);
		Candidates.Add(FString::Printf(TEXT("U%s"), *ClassName));
		Candidates.Add(FString::Printf(TEXT("MaterialExpression%s"), *ClassName));
		Candidates.Add(FString::Printf(TEXT("UMaterialExpression%s"), *ClassName));

		// Direct lookup by short name first.
		for (const FString& Candidate : Candidates)
		{
			UClass* Found = FindFirstObject<UClass>(*Candidate, EFindFirstObjectOptions::None);
			if (Found && Found->IsChildOf(UMaterialExpression::StaticClass()) && !Found->HasAnyClassFlags(CLASS_Abstract))
			{
				return Found;
			}
		}

		// Fuzzy fallback: scan all subclasses.
		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(UMaterialExpression::StaticClass(), DerivedClasses, true);

		for (UClass* Class : DerivedClasses)
		{
			if (!Class || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
			{
				continue;
			}

			const FString ClassShort = StripExpressionShortName(Class->GetName());
			if (ClassShort.Equals(ClassName, ESearchCase::IgnoreCase) ||
				Class->GetName().Equals(ClassName, ESearchCase::IgnoreCase) ||
				Class->GetName().Equals(FString::Printf(TEXT("MaterialExpression%s"), *ClassName), ESearchCase::IgnoreCase))
			{
				return Class;
			}
		}

		OutError = FString::Printf(TEXT("Could not find UMaterialExpression subclass: %s"), *ClassName);
		return nullptr;
	}

	UMaterialExpression* FindExpressionByIdentifier(UMaterial* Material, const FString& Identifier, int32& OutIndex)
	{
		OutIndex = INDEX_NONE;
		if (!Material || Identifier.IsEmpty())
		{
			return nullptr;
		}

		TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();

		// Priority 1: numeric index.
		if (Identifier.IsNumeric())
		{
			const int32 Index = FCString::Atoi(*Identifier);
			if (Index >= 0 && Index < Expressions.Num())
			{
				OutIndex = Index;
				return Expressions[Index];
			}
		}

		// Priority 2: parameter name.
		for (int32 i = 0; i < Expressions.Num(); ++i)
		{
			UMaterialExpression* Expr = Expressions[i];
			if (!Expr)
			{
				continue;
			}
			if (Expr->HasAParameterName() && Expr->GetParameterName().ToString().Equals(Identifier, ESearchCase::IgnoreCase))
			{
				OutIndex = i;
				return Expr;
			}
		}

		// Priority 3: GetDescription() exact match.
		for (int32 i = 0; i < Expressions.Num(); ++i)
		{
			UMaterialExpression* Expr = Expressions[i];
			if (!Expr)
			{
				continue;
			}
			if (Expr->GetDescription().Equals(Identifier, ESearchCase::IgnoreCase))
			{
				OutIndex = i;
				return Expr;
			}
		}

		// Priority 4: class short name when unique.
		int32 ShortMatchIndex = INDEX_NONE;
		UMaterialExpression* ShortMatch = nullptr;
		int32 ShortMatchCount = 0;
		for (int32 i = 0; i < Expressions.Num(); ++i)
		{
			UMaterialExpression* Expr = Expressions[i];
			if (!Expr)
			{
				continue;
			}
			const FString ShortName = StripExpressionShortName(Expr->GetClass()->GetName());
			if (ShortName.Equals(Identifier, ESearchCase::IgnoreCase))
			{
				ShortMatch = Expr;
				ShortMatchIndex = i;
				++ShortMatchCount;
			}
		}
		if (ShortMatchCount == 1)
		{
			OutIndex = ShortMatchIndex;
			return ShortMatch;
		}

		return nullptr;
	}

	// ============================================================================
	// Markdown formatting (for inspect tools)
	// ============================================================================

	namespace
	{
		static FString GetExpressionShortName(const UMaterialExpression* Expr)
		{
			if (!Expr)
			{
				return TEXT("(null)");
			}
			return StripExpressionShortName(Expr->GetClass()->GetName());
		}

		/**
		 * Find which output index of `Source` corresponds to the FExpressionInput value.
		 * Returns the output's display name, or "Out" if unnamed.
		 */
		static FString GetSourceOutputDisplayName(UMaterialExpression* Source, int32 OutputIndex)
		{
			if (!Source)
			{
				return TEXT("?");
			}
			TArray<FExpressionOutput>& Outputs = Source->GetOutputs();
			if (OutputIndex >= 0 && OutputIndex < Outputs.Num())
			{
				const FName OutName = Outputs[OutputIndex].OutputName;
				if (OutName != NAME_None)
				{
					return OutName.ToString();
				}
			}
			return TEXT("Out");
		}

		static FString FormatLegacyAttributeRow(const TCHAR* AttributeLabel, const FExpressionInput& Input, const TConstArrayView<TObjectPtr<UMaterialExpression>>& Expressions)
		{
			if (!Input.Expression)
			{
				return FString();
			}
			int32 SourceIdx = Expressions.IndexOfByKey(Input.Expression);
			const FString SourceName = SourceIdx != INDEX_NONE
				? FString::Printf(TEXT("[%d] %s"), SourceIdx, *GetExpressionShortName(Input.Expression))
				: GetExpressionShortName(Input.Expression);
			const FString OutputName = GetSourceOutputDisplayName(Input.Expression, Input.OutputIndex);
			return FString::Printf(TEXT("- %s: %s.%s\n"), AttributeLabel, *SourceName, *OutputName);
		}
	}

	FString FormatMaterialStructure(const UMaterial* Material, const FString& DetailLevel)
	{
		if (!Material)
		{
			return TEXT("(null material)\n");
		}

		const bool bFull = DetailLevel.Equals(TEXT("full"), ESearchCase::IgnoreCase);

		FString Out;
		Out += FString::Printf(TEXT("# Material: %s\n\n"), *Material->GetPathName());
		Out += FString::Printf(TEXT("- Class: UMaterial\n"));
		Out += FString::Printf(TEXT("- Domain: %s\n"), *UEnum::GetValueAsString(Material->MaterialDomain.GetValue()));
		Out += FString::Printf(TEXT("- ShadingModel: %s\n"), *UEnum::GetValueAsString(Material->GetShadingModels().GetFirstShadingModel()));
		Out += FString::Printf(TEXT("- BlendMode: %s\n"), *UEnum::GetValueAsString(Material->BlendMode.GetValue()));

		// Usage flag roll-up (only emit for non-default true values).
		TArray<FString> UsageFlags;
		if (Material->bUsedWithSkeletalMesh) UsageFlags.Add(TEXT("SkeletalMesh"));
		if (Material->bUsedWithStaticLighting) UsageFlags.Add(TEXT("StaticLighting"));
		if (Material->bUsedWithParticleSprites) UsageFlags.Add(TEXT("ParticleSprites"));
		if (Material->bUsedWithBeamTrails) UsageFlags.Add(TEXT("BeamTrails"));
		if (Material->bUsedWithMeshParticles) UsageFlags.Add(TEXT("MeshParticles"));
		if (Material->bUsedWithNiagaraSprites) UsageFlags.Add(TEXT("NiagaraSprites"));
		if (Material->bUsedWithNiagaraRibbons) UsageFlags.Add(TEXT("NiagaraRibbons"));
		if (Material->bUsedWithNiagaraMeshParticles) UsageFlags.Add(TEXT("NiagaraMeshParticles"));
		if (Material->bUsedWithMorphTargets) UsageFlags.Add(TEXT("MorphTargets"));
		if (Material->bUsedWithSplineMeshes) UsageFlags.Add(TEXT("SplineMeshes"));
		if (Material->bUsedWithInstancedStaticMeshes) UsageFlags.Add(TEXT("InstancedStaticMeshes"));
		if (Material->bUsedWithGeometryCollections) UsageFlags.Add(TEXT("GeometryCollections"));
		if (UsageFlags.Num() > 0)
		{
			Out += FString::Printf(TEXT("- Usage: %s\n"), *FString::Join(UsageFlags, TEXT(", ")));
		}

		TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();
		Out += FString::Printf(TEXT("- Expressions: %d\n\n"), Expressions.Num());

		// Material attribute connections (legacy pins via UMaterialEditorOnlyData).
		if (const UMaterialEditorOnlyData* EOData = Material->GetEditorOnlyData())
		{
			FString Attrs;
			Attrs += FormatLegacyAttributeRow(TEXT("BaseColor"), EOData->BaseColor, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("Metallic"), EOData->Metallic, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("Specular"), EOData->Specular, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("Roughness"), EOData->Roughness, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("Anisotropy"), EOData->Anisotropy, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("Normal"), EOData->Normal, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("Tangent"), EOData->Tangent, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("EmissiveColor"), EOData->EmissiveColor, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("Opacity"), EOData->Opacity, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("OpacityMask"), EOData->OpacityMask, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("WorldPositionOffset"), EOData->WorldPositionOffset, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("SubsurfaceColor"), EOData->SubsurfaceColor, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("AmbientOcclusion"), EOData->AmbientOcclusion, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("Refraction"), EOData->Refraction, Expressions);
			Attrs += FormatLegacyAttributeRow(TEXT("PixelDepthOffset"), EOData->PixelDepthOffset, Expressions);

			for (int32 UVIdx = 0; UVIdx < 8; ++UVIdx)
			{
				const FString Label = FString::Printf(TEXT("CustomizedUVs%d"), UVIdx);
				Attrs += FormatLegacyAttributeRow(*Label, EOData->CustomizedUVs[UVIdx], Expressions);
			}

			if (Substrate::IsSubstrateEnabled())
			{
				Attrs += FormatLegacyAttributeRow(TEXT("FrontMaterial"), EOData->FrontMaterial, Expressions);
			}

			if (!Attrs.IsEmpty())
			{
				Out += TEXT("## Material Attribute Connections\n\n");
				Out += Attrs;
				Out += TEXT("\n");
			}
		}

		Out += FormatParameterSummary(Material);

		if (bFull)
		{
			Out += TEXT("\n## Expressions\n\n");
			for (int32 i = 0; i < Expressions.Num(); ++i)
			{
				Out += FormatExpressionDetail(Material, Expressions[i], i, /*bIncludeConnections=*/true);
				Out += TEXT("\n");
			}
		}

		return Out;
	}

	FString FormatExpressionDetail(const UMaterial* Material, const UMaterialExpression* Expression, int32 Index, bool bIncludeConnections)
	{
		if (!Expression)
		{
			return TEXT("(null expression)\n");
		}

		FString Out;
		const FString ShortName = GetExpressionShortName(Expression);
		Out += FString::Printf(TEXT("### [%d] %s"), Index, *ShortName);

		if (Expression->HasAParameterName())
		{
			const FName ParamName = Expression->GetParameterName();
			if (ParamName != NAME_None)
			{
				Out += FString::Printf(TEXT(" \"%s\""), *ParamName.ToString());
			}
		}
		Out += TEXT("\n");

		Out += FString::Printf(TEXT("- Position: (%d, %d)\n"),
			Expression->MaterialExpressionEditorX,
			Expression->MaterialExpressionEditorY);

		const FString Desc = Expression->GetDescription();
		if (!Desc.IsEmpty() && Desc != ShortName)
		{
			Out += FString::Printf(TEXT("- Description: %s\n"), *Desc);
		}

		// Outputs
		UMaterialExpression* MutableExpr = const_cast<UMaterialExpression*>(Expression);
		const TArray<FExpressionOutput>& Outputs = MutableExpr->GetOutputs();
		if (Outputs.Num() > 0)
		{
			TArray<FString> OutNames;
			for (const FExpressionOutput& Output : Outputs)
			{
				OutNames.Add(Output.OutputName != NAME_None ? Output.OutputName.ToString() : TEXT("Out"));
			}
			Out += FString::Printf(TEXT("- Outputs: %s\n"), *FString::Join(OutNames, TEXT(", ")));
		}

		// Inputs and their source connections.
		if (bIncludeConnections)
		{
			TConstArrayView<TObjectPtr<UMaterialExpression>> AllExprs = Material ? Material->GetExpressions() : TConstArrayView<TObjectPtr<UMaterialExpression>>();

			for (int32 InputIdx = 0; ; ++InputIdx)
			{
				FExpressionInput* Input = MutableExpr->GetInput(InputIdx);
				if (!Input)
				{
					break;
				}
				const FName InputName = MutableExpr->GetInputName(InputIdx);
				const FString InputLabel = InputName != NAME_None ? InputName.ToString() : FString::Printf(TEXT("Input%d"), InputIdx);
				if (Input->Expression)
				{
					int32 SourceIdx = AllExprs.IndexOfByKey(Input->Expression);
					const FString SourceLabel = SourceIdx != INDEX_NONE
						? FString::Printf(TEXT("[%d] %s"), SourceIdx, *GetExpressionShortName(Input->Expression))
						: GetExpressionShortName(Input->Expression);
					const FString SourceOutput = GetSourceOutputDisplayName(Input->Expression, Input->OutputIndex);
					Out += FString::Printf(TEXT("- In %s <- %s.%s\n"), *InputLabel, *SourceLabel, *SourceOutput);
				}
			}
		}

		return Out;
	}

	FString FormatParameterSummary(const UMaterial* Material)
	{
		if (!Material)
		{
			return FString();
		}

		FString Out;
		TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();

		auto ParamGroup = [](const UMaterialExpressionParameter* Param) -> FString
		{
			if (!Param || Param->Group == NAME_None) return FString();
			return Param->Group.ToString();
		};

		// Scalar parameters.
		{
			TArray<FString> Rows;
			for (UMaterialExpression* Expr : Expressions)
			{
				if (UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expr))
				{
					Rows.Add(FString::Printf(TEXT("| %s | Scalar | %f | %s |"),
						*Scalar->ParameterName.ToString(), Scalar->DefaultValue, *ParamGroup(Scalar)));
				}
			}
			if (Rows.Num() > 0)
			{
				Out += TEXT("\n## Scalar Parameters\n\n");
				Out += TEXT("| Name | Type | Default | Group |\n");
				Out += TEXT("|------|------|---------|-------|\n");
				for (const FString& Row : Rows) { Out += Row + TEXT("\n"); }
			}
		}

		// Vector parameters.
		{
			TArray<FString> Rows;
			for (UMaterialExpression* Expr : Expressions)
			{
				if (UMaterialExpressionVectorParameter* Vec = Cast<UMaterialExpressionVectorParameter>(Expr))
				{
					Rows.Add(FString::Printf(TEXT("| %s | Vector | (%f, %f, %f, %f) | %s |"),
						*Vec->ParameterName.ToString(),
						Vec->DefaultValue.R, Vec->DefaultValue.G, Vec->DefaultValue.B, Vec->DefaultValue.A,
						*ParamGroup(Vec)));
				}
			}
			if (Rows.Num() > 0)
			{
				Out += TEXT("\n## Vector Parameters\n\n");
				Out += TEXT("| Name | Type | Default | Group |\n");
				Out += TEXT("|------|------|---------|-------|\n");
				for (const FString& Row : Rows) { Out += Row + TEXT("\n"); }
			}
		}

		// Texture parameters.
		{
			TArray<FString> Rows;
			for (UMaterialExpression* Expr : Expressions)
			{
				if (UMaterialExpressionTextureSampleParameter* Tex = Cast<UMaterialExpressionTextureSampleParameter>(Expr))
				{
					const FString TexName = Tex->Texture ? Tex->Texture->GetPathName() : TEXT("(none)");
					const FString GroupStr = Tex->Group != NAME_None ? Tex->Group.ToString() : FString();
					Rows.Add(FString::Printf(TEXT("| %s | Texture | %s | %s |"),
						*Tex->ParameterName.ToString(), *TexName, *GroupStr));
				}
			}
			if (Rows.Num() > 0)
			{
				Out += TEXT("\n## Texture Parameters\n\n");
				Out += TEXT("| Name | Type | Default | Group |\n");
				Out += TEXT("|------|------|---------|-------|\n");
				for (const FString& Row : Rows) { Out += Row + TEXT("\n"); }
			}
		}

		// Static switch parameters.
		{
			TArray<FString> Rows;
			for (UMaterialExpression* Expr : Expressions)
			{
				if (UMaterialExpressionStaticSwitchParameter* SS = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
				{
					Rows.Add(FString::Printf(TEXT("| %s | StaticSwitch | %s | %s |"),
						*SS->ParameterName.ToString(),
						SS->DefaultValue ? TEXT("true") : TEXT("false"),
						*ParamGroup(SS)));
				}
			}
			if (Rows.Num() > 0)
			{
				Out += TEXT("\n## Static Switch Parameters\n\n");
				Out += TEXT("| Name | Type | Default | Group |\n");
				Out += TEXT("|------|------|---------|-------|\n");
				for (const FString& Row : Rows) { Out += Row + TEXT("\n"); }
			}
		}

		// Static component mask parameters.
		{
			TArray<FString> Rows;
			for (UMaterialExpression* Expr : Expressions)
			{
				if (UMaterialExpressionStaticComponentMaskParameter* M = Cast<UMaterialExpressionStaticComponentMaskParameter>(Expr))
				{
					Rows.Add(FString::Printf(TEXT("| %s | StaticComponentMask | (R=%s, G=%s, B=%s, A=%s) | %s |"),
						*M->ParameterName.ToString(),
						M->DefaultR ? TEXT("1") : TEXT("0"),
						M->DefaultG ? TEXT("1") : TEXT("0"),
						M->DefaultB ? TEXT("1") : TEXT("0"),
						M->DefaultA ? TEXT("1") : TEXT("0"),
						*ParamGroup(M)));
				}
			}
			if (Rows.Num() > 0)
			{
				Out += TEXT("\n## Static Component Mask Parameters\n\n");
				Out += TEXT("| Name | Type | Default | Group |\n");
				Out += TEXT("|------|------|---------|-------|\n");
				for (const FString& Row : Rows) { Out += Row + TEXT("\n"); }
			}
		}

		return Out;
	}

	FString FormatMaterialInstance(const UMaterialInstanceConstant* Instance)
	{
		if (!Instance)
		{
			return TEXT("(null material instance)\n");
		}

		FString Out;
		Out += FString::Printf(TEXT("# MaterialInstanceConstant: %s\n\n"), *Instance->GetPathName());

		// Parent chain walk.
		Out += TEXT("## Parent Chain\n\n");
		{
			TArray<UMaterialInterface*> Chain;
			UMaterialInterface* Cursor = Instance->Parent;
			int32 Safety = 0;
			while (Cursor && Safety++ < 32)
			{
				Chain.Add(Cursor);
				if (UMaterialInstance* MI = Cast<UMaterialInstance>(Cursor))
				{
					Cursor = MI->Parent;
				}
				else
				{
					break;
				}
			}
			if (Chain.Num() == 0)
			{
				Out += TEXT("- (no parent)\n");
			}
			else
			{
				for (int32 i = 0; i < Chain.Num(); ++i)
				{
					Out += FString::Printf(TEXT("%d. %s (%s)\n"),
						i + 1,
						*Chain[i]->GetPathName(),
						*Chain[i]->GetClass()->GetName());
				}
			}
			Out += TEXT("\n");
		}

		auto FormatParamRow = [Instance](const FName& Name, const FString& InheritedStr, const FString& OverrideStr) -> FString
		{
			FName Group;
			Instance->GetGroupName(FHashedMaterialParameterInfo(Name), Group);
			const FString GroupStr = Group != NAME_None ? Group.ToString() : FString();
			return FString::Printf(TEXT("| %s | %s | %s | %s |\n"),
				*Name.ToString(), *InheritedStr, *OverrideStr, *GroupStr);
		};

		// Scalar parameters.
		{
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Guids;
			Instance->GetAllParameterInfoOfType(EMaterialParameterType::Scalar, Infos, Guids);
			if (Infos.Num() > 0)
			{
				Out += TEXT("## Scalar Parameters\n\n");
				Out += TEXT("| Name | Inherited Value | Override Value | Group |\n");
				Out += TEXT("|------|-----------------|----------------|-------|\n");
				for (const FMaterialParameterInfo& Info : Infos)
				{
					float InheritedValue = 0.0f;
					Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(Info.Name), InheritedValue);
					FString OverrideStr;
					for (const FScalarParameterValue& Override : Instance->ScalarParameterValues)
					{
						if (Override.ParameterInfo.Name == Info.Name)
						{
							OverrideStr = FString::Printf(TEXT("%f"), Override.ParameterValue);
							break;
						}
					}
					Out += FormatParamRow(Info.Name,
						FString::Printf(TEXT("%f"), InheritedValue),
						OverrideStr);
				}
				Out += TEXT("\n");
			}
		}

		// Vector parameters.
		{
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Guids;
			Instance->GetAllParameterInfoOfType(EMaterialParameterType::Vector, Infos, Guids);
			if (Infos.Num() > 0)
			{
				Out += TEXT("## Vector Parameters\n\n");
				Out += TEXT("| Name | Inherited Value | Override Value | Group |\n");
				Out += TEXT("|------|-----------------|----------------|-------|\n");
				for (const FMaterialParameterInfo& Info : Infos)
				{
					FLinearColor InheritedValue(ForceInit);
					Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(Info.Name), InheritedValue);
					FString OverrideStr;
					for (const FVectorParameterValue& Override : Instance->VectorParameterValues)
					{
						if (Override.ParameterInfo.Name == Info.Name)
						{
							OverrideStr = FString::Printf(TEXT("(%f, %f, %f, %f)"),
								Override.ParameterValue.R, Override.ParameterValue.G,
								Override.ParameterValue.B, Override.ParameterValue.A);
							break;
						}
					}
					Out += FormatParamRow(Info.Name,
						FString::Printf(TEXT("(%f, %f, %f, %f)"),
							InheritedValue.R, InheritedValue.G, InheritedValue.B, InheritedValue.A),
						OverrideStr);
				}
				Out += TEXT("\n");
			}
		}

		// Texture parameters.
		{
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Guids;
			Instance->GetAllParameterInfoOfType(EMaterialParameterType::Texture, Infos, Guids);
			if (Infos.Num() > 0)
			{
				Out += TEXT("## Texture Parameters\n\n");
				Out += TEXT("| Name | Inherited Value | Override Value | Group |\n");
				Out += TEXT("|------|-----------------|----------------|-------|\n");
				for (const FMaterialParameterInfo& Info : Infos)
				{
					UTexture* InheritedValue = nullptr;
					Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(Info.Name), InheritedValue);
					const FString InheritedStr = InheritedValue ? InheritedValue->GetPathName() : FString(TEXT("(none)"));
					FString OverrideStr;
					for (const FTextureParameterValue& Override : Instance->TextureParameterValues)
					{
						if (Override.ParameterInfo.Name == Info.Name)
						{
							OverrideStr = Override.ParameterValue ? Override.ParameterValue->GetPathName() : FString(TEXT("(none)"));
							break;
						}
					}
					Out += FormatParamRow(Info.Name, InheritedStr, OverrideStr);
				}
				Out += TEXT("\n");
			}
		}

		// Static switch parameters.
		{
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Guids;
			Instance->GetAllParameterInfoOfType(EMaterialParameterType::StaticSwitch, Infos, Guids);
			if (Infos.Num() > 0)
			{
				Out += TEXT("## Static Switch Parameters\n\n");
				Out += TEXT("| Name | Inherited Value | Override Value | Group |\n");
				Out += TEXT("|------|-----------------|----------------|-------|\n");

				FStaticParameterSet StaticParams = Instance->GetStaticParameters();
				for (const FMaterialParameterInfo& Info : Infos)
				{
					bool InheritedValue = false;
					FGuid OutGuid;
					Instance->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(Info.Name), InheritedValue, OutGuid, /*bOveriddenOnly=*/false);
					FString OverrideStr;
					for (const FStaticSwitchParameter& SP : StaticParams.StaticSwitchParameters)
					{
						if (SP.ParameterInfo.Name == Info.Name && SP.bOverride)
						{
							OverrideStr = SP.Value ? TEXT("true") : TEXT("false");
							break;
						}
					}
					Out += FormatParamRow(Info.Name,
						InheritedValue ? TEXT("true") : TEXT("false"),
						OverrideStr);
				}
				Out += TEXT("\n");
			}
		}

		// Static component mask parameters.
		{
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Guids;
			Instance->GetAllParameterInfoOfType(EMaterialParameterType::StaticComponentMask, Infos, Guids);
			if (Infos.Num() > 0)
			{
				Out += TEXT("## Static Component Mask Parameters\n\n");
				Out += TEXT("| Name | Inherited Value | Override Value | Group |\n");
				Out += TEXT("|------|-----------------|----------------|-------|\n");

				FStaticParameterSet StaticParams = Instance->GetStaticParameters();
				for (const FMaterialParameterInfo& Info : Infos)
				{
					bool R = false, G = false, B = false, A = false;
					FGuid OutGuid;
					Instance->GetStaticComponentMaskParameterValue(FHashedMaterialParameterInfo(Info.Name), R, G, B, A, OutGuid, /*bOveriddenOnly=*/false);
					const FString InheritedStr = FString::Printf(TEXT("(R=%d, G=%d, B=%d, A=%d)"),
						R ? 1 : 0, G ? 1 : 0, B ? 1 : 0, A ? 1 : 0);
					FString OverrideStr;
#if WITH_EDITORONLY_DATA
					for (const FStaticComponentMaskParameter& MP : StaticParams.EditorOnly.StaticComponentMaskParameters)
					{
						if (MP.ParameterInfo.Name == Info.Name && MP.bOverride)
						{
							OverrideStr = FString::Printf(TEXT("(R=%d, G=%d, B=%d, A=%d)"),
								MP.R ? 1 : 0, MP.G ? 1 : 0, MP.B ? 1 : 0, MP.A ? 1 : 0);
							break;
						}
					}
#endif
					Out += FormatParamRow(Info.Name, InheritedStr, OverrideStr);
				}
				Out += TEXT("\n");
			}
		}

		return Out;
	}

	// ============================================================================
	// Graph mutation
	// ============================================================================

	bool ConnectExpressions(UMaterial* Material, UMaterialExpression* From, const FString& FromOutput, UMaterialExpression* To, const FString& ToInput, FString& OutError)
	{
		if (!Material || !From || !To)
		{
			OutError = TEXT("ConnectExpressions: null material/from/to");
			return false;
		}

		const bool bConnected = UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput);
		if (!bConnected)
		{
			TArray<FString> ValidOutputs;
			for (const FExpressionOutput& Output : From->GetOutputs())
			{
				ValidOutputs.Add(Output.OutputName != NAME_None ? Output.OutputName.ToString() : TEXT("(unnamed)"));
			}
			TArray<FString> ValidInputs;
			for (int32 i = 0; ; ++i)
			{
				FExpressionInput* Input = To->GetInput(i);
				if (!Input)
				{
					break;
				}
				const FName InName = To->GetInputName(i);
				ValidInputs.Add(InName != NAME_None ? InName.ToString() : FString::Printf(TEXT("Input%d"), i));
			}
			OutError = FString::Printf(TEXT("ConnectMaterialExpressions returned false. From outputs: [%s]. To inputs: [%s]."),
				*FString::Join(ValidOutputs, TEXT(", ")),
				*FString::Join(ValidInputs, TEXT(", ")));
			return false;
		}
		return true;
	}

	bool DisconnectExpressionInput(UMaterial* Material, UMaterialExpression* Expr, const FString& InputName, FString& OutError)
	{
		if (!Material || !Expr)
		{
			OutError = TEXT("DisconnectExpressionInput: null material/expression");
			return false;
		}

		// UMaterialEditingLibrary has no Disconnect helper; clear the FExpressionInput directly.
		bool bFound = false;
		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* Input = Expr->GetInput(i);
			if (!Input)
			{
				break;
			}
			const FName CurrentName = Expr->GetInputName(i);
			const FString CurrentNameStr = CurrentName != NAME_None ? CurrentName.ToString() : FString::Printf(TEXT("Input%d"), i);
			if (CurrentNameStr.Equals(InputName, ESearchCase::IgnoreCase))
			{
				Expr->Modify();
				Input->Expression = nullptr;
				Input->OutputIndex = 0;
				Input->Mask = 0;
				Input->MaskR = Input->MaskG = Input->MaskB = Input->MaskA = 0;
				bFound = true;
				FPropertyChangedEvent ChangeEvent(nullptr);
				Expr->PostEditChangeProperty(ChangeEvent);
				break;
			}
		}

		if (!bFound)
		{
			OutError = FString::Printf(TEXT("Input '%s' not found on expression %s"),
				*InputName, *Expr->GetClass()->GetName());
			return false;
		}
		return true;
	}

	bool ConnectToMaterialAttribute(UMaterial* Material, UMaterialExpression* From, const FString& AttributeName, const FString& OutputName, FString& OutError)
	{
		if (!Material || !From)
		{
			OutError = TEXT("ConnectToMaterialAttribute: null material/from");
			return false;
		}

		const TMap<FString, EMaterialProperty>& AttrMap = GetAttributeNameMap();
		const EMaterialProperty* PropertyPtr = AttrMap.Find(AttributeName);
		if (!PropertyPtr)
		{
			OutError = FString::Printf(TEXT("Unknown material attribute: '%s'"), *AttributeName);
			return false;
		}

		if (*PropertyPtr == MP_FrontMaterial && !Substrate::IsSubstrateEnabled())
		{
			OutError = TEXT("FrontMaterial pin is only available when Substrate is enabled");
			return false;
		}

		const bool bOk = UMaterialEditingLibrary::ConnectMaterialProperty(From, OutputName, *PropertyPtr);
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("ConnectMaterialProperty failed for attribute '%s', output '%s'"),
				*AttributeName, *OutputName);
			return false;
		}
		return true;
	}

	bool SetExpressionProperty(UMaterial* Material, UMaterialExpression* Expr, const FString& PropertyName, const FString& TextValue, FString& OutError)
	{
		if (!Expr)
		{
			OutError = TEXT("SetExpressionProperty: null expression");
			return false;
		}

		FProperty* Prop = Expr->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Prop)
		{
			// Case-insensitive fallback.
			for (TFieldIterator<FProperty> It(Expr->GetClass()); It; ++It)
			{
				if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
			}
		}
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on %s"),
				*PropertyName, *Expr->GetClass()->GetName());
			return false;
		}

		Expr->PreEditChange(Prop);
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Expr);
		const TCHAR* Result = Prop->ImportText_Direct(*TextValue, ValuePtr, Expr, PPF_None);
		if (!Result)
		{
			FPropertyChangedEvent Event(Prop);
			Expr->PostEditChangeProperty(Event);
			OutError = FString::Printf(TEXT("Failed to parse '%s' for property '%s'"), *TextValue, *PropertyName);
			return false;
		}
		FPropertyChangedEvent Event(Prop);
		Expr->PostEditChangeProperty(Event);
		return true;
	}

	// ============================================================================
	// Parameter defaults
	// ============================================================================

	namespace
	{
		template<typename TParam>
		TParam* FindParamExpression(UMaterial* Material, const FName& ParamName)
		{
			if (!Material) return nullptr;
			for (UMaterialExpression* Expr : Material->GetExpressions())
			{
				if (TParam* Param = Cast<TParam>(Expr))
				{
					if (Param->ParameterName == ParamName)
					{
						return Param;
					}
				}
			}
			return nullptr;
		}
	}

	bool SetScalarParameterDefault(UMaterial* Material, const FName& ParamName, float Value, FString& OutError)
	{
		UMaterialExpressionScalarParameter* Param = FindParamExpression<UMaterialExpressionScalarParameter>(Material, ParamName);
		if (!Param)
		{
			OutError = FString::Printf(TEXT("ScalarParameter '%s' not found"), *ParamName.ToString());
			return false;
		}
		Param->Modify();
		Param->DefaultValue = Value;
		FPropertyChangedEvent Event(nullptr);
		Param->PostEditChangeProperty(Event);
		Material->PostEditChange();
		return true;
	}

	bool SetVectorParameterDefault(UMaterial* Material, const FName& ParamName, const FLinearColor& Value, FString& OutError)
	{
		UMaterialExpressionVectorParameter* Param = FindParamExpression<UMaterialExpressionVectorParameter>(Material, ParamName);
		if (!Param)
		{
			OutError = FString::Printf(TEXT("VectorParameter '%s' not found"), *ParamName.ToString());
			return false;
		}
		Param->Modify();
		Param->DefaultValue = Value;
		FPropertyChangedEvent Event(nullptr);
		Param->PostEditChangeProperty(Event);
		Material->PostEditChange();
		return true;
	}

	bool SetTextureParameterDefault(UMaterial* Material, const FName& ParamName, UTexture* Value, FString& OutError)
	{
		UMaterialExpressionTextureSampleParameter* Param = FindParamExpression<UMaterialExpressionTextureSampleParameter>(Material, ParamName);
		if (!Param)
		{
			OutError = FString::Printf(TEXT("TextureParameter '%s' not found"), *ParamName.ToString());
			return false;
		}
		Param->Modify();
		Param->Texture = Value;
		FPropertyChangedEvent Event(nullptr);
		Param->PostEditChangeProperty(Event);
		Material->PostEditChange();
		return true;
	}

	bool SetStaticSwitchParameterDefault(UMaterial* Material, const FName& ParamName, bool Value, FString& OutError)
	{
		UMaterialExpressionStaticSwitchParameter* Param = FindParamExpression<UMaterialExpressionStaticSwitchParameter>(Material, ParamName);
		if (!Param)
		{
			OutError = FString::Printf(TEXT("StaticSwitchParameter '%s' not found"), *ParamName.ToString());
			return false;
		}
		Param->Modify();
		Param->DefaultValue = Value;
		FPropertyChangedEvent Event(nullptr);
		Param->PostEditChangeProperty(Event);
		Material->PostEditChange();
		return true;
	}

	bool SetStaticComponentMaskParameterDefault(UMaterial* Material, const FName& ParamName, bool R, bool G, bool B, bool A, FString& OutError)
	{
		UMaterialExpressionStaticComponentMaskParameter* Param = FindParamExpression<UMaterialExpressionStaticComponentMaskParameter>(Material, ParamName);
		if (!Param)
		{
			OutError = FString::Printf(TEXT("StaticComponentMaskParameter '%s' not found"), *ParamName.ToString());
			return false;
		}
		Param->Modify();
		Param->DefaultR = R ? 1 : 0;
		Param->DefaultG = G ? 1 : 0;
		Param->DefaultB = B ? 1 : 0;
		Param->DefaultA = A ? 1 : 0;
		FPropertyChangedEvent Event(nullptr);
		Param->PostEditChangeProperty(Event);
		Material->PostEditChange();
		return true;
	}

	// ============================================================================
	// MIC parameter mutation
	// ============================================================================

	bool SetMICScalar(UMaterialInstanceConstant* Instance, const FName& ParamName, float Value, FString& OutError)
	{
		if (!Instance)
		{
			OutError = TEXT("SetMICScalar: null instance");
			return false;
		}
		const bool bOk = UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, ParamName, Value);
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("Failed to set scalar parameter '%s'"), *ParamName.ToString());
			return false;
		}
		return true;
	}

	bool SetMICVector(UMaterialInstanceConstant* Instance, const FName& ParamName, const FLinearColor& Value, FString& OutError)
	{
		if (!Instance)
		{
			OutError = TEXT("SetMICVector: null instance");
			return false;
		}
		const bool bOk = UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, ParamName, Value);
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("Failed to set vector parameter '%s'"), *ParamName.ToString());
			return false;
		}
		return true;
	}

	bool SetMICTexture(UMaterialInstanceConstant* Instance, const FName& ParamName, UTexture* Value, FString& OutError)
	{
		if (!Instance)
		{
			OutError = TEXT("SetMICTexture: null instance");
			return false;
		}
		const bool bOk = UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, ParamName, Value);
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("Failed to set texture parameter '%s'"), *ParamName.ToString());
			return false;
		}
		return true;
	}

	bool SetMICStaticSwitch(UMaterialInstanceConstant* Instance, const FName& ParamName, bool Value, FString& OutError)
	{
		if (!Instance)
		{
			OutError = TEXT("SetMICStaticSwitch: null instance");
			return false;
		}
		FStaticParameterSet StaticParams = Instance->GetStaticParameters();
		bool bUpdated = false;
		for (FStaticSwitchParameter& SP : StaticParams.StaticSwitchParameters)
		{
			if (SP.ParameterInfo.Name == ParamName)
			{
				SP.Value = Value;
				SP.bOverride = true;
				bUpdated = true;
				break;
			}
		}
		if (!bUpdated)
		{
			FStaticSwitchParameter Entry;
			Entry.ParameterInfo = FMaterialParameterInfo(ParamName);
			Entry.Value = Value;
			Entry.bOverride = true;
			StaticParams.StaticSwitchParameters.Add(Entry);
		}
		Instance->UpdateStaticPermutation(StaticParams);
		return true;
	}

	bool SetMICStaticComponentMask(UMaterialInstanceConstant* Instance, const FName& ParamName, bool R, bool G, bool B, bool A, FString& OutError)
	{
		if (!Instance)
		{
			OutError = TEXT("SetMICStaticComponentMask: null instance");
			return false;
		}
		FStaticParameterSet StaticParams = Instance->GetStaticParameters();
#if WITH_EDITORONLY_DATA
		bool bUpdated = false;
		for (FStaticComponentMaskParameter& MP : StaticParams.EditorOnly.StaticComponentMaskParameters)
		{
			if (MP.ParameterInfo.Name == ParamName)
			{
				MP.R = R; MP.G = G; MP.B = B; MP.A = A;
				MP.bOverride = true;
				bUpdated = true;
				break;
			}
		}
		if (!bUpdated)
		{
			FStaticComponentMaskParameter Entry;
			Entry.ParameterInfo = FMaterialParameterInfo(ParamName);
			Entry.R = R; Entry.G = G; Entry.B = B; Entry.A = A;
			Entry.bOverride = true;
			StaticParams.EditorOnly.StaticComponentMaskParameters.Add(Entry);
		}
#endif
		Instance->UpdateStaticPermutation(StaticParams);
		return true;
	}

	bool ClearMICOverride(UMaterialInstanceConstant* Instance, const FName& ParamName, EMaterialParameterType Type, FString& OutError)
	{
		if (!Instance)
		{
			OutError = TEXT("ClearMICOverride: null instance");
			return false;
		}

		bool bChanged = false;
		switch (Type)
		{
		case EMaterialParameterType::Scalar:
			for (int32 i = Instance->ScalarParameterValues.Num() - 1; i >= 0; --i)
			{
				if (Instance->ScalarParameterValues[i].ParameterInfo.Name == ParamName)
				{
					Instance->ScalarParameterValues.RemoveAt(i);
					bChanged = true;
				}
			}
			break;
		case EMaterialParameterType::Vector:
			for (int32 i = Instance->VectorParameterValues.Num() - 1; i >= 0; --i)
			{
				if (Instance->VectorParameterValues[i].ParameterInfo.Name == ParamName)
				{
					Instance->VectorParameterValues.RemoveAt(i);
					bChanged = true;
				}
			}
			break;
		case EMaterialParameterType::Texture:
			for (int32 i = Instance->TextureParameterValues.Num() - 1; i >= 0; --i)
			{
				if (Instance->TextureParameterValues[i].ParameterInfo.Name == ParamName)
				{
					Instance->TextureParameterValues.RemoveAt(i);
					bChanged = true;
				}
			}
			break;
		case EMaterialParameterType::StaticSwitch:
		{
			FStaticParameterSet StaticParams = Instance->GetStaticParameters();
			for (FStaticSwitchParameter& SP : StaticParams.StaticSwitchParameters)
			{
				if (SP.ParameterInfo.Name == ParamName && SP.bOverride)
				{
					SP.bOverride = false;
					bChanged = true;
				}
			}
			if (bChanged)
			{
				Instance->UpdateStaticPermutation(StaticParams);
			}
			break;
		}
		case EMaterialParameterType::StaticComponentMask:
		{
#if WITH_EDITORONLY_DATA
			FStaticParameterSet StaticParams = Instance->GetStaticParameters();
			for (FStaticComponentMaskParameter& MP : StaticParams.EditorOnly.StaticComponentMaskParameters)
			{
				if (MP.ParameterInfo.Name == ParamName && MP.bOverride)
				{
					MP.bOverride = false;
					bChanged = true;
				}
			}
			if (bChanged)
			{
				Instance->UpdateStaticPermutation(StaticParams);
			}
#endif
			break;
		}
		default:
			OutError = FString::Printf(TEXT("ClearMICOverride: unsupported parameter type %d"), (int32)Type);
			return false;
		}

		if (!bChanged)
		{
			OutError = FString::Printf(TEXT("No override found for parameter '%s'"), *ParamName.ToString());
			return false;
		}

		Instance->PostEditChange();
		return true;
	}

	// ============================================================================
	// Root properties
	// ============================================================================

	bool SetShadingModel(UMaterial* Material, EMaterialShadingModel NewModel, FString& OutError)
	{
		if (!Material)
		{
			OutError = TEXT("SetShadingModel: null material");
			return false;
		}

		FProperty* Prop = UMaterial::StaticClass()->FindPropertyByName(TEXT("ShadingModel"));
		if (!Prop)
		{
			OutError = TEXT("SetShadingModel: failed to find ShadingModel property");
			return false;
		}
		Material->PreEditChange(Prop);
		Material->SetShadingModel(NewModel);
		FPropertyChangedEvent Event(Prop);
		Material->PostEditChangeProperty(Event);
		return true;
	}

	bool SetBlendMode(UMaterial* Material, EBlendMode NewMode, FString& OutError)
	{
		if (!Material)
		{
			OutError = TEXT("SetBlendMode: null material");
			return false;
		}

		FProperty* Prop = UMaterial::StaticClass()->FindPropertyByName(TEXT("BlendMode"));
		if (!Prop)
		{
			OutError = TEXT("SetBlendMode: failed to find BlendMode property");
			return false;
		}
		Material->PreEditChange(Prop);
		Material->BlendMode = NewMode;
		FPropertyChangedEvent Event(Prop);
		Material->PostEditChangeProperty(Event);
		return true;
	}

	// ============================================================================
	// Compile + save
	// ============================================================================

	bool CompileMaterial(UMaterial* Material, bool bWaitForCompile, FString& OutError)
	{
		if (!Material)
		{
			OutError = TEXT("CompileMaterial: null material");
			return false;
		}

		UMaterialEditingLibrary::RecompileMaterial(Material);

		if (bWaitForCompile && GShaderCompilingManager)
		{
			const double StartTime = FPlatformTime::Seconds();
			const double TimeoutSeconds = 30.0;
			while (GShaderCompilingManager->IsCompiling())
			{
				if ((FPlatformTime::Seconds() - StartTime) > TimeoutSeconds)
				{
					OutError = TEXT("Timed out (>30s) waiting for shader compilation");
					return false;
				}
				GShaderCompilingManager->ProcessAsyncResults(false, false);
				FPlatformProcess::Sleep(0.05f);
			}
			GShaderCompilingManager->FinishAllCompilation();
		}

#if WITH_EDITOR
		// Drain compile errors from the material resource for the current feature level.
#if UE_VERSION_OLDER_THAN(5, 7, 0)
		if (const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel))
#else
		if (const FMaterialResource* Resource = Material->GetMaterialResource(GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel)))
#endif
		{
			const TArray<FString>& Errors = Resource->GetCompileErrors();
			if (Errors.Num() > 0)
			{
				OutError = FString::Join(Errors, TEXT("; "));
				return false;
			}
		}
#endif

		return true;
	}

	bool SaveMaterialAsset(UMaterial* Material, FString& OutError)
	{
		if (!Material)
		{
			OutError = TEXT("SaveMaterialAsset: null material");
			return false;
		}
		UPackage* Package = Material->GetPackage();
		if (!Package)
		{
			OutError = TEXT("SaveMaterialAsset: material has no package");
			return false;
		}
		Package->SetDirtyFlag(true);

		TArray<UPackage*> ToSave;
		ToSave.Add(Package);
		const bool bOk = UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty=*/true);
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("Failed to save material package: %s"), *Package->GetName());
			return false;
		}
		return true;
	}

	bool SaveMaterialInstanceAsset(UMaterialInstanceConstant* Instance, FString& OutError)
	{
		if (!Instance)
		{
			OutError = TEXT("SaveMaterialInstanceAsset: null instance");
			return false;
		}
		UPackage* Package = Instance->GetPackage();
		if (!Package)
		{
			OutError = TEXT("SaveMaterialInstanceAsset: instance has no package");
			return false;
		}
		Package->SetDirtyFlag(true);

		TArray<UPackage*> ToSave;
		ToSave.Add(Package);
		const bool bOk = UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty=*/true);
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("Failed to save MIC package: %s"), *Package->GetName());
			return false;
		}
		return true;
	}

	// ============================================================================
	// Edit scoping
	// ============================================================================

	void BeginEdit(UMaterial* Material)
	{
		if (!Material)
		{
			return;
		}
		TWeakObjectPtr<UMaterial> Key(Material);
		auto& Map = GetEditContexts();
		if (Map.Contains(Key))
		{
			// Already in an edit scope; nest is a no-op.
			return;
		}
		Map.Add(Key, MakeUnique<FMaterialUpdateContext>());
		Material->PreEditChange(nullptr);
	}

	void EndEdit(UMaterial* Material)
	{
		if (!Material)
		{
			return;
		}
		TWeakObjectPtr<UMaterial> Key(Material);
		auto& Map = GetEditContexts();
		if (!Map.Contains(Key))
		{
			return;
		}
		Material->PostEditChange();
		Map.Remove(Key); // FMaterialUpdateContext destructor refreshes dependents
	}
} // namespace ClaireonMaterialHelpers
