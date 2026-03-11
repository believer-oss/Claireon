// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UObject;
class UPackage;

/**
 * Shared utility functions for diff MCP tools.
 * Provides resolution parsing, git-based asset extraction, package loading,
 * property value export, and temp file management.
 */
namespace ClaireonDiffHelpers
{
	/** Resolution levels for diff output. */
	enum class EDiffResolution : uint8
	{
		Exists,   // Boolean: any differences? Stops at first diff.
		Summary,  // Lists each difference with type. No values.
		Detailed  // Full diff with old/new values.
	};

	/** Parse a resolution string into the enum. Returns false on invalid input. */
	bool ParseResolution(const FString& InString, EDiffResolution& OutResolution, FString& OutError);

	// ── Asset Validation & Loading ──────────────────────────────────────

	/** Validate that an asset path starts with /Game/. */
	bool ValidateAssetPath(const FString& AssetPath, FString& OutError);

	/** Load an asset from an Unreal content path. Returns null + error on failure. */
	UObject* LoadAssetFromPath(const FString& AssetPath, FString& OutError);

	// ── Git Revision Support ────────────────────────────────────────────

	/** Convert a /Game/ asset path to a git-relative path (e.g. Content/...). */
	FString ConvertAssetPathToGitRelativePath(const FString& AssetPath);

	/**
	 * Extract a .uasset from a git revision to a temp file.
	 * Uses cmd.exe /C with shell redirection for binary-safe extraction.
	 * Returns the temp file path, or empty string + error on failure.
	 */
	FString ExtractAssetFromGitRevision(const FString& GitRelativePath, const FString& Revision, FString& OutError);

	/**
	 * Load a package from a temp file using DiffUtils::LoadPackageForDiff.
	 * @param TempFilePath   Filesystem path to the extracted .uasset
	 * @param OrigAssetPath  Original /Game/ asset path for proper outer resolution
	 * @return Loaded package, or null + error on failure
	 */
	UPackage* LoadPackageForDiff(const FString& TempFilePath, const FString& OrigAssetPath, FString& OutError);

	/** Find the primary asset object within a loaded diff package. */
	UObject* FindAssetInPackage(UPackage* Package, FString& OutError);

	/** Clean up a temp file created during git extraction. */
	void CleanupTempFile(const FString& TempFilePath);

	/** Get the base temp directory for diff operations. */
	FString GetDiffTempDir();

	// ── Side Resolution (A/B Model) ─────────────────────────────────────

	/** Result of resolving one side of a diff comparison. */
	struct FResolvedDiffSide
	{
		/** The loaded object for this side. */
		UObject* Object = nullptr;

		/** Optional temp file path that needs cleanup (empty if loaded from editor). */
		FString TempFilePath;

		/** The package loaded for diff (if from git revision). Kept alive to prevent GC. */
		UPackage* DiffPackage = nullptr;

		bool IsValid() const { return Object != nullptr; }
	};

	/**
	 * Resolve one side of a diff comparison.
	 * If Revision is empty, loads from editor state. Otherwise, extracts from git and loads via DiffUtils.
	 */
	FResolvedDiffSide ResolveDiffSide(const FString& AssetPath, const FString& Revision, FString& OutError);

	// ── Property Value Export ───────────────────────────────────────────

	/** Export a property value as a display string using FProperty::ExportTextItem_Direct. */
	FString ExportPropertyValue(const FProperty* Property, const void* ContainerPtr);

	// ── Scoped Cleanup ──────────────────────────────────────────────────

	/** RAII guard that deletes a temp file on destruction unless released. */
	struct FScopedTempFile
	{
		FString Path;

		FScopedTempFile() = default;
		explicit FScopedTempFile(const FString& InPath) : Path(InPath) {}

		~FScopedTempFile()
		{
			if (!Path.IsEmpty())
			{
				CleanupTempFile(Path);
			}
		}

		/** Disarm the destructor -- caller takes ownership of cleanup. */
		void Release() { Path.Empty(); }

		// Non-copyable
		FScopedTempFile(const FScopedTempFile&) = delete;
		FScopedTempFile& operator=(const FScopedTempFile&) = delete;

		// Movable
		FScopedTempFile(FScopedTempFile&& Other) noexcept : Path(MoveTemp(Other.Path)) {}
		FScopedTempFile& operator=(FScopedTempFile&& Other) noexcept
		{
			if (this != &Other)
			{
				if (!Path.IsEmpty())
				{
					CleanupTempFile(Path);
				}
				Path = MoveTemp(Other.Path);
			}
			return *this;
		}
	};

	// ── Parameter Validation ────────────────────────────────────────────

	/**
	 * Validate the A/B diff parameters: at least one revision must be specified,
	 * or asset_path_b must differ from asset_path_a.
	 */
	bool ValidateDiffParameters(
		const FString& AssetPathA, const FString& RevisionA,
		const FString& AssetPathB, const FString& RevisionB,
		FString& OutError);

	/** Sanitize a git revision string. Returns false if it contains invalid characters. */
	bool SanitizeRevision(const FString& Revision, FString& OutError);

	/** Format a side label for output (e.g. "current" or revision string). */
	FString FormatSideLabel(const FString& Revision);

} // namespace ClaireonDiffHelpers
