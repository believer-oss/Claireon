// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonPortDerivation.h"

#include "Misc/Paths.h"

namespace
{
	// File-local discriminator on the anonymous namespace: anon-NS helper
	// names can collide under non-unity Linux clang strict if another TU
	// defines the same name. The "PortDerivation_" prefix makes these
	// names unique across the module.

	// Self-contained SHA-256 implementation. UE on Windows lacks a
	// stock SHA-256 helper (FPlatformMisc::GetSHA256Signature asserts as
	// unimplemented), and pulling OpenSSL into the public Claireon
	// surface is out of scope here. The hash is a pure content
	// fingerprint -- not a security boundary -- so a clean-room reference
	// implementation is appropriate.

	constexpr uint32 PortDerivation_SHA256_K[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
		0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
		0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
		0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
		0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
		0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
		0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
		0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
		0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
	};

	FORCEINLINE uint32 PortDerivation_RotR(uint32 X, uint32 N)
	{
		return (X >> N) | (X << (32 - N));
	}

	void PortDerivation_Sha256(const uint8* Input, int32 Length, uint8 OutDigest[32])
	{
		uint32 H[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
		};

		// Pad: append 0x80, then zero-pad until length % 64 == 56, then
		// append the bit-length as 8 big-endian bytes.
		const int32 OriginalLen = Length;
		const uint64 BitLen = static_cast<uint64>(OriginalLen) * 8ull;
		// Build padded buffer in-place into a scratch array.
		const int32 PaddedLen = ((OriginalLen + 9 + 63) / 64) * 64;
		TArray<uint8> Buf;
		Buf.SetNumZeroed(PaddedLen);
		FMemory::Memcpy(Buf.GetData(), Input, OriginalLen);
		Buf[OriginalLen] = 0x80;
		// Big-endian length in last 8 bytes.
		for (int32 I = 0; I < 8; ++I)
		{
			Buf[PaddedLen - 1 - I] = static_cast<uint8>((BitLen >> (I * 8)) & 0xff);
		}

		// Process each 512-bit (64-byte) block.
		for (int32 Off = 0; Off < PaddedLen; Off += 64)
		{
			uint32 W[64];
			for (int32 I = 0; I < 16; ++I)
			{
				const int32 J = Off + I * 4;
				W[I] = (static_cast<uint32>(Buf[J + 0]) << 24)
				     | (static_cast<uint32>(Buf[J + 1]) << 16)
				     | (static_cast<uint32>(Buf[J + 2]) << 8)
				     |  static_cast<uint32>(Buf[J + 3]);
			}
			for (int32 I = 16; I < 64; ++I)
			{
				const uint32 S0 = PortDerivation_RotR(W[I - 15], 7)
				                ^ PortDerivation_RotR(W[I - 15], 18)
				                ^ (W[I - 15] >> 3);
				const uint32 S1 = PortDerivation_RotR(W[I - 2], 17)
				                ^ PortDerivation_RotR(W[I - 2], 19)
				                ^ (W[I - 2] >> 10);
				W[I] = W[I - 16] + S0 + W[I - 7] + S1;
			}

			uint32 A = H[0]; uint32 B = H[1]; uint32 C = H[2]; uint32 D = H[3];
			uint32 E = H[4]; uint32 F = H[5]; uint32 G = H[6]; uint32 HH = H[7];

			for (int32 I = 0; I < 64; ++I)
			{
				const uint32 S1 = PortDerivation_RotR(E, 6)
				                ^ PortDerivation_RotR(E, 11)
				                ^ PortDerivation_RotR(E, 25);
				const uint32 Ch = (E & F) ^ ((~E) & G);
				const uint32 Temp1 = HH + S1 + Ch + PortDerivation_SHA256_K[I] + W[I];
				const uint32 S0 = PortDerivation_RotR(A, 2)
				                ^ PortDerivation_RotR(A, 13)
				                ^ PortDerivation_RotR(A, 22);
				const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
				const uint32 Temp2 = S0 + Maj;

				HH = G;
				G = F;
				F = E;
				E = D + Temp1;
				D = C;
				C = B;
				B = A;
				A = Temp1 + Temp2;
			}

			H[0] += A; H[1] += B; H[2] += C; H[3] += D;
			H[4] += E; H[5] += F; H[6] += G; H[7] += HH;
		}

		for (int32 I = 0; I < 8; ++I)
		{
			OutDigest[I * 4 + 0] = static_cast<uint8>((H[I] >> 24) & 0xff);
			OutDigest[I * 4 + 1] = static_cast<uint8>((H[I] >> 16) & 0xff);
			OutDigest[I * 4 + 2] = static_cast<uint8>((H[I] >> 8) & 0xff);
			OutDigest[I * 4 + 3] = static_cast<uint8>((H[I] >> 0) & 0xff);
		}
	}

	// Mirror of claireon_proxy.py::canonicalize_worktree -- realpath +
	// lowercase. The Python helper uses os.path.realpath; UE's
	// FPaths::ConvertRelativePathToFull is the closest equivalent for
	// editor-side input that already begins as an absolute path.
	//
	// Path-separator note: ConvertRelativePathToFull rewrites backslashes
	// to forward slashes, but Python's os.path.realpath on Windows and
	// PowerShell's [System.IO.Path]::GetFullPath both preserve the OS-native
	// separator (backslash on Windows). The three implementations must hash
	// the same byte string or they derive different ports for the same
	// worktree -- which would split editor (direct-connect) and
	// proxy/.mcp.json onto different ports. We normalise to backslashes on
	// Windows after canonicalisation to match the other two sides.
	FString PortDerivation_Canonicalize(const FString& WorktreeRoot)
	{
		FString Full = FPaths::ConvertRelativePathToFull(WorktreeRoot);
		// Strip trailing path separator if present (FPaths can leave one).
		while (Full.EndsWith(TEXT("/")) || Full.EndsWith(TEXT("\\")))
		{
			Full.LeftChopInline(1, EAllowShrinking::No);
		}
#if PLATFORM_WINDOWS
		// Match os.path.realpath / [System.IO.Path]::GetFullPath only when
		// the path is Windows-style (begins with a drive letter). POSIX-style
		// inputs starting with `/` are left alone so cross-platform unit
		// fixtures hash the same on both sides.
		if (Full.Len() >= 2 && Full[1] == TEXT(':'))
		{
			Full.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
		}
#endif
		return Full.ToLower();
	}
}

namespace Claireon
{
	uint16 DeriveDefaultMcpPort(const FString& WorktreeRoot)
	{
		const FString Canonical = PortDerivation_Canonicalize(WorktreeRoot);
		// UTF-8 encode the canonical path (matches Python's encode("utf-8")).
		const FTCHARToUTF8 Utf8(*Canonical);
		uint8 Digest[32] = {0};
		PortDerivation_Sha256(reinterpret_cast<const uint8*>(Utf8.Get()),
		                      Utf8.Length(),
		                      Digest);
		// First two big-endian bytes folded into [49152, 65535].
		const uint32 Offset = (static_cast<uint32>(Digest[0]) << 8)
		                    |  static_cast<uint32>(Digest[1]);
		const uint32 Port = 49152u + (Offset % 16384u);
		return static_cast<uint16>(Port);
	}
}
